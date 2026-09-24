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


PROBLEMS = H.LABELS[1:]


def targets(label):
    parts = set(label.split("+"))
    return [1.0 if problem in parts else 0.0 for problem in PROBLEMS]


def load_windows(paths):
    runs = {}
    for path in paths:
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
                y.append(targets(window[0]["label"]))
        windows[run] = (np.array(x, dtype=np.float32), np.array(y, dtype=np.float32))
    return windows


def sigmoid(z):
    return 1 / (1 + np.exp(-np.clip(z, -40, 40)))


def train(x, y, hidden, epochs, rng):
    sizes = [x.shape[1]] + hidden + [len(PROBLEMS)]
    params = [[rng.normal(0, np.sqrt(2 / a), (a, b)).astype(np.float32), np.zeros(b, np.float32)]
              for a, b in zip(sizes[:-1], sizes[1:])]
    moments = [[np.zeros_like(p) for p in layer] for layer in params]
    velocities = [[np.zeros_like(p) for p in layer] for layer in params]
    positive = y.mean(0)
    pos_weight = (0.5 / np.maximum(positive, 1e-3)).astype(np.float32)
    neg_weight = (0.5 / np.maximum(1 - positive, 1e-3)).astype(np.float32)
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
            labels = y[batch]
            weight = labels * pos_weight + (1 - labels) * neg_weight
            grad = (sigmoid(activations[-1]) - labels) * weight / len(batch)
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


def report(title, probabilities, expected):
    predicted = probabilities >= 0.8
    print(title)
    for index, problem in enumerate(PROBLEMS):
        has = expected[:, index] == 1
        found = predicted[has, index].mean() * 100 if has.any() else float("nan")
        wrong = int(predicted[~has, index].sum())
        print(f"  {problem:<13} found {found:6.2f}% of {int(has.sum()):4d}   false alarms {wrong}")
    multi = expected.sum(1) == 2
    if multi.any():
        both = (predicted[multi] == (expected[multi] == 1)).all(1).mean() * 100
        print(f"  two problems at once: both found exactly in {both:.2f}% of {int(multi.sum())} windows")
    normal = expected.sum(1) == 0
    print(f"  normal windows with any alarm: {int(predicted[normal].any(1).sum())} of {int(normal.sum())}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=120)
    parser.add_argument("--hidden", type=int, nargs=2, default=[64, 32])
    args = parser.parse_args()

    data = os.path.join(HERE, "data")
    paths = sorted(os.path.join(data, name) for name in os.listdir(data)
                   if name.startswith("samples") and name.endswith(".csv"))
    windows = load_windows(paths)
    runs = sorted(windows)
    rng = np.random.default_rng(1)

    for held_out in runs:
        x = np.concatenate([windows[r][0] for r in runs if r != held_out])
        y = np.concatenate([windows[r][1] for r in runs if r != held_out])
        params = train(x, y, args.hidden, args.epochs, rng)
        probabilities = sigmoid(forward(params, windows[held_out][0])[-1])
        report(f"test on unseen run {held_out}:", probabilities, windows[held_out][1])

    x = np.concatenate([windows[r][0] for r in runs])
    y = np.concatenate([windows[r][1] for r in runs])
    params = train(x, y, args.hidden, args.epochs, rng)
    scales = [float(np.percentile(a, 99.99)) for a in forward(params, x)[1:-1]]
    path = os.path.join(HERE, "health.knm")
    knm.save(path, knm.quantize_layers([(w, b) for w, b in params], scales), [PROBLEMS],
             config=(H.FEATURES, H.WINDOW, H.FEATURE_VERSION, 1, 0, 0))
    model = knm.load(path)
    report(f"saved {path} ({model['bytes'] / 1024:.1f} KiB), int8 model on all data:",
           sigmoid(knm.raw_logits(model, x)), y)


if __name__ == "__main__":
    main()
