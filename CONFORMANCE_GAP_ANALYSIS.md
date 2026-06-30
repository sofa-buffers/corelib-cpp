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

## Spec revision

Audited against the **updated** `CORELIB_PLAN.md` (commit `dcb85d6`), dated
**2026-06-30**. The substantive change in this revision is that **zero-length arrays
and empty sequences are now legal, fully-specified wire forms** that every conforming
encoder and decoder must produce and accept.

**What changed vs the previous revision**

- **§4.7** — `element_count` range is now `0 .. 2,147,483,647` (was `1 ..`). A
  **zero-count integer array** (unsigned or signed) is valid and is exactly
  `[ header_varint ] [ element_count_varint = 0 ]` with nothing after it.
  Absent-vs-empty is now a **code-generator** concern, not a wire-level one.
- **§4.8** — a **zero-count fixlen array** (fp32/fp64) has **no `fixlen_word` and no
  payload**: exactly `[ header_varint ] [ element_count_varint = 0 ]`.
- **§4.9** — an **empty sequence** (`sequence start` immediately followed by the
  `0x07` end) is legal and a decoder **must** accept it. It is the composite-type
  counterpart of a zero-count array.
- **Consequence for this audit.** The previous revision's assumption "arrays are never
  empty (count ≥ 1)" is withdrawn. The previous finding **N1** ("encoder allows
  count-0 arrays") is therefore *no longer a deviation* for integer arrays — it is now
  the required behaviour. The audit re-focuses on whether the **decoder accepts**
  these forms, and discovers a **new GAP** specific to **zero-count fixlen arrays**
  (the port emits *and* requires a spurious `fixlen_word`, violating §4.8 on both
  sides — see R2).

## Summary

| Status | Count |
|--------|------:|
| PASS | 9 |
| PARTIAL | 7 |
| GAP | 2 |
| **Total checklist items** | **18** |

Net change vs previous revision (PASS 11 / PARTIAL 6 / GAP 1):

- **Item 6** (arrays) downgraded **PASS → GAP**: the zero-count **fixlen** array wire
  form is wrong on both encode and decode (§4.8). The old minor note N1 (count-0
  *integer* arrays) is now compliant and is dropped.
- **Item 12** (tests) downgraded **PASS → PARTIAL**: the shared vectors still pass, but
  there is **no coverage** for the newly-legal zero-count arrays or empty sequences,
  and adding a conformant zero-count fixlen vector would currently fail.
- **Item 7** remains GAP (still no `MAX_DEPTH` enforcement), but the **empty-sequence**
  half of §4.9 is now verified **correct** on both encode and decode.

Zero-length support at a glance:

| Form | Encode | Decode | Tests |
|------|--------|--------|-------|
| Zero-count **unsigned** int array (`0b011`) | PASS (`:357`) | PASS (`:957`,`:1027`) | none |
| Zero-count **signed** int array (`0b100`) | PASS (`:357`) | PASS (`:957`,`:1027`) | none |
| Zero-count **fixlen** array (fp32/fp64, `0b101`) | **GAP** — emits stray `fixlen_word` (`:397`) | **GAP** — reads stray `fixlen_word` (`:966`,`:1034`) | none |
| **Empty sequence** (`start`+`0x07`) | PASS (`:616`,`:624`) | PASS (`:973`,`:1004`,`:1216`) | none |

## Per-checklist-item results

