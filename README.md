<p align="center"><img src="assets/sofabuffers_logo.png" alt="SofaBuffers" height="140"></p>

# SofaBuffers

<b>Structured Objects For Anyone</b><br>
<i>... so optimized, feels amazing.</i>

[Would you like to know more?](https://github.com/sofa-buffers)

## SofaBuffers C++ library

[GitHub repository](https://github.com/sofa-buffers/corelib-cpp)

A **streaming**, **dependency-free**, pure-**C++20** implementation of the
SofaBuffers (*Sofab*) serialization format — written from scratch with no C
backend. It packs structured fields into a caller-owned buffer and decodes them
through a protobuf-style cursor that advances a pointer over the message.

It is API-compatible with the C/C++ corelib's
[`sofab.hpp`](https://github.com/sofa-buffers/corelib-c-cpp) — same
`sofab::OStream` / `sofab::OStreamInline` / `sofab::IStreamObject` surface — but
shares no code with it and is tuned for raw speed. Requires **C++20** or later.

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
#include <vector>

std::vector<uint8_t> out;
uint8_t scratch[16];
sofab::OStream os(scratch, sizeof(scratch), 0,
    [&](const uint8_t* p, size_t n){ out.insert(out.end(), p, p + n); });
for (uint32_t i = 0; i < 1000; i++)
    os.write(sofab::id(i), uint64_t(i));
os.flush(); // push the tail
```

## API summary

**Encoder**

| Class | Purpose |
|-------|---------|
| `OStreamInline<N, Offset=0>` | Stack-allocated N-byte output stream — zero heap, suitable for any target |
| `OStream(buf, len, offset, flush)` | Buffer + flush-callback encoder; `flush` called whenever the buffer fills |

Common methods: `write(id, value)` (deduces type — unsigned, signed, bool, fp32, fp64, string, blob, array); `sequenceBegin(id)` / `sequenceEnd()`; `flush()`; `bytesUsed()`; `data()`.

**Decoder**

| Class | Purpose |
|-------|---------|
| `IStreamMessage` | Abstract base — override `deserialize(is, id, size, count)` to bind fields |
| `IStreamObject<T>` | Wraps an `IStreamMessage`; call `feed(buf, len)` to decode; access via `->` / `*` |
| `IStreamInline` | Lambda-based decoder — pass a `std::function` without subclassing |

Inside `deserialize`, call `is.read(dest)` to bind a field or do nothing to skip.

**Constants:** `sofab::API_VERSION` (`1`), `sofab::ID_MAX`.

## Feature flags

The library is header-only C++20. All wire features — fixlen values (fp32/fp64,
string, blob), arrays, and sequences — are always included; there are no
compile-time disable switches. Use the C corelib (`corelib-c-cpp`) if binary
footprint is a constraint.

## Build & test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires a C++20-capable compiler (GCC 11+, Clang 14+, MSVC 19.30+) and CMake 3.20+.

Test suites:

- **`test_roundtrip`** — focused encode/decode/nested/chunked/skip checks.
- **`test_vectors`** — replays the shared `assets/test_vectors.json` conformance
  suite (copied verbatim from the `documentation` repo) for encode, decode, and
  byte-at-a-time chunked streaming.

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
