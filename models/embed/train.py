import argparse
import os
import random
import re
import struct
import time

import numpy as np
import torch

import tokens as T

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache")
OUT = os.path.join(HERE, "knocembed.knm")
TEACHER = "sentence-transformers/all-MiniLM-L6-v2"
MAGIC = b"KNEMBED1"

SYNONYMS = {
    "invoice": ["billing", "due date", "statement of account", "pay by", "electricity bill", "phone bill", "charges"],
    "receipt": ["purchase", "bought", "order", "payment successful", "transaction id"],
    "resume": ["cv", "biodata", "profile", "career summary", "employment history", "software engineer",
               "years of experience", "references"],
    "recipe": ["food", "dish", "cook", "kitchen", "boil", "fry", "serve hot", "salt and pepper", "pasta", "curry"],
    "meeting": ["minutes", "discussion", "decisions", "follow up", "team sync", "standup"],
    "code": ["printf", "return", "include", "def", "import", "void", "int main", "compile error", "debug"],
    "photo": ["pictures", "beach", "trip", "holiday", "vacation", "jpg", "png", "gallery"],
    "travel": ["flight", "airport", "hotel", "journey", "tour", "booking reference"],
    "todo": ["checklist", "reminder", "errands", "pending"],
    "medical": ["clinic", "medicine", "diagnosis", "patient"],
    "school": ["class", "teacher", "student", "grades", "semester"],
}

KINDS = {
    "invoice": ["invoice", "bill", "payment due", "amount due", "total payable", "tax invoice", "billing statement"],
    "receipt": ["receipt", "payment received", "thank you for your purchase", "order confirmation", "paid"],
    "bank": ["bank statement", "account balance", "transactions", "credit card statement", "deposit", "withdrawal"],
    "salary": ["salary slip", "payslip", "monthly pay", "gross salary", "net pay", "income"],
    "tax": ["income tax return", "tax filing", "tax refund", "deductions", "tax form"],
    "recipe": ["recipe", "ingredients", "cooking instructions", "bake for 20 minutes", "stir the sauce",
               "chop the onions", "preheat the oven"],
    "resume": ["resume", "curriculum vitae", "work experience", "education", "skills", "job application"],
    "cover": ["cover letter", "dear hiring manager", "i am applying for", "job opening"],
    "meeting": ["meeting notes", "agenda", "action items", "attendees", "minutes of the meeting"],
    "todo": ["to do list", "tasks", "buy milk", "remember to", "shopping list", "groceries"],
    "travel": ["flight ticket", "boarding pass", "hotel booking", "itinerary", "trip", "vacation", "passport"],
    "photo": ["photo", "picture", "image", "camera", "selfie", "screenshot", "holiday photos"],
    "music": ["song", "music", "album", "playlist", "audio track", "mp3"],
    "video": ["video", "movie", "film", "recording", "clip"],
    "code": ["source code", "function", "compile", "program", "int main", "return 0", "include stdio",
             "python script", "class", "bug fix"],
    "report": ["project report", "quarterly report", "analysis", "summary", "results", "conclusion"],
    "contract": ["contract", "agreement", "terms and conditions", "signed by", "lease", "rental agreement"],
    "medical": ["medical report", "prescription", "doctor", "blood test", "hospital", "health checkup"],
    "school": ["homework", "assignment", "lecture notes", "exam", "syllabus", "college", "university"],
    "letter": ["letter", "dear sir", "yours sincerely", "regards", "request"],
    "manual": ["user manual", "instructions", "how to install", "setup guide", "troubleshooting"],
    "backup": ["backup", "archive", "compressed files", "zip file"],
    "config": ["configuration", "settings", "json config", "key value"],
}
EXTRA = [
    "cv", "cv resume", "resume", "curriculum vitae", "my cv", "biodata", "job profile", "id card", "pan card",
    "aadhaar card", "passport", "visa", "driving licence", "pdf", "pdf document", "jpg photo", "png image",
    "mp3 song", "mp4 video", "docx word document", "xlsx spreadsheet", "ppt slides presentation", "txt text notes",
    "md markdown notes", "zip archive", "iso disk image", "deb installer package", "exe program",
    "c source code", "py python script", "js javascript code", "json config data", "csv spreadsheet data",
    "source code", "program source", "program code", "code", "script", "software", "app source",
    "bill", "invoice", "receipt", "statement", "minutes", "meeting minutes", "notes", "todo", "photos", "pictures",
    "holiday pictures", "vacation photos", "selfies", "screenshots", "songs", "movies", "recipes", "cooking", "food",
]
CODE = [
    "#include <stdio.h> int main(void) { printf(\"hello\\n\"); return 0; }",
    "int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }",
    "#include <stdlib.h> void *p = malloc(64); free(p);",
    "for (int i = 0; i < n; i++) { sum += a[i]; }",
    "struct node { int value; struct node *next; };",
    "def main(): print(\"hello\") if __name__ == \"__main__\": main()",
    "import os import sys for name in os.listdir(path): print(name)",
    "class Stack: def push(self, x): self.items.append(x)",
    "function add(a, b) { return a + b; } console.log(add(2, 3));",
    "const express = require('express'); app.get('/', (req, res) => res.send('ok'));",
    "#!/bin/sh for f in *.txt; do echo $f; done",
    "SELECT name, email FROM users WHERE age > 30 ORDER BY name;",
    "public static void main(String[] args) { System.out.println(\"hi\"); }",
    "fn main() { let x = 5; println!(\"{}\", x); }",
    "package main import \"fmt\" func main() { fmt.Println(\"hi\") }",
    "if (x == NULL) { return -1; } else { x->count++; }",
    "while (1) { read(fd, buffer, sizeof(buffer)); }",
    "typedef struct { float x, y; } point_t;",
    "git commit -m \"fix bug\" && git push",
    "make clean && make all",
]
MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
          "november", "december"]
