/*!
 * @file bench.cpp
 * @brief SofaBuffers pure-C++20 — throughput benchmark (MB/s, CPU time).
 *
 * Mirror of bench/c/bench.c, bench/cpp/bench.cpp and corelib-rs/benches/bench.rs:
 * identical workloads, data, field ids and values, driven through the pure-C++20
 * implementation (include/sofab/sofab.hpp — no C backend), so the figures are
 * directly comparable to the C corelib, its C++ wrapper and every other port.
 *
 * BENCH_SPEC's four datasets: a 1000-element `u64` array, a small mixed
 * `typical` message, an unbounded 1 MB `blob`, and the `composite` message that
 * exercises what the flat three never reach (a wrapper array, multi-byte UTF-8,
 * depth-3 nesting, an omitted all-default field, a two-byte field header).
 *
 * **Read the `blob 1MB` rows against each other, not against the others.** Five
 * bytes of that message are metadata and a million are payload, so its MB/s is
 * this machine's `memcpy` and its memory bandwidth, not a statement about the
 * corelib. The signal is the *difference* between the one-shot row (one
 * contiguous write, no sink) and the streaming row (the same bytes through ~245
 * flushes of a 4096-byte buffer): that difference is the cost of CORELIB_PLAN
 * §5.1's divisible-run path, and this is the only workload in the suite that
 * exercises it at all. Read it as Callgrind `Ir/op` (run_callgrind.sh), where
 * instruction counts do not care about bandwidth.
 *
 * Two modes:
 *   bench               -> timed MB/s table (default, CPU time).
 *   bench <workload>    -> run one operation once and exit; used by Callgrind
 *                          (run_callgrind.sh) to count instructions/op. The
 *                          run_<workload> functions are extern "C" + noinline so
 *                          --toggle-collect=run_<workload> matches the same
 *                          symbol names as the C / C++ benchmarks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bench_common.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#define N 1000

/*! `blob 1MB` payload length. The encoded message is BLOB_LEN + 5 bytes on every
 *  port — a 1-byte header `(1 << 3) | 2` and a 4-byte fixlen word
 *  `(1000000 << 3) | 3` — which BENCH_SPEC states as a cross-port parity check. */
#define BLOB_LEN 1000000

/*! Buffer the streaming `blob 1MB` row encodes through, and the chunk size its
 *  decode row is fed in. A fixed 4096 on every port so the rows stay comparable;
 *  `MIN_OUTPUT_BUFFER` does not enter into it (it is at most 20). */
#define BLOB_CHUNK 4096

namespace
{

using sofab_bench::IStreamRaw;
using sofab_bench::OStreamRaw;

/* shared buffers */
uint64_t src[N];
uint8_t  enc_u64_buf[N * 11 + 16];
size_t   enc_u64_used;

uint8_t  typ_buf[256];
size_t   typ_used;
const uint16_t arr16[4] = {10, 20, 30, 40};

/* `blob 1MB`: the payload, the one-shot output buffer, the 4096-byte streaming
 * scratch, and the decode destination. All static, so no allocation lands in a
 * measured run. */
uint8_t blob_src[BLOB_LEN];
uint8_t blob_enc[BLOB_LEN + 16];
size_t  blob_used;
uint8_t blob_scratch[BLOB_CHUNK];
uint8_t blob_dec[BLOB_LEN];
size_t  blob_stream_used; /* bytes the streaming sink was handed, in total */

/* `composite`: encoded message and the pieces of its two payload fields. */
uint8_t comp_buf[2048];
size_t  comp_used;

/*! One cycle of the `composite` string field: 1-, 2-, 3- and 4-byte UTF-8
 *  (`a`, `ä`, `€`, `𝄞`), spelled as bytes so no source encoding can change it.
 *  The literal is split at every escape boundary because a hex escape otherwise
 *  swallows the following hex digits. */
const char kCompCycle[] = "a" "\xc3\xa4" "\xe2\x82\xac" "\xf0\x9d\x84\x9e";
constexpr size_t kCompCycleLen = sizeof kCompCycle - 1; /* 10 bytes */
char comp_text[kCompCycleLen * 32]; /* 320 UTF-8 bytes, id 2 */

/* The 64 elements of the wrapper array (id 1), "item-0" .. "item-63".
 * Built once at startup: BENCH_SPEC's dataset is the *values*, and formatting
 * them inside the measured run would put snprintf in a figure that is supposed
 * to be the encoder. Longest is "item-63" (7 chars). */
char comp_items[64][8];
uint8_t comp_item_len[64];

/* decode targets */
uint64_t dec_array[N];
struct Targets
{
    uint32_t f1; int32_t f2; bool f3; float f4;
    std::string f5; uint16_t f6[4]; uint32_t s_f1; int32_t s_f2;
} T;

struct ChildMsg : sofab::IStreamMessage
{
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) sofab::read(s, T.s_f1);
        else if (i == 2) sofab::read(s, T.s_f2);
    }
};
ChildMsg childMsg;