| # | Checklist item (§13) | Status | Evidence | Notes |
|---|----------------------|--------|----------|-------|
| 1 | All public symbols under `sofab` namespace (§6) | PASS | `namespace sofab` wraps the whole header (`include/sofab/sofab.hpp:46`, closing `:1410`); impl details nested in `sofab::detail` (`:82`). | Namespace is exactly `sofab`, not cased/abbreviated. |
| 2 | API version constant/getter returns `1` (§6) | PASS | `inline constexpr int API_VERSION = 1;` (`include/sofab/sofab.hpp:49`). | Re-exported in README "Constants". |
| 3 | Varint & zig-zag encode/decode match §4.1–4.2 | PASS | `encodeVarint` (`:209`), `getVarint` with overflow guard `shift >= 64` (`:860`), `zigzagEncode/Decode` (`:118`,`:127`). | Overlong varints rejected; verified by `test_roundtrip` malformed cases. |
| 4 | Header packing `(id<<3)\|type` + all 8 wire types (§4.3) | PASS | `putHeader` packs `(id<<3)\|type` (`:282`); `enum Wire {0..7}` all defined (`:85`) and handled in `dispatchLevel`/`skipPayload`/`measureField`. | Tag values normative and correct. |
| 5 | Fixlen word `(len<<3)\|subtype`, LE floats, UTF-8 strings w/o terminator, blobs (§4.6) | PASS | `writeFixlen` (`:314`), `writeFloatScalar` LE byte emit (`:332`), `Fix {Fp32,Fp64,String,Blob}` (`:98`); strings write `sv.size()` bytes, no NUL (`:549`). | Float read/write keep bit pattern (`std::bit_cast`); vectors incl. `±0/±inf` pass. Scalar fixlen `len==0` ok (`string_empty`→`0202`, `blob_empty`→`0203`). |
| 6 | Integer arrays + fixlen arrays w/ single shared fixlen word; no dynamic subtypes; **zero-count arrays per §4.7–4.8** | **GAP** | Int arrays + fixlen arrays with one shared word correct for non-empty (`writeIntArray :357`, `writeFloatArray :389`); span-of-string/bool rejected by `static_assert` (`:570`). **Zero-count integer arrays now conformant** (encode `:365`+empty loop; decode `:957`,`:1027`). **But zero-count FIXLEN arrays violate §4.8**: encoder writes the `fixlen_word` unconditionally even when `count==0` (`:397`), and the decoder *requires* it (`measureField :966-967`; `dispatchLevel :1034-1035`). | The encoder/decoder are mutually consistent (internal round-trip passes), but the wire form `[header][count=0][fixlen_word]` is **not** the spec's `[header][count=0]`. A conformant zero-count fixlen array from another port is **mis-decoded** here (the next field's bytes are consumed as the `fixlen_word`); this port's empty fixlen array is **rejected** by a conformant decoder. Cross-port interop break. See R2. |
| 7 | Sequence framing, fresh scope, single-byte `0x07` end, **empty sequence accepted**, skip-by-walking w/ depth tracking, **reject nesting beyond `MAX_DEPTH`=255** (§4.9) | **GAP** | Framing/`0x07` end correct (`sequenceEnd`→`putHeader(0,SequenceEnd)`, `:624`); skip-by-walking present (`measureField :940`, `skipPayload`→`dispatchLevel :1082`). **Empty sequence now verified**: encode = `sequenceBegin(id).sequenceEnd()`; decode handled by the immediate-end peek (`measureField :973-986`) and the `stopAtEnd` loop (`dispatchLevel :1004`, `read(InputMessage&) :1216`). **No `MAX_DEPTH` constant and no depth limit anywhere** (`grep` finds none in `include/`). | Empty-sequence half of §4.9 is compliant. The GAP is the still-missing depth bound: decode recursion (`measureField`, `dispatchLevel`) is unbounded → a deeply nested message causes native stack overflow instead of `InvalidMessage`. See R1. |
| 8 | Streaming encode into smaller-than-message buffer via flush callback + mid-stream buffer swap (§5.1) | PASS | `pushByte` flushes on full (`:227`), `flush()` (`:503`), `OStream::setBuffer` swap (`:685`), offset support (`initBuffer` `:196`). | Verified by `test_vectors` chunked-encode with 1/3/7-byte buffers. |
| 9 | Streaming decode via `feed` of arbitrarily small chunks, push/pull-read, lazy binding, auto-skip (§5.2) | PARTIAL | `feed` accepts any chunk size (`:1109`); pull-`read` (`:1175`), lazy bind via `consumed_`/auto-skip (`:1045`). One-byte feed verified. **But** `parseTopLevel`→`measureField` requires a *whole* top-level field (incl. an entire nested sequence) buffered in `acc_` before it dispatches (`:1144`,`:940`). | The "never hold the whole message in memory" / "resume the state machine at any byte boundary" guarantee is only met for splitting a field's *bytes*, not for streaming a large field/sequence field-by-field. README admits "the whole message must already be in one contiguous buffer". See R3. |
| 10 | Error reporting follows §6.3 baseline codes (return codes here) | PASS | `enum class Error { None, UsageError, BufferFull, InvalidArgument, InvalidMessage }` (`:65`); no exceptions (correct for C++/`-fno-exceptions` targets). | Minor: `UsageError` is defined but never returned by the library; read-type mismatches are not detected (caller-trusted). See note N2. |
| 11 | Streaming primitives sufficient for a thin, dead-simple generated-object layer that also (de)serializes in chunks (§6.1) | PARTIAL | Hooks present: `OStreamMessage::serialize` (`:747`), `IStreamMessage::deserialize` (`:1373`), `OStreamObject` (`operator->` populate + `serialize()`, `:761`), `IStreamObject` (`feed` + `operator*`, `:1386`). | Top-level streaming works; nested generated sub-objects inherit the item-9 limitation (a child object is bound only after its whole sequence is buffered), so "resume a half-built nested object across chunk boundaries" is not fully met. Resolved by R3. |
| 12 | All shared vectors pass encode+decode, plus chunked, roundtrip, malformed, skip (§7) | PARTIAL | `test/test_vectors.cpp` runs encode / chunked-encode / decode / chunked-decode / skip-ids / roundtrip; `test/test_roundtrip.cpp` adds malformed + skip + resync. Executed: 485 + 37 checks, 0 failures across all 67 vectors. | **No coverage of the newly-legal zero-length forms.** `assets/test_vectors.json` has no zero-count array vector (every `array` op carries values) and no empty-sequence vector (no `sequence_begin` immediately followed by `sequence_end`); `test_roundtrip.cpp` has no empty-span array or empty-sequence case. A conformant zero-count **fixlen** vector would currently fail (item 6 / R2). See R7. |
| 13 | `assets/` populated per §8 (branding + `test_vectors.json`) | PASS | `assets/sofabuffers_logo.png`, `assets/sofabuffers_icon.png`, `assets/test_vectors.json` present & git-tracked; logo referenced by README header. | Minor doc inconsistency: README says vectors copied "from the documentation repo"; §8 says they originate in `corelib-c-cpp`. See note N3. The shared vectors do not yet exercise zero-length forms (see item 12 / R7). |
| 14 | README follows family format with badges + required sections (§9) | PARTIAL | Header/logo/tagline/org link, "SofaBuffers C++ library" w/ Coverage+Docs badges, "Why this design" table, Usage (2 examples), API summary, Feature flags, Build & test, Benchmarks all present. | (a) "## Feature flags" exists but, contrary to §9 item 7, contains **no toggle table or minimal-build example** (states none exist). (b) The "larger than the buffer" Usage example uses an `OStream(scratch, size, 0, lambda)` ctor + `(const uint8_t*,size_t)` callback that **do not exist** (actual ctor takes `shared_ptr<uint8_t[]>` and a `std::span` callback) → won't compile. (c) No explicit standalone CI badge in the library section. See R5. |
| 15 | `perf` (CPU-independent) + `bench` (MB/s) tools present & runnable (§10) | PASS | `bench/perf.cpp`, `bench/bench.cpp`; CMake targets `run_perf`/`run_bench` (`CMakeLists.txt:62`), binaries built (`build/perf`, `build/bench`); README "Benchmarks" documents both. | Minor: repo has no `BENCH_SPEC.md` (referenced by §10 as the cross-language SoT); not a §13 checkbox. See note N4. |
| 16 | `.devcontainer/` complete; extensions incl. `anthropic.claude-code`; `.env` gitignored (§11) | PARTIAL | All six files present (`Dockerfile`, `build.sh`, `start.sh`, `attach.sh`, `devcontainer.json`, `.env.example`); extensions list `anthropic.claude-code` (`devcontainer.json:11`); image tagged `cpp-devcontainer` (`build.sh:6`); `.devcontainer/.env` gitignored via `.devcontainer/.gitignore` and untracked (`git ls-files` confirms). | §11.3 requires the **running container name** to also be `cpp-devcontainer`; `start.sh` uses `--name sofa-cpp-dev` and `attach.sh` execs `sofa-cpp-dev`. See R6. |
| 17 | `ci.yml` builds+tests on push & PR; matrix where it matters; coverage uploaded + badge in README (§12.1) | PARTIAL | CI on push+PR across GCC, Clang, and big-endian ppc64 (`.github/workflows/build-*.yaml`); coverage via `gcovr` published to a `badges` branch and wired into README (`coverage.yaml`). | (a) **No workflow named `ci.yml`** (§12 names it explicitly); CI is split across 4 files and uses separate workflows instead of a `fail-fast:false` matrix. (b) §12.1 step 4 ("build debug **and release**") not met — CI builds Debug only. See R4. |
| 18 | `docs.yml` builds HTML docs + publishes to Pages via Actions deploy (no `gh-pages`); Docs badge links to site (§12.2) | PARTIAL | `build-doxygen.yaml`: Doxygen, `upload-pages-artifact@v3`, `deploy-pages@v4`, `permissions: pages/id-token: write`, deploy gated to push-main; Docs badge → `https://sofa-buffers.github.io/corelib-cpp/`. No `gh-pages` branch. | (a) **No workflow named `docs.yml`** (§12 names it). (b) §12.2 says "runs on push to main only (not on pull requests)"; this workflow also triggers on `pull_request`. See R5/R4. |