NAMES = ["amit", "priya", "rahul", "sara", "john", "maria", "wei", "fatima", "arjun", "neha"]
PLACES = ["goa", "delhi", "paris", "london", "tokyo", "mumbai", "new york", "bangalore", "the mountains", "the beach"]
SHOPS = ["amazon", "flipkart", "electricity board", "internet provider", "the hospital", "the hotel", "a restaurant",
         "the phone company", "the landlord", "the gym"]


def synthetic(count, rng):
    out = []
    for _ in range(count):
        kind = rng.choice(list(KINDS))
        phrases = rng.sample(KINDS[kind], k=min(len(KINDS[kind]), rng.randint(1, 3)))
        extra = []
        if rng.random() < 0.5:
            extra.append(rng.choice(MONTHS))
        if rng.random() < 0.4:
            extra.append(rng.choice(NAMES))
        if kind in ("invoice", "receipt", "bank") and rng.random() < 0.6:
            extra.append(rng.choice(SHOPS))
            extra.append(f"{rng.randint(10, 99999)} rupees")
        if kind in ("travel", "photo") and rng.random() < 0.6:
            extra.append(rng.choice(PLACES))
        words = phrases + extra
        rng.shuffle(words)
        text = " ".join(words)
        style = rng.random()
        if style < 0.25:
            text = "_".join(text.split()[:4]) + rng.choice([".pdf", ".txt", ".docx", ".jpg", ".png", ".md", ".c", ".csv"])
        elif style < 0.35:
            text = " ".join(w.capitalize() for w in text.split())
        out.append(text)
    return out


def load_wiki(limit, rng):
    from huggingface_hub import hf_hub_download
    import pyarrow.parquet as pq

    path = hf_hub_download("Salesforce/wikitext", "wikitext-103-raw-v1/train-00000-of-00002.parquet",
                           repo_type="dataset")
    paragraphs = [p for p in pq.read_table(path).column("text").to_pylist()
                  if len(p.strip()) > 40 and not p.strip().startswith("=")]
    sentences = []
    for p in paragraphs:
        for s in p.replace(" @-@ ", "-").replace(" @,@ ", ",").replace(" @.@ ", ".").split(" . "):
            n = len(s.split())
            if 5 <= n <= 40:
                sentences.append(s.strip())
    rng.shuffle(sentences)
    return sentences[:limit], paragraphs


def vocabulary(paragraphs, limit):
    counts = {}
    for p in paragraphs[:200000]:
        for w in re.findall(r"[a-z]+", p.lower()):
            counts[w] = counts.get(w, 0) + 1
    ranked = sorted(counts, key=lambda w: -counts[w])
    return [w for w in ranked if len(w) > 1][:limit]


def phrase_pool(kind):
    return KINDS[kind] + SYNONYMS.get(kind, [])


def pairs(count, rng):
    queries = []
    docs = []
    kinds = list(KINDS)
    for _ in range(count):
        kind = rng.choice(kinds)
        pool = phrase_pool(kind)
        query = rng.choice(pool)
        if rng.random() < 0.3:
            query = rng.choice(query.split())
        others = [p for p in pool if p != query and query not in p.split()]
        body = rng.sample(others, k=min(len(others), rng.randint(2, 4)))
        if rng.random() < 0.5:
            body.append(rng.choice(MONTHS))
        if rng.random() < 0.3:
            body.append(rng.choice(NAMES))
        name = "_".join(rng.choice(others).split()[:2]) + rng.choice([".txt", ".pdf", ".md", ".jpg", ".c", ".docx"])
        docs.append(f"{name} {name} {' '.join(body)}")
        queries.append(query)
    return queries, docs