/* ---- `composite` decode destinations --------------------------------------
 *
 * Heap-free throughout: an InlineVector of FixedString for the wrapper array, a
 * FixedString for the 320-byte payload. A std::vector<std::string> would put 64
 * frees and 64 allocations into every measured decode, which is the allocator's
 * figure and not the decoder's. */

struct CompOut
{
    sofab::InlineVector<sofab::FixedString<16>, 64> items; /* id 1 */
    sofab::FixedString<384> text;                          /* id 2 */
    uint32_t deep = 0;                                     /* id 3 → 1 → 1 → 1 */
    int32_t  sig = 0;                                      /* id 3 → 2 */
    uint32_t big = 0;                                      /* id 130 */
} C;

/* id 3 is nested three levels deep, so it needs three readers: the hold-back run
 * on the encode side has a matching descent on the decode side. */
struct CompLevel3 : sofab::IStreamMessage /* { 1: unsigned 7 } */
{
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) sofab::read(s, C.deep);
    }
} compL3;

struct CompLevel2 : sofab::IStreamMessage /* { 1: { … } } */
{
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) sofab::read(s, compL3);
    }
} compL2;

struct CompLevel1 : sofab::IStreamMessage /* { 1: { … }, 2: signed -1 } */
{
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) sofab::read(s, compL2);
        else if (i == 2) sofab::read(s, C.sig);
    }
} compL1;

/* The wrapper array's collector (MESSAGE_SPEC §5.1): one field header per
 * element, the element id is the array index. Declared count 64, element maxlen
 * 16 — the schema facts a generated reader would pass, and what sizes the
 * heap-free destination above. */
sofab::StringSeq<decltype(CompOut::items)> compItems{C.items, 64, 16};

/* The `blob 1MB` decode row is fed in chunks, so its stream lives across the
 * calls that make up one op — and across ops, deliberately. `reset()` is the
 * message-loop call (it starts a new message but keeps the reassembly buffer's
 * capacity), so a decoder that has run once does not re-acquire a megabyte for
 * the next message. Constructing a fresh stream per op instead would put that
 * megabyte of allocator traffic into every iteration and measure malloc. */
IStreamRaw blobStream;

