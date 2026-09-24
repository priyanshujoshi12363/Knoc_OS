import math

WINDOW = 10
LABELS = ["normal", "memory_leak", "cpu_hog", "disk_thrash", "spawn_storm", "disk_filling"]
CULPRITS = {"memory_leak": "top_mem_name", "cpu_hog": "top_cpu_name", "disk_thrash": "top_sys_name",
            "spawn_storm": "top_spawn_name", "disk_filling": "top_sys_name"}
FEATURE_VERSION = 2


def clip(value):
    return 0.0 if value < 0 else 1.0 if value > 1 else value


def scaled_log(value, top):
    return clip(math.log2(1 + max(value, 0)) / top)


def squash(value, scale):
    return 0.5 + 0.5 * math.tanh(value / scale)


def used(s, total, free):
    return s[total] - s[free]


def metrics(s, base):
    ram_change = used(s, "ram_total_kib", "ram_free_kib") - used(base, "ram_total_kib", "ram_free_kib")
    disk_change = used(s, "disk_total_kib", "disk_free_kib") - used(base, "disk_total_kib", "disk_free_kib")
    return [
        clip(s["cpu_busy"] / 100),
        clip(s["top_cpu"] / 100),
        scaled_log(s["switches"], 16),
        scaled_log(s["syscalls"], 14),
        scaled_log(s["spawns"], 6),
        scaled_log(s["crashes"], 4),
        scaled_log(s["disk_reads"], 14),
        scaled_log(s["disk_writes"], 14),
        scaled_log(s["disk_wait"], 8),
        squash(ram_change, 16384),
        scaled_log(s["user_memory_kib"], 21),
        squash(disk_change, 4096),
        scaled_log(s["top_mem_kib"], 21),
        scaled_log(s["top_sys"], 14),
        scaled_log(s["top_spawn"], 6),
    ]


METRICS = 15
FEATURES = METRICS * 3 + 3


def window_features(window):
    rows = [metrics(s, window[0]) for s in window]
    vector = []
    for m in range(METRICS):
        values = [row[m] for row in rows]
        vector.append(sum(values) / len(values))
        vector.append(max(values))
        early = sum(values[:3]) / 3
        late = sum(values[-3:]) / 3
        vector.append(0.5 + 0.5 * max(-1.0, min(1.0, (late - early) * 4)))
    span = len(window) - 1
    first, last = window[0], window[-1]
    vector.append(squash((last["user_memory_kib"] - first["user_memory_kib"]) / span, 1024))
    disk_used_first = first["disk_total_kib"] - first["disk_free_kib"]
    disk_used_last = last["disk_total_kib"] - last["disk_free_kib"]
    vector.append(squash((disk_used_last - disk_used_first) / span, 512))
    vector.append(squash((last["top_mem_kib"] - first["top_mem_kib"]) / span, 1024))
    return vector
