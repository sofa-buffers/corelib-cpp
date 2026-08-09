#!/usr/bin/env bash
#
# SofaBuffers pure-C++20 — guards the benchmark tooling against BENCH_SPEC.
#
# CORELIB_PLAN §10 requires three benchmark tools (`bench`, `perf`,
# `run_callgrind.sh`) that follow one specification — BENCH_SPEC.md — so that
# their numbers stay comparable across languages. Two things have to hold for
# that, and neither is visible to a C++ unit test:
#
#   * **The workloads are BENCH_SPEC's**, all of them, with the cross-port parity
#     sizes it fixes (the `blob 1MB` message is 1,000,005 bytes, `composite` is
#     956). A port that quietly ships a subset still prints a table that parses,
#     and the missing rows only show up as a gap in the comparison.
#   * **The output matches BENCH_SPEC's grammar**, since a row the central
#     harness cannot parse is a row that does not exist. The regexes below are
#     the ones from BENCH_SPEC "Output grammar", not a paraphrase.
#
# On top of that the three tools must share one definition of the workloads, the
# stream adapters and the timing harness; a copied definition lets two tools
# drift apart silently. That part is checked structurally (no second copy in the
# tree) and behaviourally (the list the Callgrind driver consumes really is the
# list `bench` dispatches on, and a workload the binary rejects aborts the driver
# with a diagnostic instead of printing a dash).
#
# Usage: test_bench_tools.sh <bench-binary> <bench-source-dir> [<perf-binary>]
#
# SPDX-License-Identifier: MIT
set -uo pipefail

BIN="${1:?usage: test_bench_tools.sh <bench-binary> <bench-source-dir> [<perf-binary>]}"
SRC="${2:?usage: test_bench_tools.sh <bench-binary> <bench-source-dir> [<perf-binary>]}"
PERF="${3:-}"

# CTest maps 77 to "skipped" (SKIP_RETURN_CODE): a build that did not produce the
# bench binary (test-only target selection) has nothing to check here.
if [ ! -x "$BIN" ]; then
    echo "SKIP: bench binary not built at $BIN"
    exit 77
fi

# The measured loop is ~1 s of CPU time per workload by design (BENCH_SPEC
# "Timing"), which is a minute of CI for a grammar check. The tools honour
# SOFAB_BENCH_SECONDS so this test can drive the *same* code path over a token
# loop: what is being checked here is the shape of the table, never the numbers.
export SOFAB_BENCH_SECONDS=0.001

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok()   { echo "  ok: $*"; }
# A group of checks reports one line: "ok" only when nothing in it failed, so a
# passing summary can never sit under the failures it is summarising.
mark() { before="$fails"; }
done_ok() { [ "$fails" = "$before" ] && ok "$*"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- 1. `bench --list` is the published workload table ---------------------

if ! "$BIN" --list >"$TMP/list.txt" 2>"$TMP/list.err"; then
    fail "'$BIN --list' exited non-zero: $(cat "$TMP/list.err")"
    echo "$fails failure(s)" >&2
    exit 1
fi

names=(); labels=()
while IFS=$'\t' read -r name label; do
    [ -n "${name:-}" ] || continue
    names+=("$name"); labels+=("${label:-}")
done <"$TMP/list.txt"

if [ "${#names[@]}" -eq 0 ]; then
    fail "'bench --list' printed no workloads"
    echo "$fails failure(s)" >&2
    exit 1
fi
ok "bench --list published ${#names[@]} workloads"

for i in "${!names[@]}"; do
    case "${names[$i]}" in
        *[!a-z0-9_]*|"") fail "workload name '${names[$i]}' is not a Callgrind-safe symbol suffix";;
    esac
    [ -n "${labels[$i]}" ] || fail "workload '${names[$i]}' has no row label"
done

# Every published name must actually run, and report its message size (the
# Callgrind driver reads BYTES= out of the single-shot run's stderr).
declare -A bytes_of=()
for w in "${names[@]}"; do
    if ! "$BIN" "$w" >"$TMP/$w.out" 2>"$TMP/$w.err"; then
        fail "single-shot run of published workload '$w' exited non-zero"
        continue
    fi
    b="$(grep -ohE 'BYTES=[0-9]+' "$TMP/$w.err" | head -1 | cut -d= -f2)"
    [ -n "$b" ] || fail "workload '$w' printed no BYTES= line"
    bytes_of["$w"]="$b"
done
ok "every published workload runs single-shot and reports BYTES="

# --- 1b. the published set is BENCH_SPEC's, and only BENCH_SPEC's ----------
#
# BENCH_SPEC fixes four datasets and eleven rows, of which exactly one —
# `encode: blob 1MB passthrough` — is optional (a port that does not implement
# CORELIB_PLAN §5.1 pass-through omits it rather than printing a placeholder).
# Anything outside that set would land in the cross-language comparison tables as
# a row no other port has.