void make_src()
{
    for (int i = 0; i < N; i++)
        src[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL;

    /* Same constant as the u64 array, low byte — one magic number in the suite,
     * and the same derivation in every port. */
    for (size_t i = 0; i < BLOB_LEN; i++)
        blob_src[i] = (uint8_t)((uint64_t)i * 0x9E3779B97F4A7C15ULL);

    for (size_t i = 0; i < sizeof comp_text; i++)
        comp_text[i] = kCompCycle[i % kCompCycleLen];

    for (int i = 0; i < 64; i++)
        comp_item_len[i] = (uint8_t)snprintf(comp_items[i], sizeof comp_items[i], "item-%d", i);

    blobStream.init([](sofab::id id, size_t, size_t) {
        if (id == 1) blobStream.read(blob_dec, sizeof blob_dec);
    });
}

void encode_typical(OStreamRaw &os)
{
    os.write(1, static_cast<uint32_t>(0xDEADBEEF));
    os.write(2, static_cast<int32_t>(-12345));
    os.write(3, true);
    os.write(4, 3.14159f);
    os.write(5, "sofab");
    os.write(6, std::span<const uint16_t>(arr16, 4));
    os.sequenceBeginLazy(7);
    os.write(1, static_cast<uint32_t>(99));
    os.write(2, static_cast<int32_t>(-7));
    os.sequenceEnd();
}

/*!
 * The `composite` message (BENCH_SPEC), field by field and each for a reason:
 *
 * - **id 1** — the suite's only wrapper array (MESSAGE_SPEC §5.1): 64 string
 *   elements carrying one field header each, element id = array index, so ids
 *   0–15 encode in one header byte and 16–63 in two.
 * - **id 2** — 320 UTF-8 bytes covering 1-, 2-, 3- and 4-byte sequences, which
 *   puts the §6.4 validator on a payload that is not ASCII (its word-skip fast
 *   path does not apply).
 * - **id 3** — nesting at depth 3, so the lazy hold-back run grows past the
 *   single level `typical` and `perf` reach.
 * - **id 4** — equal to its declared default, so the encoder must *not* write
 *   it: `sequenceBeginLazy` + `sequenceEnd` with nothing in between is the
 *   hold-back's discard path, the one branch nothing else in the suite takes.
 * - **id 130** — the suite's only two-byte field header, `(130 << 3) | 0`.
 *
 * Encodes to 956 bytes on every port (the reference implementation's figure),
 * which is this dataset's parity check the way 170 is `perf`'s.
 */
void encode_composite(OStreamRaw &os)
{
    os.sequenceBeginLazy(1);
    for (sofab::id i = 0; i < 64; i++)
        os.write(i, std::string_view(comp_items[i], comp_item_len[i]));
    os.sequenceEnd();

    os.write(2, std::string_view(comp_text, sizeof comp_text));

    os.sequenceBeginLazy(3);
    os.sequenceBeginLazy(1);
    os.sequenceBeginLazy(1);
    os.write(1, static_cast<uint32_t>(7));
    os.sequenceEnd();
    os.sequenceEnd();
    os.write(2, static_cast<int32_t>(-1));
    os.sequenceEnd();

    /* id 4: all-default — opened and dropped, emitting nothing. */
    os.sequenceBeginLazy(4);
    os.sequenceEnd();

    os.write(130, static_cast<uint32_t>(0xDEADBEEF));
}

/*!
 * The streaming `blob 1MB` row's sink. BENCH_SPEC is explicit that it **consumes
 * and discards**: accumulating the bytes would charge the streaming row a copy
 * the one-shot row never pays, and I/O is not deterministic under Callgrind.
 * Folding one byte per call is the minimum that keeps the call from being
 * optimised away; the byte counter is what proves the row really moved a
 * megabyte, and costs one add per flush.
 *
 * It returns without installing a buffer, which is §5.1's *copying* sink: the
 * encoder resumes in the same 4096-byte buffer from offset 0.
 */
uint8_t blob_sink_acc;

void blob_sink(std::span<const uint8_t> packet) noexcept
{
    blob_sink_acc ^= packet.empty() ? 0 : packet[0];
    blob_stream_used += packet.size();
}

double measure(void (*fn)(), size_t bytes)
{
    fn(); /* warmup */
    return sofab_bench::measureLoop([fn] { fn(); }, bytes).mb_s;
}

} // namespace

/* ---- workloads (extern "C" + noinline = stable Callgrind toggle points) ---
 *
 * `flatten` on top of `noinline` is what keeps an Ir/op figure comparable from
 * one release of this file to the next. Without it a workload's instruction
 * count depends on how much *else* is in the translation unit: GCC's inlining
 * budget is TU-wide, so growing the suite from four workloads to ten moved
 * `encode: typical message` from 224 to 330 Ir/op and `decode: typical message`
 * from 1199 to 1355 — up to a 47 % "regression" in a library that had not
 * changed a line. `flatten` inlines each entry point's call tree, and the
 * four pre-existing workloads then measure the same with ten workloads in the
 * file as they did with four (verified: 226 / 1275 / 35 044 / 43 839 either
 * way). The figures it reports are a hair above what an unconstrained GCC finds
 * for a four-workload file, and that is the trade — a number that is stable is
 * worth more here than a number that is minimal. */

extern "C" __attribute__((noinline, flatten)) void run_encode_u64_array()
{
    OStreamRaw os;
    os.init(enc_u64_buf, sizeof enc_u64_buf);
    os.write(1, std::span<const uint64_t>(src, N));
    enc_u64_used = os.bytesUsed();
}

extern "C" __attribute__((noinline, flatten)) void run_encode_typical()
{
    OStreamRaw os;
    os.init(typ_buf, sizeof typ_buf);
    encode_typical(os);
    typ_used = os.bytesUsed();
}

extern "C" __attribute__((noinline, flatten)) void run_decode_u64_array()
{
    IStreamRaw is;
    is.init([&is](sofab::id id, size_t, size_t count) {
        if (id == 1) { std::span<uint64_t> sp(dec_array, count); sofab::read(is, sp); }
    });
    is.feed(enc_u64_buf, enc_u64_used);
}

