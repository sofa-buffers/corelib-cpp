#!/usr/bin/env bash
#
# SofaBuffers pure-C++20 — machine-independent instruction cost.
#
# Runs each benchmark workload once under Callgrind and reports instructions
# retired per operation (Ir/op). Unlike wall-clock or CPU time, instruction
# counts are deterministic and independent of the host's clock speed and
# scheduler, so the numbers compare across machines (and against the C/C++/
# Rust/Go/Python/TypeScript tools — the workloads, ids and values are identical).
#
# The `bench` binary exposes each workload as an `extern "C"` non-inlined
# `run_<workload>` symbol doing exactly one op (see bench.cpp); this drives it
# under `--collect-atstart=no --toggle-collect=run_<workload>`, so the reported
# Ir is one op's instruction count directly — no rep-count subtraction (native
# symbols, unlike the JIT/interpreted ports).
#
# Prereqs: valgrind, cmake, a C++20 compiler. Builds the bench binary if missing.
# Usage:   bash bench/run_callgrind.sh          (or BUILD=<dir> bash …)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# A dedicated build dir (not the top-level build/, which a dev may have
# configured for another purpose) so this is self-contained and reproducible.
BUILD="${BUILD:-$ROOT/build/callgrind}"
BIN="$BUILD/bench/bench"

if ! command -v valgrind >/dev/null 2>&1; then
    echo "error: valgrind not found (needed for instruction counts)." >&2
    echo "       install it, e.g.  apt-get install valgrind" >&2
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo ">> building bench (-O3) ..." >&2
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    # Explicit job count: bare `--parallel` means an unlimited `make -j`.
    cmake --build "$BUILD" --parallel "$(nproc)" --target bench >/dev/null
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# The workloads and their row labels come from the bench binary itself
# ("name<TAB>label" per line, straight out of the table bench.cpp dispatches on).
# CORELIB_PLAN §10 wants the three tools to follow one definition, so this script
# keeps no list of its own: a workload added, removed or renamed in bench.cpp
# shows up here without an edit, and can never silently disagree.
if ! LIST="$("$BIN" --list 2>"$OUT/list.log")"; then
    echo "error: '$BIN --list' failed — rebuild the bench binary:" >&2
    cat "$OUT/list.log" >&2
    exit 1
fi

WORKLOADS=(); LABELS=()
while IFS=$'\t' read -r name label; do
    [ -n "${name:-}" ] || continue
    WORKLOADS+=("$name"); LABELS+=("${label:-$name}")
done <<<"$LIST"

if [ "${#WORKLOADS[@]}" -eq 0 ]; then
    echo "error: '$BIN --list' published no workloads." >&2
    exit 1
fi

run_cg() { # $1 workload
    if ! valgrind --tool=callgrind --collect-atstart=no --toggle-collect="run_$1" \
        --callgrind-out-file="$OUT/$1.out" "$BIN" "$1" \
        >/dev/null 2>"$OUT/$1.log"; then
        echo "error: the Callgrind run for workload '$1' failed:" >&2
        cat "$OUT/$1.log" >&2
        exit 1
    fi
}

ir_of()    { grep -m1 '^summary:' "$OUT/$1.out" 2>/dev/null | awk '{print $2}'; }
bytes_of() { grep -ohE 'BYTES=[0-9]+' "$OUT/$1.log" 2>/dev/null | head -1 | cut -d= -f2; }

echo ">> Measuring instructions/op under Callgrind (this is slow) ..." >&2
echo
echo "==============================================================================="
echo " SofaBuffers pure-C++20 instruction cost   (Callgrind, Ir/op)"
echo " instructions/op: lower is better. Deterministic & machine-independent."
echo "==============================================================================="
printf "%-26s %16s %9s\n" "Workload" "instr/op" "bytes"
printf "%-26s %16s %9s\n" "--------" "--------" "-----"

for i in "${!WORKLOADS[@]}"; do
    w="${WORKLOADS[$i]}"
    run_cg "$w"
    # A missing figure is a failure, not a dash: a table with a "-" in it looks
    # exactly like a fresh one, so a broken measurement has to stop the run.
    ir="$(ir_of "$w" || true)"; b="$(bytes_of "$w" || true)"
    if [ -z "$ir" ] || [ -z "$b" ]; then
        echo "error: workload '$w' produced no instruction count / message size:" >&2
        cat "$OUT/$w.log" >&2
        exit 1
    fi
    printf "%-26s %16s %9s\n" "${LABELS[$i]}" "$ir" "$b"
done
echo
echo "Ir = instructions retired (Callgrind). Independent of CPU clock and OS"
echo "scheduling; depends only on the executed code, so it compares across machines."
