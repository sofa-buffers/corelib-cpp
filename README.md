<p align="center"><img src="assets/sofabuffers_logo.png" alt="SofaBuffers" height="140"></p>

# SofaBuffers

<b>Structured Objects For Anyone</b><br>
<i>... so optimized, feels amazing.</i>

[Would you like to know more?](https://github.com/sofa-buffers)

## SofaBuffers C++ library

[![CI](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml/badge.svg?branch=main)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml)
[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/sofa-buffers/corelib-cpp/badges/coverage-cpp.json)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/coverage.yaml)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://sofa-buffers.github.io/corelib-cpp/)

[GitHub repository](https://github.com/sofa-buffers/corelib-cpp)

A **streaming**, **dependency-free**, pure-**C++20** implementation of the
SofaBuffers (*Sofab*) serialization format, written from scratch with no C
backend. It packs structured fields into a caller-owned buffer and decodes them
with a protobuf-style cursor that advances over the message.

It presents the same `sofab::OStream` / `sofab::OStreamInline` / `sofab::IStreamObject`
surface as the footprint-oriented C/C++ corelib
([`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp)), but shares no
code with it and is tuned for raw throughput. See
[Choosing between the two C++ corelibs](#choosing-between-the-two-c-corelibs)
for when to pick which.

### Requirements

- A **C++20** compiler — GCC 11+, Clang 14+, or MSVC 19.30+.
- CMake **3.10+** to build the tests, benchmarks and docs. The library itself is
  header-only and needs no build step.

### Dependencies

**None** beyond the C++ standard library. The single header pulls in only
standard headers (`<array>`, `<bit>`, `<concepts>`, `<span>`, `<string>`,
`<string_view>`, `<memory>`, `<functional>`, …). No third-party dependencies, no
C backend.

### Built with the following compilers

Non-native targets are built and run under [QEMU](https://www.qemu.org/)
user-mode emulation in CI, reproducible locally without the real hardware. Both
x86_64 legs build and run the full suite twice — in `Debug` **and** in `Release`
(`-O3 -DNDEBUG`) — so a defect only the optimiser exposes cannot ship green. The
**CI** badge at the top of this section tracks the first row; the table is the
whole matrix.

| Target | Status |
| - | - |
| GCC x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-x86_64.yaml) |
| Clang x86_64 (little endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-clang-x86_64.yaml) |
| GCC ppc64 (big endian) | [![badge](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml/badge.svg)](https://github.com/sofa-buffers/corelib-cpp/actions/workflows/build-gcc-ppc64-bigendian.yaml) |

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

The Conan package `sofa-buffers-corelib-cpp`
([`conanfile.py`](conanfile.py)) installs a CMake package config exposing the
same target:

```cmake
find_package(sofa-buffers-corelib-cpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE sofa-buffers::corelib)
```

## Why this design

The C corelib (`corelib-c-cpp`) optimises for **minimal code size and RAM** (it
targets bare metal). This library makes the opposite trade: size and memory are
not a concern, the goal is **throughput**, and the decoder is tuned for the case
where the whole message is already in contiguous memory.

| Goal | How |
|------|-----|
| Fast encode | Payloads written with a single `memcpy`; a field's header + value varints emitted as one write; whole float arrays copied in one shot on little-endian. |
| Fast decode | The *parse* allocates nothing — the cursor walks the caller's buffer in place, and only an incomplete trailing field is ever buffered; float arrays are bulk-`memcpy`'d; a `string`/`blob` payload is copied straight into the destination, with no intermediate representation. |
| Still streamable | `OStream`/`OStreamInline` flush a small buffer via callback; `feed()` dispatches each complete top-level field and buffers only an incomplete tail. |
| Modern C++ | `std::span`, `std::bit_cast`, concepts, `if constexpr` `write()`/`read()` deduction, `[[nodiscard]]`. Little-endian handled explicitly. |

## Usage

The codec has four use cases — serialize a message that fits in one buffer,
serialize one too large for the buffer (streamed out in chunks), deserialize a
whole message, and deserialize one arriving in chunks — plus the generated-code
path that wraps them.

### Serialize

Write fields into a stack buffer big enough to hold the whole message and take a
view of the bytes. Every `write()` returns a chainable `Result` that latches the
first error, so you write fluently and check once at the end.

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
capacity and the wire count `M` is the array's *length*, so `{1, 2, 3, 0, 0}`
encodes as `M = 5` — narrowing it to its non-default prefix would send a
different value (MESSAGE_SPEC §3, shared vector
`array_unsigned_trailing_defaults`). Up to 0.10.0 the header also shipped a
`sofab::trimTail` helper that performed exactly that narrowing; it implemented a
superseded reading of §3 and has been **removed**. Neither this library nor
generated code ever called it, so no call site needs porting — a caller that used
it directly passes the container itself instead.

### Serialize stream

When the message is larger than the buffer, give the stream a flush callback: the
buffer becomes a small reusable window, drained whenever it fills (and once at the
end), so the encoder never holds the whole message in memory.

```cpp
#include "sofab/sofab.hpp"

std::vector<uint8_t> out;
sofab::OStreamInline<16> os(          // 16-byte window; drained each time it fills
    [&](std::span<const uint8_t> chunk){ out.insert(out.end(), chunk.begin(), chunk.end()); });

for (uint32_t i = 0; i < 1000; i++)
    os.write(sofab::id(i), uint64_t(i));
os.flush();                           // push the tail; `out` holds the whole message
```

### Deserialize

Derive a message from `IStreamMessage` and dispatch fields in `deserialize()`;
`IStreamObject` wires the decoder to an embedded instance. Fields you don't
`read()` are measured and skipped automatically.

```cpp
#include "sofab/sofab.hpp"

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

A decoder cannot see a message boundary: a message has no framing on the wire, and
since MESSAGE_SPEC §2 an all-default message is the *empty byte string*. Successive
`feed()` calls therefore continue **one** message. To decode a second message into
the same object, call `reset()` — it re-initialises the wrapped message **and** the
decoder (reassembly buffer, sticky flags, the latched terminal verdict,
`skipped()` counter) together:

```cpp
sofab::IStreamObject<Sensor> in;
in.feed(a.data(), a.size());          // message A
in.reset();                           // ← required
in.feed(b.data(), b.size());          // message B, on a clean destination
```

Without it, every field that message B does **not** carry keeps message A's value.
That is obvious for a scalar (§2 omits a default-valued field, so nothing is
delivered for it), and it is equally true of a **wrapper-array** field: its
collector (`StringSeq` / `BlobSeq` / `MessageSeq`) clears the destination in
`prepare()`, which runs only when the wrapper sequence is actually *present*. An
absent array field reaches no collector at all, so it would keep the previous
decode's elements instead of reading as the empty array §2 requires. MESSAGE_SPEC
§5.1 places this duty on the decoding side — *"supplying a cleanly initialised
destination is the application's responsibility"* — and `reset()` is how you
discharge it. (`IStreamInline` has the same `reset()`, but it owns no destination:
a callback-driven decoder clears its own targets.)

### Deserialize stream

`feed()` can be called repeatedly with whatever bytes have arrived; a field that
straddles a chunk boundary is buffered internally and re-parsed once its remainder
arrives, so a chunked stream decodes identically to a one-shot buffer — no matter
where the chunks come from.

```cpp
sofab::IStreamObject<Sensor> in;
for (uint8_t b : wire)                // feed whatever arrives — here one byte at a time
    in.feed(&b, 1);
// (*in) is fully populated
```

Each `feed()` returns a three-valued decode outcome (spec §7) — there is **no**
separate `finish`/`finalize` step, and the same three results apply to a one-shot
buffer and to chunked streaming:

| `Result`                | `code()`                 | `status()`                    | meaning |
|-------------------------|--------------------------|-------------------------------|---------|
| `complete()` / `ok()`   | `Error::None`            | `DecodeStatus::Complete`      | the consumed bytes end **exactly** at a field boundary — a valid message |
| `incomplete()`          | `Error::Incomplete`      | `DecodeStatus::Incomplete`    | the bytes end **inside** a field (a partial varint, a short fixlen/array payload) or with an open sequence; the partial tail is retained for the next `feed()` |
| `invalid()`             | `Error::InvalidMessage`  | `DecodeStatus::Invalid`       | the bytes are malformed **regardless of what follows** (varint over 64 bits, bad subtype/length, count/id over max, nesting past `MAX_DEPTH`, dangling sequence-end, …) |

`Incomplete` is **not** an error — it means "the message may continue": a streaming
caller reads it as "feed me more bytes", while a caller that has delivered all its
bytes and still sees `Incomplete` knows the message was truncated. A truncated tail
is therefore never silently accepted as `Complete`, nor rejected as `Invalid`.

`Invalid` — and the `LimitExceeded` policy code below — are **terminal**, and the
decoder latches them on the *stream*. Spec §5.2 answers "can more bytes change it?"
with *"no — terminal"* for `INVALID`, and §6.3 calls `LimitExceeded` *"a terminal,
receiver-local policy rejection"*. So once a `feed()` has returned either code,
every later `feed()` returns the same code immediately: no further bytes are
parsed, no field is delivered, nothing is buffered. `reset()` is the way back —
which is the documented start of a new message anyway.

```cpp
sofab::IStreamObject<Sensor> in;
in.feed(garbage.data(), garbage.size());  // Invalid
in.feed(wire.data(), wire.size());        // still Invalid — the stream is condemned
in.reset();                               // ← the only way back
in.feed(wire.data(), wire.size());        // Complete
```

That is what makes the outcome **chunk-independent** (spec §7.2 item 4): the same
bytes fed whole, in odd-sized chunks or one at a time all end on the same verdict.
Without the latch it would depend on where the chunk boundaries happened to fall,
and a sender could prefix garbage to a valid message and still have the receiver
report `Complete`.

#### Streaming buffer limit (opt-in)

A never-completing or huge trailing field would otherwise grow the internal
reassembly buffer without bound (a field may claim up to `FIXLEN_MAX`/`ARRAY_MAX`
≈ 2 GB). To cap that, pass a `sofab::Limits` to the stream constructor:

```cpp
sofab::IStreamObject<Sensor> in{ sofab::Limits{ .max_buffered_field = 64 * 1024 } };
```

`max_buffered_field` bounds how large a *single* incomplete top-level field may
grow the buffer. A field whose declared size exceeds it fails `feed()` with
`Error::LimitExceeded` the moment the size is known — before the payload is
buffered, so an oversized header is rejected even if its bytes never arrive. The
check is **chunk-independent**: the same field is rejected whether fed whole or
byte by byte. This is a receiver-side **policy** code, kept distinct from
`invalid()` / `Error::InvalidMessage` — exceeding a local limit is not wire
malformation. The default is **no cap** (`SIZE_MAX`), so streams are unbounded
unless you opt in. Bytes are never clamped or truncated — the `feed()` simply fails.

**A schema bound outranks the cap.** Spec §6.2.1 forbids applying a receiver-side
limit "to a field the schema already bounds", and §6.3 says `LimitExceeded` is
"never raised for a field the schema bounds — there an over-bound value is
`InvalidMessage`". So a declared length past a `maxlen`, or a count past a
`count:`, is `Error::InvalidMessage` **whatever** the cap is set to: the same
bytes cannot decode as a *validity* failure on one receiver and a *capacity*
refusal on another merely because the second sized its buffer smaller. To make
that work, an over-cap field is still delivered to its deliver callback — that is
the only place the declared `maxlen`/`count` is known — but with its **payload
withheld**: `readString`/`readArray`/… settle the wire type (§7.3) and the schema
bound (§7.1) as usual and then report `Incomplete` instead of materialising
anything, so a field the cap refuses copies no bytes, resizes no destination and
allocates nothing. Only a field the schema leaves *unbounded* ends in
`LimitExceeded`.

#### Strict UTF-8 validation (`SOFAB_STRICT_UTF8`, default ON)

A SofaBuffers `string` carries UTF-8 text; `blob` is the type for opaque bytes
(spec MESSAGE_SPEC §8, CORELIB_PLAN §6.4). With the compile-time flag
`SOFAB_STRICT_UTF8` **ON** (the default) an invalid-UTF-8 `string` is rejected
**symmetrically**:

- **encode** — `write(id, string_view)` for a non-UTF-8 value returns
  `Error::InvalidArgument` and emits nothing;
- **decode** — a materialised (`read` / `readString` into `std::string` or
  `sofab::FixedString<N>`)
  `string` whose complete payload is not valid UTF-8 is the `INVALID` outcome
  (`Error::InvalidMessage` / `DecodeStatus::Invalid`), surfaced through the same
  sticky-error channel as `invalidate()`.

The validator (`sofab::utf8_valid(std::string_view)`) is a real validator, not a
byte-range shortcut: it rejects overlong forms (including `C0 80`), UTF-16
surrogates `U+D800`–`U+DFFF`, and code points above `U+10FFFF`, and accepts an
embedded `U+0000`. `blob` is **never** validated in either direction, and a
**skipped** `string` is never validated — only a materialised read is.

Because there is no runtime encode-side configuration object, the knob is a
compile-time `#define` (the option §6.4 permits for C++), so it gates encode and
decode with one symmetric switch. Define `SOFAB_STRICT_UTF8=0` before including
`<sofab/sofab.hpp>` for a documented **non-strict** build: the validation code
folds away entirely and payloads are stored **verbatim** — raw, never lossy. The
option is a validation *policy*, never a wire-format switch, so peers with
different settings interoperate on all valid data; conformance and the shared
vectors run with it **ON**.

`SOFAB_STRICT_UTF8` is also the **only** compile-time knob this library has: the
header always compiles the full wire format, with no `#ifdef` gating of wire
types and nothing else to configure. (The conformance harness recognises the
family's `SOFAB_DISABLE_*` names only so it can skip the matching vectors when
validating a feature-reduced profile; the defines do **not** change this
library.) For a strictly minimal binary, use the C corelib
[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp), which does gate
wire features.

### Code generator

The usual way to drive the library is through **generated object code**: a schema
compiled by `sofabgen` emits a struct per message deriving `OStreamMessage` /
`IStreamMessage`, with `serialize` / `deserialize` bodies, a `_maxSize` bound,
`encode()` / `decode()` helpers, and a `try_decode()` that surfaces the
three-valued decode `Result` instead of assuming success. A hand-written
stand-in, encoded then decoded:

```cpp
#include "sofab/sofab.hpp"

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

`sofab::OStreamObject<Point>` is the encode-side counterpart of the
`IStreamObject` used above: it bundles a `Point` with an inline buffer of
`Point::_maxSize` bytes, reaches the message through `operator->` — as
`IStreamObject` hands back the decoded one through `operator*` — and encodes it
in a single `serialize()` call, so neither the stream nor the buffer has to be
managed separately.

```cpp
sofab::OStreamObject<Point> out;
out->x = 3; out->y = 4;
out.serialize();          // out.data() / out.bytesUsed() now hold the message
```

Constructed with a flush callback it streams instead, and `serialize()` hands
the bytes to that sink; constructed without one — as above — there is nowhere to
drain to, so the message simply stays in the buffer (see
[Memory handling](#memory-handling)). A third template argument reserves a head
of leading bytes in front of the message, exactly like `OStreamInline`'s.

Messages nest: passing a message deriving `OStreamMessage` to `write(id, msg)`
encodes it as a sub-sequence, and `is.read(childMsg)` descends into it on decode.

#### Sequence framing: an all-default sub-message is omitted

MESSAGE_SPEC §2 **omits** a sequence-typed *field* whose value equals its
declared default, while a wrapper-array *element* **keeps** its frame even when
all-default — element presence is what carries a dynamic array's length (§5.1).
Both verdicts depend on what the children turn out to be, but the sequence header
has to be on the wire before them, so the encoder **holds the header back**
instead of buffering the sub-message:

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

The choice of closer is **static** — a property of the position in the schema, not
of the value — so it rides on the two message writes:

- `writeLazy(id, msg)` — the **field** form (`sequenceEnd`): a `struct`/`union`
  field, or an array wrapper. An all-default child encodes to *zero bytes*.
- `write(id, msg)` — the **element** form (`sequenceEndKeep`): a wrapper-array
  element, whose frame must survive. An all-default child encodes to `1e 07` at
  id 3.

Getting it wrong is asymmetric — a needless `sequenceEndKeep` costs one
non-canonical empty frame that every decoder normalizes away, a wrong
`sequenceEnd` silently changes an array's length — so `write` (keep) is the safe
default and `writeLazy` the deliberate one. Generated code picks per position.

There is **no depth window**: the pending run grows on demand up to `MAX_DEPTH`
(255), so the omission is canonical at every legal nesting depth
(CORELIB_PLAN §6). The held-back ids are encoder state, never buffer content, so
a flush can never split a run and a tiny output buffer yields byte-identical
output. Decoding is unaffected: an empty frame remains legal input and decodes to
the same value as an omitted one.

**What it costs.** The hold-back is not free, and the two prices are measured,
not estimated:

- **Instructions.** The cost falls entirely on the shared `encode: typical
  message` workload, the one that nests a sub-message: **353 Ir/op against 285**
  for the pre-§2 encoder that framed eagerly — **+68 Ir/op, +24 %**. The other
  three workloads were unchanged (`encode: u64 array` 106 972 vs 107 961; both
  decode figures identical). Those absolutes are as measured when the hold-back
  landed; the varint fast paths have since moved every workload, so compare them
  with each other rather than with the current
  [Instruction counts](#instruction-counts-callgrind).
- **State.** The run lives in the stream object, so every stream grew by **64
  bytes**: `sizeof(OStreamInline<64>)` 144 → 208, `sizeof(OStreamView)` 80 → 144,
  `sizeof(OStream)` 96 → 160. That is per stream, not per message — it does not
  scale with what is encoded.

Beyond the run's inline depth the ids spill to the heap (see
[Memory handling](#memory-handling)); a failed allocation there is **reported,
not fatal** — the open is refused with `Error::BufferFull` and `ok()` turns
false.

The same generated struct also streams — no whole-message buffer on either side.
Its `serialize` targets any output stream, so a small flushing window works, and
an `IStreamObject` accepts the wire bytes in arbitrary chunks:

```cpp
// encode: stream through a 16-byte window instead of a whole-message buffer
std::vector<uint8_t> wire;
sofab::OStreamInline<16> os(
    [&](std::span<const uint8_t> chunk){ wire.insert(wire.end(), chunk.begin(), chunk.end()); });
pt.serialize(os);
os.flush();

// decode: feed whatever arrives; poll the value once feed() reports complete()
sofab::IStreamObject<Point> in;
auto r = in.feed(wire.data(), 1);                          // first byte…
for (size_t i = 1; i < wire.size(); ++i)
    r = in.feed(wire.data() + i, 1);                       // …then the rest as it arrives
if (r.complete()) { Point got = *in; }                     // got.x == 3, got.y == 4
```

## Memory handling

Buffer ownership is the defining trade-off of the C++ port, and it is the inverse
of the C (`corelib-c-cpp`) port.

**Decode (`feed` / `IStream`) — in-place parsing over the caller's buffer.**
`feed()` parses *in place*: when nothing is buffered (the common case, a whole
message handed in at once) the cursor walks straight over the caller's contiguous
`buf`, allocating and copying nothing.

- **A fed chunk is yours again the moment `feed()` returns.** Every destination
  owns what it receives: a `string` or `blob` is copied out before the call
  returns, so you may reuse, overwrite or free the chunk immediately and the
  decoded message is unaffected. There is deliberately **no** borrowing
  destination — `read(std::string_view&)` does not exist, and asking for one is a
  compile error that names the owning alternatives.
- Integer/float arrays decode into the caller-provided `span`/container (float
  arrays via a single `memcpy` on little-endian); `read(void* dst, size_t maxlen)`
  copies a blob out. The stream never allocates the destination.
- `read(void* dst, size_t maxlen)` **declares a `blob`**, the way every typed read
  declares its type: it compares the whole wire tag first, so a field that is not a
  `blob` — an integer, an array, a sequence, an `fp32`, a `string` — is left for the
  decoder to skip (§7.3), `dst` is untouched and the call returns 0. A zero-length
  blob also copies 0 bytes, so `consumed()` is what tells the two apart.
- `read(std::string&)` declares a fixlen **payload**, which is the pair `string`
  *and* `blob` — a `std::string` owns either one verbatim, and `readString()` /
  `readBlob()` are how you narrow the declaration to exactly one subtype. An
  `fp32` or `fp64` field shares `Wire::Fixlen` with them but is a *value*, not a
  payload, so it is left for the decoder to skip (§7.3): the string keeps what it
  held instead of receiving the float's four or eight raw bytes as text — which
  the strict-UTF-8 check would not have looked at either, since it applies to the
  `string` subtype only.
- If a `feed()` chunk ends mid-field, only that trailing field is copied into an
  internal accumulator and re-parsed on the next `feed()`. That accumulator is the
  one piece of library-owned heap on the decode path, and
  `Limits::max_buffered_field` is what bounds it.

This inverts the C port's deferred-copy model (where `read()` binds an
address-stable destination that a later `feed()` fills). Here `read()` pulls the
value out immediately, so no per-field destination has to stay stable across
chunks — and no input buffer has to outlive the call it was passed to.

**Encode (`OStream` / `OStreamView` / `OStreamInline`) — writes into a
caller-supplied, fixed-size buffer; flushes, never grows.** The library
**allocates no output buffer**: `OStreamView` writes into memory the caller
already owns, `OStream` adopts a `std::shared_ptr<uint8_t[]>` the caller hands
it, and `OStreamInline<N>` carries an `N`-byte `std::array` inside the stream
object itself (so the *buffer* costs no heap). Sizing the storage belongs to the
layer that knows the schema — generated code allocates `MAX_SIZE` and installs it
without a sink, or installs a scratch buffer with an appending sink when the
schema is unbounded. None of the three grows: at the buffer end the stream calls
the flush callback with the filled bytes and continues; **without** a callback a
full buffer yields `Error::BufferFull`.

**A rejected argument never reaches the buffer.** A write the format cannot carry
returns `Error::InvalidArgument` and emits *nothing* — a field ID above `ID_MAX`,
nesting past `MAX_DEPTH`, a non-UTF-8 `string` under the strict check (above),
an array of more than `ARRAY_MAX` elements or a payload longer than `FIXLEN_MAX`,
and a **negative** length handed to the raw-blob overload
`write(id, const void*, int32_t)`. That length is signed for symmetry with the
generated accessors, but CORELIB_PLAN §6.2 bounds a fixlen payload to
`0 .. 2,147,483,647`, so the sign is checked *before* the value is widened: the
encoder never turns `-1` into a huge unsigned length and never reads past the
object you handed it. The stream keeps working after such a rejection: the next
write encodes normally, and the refused field is simply absent from the wire.

`ARRAY_MAX` and `FIXLEN_MAX` are the same §6.2 ceilings the **decoder** enforces
on a count or length word, and they bind in both directions: a count above
`ARRAY_MAX`, or a declared length above `FIXLEN_MAX`, is `INVALID` for every
decoder (§5.2) — this library's own included — so writing one would produce bytes
nothing can read. Both are checked before any byte of the field is composed, so
an over-long argument costs one comparison, is never partially written, never
commits a
[held-back sequence run](#sequence-framing-an-all-default-sub-message-is-omitted),
and — with a flush sink installed, where no `BufferFull` would ever stop the
element loop — never streams a 2 GB body out behind a header the receiver has to
reject.

**`ok()` / `error()` is the verdict for the whole encode.** Every refusal above
is sticky, exactly as `BufferFull` is: the code comes back in that call's
`Result` *and* latches on the stream, first failure wins. That is what makes the
verdict usable from generated code, which issues its writes one at a time and
discards each `Result` — check `os.ok()` once, after the last write, and it is
false if *anything* the encoder was asked to write did not reach the output
(CORELIB_PLAN §5.1: an encoder must not hand on partial output as if it were
complete, and a helper that ignores the report is non-conformant). `error()`
names the first thing that went wrong: `Error::BufferFull` for an overflow with
no sink, `Error::InvalidArgument` for a rejected buffer installation or for a
field the format cannot carry.

**`MIN_OUTPUT_BUFFER` is `1`.** That is the smallest buffer this port accepts
**for streaming**, and it binds a buffer installed **together with a flush sink**
— `buflen - offset` must be at least that much, checked where the buffer is
handed over (a constructor or `OStream::setBuffer`), never partway through a
message. A rejected installation leaves the stream inert with `ok() == false` and
`error() == Error::InvalidArgument`. A buffer installed **without** a sink is
subject to no minimum at all: no flush can occur, so a two-byte message still
encodes into a two-byte buffer. Every write funnels through a per-byte fallback
that flushes across the boundary, so no write has to land contiguously and a
one-byte buffer streams a message of any size — byte-identical to the one-shot
output.

**Flush handover.** A sink either *copies* what it is handed — it returns without
installing anything, and the encoder resumes in the same buffer at offset 0 — or
it *takes* the buffer, in which case it **must** call `setBuffer` before
returning. The start offset belongs to the *installation*, not to the buffer, and
is consumed by the first flush that returns without one; re-installing is how a
sink re-arms header room in every packet. This port does **not** implement
pass-through of a `string`/`blob` run, so a sink is only ever handed the buffer it
installed — never memory belonging to the caller's payload.

**`flush()` without a sink is a no-op.** There is nowhere to drain to, so it
touches neither the cursor nor the reserved head: it reports the byte count and
leaves the encoded message exactly where it is, readable through `data()` /
`bytesUsed()`, with the next write appending after it. Only a *handover the
callback returned from* moves the cursor back to offset 0, and without a callback
none happens. That is what makes the one-shot path safe — `OStreamObject::serialize()`
flushes internally, and a default-constructed one has no sink — and it is why
`flush()` is not a way to reuse a buffer: there is no reset operation, and a
stream starts over by installing a buffer (`OStream::setBuffer`) or by being
constructed anew.

The one allocation an encoder can still make is the held-back sequence run (see
[Sequence framing](#sequence-framing-an-all-default-sub-message-is-omitted)): the
list of open-but-unwritten sequence ids. The first eight levels of nesting live
**inside** the stream object (costing it 64 bytes of state), so the ordinary
encode allocates nothing; only a run nested deeper than that spills to the heap,
and it is bounded by `MAX_DEPTH` (255 ids, ~1 KiB) regardless. It holds encoder
state, never message bytes, so it never scales with the message.

That allocation is the **only** one an encode can fail on, and failing it is not
fatal: with exceptions enabled (the default) the `bad_alloc` is caught,
`sequenceBeginLazy` refuses to open the sequence and returns `Error::BufferFull`
with `ok()` false. In a build compiled `-fno-exceptions` there is nothing to
catch and the allocation failure terminates the process, exactly as any other
allocation in such a build does — nest below the inline depth if that matters.

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
sofab::FixedString<24>                       name;    // maxlen 24
sofab::InlineVector<sofab::FixedString<8>, 4> tags;   // count 4, maxlen 8

case 1: is.readString(name, 24); break;               // same call either way
case 2: { sofab::StringSeq c{tags, 4, 8}; is.read(c); } break;
```

The call sites are spelled identically for both storage kinds — `readString`,
`readBlob` and `readArray` pick the branch off the destination's own capabilities,
and the collectors deduce their container — so switching a field's storage is a
change of member type and nothing else. The wire is unaffected in both directions.

Semantics are unchanged too: a payload past the declared bound is `INVALID`
(§7.1) rather than truncated, and one past the container's own capacity is
refused as well — a decode never drops what it cannot store and never reports
`COMPLETE` with elements missing (MESSAGE_SPEC §3: a decoder materializes
*exactly* the `M` elements the wire carries, "the same value on a pre-sized
target and on a growable one"). The two refusals are terminal and leave the
destination untouched, but they stay in **different categories**, because they
say different things:

| ceiling that was passed | outcome | why |
|---|---|---|
| a declared `maxlen` / `count` | `INVALID` | the schema makes the message malformed (§7.1) |
| the destination's capacity, nothing declared | `readArray`: `LimitExceeded`<br>`readString` / `readBlob`: `INVALID` | the bytes are well-formed — the same message decodes into a growable destination — so an unbounded array reports the receiver-side category of CORELIB_PLAN §6.2.1, the one the configured `max_dyn_array_count` cap already uses |

The check keys on the destination *publishing* a capacity, as the table's
storage kinds do. A raw fixed-extent destination that publishes none — a plain
`std::array`, a bound `std::span` — is the low-level `read()` contract instead:
the leading elements land in it and the surplus is parsed only to keep the
framing.

A wire-type mismatch still skips the field and leaves the
destination untouched (§7.3); strict UTF-8 still rejects. What changes is only
that a decode into fully bounded fields performs **no allocation at all** —
`test_roundtrip.cpp`'s `heapFreeStorage()` checks that against the `operator new`
counter rather than asserting it.

These three types are deliberately identical, in name and behaviour, to
`corelib-c-cpp`'s, so generated code for a bounded field is the same whichever C++
corelib it targets.

## Build & test

```sh
cmake -S . -B build
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

Always give `--parallel` an explicit job count. Bare `--parallel` defers to the
native build tool's default, and with the Unix Makefiles generator that is
`make -j` with *no* limit — one compiler per translation unit, all at once.

Run it in both configurations — `-DCMAKE_BUILD_TYPE=Debug` and
`-DCMAKE_BUILD_TYPE=Release` — which is what CI does; a Release-only defect
(strict aliasing, UB the optimiser acts on, an uninitialised field) is invisible
to a Debug-only run.

Five suites run under CTest:

- **`test_roundtrip`** — encode/decode/nested/chunked/skip checks plus the
  three-valued decode outcome (§7: COMPLETE / INCOMPLETE / INVALID), malformed-input
  handling (truncated tails held as `Incomplete`; overlong varints, oversized
  lengths and stray markers rejected as `InvalidMessage`), the terminal latch
  (§5.2/§6.3: every chunking of a malformed stream ends on the same verdict, and
  valid bytes appended after it never revive the stream), resync after a
  skipped sub-sequence, and the §2 sequence framing above (an all-default
  sequence omitted, a kept element frame, hold-back at full `MAX_DEPTH`, and
  identical bytes when a run is committed across a flush boundary).
  One check in it is structural rather than byte-driven: the header decodes a
  field header `(id<<3)|type` in more than one place, and a copy that omits the
  §4.6/§4.8/§6.2 ceilings is a hole that feeding bytes cannot expose for as long
  as it stays unreachable — so `headerParsersEnforceCeilings()` also scans
  `include/sofab/sofab.hpp` itself (CMake bakes the path in) and requires every
  header-decode site to carry those checks.
  Four checks in it are *swept* rather than written once, because the code they
  cover is a template and one instantiation proves nothing about the next: the
  bulk array decoder is driven through the whole §7 matrix (tag, `count`, policy
  cap, destination capacity, element width, both element loops and their surplus
  halves) for every declared element type and destination shape; every message
  type the suite declares gets the `reset()` contract checked on its own
  instantiation; a set of real messages is fed at *every* split point and one
  byte at a time (§7.2 item 4 — the verdict may not depend on the chunking); and
  §6.4 is checked with the invalid UTF-8 sequence starting past the end of the
  first chunk, the case the shared `invalid_utf8` vectors do not reach.
- **`test_vectors`** — replays the shared `assets/test_vectors.json` conformance
  suite (copied verbatim from `corelib-c-cpp`, the authoritative source) for
  encode, decode, and byte-at-a-time chunked streaming. It asserts each vector's
  `serialized` column — the primitive-layer ground truth — and, replaying the
  same ops with the dropping closer, also its `serialized_sparse` column for the
  three vectors whose sparse form is *pure sequence omission* (the rest of that
  column encodes per-field defaults, which needs a schema and belongs to the
  **generator's** conformance drivers). For every other vector the same replay
  must reproduce `serialized` unchanged. The file is read and JSON-parsed
  **once** per run and every group in it (`vectors`, `invalid_utf8`) is walked
  off that one parse; since the envelope is generated upstream and has grown a
  top-level key before, each walker demands its own key and the run fails loudly
  when it is absent, renamed or empty, instead of quietly testing nothing.
- **`test_bench_tools`** — guards the benchmark tooling against BENCH_SPEC and
  against internal drift (see [Benchmarks](#benchmarks)): the published workloads
  are BENCH_SPEC's, all of them and none outside its output grammar; both printed
  tables match that grammar row for row; the cross-port parity sizes hold
  (`blob 1MB` 1,000,005, `composite` 956, `perf` 170); every workload
  `bench --list` publishes runs and reports its size; an unknown name is
  rejected; neither `run_callgrind.sh` nor `bench/CMakeLists.txt` keeps a second
  copy of the workload list; and `bench.cpp`/`perf.cpp` take the stream adapters
  and the timing harness from `bench_common.hpp` rather than redeclaring them.
  It also
  drives `run_callgrind.sh` end to end against a stub `valgrind`, so it needs no
  Valgrind and still proves a failing measurement aborts the table with the
  captured diagnostic. Skipped when the build did not produce the `bench` binary.
- **`test_ci_workflows`** — the one check aimed at CI rather than at the
  library: which configurations get built is a property of
  `.github/workflows/`, so no C++ test can notice when a leg disappears. It
  requires that the workflows running CTest cover both `Debug` and `Release`,
  that a testing workflow never configures CMake without a build type (the empty
  default is neither: no optimisation, no `NDEBUG`), and that a workflow using a
  strategy matrix sets `fail-fast: false`. `${{ matrix.build_type }}` is resolved
  against the workflow's own `build_type:` axis. Skipped in a source tree
  without `.github/workflows/`.
- **`test_readme_structure`** — the other document-level check: CORELIB_PLAN §9
  fixes this README's section list, their order and the badge block for the whole
  corelib family, and nothing in the library can notice that shape drifting. It
  reads `README.md` and requires the §9.1 header block, a §9.2 badge block
  carrying CI, coverage and Docs in that order, exactly the §9 top-level sections
  with no invented ones, no API-documentation section (§9.4), and the §9.8
  two-corelib comparison as a subsection of
  [Benchmarks](#benchmarks). Skipped in a tree without a README.
- **`test_vendored_provenance`** — the shared vectors and the JSON reader that
  loads them are verbatim copies of `corelib-c-cpp`, and `test_vectors` replays
  whatever bytes are on disk, pinned or not. What makes a re-sync checkable is
  the provenance table in `test/shared/README.md`: the merged upstream commit
  each copy came from and the `md5` it hashed to. This check reads that table
  and re-hashes the files, so a copy refreshed without re-pinning its row goes
  red rather than drifting silently; it also rejects a pin that is not a bare
  merged commit — a branch or an unmerged PR head leaves the next re-syncer with
  no SHA to diff against — and a vendored file with no row at all. Skipped in a
  tree without `test/shared/README.md`.

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

Three tools mirror the C / C++ / Rust / Go / Java / Python benchmarks on
identical workloads, so results are directly comparable across languages. The
workloads are BENCH_SPEC's four datasets — a 1000-element `u64` array, a
`typical` mixed message, an unbounded **1 MB `blob`**, and a `composite` message
that exercises what the flat three never reach (a wrapper array, multi-byte
UTF-8, depth-3 nesting, a field the encoder must omit, a two-byte field header):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
cmake --build build --target run_perf    # per-op cost (cycles/op + MB/s)
cmake --build build --target run_bench   # sustained throughput (MB/s)
```

`perf` reads a hardware cycle counter (x86 TSC / AArch64 `cntvct_el0`) for a
machine-independent cost figure and reports throughput in MB/s; `bench` reports
the throughput table. The third tool, `bench/run_callgrind.sh`, runs each
workload once under Callgrind and prints an instructions-per-operation (Ir/op)
table — deterministic and machine-independent, so the figures compare across
hosts and against the other language ports:

```sh
bash bench/run_callgrind.sh                       # Ir/op table (needs valgrind)
cmake --build build --target run_bench_callgrind  # the same script, driven from CMake
```

Those figures are the head-to-head data below.

On this machine (GCC 15, `-O3`, x86-64) the throughput table reads:

```
encode: u64 array (1000)         4295.75      decode: u64 array (1000)     3529.74
encode: typical message          2051.08      decode: typical message       377.27
encode: blob 1MB one-shot       42063.92      decode: blob 1MB            16696.61
encode: blob 1MB streaming        970.19      decode: composite             539.41
encode: composite                 922.58      decode: composite skip-all   1705.72
```

Two of those rows are not a statement about this library. Five bytes of the
`blob 1MB` message are metadata and a million are payload, so its MB/s is the
host's `memcpy` and its memory bandwidth — **read the one-shot and streaming
rows against each other, never either alone.** Their difference is what
CORELIB_PLAN §5.1's divisible-run path costs, and it is stark: 42.1 GB/s in one
contiguous write against 0.97 GB/s through a 4096-byte buffer with a flush sink,
**43×**. The instruction counts below put the same gap at **13×** with bandwidth
taken out of it — the encoder's `pushBytes` takes a `memcpy` when the run fits
the buffer and falls back to a byte-at-a-time loop when it does not, and a
megabyte through 4096-byte buffers is entirely the second case. Nothing else in
the suite reaches that path: the other three datasets are schema-bounded and
small enough that no flush happens mid-encode. `blob 1MB passthrough`,
BENCH_SPEC's optional third row, is absent because this port implements no
pass-through permission.

All three tools measure **one** list of workloads: the table in `bench/bench.cpp`,
published by `bench --list` as `name<TAB>label` lines and read from there by
`run_callgrind.sh` and the CMake target, so a workload cannot be renamed in one
tool and go stale in another. The stream adapters and the timing rules
(BENCH_SPEC's ~1 s CPU-time loop) likewise live once, in `bench/bench_common.hpp`,
included by both `bench` and `perf`. `bench <workload>` runs a single operation
and exits — the Callgrind single-shot mode; an unknown name is rejected, and a
run that yields no figure aborts the table instead of printing a `-`:

```sh
build/bench/bench --list           # the workloads, one "name<TAB>label" per line
build/bench/bench encode_typical   # one operation, then exit (Callgrind mode)
```

`ctest -R test_bench_tools` guards that sharing, and holds both printed tables
against BENCH_SPEC's output grammar and its two cross-port parity sizes
(`blob 1MB` = 1,000,005 bytes, `composite` = 956, as `perf`'s message is 170). It
drives the real loops over a token time budget via `SOFAB_BENCH_SECONDS`, which
exists for that check alone — nothing that publishes a number changes it, and
the default is BENCH_SPEC's ~1 s per workload.

### Choosing between the two C++ corelibs

SofaBuffers ships **two** C++ implementations of the same wire format, tuned for
opposite ends of the spectrum:

- **`corelib-cpp` (this library)** — pure C++20, no C backend. Optimised for
  **throughput** on desktop/server targets. Parses in place over the caller's
  buffer and pulls each value straight into its destination, so no per-field
  destination has to stay alive across chunks.
- **[`corelib-c-cpp`](https://github.com/sofa-buffers/corelib-c-cpp)** — a C
  object API with a thin C++ wrapper (`sofab.hpp`). Optimised for **minimal code
  size and RAM** on bare-metal / microcontroller targets, using a deferred-copy
  model (bind destinations, copy on `feed()`).

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

Machine-independent instruction counts from the shared benchmark tooling, this
library against the C corelib and its C++ wrapper on identical workloads (lower
is better, **bold** is the row's winner). All three are compiled at `-O3` so the
comparison is like-for-like, and BENCH_SPEC's full ten rows are now measured on
both sides:

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
taken on its own machine with the same script. Ir/op is what makes mixing the
two runs legitimate — it depends on the executed code and not on the host clock
or scheduler — which is also why it is the figure both ports quote.

Two pairs of rows are worth reading against each other rather than alone. The
`blob 1MB` pair costs this library **13× more instructions** to put the same
megabyte through a 4096-byte buffer with a flush sink than to write it
contiguously — 13 instructions per payload byte against one. That is the whole
price of CORELIB_PLAN §5.1's divisible-run path here, and it comes from
`pushBytes` falling back to a byte-at-a-time `pushByte` loop the moment the run
does not fit the buffer; the one-shot row takes the `memcpy` branch. Nothing else
in the suite reaches that fallback. `decode: composite skip-all` is the other:
walking the message and materialising nothing costs **7 671** against **22 417**
to decode it, so about two-thirds of a decode is the destinations, not the parse.

**`encode: blob 1MB streaming` is the one row this library loses**, and the two
readings are the same fact from opposite sides. The C core has no fast path to
fall out of — it pushes every payload byte through one bounds-checked path, so
streaming costs it what the one-shot write cost (+0.05 %) — while this library
pays 13× its own one-shot figure the moment the run stops fitting. Optimising for
the buffer that holds the whole message does not pay when the buffer deliberately
cannot, and the gap is not small: **1.3× more instructions** than the C core.
Closing it means a bounded `memcpy` per flush window instead of the byte loop.

The two message rows read 224 and 1199 before the suite grew to BENCH_SPEC's
full ten workloads, and the library did not change: GCC's inlining budget is
per-translation-unit, so a bigger `bench.cpp` was enough to move them (to 330 and
1355, in fact, before the entry points were marked `flatten`). `flatten` pins
each Callgrind toggle point to its own call tree, which is what makes a figure
comparable from one release to the next; the rows above then measure identically
whether the file holds four workloads or ten, at a couple of percent above the
absolute minimum an unconstrained GCC finds for a small file. That trade is
deliberate — a stable number is worth more here than a minimal one.

The encode figures include the §2 sequence hold-back — the price of never
framing an all-default sub-message, see
[Sequence framing](#sequence-framing-an-all-default-sub-message-is-omitted).
That cost was **+68 Ir/op (+24 %)** on `encode: typical message` when it landed;
the absolutes it was measured against (285 → 353) predate the varint fast paths
below, so they no longer match the table.

On the nine rows it wins, the pure-C++20 port is **1.4× to 10× cheaper** in
instructions because it fuses header+value writes, composes fields straight into
the buffer, and parses in place without the C port's per-field bookkeeping. The
narrowest margins are the `composite` rows (−29 % / −30 %), where the message is
mostly small varlen fields and per-field work dominates; the widest are the bulk
ones. What the C core buys with those instructions is its own contract —
deferred-copy into caller-owned, address-stable storage with no heap and a fixed
footprint — so most of the gap is a deliberate trade, not a defect on either
side. The array workloads pull
furthest ahead because their varint runs establish the ten-byte window once and
then move whole 64-bit words — eight varint bytes per store on encode, one load
plus a terminator scan on decode — instead of testing bounds and continuation a
byte at a time. Inside that word step the bit-spread is written as an add rather
than a mask-and-recombine: once the 7-bit groups are laid out, each round's two
halves partition the live bits, so `(w & keep) | ((w & move) << k)` is
`w + (2^k - 1) * (w & move)` and drops a mask and an OR per round.

On the message-shaped workloads the cost is dominated instead by what happens
*around* the bytes, so that is what the same figures made worth removing: the
nested-sequence dispatcher took its per-field callback as a `std::function`,
which cost a construction, a manager-driven destruction and two indirect hops on
every sub-message read; and the UTF-8 validator re-tested its word-skip
condition once per character, so a short ASCII payload paid four branches a byte.
Neither is visible on an array workload, and together they were a fifth of
`decode: typical message` and a quarter of `encode: typical message`.

In the multi-language arena it lands at a 434-byte wire size (vs protobuf's
494 bytes) and roughly **1.3× the throughput of protobuf** for a comparable C++
message. That arena figure predates the varint work above and has not been
re-run, so treat it as a floor rather than a current reading.

**Rule of thumb:** reach for **`corelib-cpp`** for desktop/server throughput, and
for **`corelib-c-cpp`** when you need a strictly minimal binary and tight RAM on a
footprint-constrained target.