extern "C" __attribute__((noinline, flatten)) void run_decode_typical()
{
    IStreamRaw is;
    is.init([&is](sofab::id id, size_t, size_t) {
        switch (id)
        {
            case 1: sofab::read(is, T.f1); break;
            case 2: sofab::read(is, T.f2); break;
            case 3: sofab::read(is, T.f3); break;
            case 4: sofab::read(is, T.f4); break;
            case 5: sofab::read(is, T.f5); break;
            case 6: { std::span<uint16_t> sp(T.f6, 4); sofab::read(is, sp); } break;
            case 7: sofab::read(is, childMsg); break;
            default: break;
        }
    });
    is.feed(typ_buf, typ_used);
}

/* `blob 1MB`, the floor: the whole message into a buffer sized by hand to hold
 * it (1,000,005 bytes), no sink, so the payload is one contiguous write and no
 * flush logic runs. The buffer is NOT taken from a generated MAX_SIZE — this
 * schema declares no maxlen, so its MAX_SIZE would be the configured ceiling
 * rather than a size the message cannot exceed (BENCH_SPEC). */
extern "C" __attribute__((noinline, flatten)) void run_encode_blob_oneshot()
{
    OStreamRaw os;
    os.init(blob_enc, sizeof blob_enc);
    os.write(1, blob_src, (int32_t)BLOB_LEN);
    blob_used = os.bytesUsed();
}

/* The same bytes through a 4096-byte buffer with a flush sink: ~245 flushes,
 * and every one of them lands mid-payload, so this is the divisible-run path of
 * CORELIB_PLAN §5.1 end to end. Pass-through is not granted — this port
 * implements none, so BENCH_SPEC's optional `blob 1MB passthrough` row is
 * omitted rather than printed as a placeholder. */
extern "C" __attribute__((noinline, flatten)) void run_encode_blob_streaming()
{
    blob_stream_used = 0;
    OStreamRaw os;
    os.init(blob_sink, blob_scratch, sizeof blob_scratch);
    os.write(1, blob_src, (int32_t)BLOB_LEN);
    os.flush();
}

/* `blob 1MB` decode, fed in 4096-byte chunks: every chunk but the last ends
 * inside the payload, so all but the last feed returns INCOMPLETE and the field
 * is reassembled across them. `reset()` first — this is a new message on a
 * stream that has already carried one (§5.1: a clean destination is the
 * application's job), and it is what keeps the reassembly capacity. */
extern "C" __attribute__((noinline, flatten)) void run_decode_blob()
{
    blobStream.reset();
    for (size_t off = 0; off < blob_used; off += BLOB_CHUNK)
    {
        const size_t n = blob_used - off < BLOB_CHUNK ? blob_used - off : (size_t)BLOB_CHUNK;
        blobStream.feed(blob_enc + off, n);
    }
}

extern "C" __attribute__((noinline, flatten)) void run_encode_composite()
{
    OStreamRaw os;
    os.init(comp_buf, sizeof comp_buf);
    encode_composite(os);
    comp_used = os.bytesUsed();
}

extern "C" __attribute__((noinline, flatten)) void run_decode_composite()
{
    IStreamRaw is;
    is.init([&is](sofab::id id, size_t, size_t) {
        switch (id)
        {
            case 1:   sofab::read(is, compItems); break;
            case 2:   sofab::readString(is, C.text); break;
            case 3:   sofab::read(is, compL1); break;
            case 130: sofab::read(is, C.big); break;
            default: break;
        }
    });
    is.feed(comp_buf, comp_used);
}

/* `decode: composite skip-all` — the path a router or filter runs: walk the
 * message, materialize nothing. A callback that reads nothing declines every
 * field, so the decoder skips each one by its metadata (and descends into the
 * sequences to skip those too). Its distance from run_decode_composite is what
 * not-decoding is worth here. */
extern "C" __attribute__((noinline, flatten)) void run_decode_composite_skip()
{
    IStreamRaw is;
    is.init([](sofab::id, size_t, size_t) {});
    is.feed(comp_buf, comp_used);
}

/* ---- the workload table: the one definition of what this suite measures ---
 *
 * The timed table, the single-shot Callgrind mode and `--list` all walk this
 * array, so a workload cannot exist in one of them and be missing from another.
 * `bench --list` prints it as "name<TAB>label" lines; run_callgrind.sh and the
 * run_bench_callgrind CMake target take their workload names and their row
 * labels from that output instead of keeping copies (CORELIB_PLAN §10). */

