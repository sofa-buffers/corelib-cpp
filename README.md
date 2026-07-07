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

It presents the same `sofab::OStream` / `sofab::OStreamInline` / `sofab::IStreamObject`
surface as the footprint-oriented C/C++ corelib
([`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp)), but shares no
code with it and is tuned for raw throughput. See
[Choosing between the two C++ corelibs](#choosing-between-the-two-c-corelibs)
for when to pick which.

### Requirements

- A **C++20**-capable compiler — GCC 11+, Clang 14+, or MSVC 19.30+.
- CMake **3.10+** to build the tests, benchmarks and docs (the library itself is
  header-only and needs no build step).

### Dependencies

**None** beyond the C++ standard library. The single header pulls in only
standard headers (`<array>`, `<bit>`, `<concepts>`, `<span>`, `<string>`,
`<string_view>`, `<memory>`, `<functional>`, …); there are no third-party
dependencies and no C backend.

### Built with the following compilers

| Target | Status |
| - | - |
| GCC x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml) |
| Clang x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml) |
| GCC ppc64 (big endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml) |

> The non-native targets above are built and run under [QEMU](https://www.qemu.org/) user-mode emulation in CI, so you can reproduce any of them locally without the real hardware.

### Packaging

Distributed as the port `sofa-buffers-corelib-cpp`; every route exposes the
same target `sofa-buffers::corelib` and `#include <sofab/…>`.

#### CMake

Pull it straight from the repo with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  sofa-buffers-corelib-cpp
  GIT_REPOSITORY https://github.com/sofa-buffers/corelib-cpp.git
  GIT_TAG        <tag or branch>
)
FetchContent_MakeAvailable(sofa-buffers-corelib-cpp)
target_link_libraries(my_app PRIVATE sofa-buffers::corelib)
```

#### Conan

Distributed as the Conan package `sofa-buffers-corelib-cpp`
([`conanfile.py`](conanfile.py)), which installs a CMake package config exposing
the same target:

```cmake
find_package(sofa-buffers-corelib-cpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE sofa-buffers::corelib)
```

## Why this design

The C corelib (`corelib-c-cpp`) is optimised for **minimal code size and RAM**
(it targets bare metal). This library makes the opposite trade: size and memory
are not a concern, the goal is **throughput**, and the decoder is tuned for the
common case where the whole message is already in contiguous memory.

| Goal | How |
|------|-----|
| Fast encode | Payloads written with a single `memcpy`; a field's header + value varints emitted as one combined write; whole float arrays copied in one shot on little-endian. |
| Fast decode | The common case parses **with zero copies and zero allocations** — the cursor walks the caller's buffer in place; float arrays bulk-`memcpy`'d; `std::string_view` reads are zero-copy views into the buffer. |
| Still streamable | `OStream`/`OStreamInline` flush a small buffer via callback; `feed()` dispatches each complete top-level field and buffers only an incomplete tail. |
| Modern C++ | `std::span`, `std::bit_cast`, concepts, `if constexpr` `write()`/`read()` deduction, `[[nodiscard]]`. Little-endian handled explicitly — no host-endian branching. |

## Usage

### Simple encode & decode

```cpp
#include "sofab/sofab.hpp"

// ---- encode into an inline (stack) buffer, no heap ----
sofab::OStreamInline<64> os;
os.write(1, 42u).write(2, -7).write(3, "hi");
std::span<const uint8_t> msg{os.data(), os.bytesUsed()};

// ---- decode by pushing fields into an IStreamMessage ----
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

Every `write()` returns a chainable `Result` that latches the first error, so a
whole message is written fluently and checked once at the end. During decode, a
field you don't `read()` is measured and skipped automatically — unknown or
unwanted fields cost only a step-over.

### Streaming a message larger than the buffer (`OStream`)

`OStream`/`OStreamInline` never grow their buffer. Give them a flush callback and
the buffer becomes a small reusable window: it is drained whenever it fills (and
once more at the end), so the encoder never needs the whole message in memory.

```cpp
#include "sofab/sofab.hpp"
#include <span>
#include <vector>

std::vector<uint8_t> out;

// A 16-byte window — far smaller than the message. The flush callback takes a
// std::span<const uint8_t> and drains the window each time it fills.
sofab::OStreamInline<16> os(
    [&](std::span<const uint8_t> chunk){ out.insert(out.end(), chunk.begin(), chunk.end()); });

for (uint32_t i = 0; i < 1000; i++)
    os.write(sofab::id(i), uint64_t(i));
os.flush(); // push the tail; `out` now holds the complete message
```

A heap-backed `sofab::OStream` works the same way and additionally lets you swap
the backing buffer mid-stream (typically inside the callback) via `setBuffer()`.
Its buffer is owned through a `std::shared_ptr<uint8_t[]>` — either allocated for
you (`OStream(buflen, offset = 0)`) or handed in
(`OStream(flush, buffer, buflen, offset = 0)`):

```cpp
auto buf = std::shared_ptr<uint8_t[]>(new uint8_t[256]);
sofab::OStream os([&](std::span<const uint8_t> c){ sink(c); }, buf, 256);
os.write(1, 123u).write(2, "payload");
os.flush(); // OStream's destructor also flushes automatically
```

### Streaming decode (`IStream`)

The decoder is a pull cursor: `feed()` can be called repeatedly with whatever
bytes have arrived. A field that straddles a chunk boundary is buffered
internally and re-parsed once its remainder arrives, so the same
`IStreamObject`/`IStreamInline` decodes a byte-at-a-time stream identically to a
one-shot buffer:

```cpp
sofab::IStreamObject<Sensor> in;
for (uint8_t b : wire)            // feed one byte at a time
    in.feed(&b, 1);
// (*in) is fully populated
```

`IStreamInline` is the lambda variant of the same decoder — construct it with a
`(id, size, count)` callback instead of subclassing `IStreamMessage`.

### Generated code (the common case)

The usual way to drive the library is through **generated object code**: a schema
compiled by `sofabgen` emits a struct per message that derives `OStreamMessage`
(with a `serialize`) and `IStreamMessage` (with a `deserialize`) plus a
`_maxSize` bound, and calls this runtime under the hood. A hand-written
equivalent looks like:

```cpp
struct Point : sofab::OStreamMessage, sofab::IStreamMessage {
    static constexpr std::size_t _maxSize = 32;  // upper bound on the encoded size
    int32_t x = 0, y = 0;

    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl& os) const noexcept override {
        return os.write(1, x).write(2, y);
    }
    void deserialize(sofab::IStreamImpl& is, sofab::id id, size_t, size_t) noexcept override {
        switch (id) { case 1: is.read(x); break; case 2: is.read(y); break; }
    }
};

// encode: serialize the message into an inline buffer sized by _maxSize
Point pt; pt.x = 3; pt.y = 4;
sofab::OStreamInline<Point::_maxSize> enc;
pt.serialize(enc);
std::span<const uint8_t> wire{enc.data(), enc.bytesUsed()};

// decode: IStreamObject routes each top-level field into deserialize()
sofab::IStreamObject<Point> dec;
dec.feed(wire.data(), wire.size());
// (*dec).x == 3, (*dec).y == 4
```

Messages nest: passing a message deriving `OStreamMessage` to `write(id, msg)`
encodes it as a sub-sequence, and `is.read(childMsg)` on the decode side descends
into it. `OStreamObject<Msg>` / `IStreamObject<Msg>` bundle a message with an
inline buffer if you prefer to carry both together.

## API summary

All public types are in the `sofab` namespace.

### Encoding API

Encoders come in three flavours — `OStreamInline<N, Offset = 0>` (an `N`-byte
stack buffer, zero heap), `OStream` (a `std::shared_ptr`-owned heap buffer that
can be swapped at runtime), and `OStreamObject<Msg>` (an inline buffer bundled
with a generated message). All share one chainable, sticky-error API on
`OStreamImpl`:

- **`write(id, value)`** deduces the wire type from the C++ type — unsigned and
  signed integers, `bool`, `float`/`double`, strings (anything convertible to
  `std::string_view`), and `std::span`/contiguous containers of integer or float
  scalars (encoded as arrays). `write(id, ptr, int32_t size)` writes an opaque
  blob. `writeIf(id, value, cond)` writes conditionally.
- **`sequenceBegin(id)` / `sequenceEnd()`** open and close nested sub-messages
  explicitly; alternatively `write(id, msg)` encodes an `OStreamMessage`.
- Every call returns a **`Result`** that latches the *first* error (`ok()`,
  `code()`, `operator bool`), so a message is written fluently and validated
  once. `bytesUsed()` / `data()` expose the current contents; `flush()` pushes
  the tail through the flush callback.

Anything `write()`/`read()` can't map to a wire type is a compile-time
`static_assert` — including arrays of *dynamic* element types (spans of `bool`,
`std::string`, blobs, or nested messages), since fixlen arrays carry one fixed
element size on the wire.

### Decoding API

Decoding is driven by **`feed(buf, len)`**, which returns a `Result` and can be
called repeatedly for streaming. For each top-level field it invokes a
per-field handler — either a subclass's `deserialize(is, id, size, count)`
(via `IStreamObject<Msg>`) or a `std::function` callback (via `IStreamInline`) —
where `size` is the payload byte-length (element size for fixlen arrays) and
`count` is the array element count (`0` for scalars).

Inside the handler, `is.read(dest)` binds the field; the destination type selects
the decoder via `if constexpr`. Integers, `bool`, and floats decode by value;
`std::string`/blob reads copy into caller storage; `std::string_view` reads are
**zero-copy** views into the source buffer; arrays decode into a caller-provided
`std::span`/container. Reading a message deriving `IStreamMessage` descends a
sub-sequence. **Doing nothing skips** the field. The cursor is bounds-checked:
a malformed or truncated field latches an error and `feed()` returns
`Error::InvalidMessage`.

### Memory handling

This is the defining trade-off of the C++ port, and it is the opposite of the C
(`corelib-c-cpp`) port.

**Decoder — zero-copy views into the caller's buffer.** `feed()` parses *in
place*: when nothing is buffered (the common case — a whole message handed in at
once) the cursor walks straight over the caller's contiguous `buf`, allocating
nothing and copying nothing. Consequently:

- `read(std::string_view&)` returns a view that **points directly into `buf`**;
  no bytes are copied. The view stays valid only as long as that source buffer
  stays alive and unmodified — outliving the buffer is a dangling-view bug. Use
  `read(std::string&)` when you need an owning copy.
- Integer arrays are decoded element-wise straight into the caller-provided
  `span`/container; float arrays are bulk-`memcpy`'d into it (one `memcpy` on
  little-endian). Either way the destination storage is the caller's — the
  stream never allocates it.
