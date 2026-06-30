<p align="center"><img src="assets/sofabuffers_logo.png" alt="SofaBuffers" height="140"></p>

# SofaBuffers

<b>Structured Objects For Anyone</b><br>
<i>... so optimized, feels amazing.</i>

[Would you like to know more?](https://github.com/sofa-buffers)

## SofaBuffers C++ library

[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/sofa-buffers/corelib-cpp/badges/coverage-cpp.json)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/coverage.yaml)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://sofa-buffers.github.io/corelib-cpp/)

[GitHub repository](https://github.com/sofa-buffers/corelib-cpp)

A **streaming**, **dependency-free**, pure-**C++20** implementation of the
SofaBuffers (*Sofab*) serialization format — written from scratch with no C
backend. It packs structured fields into a caller-owned buffer and decodes them
through a protobuf-style cursor that advances a pointer over the message.

It is API-compatible with the C/C++ corelib's
[`sofab.hpp`](https://github.com/sofa-buffers/corelib-c-cpp) — same
`sofab::OStream` / `sofab::OStreamInline` / `sofab::IStreamObject` surface — but
shares no code with it and is tuned for raw speed. Requires **C++20** or later.

### Built with following compilers

| Target | Status |
| - | - |
| GCC x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml) |
| Clang x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml) |
| GCC ppc64 (big endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml) |

The big-endian job cross-compiles to PowerPC64 and runs the full suite under
qemu, exercising the byte-swapping float paths that little-endian hosts skip.

Header-only: add `include/` to your include path, or install via CMake:

```sh
cmake -S . -B build && cmake --install build
```

## Why this design

The C corelib is optimised for **minimal code size and RAM** (it targets bare
metal). This library makes the opposite trade: size and memory are not a concern,
the goal is **throughput**, and the decoder is tuned for the common case where the
whole message is already in contiguous memory.

| Goal | How |
|------|-----|
| Fast encode | Payloads written with a single `memcpy`; a field's header + value varints emitted as one combined write; whole float arrays copied in one shot on little-endian. |
| Fast decode | The common case parses **with zero copies and zero allocations** — the cursor walks the caller's buffer in place; float arrays bulk-`memcpy`'d; `std::string_view` reads are zero-copy views into the buffer. |
| Still streamable | `OStream` flushes a small buffer via callback; `feed()` dispatches each complete top-level field and buffers only an incomplete tail. |
| Modern C++ | `std::span`, `std::bit_cast`, concepts, `if constexpr` `write()`/`read()` deduction, `[[nodiscard]]`. Little-endian handled explicitly — no host-endian branching. |

## Usage

```cpp
#include "sofab/sofab.hpp"

// ---- encode (inline buffer, no heap) ----
sofab::OStreamInline<64> os;
os.write(1, 42u).write(2, -7).write(3, "hi");
std::span<const uint8_t> msg{os.data(), os.bytesUsed()};

// ---- decode (push to your IStreamMessage) ----
struct Sensor : sofab::IStreamMessage {
    uint32_t id = 0; float value = 0;
    void deserialize(sofab::IStreamImpl& is, sofab::id i, size_t, size_t) noexcept override {
        switch (i) { case 1: is.read(id); break; case 2: is.read(value); break; }
    }
};
sofab::IStreamObject<Sensor> in;
in.feed(msg.data(), msg.size());
// (*in).id and (*in).value now hold the decoded values
```

### Streaming a message larger than the buffer

```cpp
#include "sofab/sofab.hpp"
#include <span>
#include <vector>

std::vector<uint8_t> out;

// A 16-byte window — far smaller than the message. The flush callback drains it
// each time it fills (and once more at the end), so the encoder never needs the
// whole message in memory. The callback takes a std::span<const uint8_t>.
sofab::OStreamInline<16> os(
    [&](std::span<const uint8_t> chunk){ out.insert(out.end(), chunk.begin(), chunk.end()); });

for (uint32_t i = 0; i < 1000; i++)
    os.write(sofab::id(i), uint64_t(i));
os.flush(); // push the tail; `out` now holds the complete message
```

A heap-backed `OStream` works the same way and additionally lets you swap the
backing buffer mid-stream (typically inside the callback):
`sofab::OStream os(callback, buffer, buflen, offset = 0)` where `buffer` is a
`std::shared_ptr<uint8_t[]>` and `callback` is the same `std::span` sink.

## API summary

**Encoder**

