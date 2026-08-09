#!/usr/bin/env bash
#
# SofaBuffers pure-C++20 — guards README.md against CORELIB_PLAN §9.
#
# §9 fixes the README's shape for the whole corelib family: "Do not change the
# section ordering and do not invent new top-level sections; that shared shape
# is the point." A reader who knows one port's README navigates any other by
# looking in the same place. Nothing inside the library can notice when that
# shape drifts — a top-level section that exists in no other port, or a missing
# badge, is invisible to every C++ test — so the check has to read the document.
#
# What it enforces:
#
#   1. §9.1 the centered header block: logo, `# SofaBuffers`, tagline, org link.
#   2. §9.2 the badge block that opens the library section carries a CI badge, a
#      coverage badge and a Docs badge, in that order, before any prose.
#   3. §9 the `## ` sections are exactly the prescribed list, in order:
#      `## SofaBuffers <Language> library`, `## Why this design`, `## Usage`,
#      `## Memory handling`, `## Build & test`, `## Benchmarks` — no extras.
#   4. §9.4 no API-documentation section at any heading level; the Docs badge is
#      the only pointer to the generated reference.
#   5. §9.8 C++ is a language with two corelibs, so the comparison between them
#      is a *subsection* of `## Benchmarks` — its final one — not a section of
#      its own after it.
#
# Usage: test_readme_structure.sh <path-to-README.md>
#
# SPDX-License-Identifier: MIT
set -uo pipefail

README="${1:?usage: test_readme_structure.sh <path-to-README.md>}"

# CTest maps 77 to "skipped" (SKIP_RETURN_CODE): a consumer tree that vendored
# only the header has no README to check.
if [ ! -f "$README" ]; then
    echo "SKIP: no README at $README"
    exit 77
fi

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok()   { echo "  ok: $*"; }

# Headings are read from a copy with fenced code blocks blanked out: a shell
# comment inside a ```sh block is not a section, and the Build & test snippets
# are full of them.
PROSE="$(mktemp)"
trap 'rm -f "$PROSE"' EXIT
awk '/^```/ { fence = !fence; print ""; next } fence { print "" ; next } { print }' \
    "$README" > "$PROSE"

# ---------------------------------------------------------------- §9.1 header

grep -q '<p align="center"><img src="assets/sofabuffers_logo.png"' "$README" \
    && ok "§9.1 centered logo" || fail "§9.1 centered logo block missing"
grep -qx '# SofaBuffers' "$README" \
    && ok "§9.1 title" || fail "§9.1 '# SofaBuffers' title missing"
grep -qF '<b>Structured Objects For Anyone</b><br>' "$README" \
    && ok "§9.1 tagline" || fail "§9.1 tagline missing"
grep -q 'https://github.com/sofa-buffers' "$README" \
    && ok "§9.1 organization link" || fail "§9.1 link back to the organization missing"

# ------------------------------------------------------- §9 top-level sections

# The section list §9 prescribes, in order. The first is the only one whose
# wording varies per port (`## SofaBuffers <Language> library`).
expected="## SofaBuffers C++ library
## Why this design
## Usage
## Memory handling
## Build & test
## Benchmarks"

actual="$(grep -E '^## ' "$PROSE")"

if [ "$actual" = "$expected" ]; then
    ok "§9 top-level sections are the prescribed list, in order"
else
    fail "§9 top-level sections differ from the prescribed list (no invented sections, no reordering)"
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") \
        | sed 's/^/      /' >&2
fi

# --------------------------------------------------------- §9.2 badge block

# Everything between the library heading and the first blank line after it: §9.2
# puts the badges first in the section, ahead of the GitHub link and the summary.
badges="$(awk '
    /^## SofaBuffers .* library$/ { inside = 1; next }
    inside && /^[[:space:]]*$/    { if (seen) exit; next }
    inside                        { seen = 1; print }
' "$PROSE")"

if [ -z "$badges" ]; then
    fail "§9.2 the library section opens with no badge block"
else
    # Badge alt texts, in document order: "[![CI](…)](…)" -> "CI".
    order="$(printf '%s\n' "$badges" | grep -oE '^\[!\[[^]]*\]' | sed 's/^\[!\[//; s/\]$//')"
    for want in CI Coverage Docs; do
        printf '%s\n' "$order" | grep -qi "^${want}\$" \
            && ok "§9.2 badge block carries a $want badge" \
            || fail "§9.2 badge block carries no $want badge (it has:$(printf ' %s' $order))"
    done
    # …and in the order §9.2 lists them: CI, coverage, Docs.
    ranked="$(printf '%s\n' "$order" | grep -iE '^(CI|Coverage|Docs)$' | tr 'A-Z' 'a-z')"
    if [ "$ranked" = "$(printf 'ci\ncoverage\ndocs')" ]; then
        ok "§9.2 badges are in the CI / coverage / Docs order"
    else
        fail "§9.2 badges out of order: expected CI, coverage, Docs — got$(printf ' %s' $ranked)"
    fi
fi

# ------------------------------------------------ §9.4 no API-doc section

if grep -qiE '^#+ +(source documentation|api reference|api documentation|api docs)[[:space:]]*$' "$PROSE"; then
    fail "§9.4 an API-documentation section exists; the Docs badge is the only pointer"
else
    ok "§9.4 no API-documentation section"
fi

# ------------------------------------- §9.8 two-corelib comparison placement

# C++ ships two corelibs for two use cases, so §9.8 requires the comparison as a
# final *subsection* of `## Benchmarks`.
compare_heading="$(grep -nE '^#+ +Choosing between the two' "$PROSE" | head -1)"
if [ -z "$compare_heading" ]; then
    fail "§9.8 no subsection comparing the two C++ corelibs"
else
    level="${compare_heading#*:}"
    level="${level%% *}"
    if [ "$level" = "###" ]; then
        ok "§9.8 the two-corelib comparison is a subsection"
    else
        fail "§9.8 the two-corelib comparison is a '$level' heading; §9.8 makes it a '###' subsection of ## Benchmarks"
    fi
    # …and it must sit under Benchmarks, which is therefore the last section.
    owner="$(awk -F: -v n="${compare_heading%%:*}" '
        NR < n && /^## / { last = $0 }
        END { print last }
    ' "$PROSE")"
    if [ "$owner" = "## Benchmarks" ]; then
        ok "§9.8 the comparison sits under ## Benchmarks"
    else
        fail "§9.8 the comparison sits under '$owner', not under ## Benchmarks"
    fi
fi

if [ "$fails" -ne 0 ]; then
    echo "test_readme_structure: $fails failure(s)" >&2
    exit 1
fi
echo "test_readme_structure: all checks passed"