def build_corpus(args):
    rng = random.Random(7)
    path = os.path.join(CACHE, "corpus.txt")
    if os.path.exists(path):
        with open(path) as f:
            return f.read().split("\n")
    sentences, paragraphs = load_wiki(args.sentences, rng)
    texts = sentences + vocabulary(paragraphs, args.words) + synthetic(args.synthetic, rng)
    texts += [w for kind in KINDS.values() for w in kind]
    texts = [t.replace("\n", " ") for t in texts if t.strip()]
    os.makedirs(CACHE, exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(texts))
    return texts


def build_extra():
    path = os.path.join(CACHE, "extra.txt")
    if os.path.exists(path):
        with open(path) as f:
            return f.read().split("\n")
    rng = random.Random(11)
    texts = EXTRA + CODE + [c.replace("(", " ").replace(")", " ") for c in CODE]
    texts += [" ".join(rng.sample(CODE, 2)) for _ in range(300)]
    texts += synthetic(15000, rng)
    with open(path, "w") as f:
        f.write("\n".join(t.replace("\n", " ") for t in texts))
    return texts


def teacher_vectors(texts, name="teacher.npy"):
    path = os.path.join(CACHE, name)
    if os.path.exists(path):
        return np.load(path)
    from sentence_transformers import SentenceTransformer

    model = SentenceTransformer(TEACHER, device="cpu")
    started = time.time()
    vectors = model.encode(texts, batch_size=256, convert_to_numpy=True, normalize_embeddings=True,
                           show_progress_bar=True)
    print(f"teacher: {len(texts)} texts in {time.time() - started:.0f} s")
    np.save(path, vectors.astype(np.float32))
    return vectors


def reduce(vectors, dim):
    mean = vectors.mean(axis=0)
    _, _, vt = np.linalg.svd(vectors[:50000] - mean, full_matrices=False)
    reduced = (vectors - mean) @ vt[:dim].T
    return reduced / np.linalg.norm(reduced, axis=1, keepdims=True)


class Student(torch.nn.Module):
    def __init__(self, buckets, dim):
        super().__init__()
        self.table = torch.nn.EmbeddingBag(buckets, dim, mode="mean", sparse=True)
        torch.nn.init.normal_(self.table.weight, 0, 0.05)

    def forward(self, feature_ids, feature_offsets, word_sample, samples):
        word_vectors = self.table(feature_ids, feature_offsets)
        out = torch.zeros(samples, word_vectors.shape[1])
        out.index_add_(0, word_sample, word_vectors)
        return torch.nn.functional.normalize(out, dim=1)


def encode_words(texts, buckets):
    vocab = {}
    word_features = []
    samples = []
    for text in texts:
        ids = []
        for w in T.words(text):
            index = vocab.get(w)
            if index is None:
                index = len(word_features)
                vocab[w] = index
                word_features.append(np.array(T.features(w, buckets), dtype=np.int64))
            ids.append(index)
        samples.append(np.array(ids, dtype=np.int64))
    return samples, word_features


def batch_tensors(batch, samples, word_features):
    feature_ids = []
    offsets = []
    word_sample = []
    position = 0
    for row, s in enumerate(batch):
        for w in samples[s]:
            offsets.append(position)
            f = word_features[w]
            feature_ids.append(f)
            position += len(f)
            word_sample.append(row)
    if not offsets:
        return None
    return (torch.from_numpy(np.concatenate(feature_ids)), torch.tensor(offsets), torch.tensor(word_sample))


def embed(model_table, text, buckets):
    vector = np.zeros(model_table.shape[1], dtype=np.float32)
    for w in T.words(text):
        vector += model_table[T.features(w, buckets)].mean(axis=0)
    norm = np.linalg.norm(vector)
    return vector / norm if norm else vector


