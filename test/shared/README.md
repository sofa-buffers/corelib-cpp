# `test/shared/` — vendored test harness

The files here are **not original to this repository**. They are verbatim copies
of upstream sources, vendored so the test targets build on every CI target
(including the cross-compiled big-endian one) with no third-party dependency and
no network fetch. Do not hand-edit them — re-sync from upstream instead (see
below).

## Provenance

| File | Upstream source | Pinned at | `md5` |
|------|-----------------|-----------|-------|
| `sofab_test_json.c` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `test/shared/sofab_test_json.c` | commit `e149c218cdbb` (2026-06-25) | `4dd57a285e9e3d16b3aced83800d7bff` |
| `sofab_test_json.h` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `test/shared/sofab_test_json.h` | commit `e149c218cdbb` (2026-06-25) | `dc69a2eab0135b5903f9bafa8785ebad` |

Each row pins a **merged** upstream commit — the last commit on `corelib-c-cpp`'s
`main` that touched that file — and the `md5` the copy hashed to when it was
taken. The checksum is what makes the record self-checking: `test_vendored_provenance`
re-hashes every vendored file on each `ctest` run, so a copy refreshed without
re-pinning its row turns red instead of drifting silently.

`sofab_test_json.{c,h}` is a tiny dependency-free JSON reader. Its only job is to
load the shared conformance vectors so `test/test_vectors.cpp` can replay them
through this repo's pure-C++20 `sofab::OStream` / `sofab::IStream`.

A vector may also carry `skip_ids`: the ids a receiver leaves unread, at every
nesting level, so the decoder skips the field whatever its wire type — and the
whole sub-sequence when the id names one. `test_vectors.cpp` runs that scenario
for every such vector, whole and one byte at a time (a resync bug across a chunk
boundary is invisible to a single-buffer feed), and then asserts the remaining
fields still decode to their exact values. Nothing on the load path is
fixed-size, and main() measures the largest loaded skip list, id, array and
payload against the sizes the current file needs, so a cap that truncated one
would fail loudly rather than quietly test less.

