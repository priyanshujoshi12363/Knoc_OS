import argparse
import csv
import os
import time

import numpy as np

import features as F
import knm

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data", "files")
CACHE = os.path.join(HERE, "cache")


def load_split(split):
    cache = os.path.join(CACHE, f"{split}.npz")
    source = os.path.join(DATA, f"{split}.csv")
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(source):
        stored = np.load(cache, allow_pickle=True)
        return stored["x"], stored["types"], stored["sources"], list(stored["names"]), list(stored["origin"])

    rows = list(csv.DictReader(open(source)))
    x = np.zeros((len(rows), F.FEATURES), dtype=np.float32)
    for i, row in enumerate(rows):
        x[i] = F.features(row["name"], int(row["size"]), bytes.fromhex(row["head_hex"]))
    types = np.array([F.TYPES.index(row["label"]) for row in rows])
    sources = np.array([F.SOURCES.index(row["source"]) for row in rows])
    names = [row["name"] for row in rows]
    origin = [row["origin"] for row in rows]
    os.makedirs(CACHE, exist_ok=True)
    np.savez(cache, x=x, types=types, sources=sources, names=np.array(names, dtype=object),
             origin=np.array(origin, dtype=object))
    return x, types, sources, names, origin


class Network:
    def __init__(self, sizes, rng):
        self.params = []
        for inputs, outputs in zip(sizes[:-1], sizes[1:]):
            weights = rng.normal(0, np.sqrt(2.0 / inputs), (inputs, outputs)).astype(np.float32)
            self.params.append([weights, np.zeros(outputs, dtype=np.float32)])
        self.moments = [[np.zeros_like(p) for p in layer] for layer in self.params]
        self.velocities = [[np.zeros_like(p) for p in layer] for layer in self.params]
        self.step = 0

    def forward(self, x, dropout=0.0, rng=None):
        activations = [x]
        masks = []
        for index, (weights, bias) in enumerate(self.params):
            z = activations[-1] @ weights + bias
            if index < len(self.params) - 1:
                z = np.maximum(z, 0)
                if dropout > 0:
                    mask = (rng.random(z.shape) >= dropout).astype(np.float32) / (1 - dropout)
                    z = z * mask
                    masks.append(mask)
                else:
                    masks.append(None)
            activations.append(z)
        return activations, masks

    def train_batch(self, x, types, sources, lr, dropout, decay, rng):
        activations, masks = self.forward(x, dropout, rng)
        logits = activations[-1]
        grad = np.zeros_like(logits)
        loss = 0.0
        for start, count, labels in ((0, len(F.TYPES), types), (len(F.TYPES), len(F.SOURCES), sources)):
            part = logits[:, start:start + count]
            part = part - part.max(axis=1, keepdims=True)
            probabilities = np.exp(part) / np.exp(part).sum(axis=1, keepdims=True)
            loss -= np.log(probabilities[np.arange(len(labels)), labels] + 1e-9).mean()
            probabilities[np.arange(len(labels)), labels] -= 1
            grad[:, start:start + count] = probabilities / len(labels)

        self.step += 1
        for index in reversed(range(len(self.params))):
            weights, bias = self.params[index]
            grad_w = activations[index].T @ grad + decay * weights
            grad_b = grad.sum(axis=0)
            if index > 0:
                grad = grad @ weights.T
                grad = grad * (activations[index] > 0)
                if masks[index - 1] is not None:
                    grad = grad * masks[index - 1]
            for slot, g in ((0, grad_w), (1, grad_b)):
                m = self.moments[index][slot] = 0.9 * self.moments[index][slot] + 0.1 * g
                v = self.velocities[index][slot] = 0.999 * self.velocities[index][slot] + 0.001 * g * g
                m_hat = m / (1 - 0.9 ** self.step)
                v_hat = v / (1 - 0.999 ** self.step)
                self.params[index][slot] -= lr * m_hat / (np.sqrt(v_hat) + 1e-8)
        return loss

    def predict(self, x):
        logits = self.forward(x)[0][-1]
        return logits[:, :len(F.TYPES)].argmax(1), logits[:, len(F.TYPES):].argmax(1)

    def hidden_scales(self, x):
        activations = self.forward(x)[0]
        return [float(np.percentile(a, 99.99)) for a in activations[1:-1]]


def accuracy(predicted, expected):
    return float((predicted == expected).mean())


def report(title, predicted, expected, names):
    print(f"\n{title}")
    for index, name in enumerate(names):
        selected = expected == index
        if selected.any():
            wrong = predicted[selected] != index
            print(f"  {name:<17} {accuracy(predicted[selected], expected[selected]) * 100:6.2f}%"
                  f"  ({int(wrong.sum())} wrong of {int(selected.sum())})")


CONFIGS = [
    {"hidden": [256, 128], "dropout": 0.2, "lr": 0.002},
    {"hidden": [192, 96], "dropout": 0.1, "lr": 0.002},
    {"hidden": [128, 64], "dropout": 0.1, "lr": 0.003},
    {"hidden": [320, 96], "dropout": 0.3, "lr": 0.0015},
]


