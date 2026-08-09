#!/usr/bin/env bash
#
# SofaBuffers pure-C++20 — guards the CI build matrix against CORELIB_PLAN §12.1.
#
# §12.1's required steps say the library is "built in both debug and release
# configurations" and the full suite run against each. That is a property of the
# workflow files, not of the library, so no amount of C++ testing can catch its
# loss: a Release-only defect (strict aliasing or UB the optimiser acts on in the
# varint window fast paths, an uninitialised field NDEBUG makes visible) ships
# green when every workflow configures Debug. This test reads
# `.github/workflows/` and requires that
#
#   1. the workflows that build and run CTest cover *both* CMAKE_BUILD_TYPE=Debug
#      and CMAKE_BUILD_TYPE=Release,
#   2. every configure step in a testing workflow names a build type (an unset
#      CMAKE_BUILD_TYPE is neither: no -O, no NDEBUG, and silently so), and
#   3. a workflow that uses a strategy matrix sets `fail-fast: false`, so one red
#      leg does not hide the others' results (§12.1, "Matrix build").
#
# `${{ matrix.build_type }}` is resolved against the workflow's own
# `build_type: [ ... ]` matrix axis, so the matrix shape §12.1 prescribes counts
# as covering every type it lists.
#
# Usage: test_ci_workflows.sh <workflows-dir>
#
# SPDX-License-Identifier: MIT
set -uo pipefail

WF_DIR="${1:?usage: test_ci_workflows.sh <workflows-dir>}"

# CTest maps 77 to "skipped" (SKIP_RETURN_CODE): a source tree without the
# workflow directory (release tarball, vendored copy) has nothing to check.
if [ ! -d "$WF_DIR" ]; then
    echo "SKIP: no workflow directory at $WF_DIR"
    exit 77
fi

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok()   { echo "  ok: $*"; }

shopt -s nullglob
workflows=("$WF_DIR"/*.yaml "$WF_DIR"/*.yml)
shopt -u nullglob

if [ ${#workflows[@]} -eq 0 ]; then
    fail "no workflow files under $WF_DIR"
    exit 1
fi

# Build types configured by workflows that also run the test suite.
tested_types=""

for wf in "${workflows[@]}"; do
    name="$(basename "$wf")"

    # Does this workflow run the suite? Only those owe §12.1 a build type.
    runs_ctest=no
    grep -qE '(^|[^[:alnum:]_./-])ctest([^[:alnum:]_-]|$)' "$wf" && runs_ctest=yes

    # The matrix axis a `${{ matrix.build_type }}` reference resolves against.
    matrix_types="$(grep -oE 'build_type:[[:space:]]*\[[^]]*\]' "$wf" \
                    | sed 's/.*\[//; s/\]//; s/[",]/ /g')"

    # `fail-fast: false` is mandatory wherever a matrix is used (§12.1).
    if grep -qE '^[[:space:]]*matrix:[[:space:]]*$' "$wf"; then
        if grep -qE '^[[:space:]]*fail-fast:[[:space:]]*false[[:space:]]*$' "$wf"; then
            ok "$name: strategy matrix sets fail-fast: false"
        else
            fail "$name: uses a strategy matrix without 'fail-fast: false' (§12.1)"
        fi
    fi

    saw_configure=no
    while IFS= read -r line; do
        saw_configure=yes
        value="${line#*-DCMAKE_BUILD_TYPE=}"
        value="${value%% \\*}"                       # drop a line continuation
        value="$(printf '%s' "$value" | sed 's/[[:space:]]*$//')"
        case "$value" in
            *matrix.build_type*)
                if [ -z "$matrix_types" ]; then
                    fail "$name: uses \${{ matrix.build_type }} with no 'build_type: [ ... ]' axis"
                else
                    ok "$name: build types from the matrix axis:$(printf ' %s' $matrix_types)"
                    [ "$runs_ctest" = yes ] && tested_types="$tested_types $matrix_types"
                fi
                ;;
            "")
                fail "$name: -DCMAKE_BUILD_TYPE= with an empty value"
                ;;
            *'${{'*)
                fail "$name: build type '$value' comes from an expression this check cannot resolve"
                ;;
            *)
                ok "$name: configures $value"
                [ "$runs_ctest" = yes ] && tested_types="$tested_types $value"
                ;;
        esac
    done < <(grep -F -- '-DCMAKE_BUILD_TYPE=' "$wf")

    # A testing workflow that configures CMake without a build type gets the
    # empty default: no optimisation, no NDEBUG, and it looks like neither leg.
    if [ "$runs_ctest" = yes ] && [ "$saw_configure" = no ]; then
        fail "$name: runs ctest but never sets -DCMAKE_BUILD_TYPE"
    fi
done

for want in Debug Release; do
    found=no
    for have in $tested_types; do
        [ "$have" = "$want" ] && found=yes
    done
    if [ "$found" = yes ]; then
        ok "some workflow builds and tests $want"
    else
        fail "no workflow builds and tests CMAKE_BUILD_TYPE=$want (CORELIB_PLAN §12.1 step 4)"
    fi
done

if [ "$fails" -ne 0 ]; then
    echo "test_ci_workflows: $fails failure(s)" >&2
    exit 1
fi
echo "test_ci_workflows: all checks passed"
