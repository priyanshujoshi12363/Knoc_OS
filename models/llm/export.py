import argparse
import json
import os
import struct

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"KNOCLLM1"
VERSION = 1
GROUP = 64
ALIGN = 64
MAX_SEQ = 1024


def bytes_to_unicode():
    printable = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + \
        list(range(ord("®"), ord("ÿ") + 1))
    codes = printable[:]
    extra = 0
    for b in range(256):
        if b not in printable:
            printable.append(b)
            codes.append(256 + extra)
            extra += 1
    return dict(zip(printable, [chr(c) for c in codes]))


UNICODE_TO_BYTE = {v: k for k, v in bytes_to_unicode().items()}


def token_bytes(text):
    return bytes(UNICODE_TO_BYTE[ch] for ch in text)


def load_safetensors(path):
    with open(path, "rb") as file:
        length = struct.unpack("<Q", file.read(8))[0]
        header = json.loads(file.read(length))
        base = 8 + length
        tensors = {}
        for name, info in header.items():
            if name == "__metadata__":
                continue
            start, end = info["data_offsets"]
            file.seek(base + start)
            raw = np.frombuffer(file.read(end - start), dtype=np.uint16)
            if info["dtype"] != "BF16":
                raise ValueError(f"{name}: unsupported dtype {info['dtype']}")
            tensors[name] = (raw.astype(np.uint32) << 16).view(np.float32).reshape(info["shape"])
    return tensors


def quantize(weights):
    rows, cols = weights.shape
    groups = weights.reshape(rows, cols // GROUP, GROUP)
    scales = np.abs(groups).max(axis=2) / 127.0
    scales[scales == 0] = 1.0
    q = np.clip(np.round(groups / scales[..., None]), -127, 127).astype(np.int8)
    return q.reshape(rows, cols), scales.astype(np.float32)


def dequantize(q, scales):
    rows, cols = q.shape
    return (q.reshape(rows, cols // GROUP, GROUP).astype(np.float32) * scales[..., None]).reshape(rows, cols)


def pad(blob):
    return blob + bytes((-len(blob)) % ALIGN)


def tokenizer_section(tokenizer, vocab_size):
    vocab = [b""] * vocab_size
    for text, index in tokenizer["model"]["vocab"].items():
        vocab[index] = token_bytes(text)
    for added in tokenizer["added_tokens"]:
        vocab[added["id"]] = added["content"].encode()
    ids = {bytes_value: index for index, bytes_value in enumerate(vocab) if bytes_value and index < 151643}
    triples = []
    for rank, merge in enumerate(tokenizer["model"]["merges"]):
        left, right = merge.split(" ") if isinstance(merge, str) else merge
        left_bytes, right_bytes = token_bytes(left), token_bytes(right)
        result = ids.get(left_bytes + right_bytes)
        if result is not None and left_bytes in ids and right_bytes in ids:
            triples.append((ids[left_bytes], ids[right_bytes], result))
    blob = bytearray()
    for token in vocab:
        blob += struct.pack("<H", len(token)) + token
    blob = pad(bytes(blob))
    blob += struct.pack("<I", len(triples)) + b"".join(struct.pack("<III", *t) for t in triples)
    return pad(blob), len(triples)


def matrix(q, scales):
    return q.tobytes() + scales.tobytes()


def floats(values):
    return values.astype(np.float32).tobytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=os.path.join(HERE, "qwen"))
    parser.add_argument("--out", default=os.path.join(HERE, "qwen.kllm"))
    args = parser.parse_args()

    config = json.load(open(os.path.join(args.model, "config.json")))
    tokenizer = json.load(open(os.path.join(args.model, "tokenizer.json")))
    tensors = load_safetensors(os.path.join(args.model, "model.safetensors"))

    dim = config["hidden_size"]
    hidden = config["intermediate_size"]
    layers = config["num_hidden_layers"]
    heads = config["num_attention_heads"]
    kv_heads = config["num_key_value_heads"]
    vocab = config["vocab_size"]

    tokens, merges = tokenizer_section(tokenizer, vocab)
    q, s = quantize(tensors["model.embed_tokens.weight"])
    embedding = pad(matrix(q, s))

    body = bytearray()
    for i in range(layers):
        prefix = f"model.layers.{i}."
        body += floats(tensors[prefix + "input_layernorm.weight"])
        for name in ("q", "k", "v"):
            body += matrix(*quantize(tensors[prefix + f"self_attn.{name}_proj.weight"]))
            body += floats(tensors[prefix + f"self_attn.{name}_proj.bias"])
        body += matrix(*quantize(tensors[prefix + "self_attn.o_proj.weight"]))
        body += floats(tensors[prefix + "post_attention_layernorm.weight"])
        for name in ("gate", "up", "down"):
            body += matrix(*quantize(tensors[prefix + f"mlp.{name}_proj.weight"]))
    body += floats(tensors["model.norm.weight"])
    body = pad(bytes(body))

    header_size = 128
    sections = [tokens, embedding, body]
    offsets = []
    position = header_size
    for section in sections:
        offsets.append(position)
        position += len(section)

    header = MAGIC + struct.pack(
        "<IIIIIIIIIffIIIIIIQQQQQQ",
        VERSION, dim, hidden, layers, heads, kv_heads, vocab, MAX_SEQ, GROUP,
        float(config["rope_theta"]), float(config["rms_norm_eps"]),
        151643, 151644, 151645, merges, 0, 0,
        offsets[0], len(tokens), offsets[1], len(embedding), offsets[2], len(body))
    header = header + bytes(header_size - len(header))

    with open(args.out, "wb") as file:
        file.write(header)
        for section in sections:
            file.write(section)

    total = os.path.getsize(args.out)
    print(f"wrote {args.out}: {total / 1e6:.0f} MB (tokenizer {len(tokens) / 1e6:.1f} MB, "
          f"embeddings {len(embedding) / 1e6:.0f} MB, layers {len(body) / 1e6:.0f} MB), "
          f"{merges} merges")


if __name__ == "__main__":
    main()