| Class | Purpose |
|-------|---------|
| `OStreamInline<N, Offset=0>` | Stack-allocated N-byte output stream — zero heap, suitable for any target |
| `OStream(flush, buf, len, offset=0)` | Heap-buffer + flush-callback encoder (`flush` first; `buf` is a `std::shared_ptr<uint8_t[]>`); `flush` is called whenever the buffer fills, and the buffer can be swapped mid-stream via `setBuffer` |
| `OStreamInline<N>(flush)` | Stack-buffer encoder with a flush callback — small reusable window, zero heap |

Common methods: `write(id, value)` (deduces type — unsigned, signed, bool, fp32,
fp64, string, blob, array); `write(id, ptr, size)` (blob from a raw pointer);
`writeIf(id, value, cond)`; `sequenceBegin(id)` / `sequenceEnd()`; `flush()`;
`bytesUsed()`; `data()`. Every call returns a chainable `Result` that latches the
first error (`ok()`, `code()`, `operator bool`), so a whole message can be written
fluently and checked once at the end.

**Decoder**

| Class | Purpose |
|-------|---------|
| `IStreamMessage` | Abstract base — override `deserialize(is, id, size, count)` to bind fields |
| `IStreamObject<T>` | Wraps an `IStreamMessage`; call `feed(buf, len)` to decode; access via `->` / `*` |
| `IStreamInline` | Lambda-based decoder — pass a `std::function` without subclassing |

`feed(const uint8_t* buf, size_t len)` drives decoding and returns a `Result`
(`ok()` / `code()` / `operator bool`). For each top-level field it invokes the
callback / `deserialize(is, id, size, count)`, where `size` is the field's payload
length in bytes (element size for fixlen arrays) and `count` is the array element
count (`0` for scalars). Inside that callback, call `is.read(dest)` to bind the
field or **do nothing to skip it** — an unconsumed field is automatically stepped
over, so unknown / unwanted fields cost only a measure-and-advance.

### Read operations

All reads happen on the `IStreamImpl& is` passed to the callback; the type of the
destination selects the decoder via `if constexpr`. The cursor is bounds-checked —
on a malformed/truncated field the stream latches an error and `feed()` returns
`Error::InvalidMessage`.

| Call | Destination type | Result |
|------|------------------|--------|
| `is.read(x)` | `uint8_t … uint64_t` | unsigned varint decoded into `x` |
| `is.read(x)` | `int8_t … int64_t` | zig-zag varint decoded into `x` |
| `is.read(x)` | `bool` | varint `!= 0` |
| `is.read(x)` | `float` / `double` | fp32 / fp64 loaded from the little-endian payload |
| `is.read(x)` | `std::string_view` | **zero-copy** view onto the `size` payload bytes in the buffer |
| `is.read(x)` | `std::string` | owning **copy** of the `size` payload bytes |
| `is.read(x)` | `std::span<I>` / contiguous container of `I` (`I` an integer) | up to `min(dest.size(), count)` varints decoded element-wise into the caller's storage |
| `is.read(x)` | `std::span<float/double>` / container thereof | array bulk-`memcpy`'d into the caller's storage (per-element byte-swap on big-endian) |
| `is.read(dst, maxlen)` | `void* dst, size_t maxlen` | blob copied into the caller buffer; **returns** `size_t` bytes copied (`min(maxlen, size)`) |
| `is.read(nested)` | a type derived from `IStreamMessage` | descends the sub-sequence, dispatching its fields to `nested.deserialize(...)` |
| *(skip)* | — | leave the field unread; the payload is measured and skipped |

### Allowed templates

`write()` and `read()` deduce the wire type from the C++ type; only these are
accepted (anything else is a hard `static_assert`):

| Category | Types | Wire form |
|----------|-------|-----------|
| Unsigned ints | `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` | unsigned varint |
| Signed ints | `int8_t`, `int16_t`, `int32_t`, `int64_t` | zig-zag varint |
| Bool | `bool` | varint `0`/`1` |
| Floats | `float` → fp32, `double` → fp64 | fixlen little-endian |
| String | anything convertible to `std::string_view` (encode); `std::string_view` / `std::string` (decode) | fixlen string |
| Blob | `write(id, const void*, int32_t)` (encode); `read(void*, size_t)` (decode) | fixlen blob |
| Fixlen arrays | `std::span<I>` / contiguous containers of the **integer or float** scalars above | array-unsigned / array-signed / array-fixlen |
| Sequences | a type deriving `OStreamMessage` (encode) / `IStreamMessage` (decode), or explicit `sequenceBegin`/`sequenceEnd` | nested sequence |

