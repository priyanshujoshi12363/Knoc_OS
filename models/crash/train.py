import argparse
import csv
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.append(os.path.join(HERE, "..", "filenet"))

import knm

LABELS = ["null_pointer", "bad_pointer", "unallocated", "stack_overflow", "bad_jump",
          "illegal_instruction", "misaligned", "code_corruption", "kernel_panic", "kernel_freeze"]
HEADER = os.path.join(ROOT, "kernel", "crashnet_model.h")


FEATURES = 107


def load(paths):
    runs, x, y = [], [], []
    for path in paths:
        for row in csv.DictReader(open(path)):
            if len(row["features"].split(",")) != FEATURES:
                continue
            runs.append(int(row["run"]))
            x.append([int(v) / 1000 for v in row["features"].split(",")])
            y.append(LABELS.index(row["label"]))
    return np.array(runs), np.array(x, dtype=np.float32), np.array(y)


def train(x, y, hidden, epochs, rng):
    sizes = [x.shape[1]] + hidden + [len(LABELS)]
    params = [[rng.normal(0, np.sqrt(2 / a), (a, b)).astype(np.float32), np.zeros(b, np.float32)]
              for a, b in zip(sizes[:-1], sizes[1:])]
    moments = [[np.zeros_like(p) for p in layer] for layer in params]
    velocities = [[np.zeros_like(p) for p in layer] for layer in params]
    counts = np.bincount(y, minlength=len(LABELS)).astype(np.float32)
    weights = (counts.sum() / np.maximum(counts, 1) / len(LABELS)).astype(np.float32)
    step = 0
    for epoch in range(epochs):
        lr = 0.003 * 0.5 * (1 + np.cos(np.pi * epoch / epochs))
        order = rng.permutation(len(x))
        for start in range(0, len(order), 32):
            batch = order[start:start + 32]
            activations = forward(params, x[batch])
            logits = activations[-1] - activations[-1].max(1, keepdims=True)
            probabilities = np.exp(logits) / np.exp(logits).sum(1, keepdims=True)
            probabilities[np.arange(len(batch)), y[batch]] -= 1
            grad = probabilities * weights[y[batch]][:, None] / len(batch)
            step += 1
            for i in reversed(range(len(params))):
                w, b = params[i]
                grad_w = activations[i].T @ grad + 1e-4 * w
                grad_b = grad.sum(0)
                if i > 0:
                    grad = (grad @ w.T) * (activations[i] > 0)
                for slot, g in ((0, grad_w), (1, grad_b)):
                    moments[i][slot] = 0.9 * moments[i][slot] + 0.1 * g
                    velocities[i][slot] = 0.999 * velocities[i][slot] + 0.001 * g * g
                    m = moments[i][slot] / (1 - 0.9 ** step)
                    v = velocities[i][slot] / (1 - 0.999 ** step)
                    params[i][slot] -= lr * m / (np.sqrt(v) + 1e-8)
    return params


def forward(params, x):
    activations = [x]
    for i, (w, b) in enumerate(params):
        z = activations[-1] @ w + b
        activations.append(np.maximum(z, 0) if i < len(params) - 1 else z)
    return activations


def report(title, predicted, expected):
    print(title)
    for index, label in enumerate(LABELS):
        selected = expected == index
        if selected.any():
            print(f"  {label:<20} {(predicted[selected] == index).mean() * 100:6.2f}%  ({int(selected.sum())} crashes)")
    print(f"  overall {(predicted == expected).mean() * 100:.2f}%")


def c_array(kind, name, values, per_line=24):
    def number(v):
        text = f"{float(v):.9g}"
        return text + ("f" if "." in text or "e" in text else ".0f")

    items = [str(int(v)) if kind != "float" else number(v) for v in values]
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return f"static const {kind} {name}[{len(items)}] = {{\n    " + ",\n    ".join(lines) + "\n};\n"


def export_header(quantized, features):
    out = ["#ifndef CRASHNET_MODEL_H", "#define CRASHNET_MODEL_H", "",
           "#define CRASHNET_READY 1", f"#define CRASHNET_FEATURES {features}",
           f"#define CRASHNET_CLASSES {len(LABELS)}", f"#define CRASHNET_LAYERS {len(quantized)}", ""]
    for i, layer in enumerate(quantized):
        out.append(c_array("signed char", f"crashnet_w{i}", layer["weights"].reshape(-1)))
        out.append(c_array("int", f"crashnet_b{i}", layer["bias"]))
        out.append(c_array("float", f"crashnet_s{i}", layer["w_scale"]))
    count = len(quantized)
    out.append(c_array("unsigned int", "crashnet_inputs", [l["in"] for l in quantized]))
    out.append(c_array("unsigned int", "crashnet_outputs", [l["out"] for l in quantized]))
    out.append(c_array("int", "crashnet_relu", [1 if l["relu"] else 0 for l in quantized]))
    out.append(c_array("float", "crashnet_in_scale", [l["in_scale"] for l in quantized]))
    out.append(c_array("float", "crashnet_out_scale", [l["out_scale"] for l in quantized]))
    out.append(f"static const signed char *const crashnet_weights[{count}] = {{"
               + ", ".join(f"crashnet_w{i}" for i in range(count)) + "};")
    out.append(f"static const int *const crashnet_bias[{count}] = {{"
               + ", ".join(f"crashnet_b{i}" for i in range(count)) + "};")
    out.append(f"static const float *const crashnet_w_scale[{count}] = {{"
               + ", ".join(f"crashnet_s{i}" for i in range(count)) + "};")
    out += ["", "#endif", ""]
    open(HEADER, "w").write("\n".join(out))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=150)
    parser.add_argument("--hidden", type=int, nargs=2, default=[32, 16])
    args = parser.parse_args()

    data = os.path.join(HERE, "data")
    paths = sorted(os.path.join(data, name) for name in os.listdir(data) if name.endswith(".csv"))
    runs, x, y = load(paths)
    rng = np.random.default_rng(1)

    groups = sorted(set(runs))
    folds = [groups[i::4] for i in range(4)]
    predicted = np.zeros_like(y)
    for held in folds:
        test = np.isin(runs, held)
        params = train(x[~test], y[~test], args.hidden, args.epochs, rng)
        predicted[test] = forward(params, x[test])[-1].argmax(1)
    report("4-fold test, every crash judged by a model that never saw its run:", predicted, y)

    params = train(x, y, args.hidden, args.epochs, rng)
    scales = [float(np.percentile(a, 99.99)) or 1.0 for a in forward(params, x)[1:-1]]
    quantized = knm.quantize_layers([(w, b) for w, b in params], scales)
    path = os.path.join(HERE, "crashnet.knm")
    knm.save(path, quantized, [LABELS], config=(x.shape[1], len(LABELS), 1, 0, 0, 0))
    model = knm.load(path)
    probabilities = knm.infer(model, x)[0]
    report(f"int8 model on all data ({model['bytes'] / 1024:.1f} KiB):", probabilities.argmax(1), y)
    export_header(quantized, x.shape[1])
    print(f"wrote {HEADER}")


if __name__ == "__main__":
    main()
