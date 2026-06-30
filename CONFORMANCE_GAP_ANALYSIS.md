# SofaBuffers `corelib-cpp` — Conformance Gap Analysis & Remediation Plan

Audit of the standalone C++20 port (`corelib-cpp`) against the language-independent
specification (`CORELIB_PLAN.md`), with particular focus on the **§13 Conformance
Checklist**. Each item was verified by reading the source (not inferred from names);
the shared vectors and round-trip suites were executed to confirm runtime behaviour.

- Implementation under audit: `include/sofab/sofab.hpp` (header-only, 1414 lines).
- Tests executed during the audit:
  - `build/test_roundtrip` → `37 checks, 0 failures`.
  - `build/test_vectors` (against `assets/test_vectors.json`, 67 vectors) → `67 vectors, 67 run, 0 skipped, 485 checks, 0 failures`.

> Scope note: §2 of the spec folds C++ under `corelib-c-cpp`, but this is a separate,
> from-scratch repo and is audited on its own merits.

## Summary

| Status | Count |
|--------|------:|
| PASS | 11 |
| PARTIAL | 6 |
| GAP | 1 |
| **Total checklist items** | **18** |

The wire format, varint/zigzag, all 8 wire types, fixlen handling, arrays, encoder
streaming, the shared-vector suite, assets, and benchmark tools are all solid and
verified. The findings concentrate in three areas: (1) **no `MAX_DEPTH` enforcement**
(the one hard GAP — a decode-side crash vector), (2) the **decoder buffers each whole
top-level field/sequence before dispatch**, weakening the core "never hold the whole
message" streaming guarantee, and (3) several **packaging deviations** (workflow file
names, CI release build, devcontainer container name, README feature-flags section).

## Per-checklist-item results

