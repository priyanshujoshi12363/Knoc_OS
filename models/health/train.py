import argparse
import csv
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "filenet"))

import knm
import features as H

NUMERIC = ["seq", "cpu_busy", "switches", "syscalls", "denied", "processes", "spawns", "crashes",
           "disk_reads", "disk_writes", "disk_wait", "ram_free_kib", "ram_total_kib",
           "user_memory_kib", "disk_free_kib", "disk_total_kib", "top_cpu", "top_mem_kib",
           "top_sys", "top_spawn"]


def load_windows(path):
    runs = {}
    for row in csv.DictReader(open(path)):
        sample = {key: int(row[key]) for key in NUMERIC}
        sample["label"] = row["label"]
        runs.setdefault(int(row["run"]), []).append(sample)

    windows = {}
    for run, samples in runs.items():
        samples.sort(key=lambda s: s["seq"])
        x, y = [], []
        for end in range(H.WINDOW, len(samples) + 1):
            window = samples[end - H.WINDOW:end]
            labels = {s["label"] for s in window}
            consecutive = window[-1]["seq"] - window[0]["seq"] == H.WINDOW - 1
            if len(labels) == 1 and consecutive:
                x.append(H.window_features(window))
                y.append(H.LABELS.index(window[0]["label"]))
        windows[run] = (np.array(x, dtype=np.float32), np.array(y))
    return windows


def train(x, y, hidden, epochs, rng):
    sizes = [x.shape[1]] + hidden + [len(H.LABELS)]
    params = [[rng.normal(0, np.sqrt(2 / a), (a, b)).astype(np.float32), np.zeros(b, np.float32)]
              for a, b in zip(sizes[:-1], sizes[1:])]
    moments = [[np.zeros_like(p) for p in layer] for layer in params]
    velocities = [[np.zeros_like(p) for p in layer] for layer in params]
    counts = np.bincount(y, minlength=len(H.LABELS)).astype(np.float32)
    weights = (counts.sum() / np.maximum(counts, 1) / len(H.LABELS)).astype(np.float32)
    step = 0
    for epoch in range(epochs):
        lr = 0.003 * 0.5 * (1 + np.cos(np.pi * epoch / epochs))
        order = rng.permutation(len(x))
        for start in range(0, len(order), 64):
            batch = order[start:start + 64]
            activations = [x[batch]]
            for i, (w, b) in enumerate(params):
                z = activations[-1] @ w + b
                activations.append(np.maximum(z, 0) if i < len(params) - 1 else z)
            logits = activations[-1] - activations[-1].max(1, keepdims=True)
            probabilities = np.exp(logits) / np.exp(logits).sum(1, keepdims=True)
            labels = y[batch]
            probabilities[np.arange(len(batch)), labels] -= 1
            grad = probabilities * weights[labels][:, None] / len(batch)
            step += 1
            for i in reversed(range(len(params))):
                w, b = params[i]
                grad_w = activations[i].T @ grad + 1e-5 * w
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
    for index, label in enumerate(H.LABELS):
        selected = expected == index
        if selected.any():
            right = (predicted[selected] == index).mean() * 100
            print(f"  {label:<13} {right:6.2f}%  ({int(selected.sum())} windows)")
    false_alarms = ((expected == 0) & (predicted != 0)).sum()
    print(f"  false alarms: {int(false_alarms)} of {int((expected == 0).sum())} normal windows")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=120)
    parser.add_argument("--hidden", type=int, nargs=2, default=[64, 32])
    args = parser.parse_args()

    windows = load_windows(os.path.join(HERE, "data", "samples.csv"))
    runs = sorted(windows)
    rng = np.random.default_rng(1)

    for held_out in runs:
        x = np.concatenate([windows[r][0] for r in runs if r != held_out])
        y = np.concatenate([windows[r][1] for r in runs if r != held_out])
        params = train(x, y, args.hidden, args.epochs, rng)
        predicted = forward(params, windows[held_out][0])[-1].argmax(1)
        report(f"test on unseen run {held_out}:", predicted, windows[held_out][1])

    x = np.concatenate([windows[r][0] for r in runs])
    y = np.concatenate([windows[r][1] for r in runs])
    params = train(x, y, args.hidden, args.epochs, rng)
    scales = [float(np.percentile(a, 99.99)) for a in forward(params, x)[1:-1]]
    path = os.path.join(HERE, "health.knm")
    knm.save(path, knm.quantize_layers([(w, b) for w, b in params], scales), [H.LABELS],
             config=(H.FEATURES, H.WINDOW, H.FEATURE_VERSION, 0, 0, 0))
    model = knm.load(path)
    probabilities = knm.infer(model, x)[0]
    print(f"saved {path} ({model['bytes'] / 1024:.1f} KiB), int8 accuracy on all data: "
          f"{(probabilities.argmax(1) == y).mean() * 100:.2f}%")


if __name__ == "__main__":
    main()