---

## Remediation Plan

Ordered by severity. R1 (crash vector) and R2 (wire-format / interop break) are
normative and should be done first; R3 is the streaming-depth normative item; R4–R7
are packaging/docs/coverage conformance.

### R1 — Enforce `MAX_DEPTH` = 255 on decode (and expose the constant) — *GAP (item 7), highest severity*

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

### R2 — Make zero-count fixlen arrays omit the `fixlen_word` (encode + decode) — *GAP (item 6), high severity (interop)*

**Problem.** §4.8 (updated) requires that when `element_count == 0`, a fixlen array is
exactly `[ header_varint ] [ element_count_varint = 0 ]` — **no `fixlen_word` and no
payload follow**. The port violates this on **both** sides:
- *Encode:* `writeFloatArray` writes the `fixlen_word` unconditionally
  (`include/sofab/sofab.hpp:397`) even for an empty span, emitting
  `[header][count=0][fixlen_word]`.
- *Decode:* `measureField` (`:966-967`) and `dispatchLevel` (`:1034-1035`) read the
  `fixlen_word` unconditionally even when `count==0`, so a spec-conformant zero-count
  fixlen array (with nothing after the count) is mis-parsed — the **next field's
  bytes** are consumed as the `fixlen_word`, corrupting the stream.

