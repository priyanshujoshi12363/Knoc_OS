import struct

import numpy as np

import features as F

MAGIC = b"KNOCNN01"
VERSION = 1


def quantize_layers(layers, activation_scales):
    quantized = []
    in_scale = 1.0 / 127
    for index, (weights, bias) in enumerate(layers):
        last = index == len(layers) - 1
        w_scale = np.maximum(np.abs(weights).max(axis=0), 1e-8) / 127
        w_scale = np.maximum(w_scale, np.abs(bias) / (in_scale * 2.0 ** 30))
        wq = np.clip(np.round(weights / w_scale), -127, 127).astype(np.int8)
        bq = np.round(bias / (in_scale * w_scale)).astype(np.int32)
        out_scale = 0.0 if last else float(activation_scales[index]) / 127
        quantized.append({"in": weights.shape[0], "out": weights.shape[1], "relu": not last,
                          "in_scale": in_scale, "out_scale": out_scale,
                          "w_scale": w_scale.astype(np.float32), "bias": bq,
                          "weights": np.ascontiguousarray(wq.T)})
        in_scale = out_scale
    return quantized


def save(path, quantized, heads, config=None):
    if config is None:
        config = (F.HIST, F.PAIRS, F.MAGIC, F.NAME, F.EXT, F.SHAPE)
    with open(path, "wb") as file:
        file.write(MAGIC)
        file.write(struct.pack("<I", VERSION))
        file.write(struct.pack("<6I", *config))
        file.write(struct.pack("<I", len(heads)))
        for names in heads:
            file.write(struct.pack("<I", len(names)))
            for name in names:
                encoded = name.encode()
                file.write(struct.pack("<B", len(encoded)) + encoded)
        file.write(struct.pack("<I", len(quantized)))
        for layer in quantized:
            file.write(struct.pack("<III", layer["in"], layer["out"], int(layer["relu"])))
            file.write(struct.pack("<ff", layer["in_scale"], layer["out_scale"]))
            file.write(layer["w_scale"].astype("<f4").tobytes())
            file.write(layer["bias"].astype("<i4").tobytes())
            file.write(layer["weights"].astype(np.int8).tobytes())


def load(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise ValueError("not a KnocOS model file")
    position = 8
    version, = struct.unpack_from("<I", data, position)
    position += 4
    config = struct.unpack_from("<6I", data, position)
    position += 24
    head_count, = struct.unpack_from("<I", data, position)
    position += 4
    heads = []
    for _ in range(head_count):
        count, = struct.unpack_from("<I", data, position)
        position += 4
        names = []
        for _ in range(count):
            length = data[position]
            names.append(data[position + 1:position + 1 + length].decode())
            position += 1 + length
        heads.append(names)
    layer_count, = struct.unpack_from("<I", data, position)
    position += 4
    layers = []
    for _ in range(layer_count):
        inputs, outputs, relu = struct.unpack_from("<III", data, position)
        position += 12
        in_scale, out_scale = struct.unpack_from("<ff", data, position)
        position += 8
        w_scale = np.frombuffer(data, "<f4", outputs, position)
        position += 4 * outputs
        bias = np.frombuffer(data, "<i4", outputs, position)
        position += 4 * outputs
        weights = np.frombuffer(data, np.int8, outputs * inputs, position).reshape(outputs, inputs)
        position += outputs * inputs
        layers.append({"in": inputs, "out": outputs, "relu": bool(relu), "in_scale": in_scale,
                       "out_scale": out_scale, "w_scale": w_scale, "bias": bias,
                       "weights": weights})
    return {"version": version, "config": config, "heads": heads, "layers": layers,
            "bytes": len(data)}


def raw_logits(model, x):
    xq = np.clip(np.round(np.asarray(x, dtype=np.float64) * 127), 0, 127).astype(np.int32)
    logits = None
    for layer in model["layers"]:
        acc = xq @ layer["weights"].astype(np.int32).T + layer["bias"]
        real = acc * (layer["in_scale"] * layer["w_scale"].astype(np.float64))
        if layer["relu"]:
            xq = np.clip(np.round(np.maximum(real, 0) / layer["out_scale"]), 0, 127).astype(np.int32)
        else:
            logits = real
    return logits


def infer(model, x):
    logits = raw_logits(model, x)
    results = []
    start = 0
    for names in model["heads"]:
        part = logits[..., start:start + len(names)]
        part = part - part.max(axis=-1, keepdims=True)
        probabilities = np.exp(part) / np.exp(part).sum(axis=-1, keepdims=True)
        results.append(probabilities)
        start += len(names)
    return results
