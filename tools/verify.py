import sys
def norm(path):
    s = []
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if not line:
                continue
            a, b, w = line.split('\t')
            a, b = int(a), int(b)
            if a > b:
                a, b = b, a
            s.append((a, b, int(w)))
    return sorted(s)
a, b = norm(sys.argv[1]), norm(sys.argv[2])
print("lines:", len(a), len(b))
print("MATCH" if a == b else "MISMATCH")
if a != b:
    sa, sb = set(a), set(b)
    print("only in first :", list(sa - sb)[:5])
    print("only in second:", list(sb - sa)[:5])
    sys.exit(1)