- `read(void* dst, size_t maxlen)` copies a blob into the caller's `dst` and
  returns the number of bytes written (`min(maxlen, size)`).
- Because decoding is a pointer walk over contiguous memory, **the whole message
  must eventually be in contiguous buffers.** If a chunk handed to `feed()` ends
  mid-field, only that incomplete trailing field is copied into an internal
  accumulator and re-parsed on the next `feed()`; views read from such a stitched
  field point into that accumulator instead of the original chunk.

This is the inverse of the C port's deferred-copy model, where `read()` only
*binds* a destination pointer and a later `feed()` copies the bytes into it as
they arrive (so destinations must be address-stable). Here `read()` pulls the
value out immediately and strings come back as views, so there is no per-field
destination to keep stable — but the **input buffer** must stay alive for as long
as any returned view is used.

**Encoder — writes into an owned, fixed-size buffer; flushes, never grows.** An
`OStream` owns its buffer through a `std::shared_ptr<uint8_t[]>` (allocated for
you, handed in, or shared); an `OStreamInline<N>` owns an `N`-byte `std::array`
on the stack with zero heap use. Neither buffer grows. When the cursor reaches
the end it calls the flush callback with the filled bytes and rewinds;
**without** a callback a full buffer yields `Error::BufferFull`. So to emit a
message larger than the buffer you supply a flush callback (see the streaming
example above) and the buffer acts as a small reusable window.