Not allowed: arrays whose element type is itself a *dynamic* subtype — i.e. spans
of `bool`, `std::string`, blobs, or nested messages. Fixlen arrays carry a single
fixed element size on the wire, so only the scalar integer/float element types are
permitted; passing any other span element type is a compile error.

Inline encoders take their capacity as template parameters:
`OStreamInline<N, Offset = 0>` reserves an `N`-byte stack buffer and starts writing
at byte `Offset` (`Offset < N`), leaving room for a caller-prepended header.
`OStreamObject<Msg, N = Msg::_maxSize, Offset = 0>` couples that buffer with a
message instance.

### Memory handling

This is the defining trade-off of the C++ port, and it is the opposite of the C
(`corelib-c-cpp`) port.

**Decoder — zero-copy views into the caller's buffer.** `feed()` parses *in place*:
when nothing is buffered (the common case — a whole message handed in at once) the
cursor walks straight over the caller's contiguous `buf`, allocating nothing and
copying nothing. Consequently:

- `read(std::string_view&)` returns a view that **points directly into `buf`**; no
  bytes are copied. The view stays valid only as long as that source buffer stays
  alive and unmodified — outliving the buffer is a dangling-view bug. Use
  `read(std::string&)` when you need an owning copy.
- Integer arrays are decoded element-wise straight into the caller-provided
  `span`/container; float arrays are bulk-`memcpy`'d into it (one `memcpy` on
  little-endian). Either way the destination storage is the caller's — the stream
  never allocates it.
- `read(void* dst, size_t maxlen)` copies a blob into the caller's `dst`.
- Because decoding is a pointer walk over contiguous memory, **the whole message
  must already be in one contiguous buffer.** If a chunk handed to `feed()` ends
  mid-field, only that incomplete trailing field is copied into an internal
  accumulator and re-parsed on the next `feed()`; views read from such a stitched
  field point into that accumulator instead of the original chunk.

This is the inverse of the C port's deferred-copy model, where `read()` only *binds*
a destination pointer and a later `feed()` copies the bytes into it as they arrive
(so destinations must be address-stable). Here `read()` pulls the value out
immediately and strings come back as views, so there is no per-field destination to
keep stable — but the **input buffer** must stay alive for as long as any returned
view is used.