def train_model(config, epochs, seed, data):
    x_train, t_train, s_train, x_val, t_val, s_val = data
    rng = np.random.default_rng(seed)
    network = Network([F.FEATURES] + config["hidden"] + [len(F.TYPES) + len(F.SOURCES)], rng)
    best = (-1.0, None)
    for epoch in range(1, epochs + 1):
        order = rng.permutation(len(x_train))
        lr = config["lr"] * (0.5 * (1 + np.cos(np.pi * (epoch - 1) / epochs)))
        for start in range(0, len(order), 128):
            batch = order[start:start + 128]
            network.train_batch(x_train[batch], t_train[batch], s_train[batch],
                                lr, config["dropout"], 1e-5, rng)
        type_pred, source_pred = network.predict(x_val)
        score = (accuracy(type_pred, t_val) + accuracy(source_pred, s_val)) / 2
        if score > best[0]:
            best = (score, [[p.copy() for p in layer] for layer in network.params])
    network.params = best[1]
    return network, best[0]


def fit_temperature(logits, labels):
    best = (np.inf, 1.0)
    for temperature in np.arange(0.5, 6.01, 0.05):
        scaled = logits / temperature
        scaled = scaled - scaled.max(axis=1, keepdims=True)
        log_probabilities = scaled - np.log(np.exp(scaled).sum(axis=1, keepdims=True))
        loss = -log_probabilities[np.arange(len(labels)), labels].mean()
        if loss < best[0]:
            best = (loss, float(temperature))
    return best[1]


def confidence_report(title, probabilities, labels, threshold):
    predicted = probabilities.argmax(1)
    sure = probabilities.max(1) >= threshold
    covered = sure.mean() * 100
    right = accuracy(predicted[sure], labels[sure]) * 100 if sure.any() else 0.0
    print(f"  {title:<7} sure on {covered:5.1f}% of files, correct on {right:6.2f}% of those")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()

    started = time.time()
    x_train, t_train, s_train, _, _ = load_split("train")
    x_val, t_val, s_val, _, _ = load_split("val")
    x_test, t_test, s_test, names_test, origin_test = load_split("test")
    print(f"features ready in {time.time() - started:.1f} s: train {len(x_train)}, "
          f"val {len(x_val)}, test {len(x_test)}, {F.FEATURES} features")
    data = (x_train, t_train, s_train, x_val, t_val, s_val)

    configs = CONFIGS[:1] if args.quick else CONFIGS
    results = []
    for config in configs:
        begun = time.time()
        network, score = train_model(config, args.epochs, args.seed, data)
        results.append((score, config, network))
        print(f"hidden {config['hidden']}, dropout {config['dropout']}, lr {config['lr']}: "
              f"val {score * 100:.2f}%  ({time.time() - begun:.0f} s)")
    score, config, network = max(results, key=lambda result: result[0])
    print(f"best settings: hidden {config['hidden']}, dropout {config['dropout']}, lr {config['lr']}")

    logits = network.forward(x_val)[0][-1]
    type_temperature = fit_temperature(logits[:, :len(F.TYPES)], t_val)
    source_temperature = fit_temperature(logits[:, len(F.TYPES):], s_val)
    print(f"confidence calibration: type temperature {type_temperature:.2f}, "
          f"source temperature {source_temperature:.2f}")

    last_weights, last_bias = network.params[-1]
    scale = np.ones(last_weights.shape[1], dtype=np.float32)
    scale[:len(F.TYPES)] /= type_temperature
    scale[len(F.TYPES):] /= source_temperature
    network.params[-1] = [last_weights * scale, last_bias * scale]

    scales = network.hidden_scales(x_train)
    path = os.path.join(HERE, "filenet.knm")
    knm.save(path, knm.quantize_layers([(w, b) for w, b in network.params], scales),
             [F.TYPES, F.SOURCES])

    type_float, source_float = network.predict(x_test)
    model = knm.load(path)
    type_prob, source_prob = knm.infer(model, x_test)
    type_int8, source_int8 = type_prob.argmax(1), source_prob.argmax(1)

    print(f"\nsaved {path} ({model['bytes'] / 1024:.0f} KiB, int8)")
    print(f"test (float)  type {accuracy(type_float, t_test) * 100:6.2f}%   "
          f"source {accuracy(source_float, s_test) * 100:6.2f}%")
    print(f"test (int8)   type {accuracy(type_int8, t_test) * 100:6.2f}%   "
          f"source {accuracy(source_int8, s_test) * 100:6.2f}%")

    no_extension = np.array(["." not in n for n in names_test])
    real = np.array([o == "real" for o in origin_test])
    print(f"test, names without extension: type {accuracy(type_int8[no_extension], t_test[no_extension]) * 100:6.2f}%"
          f" ({int(no_extension.sum())} files)")
    print(f"test, real system files:       type {accuracy(type_int8[real], t_test[real]) * 100:6.2f}%"
          f" ({int(real.sum())} files)")

    print("\nconfidence at the organizer's 90% threshold (test set):")
    confidence_report("type", type_prob, t_test, 0.9)
    confidence_report("source", source_prob, s_test, 0.9)

    report("int8 type accuracy by class", type_int8, t_test, F.TYPES)
    report("int8 source accuracy by class", source_int8, s_test, F.SOURCES)

    mistakes = [(names_test[i], F.TYPES[t_test[i]], F.TYPES[type_int8[i]], F.SOURCES[s_test[i]],
                 F.SOURCES[source_int8[i]]) for i in range(len(names_test))
                if type_int8[i] != t_test[i] or source_int8[i] != s_test[i]]
    print(f"\nmistakes: {len(mistakes)}")
    for name, true_type, got_type, true_source, got_source in mistakes[:15]:
        print(f"  {name[:48]:<48} type {true_type}->{got_type}  source {true_source}->{got_source}")


if __name__ == "__main__":
    main()