namespace
{

struct Workload
{
    const char *name;    /* CLI name; the Callgrind toggle is run_<name> */
    const char *label;   /* row label, per BENCH_SPEC's output grammar */
    void (*setup)();     /* prepares the input, excluded from collection */
    void (*run)();       /* the measured operation */
    const size_t *bytes; /* message size, valid once setup+run have run */
};

/* The `blob 1MB` decode row's setup encodes the input *and* runs one decode.
 * The second is what makes the single-shot Callgrind figure the same operation
 * the timed loop measures: the first decode grows the reassembly buffer to a
 * megabyte, and collecting that one-off growth would report the allocator's
 * cost as the decoder's on every port that reassembles. `reset()` keeps the
 * capacity, so from the second call on there is none. */
void setup_decode_blob()
{
    run_encode_blob_oneshot();
    run_decode_blob();
}

const Workload kWorkloads[] = {
    {"encode_u64_array", "encode: u64 array (1000)", nullptr, run_encode_u64_array, &enc_u64_used},
    {"encode_typical", "encode: typical message", nullptr, run_encode_typical, &typ_used},
    {"encode_blob_oneshot", "encode: blob 1MB one-shot", nullptr, run_encode_blob_oneshot,
     &blob_used},
    /* The streaming row's size is what the sink was actually handed, so a row
     * that did not move the whole megabyte fails the parity check instead of
     * borrowing the one-shot row's figure. */
    {"encode_blob_streaming", "encode: blob 1MB streaming", nullptr, run_encode_blob_streaming,
     &blob_stream_used},
    {"encode_composite", "encode: composite", nullptr, run_encode_composite, &comp_used},
    {"decode_u64_array", "decode: u64 array (1000)", run_encode_u64_array, run_decode_u64_array,
     &enc_u64_used},
    {"decode_typical", "decode: typical message", run_encode_typical, run_decode_typical,
     &typ_used},
    {"decode_blob", "decode: blob 1MB", setup_decode_blob, run_decode_blob, &blob_used},
    {"decode_composite", "decode: composite", run_encode_composite, run_decode_composite,
     &comp_used},
    {"decode_composite_skip", "decode: composite skip-all", run_encode_composite,
     run_decode_composite_skip, &comp_used},
};

const Workload *find(const char *name)
{
    for (const Workload &w : kWorkloads)
        if (!strcmp(w.name, name)) return &w;
    return nullptr;
}

/* ---- single-shot mode (one operation, for Callgrind instruction counts) -- */

int run_one(const char *name)
{
    const Workload *w = find(name);
    if (!w)
    {
        fprintf(stderr, "unknown workload: %s\n", name);
        return 1;
    }

    make_src();
    if (w->setup) w->setup();
    w->run();

    /* Every workload's result reaches an observable, so none of them can be
     * optimised away between the toggle points Callgrind collects. */
    fprintf(stderr, "arr0=%llu f1=%u s_f2=%d str=%.5s blob=%u sink=%u items=%zu deep=%u BYTES=%zu\n",
            (unsigned long long)dec_array[0], T.f1, T.s_f2, T.f5.c_str(),
            blob_dec[BLOB_LEN - 1], blob_sink_acc, C.items.size(), C.deep, *w->bytes);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    T.f5.reserve(16); /* string read buffer, reserved outside the measured run */

    if (argc >= 2)
    {
        if (!strcmp(argv[1], "--list"))
        {
            for (const Workload &w : kWorkloads) printf("%s\t%s\n", w.name, w.label);
            return 0;
        }
        return run_one(argv[1]);
    }

    make_src();
    for (const Workload &w : kWorkloads) /* prime every buffer and message size */
    {
        if (w.setup) w.setup();
        w.run();
    }

    printf("=== SofaBuffers pure-C++20 throughput (CPU time, MB/s) ===\n");
    printf("%-26s %12s\n", "Workload", "MB/s");
    printf("%-26s %12s\n", "--------", "----");
    for (const Workload &w : kWorkloads)
        printf("%-26s %12.2f\n", w.label, measure(w.run, *w.bytes));
    printf("\nMB = 1e6 bytes. ~1s CPU-time loop per workload.\n");
    printf("blob 1MB is bandwidth-bound: read one-shot vs streaming, not either alone.\n");
    return 0;
}
