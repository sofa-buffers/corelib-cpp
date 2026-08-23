<p align="center"><img src="assets/sofabuffers_logo.png" alt="SofaBuffers" height="140"></p>

# SofaBuffers

<b>Structured Objects For Anyone</b><br>
<i>... so optimized, feels amazing.</i>

[Would you like to know more?](https://github.com/sofa-buffers)

## SofaBuffers C++ library

[![CI](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/sofa-buffers/corelib-cpp/badges/coverage-cpp.json)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/ci.yml)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://sofa-buffers.github.io/corelib-cpp/)

[GitHub repository](https://github.com/sofa-buffers/corelib-cpp)

A **streaming**, **dependency-free**, pure-**C++20** implementation of the
SofaBuffers (*Sofab*) serialization format, written from scratch with no C
backend. It packs structured fields into a caller-owned buffer and decodes them
with a protobuf-style cursor that advances over the message.

It presents the same `sofab::OStream` / `sofab::OStreamInline` /
`sofab::IStreamObject` surface as the footprint-oriented
[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp) but shares no
code with it and is tuned for raw throughput — see
[Choosing between the two C++ corelibs](#choosing-between-the-two-c-corelibs).

### Requirements

- A **C++20** compiler — GCC 11+, Clang 14+, or MSVC 19.30+.
- CMake **3.10+** for the tests, benchmarks and docs. The library is
  header-only and needs no build step.

### Dependencies

**None** beyond the C++ standard library (`<array>`, `<bit>`, `<concepts>`,
`<span>`, `<string>`, `<string_view>`, `<memory>`, `<functional>`, …). No
third-party dependencies, no C backend.

### Built with the following compilers

Non-native targets run under [QEMU](https://www.qemu.org/) user-mode emulation
in CI. Both x86_64 legs build and run the suite in `Debug` **and** `Release`
(`-O3 -DNDEBUG`), so a defect only the optimiser exposes cannot ship green.

| Target | Compiler | Runs |
| - | - | - |
| x86_64 (little endian) | GCC, Debug and Release | build + test suite |
| x86_64 (little endian) | Clang, Debug and Release | build + test suite |
| ppc64 (big endian) | GCC, cross + static | build + test suite under QEMU |

The CI badge above covers all three — they are one pipeline, and GitHub
publishes one badge per workflow file.

### Packaging

Distributed as the port `sofa-buffers-corelib-cpp`; every route exposes the same
target `sofa-buffers::corelib` and `#include <sofab/…>`.

#### CMake

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

The Conan package ([`conanfile.py`](conanfile.py)) installs a CMake package
config exposing the same target:

```cmake
find_package(sofa-buffers-corelib-cpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE sofa-buffers::corelib)
```

## Why this design

The C corelib optimises for minimal code size and RAM; it targets bare metal.
This library makes the opposite trade: size and memory are not a concern, the
goal is **throughput**, and the decoder is tuned for a message already in
contiguous memory.

| Goal | How |
|------|-----|
| Fast encode | Payloads written with a single `memcpy`; a field's header + value varints emitted as one write; whole float arrays copied in one shot on little-endian. |
| Fast decode | The *parse* allocates nothing — the cursor walks the caller's buffer in place, and only an incomplete trailing field is ever buffered; float arrays are bulk-`memcpy`'d; a `string`/`blob` payload is copied straight into the destination. |
| Still streamable | `OStream`/`OStreamInline` flush a small buffer via callback; `feed()` dispatches each complete top-level field and buffers only an incomplete tail. |
| Modern C++ | `std::span`, `std::bit_cast`, concepts, `if constexpr` `write()`/`read()` deduction, `[[nodiscard]]`. Little-endian handled explicitly. |

## Usage

Four use cases — serialize a message that fits one buffer, serialize one too
large for it, deserialize a whole message, deserialize one arriving in chunks —
plus the generated-code path that wraps them.

### Serialize

Write into a stack buffer and take a view of the bytes. Every `write()` returns
a chainable `Result` that latches the first error, so you check once at the end.

```cpp
#include "sofab/sofab.hpp"

sofab::OStreamInline<64> os;          // 64-byte inline (stack) buffer, no heap
os.write(1, 42u)
  .write(2, -7)
  .write(3, "hi");
std::span<const uint8_t> msg{os.data(), os.bytesUsed()};
```

An array field is written **whole**: every element the container holds reaches
the wire, trailing default-valued ones included. A schema `count: N` is a
capacity while the wire count `M` is the array's *length*, so `{1, 2, 3, 0, 0}`
encodes as `M = 5` (MESSAGE_SPEC §3).

### Serialize stream

For a message larger than the buffer, give the stream a flush callback: the
buffer becomes a small reusable window, drained whenever it fills and once at
the end.

```cpp
std::vector<uint8_t> out;
sofab::OStreamInline<16> os(          // 16-byte window; drained each time it fills
    [&](std::span<const uint8_t> chunk){ out.insert(out.end(), chunk.begin(), chunk.end()); });

for (uint32_t i = 0; i < 1000; i++)
    os.write(sofab::id(i), uint64_t(i));
os.flush();                           // push the tail; `out` holds the whole message
```

### Deserialize

Derive a message from `IStreamMessage` and dispatch fields in `deserialize()`;
`IStreamObject` wires the decoder to an embedded instance. Fields you do not
`read()` are measured and skipped automatically.

```cpp
struct Sensor : sofab::IStreamMessage {
    uint32_t id = 0; float value = 0;
    void deserialize(sofab::IStreamImpl& is, sofab::id i, size_t, size_t) noexcept override {
        switch (i) { case 1: is.read(id); break; case 2: is.read(value); break; }
    }
};

sofab::IStreamObject<Sensor> in;
in.feed(msg.data(), msg.size());      // msg from the Serialize example above
// (*in).id and (*in).value now hold the decoded values
```

#### One message per destination — `reset()` between messages

A message carries no framing, and an all-default message is the empty byte
string, so a decoder cannot see a message boundary: successive `feed()` calls
continue **one** message. Call `reset()` to decode a second one into the same
object — it re-initialises the wrapped message and the decoder together
(reassembly buffer, sticky flags, the latched verdict, `skipped()` counter).

```cpp
sofab::IStreamObject<Sensor> in;
in.feed(a.data(), a.size());          // message A
in.reset();                           // ← required
in.feed(b.data(), b.size());          // message B, on a clean destination
```

Without it, every field message B does not carry keeps message A's value — for
scalars, because §2 omits a default-valued field, and equally for a
**wrapper-array** field, whose collector clears the destination only when the
wrapper sequence is present. MESSAGE_SPEC §5.1 places this duty on the decoding
side. (`IStreamInline` has the same `reset()` but owns no destination; a
callback-driven decoder clears its own targets.)

### Deserialize stream

`feed()` takes whatever bytes have arrived; a field straddling a chunk boundary
is buffered and re-parsed once its remainder comes, so a chunked stream decodes
identically to a one-shot buffer.

```cpp
sofab::IStreamObject<Sensor> in;
for (uint8_t b : wire)                // feed whatever arrives — here one byte at a time
    in.feed(&b, 1);
// (*in) is fully populated
```

Each `feed()` returns a three-valued decode outcome. There is **no** separate
`finish`/`finalize` step, and the same three apply to a one-shot buffer:

| `Result`                | `code()`                 | `status()`                    | meaning |
|-------------------------|--------------------------|-------------------------------|---------|
| `complete()` / `ok()`   | `Error::None`            | `DecodeStatus::Complete`      | the consumed bytes end **exactly** at a field boundary — a valid message |
| `incomplete()`          | `Error::Incomplete`      | `DecodeStatus::Incomplete`    | the bytes end **inside** a field (a partial varint, a short fixlen/array payload) or with an open sequence; the partial tail is retained for the next `feed()` |
| `invalid()`             | `Error::InvalidMessage`  | `DecodeStatus::Invalid`       | the bytes are malformed **regardless of what follows** (varint over 64 bits, bad subtype/length, count/id over max, nesting past `MAX_DEPTH`, dangling sequence-end, …) |

`Incomplete` is not an error — it means "the message may continue". A streaming
caller reads it as "feed me more bytes"; a caller that has delivered all its
bytes and still sees it knows the message was truncated.

`Invalid` and the `LimitExceeded` policy code below are **terminal**, and the
decoder latches them on the stream: once a `feed()` returns either, every later
`feed()` returns it immediately without parsing. `reset()` is the way back.

```cpp
sofab::IStreamObject<Sensor> in;
in.feed(garbage.data(), garbage.size());  // Invalid
in.feed(wire.data(), wire.size());        // still Invalid — the stream is condemned
in.reset();                               // ← the only way back
in.feed(wire.data(), wire.size());        // Complete
```

The latch is what keeps the outcome chunk-independent: the same bytes fed whole,
in odd-sized chunks or one at a time end on the same verdict, and a sender cannot
prefix garbage to a valid message and still have the receiver report `Complete`.

#### Streaming buffer limit (opt-in)

A never-completing or huge trailing field would otherwise grow the internal
reassembly buffer without bound — a field may claim up to `FIXLEN_MAX` ≈ 2 GB.
Pass a `sofab::Limits` to cap it:

```cpp
sofab::IStreamObject<Sensor> in{ sofab::Limits{ .max_buffered_field = 64 * 1024 } };
```

`max_buffered_field` bounds how large a *single* incomplete top-level field may
grow the buffer. A field whose declared size exceeds it fails `feed()` with
`Error::LimitExceeded` the moment the size is known, before the payload is
buffered — so an oversized header is rejected even if its bytes never arrive,
whether fed whole or byte by byte. This is a receiver-side **policy** code, kept
distinct from `Error::InvalidMessage`: a local limit is not wire malformation.
The default is no cap (`SIZE_MAX`). Bytes are never clamped or truncated; the
`feed()` simply fails.

**A schema bound outranks the cap.** A declared length past a `maxlen`, or a
count past a `count:`, is `Error::InvalidMessage` whatever the cap is set to
(§6.2.1, §6.3) — the same bytes must not decode as a validity failure on one
receiver and a capacity refusal on another. An over-cap field is therefore still
delivered to its callback, which is the only place the schema bound is known, but
with its **payload withheld**: `readString`/`readArray`/… settle the wire type
(§7.3) and the schema bound (§7.1) and then report `Incomplete` instead of
materialising anything. Only a field the schema leaves unbounded ends in
`LimitExceeded`.

#### Strict UTF-8 validation (`SOFAB_STRICT_UTF8`, default ON)

A `string` carries UTF-8 text; `blob` is the type for opaque bytes. With
`SOFAB_STRICT_UTF8` **ON** (the default) an invalid-UTF-8 `string` is rejected
symmetrically:

- **encode** — `write(id, string_view)` for a non-UTF-8 value returns
  `Error::InvalidArgument` and emits nothing;
- **decode** — a *materialised* `string` whose complete payload is not valid
  UTF-8 is `Error::InvalidMessage` / `DecodeStatus::Invalid`.

`sofab::utf8_valid(std::string_view)` is a real validator, not a byte-range
shortcut: it rejects overlong forms (including `C0 80`), surrogates
`U+D800`–`U+DFFF` and code points above `U+10FFFF`, and accepts an embedded
`U+0000`. A `blob` is never validated in either direction, and a **skipped**
`string` is never validated — only a materialised read is.

Define `SOFAB_STRICT_UTF8=0` before including `<sofab/sofab.hpp>` for a
documented non-strict build: the validation code folds away entirely and payloads
are stored verbatim — raw, never lossy. It is a validation policy, never a
wire-format switch, so peers with different settings interoperate on all valid
data; conformance and the shared vectors run with it ON.

It is also the **only** compile-time knob here: the header always compiles the
full wire format. For a strictly minimal binary use
[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp), which does gate
wire features.

### Code generator

The usual way to drive the library is through generated object code. A schema
compiled by `sofabgen` emits a struct per message deriving `OStreamMessage` /
`IStreamMessage`, with `serialize` / `deserialize` bodies, a `_maxSize` bound,
`encode()` / `decode()` helpers, and a `try_decode()` that surfaces the
three-valued `Result` instead of assuming success. A hand-written stand-in:

```cpp
struct Point : sofab::OStreamMessage, sofab::IStreamMessage {
    static constexpr std::size_t _maxSize = 32;   // upper bound on the encoded size
    int32_t x = 0, y = 0;

    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl& os) const noexcept override {
        return os.write(1, x).write(2, y);
    }
    void deserialize(sofab::IStreamImpl& is, sofab::id id, size_t, size_t) noexcept override {
        switch (id) { case 1: is.read(x); break; case 2: is.read(y); break; }
    }
    std::vector<uint8_t> encode() const {
        sofab::OStreamInline<_maxSize> os; serialize(os);
        return {os.data(), os.data() + os.bytesUsed()};
    }
    static Point decode(const uint8_t* data, size_t len) {
        sofab::IStreamObject<Point> in; in.feed(data, len); return *in;
    }
};

Point pt; pt.x = 3; pt.y = 4;
std::vector<uint8_t> wire = pt.encode();
Point got = Point::decode(wire.data(), wire.size());   // got.x == 3, got.y == 4
```

`sofab::OStreamObject<Point>` is the encode-side counterpart of `IStreamObject`:
it bundles a `Point` with an inline buffer of `Point::_maxSize` bytes, reaches
the message through `operator->`, and encodes in one `serialize()` call.

```cpp
sofab::OStreamObject<Point> out;
out->x = 3; out->y = 4;
out.serialize();          // out.data() / out.bytesUsed() now hold the message
```

Constructed with a flush callback it streams instead; without one the message
stays in the buffer (see [Memory handling](#memory-handling)). A third template
argument reserves a head of leading bytes, exactly like `OStreamInline`'s.

Messages nest: passing a message deriving `OStreamMessage` to `write(id, msg)`
encodes it as a sub-sequence, and `is.read(childMsg)` descends into it on decode.
The same generated struct streams on both sides — `serialize` targets any output
stream, and an `IStreamObject` accepts the wire bytes in arbitrary chunks:

```cpp
// encode: stream through a 16-byte window instead of a whole-message buffer
std::vector<uint8_t> wire;
sofab::OStreamInline<16> os(
    [&](std::span<const uint8_t> chunk){ wire.insert(wire.end(), chunk.begin(), chunk.end()); });
pt.serialize(os);
os.flush();

// decode: feed whatever arrives; poll the value once feed() reports complete()
sofab::IStreamObject<Point> in;
auto r = in.feed(wire.data(), 1);                      // first byte…
for (size_t i = 1; i < wire.size(); ++i)
    r = in.feed(wire.data() + i, 1);                   // …then the rest as it arrives
if (r.complete()) { Point got = *in; }                 // got.x == 3, got.y == 4
```

#### Sequence framing: an all-default sub-message is omitted

MESSAGE_SPEC §2 **omits** a sequence-typed *field* whose value equals its
declared default, while a wrapper-array *element* **keeps** its frame even when
all-default — element presence is what carries a dynamic array's length (§5.1).
Both depend on what the children turn out to be, but the sequence header has to
be on the wire before them, so the encoder holds the header back instead of
buffering the sub-message:

| call | effect |
|---|---|
| `sequenceBeginLazy(id)` | opens a scope and holds its header back; the open ids form a *pending run* |
| any field write | emits the whole pending run first, outermost header first, then the field |
| `sequenceEnd()` | got no content ⇒ **drop** the frame, header and end marker both; otherwise emit the end marker |
| `sequenceEndKeep()` | behaves like a write: emits the run **and** the end marker, so a contentless sequence still reaches the wire as `begin` + `end` |

```cpp
sofab::OStreamInline<64> os;
os.sequenceBeginLazy(1).sequenceEnd();                    // (nothing) — the field vanishes
os.sequenceBeginLazy(1).sequenceEndKeep();                // 0e 07     — begin + end kept
os.sequenceBeginLazy(1).write(2, 5u).sequenceEnd();       // 0e 10 05 07 — content ⇒ framed
```

The choice of closer is **static** — a property of the position in the schema,
not of the value — so it rides on the two message writes:

- `writeLazy(id, msg)` — the **field** form (`sequenceEnd`): a `struct`/`union`
  field, or an array wrapper. An all-default child encodes to *zero bytes*.
- `write(id, msg)` — the **element** form (`sequenceEndKeep`): a wrapper-array
  element, whose frame must survive. An all-default child encodes to `1e 07` at
  id 3.

Getting it wrong is asymmetric — a needless `sequenceEndKeep` costs one
non-canonical empty frame that every decoder normalizes away, a wrong
`sequenceEnd` silently changes an array's length — so `write` (keep) is the safe
default and `writeLazy` the deliberate one. Generated code picks per position.

There is no depth window: the pending run grows on demand up to `MAX_DEPTH`
(255), so the omission is canonical at every legal depth. The held-back ids are
encoder state, never buffer content, so a flush can never split a run and a tiny
output buffer yields byte-identical output. Beyond the run's inline depth the
ids spill to the heap (see [Memory handling](#memory-handling)); a failed
allocation there is reported, not fatal — the open is refused with
`Error::BufferFull` and `ok()` turns false.

## Memory handling

Buffer ownership is the defining trade-off of this port, and it is the inverse of
the C port's.

**Decode — in-place parsing over the caller's buffer.** When nothing is buffered
(the common case, a whole message handed in at once) the cursor walks straight
over the caller's contiguous `buf`, allocating and copying nothing.

- **A fed chunk is yours again the moment `feed()` returns.** Every destination
  owns what it receives: a `string` or `blob` is copied out before the call
  returns. There is deliberately **no** borrowing destination —
  `read(std::string_view&)` does not exist, and asking for one is a compile error
  that names the owning alternatives.
- Integer and float arrays decode into the caller-provided `span`/container, and
  `read(void* dst, size_t maxlen)` copies a blob out. The stream never allocates
  the destination.
- If a `feed()` chunk ends mid-field, only that trailing field is copied into an
  internal accumulator and re-parsed on the next `feed()`. That accumulator is
  the one piece of library-owned heap on the decode path, and
  `Limits::max_buffered_field` bounds it.

This inverts the C port's deferred-copy model, where `read()` binds an
address-stable destination that a later `feed()` fills. Here `read()` pulls the
value out immediately, so no destination has to stay stable across chunks and no
input buffer has to outlive the call it was passed to.

**Encode — writes into a caller-supplied, fixed-size buffer; flushes, never
grows.** The library **allocates no output buffer**: `OStreamView` writes into
memory the caller already owns, `OStream` adopts a `std::shared_ptr<uint8_t[]>`
handed to it, and `OStreamInline<N>` carries an `N`-byte `std::array` inside the
stream object. Sizing belongs to the layer that knows the schema — generated code
allocates `MAX_SIZE` and installs it without a sink, or installs a scratch buffer
with an appending sink when the schema is unbounded. None of the three grows: at
the buffer end the stream calls the flush callback with the filled bytes and
continues; **without** a callback a full buffer yields `Error::BufferFull`.

**`ok()` / `error()` is the verdict for the whole encode.** A write the format
cannot carry returns `Error::InvalidArgument` and emits nothing — an id above
`ID_MAX`, nesting past `MAX_DEPTH`, a non-UTF-8 `string` under the strict check,
an array past `ARRAY_MAX`, a payload past `FIXLEN_MAX`, or a negative length
handed to `write(id, const void*, int32_t)`. Every refusal is sticky and latches
on the stream, first failure wins, so generated code can issue its writes one at
a time, discard each `Result`, and check `os.ok()` once at the end. The stream
keeps working after a rejection: the next write encodes normally and the refused
field is simply absent.

**`MIN_OUTPUT_BUFFER` is `1`.** That is the smallest buffer this port accepts
**for streaming**, and it binds a buffer installed together with a flush sink:
`buflen - offset` must be at least that much, checked where the buffer is handed
over (a constructor or `OStream::setBuffer`), never partway through a message. A
rejected installation leaves the stream inert with `ok() == false`. A buffer
installed **without** a sink has no minimum at all, so a two-byte message still
encodes into a two-byte buffer. Every write funnels through a per-byte fallback
that flushes across the boundary, so nothing has to land contiguously and a
one-byte buffer streams a message of any size, byte-identical to the one-shot
output.

**Flush handover.** A sink either *copies* what it is handed — it returns without
installing anything, and the encoder resumes in the same buffer at offset 0 — or
it *takes* the buffer, in which case it **must** call `setBuffer` before
returning. The start offset belongs to the installation, not to the buffer, and
is consumed by the first flush that returns without one; re-installing is how a
sink re-arms header room in every packet. This port does **not** implement
pass-through of a `string`/`blob` run, so a sink is only ever handed the buffer
it installed.

**`flush()` without a sink is a no-op.** There is nowhere to drain to: it reports
the byte count and leaves the message where it is, readable through `data()` /
`bytesUsed()`, with the next write appending after it. Only a handover the
callback returned from moves the cursor back to offset 0. A stream starts over by
installing a buffer or by being constructed anew — `flush()` is not a reset.

The one allocation an encode can make is the held-back sequence run (see
[Sequence framing](#sequence-framing-an-all-default-sub-message-is-omitted)).
The first eight levels of nesting live inside the stream object, costing it 64
bytes of state, so the ordinary encode allocates nothing; only a deeper run
spills to the heap, bounded by `MAX_DEPTH` (255 ids, ~1 KiB). With exceptions
enabled the `bad_alloc` is caught and `sequenceBeginLazy` returns
`Error::BufferFull`; under `-fno-exceptions` the failure terminates the process,
as any other allocation in such a build does.

### Heap-free destinations

The stream never allocates the *destination* — but `std::string` and
`std::vector` do, for their own storage. Where a schema bounds a field, three
containers hold the value inline instead, and the typed reads accept them
alongside the growable ones:

| Field | growable | heap-free |
|---|---|---|
| `string`, `maxlen M` | `std::string` | `sofab::FixedString<M>` |
| `blob`, `maxlen M` | `std::vector<uint8_t>` | `sofab::FixedBytes<M>` |
| `array`, `count N` | `std::vector<T>` | `sofab::InlineVector<T, N>` |

```cpp
sofab::FixedString<24>                        name;   // maxlen 24
sofab::InlineVector<sofab::FixedString<8>, 4> tags;   // count 4, maxlen 8
sofab::InlineVector<Reading, 4>               rows;   // count 4, struct elements

case 1: is.readString(name, 24); break;               // same call either way
case 2: { sofab::StringSeq c{tags, 4, 8}; is.read(c); } break;
case 3: { sofab::MessageSeq<decltype(rows)> c; c.out = &rows; is.read(c); } break;
```

Call sites are spelled identically for both storage kinds, so switching a field's
storage is a change of member type and nothing else. Semantics are unchanged: a
payload past the declared bound is `INVALID` (§7.1) rather than truncated, and
one past the container's own capacity is refused as well — a decode never drops
what it cannot store. The two refusals stay in different categories:

| ceiling that was passed | outcome | why |
|---|---|---|
| a declared `maxlen` / `count` | `INVALID` | the schema makes the message malformed (§7.1) |
| the destination's capacity, nothing declared | `readArray`: `LimitExceeded`<br>`readString` / `readBlob`: `INVALID` | the bytes are well-formed — the same message decodes into a growable destination — so this is the receiver-side category of §6.2.1 |

A decode into fully bounded fields performs **no allocation at all**;
`test_roundtrip.cpp`'s `heapFreeStorage()` checks that against the `operator new`
counter. The three types are deliberately identical, in name and behaviour, to
`corelib-c-cpp`'s, so generated code for a bounded field is the same whichever
C++ corelib it targets.

## Build & test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

Give `--parallel` an explicit job count: bare `--parallel` defers to the native
build tool, and with Unix Makefiles that is `make -j` with no limit. Run both
`-DCMAKE_BUILD_TYPE=Debug` and `Release`, as CI does — a Release-only defect
(strict aliasing, UB the optimiser acts on, an uninitialised field) is invisible
to a Debug-only run.

Six suites run under CTest:

- **`test_roundtrip`** — encode/decode/nested/chunked/skip, the three-valued
  outcome, malformed input, the terminal latch, and §2 sequence framing.
- **`test_vectors`** — replays the shared `assets/test_vectors.json` for encode,
  decode and byte-at-a-time streaming.
- **`test_bench_tools`** — holds the benchmark tooling to BENCH_SPEC: the
  workload list, both output tables, and the cross-port parity sizes.
- **`test_ci_workflows`** — reads `.github/workflows/` and requires both build
  types, no untyped CMake configure, and `fail-fast: false` on a matrix.
- **`test_readme_structure`** — reads this README and requires the §9 section
  list and order, the §9.2 badge block, the §9.5 examples, the §6.4 and §9.6
  facts, the §6.1.1 name set, and that every in-document link resolves.
- **`test_vendored_provenance`** — re-hashes the vendored copies against the
  provenance table in `test/shared/README.md`, so a refresh without a re-pin
  goes red.

### Coverage and API docs

```sh
# line/branch coverage of the header (needs gcovr)
cmake -S . -B build -DSOFAB_ENABLE_COVERAGE=ON
cmake --build build --parallel "$(nproc)" && ctest --test-dir build
gcovr --root . --filter '^include/sofab/.*\.hpp$' --object-directory build --print-summary

# Doxygen HTML (needs doxygen + graphviz)
cmake -S . -B build -DSOFAB_ENABLE_DOXYGEN=ON
cmake --build build --target doc
```

## Benchmarks

Three tools run BENCH_SPEC's workloads — a 1000-element `u64` array, a `typical`
mixed message, an unbounded 1 MB `blob`, and a `composite` message that reaches
what the flat three do not (a wrapper array, multi-byte UTF-8, depth-3 nesting, a
field the encoder must omit, a two-byte field header) — so results compare
directly across languages.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
cmake --build build --target run_perf    # per-op cost (cycles/op + MB/s)
cmake --build build --target run_bench   # sustained throughput (MB/s)
bash bench/run_callgrind.sh              # Ir/op table (needs valgrind)
```

`perf` reads a hardware cycle counter (x86 TSC / AArch64 `cntvct_el0`) for a
machine-independent per-op cost; `bench` reports throughput;
`bench/run_callgrind.sh` runs each workload under Callgrind for a deterministic
instructions-per-operation figure, which is what the comparison below uses.

Five bytes of the `blob 1MB` message are metadata and a million are payload, so
its MB/s is the host's `memcpy` and its memory bandwidth — **read the one-shot
and streaming rows against each other, never either alone.** Their difference is
what the divisible-run path costs. `blob 1MB passthrough`, BENCH_SPEC's optional
row, is absent because this port implements no pass-through permission.

Measured throughput is not reproduced here — it belongs to the cross-language
benchmark arena, which runs every port on one host under one methodology. The
head-to-head instruction counts below stay, because they are the only place the
two C++ corelibs can be compared at all.

All three tools read **one** workload list — the table in `bench/bench.cpp`,
published by `bench --list` — so a workload cannot be renamed in one tool and go
stale in another. `bench <workload>` runs a single operation and exits, which is
the Callgrind single-shot mode:

```sh
build/bench/bench --list           # the workloads, one "name<TAB>label" per line
build/bench/bench encode_typical   # one operation, then exit (Callgrind mode)
```

### Choosing between the two C++ corelibs

SofaBuffers ships two C++ implementations of the same wire format, tuned for
opposite ends of the spectrum:

- **`corelib-cpp` (this library)** — pure C++20, no C backend. Optimised for
  **throughput** on desktop/server targets. Parses in place over the caller's
  buffer and pulls each value straight into its destination.
- **[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp)** — a C
  object API with a thin C++ wrapper. Optimised for **minimal code size and RAM**
  on bare-metal targets, using a deferred-copy model.

Both expose a compatible `sofab::OStream` / `sofab::IStreamObject` surface, so
porting between them is mostly mechanical.

| | `corelib-cpp` (this) | `corelib-c-cpp` |
|---|---|---|
| Primary goal | Maximum throughput | Minimum footprint |
| Implementation | Pure C++20, header-only | C core + C++ wrapper header |
| Decode model | In place over the caller's buffer, each value copied out before `feed()` returns | Deferred-copy into address-stable destinations |
| Feature gating | Always full format (no `#ifdef`) | `SOFAB_DISABLE_*` compile out unused wire features |
| Target | Desktop / server | Bare metal / embedded C and C++ |

#### Instruction counts (Callgrind)

Machine-independent instruction counts on identical workloads, all three
compiled at `-O3` (lower is better, **bold** is the row's winner):

| Workload | C | C++ wrapper | this (pure C++20) |
|---|--:|--:|--:|
| encode: u64 array (1000)   |    125 999 |    126 028 | **35 046** (−72 %) |
| encode: typical message    |        966 |      1 063 | **226** (−77 %) |
| encode: blob 1MB one-shot  | 10 000 162 | 10 000 191 | **1 000 026** (−90 %) |
| encode: blob 1MB streaming | **10 004 819** | 10 009 790 | 13 009 127 (+30 %) |
| encode: composite          |     16 164 |     16 501 | **11 514** (−29 %) |
| decode: u64 array (1000)   |    300 432 |    300 433 | **43 839** (−85 %) |
| decode: typical message    |      2 109 |      2 108 | **1 275** (−40 %) |
| decode: blob 1MB           | 25 011 323 | 25 011 327 | **3 654 639** (−85 %) |
| decode: composite          |     32 168 |     36 533 | **22 417** (−30 %) |
| decode: composite skip-all |     25 411 |     25 411 | **7 671** (−70 %) |

Percentages are against the C column. The last column is this tree, reproduced
with `bash bench/run_callgrind.sh`; the two C columns are the current reading
[published by `corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp#what-the-speed-difference-actually-is),
taken on its own machine with the same script. Ir/op is what makes mixing the two
runs legitimate — it depends on the executed code, not on the host clock.

Two pairs are worth reading against each other. The `blob 1MB` pair costs this
library **13× more instructions** to put the same megabyte through a 4096-byte
buffer with a flush sink than to write it contiguously: `pushBytes` falls back to
a byte-at-a-time loop the moment the run does not fit the buffer, where the
one-shot row takes the `memcpy` branch. And `decode: composite skip-all` costs
**7 671** against **22 417** to decode it, so about two-thirds of a decode is the
destinations, not the parse.

**`encode: blob 1MB streaming` is the one row this library loses.** The C core has
no fast path to fall out of — it pushes every payload byte through one
bounds-checked path, so streaming costs it what the one-shot write cost — while
this library pays 13× its own one-shot figure once the run stops fitting.
Optimising for the buffer that holds the whole message does not pay when the
buffer deliberately cannot. Closing the gap means a bounded `memcpy` per flush
window instead of the byte loop.

On the nine rows it wins, the pure-C++20 port is **1.4× to 10× cheaper** because
it fuses header+value writes, composes fields straight into the buffer, and
parses in place without the C port's per-field bookkeeping. The narrowest margins
are the `composite` rows, where the message is mostly small varlen fields and
per-field work dominates; the widest are the bulk ones, whose varint runs
establish the ten-byte window once and then move whole 64-bit words. What the C
core buys with those instructions is its own contract — deferred-copy into
caller-owned, address-stable storage with no heap and a fixed footprint — so most
of the gap is a deliberate trade, not a defect on either side.

**Rule of thumb:** reach for **`corelib-cpp`** for desktop/server throughput, and
for **`corelib-c-cpp`** when you need a strictly minimal binary and tight RAM on
a footprint-constrained target.