| # | Checklist item (§13) | Status | Evidence | Notes |
|---|----------------------|--------|----------|-------|
| 1 | All public symbols under `sofab` namespace (§6) | PASS | `namespace sofab` wraps the whole header (`include/sofab/sofab.hpp:46`, closing `:1410`); impl details nested in `sofab::detail` (`:82`). | Namespace is exactly `sofab`, not cased/abbreviated. |
| 2 | API version constant/getter returns `1` (§6) | PASS | `inline constexpr int API_VERSION = 1;` (`include/sofab/sofab.hpp:49`). | Re-exported in README "Constants". |
| 3 | Varint & zig-zag encode/decode match §4.1–4.2 | PASS | `encodeVarint` (`:209`), `getVarint` with overflow guard `shift >= 64` (`:860`), `zigzagEncode/Decode` (`:118`,`:127`). | Overlong varints rejected; verified by `test_roundtrip` malformed cases. |
| 4 | Header packing `(id<<3)\|type` + all 8 wire types (§4.3) | PASS | `putHeader` packs `(id<<3)\|type` (`:282`); `enum Wire {0..7}` all defined (`:85`) and handled in `dispatchLevel`/`skipPayload`/`measureField`. | Tag values normative and correct. |
| 5 | Fixlen word `(len<<3)\|subtype`, LE floats, UTF-8 strings w/o terminator, blobs (§4.6) | PASS | `writeFixlen` (`:314`), `writeFloatScalar` LE byte emit (`:332`), `Fix {Fp32,Fp64,String,Blob}` (`:98`); strings write `sv.size()` bytes, no NUL (`:549`). | Float read/write keep bit pattern (`std::bit_cast`); vectors incl. `±0/±inf` pass. |
| 6 | Integer arrays + fixlen arrays w/ single shared fixlen word; no dynamic subtypes in fixlen arrays (§4.7–4.8) | PASS | `writeIntArray` (`:357`), `writeFloatArray` single fixlen word (`:389`); span-of-string/bool/etc. rejected by `static_assert` (`:570`). | Minor: encoder does not enforce `element_count >= 1` (§4.7 "never empty on wire"); an empty span emits a count-0 array. See note N1. |
| 7 | Sequence framing, fresh scope, single-byte `0x07` end, skip-by-walking w/ depth tracking, **reject nesting beyond `MAX_DEPTH`=255** (§4.9) | **GAP** | Framing/`0x07` end correct (`sequenceEnd` → `putHeader(0,SequenceEnd)`, `:624`); skip-by-walking present (`measureField` `:940`, `skipPayload`→`dispatchLevel` `:1082`). **No `MAX_DEPTH` constant and no depth limit anywhere** (`grep` finds none in `include/`). | Decode recursion (`measureField`, `dispatchLevel`) is unbounded → a deeply nested message causes native stack overflow instead of `InvalidMessage`. See R1. |
| 8 | Streaming encode into smaller-than-message buffer via flush callback + mid-stream buffer swap (§5.1) | PASS | `pushByte` flushes on full (`:227`), `flush()` (`:503`), `OStream::setBuffer` swap (`:685`), offset support (`initBuffer` `:196`). | Verified by `test_vectors` chunked-encode with 1/3/7-byte buffers. |
| 9 | Streaming decode via `feed` of arbitrarily small chunks, push/pull-read, lazy binding, auto-skip (§5.2) | PARTIAL | `feed` accepts any chunk size (`:1109`); pull-`read` (`:1175`), lazy bind via `consumed_`/auto-skip (`:1045`). One-byte feed verified. **But** `parseTopLevel`→`measureField` requires a *whole* top-level field (incl. an entire nested sequence) buffered in `acc_` before it dispatches (`:1144`,`:940`). | The "never hold the whole message in memory" / "resume the state machine at any byte boundary" guarantee is only met for splitting a field's *bytes*, not for streaming a large field/sequence field-by-field. README admits "the whole message must already be in one contiguous buffer". See R2. |
| 10 | Error reporting follows §6.3 baseline codes (return codes here) | PASS | `enum class Error { None, UsageError, BufferFull, InvalidArgument, InvalidMessage }` (`:65`); no exceptions (correct for C++/`-fno-exceptions` targets). | Minor: `UsageError` is defined but never returned by the library; read-type mismatches are not detected (caller-trusted). See note N2. |
| 11 | Streaming primitives sufficient for a thin, dead-simple generated-object layer that also (de)serializes in chunks (§6.1) | PARTIAL | Hooks present: `OStreamMessage::serialize` (`:747`), `IStreamMessage::deserialize` (`:1373`), `OStreamObject` (`operator->` populate + `serialize()`, `:761`), `IStreamObject` (`feed` + `operator*`, `:1386`). | Top-level streaming works; nested generated sub-objects inherit the item-9 limitation (a child object is bound only after its whole sequence is buffered), so "resume a half-built nested object across chunk boundaries" is not fully met. Resolved by R2. |
| 12 | All shared vectors pass encode+decode, plus chunked, roundtrip, malformed, skip (§7) | PASS | `test/test_vectors.cpp` runs encode / chunked-encode / decode / chunked-decode / skip-ids / roundtrip; `test/test_roundtrip.cpp` adds malformed + skip + resync. Executed: 485 + 37 checks, 0 failures. | Vectors: 67, covering all ops, all array elem types, id boundary `2147483647`, 8 skip vectors. |
| 13 | `assets/` populated per §8 (branding + `test_vectors.json`) | PASS | `assets/sofabuffers_logo.png`, `assets/sofabuffers_icon.png`, `assets/test_vectors.json` present & git-tracked; logo referenced by README header. | Minor doc inconsistency: README says vectors copied "from the documentation repo"; §8 says they originate in `corelib-c-cpp`. See note N3. |
| 14 | README follows family format with badges + required sections (§9) | PARTIAL | Header/logo/tagline/org link, "SofaBuffers C++ library" w/ Coverage+Docs badges, "Why this design" table, Usage (2 examples), API summary, Feature flags, Build & test, Benchmarks all present. | (a) "## Feature flags" exists but, contrary to §9 item 7, contains **no toggle table or minimal-build example** (states none exist). (b) The "larger than the buffer" Usage example uses an `OStream(scratch, size, 0, lambda)` ctor + `(const uint8_t*,size_t)` callback that **do not exist** (actual ctor takes `shared_ptr<uint8_t[]>` and a `std::span` callback) → won't compile. (c) No explicit standalone CI badge in the library section. See R5. |
| 15 | `perf` (CPU-independent) + `bench` (MB/s) tools present & runnable (§10) | PASS | `bench/perf.cpp`, `bench/bench.cpp`; CMake targets `run_perf`/`run_bench` (`CMakeLists.txt:62`), binaries built (`build/perf`, `build/bench`); README "Benchmarks" documents both. | Minor: repo has no `BENCH_SPEC.md` (referenced by §10 as the cross-language SoT); not a §13 checkbox. See note N4. |
| 16 | `.devcontainer/` complete; extensions incl. `anthropic.claude-code`; `.env` gitignored (§11) | PARTIAL | All six files present (`Dockerfile`, `build.sh`, `start.sh`, `attach.sh`, `devcontainer.json`, `.env.example`); extensions list `anthropic.claude-code` (`devcontainer.json:11`); image tagged `cpp-devcontainer` (`build.sh:6`); `.devcontainer/.env` gitignored via `.devcontainer/.gitignore` and untracked (`git ls-files` confirms). | §11.3 requires the **running container name** to also be `cpp-devcontainer`; `start.sh` uses `--name sofa-cpp-dev` and `attach.sh` execs `sofa-cpp-dev`. See R6. |
| 17 | `ci.yml` builds+tests on push & PR; matrix where it matters; coverage uploaded + badge in README (§12.1) | PARTIAL | CI on push+PR across GCC, Clang, and big-endian ppc64 (`.github/workflows/build-*.yaml`); coverage via `gcovr` published to a `badges` branch and wired into README (`coverage.yaml`). | (a) **No workflow named `ci.yml`** (§12 names it explicitly); CI is split across 4 files and uses separate workflows instead of a `fail-fast:false` matrix. (b) §12.1 step 4 ("build debug **and release**") not met — CI builds Debug only. See R4. |
| 18 | `docs.yml` builds HTML docs + publishes to Pages via Actions deploy (no `gh-pages`); Docs badge links to site (§12.2) | PARTIAL | `build-doxygen.yaml`: Doxygen, `upload-pages-artifact@v3`, `deploy-pages@v4`, `permissions: pages/id-token: write`, deploy gated to push-main; Docs badge → `https://sofa-buffers.github.io/corelib-cpp/`. No `gh-pages` branch. | (a) **No workflow named `docs.yml`** (§12 names it). (b) §12.2 says "runs on push to main only (not on pull requests)"; this workflow also triggers on `pull_request`. See R5/R4. |

