# `test/shared/` — vendored test harness

The files here are **not original to this repository**. They are verbatim copies
of upstream sources, vendored so the test targets build on every CI target
(including the cross-compiled big-endian one) with no third-party dependency and
no network fetch. Do not hand-edit them — re-sync from upstream instead (see
below).

## Provenance

| File | Upstream source | Pinned at |
|------|-----------------|-----------|
| `sofab_test_json.c` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `test/shared/sofab_test_json.c` | commit `e149c218cdbb` (2026-06-25) |
| `sofab_test_json.h` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `test/shared/sofab_test_json.h` | commit `e149c218cdbb` (2026-06-25) |

`sofab_test_json.{c,h}` is a tiny dependency-free JSON reader. Its only job is to
load the shared conformance vectors so `test/test_vectors.cpp` can replay them
through this repo's pure-C++20 `sofab::OStream` / `sofab::IStream`.

The vectors it loads are **also vendored**, from the same upstream:

| File | Upstream source | Pinned at |
|------|-----------------|-----------|
| `../../assets/test_vectors.json` | [`sofa-buffers/corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) → `assets/test_vectors.json` | commit `c0579e940e37` (2026-06-27) |

`test_vectors.json` is the cross-language source of truth for the wire format and
is copied verbatim into every SofaBuffers corelib. We track the copy vendored in
`corelib-c-cpp` (which itself mirrors the `documentation` repo, the ultimate
authority); if our copy and upstream ever disagree, the upstream file wins.

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

Then update the "Pinned at" commits in the table above to the new upstream SHAs.