## Feature flags

The library is header-only C++20 and **ships with every wire feature enabled by
default** — this port optimises for throughput rather than footprint, so unlike
the C corelib (`corelib-c-cpp`) the header itself contains no `#ifdef` gating and
all features are always compiled in. The conformance harness
(`test/test_vectors.cpp`), however, recognises the family-standard
`SOFAB_DISABLE_*` toggles and skips the vectors a disabled feature would
exercise, so a feature-reduced profile can be described and tested with the same
switches used across the other ports.

| Build option (define) | Default | Effect |
|-----------------------|---------|--------|
| `SOFAB_DISABLE_FIXLEN_SUPPORT`   | off (fixlen on)   | Drops fp32/fp64, string and blob fixlen values (and fixlen arrays). |
| `SOFAB_DISABLE_FP64_SUPPORT`     | off (fp64 on)     | Drops 64-bit `double` support (fp32 stays). |
| `SOFAB_DISABLE_ARRAY_SUPPORT`    | off (arrays on)   | Drops the array wire types (unsigned / signed / fixlen arrays). |
| `SOFAB_DISABLE_SEQUENCE_SUPPORT` | off (sequences on)| Drops nested sequences (`sequenceBegin`/`sequenceEnd`). |
| `SOFAB_DISABLE_INT64_SUPPORT`    | off (int64 on)    | Restricts integers to 32-bit. |

