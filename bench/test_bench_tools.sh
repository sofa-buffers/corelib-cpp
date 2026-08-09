#!/usr/bin/env bash
#
# SofaBuffers pure-C++20 — guards the benchmark tooling's single sources of truth.
#
# CORELIB_PLAN §10 requires three benchmark tools (`bench`, `perf`,
# `run_callgrind.sh`) that follow one specification, so that their numbers stay
# comparable across languages. That only holds if the three share one definition
# of the workloads, one set of stream adapters and one timing harness; a copied
# definition lets two tools drift apart silently. This test checks the sharing
# structurally (no second copy in the tree) and behaviourally (the list the
# Callgrind driver consumes really is the list `bench` dispatches on, and a
# workload the binary rejects aborts the driver with a diagnostic instead of
# printing a dash).
#
# Usage: test_bench_tools.sh <bench-binary> <bench-source-dir>
#
# SPDX-License-Identifier: MIT
set -uo pipefail

BIN="${1:?usage: test_bench_tools.sh <bench-binary> <bench-source-dir>}"
SRC="${2:?usage: test_bench_tools.sh <bench-binary> <bench-source-dir>}"

# CTest maps 77 to "skipped" (SKIP_RETURN_CODE): a build that did not produce the
# bench binary (test-only target selection) has nothing to check here.
if [ ! -x "$BIN" ]; then
    echo "SKIP: bench binary not built at $BIN"
    exit 77
fi

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok()   { echo "  ok: $*"; }

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
for w in "${names[@]}"; do
    if ! "$BIN" "$w" >"$TMP/$w.out" 2>"$TMP/$w.err"; then
        fail "single-shot run of published workload '$w' exited non-zero"
        continue
    fi
    grep -qE 'BYTES=[0-9]+' "$TMP/$w.err" || fail "workload '$w' printed no BYTES= line"
done
ok "every published workload runs single-shot and reports BYTES="

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
    for sym in 'class OStreamRaw' 'class IStreamRaw' 'double cpu_now' 'kBlockSeconds ='; do
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

if [ "$fails" -ne 0 ]; then
    echo "$fails failure(s)" >&2
    exit 1
fi
echo "bench tooling: all checks passed"