required_labels=(
    "encode: u64 array (1000)"
    "encode: typical message"
    "encode: blob 1MB one-shot"
    "encode: blob 1MB streaming"
    "encode: composite"
    "decode: u64 array (1000)"
    "decode: typical message"
    "decode: blob 1MB"
    "decode: composite"
    "decode: composite skip-all"
)

# BENCH_SPEC "Output grammar", verbatim: the label alternatives the central
# harness matches. A label outside it is a row that will not be parsed.
SPEC_LABEL='(encode|decode): (u64 array \(1000\)|typical message|blob 1MB one-shot|blob 1MB streaming|blob 1MB passthrough|blob 1MB|composite skip-all|composite)'

mark
for want in "${required_labels[@]}"; do
    found=0
    for have in "${labels[@]}"; do [ "$have" = "$want" ] && found=1; done
    [ "$found" = 1 ] || fail "BENCH_SPEC row '$want' is not published by 'bench --list'"
done
for have in "${labels[@]}"; do
    grep -qE "^$SPEC_LABEL$" <<<"$have" \
        || fail "row label '$have' is not one BENCH_SPEC's output grammar admits"
done
done_ok "every BENCH_SPEC row is published, and no row outside its grammar is"

# --- 1c. the cross-port parity sizes ---------------------------------------
#
# BENCH_SPEC states the encoded size of two of its messages outright and calls
# each "a parity check": a port whose figure differs has diverged on the wire,
# and its whole column is then measuring something else. `blob 1MB` is a 1-byte
# header plus a 4-byte fixlen word plus 1,000,000 payload bytes; `composite`'s
# 956 comes from the reference implementation (corelib-rs).

parity_size() { # $1 workload substring, $2 expected size
    for w in "${names[@]}"; do
        case "$w" in
            *"$1"*)
                [ "${bytes_of[$w]:-}" = "$2" ] \
                    || fail "workload '$w' reports ${bytes_of[$w]:-<none>} bytes, BENCH_SPEC's parity size is $2";;
        esac
    done
}
mark
parity_size blob 1000005
parity_size composite 956
done_ok "the blob 1MB (1000005) and composite (956) parity sizes hold"

# An unpublished name must be rejected, not silently measured as something else.
if "$BIN" no_such_workload >/dev/null 2>"$TMP/bogus.err"; then
    fail "an unknown workload name was accepted"
else
    grep -q 'unknown workload' "$TMP/bogus.err" || fail "unknown workload rejected without a diagnostic"
fi
ok "an unknown workload name is rejected with a diagnostic"

# --- 2. no second copy of the workload list --------------------------------

for f in run_callgrind.sh CMakeLists.txt; do
    for i in "${!names[@]}"; do
        if grep -qF -- "${names[$i]}" "$SRC/$f"; then
            fail "$f hardcodes the workload name '${names[$i]}' — it must come from 'bench --list'"
        fi
        if grep -qF -- "${labels[$i]}" "$SRC/$f"; then
            fail "$f hardcodes the row label '${labels[$i]}' — it must come from 'bench --list'"
        fi
    done
done
ok "run_callgrind.sh and CMakeLists.txt hold no copy of the workload list"

# --- 3. no second copy of the adapters and the timing harness --------------

for f in bench.cpp perf.cpp; do
    grep -q '#include "bench_common.hpp"' "$SRC/$f" \
        || fail "$f does not include the shared bench_common.hpp"
    for sym in 'class OStreamRaw' 'class IStreamRaw' 'double cpu_now' \
               'double loopSeconds' 'double blockSeconds'; do
        grep -q "$sym" "$SRC/$f" \
            && fail "$f defines its own '$sym' — that belongs in bench_common.hpp"
    done
done
ok "bench.cpp and perf.cpp share the adapters and the timing harness"

# --- 4. the Callgrind driver fails loudly on a rejected run ----------------
#
# Driven with a stub `valgrind` so this needs no real Valgrind (and takes
# milliseconds). The stub is passed the same argv the driver builds, so it also
# proves the driver still invokes the binary with the published names.

mkdir -p "$TMP/stub" "$TMP/fakebuild/bench"
ln -sf "$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" "$TMP/fakebuild/bench/bench"

cat >"$TMP/stub/valgrind" <<'STUB'
#!/usr/bin/env bash
out=""; argv=()
for a in "$@"; do
    case "$a" in
        --callgrind-out-file=*) out="${a#*=}";;
        --*) ;;
        *) argv+=("$a");;
    esac
done
[ -n "$out" ] && printf 'summary: 4242\n' >"$out"
exec "${argv[@]}"
STUB
chmod +x "$TMP/stub/valgrind"

if PATH="$TMP/stub:$PATH" BUILD="$TMP/fakebuild" bash "$SRC/run_callgrind.sh" \
        >"$TMP/cg.out" 2>"$TMP/cg.err"; then
    for i in "${!names[@]}"; do
        grep -qF -- "${labels[$i]}" "$TMP/cg.out" \
            || fail "run_callgrind.sh printed no row for '${names[$i]}'"
    done
    grep -q '4242' "$TMP/cg.out" || fail "run_callgrind.sh printed no instruction count"
    ok "run_callgrind.sh prints one row per published workload"
