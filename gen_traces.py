#!/usr/bin/env python3
"""Writes CS:APP-format allocation traces for mm_bench: several seeds of each
workload family, as traces/<family>-s<seed>.rep. Each family models a pattern real
programs show (README.md, Workloads); `uniform` is only a baseline."""

import argparse
import math
import os
import random


class Trace:
    """Records ops and hands out ids, reusing freed ones so the id table stays small."""

    def __init__(self, rng):
        self.rng = rng
        self.ops = []
        self.live = {}  # id -> size
        self.free_ids = []
        self.next_id = 0

    def malloc(self, size):
        if self.free_ids:
            i = self.free_ids.pop()
        else:
            i = self.next_id
            self.next_id += 1
        self.live[i] = size
        self.ops.append(f"a {i} {size}")
        return i

    def free(self, i):
        del self.live[i]
        self.free_ids.append(i)
        self.ops.append(f"f {i}")

    def realloc(self, i, size):
        self.live[i] = size
        self.ops.append(f"r {i} {size}")

    def free_random(self):
        self.free(self.rng.choice(list(self.live)))

    def free_all(self):
        ids = list(self.live)
        self.rng.shuffle(ids)
        for i in ids:
            self.free(i)

    def write(self, path):
        with open(path, "w") as f:
            # CS:APP header: suggested heap size (unused), id count, op count, weight
            f.write(f"0\n{self.next_id}\n{len(self.ops)}\n1\n")
            f.write("\n".join(self.ops) + "\n")


# ---- size distributions ----

def small_size(rng):
    """Mostly a few hot small sizes, with a lognormal tail."""
    if rng.random() < 0.7:
        return rng.choice((8, 16, 24, 32, 48, 64))
    return max(1, min(4096, int(rng.lognormvariate(math.log(64), 1.0))))


def any_size(rng):
    """Small objects plus the occasional page-sized or larger buffer."""
    r = rng.random()
    if r < 0.85:
        return small_size(rng)
    return rng.randint(256, 8192) if r < 0.98 else rng.randint(16384, 262144)


# ---- workload families: each appends ops to t until it has about n ----

def churn(t, n):
    """Steady state: short-lived small objects around a ~2000-object live set."""
    while len(t.ops) < n:
        if len(t.live) < 2000 or t.rng.random() < 0.5:
            t.malloc(small_size(t.rng))
        else:
            t.free_random()


def ramp(t, n):
    """Live memory only grows: long-lived objects, rare frees."""
    while len(t.ops) < n:
        if t.live and t.rng.random() < 0.1:
            t.free_random()
        else:
            t.malloc(small_size(t.rng))


def peaks(t, n):
    """Build a large structure, then free all but 2% of it; repeat."""
    while len(t.ops) < n:
        for _ in range(t.rng.randint(5000, 20000)):
            t.malloc(any_size(t.rng))
        ids = list(t.live)
        t.rng.shuffle(ids)
        for i in ids[: len(ids) * 98 // 100]:
            t.free(i)


def plateau(t, n):
    """A long-lived set allocated up front, with short-lived churn beside it."""
    for _ in range(min(n // 4, 20000)):
        t.malloc(any_size(t.rng))
    short = []
    while len(t.ops) < n:
        if len(short) < 500 and (not short or t.rng.random() < 0.55):
            short.append(t.malloc(small_size(t.rng)))
        else:
            j = t.rng.randrange(len(short))
            short[j], short[-1] = short[-1], short[j]
            t.free(short.pop())


def bimodal(t, n):
    """Tiny nodes mixed with 4-64 KB buffers: a fragmentation stress test."""
    while len(t.ops) < n:
        if len(t.live) > 3000 and t.rng.random() < 0.5:
            t.free_random()
        elif t.rng.random() < 0.9:
            t.malloc(t.rng.choice((16, 24, 32)))
        else:
            t.malloc(t.rng.randint(4096, 65536))


def fifo(t, n):
    """Queue: objects are freed in the order they were allocated."""
    queue = []
    while len(t.ops) < n:
        if len(queue) < 1000 or t.rng.random() < 0.5:
            queue.append(t.malloc(small_size(t.rng)))
        else:
            t.free(queue.pop(0))


def lifo(t, n):
    """Stack: the newest object is freed first."""
    stack = []
    while len(t.ops) < n:
        if not stack or t.rng.random() < 0.52:
            stack.append(t.malloc(small_size(t.rng)))
        else:
            t.free(stack.pop())


def realloc_grow(t, n):
    """Up to ~200 growable buffers doubling through realloc (to 64 KiB), among small objects."""
    buffers = {}  # id -> size
    while len(t.ops) < n:
        r = t.rng.random()
        if not buffers or (r < 0.2 and len(buffers) < 200):
            size = t.rng.choice((16, 32, 64))
            buffers[t.malloc(size)] = size
        elif r < 0.3:
            i = t.rng.choice(list(buffers))
            del buffers[i]
            t.free(i)
        else:
            i = t.rng.choice(list(buffers))
            if buffers[i] < 1 << 16:
                buffers[i] *= 2
                t.realloc(i, buffers[i])
        if t.rng.random() < 0.5:
            t.malloc(small_size(t.rng))
        if len(t.live) > len(buffers) + 500:
            small = [i for i in t.live if i not in buffers]
            t.free(t.rng.choice(small))


def uniform(t, n):
    """Uniformly random sizes and lifetimes. Unrealistic; kept as a baseline."""
    while len(t.ops) < n:
        if t.live and t.rng.random() < 0.45:
            t.free_random()
        else:
            t.malloc(t.rng.randint(1, 16384))


FAMILIES = {
    "churn": churn, "ramp": ramp, "peaks": peaks, "plateau": plateau,
    "bimodal": bimodal, "fifo": fifo, "lifo": lifo, "realloc": realloc_grow,
    "uniform": uniform,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--out", default="traces", help="output directory")
    ap.add_argument("--seeds", type=int, default=5, help="traces per family")
    ap.add_argument("--ops", type=int, default=100000, help="approximate ops per trace")
    ap.add_argument("--families", default=",".join(FAMILIES), help="comma-separated families")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    for name in args.families.split(","):
        for seed in range(1, args.seeds + 1):
            # seeded by family name too, so families never share a random stream
            t = Trace(random.Random(f"{name}-{seed}"))
            FAMILIES[name](t, args.ops)
            t.free_all()  # programs that exit cleanly free everything
            path = os.path.join(args.out, f"{name}-s{seed}.rep")
            t.write(path)
            print(f"{path}: {len(t.ops)} ops, {t.next_id} ids")


if __name__ == "__main__":
    main()
