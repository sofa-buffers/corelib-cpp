<p align="center"><img src="assets/sofabuffers_logo.png" alt="SofaBuffers Logo" height="140"></p>

# SofaBuffers C++ library

A **streaming**, **dependency-free**, pure-**C++20** implementation of the
SofaBuffers (*Sofab*) serialization format — written from scratch with no C
backend. It packs structured fields into a caller-owned buffer and decodes them
through a protobuf-style cursor that advances a pointer over the message.

[GitHub repository](https://github.com/sofa-buffers/corelib-cpp)

It is API-compatible with the C/C++ corelib's
[`sofab.hpp`](https://github.com/sofa-buffers/corelib-c-cpp) — same
`sofab::OStream` / `sofab::OStreamInline` / `sofab::IStreamObject` surface — but
shares no code with it and is tuned for raw speed.

> Validated against the **full shared conformance suite**: all 47 vectors in
> `assets/test_vectors.json` pass encode / decode / roundtrip / chunked
> (329 checks) — the same coverage the C library runs.

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

## Performance

Machine-independent instruction counts (Callgrind, lower is better) for the shared
bench workloads, comparing this library against the C corelib and its C++ wrapper:

| Workload | C (`-Os` lib) | C++ wrapper | **this (pure C++20)** |
|---|--:|--:|--:|
| encode: u64 array (1000) | 137458 | 137488 | **106963** (−22%) |
| encode: typical message |    796 |    826 | **233** (−71%) |
| decode: u64 array (1000) | 250917 | 250918 | **169002** (−33%) |
| decode: typical message |   1771 |   1773 | **1432** (−19%) |

Sustained throughput for a ~112-byte mixed message (`-O3`, x86-64):
encode ≈ 1.3–1.5 GB/s, decode ≈ 0.8 GB/s. Two tools reproduce these:

- **`bench`** — sustained throughput (MB/s); also the Callgrind single-shot
  driver. `cmake --build build --target run_bench`
- **`perf`** — per-operation cost (hardware cycles/op + MB/s).
  `cmake --build build --target run_perf`

## Architecture compliance

Implements the wire format in
[ARCHITECTURE.md](https://github.com/sofa-buffers/documentation/blob/main/ARCHITECTURE.md):
all 8 wire types with `(id << 3) | type` header packing, LEB128 varint and zig-zag
signed encoding, little-endian byte order, fixed `0x07` sequence-end, streaming
encode via flush callback and streaming decode via `feed`, in the `sofab::`
namespace.

## Build & test

Header-only — add `include/` to your include path. To run the checks and benches:

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

- **`test_roundtrip`** — focused encode/decode/nested/chunked/skip checks.
- **`test_vectors`** — drives the encoder/decoder over the full
  `assets/test_vectors.json` suite (47 vectors, 329 checks).

## Example

```cpp
#include "sofab/sofab.hpp"

// encode
sofab::OStreamInline<64> os;
os.write(1, 42u).write(2, -7).write(3, "hi");
std::span<const uint8_t> msg{os.data(), os.bytesUsed()};

// decode
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

## License

MIT — see [LICENSE](LICENSE).