---

## Remediation Plan

Ordered by severity. Items R1 and R2 are behavioural/normative and should be done
first; R3–R6 are packaging/docs conformance.

### R1 — Enforce `MAX_DEPTH` = 255 on decode (and expose the constant) — *GAP, highest severity*

**Problem.** The spec (§4.9, §6.2) mandates `MAX_DEPTH = 255`: an encoder must not
open more than 255 nested sequences and a decoder **must reject** deeper nesting with
`InvalidMessage` "rather than risk unbounded recursion / stack growth". The C++ port
defines no `MAX_DEPTH` constant and tracks no depth. The decode walkers `measureField`
(`include/sofab/sofab.hpp:940`) and `dispatchLevel`/`skipPayload`
(`:1004`,`:1059`,`:1082`) recurse once per nesting level with no bound, so a crafted
message with thousands of nested `sequence_start` markers overflows the native stack —
a denial-of-service / crash vector, exactly what the spec forbids.

**Fix.**
- Add a public normative constant, e.g. `inline constexpr int MAX_DEPTH = 255;` in
  `namespace sofab` (near `API_VERSION`).
- Thread a `depth` counter through `measureField`, `dispatchLevel`, and the
  nested-sequence path of `skipPayload`/`read(InputMessage&)`; when a `SequenceStart`
  would push depth past 255, set the error flag so `feed()` returns
  `Error::InvalidMessage`.
- (Optional, symmetry) Have the encoder track open sequences and return
  `InvalidArgument`/`UsageError` if `sequenceBegin` exceeds `MAX_DEPTH`.

**Files.** `include/sofab/sofab.hpp` (constant + depth threading);
`test/test_roundtrip.cpp` (add a malformed-depth case).