Because both sides agree on the wrong form, the internal round-trip passes and the
bug is invisible to the current suite — but it is a genuine **cross-port interop
break** in both directions. (Zero-count *integer* arrays are already correct: encode
`:365` + empty loop, decode `:957`,`:1027`.)

**Fix.**
- In `writeFloatArray`, when `elems.size() == 0`, emit only the header and the
  `element_count = 0` varint; do **not** write the `fixlen_word` and do **not** copy
  any payload.
- In `measureField` and `dispatchLevel`, when the decoded `element_count == 0` for an
  `ArrayFixlen` field, do **not** read a `fixlen_word`; treat the field as complete
  after the count (set `fixLen_ = 0`, `count_ = 0`). Ensure `skipPayload` and the
  array `read` path handle `count_ == 0` (they already loop/advance zero bytes).

**Files.** `include/sofab/sofab.hpp` (`writeFloatArray`, `measureField`,
`dispatchLevel`); `test/test_roundtrip.cpp` and/or `assets/test_vectors.json` (add a
zero-count fixlen-array case — see R7).

**Acceptance criteria.**
- Encoding an empty `float`/`double` span produces exactly `[header][00]` (no
  `fixlen_word`).
- Feeding `[header][00]` for an `ArrayFixlen` field decodes to a zero-length array and
  leaves the cursor at the next field (verified by a following field round-tripping).
- Zero-count unsigned and signed integer arrays continue to round-trip.
- All existing checks still pass.

### R3 — Make streaming decode dispatch fields without buffering the whole field/sequence — *PARTIAL (items 9 & 11), high severity*

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

### R7 — Add zero-length-array & empty-sequence test coverage — *PARTIAL (items 12, 13)*

**Problem.** §4.7–4.9 now make zero-count arrays and empty sequences first-class wire
forms, but the suite exercises none of them. `assets/test_vectors.json` (67 vectors)
has no zero-count array (every `array` op carries values) and no empty sequence
(`sequence_begin` immediately followed by `sequence_end`); `test/test_roundtrip.cpp`
has no empty-span array or empty-sequence case. Without these, the §4.8 fixlen bug
(R2) is undetectable and conformance to the new rules is unverified.

**Fix.**
- Add round-trip / encode / decode cases for: a zero-count **unsigned** array, a
  zero-count **signed** array, a zero-count **fixlen** (fp32 and/or fp64) array
  encoding to exactly `[header][00]`, and an **empty sequence** encoding to
  `[header][07]`. Feed each one-byte-at-a-time too (chunked path).
- Because `test_vectors.json` is generated by and copied from `corelib-c-cpp` (§7/§8),
  request/regenerate upstream vectors for these forms once `corelib-c-cpp` adopts the
  spec change, then refresh `assets/test_vectors.json`. In the meantime add the cases
  to `test_roundtrip.cpp` so the port is covered locally.

**Files.** `test/test_roundtrip.cpp` (local cases now); `assets/test_vectors.json`
(refresh from `corelib-c-cpp` when available).

**Acceptance criteria.** Tests assert the exact bytes for a zero-count fixlen array
(`[header][00]`, no `fixlen_word`) and an empty sequence (`[header][07]`), each
round-trips and survives one-byte chunked decode, and all forms decode with the cursor
correctly positioned at the following field.

---

## Minor notes (not §13 checkboxes, worth tracking)

- **N1 (revised).** *Superseded by the spec change.* Zero-count **integer** arrays are
  now the required wire form (§4.7) and the encoder/decoder already produce/accept
  them correctly — no longer a deviation. The remaining empty-array issue is specific
  to **fixlen** arrays and is tracked as a §13 GAP (item 6 / R2), not a minor note.
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