**Encoder — writes into an owned, fixed-size buffer; flushes, never grows.** An
`OStream` owns its buffer through a `std::shared_ptr<uint8_t[]>` (allocated for you,
or one you hand in / share); an `OStreamInline<N>` owns an `N`-byte `std::array` on
the stack with zero heap use. Neither buffer grows. When the cursor reaches the end
it calls the flush callback with the filled bytes and rewinds to the buffer start;
**without** a callback a full buffer yields `Error::BufferFull`. So to emit a message
larger than the buffer you supply a flush callback (see "Streaming a message larger
than the buffer" above) and the buffer acts as a small reusable window. `bytesUsed()`
/ `data()` expose the current contents, `flush()` pushes the tail, and `OStream`'s
destructor flushes automatically.

**Constants:** `sofab::API_VERSION` (`1`); the spec §6.2 limits `sofab::ID_MAX`,
`sofab::FIXLEN_MAX`, `sofab::ARRAY_MAX` (all `INT32_MAX`) and `sofab::MAX_DEPTH`
(`255`, the maximum nested-sequence depth); `sofab::id` (the `uint32_t` field-id
type); and the `sofab::Error` codes (`None`, `BufferFull`, `InvalidArgument`,
`InvalidMessage`, `UsageError`) returned via `Result`. Opening more than
`MAX_DEPTH` nested sequences fails encoding with `InvalidArgument`; a message that
nests deeper is rejected on decode with `InvalidMessage` (never an unbounded
recursion).

## Feature flags

The library is header-only C++20 and **ships with every wire feature enabled by
default** — this port optimises for throughput rather than footprint, so unlike the
C corelib (`corelib-c-cpp`) the header itself contains no `#ifdef` gating and all
features are always compiled in. The conformance harness (`test/test_vectors.cpp`),
however, recognises the family-standard `SOFAB_DISABLE_*` toggles and skips the
vectors a disabled feature would exercise, so a feature-reduced profile can be
described and tested with the same switches used across the other ports.

| Build option (define) | Default | Effect |
|-----------------------|---------|--------|
| `SOFAB_DISABLE_FIXLEN_SUPPORT`   | off (fixlen on)   | Drops fp32/fp64, string and blob fixlen values (and fixlen arrays). |
| `SOFAB_DISABLE_FP64_SUPPORT`     | off (fp64 on)     | Drops 64-bit `double` support (fp32 stays). |
| `SOFAB_DISABLE_ARRAY_SUPPORT`    | off (arrays on)   | Drops the array wire types (unsigned / signed / fixlen arrays). |
| `SOFAB_DISABLE_SEQUENCE_SUPPORT` | off (sequences on)| Drops nested sequences (`sequenceBegin`/`sequenceEnd`). |
| `SOFAB_DISABLE_INT64_SUPPORT`    | off (int64 on)    | Restricts integers to 32-bit. |

Minimal build (drop fp64 and sequences, keep everything else):

```sh
cmake -S . -B build -DCMAKE_CXX_FLAGS="-DSOFAB_DISABLE_FP64_SUPPORT -DSOFAB_DISABLE_SEQUENCE_SUPPORT"
cmake --build build --parallel
ctest --test-dir build --output-on-failure   # the matching vectors are skipped automatically
```

> Note: because this header is intentionally always-on, the `SOFAB_DISABLE_*`
> defines currently affect only which conformance vectors run; they are the
> forward-compatible hooks a footprint-constrained fork would gate the header on.
> If a strictly minimal binary is the goal, prefer the C corelib (`corelib-c-cpp`).

## Build & test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires a C++20-capable compiler (GCC 11+, Clang 14+, MSVC 19.30+) and CMake 3.20+.

Test suites:

- **`test_roundtrip`** — focused encode/decode/nested/chunked/skip checks plus
  malformed-input handling (truncated/overlong varints, oversized lengths,
  stray markers) and resync after a skipped sub-sequence.
- **`test_vectors`** — replays the shared `assets/test_vectors.json` conformance
  suite (generated by and copied verbatim from `corelib-c-cpp`, the authoritative
  source per the spec) for encode, decode, and byte-at-a-time chunked streaming.

### Coverage and API docs

```sh
# line/branch coverage of the header (needs gcovr)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSOFAB_ENABLE_COVERAGE=ON
cmake --build build --parallel && ctest --test-dir build
gcovr --root . --filter '^include/sofab/.*\.hpp$' --object-directory build --print-summary

# Doxygen HTML into build/docs/html (needs doxygen + graphviz)
cmake -S . -B build -DSOFAB_ENABLE_DOXYGEN=ON
cmake --build build --target doc
```

CI runs these on every push: GCC/Clang build+test, a big-endian (ppc64) build
that runs the suite under qemu, a coverage job that publishes the badge above,
and a Doxygen job that deploys the API docs to GitHub Pages.

> CI layout vs. the spec: the language-independent plan (§12) names two canonical
> workflows, `ci.yml` and `docs.yml`. This repo instead splits CI across
> per-target files (`build-gcc-x86_64.yaml`, `build-clang-x86_64.yaml`,
> `build-gcc-ppc64-bigendian.yaml`, `coverage.yaml`) plus a separate
> `build-doxygen.yaml` for docs; the build matrix currently exercises the **Debug**
> configuration only. These are tracked deviations — the build/test/coverage/docs
> coverage itself is equivalent, only the file names and the Release leg differ.

## Benchmarks

Two tools mirror the C/C++/Rust/Go/Java/Python benchmarks — same workloads (a
1000-element `u64` array and a typical mixed message) — so results are directly
comparable across languages:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --target run_perf   # per-op cost (cycles/op + MB/s)
cmake --build build --target run_bench  # throughput (MB/s), C and C++
```

`perf` uses hardware cycle counters (x86 TSC / AArch64 counter) for a
machine-independent cost figure. `bench` reports sustained throughput in MB/s
(MB = 1e6 bytes) over a ~1 s CPU-time loop.

Machine-independent instruction counts (Callgrind) comparing this library against
the C corelib and its C++ wrapper:

| Workload | C (`-Os`) | C++ wrapper | this (pure C++20) |
|---|--:|--:|--:|
| encode: u64 array (1000) | 137 458 | 137 488 | **106 963** (−22 %) |
| encode: typical message  |     796 |     826 | **233** (−71 %) |
| decode: u64 array (1000) | 250 917 | 250 918 | **169 002** (−33 %) |
| decode: typical message  |   1 771 |   1 773 | **1 432** (−19 %) |

For Callgrind single-shot runs:

```sh
cmake --build build --target run_bench_callgrind  # needs valgrind
```
