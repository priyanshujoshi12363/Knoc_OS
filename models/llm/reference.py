import argparse
import os
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = "<IIIIIIIIIffIIIIIIQQQQQQ"


class Model:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:8] != b"KNOCLLM1":
            raise ValueError("not a KnocOS LLM file")
        fields = struct.unpack_from(HEADER, data, 8)
        (_, self.dim, self.hidden, self.layers, self.heads, self.kv_heads, self.vocab, self.max_seq,
         self.group, self.theta, self.eps, self.endoftext, self.im_start, self.im_end, merge_count,
         _, _, tok_off, _, emb_off, _, body_off, _) = fields
        self.head_dim = self.dim // self.heads
        self.kv_dim = self.kv_heads * self.head_dim

        position = tok_off
        self.tokens = []
        for _ in range(self.vocab):
            length = struct.unpack_from("<H", data, position)[0]
            self.tokens.append(data[position + 2:position + 2 + length])
            position += 2 + length
        position += (-(position - tok_off)) % 64
        count = struct.unpack_from("<I", data, position)[0]
        triples = np.frombuffer(data, np.uint32, count * 3, position + 4).reshape(count, 3)
        self.merges = {(int(a), int(b)): (rank, int(c)) for rank, (a, b, c) in enumerate(triples)}
        self.byte_ids = {}
        for index, token in enumerate(self.tokens[:self.endoftext]):
            if len(token) == 1:
                self.byte_ids[token[0]] = index

        self.data = data
        self.position = emb_off
        self.embedding = self.matrix(self.vocab, self.dim)
        self.position = body_off
        self.blocks = []
        for _ in range(self.layers):
            block = {"attn_norm": self.floats(self.dim)}
            block["wq"], block["bq"] = self.matrix(self.dim, self.dim), self.floats(self.dim)
            block["wk"], block["bk"] = self.matrix(self.kv_dim, self.dim), self.floats(self.kv_dim)
            block["wv"], block["bv"] = self.matrix(self.kv_dim, self.dim), self.floats(self.kv_dim)
            block["wo"] = self.matrix(self.dim, self.dim)
            block["ffn_norm"] = self.floats(self.dim)
            block["w_gate"] = self.matrix(self.hidden, self.dim)
            block["w_up"] = self.matrix(self.hidden, self.dim)
            block["w_down"] = self.matrix(self.dim, self.hidden)
            self.blocks.append(block)
        self.final_norm = self.floats(self.dim)

    def floats(self, count):
        values = np.frombuffer(self.data, np.float32, count, self.position)
        self.position += 4 * count
        return values

    def matrix(self, rows, cols):
        q = np.frombuffer(self.data, np.int8, rows * cols, self.position).reshape(rows, cols)
        self.position += rows * cols
        scales = np.frombuffer(self.data, np.float32, rows * cols // self.group, self.position)
        self.position += 4 * rows * cols // self.group
        return (q.reshape(rows, cols // self.group, self.group).astype(np.float32)
                * scales.reshape(rows, cols // self.group, 1)).reshape(rows, cols)


def pieces(text):
    out = []
    data = text.encode()
    i = 0
    n = len(data)

    def is_letter(b):
        return (65 <= b <= 90) or (97 <= b <= 122) or b >= 0x80

    def is_digit(b):
        return 48 <= b <= 57

    def is_space(b):
        return b in (32, 9, 10, 13)

    while i < n:
        b = data[i]
        if b == 39 and i + 1 < n:
            for suffix in (b"'s", b"'t", b"'re", b"'ve", b"'m", b"'ll", b"'d"):
                if data[i:i + len(suffix)].lower() == suffix:
                    out.append(data[i:i + len(suffix)])
                    i += len(suffix)
                    break
            else:
                suffix = None
            if suffix is not None:
                continue
        if is_letter(b) or (not is_space(b) and not is_digit(b) and b not in (10, 13)
                            and i + 1 < n and is_letter(data[i + 1]) and not is_letter(b)):
            j = i + 1 if not is_letter(b) else i
            while j < n and is_letter(data[j]):
                j += 1
            out.append(data[i:j])
            i = j
            continue
        if is_digit(b):
            out.append(data[i:i + 1])
            i += 1
            continue
        if b in (10, 13) or (is_space(b) and any(c in (10, 13) for c in data[i:i + 1])):
            j = i
            while j < n and data[j] in (10, 13):
                j += 1
            out.append(data[i:j])
            i = j
            continue
        if is_space(b):
            j = i
            while j < n and data[j] in (32, 9):
                j += 1
            if j < n and not is_space(data[j]) and j - i > 1:
                out.append(data[i:j - 1])
                i = j - 1
                continue
            if j < n and not is_space(data[j]) and not is_letter(data[j]) and not is_digit(data[j]):
                k = j
                while k < n and not is_space(data[k]) and not is_letter(data[k]) and not is_digit(data[k]):
                    k += 1
                while k < n and data[k] in (10, 13):
                    k += 1
                out.append(data[i:k])
                i = k
                continue
            if j == n or j - i > 1 or data[j] in (10, 13):
                out.append(data[i:j])
                i = j
                continue
            j = i + 1
            while j < n and is_letter(data[j]):
                j += 1
            out.append(data[i:j])
            i = j
            continue
        j = i
        while j < n and not is_space(data[j]) and not is_letter(data[j]) and not is_digit(data[j]):
            j += 1
        while j < n and data[j] in (10, 13):
            j += 1
        out.append(data[i:j])
        i = j
    return out


def encode(model, text):
    ids = []
    for piece in pieces(text):
        tokens = [model.byte_ids[b] for b in piece]
        while len(tokens) > 1:
            best = None
            for k in range(len(tokens) - 1):
                found = model.merges.get((tokens[k], tokens[k + 1]))
                if found and (best is None or found[0] < best[0]):
                    best = (found[0], k, found[1])
            if best is None:
                break
            _, k, merged = best
            tokens = tokens[:k] + [merged] + tokens[k + 2:]
        ids.extend(tokens)
    return ids


def chat_prompt(model, system, question):
    ids = [model.im_start] + encode(model, "system\n" + system) + [model.im_end] + encode(model, "\n")
    ids += [model.im_start] + encode(model, "user\n" + question) + [model.im_end] + encode(model, "\n")
    ids += [model.im_start] + encode(model, "assistant\n")
    return ids


def rms_norm(x, weight, eps):
    return x / np.sqrt(np.mean(x * x) + eps) * weight


def rope(vector, position, head_dim, theta):
    half = head_dim // 2
    freqs = theta ** (-np.arange(half, dtype=np.float64) * 2 / head_dim)
    angle = position * freqs
    cos, sin = np.cos(angle), np.sin(angle)
    v = vector.reshape(-1, head_dim).astype(np.float64)
    first, second = v[:, :half].copy(), v[:, half:].copy()
    v[:, :half] = first * cos - second * sin
    v[:, half:] = second * cos + first * sin
    return v.reshape(-1).astype(np.float32)


def forward(model, token, position, cache):
    x = model.embedding[token].copy()
    group = model.heads // model.kv_heads
    for layer, block in enumerate(model.blocks):
        h = rms_norm(x, block["attn_norm"], model.eps)
        q = rope(block["wq"] @ h + block["bq"], position, model.head_dim, model.theta)
        k = rope(block["wk"] @ h + block["bk"], position, model.head_dim, model.theta)
        v = block["wv"] @ h + block["bv"]
        cache[layer][0][position] = k
        cache[layer][1][position] = v
        keys = cache[layer][0][:position + 1].reshape(position + 1, model.kv_heads, model.head_dim)
        values = cache[layer][1][:position + 1].reshape(position + 1, model.kv_heads, model.head_dim)
        out = np.zeros(model.dim, dtype=np.float32)
        for head in range(model.heads):
            kv = head // group
            qh = q[head * model.head_dim:(head + 1) * model.head_dim]
            scores = keys[:, kv, :] @ qh / np.sqrt(model.head_dim)
            scores = np.exp(scores - scores.max())
            scores /= scores.sum()
            out[head * model.head_dim:(head + 1) * model.head_dim] = scores @ values[:, kv, :]
        x = x + block["wo"] @ out
        h = rms_norm(x, block["ffn_norm"], model.eps)
        gate = block["w_gate"] @ h
        x = x + block["w_down"] @ ((gate / (1 + np.exp(-gate))) * (block["w_up"] @ h))
    x = rms_norm(x, model.final_norm, model.eps)
    return model.embedding @ x


def generate(model, ids, count):
    cache = [[np.zeros((model.max_seq, model.kv_dim), np.float32) for _ in range(2)]
             for _ in range(model.layers)]
    output = []
    logits = None
    for position, token in enumerate(ids):
        logits = forward(model, token, position, cache)
    position = len(ids)
    for _ in range(count):
        token = int(np.argmax(logits))
        if token in (model.im_end, model.endoftext):
            break
        output.append(token)
        logits = forward(model, token, position, cache)
        position += 1
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("question", nargs="?", default="What is an operating system? Answer in one sentence.")
    parser.add_argument("--model", default=os.path.join(HERE, "qwen.kllm"))
    parser.add_argument("--tokens", type=int, default=40)
    args = parser.parse_args()

    started = time.time()
    model = Model(args.model)
    ids = chat_prompt(model, "You are the KnocOS assistant. Answer briefly.", args.question)
    print(f"loaded in {time.time() - started:.1f} s, prompt {len(ids)} tokens")
    started = time.time()
    output = generate(model, ids, args.tokens)
    text = b"".join(model.tokens[t] for t in output).decode(errors="replace")
    print(f"answer ({len(output)} tokens, {time.time() - started:.1f} s): {text}")
    print("prompt ids:", ids)
    print("answer ids:", output)


if __name__ == "__main__":
    sys.exit(main())
