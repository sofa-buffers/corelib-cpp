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

The vectors it loads are **also vendored**, from the same upstream:

| File | Upstream source | Pinned at | `md5` |
|------|-----------------|-----------|-------|
| `../../assets/test_vectors.json` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `assets/test_vectors.json` | commit `f30244e3f29e` (2026-07-28) | `f542c66bfa6aecb3636282ec6e802a1b` |

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
   grown a key before (`invalid_utf8`). `test/test_vectors.cpp` reads and parses
   the file exactly once and walks every group off that single parse; each walker
   demands its own top-level key and fails the run when it is absent, renamed or
   empty. A re-sync that reshapes the envelope therefore shows up as a red
   `test_vectors` reporting `vector file has no non-empty "<key>" array`, not as
   a green run that silently tested nothing.

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