```sh
# drop fp64 and sequences; the matching conformance vectors are skipped automatically
cmake -S . -B build -DCMAKE_CXX_FLAGS="-DSOFAB_DISABLE_FP64_SUPPORT -DSOFAB_DISABLE_SEQUENCE_SUPPORT"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

> Because this header is intentionally always-on, the `SOFAB_DISABLE_*` defines
> currently affect only which conformance vectors run; they are the
> forward-compatible hooks a footprint-constrained fork would gate the header on.
> If a strictly minimal binary is the goal, prefer the C corelib
> (`corelib-c-cpp`).

## Build & test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.30+) and CMake 3.10+.
Two suites run under CTest:

- **`test_roundtrip`** — focused encode/decode/nested/chunked/skip checks plus
  malformed-input handling (truncated/overlong varints, oversized lengths, stray
  markers) and resync after a skipped sub-sequence.
- **`test_vectors`** — replays the shared `assets/test_vectors.json` conformance
  suite (copied verbatim from `corelib-c-cpp`, the authoritative source) for
  encode, decode, and byte-at-a-time chunked streaming.

### Coverage and API docs

```sh
# line/branch coverage of the header (needs gcovr)
cmake -S . -B build -DSOFAB_ENABLE_COVERAGE=ON
cmake --build build --parallel && ctest --test-dir build
gcovr --root . --filter '^include/sofab/.*\.hpp$' --object-directory build --print-summary

# Doxygen HTML (needs doxygen + graphviz)
cmake -S . -B build -DSOFAB_ENABLE_DOXYGEN=ON
cmake --build build --target doc
```

## Benchmarks

Two tools mirror the C / C++ / Rust / Go / Java / Python benchmarks — same
workloads (a 1000-element `u64` array and a typical mixed message) — so results
are directly comparable across languages:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --target run_perf    # per-op cost (cycles/op + MB/s)
cmake --build build --target run_bench   # sustained throughput (MB/s)
```

`perf` reads a hardware cycle counter (x86 TSC / AArch64 `cntvct_el0`) for a
machine-independent cost figure, then reports throughput in MB/s (MB = 1e6
bytes) over a CPU-time loop; `bench` reports the throughput table on its own. For
machine-independent instruction counts, the same `bench` binary doubles as a
Callgrind single-shot driver:

```sh
cmake --build build --target run_bench_callgrind  # needs valgrind
```

The instruction-count figures it produces are the head-to-head data in
[Choosing between the two C++ corelibs](#choosing-between-the-two-c-corelibs)
below.

## Choosing between the two C++ corelibs

SofaBuffers ships **two** C++ implementations of the same wire format, tuned for
opposite ends of the spectrum:

- **`corelib-cpp` (this library)** — pure C++20, no C backend. Optimised for
  **throughput** on desktop/server targets where code size and RAM are not a
  concern. Decodes zero-copy in place and returns `std::string_view`s into the
  caller's buffer.
- **[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp)** — a C
  object API with a thin C++ wrapper (`sofab.hpp`). Optimised for **minimal code
  size and RAM** on bare-metal / microcontroller targets, using a deferred-copy
  model (bind destinations, copy on `feed()`) that keeps the footprint tiny.

Both expose a compatible `sofab::OStream` / `sofab::IStreamObject` surface, so
porting between them is mostly mechanical.

| | `corelib-cpp` (this) | `corelib-c-cpp` |
|---|---|---|
| Primary goal | Maximum throughput | Minimum footprint |
| Implementation | Pure C++20, header-only | C core + C++ wrapper header |
| Decode model | Zero-copy views into caller buffer | Deferred-copy into address-stable destinations |
| Feature gating | Always full format (no `#ifdef`) | `SOFAB_DISABLE_*` compile out unused wire features |
| Target | Desktop / server | Bare metal / embedded C and C++ |

### Instruction counts (Callgrind)

Machine-independent instruction counts from the shared benchmark tooling,
comparing this library against the C corelib and its C++ wrapper on identical
workloads (lower is better; the pure-C++20 column is produced by this repo's
`run_bench_callgrind` target):

| Workload | C (`-Os`) | C++ wrapper | this (pure C++20) |
|---|--:|--:|--:|
| encode: u64 array (1000) | 137 458 | 137 488 | **106 963** (−22 %) |
| encode: typical message  |     796 |     826 | **233** (−71 %) |
| decode: u64 array (1000) | 250 917 | 250 918 | **169 002** (−33 %) |
| decode: typical message  |   1 771 |   1 773 | **1 432** (−19 %) |

The pure-C++20 port wins on instructions across the board because it fuses
header+value writes, bulk-copies arrays, and parses in place without the C port's
per-field bookkeeping. In the multi-language benchmark arena it lands around a
434-byte wire size and roughly **1.5× the throughput of protobuf** for a
comparable C++ message.

**Rule of thumb:** reach for **`corelib-cpp`** for desktop/server throughput,
and for **`corelib-c-cpp`** when you need a strictly minimal binary and tight RAM
on a footprint-constrained target — where its C++-wrapper build has been measured
at roughly 2× the throughput of EmbeddedProto with a fraction of the flash on a
Cortex-M.