The file carries six top-level groups and this repo runs all six: `vectors`
(the wire-format ground truth), `invalid_utf8` (negative `string` payloads),
`sequence_growth` (CORELIB_PLAN §7.2 item 8 — a wrapper array's container growth,
keyed by a delivery sequence of element ids rather than by bytes, with indices
relative to the port's own configured `max_dyn_array_count`), `header_limits`
(§6.2.1/§6.3 — bytes that declare a length or count and then end, with no payload
behind them), `header_limits_nested` (the same bytes one or two sequence frames
deeper) and `boolean_tolerant` (§4.4 — see below). The growth block is gated by a
`dynamic_arrays` capability tag: a statically bounded profile never grows and
skips it. This corelib collects into `std::vector`, so it runs it.

`header_limits` is gated by `receiver_caps`, a *profile* capability: a port
declares it when its generated code carries §6.2.1 receiver caps distinct from
schema bounds. This corelib's read API is exactly that pair — `readString` takes
the declared `maxlen`, `readStringCapped` the receiver's `max_dyn_string_len`,
and neither has a default — so it declares it and runs every case. In that block
an unsatisfied `requires` tag means **skip**, for every tag, and not the
reduced-build rejection a *vector* gets: the cases assert a rejection with a
specific category, so a build that cannot represent the construct would reject
it for an unrelated reason and appear to pass while testing nothing.

`header_limits_nested` is the **depth** axis of that same block, and a separate
top-level key deliberately: its byte strings open with a sequence header, so a
runner that ignored the new `frames` key would bind its ceiling at the top level,
cap nothing, and answer `incomplete` where the case demands `limit_exceeded`. It
adds `frames` — the chain of sequence field ids the target field is nested in,
outermost first — and nothing else; the loader, the leaf read and the terminality
rule are the flat block's, shared. Depth is its own axis because a port can carry
the schema bound into a sequence and leave the receiver cap bound at the top
level.

That block runs a **second, independent pass with the ceiling lifted**, and that
pass is what makes it evidence. Its cases end at end-of-input with one or two
frames still open, which is a fully sufficient second reason to answer
`incomplete` — so a rejection that came from somewhere else entirely (a depth
guard, a refusal of unclosed frames) would pass the forward pass while never
consulting the ceiling under test. Lifting the ceiling must change the answer;
the pass asserts only that it *changed*, and asserts how many cases it checked,
because a control loop that examined nothing is green and proves nothing.

`boolean_tolerant` (CORELIB_PLAN §4.4) is the one block whose bytes no conforming
encoder produces, which is why it is hand-authored and cannot live in `vectors`:
"canonical on encode, tolerant on decode" says an encoder MUST write `true` as
`1` while a decoder MUST read **every** non-zero value as `true`. A boolean
carries no width bound at all — unlike an `enum` or a `bitfield`
(MESSAGE_SPEC §1) — so `2`, `256` and `2^64-1` at a boolean position are `true`,
never `INVALID` and never truncated to `false`. Each case is therefore run as a
decode **and** a re-encode, whole and one byte at a time, because the three
defects it exists for land in three different assertions: answering `INVALID`
for `256` fails the outcome check; masking `256` down to the destination width
before the zero-test yields `false` with a *perfect* outcome and fails only the
stored-value check; storing the raw `2` passes both the outcome and any
truthiness check and fails only the re-encode, which must emit `1`. The stored
value is compared as **bytes**, copied out of the `bool` destination: a C++
`bool` object holding `2` has no value at all, so comparing it against `true`
would be meaningless rather than revealing, and the destination is poisoned with
`0xaa` first so a decoder that never writes it cannot pass `boolean_tolerant_zero`
against zeroed storage.

In that block an unsatisfied `requires` tag means **reject**, not skip — the
opposite of `header_limits` and the same rule a *vector* gets: §4.4 lifts the
width bound the *type* carries, never the one a *build* has, so under a narrowed
accumulator (§6.2.2) a boolean carrying `2^64-1` overflows before any boolean
rule can apply and `INVALID` (§5.2.2) is the conformant answer. Skipping would
assert nothing at all, leaving the truncation the block exists to catch untested
in exactly the build most likely to have it. This full-feature C++20 build
satisfies every tag, so all eight cases run positively and the reject branch is
unreachable; it is implemented anyway so a feature-reduced profile would need no
new code.

The vectors it loads are **also vendored**, from the same upstream:

| File | Upstream source | Pinned at | `md5` |
|------|-----------------|-----------|-------|
| `../../assets/test_vectors.json` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `assets/test_vectors.json` | commit `35f2df77ac2d` (2026-09-19) | `72d6cfe07fc801fc08a39b5ba435086e` |

`test_vectors.json` is the cross-language source of truth for the wire format and
is copied verbatim into every SofaBuffers corelib. We track the copy vendored in
`corelib-c-cpp` (which itself mirrors the `documentation` repo, the ultimate
authority); if our copy and upstream ever disagree, the upstream file wins.

A vector's `serialized` column is what **this** repo asserts: the primitive-layer
bytes its op list produces. Vectors also carry `serialized_sparse` — the same
message with all-default sequence *fields* omitted (MESSAGE_SPEC §2). Producing
that form in general needs a schema and per-field defaults, which a corelib does
not have, so most of that column is consumed by the **generator's** conformance
drivers. The exception is asserted here: where a vector's sparse form differs
from its dense one *only* by an omitted empty sequence (`empty_sequence`,
`nested_empty_sequences`, `empty_sequence_between_fields`), replaying the op list
with the dropping closer reproduces it byte-for-byte with no schema at all, and
`test_vectors.cpp` checks that — plus that the dropping closer changes nothing
for every other vector.

## ⚠️ Keep in sync

These are snapshots, not forks. **They must be refreshed whenever upstream
changes** — there is no automatic update. Re-sync when either of the following
happens:

1. **The JSON reader changes upstream** (`corelib-c-cpp/test/shared/`) — re-copy
   `sofab_test_json.{c,h}` so this harness keeps parity with the rest of the
   family.
2. **The vector file changes upstream** (`corelib-c-cpp/assets/test_vectors.json`)
   — re-copy it. If the change introduces a new field-operation kind, element
   type, or JSON shape, the reader **and** `test/test_vectors.cpp` (which maps
   the JSON ops onto encode/decode calls) may also need updating, not just the
   data file. A vector may carry an optional top-level `requires` array of
   capability tags (`fixlen`, `array`, `sequence`, `fp64`, `int64`); a build
   compiled without a feature (a `SOFAB_DISABLE_*` flag) skips the vectors that
   need it. This full-feature C++20 build provides every capability, so it runs
   all vectors — the filter exists only so a feature-reduced build would skip
   what it cannot represent. See `assets/test_vectors_README.md` upstream for the
   authoritative format description.

   The **envelope** — the file's top-level keys — is upstream's too, and it has
   grown a key five times (`invalid_utf8`, then `sequence_growth`, then
   `header_limits`, then `header_limits_nested`, then `boolean_tolerant`).
   `test/test_vectors.cpp` reads and parses the file exactly once and walks every
   group off that single parse; each walker demands its own top-level key and
   fails the run when it is absent, renamed or empty. A re-sync that reshapes the
   envelope therefore shows up as a red `test_vectors` reporting `vector file has
   no non-empty "<key>" array`, not as a green run that silently tested nothing.

After any re-sync, run the suite (`ctest --test-dir build`) — a green
`test_vectors` run is what proves this implementation still matches the shared
spec.

### Re-sync commands (run from the repo root)

```sh
# shared JSON reader  <- corelib-c-cpp
curl -fsSL https://raw.githubusercontent.com/sofa-buffers/corelib-c-cpp/main/test/shared/sofab_test_json.c -o test/shared/sofab_test_json.c
curl -fsSL https://raw.githubusercontent.com/sofa-buffers/corelib-c-cpp/main/test/shared/sofab_test_json.h -o test/shared/sofab_test_json.h

# conformance vectors  <- corelib-c-cpp (mirrors documentation, the source of truth)
curl -fsSL https://raw.githubusercontent.com/sofa-buffers/corelib-c-cpp/main/assets/test_vectors.json -o assets/test_vectors.json
```

Then re-pin the table above. For each file, take the SHA of the last **merged**
commit that touched it upstream — never a branch or an unmerged PR head, which
gives a later reader nothing to diff against — and its new checksum:

```sh
# the merged SHA to pin (repeat per path)
gh api 'repos/sofa-buffers/corelib-c-cpp/commits?path=assets/test_vectors.json&sha=main&per_page=1' \
    --jq '.[0] | "\(.sha[0:12]) (\(.commit.committer.date[0:10]))"'

# the checksums to record
md5sum test/shared/sofab_test_json.c test/shared/sofab_test_json.h assets/test_vectors.json
```

Verifying an existing checkout needs neither command nor the network — the
recorded checksums are re-hashed by `ctest -R test_vendored_provenance`, which
also rejects a pin that is not a bare merged commit.