else
    fail "run_callgrind.sh failed on a successful run: $(cat "$TMP/cg.err")"
fi

cat >"$TMP/stub/valgrind" <<'STUB'
#!/usr/bin/env bash
echo "stub-valgrind: simulated rejected run" >&2
exit 1
STUB
chmod +x "$TMP/stub/valgrind"

if PATH="$TMP/stub:$PATH" BUILD="$TMP/fakebuild" bash "$SRC/run_callgrind.sh" \
        >"$TMP/cgf.out" 2>"$TMP/cgf.err"; then
    fail "run_callgrind.sh exited 0 although the measured run failed"
else
    grep -q 'simulated rejected run' "$TMP/cgf.err" \
        || fail "run_callgrind.sh swallowed the failing run's diagnostic"
    grep -qE '^-+ +-+' "$TMP/cgf.out" >/dev/null 2>&1 # header only, no data rows
    ok "run_callgrind.sh aborts with the captured diagnostic when a run fails"
fi

# --- 5. the throughput table matches BENCH_SPEC's output grammar -----------
#
# A row the central harness cannot parse is a row that does not exist, so this
# runs the real table (over a token loop, see SOFAB_BENCH_SECONDS above) and
# holds it against the spec's regexes rather than against a description of them.

if "$BIN" >"$TMP/bench.out" 2>"$TMP/bench.err"; then
    mark
    grep -qE '^=== SofaBuffers .+ throughput \(CPU time, MB/s\) ===$' "$TMP/bench.out" \
        || fail "the throughput table has no '=== SofaBuffers <Label> throughput …' marker"
    grep -qE '^Workload +MB/s$' "$TMP/bench.out" || fail "the throughput table has no column header"
    grep -qE '^MB = 1e6 bytes\. ~1s CPU-time loop per workload\.$' "$TMP/bench.out" \
        || fail "the throughput table has no 'MB = 1e6 bytes' footer"

    rows=0
    while IFS= read -r line; do
        case "$line" in
            encode:*|decode:*)
                rows=$((rows + 1))
                grep -qE "^$SPEC_LABEL +[0-9]+\.[0-9]{2}$" <<<"$line" \
                    || fail "table row does not match BENCH_SPEC's row grammar: '$line'";;
        esac
    done <"$TMP/bench.out"
    [ "$rows" = "${#names[@]}" ] \
        || fail "the table printed $rows rows for ${#names[@]} published workloads"
    done_ok "the throughput table matches BENCH_SPEC's output grammar ($rows rows)"
else
    fail "the bench binary failed to print its table: $(cat "$TMP/bench.err")"
fi

# --- 6. `perf` is present, runs, and matches its own grammar ---------------
#
# "All three tools are required: an implementation that ships only two is
# incomplete." The harness keys on the `perf: serialize` / `perf: deserialize`
# markers and the two value lines, and the 170-byte message size is the `perf`
# dataset's own cross-port parity check.

if [ -z "$PERF" ]; then
    echo "  --: perf binary not passed; skipping the per-op grammar check"
elif [ ! -x "$PERF" ]; then
    fail "perf binary not built at $PERF (BENCH_SPEC requires all three tools)"
elif "$PERF" >"$TMP/perf.out" 2>"$TMP/perf.err"; then
    mark
    grep -qE '^=== SofaBuffers .+ per-op cost \(cycles/op \+ throughput MB/s\) ===$' "$TMP/perf.out" \
        || fail "perf has no '=== SofaBuffers <Label> per-op cost …' marker"
    for marker in 'perf: serialize' 'perf: deserialize'; do
        grep -qF -- "$marker" "$TMP/perf.out" || fail "perf prints no '$marker' section"
    done
    grep -qE '^  message size  : 170 bytes$' "$TMP/perf.out" \
        || fail "the perf message is not BENCH_SPEC's 170 bytes — the encoding has diverged"
    grep -qE '^  cycles/op     : ([0-9.]+  \(hardware cycle counter\)|\(cycle counter unavailable)' "$TMP/perf.out" \
        || fail "perf prints no 'cycles/op' value (nor the documented unavailable line)"
    grep -qE '^  CPU time/op   : [0-9.]+ ns' "$TMP/perf.out" || fail "perf prints no 'CPU time/op : <n> ns' line"
    grep -qE '^  throughput    : [0-9.]+ MB/s' "$TMP/perf.out" || fail "perf prints no throughput line"
    grep -qF 'cycles/op tracks code cost;' "$TMP/perf.out" \
        || fail "perf omits the trailing 'cycles/op tracks code cost; …' line BENCH_SPEC keeps on every port"
    done_ok "perf matches BENCH_SPEC's per-op grammar"
else
    fail "the perf binary failed to print its report: $(cat "$TMP/perf.err")"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails failure(s)" >&2
    exit 1
fi
echo "bench tooling: all checks passed"
