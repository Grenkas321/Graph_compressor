#!/usr/bin/env python3
"""Generates a random test graph in the input format of the task."""
import random
import sys

def generate(path, vertex_count, avg_degree, gamma=2.6, loop_probability=0.0, weight_mode="uniform", seed=1):
    rnd = random.Random(seed)
    ids = rnd.sample(range(2 ** 32), vertex_count)
    ids.sort()

    # Power law degrees with the requested mean.
    degrees = []
    for _ in range(vertex_count):
        d = int((rnd.random() ** (-1.0 / (gamma - 1))) * (avg_degree * (gamma - 2) / (gamma - 1)))
        degrees.append(max(1, min(d, vertex_count - 1)))

    stubs = []
    for index, degree in enumerate(degrees):
        stubs.extend([index] * degree)
    rnd.shuffle(stubs)

    edges = set()
    for i in range(0, len(stubs) - 1, 2):
        a, b = stubs[i], stubs[i + 1]
        if a == b and rnd.random() >= loop_probability:
            continue
        if a > b:
            a, b = b, a
        edges.add((a, b))

    with open(path, "w") as out:
        for a, b in edges:
            if weight_mode == "uniform":
                w = rnd.randrange(256)
            elif weight_mode == "constant":
                w = 7
            else:
                w = min(255, int(abs(rnd.gauss(0, 20))))
            out.write("%d\t%d\t%d\n" % (ids[a], ids[b], w))
    return len(edges)

if __name__ == "__main__":
    path = sys.argv[1]
    count = generate(path,
                     int(sys.argv[2]),
                     float(sys.argv[3]),
                     loop_probability=float(sys.argv[4]) if len(sys.argv) > 4 else 0.0,
                     weight_mode=sys.argv[5] if len(sys.argv) > 5 else "uniform",
                     seed=int(sys.argv[6]) if len(sys.argv) > 6 else 1)
    print("%s: %d edges" % (path, count))
