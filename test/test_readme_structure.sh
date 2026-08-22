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
# Checks 1-5 guard the document's *shape*. The rest guard its *content*, which
# is the half a shrink threatens: a section can keep its heading and lose the
# fact a reader came for, and nothing inside the library notices that either.
#
#   6. §9.5 the Usage chapter still shows each example the plan lists.
#   7. §6.4 the strict-UTF-8 knob is documented, and §9.6 states
#      MIN_OUTPUT_BUFFER *in the memory chapter* — the number a caller needs
#      before it can size a streaming buffer, in the section they read to find
#      out who allocates what.
#   8. §6.1.1 no spelling outside the closed generated-object name set.
#   9. Every in-document link still resolves to a heading.
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

# ------------------------------------------- §9.5 the Usage chapter's examples

# §9.5 lists the examples every port must carry, and they are what a reader
# opens Usage for. Dropping one drops a use case, not prose. The wording is the
# family's; only the code inside each is per-language.
for want in "Serialize" "Serialize stream" "Deserialize" "Deserialize stream" "Code generator"; do
    if grep -qxF "### $want" "$PROSE"; then
        ok "§9.5 Usage shows '$want'"
    else
        fail "§9.5 Usage has no '### $want' example"
    fi
done

# ------------------------------- §6.4 / §9.6 facts no heading check can see

# Two things the plan obliges this port to state are plain prose. A
# byte-container target must document its strict-UTF-8 knob (§6.4). And §9.6
# puts MIN_OUTPUT_BUFFER in the memory chapter specifically: it is the number a
# caller needs before it can size a streaming buffer, and the memory chapter is
# where they go to find out who allocates what, so stating it elsewhere does not
# reach them.
if grep -q 'SOFAB_STRICT_UTF8' "$README"; then
    ok "§6.4 the strict-UTF-8 knob is documented"
else
    fail "§6.4 SOFAB_STRICT_UTF8 is never documented (a byte-container port must expose it)"
fi

memory="$(awk '/^## Memory handling$/ { inside = 1; next } /^## / { inside = 0 } inside' "$PROSE")"
if printf '%s\n' "$memory" | grep -q 'MIN_OUTPUT_BUFFER'; then
    ok "§9.6 MIN_OUTPUT_BUFFER is stated in the memory chapter"
else
    fail "§9.6 the memory chapter never states MIN_OUTPUT_BUFFER"
fi

# --------------------------------------- §6.1.1 the closed generated-name set

# §6.1.1 closes the generated-object layer to encode / decode / try_decode /
# serialize / deserialize / decoder, and lists the spellings a port must not
# invent beside them. Teaching one in the docs sends a reader looking for a
# surface sofabgen does not emit — as effectively as emitting it would.
if bad="$(grep -inE '\b(marshal|unmarshal|serialize_to|to_bytes|from_bytes|decode_from|decode_into)\b' "$README")"; then
    fail "§6.1.1 a name outside the closed generated-object set:"
    printf '%s\n' "$bad" | sed 's/^/      /' >&2
else
    ok "§6.1.1 no name outside the closed generated-object set"
fi

# ---------------------------------------------- in-document links resolve

# A heading that moves takes its anchor with it. That is the cheapest way for a
# restructuring to break navigation while breaking nothing a build can see.
anchors="$(grep -E '^#+ +' "$PROSE" | sed -E 's/^#+ +//' \
    | tr 'A-Z' 'a-z' | sed -E 's/[^a-z0-9 _-]//g; s/ /-/g')"
links="$(grep -oE '\]\(#[^)]+\)' "$README" | sed -E 's/^\]\(#//; s/\)$//' | sort -u)"
if [ -z "$links" ]; then
    fail "no in-document links found; the link scan is broken"
else
    broken=0
    for link in $links; do
        if ! printf '%s\n' "$anchors" | grep -qxF "$link"; then
            fail "link to #$link matches no heading"
            broken=1
        fi
    done
    [ "$broken" -eq 0 ] && ok "every in-document link resolves to a heading"
fi

if [ "$fails" -ne 0 ]; then
    echo "test_readme_structure: $fails failure(s)" >&2
    exit 1
fi
echo "test_readme_structure: all checks passed"
