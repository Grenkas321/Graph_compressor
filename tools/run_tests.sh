#!/bin/sh
# Round trip test suite: every generated graph is compressed, restored and
# compared with the original up to the order of the lines and of the first two
# identifiers in a line.
set -e
RUN=${1:-./build/run}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

check() {
    name=$1
    input=$2
    $RUN -s -i "$input" -o "$WORK/graph.bin"
    $RUN -d -i "$WORK/graph.bin" -o "$WORK/output.tsv"
    original=$(wc -c < "$input")
    compressed=$(wc -c < "$WORK/graph.bin")
    python3 tools/verify.py "$input" "$WORK/output.tsv" > "$WORK/verify.log"
    if grep -q MISMATCH "$WORK/verify.log"; then
        echo "FAIL $name"
        cat "$WORK/verify.log"
        exit 1
    fi
    echo "OK   $name: $original -> $compressed bytes ($(echo "$original $compressed" | awk '{printf "%.2f", $1/$2}')x)"
}

printf '1\t4294967295\t0\n' > "$WORK/single.tsv"
check "single edge" "$WORK/single.tsv"

printf '5\t5\t255\n5\t9\t1\n9\t9\t0\n' > "$WORK/loops.tsv"
check "loops" "$WORK/loops.tsv"

printf '0\t1\t1\n0\t2\t2\n0\t3\t3\n0\t4\t4\n0\t4294967295\t5\n' > "$WORK/star.tsv"
check "star" "$WORK/star.tsv"

for seed in 1 2 3; do
    python3 tools/generate.py "$WORK/rnd.tsv" 2000 6 0.05 uniform $seed > /dev/null
    check "random 2000 vertices, seed $seed" "$WORK/rnd.tsv"
done

python3 tools/generate.py "$WORK/const.tsv" 5000 6 0.0 constant 7 > /dev/null
check "constant weights" "$WORK/const.tsv"

python3 tools/generate.py "$WORK/skew.tsv" 5000 6 0.0 gauss 8 > /dev/null
check "skewed weights" "$WORK/skew.tsv"

python3 tools/generate.py "$WORK/big.tsv" 200000 10 0.001 uniform 9 > /dev/null
check "200000 vertices" "$WORK/big.tsv"

echo "all tests passed"