**Acceptance criteria.**
- `sofab::MAX_DEPTH == 255` is publicly visible.
- A message nesting 256+ sequences decodes to `Error::InvalidMessage` with no crash
  (verify under ASan/a sanitized CI leg).
- All existing 485 + 37 checks still pass.

### R2 — Make streaming decode dispatch fields without buffering the whole field/sequence — *PARTIAL (items 9 & 11), high severity*

**Problem.** §5.2 requires that the consumer "never has to hold the whole message",
that a header is delivered "the instant its header arrives — even if the field's
payload has not been received yet", and that a decoder can descend into nested
sequences as small `feed` chunks arrive. The current decoder instead calls
`measureField` to confirm an **entire** top-level field — including a full nested
sequence — is present in `acc_` before `dispatchOne` fires (`feed` `:1109`,
`parseTopLevel` `:1144`). Therefore a top-level sequence (e.g. a whole message wrapped
in one sequence, or a large array/blob) is fully accumulated in memory before any
callback runs, and nested generated sub-objects (item 11) are only bound after their
parent sequence is fully buffered. The byte-at-a-time *test* still passes because the
final result is identical, but the memory/streaming guarantee is not actually met.

**Fix (design-level — choose one).**
- Preferred: convert the decoder to a resumable state machine that emits a field's
  header/metadata as soon as it is complete and consumes payload incrementally across
  `feed` calls, descending into nested sequences without first buffering them (the
  push/pull model the spec describes). This removes the `measureField` pre-scan.
- Lighter-weight: at minimum, dispatch `sequence_start` and stream its children as
  they arrive (track open scopes) rather than requiring the matching `sequence_end`
  to be buffered first.

**Files.** `include/sofab/sofab.hpp` (`feed`, `parseTopLevel`, `measureField`,
`dispatchLevel`, `read(InputMessage&)`); `test/test_roundtrip.cpp` /
`test/test_vectors.cpp` (add a test that feeds a large nested message one byte at a
time and asserts the accumulator never holds the whole message — e.g. bound `acc_`
growth); update README "Memory handling" wording accordingly.

**Acceptance criteria.**
- Feeding a message whose single top-level sequence exceeds any single chunk binds
  inner fields incrementally; peak buffered bytes stay bounded (independent of total
  message size) rather than growing to the full message.
- Nested `IStreamMessage` sub-objects are populated across chunk boundaries.
- All existing conformance checks still pass.

> Note: if the project intentionally keeps the contiguous-buffer fast path as its
> headline design (README states this trade-off explicitly), the minimum bar to claim
> §5.2/§6.1 conformance is the nested-streaming behaviour above; the in-place
> zero-copy fast path can remain as an optimization for the already-contiguous case.

### R3 — Add public normative limit constants (§6.2) — *secondary, supports items 2/7*

**Problem.** §6.2 lists `ID_MAX`, `FIXLEN_MAX`, `ARRAY_MAX`, `MAX_DEPTH` as normative
constants. Only an internal `detail::kIdMax` exists (`include/sofab/sofab.hpp:107`);
there is no public `ID_MAX`, `FIXLEN_MAX`, `ARRAY_MAX`, or `MAX_DEPTH`.

**Fix.** Expose `sofab::ID_MAX` (= 2147483647), `sofab::FIXLEN_MAX`,
`sofab::ARRAY_MAX`, and `sofab::MAX_DEPTH` (see R1) as public constants and reference
them where limits are checked.

**Files.** `include/sofab/sofab.hpp`; README "Constants" list.

**Acceptance criteria.** All four constants are publicly visible with the §6.2 values
and are used by the corresponding range checks.

### R4 — Provide `ci.yml` and `docs.yml`; build Release in CI; restrict docs to push — *PARTIAL (items 17 & 18)*

**Problem.** §12 requires exactly two workflow files, `ci.yml` and `docs.yml`. The repo
ships `build-gcc-x86_64.yaml`, `build-clang-x86_64.yaml`,
`build-gcc-ppc64-bigendian.yaml`, `coverage.yaml`, and `build-doxygen.yaml`. Two
substantive §12 requirements are also unmet: CI builds **Debug only** (§12.1 step 4
requires debug *and* release), and the docs workflow triggers on `pull_request`
(§12.2: push-to-main only).