def evaluate(table, buckets):
    docs = {
        "bill_march.pdf": "bill march pdf Documents Invoice number 4471 amount due 4500 rupees electricity board",
        "pasta.txt": "pasta txt recipes Boil the pasta, fry garlic in olive oil, add tomatoes and basil",
        "fib.c": "fib c fib c code source code program include stdio int fib int n return n less than 2 printf main",
        "trip_goa.jpg": "trip goa jpg trip goa jpg Photos image picture photo",
        "resume_2026.pdf": "profile 2026 txt profile 2026 txt work text Priya Sharma Software engineer Work experience "
                           "4 years education skills",
        "meeting.md": "meeting md notes agenda action items attendees project deadline",
    }
    queries = {"invoice": "bill_march.pdf", "cooking": "pasta.txt", "program source": "fib.c",
               "holiday pictures": "trip_goa.jpg", "cv": "resume_2026.pdf", "resume": "resume_2026.pdf",
               "minutes": "meeting.md", "payment": "bill_march.pdf", "food": "pasta.txt",
               "job application": "resume_2026.pdf", "vacation": "trip_goa.jpg", "debug": "fib.c"}
    vectors = {name: embed(table, text, buckets) for name, text in docs.items()}
    hits = 0
    for query, want in queries.items():
        q = embed(table, query, buckets)
        best = max(vectors, key=lambda name: float(q @ vectors[name]))
        hits += best == want
        print(f"  {query:18} -> {best:18} {'ok' if best == want else 'MISS (wanted ' + want + ')'}")
    print(f"retrieval check: {hits}/{len(queries)}")
    return hits


def export(table, buckets, dim):
    scales = np.abs(table).max(axis=1) / 127.0
    scales[scales == 0] = 1.0
    quantized = np.clip(np.round(table / scales[:, None]), -127, 127).astype(np.int8)
    with open(OUT, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIIIIIII", buckets, dim, T.MIN_N, T.MAX_N, T.MAX_WORDS, T.MAX_WORD, 0, 0))
        f.write(scales.astype(np.float32).tobytes())
        f.write(quantized.tobytes())
    print(f"wrote {OUT}: {os.path.getsize(OUT) // 1024} KiB")
    return quantized.astype(np.float32) * scales[:, None]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--buckets", type=int, default=1 << 17)
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--sentences", type=int, default=150000)
    parser.add_argument("--words", type=int, default=40000)
    parser.add_argument("--synthetic", type=int, default=30000)
    parser.add_argument("--pairs", type=int, default=60000)
    parser.add_argument("--contrast-after", type=int, default=3)
    parser.add_argument("--contrast-weight", type=float, default=0.1)
    args = parser.parse_args()

    torch.manual_seed(7)
    torch.set_num_threads(os.cpu_count())
    texts = build_corpus(args)
    extra = build_extra()
    vectors = np.concatenate([teacher_vectors(texts), teacher_vectors(extra, "extra.npy")])
    texts = texts + extra
    reduced = reduce(vectors, args.dim).astype(np.float32)
    repeat = list(range(len(texts) - len(extra), len(texts))) * 2
    texts = texts + [texts[i] for i in repeat]
    reduced = np.concatenate([reduced, reduced[repeat]])
    targets = torch.from_numpy(reduced)
    samples, word_features = encode_words(texts, args.buckets)
    print(f"{len(texts)} texts, {len(word_features)} different words")

    queries, docs = pairs(args.pairs, random.Random(5))
    pair_texts = queries + docs
    pair_samples, pair_features = encode_words(pair_texts, args.buckets)
    pair_count = len(queries)

    model = Student(args.buckets, args.dim)
    optimizer = torch.optim.SparseAdam(model.parameters(), lr=0.01)
    order = list(range(len(texts)))
    pair_order = list(range(pair_count))
    pair_step = 0
    for epoch in range(args.epochs):
        random.Random(epoch).shuffle(order)
        random.Random(100 + epoch).shuffle(pair_order)
        total = 0.0
        contrast = 0.0
        count = 0
        steps = 0
        for start in range(0, len(order), 512):
            batch = [s for s in order[start:start + 512] if len(samples[s])]
            tensors = batch_tensors(batch, samples, word_features)
            if tensors is None:
                continue
            out = model(*tensors, len(batch))
            loss = (1 - (out * targets[batch]).sum(dim=1)).mean()
            if epoch >= args.contrast_after:
                chosen = [pair_order[(pair_step + k) % pair_count] for k in range(128)]
                pair_step += 128
                query_tensors = batch_tensors(chosen, pair_samples, pair_features)
                doc_tensors = batch_tensors([pair_count + c for c in chosen], pair_samples, pair_features)
                q = model(*query_tensors, len(chosen))
                d = model(*doc_tensors, len(chosen))
                logits = q @ d.T / 0.05
                nce = torch.nn.functional.cross_entropy(logits, torch.arange(len(chosen)))
                loss = loss + args.contrast_weight * nce
                contrast += float(nce)
                steps += 1
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            total += float(loss) * len(batch)
            count += len(batch)
        print(f"epoch {epoch + 1}: loss {total / count:.4f}" + (f", pairs {contrast / steps:.3f}" if steps else ""))

    table = export(model.table.weight.detach().numpy(), args.buckets, args.dim)
    evaluate(table, args.buckets)


if __name__ == "__main__":
    main()