**Fix.**
- Add `.github/workflows/ci.yml` consolidating build+test (GCC + Clang, ideally a
  `fail-fast:false` matrix) with both `-DCMAKE_BUILD_TYPE=Debug` and `Release` legs,
  plus the coverage step (keep big-endian as a leg/job). The existing per-compiler
  files may remain or be folded in.
- Add `.github/workflows/docs.yml` (Doxygen → Pages, Actions deploy) triggered on
  `push: branches: [main]` only.

**Files.** `.github/workflows/ci.yml` (new), `.github/workflows/docs.yml` (new);
optionally retire/rename the existing workflow files; README badges if names change.

**Acceptance criteria.** Workflows named `ci.yml` and `docs.yml` exist; CI runs on
push+PR and builds/tests Debug **and** Release; `docs.yml` runs on push-to-main only,
deploys via `upload-pages-artifact` + `deploy-pages`, and the README Docs badge
resolves to `https://sofa-buffers.github.io/corelib-cpp/`.

### R5 — Fix the README per §9 (feature-flags section + broken streaming example) — *PARTIAL (item 14)*

**Problem.** (a) §9 item 7 requires a "Feature flags / build options" table (fixlen,
array, sequence, fp64, overflow checks) with defaults and a minimal-build example; the
current section instead states there are no toggles. (b) The "Streaming a message
larger than the buffer" example calls `sofab::OStream(scratch, sizeof(scratch), 0,
lambda)` with a `(const uint8_t*, size_t)` callback — neither the ctor signature nor
the callback type exists (real ctor takes `std::shared_ptr<uint8_t[]>`; callback is
`std::function<void(std::span<const uint8_t>)>`), so the example will not compile.

**Fix.**
- Either add real feature flags (the `SOFAB_DISABLE_*` capability names already
  referenced in `test/test_vectors.cpp:53` suggest the intent) and document them in a
  table, or, if the header is deliberately always-on, document that explicitly while
  still presenting the required table shape and a minimal-build note so the section
  matches the family format.
- Rewrite the streaming example to use a real constructor (`OStream(callback, buffer,
  buflen)` with a `std::span` callback, or `OStreamInline<N>(callback)`) and verify it
  compiles.

**Files.** `README.md`.

**Acceptance criteria.** The streaming README example compiles against
`include/sofab/sofab.hpp`; the Feature flags section matches §9 item 7's required
shape.

### R6 — Name the running devcontainer `cpp-devcontainer` (§11.3) — *PARTIAL (item 16)*

**Problem.** §11.3 requires both the image tag and the **running container name** to be
`<lang>-devcontainer`. The image is `cpp-devcontainer` (correct), but `start.sh` runs
`--name sofa-cpp-dev` (`.devcontainer/start.sh:17`) and `attach.sh` execs into
`sofa-cpp-dev` (`.devcontainer/attach.sh:2`).

**Fix.** Rename the running container to `cpp-devcontainer` in `start.sh` and update
the `docker exec` target in `attach.sh` to match.

**Files.** `.devcontainer/start.sh`, `.devcontainer/attach.sh`.

**Acceptance criteria.** `start.sh` launches `--name cpp-devcontainer` and `attach.sh`
attaches to `cpp-devcontainer`.

---

## Minor notes (not §13 checkboxes, worth tracking)

- **N1.** Encoder does not enforce §4.7/§4.8 "an array is never empty on the wire"
  (`element_count >= 1`); an empty span emits a count-0 array. Generated code is
  expected to omit empty collections, but the raw API permits a non-conformant form.
- **N2.** `Error::UsageError` is defined but never returned by the library; read-side
  type/wire-type mismatches are not detected (the caller is trusted). §6.3 lists
  `UsageError` for "a type mismatch on read".
- **N3.** README states `test_vectors.json` was copied "from the documentation repo";
  §8 specifies it originates in (and is generated by) `corelib-c-cpp`.
- **N4.** No `BENCH_SPEC.md` in the repo, which §10 names as the single source of truth
  for the cross-language benchmark workloads/output grammar (the `perf`/`bench` tools
  themselves are present and runnable).
</content>
</invoke>
