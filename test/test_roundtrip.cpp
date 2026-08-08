/*!
 * @file test_roundtrip.cpp
 * @brief Standalone checks for the pure-C++20 SofaBuffers implementation.
 *
 * Validates wire compatibility against known byte sequences from the shared
 * conformance vectors (assets/test_vectors.json) and exercises encode/decode
 * round-trips, including nested sequences and arrays.
 *
 * Build & run:
 *   g++ -std=c++20 -Iinclude test/test_roundtrip.cpp -o /tmp/t && /tmp/t
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"

#include <array>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

/* --- allocation hook -------------------------------------------------------
 *
 * The encoder is allocation-free except for one place: the held-back sequence
 * run spills to the heap past its inline depth. Replacing global operator new
 * lets holdBackAllocationFailure() both COUNT allocations (proving the shallow
 * path makes none) and fail one on demand (proving a failed spill is reported
 * as an error instead of terminating the process). Under -fno-exceptions the
 * arming flag is never set and this stays a plain counting allocator. */
static unsigned long g_allocCount = 0;
static bool g_failNextAlloc = false;

void *operator new(size_t n)
{
    ++g_allocCount;
#if defined(__cpp_exceptions) && __cpp_exceptions
    if (g_failNextAlloc) { g_failNextAlloc = false; throw std::bad_alloc(); }
#endif
    if (n == 0) n = 1;
    void *p = std::malloc(n);
#if defined(__cpp_exceptions) && __cpp_exceptions
    if (!p) throw std::bad_alloc();
#endif
    return p;
}
void *operator new[](size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

static std::string toHex(std::span<const uint8_t> bytes)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : bytes) { s.push_back(h[b >> 4]); s.push_back(h[b & 0xf]); }
    return s;
}

#define CHECK(cond, what) do { \
    ++g_checks; \
    if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", what); } \
} while (0)

/* --- encode: compare produced bytes to the known wire hex --- */

template <typename Fn>
static void checkEncode(const char *name, const char *expectHex, Fn &&fn)
{
    sofab::OStreamInline<256> os;
    fn(os);
    std::string got = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
    ++g_checks;
    if (got != expectHex)
    {
        ++g_failures;
        std::printf("FAIL encode %s:\n  expected %s\n  got      %s\n", name, expectHex, got.c_str());
    }
}

static void encodeVectors()
{
    using u64 = uint64_t;
    using i64 = int64_t;

    checkEncode("unsigned_0",   "0000",   [](auto &os){ os.write(0, u64{0}); });
    checkEncode("unsigned_0x80","008001", [](auto &os){ os.write(0, u64{0x80}); });
    checkEncode("signed_min",   "01ffffffffffffffffff01", [](auto &os){ os.write(0, i64{INT64_MIN}); });
    checkEncode("boolean_true", "0001",   [](auto &os){ os.write(0, true); });
    checkEncode("fp32",         "0220560e4940", [](auto &os){ os.write(0, 3.1415f); });
    checkEncode("string",       "026248656c6c6f20436f75636821", [](auto &os){ os.write(0, "Hello Couch!"); });
    checkEncode("string_empty", "0202",   [](auto &os){ os.write(0, ""); });
    checkEncode("blob", "022b0102030405", [](auto &os){
        const uint8_t b[] = {1,2,3,4,5}; os.write(0, b, 5); });
    checkEncode("blob_empty", "0203", [](auto &os){ os.write(0, nullptr, 0); });

    checkEncode("array_unsigned_u32", "03050102038080808008ffffffff0f", [](auto &os){
        std::array<uint32_t,5> a{1,2,3,0x80000000u,UINT32_MAX}; os.write(0, a); });
    checkEncode("array_signed_i32", "0405010305ffffffff0ffeffffff0f", [](auto &os){
        std::array<int32_t,5> a{-1,-2,-3,INT32_MIN,INT32_MAX}; os.write(0, a); });
    checkEncode("array_fp32", "0505200000803f0000004000004040ffff7fffffff7f7f", [](auto &os){
        std::array<float,5> a{1.0f,2.0f,3.0f,-FLT_MAX,FLT_MAX}; os.write(0, a); });

    checkEncode("nested_sequence", "002a0e002a1153071153", [](auto &os){
        os.write(0, u64{42})
          .sequenceBeginLazy(1)
            .write(0, u64{42})
            .write(2, i64{-42})
          .sequenceEnd()
          .write(2, i64{-42});
    });
}

/* --- decode / round-trip --- */

struct ScalarMsg : sofab::IStreamMessage
{
    uint64_t a = 0; int64_t b = 0; float f = 0; double d = 0; std::string s; bool flag = false;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 1: is.read(a); break;
            case 2: is.read(b); break;
            case 3: is.read(f); break;
            case 4: is.read(d); break;
            case 5: is.read(s); break;
            case 6: is.read(flag); break;
        }
    }
};

static void roundtripScalars()
{
    sofab::OStreamInline<256> os;
    os.write(1, uint64_t{123456789})
      .write(2, int64_t{-987654321})
      .write(3, 3.14159f)
      .write(4, 2.718281828459045)
      .write(5, std::string_view{"hello sofab"})
      .write(6, true);

    sofab::IStreamObject<ScalarMsg> in;
    in.feed(os.data(), os.bytesUsed());

    CHECK((*in).a == 123456789u, "roundtrip a");
    CHECK((*in).b == -987654321, "roundtrip b");
    CHECK((*in).f == 3.14159f, "roundtrip f");
    CHECK((*in).d == 2.718281828459045, "roundtrip d");
    CHECK((*in).s == "hello sofab", "roundtrip s");
    CHECK((*in).flag == true, "roundtrip flag");
}

struct ArrMsg : sofab::IStreamMessage
{
    std::array<uint32_t,5> u{}; std::array<float,3> f{};
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id) { case 1: is.read(u); break; case 2: is.read(f); break; }
    }
};

static void roundtripArrays()
{
    sofab::OStreamInline<256> os;
    std::array<uint32_t,5> u{10, 20, 30, 0x80000000u, UINT32_MAX};
    std::array<float,3> f{1.5f, -2.5f, 1e30f};
    os.write(1, u).write(2, f);

    sofab::IStreamObject<ArrMsg> in;
    in.feed(os.data(), os.bytesUsed());

    CHECK(((*in).u == std::array<uint32_t,5>{10,20,30,0x80000000u,UINT32_MAX}), "roundtrip uint array");
    CHECK(((*in).f == std::array<float,3>{1.5f,-2.5f,1e30f}), "roundtrip float array");
}

struct Child : sofab::IStreamMessage
{
    uint64_t x = 0; int64_t y = 0;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id) { case 0: is.read(x); break; case 2: is.read(y); break; }
    }
};

struct Parent : sofab::IStreamMessage
{
    uint64_t top = 0; Child child; int64_t tail = 0;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id) { case 0: is.read(top); break; case 1: is.read(child); break; case 2: is.read(tail); break; }
    }
};

static void roundtripNested()
{
    sofab::OStreamInline<256> os;
    os.write(0, uint64_t{42})
      .sequenceBeginLazy(1)
        .write(0, uint64_t{42})
        .write(2, int64_t{-42})
      .sequenceEnd()
      .write(2, int64_t{-42});

    /* the bytes must equal the known nested_sequence vector */
    CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == "002a0e002a1153071153",
          "nested encode bytes");

    sofab::IStreamObject<Parent> in;
    in.feed(os.data(), os.bytesUsed());
    CHECK((*in).top == 42u, "nested top");
    CHECK((*in).child.x == 42u, "nested child.x");
    CHECK((*in).child.y == -42, "nested child.y");
    CHECK((*in).tail == -42, "nested tail");
}

static void chunkedDecode()
{
    sofab::OStreamInline<256> os;
    os.write(1, uint64_t{123456789}).write(2, int64_t{-987654321}).write(5, std::string_view{"chunked"});

    /* feed one byte at a time */
    sofab::IStreamObject<ScalarMsg> in;
    for (size_t i = 0; i < os.bytesUsed(); ++i)
        in.feed(os.data() + i, 1);

    CHECK((*in).a == 123456789u, "chunked a");
    CHECK((*in).b == -987654321, "chunked b");
    CHECK((*in).s == "chunked", "chunked s");
}

/* A truncated blob read through the RAW read(void*, size_t) overload is INCOMPLETE,
 * not INVALID — and it recovers once the rest arrives.
 *
 * readBlob() and readString() guard the identical condition and have always set
 * incomplete_; the raw overload set error_, which made a truncated payload INVALID
 * *and* condemned the run, so the message never completed even after the remaining
 * bytes were fed. Generated code never calls this overload (it uses readBlob), which
 * is why nothing here covered it. */
static void rawBlobReadTruncation()
{
    /* unsigned{0} = 7, then blob{1} = "ABCD" */
    static const uint8_t msg[] = {0x00, 0x07, 0x0a, 0x23, 0x41, 0x42, 0x43, 0x44};

    struct RawBlobMsg : sofab::IStreamMessage {
        uint8_t dst[4];
        size_t got = 0;
        RawBlobMsg() { std::memset(dst, 0xEE, sizeof(dst)); }
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 1) got = is.read(dst, sizeof(dst)); }
    };

    /* Cut after 4, 5 and 6: the header and the fixlen word are complete in the first
     * feed, the payload is short. Cuts 2 and 3 land before the word and were already
     * correct — they are kept as controls. */
    for (size_t cut = 2; cut <= 6; ++cut)
    {
        sofab::IStreamObject<RawBlobMsg> in;
        auto r1 = in.feed(msg, cut);
        CHECK(!r1.ok() || cut == 2, "raw blob: a short payload is not COMPLETE");
        CHECK(r1.incomplete() || cut == 2, "raw blob: a short payload is INCOMPLETE, not INVALID");

        auto r2 = in.feed(msg + cut, sizeof(msg) - cut);
        CHECK(r2.ok(), "raw blob: the message completes once the rest arrives");
        CHECK((*in).got == 4, "raw blob: the whole payload is delivered after resuming");
        CHECK(std::memcmp((*in).dst, "ABCD", 4) == 0, "raw blob: the payload bytes are correct");
    }

    /* The same field alone, truncated, in a single feed: INCOMPLETE, never INVALID. */
    static const uint8_t lone[] = {0x0a, 0x23, 0x41, 0x42};
    sofab::IStreamObject<RawBlobMsg> one;
    CHECK(one.feed(lone, sizeof(lone)).incomplete(),
          "raw blob: a lone truncated blob is INCOMPLETE");
}

static void skippingUnknownFields()
{
    /* encode 3 fields; decode a message that only reads id 2 */
    sofab::OStreamInline<256> os;
    os.write(1, uint64_t{111}).write(2, int64_t{-222}).write(3, 9.0f);

    struct Only2 : sofab::IStreamMessage {
        int64_t b = 0;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 2) is.read(b); }
    };
    sofab::IStreamObject<Only2> in;
    in.feed(os.data(), os.bytesUsed());
    CHECK((*in).b == -222, "skip: read only field 2");
}

/* --- malformed input (architecture §7.2): the decoder must never crash or read
 *     out of bounds. Per spec §7 the decode outcome is three-valued: corruption it
 *     can detect is surfaced as InvalidMessage (INVALID); bytes that begin but do
 *     not finish a field are surfaced as Incomplete (INCOMPLETE) — a first-class,
 *     non-error result distinct from a complete message (None / COMPLETE). --- */

static void malformedInput()
{
    /* Truncated value varint (continuation bit set, then the buffer ends). A
     * streaming decoder treats this as an incomplete tail — no field delivered,
     * no crash — and reports INCOMPLETE (distinct from COMPLETE), never INVALID. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x08, 0x80}; /* id 1, unsigned, dangling varint */
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::Incomplete, "malformed: truncated varint is INCOMPLETE, not COMPLETE/INVALID");
        CHECK(r.incomplete() && !r.complete() && !r.invalid(), "malformed: truncated varint status is Incomplete");
        CHECK((*in).a == 0, "malformed: truncated varint yields no value");
    }

    /* Overlong varint (more than 10 groups → exceeds 64 bits): rejected. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        std::vector<uint8_t> bytes = {0x08}; /* id 1, unsigned */
        for (int i = 0; i < 12; ++i) bytes.push_back(0x80);
        bytes.push_back(0x01);
        auto r = in.feed(bytes.data(), bytes.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: overlong varint rejected");
    }

    /* Overlong varint with NO terminating byte (all continuation, > 64 bits):
     * still INVALID, not INCOMPLETE. The overflow is decided regardless of what
     * follows, so the measure phase must reject it rather than mistake the
     * unterminated tail for a truncated field (corelib-cpp#29). */
    {
        sofab::IStreamObject<ScalarMsg> in;
        std::vector<uint8_t> bytes = {0x08}; /* id 1, unsigned */
        for (int i = 0; i < 11; ++i) bytes.push_back(0x80); /* 11 continuation bytes, no terminator */
        auto r = in.feed(bytes.data(), bytes.size());
        CHECK(r.code() == sofab::Error::InvalidMessage,
              "malformed: unterminated overlong varint is INVALID, not INCOMPLETE (#29)");
    }

    /* Overlong varint that *terminates* on the 10th byte but sets bits beyond
     * bit 63 (F-0016): a 64-bit value fits in 10 groups and the 10th byte may
     * carry only its low bit, so any higher bit is a > 64-bit overflow that
     * must be rejected — not silently wrapped/truncated (§4.1/§6.3, #39). Both
     * `…02` (the 65th bit) and `…7f` (bits 64..69) are distinct malformed
     * inputs that previously collapsed to distinct wrong values. */
    {
        /* id 1 (unsigned `a`) → getVarint path: 9×0xff then a high 10th byte. */
        for (uint8_t last : {uint8_t{0x02}, uint8_t{0x7f}})
        {
            sofab::IStreamObject<ScalarMsg> in;
            std::vector<uint8_t> bytes = {0x08};
            for (int i = 0; i < 9; ++i) bytes.push_back(0xff);
            bytes.push_back(last);
            auto r = in.feed(bytes.data(), bytes.size());
            CHECK(r.code() == sofab::Error::InvalidMessage,
                  "malformed: overlong varint (10th byte high bits) rejected (F-0016/#39)");
        }
        /* id 9 (unknown) → skipVarint path: same overlong must also be INVALID. */
        {
            sofab::IStreamObject<ScalarMsg> in;
            std::vector<uint8_t> bytes = {0x48}; /* id 9, unsigned, skipped */
            for (int i = 0; i < 9; ++i) bytes.push_back(0xff);
            bytes.push_back(0x7f);
            auto r = in.feed(bytes.data(), bytes.size());
            CHECK(r.code() == sofab::Error::InvalidMessage,
                  "malformed: overlong varint on skipped field rejected (F-0016/#39)");
        }
        /* Control: 9×0xff then 0x01 is exactly 2^64-1 and must still decode. */
        {
            sofab::IStreamObject<ScalarMsg> in;
            std::vector<uint8_t> bytes = {0x08};
            for (int i = 0; i < 9; ++i) bytes.push_back(0xff);
            bytes.push_back(0x01);
            auto r = in.feed(bytes.data(), bytes.size());
            CHECK(r.complete() && r.code() == sofab::Error::None, "control: 2^64-1 varint accepted (F-0016/#39)");
            CHECK((*in).a == UINT64_MAX, "control: 2^64-1 varint decodes to max (F-0016/#39)");
        }
    }

    /* Oversized fixlen length: the header claims far more payload than is
     * present. Held as INCOMPLETE, never read past the buffer. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x2a, static_cast<uint8_t>((200u << 3) | 2u), 'h', 'i'};
        auto r = in.feed(bytes, sizeof bytes); /* id 5, string, len=200 */
        CHECK(r.code() == sofab::Error::Incomplete, "malformed: oversized fixlen is INCOMPLETE, no OOB read");
        CHECK((*in).s.empty(), "malformed: oversized fixlen yields no string");
    }

    /* A stray sequence-end marker with no open sequence is INVALID (§7), not a
     * skippable no-op. Both the bare `0x07` (id 0) and an end with a nonzero id
     * after a complete field must be rejected. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x07};
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: stray sequence-end rejected");
    }
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x08, 0x00, 0x7f}; /* id1 unsigned=0, then dangling end (id15) */
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: dangling sequence-end after a field rejected");
    }

    /* Fixlen subtype/length must agree (§4.6): an fp32 payload is exactly 4 bytes
     * and fp64 exactly 8. A float field whose declared length is anything else is
     * INVALID regardless of what follows — not a truncated tail. This is the
     * F-0005 reproducer class (`56 0a 59` = seq{ fp64 with length 11 }). */
    {
        const uint8_t repro[] = {0x56, 0x0a, 0x59}; /* seq id10 { fixlen id1, fp64, len 11 } */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(repro, sizeof repro);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: fp64 length 11 rejected (F-0005 reproducer)");
    }
    {
        /* id5 (reused as a float field here), fixlen, fp32 subtype, wrong lengths. */
        for (uint8_t len : {0u, 3u, 5u, 8u})
        {
            const uint8_t bytes[] = {0x2a, static_cast<uint8_t>((len << 3) | 0u)}; /* fp32 */
            sofab::IStreamObject<ScalarMsg> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: fp32 with length != 4 rejected");
        }
    }
    {
        /* An fp32 with the correct length but a truncated payload stays INCOMPLETE
         * (buffered), so the strictness above never swallows a split chunk. */
        const uint8_t bytes[] = {0x2a, static_cast<uint8_t>((4u << 3) | 0u), 0x00, 0x00}; /* 2 of 4 bytes */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::Incomplete, "malformed: fp32 correct length but truncated payload is INCOMPLETE");
    }

    /* Reserved fixlen subtype (0b100..0b111) is INVALID (§4.6). */
    {
        const uint8_t bytes[] = {0x2a, static_cast<uint8_t>((4u << 3) | 4u)}; /* subtype 4 = reserved */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: reserved fixlen subtype rejected");
    }

    /* A fixlen ARRAY may only carry fp32/fp64 elements (§4.8): a string/blob
     * element subtype, or a wrong element size, is INVALID. */
    {
        const uint8_t bytes[] = {0x2d, 0x01, static_cast<uint8_t>((1u << 3) | 2u)}; /* array id5, count1, string elem */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: string element in fixlen array rejected");
    }
    {
        const uint8_t bytes[] = {0x2d, 0x01, static_cast<uint8_t>((3u << 3) | 0u)}; /* fp32 elem size 3 */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: fixlen array fp32 element size != 4 rejected");
    }

    /* An array count above ARRAY_MAX (INT32_MAX) is INVALID (§6.2) and must be
     * rejected up front rather than driving an unbounded element-skip loop. */
    {
        const uint8_t bytes[] = {0x03, 0x80, 0x80, 0x80, 0x80, 0x08}; /* array id0, count = 2^31 */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: array count above ARRAY_MAX rejected");
    }

    /* A field id above ID_MAX (2^31-1) is INVALID (§6.2) — the decoder must
     * reject it, not treat it as an unknown id and skip it (issue #47 / F-0028). */
    {
        const uint8_t bytes[] = {0x80, 0x80, 0x80, 0x80, 0x40, 0x05}; /* id = 2^31, unsigned, value 5 */
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "malformed: field id above ID_MAX rejected");
    }

    /* Skip an entire unread sub-sequence, then resync on the field after it. */
    {
        sofab::OStreamInline<256> os;
        os.sequenceBeginLazy(1).write(0, uint64_t{7}).write(1, uint64_t{8}).sequenceEnd()
          .write(2, int64_t{-222});

        struct Only2 : sofab::IStreamMessage {
            int64_t b = 0;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 2) is.read(b); }
        };
        sofab::IStreamObject<Only2> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).b == -222, "malformed/skip: resync after skipped sub-sequence");
    }
}

/* --- three-valued decode outcome (spec §7): COMPLETE / INCOMPLETE / INVALID.
 *     There is no finish/finalize step — the same call reports all three, and
 *     INCOMPLETE is a first-class, non-error result, never folded into either
 *     COMPLETE (silent-accept) or INVALID (over-strict rejection). --- */

static void threeValuedOutcomes()
{
    /* COMPLETE: a whole, well-formed message consumed exactly to a field boundary. */
    {
        sofab::OStreamInline<64> os;
        os.write(1, uint64_t{123456789}).write(2, int64_t{-987654321});
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "three-valued: complete message is COMPLETE");
        CHECK(r.status() == sofab::DecodeStatus::Complete, "three-valued: complete status");
        CHECK(r.complete() && !r.incomplete() && !r.invalid(), "three-valued: complete predicates");
        CHECK(r.ok() && static_cast<bool>(r), "three-valued: complete is ok()/bool");
        CHECK((*in).a == 123456789u && (*in).b == -987654321, "three-valued: complete values decoded");
    }

    /* INCOMPLETE: a lone dangling 0x80 — a well-formed *prefix* of a varint. More
     * bytes could complete it, so it is INCOMPLETE, not INVALID (spec §7). */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x80};
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::Incomplete, "three-valued: lone 0x80 is INCOMPLETE");
        CHECK(r.status() == sofab::DecodeStatus::Incomplete, "three-valued: incomplete status");
        CHECK(r.incomplete() && !r.complete() && !r.invalid(), "three-valued: incomplete predicates");
        CHECK(!r.ok() && !static_cast<bool>(r), "three-valued: incomplete is not ok()/bool");
    }

    /* INCOMPLETE: an open (unclosed) sequence — a bare sequence-start with no
     * matching end. Feeding the end would complete it, so it too is INCOMPLETE. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x0e}; /* id 1, sequence-start, never closed */
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::Incomplete, "three-valued: open sequence is INCOMPLETE");
    }

    /* INVALID: a varint that exceeds 64 bits is malformed regardless of what
     * follows (spec §7) and must be terminal, never held as INCOMPLETE. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        std::vector<uint8_t> bytes = {0x08}; /* id 1, unsigned */
        for (int i = 0; i < 12; ++i) bytes.push_back(0x80);
        bytes.push_back(0x01);
        auto r = in.feed(bytes.data(), bytes.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "three-valued: >64-bit varint is INVALID");
        CHECK(r.status() == sofab::DecodeStatus::Invalid, "three-valued: invalid status");
        CHECK(r.invalid() && !r.complete() && !r.incomplete(), "three-valued: invalid predicates");
    }

    /* Streaming: an INCOMPLETE prefix completes to COMPLETE once the rest arrives —
     * the split must never leak an error and must land exactly at COMPLETE. */
    {
        sofab::OStreamInline<64> os;
        os.write(1, uint64_t{123456789});
        sofab::IStreamObject<ScalarMsg> in;
        size_t n = os.bytesUsed();
        auto first = in.feed(os.data(), n - 1);
        CHECK(first.code() == sofab::Error::Incomplete, "three-valued: split head is INCOMPLETE");
        auto rest = in.feed(os.data() + n - 1, 1);
        CHECK(rest.code() == sofab::Error::None, "three-valued: split tail completes to COMPLETE");
        CHECK((*in).a == 123456789u, "three-valued: split message decodes");
    }
}

/* --- invalidate(): a deliver callback rejects content the wire layer cannot
 *     judge on its own — e.g. a generated message whose schema bounds a scalar
 *     array's element count (a wire count above the schema capacity N is
 *     INVALID per spec §3/§7, generator#100). --- */

static void callbackInvalidate()
{
    struct BoundedArr : sofab::IStreamMessage
    {
        std::array<uint32_t, 4> u{};
        int delivered = 0;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
        {
            ++delivered;
            if (id == 0)
            {
                if (count > 4) { is.invalidate(); return; } /* the generated guard */
                is.read(u);
            }
        }
    };

    /* Control: count == capacity decodes COMPLETE. */
    {
        const uint8_t bytes[] = {0x03, 0x04, 1, 2, 3, 4};
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "invalidate: count == capacity stays COMPLETE");
        CHECK(((*in).u == std::array<uint32_t, 4>{1, 2, 3, 4}), "invalidate: control values decoded");
    }

    /* count > capacity: the callback invalidates; feed reports INVALID. */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5};
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "invalidate: over-count array is INVALID");
        CHECK(r.invalid() && !r.complete() && !r.incomplete(), "invalidate: predicates report Invalid");
        CHECK(r.status() == sofab::DecodeStatus::Invalid, "invalidate: status() is Invalid");
    }

    /* Dispatch stops at the invalidated field: nothing after it is delivered. */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5, 0x08, 0x2a}; /* then id1 unsigned 42 */
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::InvalidMessage, "invalidate: INVALID with a trailing field");
        CHECK((*in).delivered == 1, "invalidate: no field delivered past the invalidated one");
    }

    /* Split feed: the verdict now lands on the FIRST chunk, not the completing one.
     * Header-first delivery runs the callback at the count word, so the over-count
     * guard fires while the array is still truncated -- which is what §5.2 asks
     * for ("more bytes could never make it valid"). The old measure-then-deliver
     * path reported INCOMPLETE here and only turned INVALID once the field
     * completed, unless a measure-phase schema had been installed to catch it
     * earlier; there is no such two-tier behaviour any more. */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5};
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, 3); /* header + count + 1 element */
        CHECK(r.code() == sofab::Error::InvalidMessage,
              "invalidate: an over-count array is INVALID at the count word, mid-stream");
    }
    /* An IN-bound array split the same way still reports INCOMPLETE and completes. */
    {
        const uint8_t bytes[] = {0x03, 0x04, 1, 2, 3, 4};
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, 3);
        CHECK(r.code() == sofab::Error::Incomplete, "invalidate: in-bound split array is INCOMPLETE");
        r = in.feed(bytes + 3, sizeof bytes - 3);
        CHECK(r.code() == sofab::Error::None, "invalidate: the completing chunk decodes");
        CHECK(((*in).u == std::array<uint32_t, 4>{1, 2, 3, 4}), "invalidate: split array values decoded");
    }
}

/* --- Header-first delivery: the schema bound rides in the read (§5.2).
 *
 *     A field is delivered at its HEADER, before its payload is known to be
 *     present. The typed read then decides, in this order: does the wire tag match
 *     (§7.3), is the count/length within the schema bound (§7.1/§5.2), and only
 *     then are the bytes here? So an over-bound field is rejected even when it is
 *     also truncated -- "INVALID dominates INCOMPLETE" -- without the decoder
 *     having to be told the schema in advance.
 *
 *     These cases replace the setSchema() descriptor tests: the bound moved from a
 *     static table into the read call, so the same verdicts are now produced by a
 *     callback that reads, and a callback that reads NOTHING has no bound to
 *     apply. --- */

static void headerFirstBounds()
{
    /* ---- over-count: array<u8>, count 4, id 15 (header 0x7b). ---- */
    struct BoundedArr : sofab::IStreamMessage
    {
        std::array<uint8_t, 4> u{};
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 15) is.readArray(u, 4); }
    };
    {   /* count 6 (>4) then EOF after 2 elements — over-count AND truncated */
        const uint8_t bytes[] = {0x7b, 0x06, 1, 2};
        sofab::IStreamObject<BoundedArr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: truncated over-count is INVALID (anti-folding)");
    }
    {   /* count 6 (>4), complete */
        const uint8_t bytes[] = {0x7b, 0x06, 1, 2, 3, 4, 5, 6};
        sofab::IStreamObject<BoundedArr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: complete over-count is INVALID");
    }
    {   /* count 4 (==bound) then EOF — clean truncation control */
        const uint8_t bytes[] = {0x7b, 0x04, 1, 2};
        sofab::IStreamObject<BoundedArr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::Incomplete,
              "header-first: clean truncation at the bound stays INCOMPLETE");
    }
    {   /* the same truncated over-count, fed one byte at a time: the count word
         * arrives on a later feed and the verdict must not fold to INCOMPLETE */
        const uint8_t bytes[] = {0x7b, 0x06, 1, 2};
        sofab::IStreamObject<BoundedArr> in;
        CHECK(in.feed(bytes, 1).code() == sofab::Error::Incomplete,
              "header-first: a lone header buffers as INCOMPLETE");
        CHECK(in.feed(bytes + 1, sizeof bytes - 1).code() == sofab::Error::InvalidMessage,
              "header-first: the continuation reports INVALID at the count word");
    }

    /* ---- over-maxlen: string, maxlen 8, id 5 (header 0x2a).
     *      fixlen word = (len<<3)|String(2): 0x52 = len 10, 0x42 = len 8. ---- */
    struct BoundedStr : sofab::IStreamMessage
    {
        std::string s;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 5) is.readString(s, 8); }
    };
    {   /* len 10 (>8), complete */
        const uint8_t bytes[] = {0x2a, 0x52, 'A','B','C','D','E','F','G','H','I','J'};
        sofab::IStreamObject<BoundedStr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: complete over-maxlen is INVALID");
    }
    {   /* len 10 (>8) then EOF after 2 payload bytes */
        const uint8_t bytes[] = {0x2a, 0x52, 'A','B'};
        sofab::IStreamObject<BoundedStr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: truncated over-maxlen is INVALID (anti-folding)");
    }
    {   /* len 8 (==maxlen) then EOF — clean truncation control */
        const uint8_t bytes[] = {0x2a, 0x42, 'A','B'};
        sofab::IStreamObject<BoundedStr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::Incomplete,
              "header-first: clean truncation at the maxlen stays INCOMPLETE");
    }
    {   /* §7.3 still wins over the bound: a BLOB at the string id contradicts the
         * declaration, so it is skipped and never measured against maxlen 8 */
        const uint8_t bytes[] = {0x2a, 0x53, 'A','B','C','D','E','F','G','H','I','J'};
        sofab::IStreamObject<BoundedStr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None && r.skipped() == 1,
              "header-first: a contradicting subtype is skipped, not bounded (§7.3)");
    }

    /* ---- over-index: wrapper array id 3, count 2. Each element carries its index
     *      as its field id; index >= 2 is INVALID (§5.1/§7), decided by the
     *      collector as the element arrives — no descriptor involved. ---- */
    struct Elems : sofab::IStreamMessage
    {
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (static_cast<size_t>(id) >= 2) { is.invalidate(); return; } }
    };
    struct WrapMsg : sofab::IStreamMessage
    {
        Elems e;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 3) is.read(e); }
    };
    {   /* elements 0,1 then element index 2 (>=2), complete */
        const uint8_t bytes[] = {0x1e, 0x00, 0x2a, 0x08, 0x2a, 0x10, 0x2a, 0x07};
        sofab::IStreamObject<WrapMsg> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: complete over-index is INVALID");
    }
    {   /* elements 0,1 then the index-2 header, then EOF */
        const uint8_t bytes[] = {0x1e, 0x00, 0x2a, 0x08, 0x2a, 0x10};
        sofab::IStreamObject<WrapMsg> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "header-first: truncated over-index is INVALID (anti-folding)");
    }
    {   /* element 0 then a truncated in-bounds element 1 */
        const uint8_t bytes[] = {0x1e, 0x00, 0x2a, 0x08};
        sofab::IStreamObject<WrapMsg> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::Incomplete,
              "header-first: in-bounds truncation stays INCOMPLETE");
    }

    /* A callback that reads NOTHING has no bound to apply: the field is skipped as
     * an unknown id would be, and a truncated one is simply INCOMPLETE. This is the
     * behavioural difference from the descriptor, and it is the correct one — the
     * bound belongs to whoever declares the field. */
    {
        struct Quiet : sofab::IStreamMessage
        { void deserialize(sofab::IStreamImpl &, sofab::id, size_t, size_t) noexcept override {} };
        const uint8_t over[] = {0x7b, 0x06, 1, 2, 3, 4, 5, 6};
        sofab::IStreamObject<Quiet> in;
        CHECK(in.feed(over, sizeof over).code() == sofab::Error::None,
              "header-first: an unread over-count field carries no bound");
    }
}

/* --- the declared element width is a validity bound, not a storage hint
 *     (MESSAGE_SPEC §7/§7.1, Crucible F-0033, corelib-cpp#64).
 *
 * The wire carries no integer width: every element of an `array` is a varint read
 * into a 64-bit accumulator. §7.1 names an over-width scalar as INVALID alongside
 * `M > N` and `maxlen`, and forbids both alternatives by name — it MUST NOT be
 * masked to the declared width, and MUST NOT be kept. For an array element the
 * check has to run where the element is decoded: reading the array into a wider
 * temporary and copying it down afterwards would defeat the bulk path.
 *
 * The bound is a schema fact, so it arrives the way `count` and `maxlen` do — in
 * the read call. Omitting it decodes exactly as before, which is what a caller
 * that has no declared width (a plain read()) must do. --- */

static void elementWidthBound()
{
    /* array<u8> count 4 at id 15 (header 0x7b = (15<<3)|ArrayUnsigned). */
    struct U8Arr : sofab::IStreamMessage
    {
        std::array<uint8_t, 4> u{};
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 15) is.readArray(u, 4, -1, sofab::ElemBound::of<uint8_t>()); }
    };
    /* the same array read WITHOUT a declared width, as a hand-written caller
     * that never had one does. */
    struct U8ArrUnbounded : sofab::IStreamMessage
    {
        std::array<uint8_t, 4> u{};
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 15) is.readArray(u, 4); }
    };
    /* array<i8> count 4 at id 15 (header 0x7c = (15<<3)|ArraySigned). Elements are
     * zig-zagged: -128 -> 255 (ff 01), -129 -> 257 (81 02), 127 -> 254 (fe 01),
     * 128 -> 256 (80 02). */
    struct I8Arr : sofab::IStreamMessage
    {
        std::array<int8_t, 4> i{};
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        { if (id == 15) is.readArray(i, 4, -1, sofab::ElemBound::of<int8_t>()); }
    };

    /* ---- THE ISOLATE: 16383 (ff 7f) into a declared u8. Masking it stores 255;
     *      §7.1 says the message is INVALID. ---- */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 0xff, 0x7f, 2, 3, 4};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: an over-width array element is INVALID, not masked");
        CHECK((*in).u[0] != 255, "§7.1: the over-width element is not kept masked either");
    }
    /* ...the same bytes with no declared width: unchanged behaviour, by design.
     * The bound is opt-in so that a caller who has no schema keeps the decode it
     * always had — it is generated code, holding the declaration, that passes it. */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 0xff, 0x7f, 2, 3, 4};
        sofab::IStreamObject<U8ArrUnbounded> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::None,
              "§7.1: without a declared width the decode is unchanged");
    }

    /* ---- the boundary itself: 255 fits a u8, 256 does not. ---- */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 0xff, 0x01, 2, 3, 4}; /* 255 */
        sofab::IStreamObject<U8Arr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "§7.1: the largest in-width element is admitted");
        CHECK(((*in).u == std::array<uint8_t, 4>{255, 2, 3, 4}), "§7.1: in-width elements decode");
    }
    {
        const uint8_t bytes[] = {0x7b, 0x04, 0x80, 0x02, 2, 3, 4}; /* 256 */
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: one past the width is INVALID");
    }
    /* not only the first element: the bound applies to every one of them */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 1, 2, 3, 0x80, 0x02};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: a LAST over-width element is INVALID too");
    }

    /* ---- signed elements are measured after the zig-zag, at both ends. ---- */
    {
        const uint8_t bytes[] = {0x7c, 0x02, 0xff, 0x01, 0xfe, 0x01}; /* -128, 127 */
        sofab::IStreamObject<I8Arr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "§7.1: the i8 extremes are admitted");
        CHECK(((*in).i == std::array<int8_t, 4>{-128, 127, 0, 0}), "§7.1: i8 elements decode");
    }
    {
        const uint8_t bytes[] = {0x7c, 0x01, 0x81, 0x02}; /* -129 */
        sofab::IStreamObject<I8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: an element below the signed minimum is INVALID");
    }
    {
        const uint8_t bytes[] = {0x7c, 0x01, 0x80, 0x02}; /* 128 */
        sofab::IStreamObject<I8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: an element above the signed maximum is INVALID");
    }

    /* ---- the wider narrow types, so the bound is not a byte-sized special case.
     *      u32's maximum needs more than 32 bits of range to express. ---- */
    {
        struct U32Arr : sofab::IStreamMessage
        {
            std::array<uint32_t, 2> u{};
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 15) is.readArray(u, 2, -1, sofab::ElemBound::of<uint32_t>()); }
        };
        {   /* 4294967295 = ff ff ff ff 0f */
            const uint8_t bytes[] = {0x7b, 0x01, 0xff, 0xff, 0xff, 0xff, 0x0f};
            sofab::IStreamObject<U32Arr> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::None && (*in).u[0] == 4294967295u,
                  "§7.1: the largest u32 element is admitted");
        }
        {   /* 4294967296 = 80 80 80 80 10 */
            const uint8_t bytes[] = {0x7b, 0x01, 0x80, 0x80, 0x80, 0x80, 0x10};
            sofab::IStreamObject<U32Arr> in;
            CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
                  "§7.1: one past the u32 width is INVALID");
        }
    }

    /* ---- a 64-bit element type has the accumulator's own range, so ElemBound::of
     *      is unarmed for it and generated code may pass it unconditionally. ---- */
    {
        static_assert(!sofab::ElemBound::of<uint64_t>().armed, "u64 needs no width bound");
        static_assert(!sofab::ElemBound::of<int64_t>().armed, "i64 needs no width bound");
        struct U64Arr : sofab::IStreamMessage
        {
            std::array<uint64_t, 1> u{};
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 15) is.readArray(u, 1, -1, sofab::ElemBound::of<uint64_t>()); }
        };
        const uint8_t bytes[] = {0x7b, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0x01}; /* UINT64_MAX */
        sofab::IStreamObject<U64Arr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None && (*in).u[0] == UINT64_MAX,
              "§7.1: a u64 element is unbounded, as its declared width is the whole range");
    }

    /* ---- a SURPLUS element -- one past the destination, parsed only to stay
     *      framed -- is over-width all the same, and §7.1 does not ask whether it
     *      had somewhere to be stored. ---- */
    {
        struct ShortDst : sofab::IStreamMessage
        {
            std::array<uint8_t, 2> u{};
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 15) is.readArray(u, -1, -1, sofab::ElemBound::of<uint8_t>()); }
        };
        {   /* four elements into a two-element destination, the LAST over-width */
            const uint8_t bytes[] = {0x7b, 0x04, 1, 2, 3, 0x80, 0x02};
            sofab::IStreamObject<ShortDst> in;
            CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
                  "§7.1: an over-width SURPLUS element is INVALID too");
        }
        {   /* control: the same shape, all elements in width */
            const uint8_t bytes[] = {0x7b, 0x04, 1, 2, 3, 4};
            sofab::IStreamObject<ShortDst> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::None && ((*in).u == std::array<uint8_t, 2>{1, 2}),
                  "§7.1: in-width surplus elements are still discarded, not rejected");
        }
    }

    /* ---- ordering, unchanged: §7.3 still decides first. A signed array at the
     *      id of a declared u8 array contradicts the declaration, so it is skipped
     *      like an unknown id and its elements are never measured. ---- */
    {
        const uint8_t bytes[] = {0x7c, 0x01, 0x80, 0x02}; /* ArraySigned, element 256 */
        sofab::IStreamObject<U8Arr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None && r.skipped() == 1,
              "§7.1: a contradicting array kind is skipped, never width-checked (§7.3)");
    }
    /* ...and the count bound still precedes it: an over-count array is INVALID at
     *    its count word, before any element is looked at. */
    {
        const uint8_t bytes[] = {0x7b, 0x06, 1, 2, 3, 4, 5, 6};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: the count bound still applies ahead of the element bound");
    }

    /* ---- a truncated array is still INCOMPLETE: the width bound rejects a value
     *      that is present, it does not manufacture a verdict for one that is not.
     *      The elements before the cut are in width, so nothing else fires. ---- */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 1, 2};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::Incomplete,
              "§7.1: a truncated in-width array stays INCOMPLETE");
    }
    /* ...but an over-width element that IS present decides before the truncation,
     *    the same way the count bound does (INVALID dominates INCOMPLETE). */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 0x80, 0x02};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
              "§7.1: an over-width element decides before the truncation");
    }
    /* ...and a bound rejection survives being split across feeds: the field is
     *    re-delivered whole, so the verdict is the same as in one chunk. */
    {
        const uint8_t bytes[] = {0x7b, 0x04, 1, 2, 3, 0x80, 0x02};
        sofab::IStreamObject<U8Arr> in;
        CHECK(in.feed(bytes, 3).code() == sofab::Error::Incomplete,
              "§7.1: the leading chunk of the array buffers as INCOMPLETE");
        CHECK(in.feed(bytes + 3, sizeof bytes - 3).code() == sofab::Error::InvalidMessage,
              "§7.1: the completing chunk reports the over-width element");
    }

    /* ---- a dynamic destination takes the same bound. ---- */
    {
        struct VecArr : sofab::IStreamMessage
        {
            std::vector<uint16_t> u;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 15) is.readArray(u, 4, -1, sofab::ElemBound::of<uint16_t>()); }
        };
        {   /* 65535 = ff ff 03 */
            const uint8_t bytes[] = {0x7b, 0x02, 0xff, 0xff, 0x03, 7};
            sofab::IStreamObject<VecArr> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK((r.code() == sofab::Error::None && (*in).u == std::vector<uint16_t>{65535, 7}),
                  "§7.1: a dynamic destination decodes its in-width elements");
        }
        {   /* 65536 = 80 80 04 */
            const uint8_t bytes[] = {0x7b, 0x02, 0x80, 0x80, 0x04, 7};
            sofab::IStreamObject<VecArr> in;
            CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
                  "§7.1: a dynamic destination rejects an over-width element");
        }
    }

    /* ---- an explicit range, for a declared type the caller spells out itself. ---- */
    {
        struct RangedArr : sofab::IStreamMessage
        {
            std::array<int32_t, 2> i{};
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 15) is.readArray(i, 2, -1, sofab::ElemBound{-10, 10}); }
        };
        {   /* -10, 10 -> zig-zag 19 (13), 20 (14) */
            const uint8_t bytes[] = {0x7c, 0x02, 19, 20};
            sofab::IStreamObject<RangedArr> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::None && ((*in).i == std::array<int32_t, 2>{-10, 10}),
                  "§7.1: an explicit range admits its endpoints");
        }
        {   /* 11 -> zig-zag 22 */
            const uint8_t bytes[] = {0x7c, 0x01, 22};
            sofab::IStreamObject<RangedArr> in;
            CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
                  "§7.1: an explicit range rejects one past its top");
        }
        {   /* -11 -> zig-zag 21 */
            const uint8_t bytes[] = {0x7c, 0x01, 21};
            sofab::IStreamObject<RangedArr> in;
            CHECK(in.feed(bytes, sizeof bytes).code() == sofab::Error::InvalidMessage,
                  "§7.1: an explicit range rejects one below its bottom");
        }
    }
}

/* --- exceedLimit(): a deliver callback enforces a receiver-side policy cap the
 *     wire layer cannot know — e.g. a generated message rejecting an unbounded
 *     array whose claimed count exceeds a configured decode limit
 *     (generator#102). Distinct from invalidate(): well-formed bytes, policy
 *     rejection, so the outcome is LimitExceeded, not InvalidMessage. --- */

static void callbackExceedLimit()
{
    struct CappedArr : sofab::IStreamMessage
    {
        std::array<uint32_t, 8> u{};
        int delivered = 0;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
        {
            ++delivered;
            if (id == 0)
            {
                if (count > 4) { is.exceedLimit(); return; } /* the generated #102 guard */
                is.read(u);
            }
        }
    };

    /* Control: count within the cap decodes COMPLETE. */
    {
        const uint8_t bytes[] = {0x03, 0x04, 1, 2, 3, 4};
        sofab::IStreamObject<CappedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "exceedLimit: count within cap stays COMPLETE");
        CHECK(((*in).u == std::array<uint32_t, 8>{1, 2, 3, 4, 0, 0, 0, 0}), "exceedLimit: control values decoded");
    }

    /* count over the cap: the callback reports the policy violation; feed
     * returns LimitExceeded — not InvalidMessage (the bytes are well-formed). */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5};
        sofab::IStreamObject<CappedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::LimitExceeded, "exceedLimit: over-cap array is LimitExceeded");
        CHECK(r.limitExceeded() && !r.invalid() && !r.complete() && !r.incomplete(), "exceedLimit: predicates report LimitExceeded");
    }

    /* Dispatch stops at the over-cap field: nothing after it is delivered. */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5, 0x08, 0x2a}; /* then id1 unsigned 42 */
        sofab::IStreamObject<CappedArr> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::LimitExceeded, "exceedLimit: LimitExceeded with a trailing field");
        CHECK((*in).delivered == 1, "exceedLimit: no field delivered past the over-cap one");
    }
}

/* --- §7.3 wire-type guard (Crucible F-0020): a decoder must SKIP a field whose
 *     header wire type is not the one its declared type maps to, rather than read
 *     it under a mismatched interpretation. read() applies zig-zag on the
 *     destination type's signedness, never on the wire type, so reading a Signed
 *     field as unsigned silently yields the raw (un-zig-zagged) varint. The
 *     IStreamImpl::wire / fixType accessors expose the delivered wire type so a
 *     deliver callback can honour the read() precondition: on a mismatch it does
 *     not call read(), and the field is skipped automatically. The full field-id ×
 *     wire-type matrix lives in the differential fuzzer; this pins the mechanism. --- */

static void wireTypeGuard()
{
    /* A message declaring field 0 as an unsigned integer. It reads only when the
     * delivered wire type matches; otherwise it leaves the field for the skip.
     * `read` records whether a value was actually pulled. */
    struct GuardedU : sofab::IStreamMessage
    {
        uint32_t v = 0xABCD;
        bool read = false;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        {
            if (id != 0) return;
            if (is.wire() != sofab::detail::Wire::Unsigned) return; /* §7.3: skip on mismatch */
            is.read(v);
            read = true;
        }
    };

    /* Correctly typed: id 0, Unsigned, value 6 -> read as 6. */
    {
        const uint8_t bytes[] = {0x00, 0x06};
        sofab::IStreamObject<GuardedU> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "wire-guard: correctly-typed field is COMPLETE");
        CHECK((*in).read && (*in).v == 6, "wire-guard: matching wire type reads the value");
    }

    /* F-0020 reproducer `01 06`: id 0 declared unsigned but delivered with wire
     * type Signed. zig-zag(6) = 3; read-as-unsigned would silently yield 6. The
     * guard must skip it — no value read, field left at its default, decode still
     * COMPLETE (a skip is not an error). */
    {
        const uint8_t bytes[] = {0x01, 0x06};
        sofab::IStreamObject<GuardedU> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "wire-guard: mismatched field skipped, still COMPLETE");
        CHECK(!(*in).read && (*in).v == 0xABCD, "wire-guard: mismatched wire type is not read (no silent mis-decode)");
    }

    /* The same shape with NO hand-written guard. This used to be the gap: the
     * unguarded reader pulled the raw varint without zig-zag and yielded 6 where
     * the Signed value is 3. read() now compares the wire tag itself (§7.3), so
     * the field is left unconsumed, the decoder skips it, the member keeps its
     * default — and the skip is counted. A caller no longer needs wire() for this. */
    {
        struct UnguardedU : sofab::IStreamMessage
        {
            uint32_t v = 0;
            bool taken = false;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 0) taken = is.read(v); }
        };
        const uint8_t bytes[] = {0x01, 0x06}; /* Signed, zig-zag 6 = 3 */
        sofab::IStreamObject<UnguardedU> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "read-seam: a skipped mismatch stays COMPLETE");
        CHECK((*in).v == 0 && !(*in).taken, "read-seam: read() skips a mismatched wire type (no silent mis-decode)");
        CHECK(in.skipped() == 1, "read-seam: the skip is counted");
        CHECK(r.skipped() == 1, "read-seam: the count rides out on the Result");
        CHECK(r.code() == sofab::Error::None, "read-seam: counting a skip does not change the outcome");
    }

    /* The counter is monotonic over the stream, NOT cleared per feed: a message
     * split across chunks must still report every skip it caused. Two mismatched
     * fields (id0 and id1, both declared unsigned, both delivered Signed), fed one
     * byte-pair at a time. */
    {
        struct TwoU : sofab::IStreamMessage
        {
            uint32_t a = 0, b = 0;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 0) is.read(a); else if (id == 1) is.read(b); }
        };
        const uint8_t bytes[] = {0x01, 0x06, 0x09, 0x06}; /* id0 Signed, id1 Signed */
        sofab::IStreamObject<TwoU> in;
        auto r1 = in.feed(bytes, 2);
        CHECK(r1.skipped() == 1, "read-seam: first chunk reports its skip");
        auto r2 = in.feed(bytes + 2, 2);
        CHECK(r2.skipped() == 2, "read-seam: the count survives across feeds");
        CHECK((*in).a == 0 && (*in).b == 0, "read-seam: neither mismatched field was read");
    }

    /* WHAT THE COUNTER COUNTS — the whole point of the diagnostic is that it
     * reports a schema DISAGREEMENT and stays quiet about the routine skips.
     *
     * A field the callback never reads cannot be counted: the count lives in the
     * read itself. That draws the line for free — an id the callback does not
     * know never reaches a read, so it is silent, while a known id whose tag
     * contradicts is counted. No rule anywhere says "do not count unknown ids";
     * it falls out of where the check sits.
     *
     * The last case is the residual limit: a callback that decides NOT to read is
     * indistinguishable from one that is uninterested, so a hand-written guard
     * hides its own skip. Generated code no longer does this anywhere — since
     * readArray()/prepare() folded the array reset behind the tag match, every
     * generated arm reaches a read — but a hand-written caller that guards by
     * hand still opts itself out of the diagnostic. The counter therefore never
     * reports a skip that did not happen. */
    {
        struct Watched : sofab::IStreamMessage
        {
            uint32_t known = 0;   /* id 0, declared unsigned */
            uint32_t guarded = 0; /* id 1, declared unsigned, read behind a hand guard */
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id == 0) is.read(known);
                else if (id == 1)
                {
                    if (is.wire() != sofab::detail::Wire::Unsigned) return; /* the generated array-arm shape */
                    is.read(guarded);
                }
                /* every other id: not ours, never read */
            }
        };
        struct Case { const char *what; const uint8_t *bytes; size_t n; size_t want; };
        const uint8_t unknownId[]  = {0x38, 0x2a};       /* id 7, unsigned — no arm */
        const uint8_t knownOk[]    = {0x00, 0x09};       /* id 0, unsigned — matches */
        const uint8_t knownWrong[] = {0x01, 0x06};       /* id 0, SIGNED — contradicts */
        const uint8_t guardedBad[] = {0x09, 0x06};       /* id 1, SIGNED — guard returns before read */
        const Case cases[] = {
            {"unknown id is not a disagreement",              unknownId,  sizeof unknownId,  0},
            {"a matching field is not a disagreement",        knownOk,    sizeof knownOk,    0},
            {"a known id with a contradicting tag counts",    knownWrong, sizeof knownWrong, 1},
            {"a guarded arm's skip is invisible (known gap)", guardedBad, sizeof guardedBad, 0},
        };
        for (const auto &c : cases)
        {
            sofab::IStreamObject<Watched> in;
            auto r = in.feed(c.bytes, c.n);
            CHECK(r.code() == sofab::Error::None, "skip-count: the probe message decodes COMPLETE");
            CHECK(r.skipped() == c.want, c.what);
        }
    }

    /* Resync: a skipped (mismatched) field must leave the cursor at the next field
     * so a following, correctly-typed field still decodes. id0 Signed (mismatch ->
     * skip), then id1 Signed value -42 (0x09 = id1|Signed, 0x53 = zig-zag(-42)). */
    {
        struct TwoFields : sofab::IStreamMessage
        {
            uint32_t a = 0xABCD;
            int32_t b = 0;
            bool readA = false;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id == 0) { if (is.wire() != sofab::detail::Wire::Unsigned) return; is.read(a); readA = true; }
                else if (id == 1) { if (is.wire() != sofab::detail::Wire::Signed) return; is.read(b); }
            }
        };
        const uint8_t bytes[] = {0x01, 0x06, 0x09, 0x53};
        sofab::IStreamObject<TwoFields> in;
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "wire-guard: skip-then-resync is COMPLETE");
        CHECK(!(*in).readA, "wire-guard: first (mismatched) field skipped");
        CHECK((*in).b == -42, "wire-guard: following field resyncs and decodes");
    }

    /* Subtype guard (§7.3 nuance): fp32/fp64/string/blob all share the Fixlen wire
     * type, so the check is bounded at wire type *plus* fixType. A field declared
     * `string` but delivered as fp32 must be skipped even though both are Fixlen. */
    {
        struct GuardedStr : sofab::IStreamMessage
        {
            std::string s = "def";
            bool read = false;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id != 5) return;
                if (is.wire() != sofab::detail::Wire::Fixlen || is.fixType() != sofab::detail::Fix::String) return;
                is.read(s);
                read = true;
            }
        };

        /* delivered as fp32 (0x2a = id5|Fixlen, 0x20 = len4|Fp32, then 4 bytes) -> skip. */
        {
            const uint8_t bytes[] = {0x2a, 0x20, 0x00, 0x00, 0x00, 0x00};
            sofab::IStreamObject<GuardedStr> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::None, "wire-guard: fp32-for-string skipped, still COMPLETE");
            CHECK(!(*in).read && (*in).s == "def", "wire-guard: wrong fixlen subtype is not read");
        }
        /* delivered as string (0x12 = len2|String, "hi") -> read. */
        {
            const uint8_t bytes[] = {0x2a, 0x12, 'h', 'i'};
            sofab::IStreamObject<GuardedStr> in;
            auto r = in.feed(bytes, sizeof bytes);
            CHECK(r.code() == sofab::Error::None, "wire-guard: matching fixlen subtype is COMPLETE");
            CHECK((*in).read && (*in).s == "hi", "wire-guard: matching fixlen subtype reads the value");
        }
    }

    /* Direct accessor readout: wire()/fixType() report the delivered form through
     * the public sofab::Wire / sofab::Fix names (the promoted enums). */
    {
        sofab::OStreamInline<64> os;
        os.write(0, uint32_t{7})            /* Unsigned */
          .write(1, int32_t{-7})            /* Signed */
          .write(2, 1.5f)                   /* Fixlen / Fp32 */
          .write(3, std::string_view{"x"}); /* Fixlen / String */

        struct Recorder : sofab::IStreamMessage
        {
            std::vector<sofab::detail::Wire> w;
            std::vector<sofab::detail::Fix> f;
            void deserialize(sofab::IStreamImpl &is, sofab::id, size_t, size_t) noexcept override
            { w.push_back(is.wire()); f.push_back(is.fixType()); } /* no read(): fields auto-skip */
        };
        sofab::IStreamObject<Recorder> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "accessor: readout message is COMPLETE");
        CHECK((*in).w.size() == 4, "accessor: all four fields delivered");
        CHECK((*in).w[0] == sofab::detail::Wire::Unsigned, "accessor: field 0 wire is Unsigned");
        CHECK((*in).w[1] == sofab::detail::Wire::Signed, "accessor: field 1 wire is Signed");
        CHECK((*in).w[2] == sofab::detail::Wire::Fixlen && (*in).f[2] == sofab::detail::Fix::Fp32, "accessor: field 2 is Fixlen/Fp32");
        CHECK((*in).w[3] == sofab::detail::Wire::Fixlen && (*in).f[3] == sofab::detail::Fix::String, "accessor: field 3 is Fixlen/String");
    }
}

/* --- zero-length wire forms (§4.7–4.9): zero-count arrays and empty sequences --- */

struct EmptyArrMsg : sofab::IStreamMessage
{
    std::array<uint32_t, 4> u{9, 9, 9, 9};
    std::array<int32_t, 4> s{9, 9, 9, 9};
    std::array<float, 4> f{9, 9, 9, 9};
    size_t uCount = 999, sCount = 999, fCount = 999;
    int64_t tail = 0;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
    {
        switch (id)
        {
            case 1: uCount = count; is.read(u); break;
            case 2: sCount = count; is.read(s); break;
            case 3: fCount = count; is.read(f); break;
            case 4: is.read(tail); break;
        }
    }
};

static void zeroLengthForms()
{
    /* encode: exact wire bytes. A zero-count integer array is [header][count=0];
     * a zero-count fixlen array still carries its fixlen_word (§4.8), so an empty
     * fp32 (0x20) and fp64 (0x41) stay distinct. An empty sequence is [start][0x07]
     * (§4.9). */
    checkEncode("array_unsigned_empty", "0300",   [](auto &os){ std::array<uint32_t, 0> a{}; os.write(0, a); });
    checkEncode("array_signed_empty",   "0400",   [](auto &os){ std::array<int32_t, 0> a{}; os.write(0, a); });
    checkEncode("array_fp32_empty",     "050020", [](auto &os){ std::array<float, 0> a{}; os.write(0, a); });
    checkEncode("array_fp64_empty",     "050041", [](auto &os){ std::array<double, 0> a{}; os.write(0, a); });
    checkEncode("empty_sequence",       "0607", [](auto &os){ os.sequenceBeginLazy(0).sequenceEndKeep(); });

    /* decode: empty arrays followed by a real field must keep the cursor aligned
     * (the empty fixlen array's fixlen_word is consumed, nothing more). Feed whole,
     * then one byte at a time. */
    sofab::OStreamInline<64> os;
    std::array<uint32_t, 0> eu{};
    std::array<int32_t, 0> es{};
    std::array<float, 0> ef{};
    os.write(1, eu).write(2, es).write(3, ef).write(4, int64_t{-42});

    for (int pass = 0; pass < 2; ++pass)
    {
        sofab::IStreamObject<EmptyArrMsg> in;
        if (pass == 0)
            in.feed(os.data(), os.bytesUsed());
        else
            for (size_t i = 0; i < os.bytesUsed(); ++i) in.feed(os.data() + i, 1);

        CHECK((*in).uCount == 0, "zero-count unsigned array: count 0");
        CHECK((*in).sCount == 0, "zero-count signed array: count 0");
        CHECK((*in).fCount == 0, "zero-count fixlen array: count 0");
        CHECK((*in).tail == -42, pass == 0 ? "zero-len: resync tail (whole)"
                                           : "zero-len: resync tail (chunked)");
    }

    /* an empty sequence must round-trip: a child message whose sub-sequence has
     * no fields decodes cleanly and the following field resyncs. */
    {
        sofab::OStreamInline<64> seq;
        seq.sequenceBeginLazy(1).sequenceEndKeep().write(2, int64_t{-7});
        struct OnlyTail : sofab::IStreamMessage {
            int64_t t = 0;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 2) is.read(t); }
        };
        sofab::IStreamObject<OnlyTail> in;
        in.feed(seq.data(), seq.bytesUsed());
        CHECK((*in).t == -7, "empty sequence: resync after empty sub-sequence");
    }
}

/* --- lazy sequence framing (MESSAGE_SPEC §2) ------------------------------- */

static void lazySequenceFraming()
{
    /* An all-default sequence carries no information, so the FIELD is omitted --
     * where the pre-§2 rule wrote a two-byte empty frame. An ELEMENT still keeps
     * its frame; that is the `end_keep` case right below. */
    checkEncode("lazy_empty_omitted", "",
                [](auto &os){ os.sequenceBeginLazy(1).sequenceEnd(); });
    checkEncode("end_keep_frames_contentless", "0e07",
                [](auto &os){ os.sequenceBeginLazy(1).sequenceEndKeep(); });

    /* EVERY writer commits the run, not just the scalar one. The five inline
     * writers each call beforeContent() themselves (there is no shared header
     * function left to catch a missing call), so each needs its own check: drop
     * the call from any one of them and exactly the line below fails, with the
     * enclosing `0e` header missing from the bytes.
     *   writeScalar        -> lazy_run_commits_on_content (and most cases here)
     *   writeFixlen        -> string / blob
     *   writeFloatScalar   -> float
     *   writeIntArray      -> integer array
     *   writeFloatArray    -> float array                                    */
    checkEncode("first_child_string_commits_the_run", "0e0212686907",
                [](auto &os){
                    os.sequenceBeginLazy(1).write(0, "hi").sequenceEnd();
                });
    checkEncode("first_child_blob_commits_the_run", "0e020b0107",
                [](auto &os){
                    const uint8_t b[] = {1};
                    os.sequenceBeginLazy(1);
                    os.write(0, b, 1);
                    os.sequenceEnd();
                });
    checkEncode("first_child_float_commits_the_run", "0e02200000803f07",
                [](auto &os){
                    os.sequenceBeginLazy(1).write(0, 1.0f).sequenceEnd();
                });
    checkEncode("first_child_double_commits_the_run", "0e0241000000000000f03f07",
                [](auto &os){
                    os.sequenceBeginLazy(1).write(0, 1.0).sequenceEnd();
                });
    checkEncode("first_child_int_array_commits_the_run", "0e0302010207",
                [](auto &os){
                    std::array<uint32_t, 2> a{1, 2};
                    os.sequenceBeginLazy(1).write(0, a).sequenceEnd();
                });
    checkEncode("first_child_float_array_commits_the_run", "0e0501200000803f07",
                [](auto &os){
                    std::array<float, 1> a{1.0f};
                    os.sequenceBeginLazy(1).write(0, a).sequenceEnd();
                });

    /* One child commits the whole held-back run, outermost header first. */
    checkEncode("lazy_run_commits_on_content", "0e16002a0707",
                [](auto &os){
                    os.sequenceBeginLazy(1).sequenceBeginLazy(2)
                      .write(0, uint64_t{42}).sequenceEnd().sequenceEnd();
                });

    /* Only the empty inner sequence drops; the outer one has content. */
    checkEncode("lazy_drops_only_empty_inner", "0e002a07",
                [](auto &os){
                    os.sequenceBeginLazy(1).sequenceBeginLazy(2).sequenceEnd()
                      .write(0, uint64_t{42}).sequenceEnd();
                });

    /* A lazy sequence after content, and sibling order, stay intact. */
    checkEncode("lazy_after_content", "00011003",
                [](auto &os){
                    os.write(0, uint64_t{1}).sequenceBeginLazy(1).sequenceEnd()
                      .write(2, uint64_t{3});
                });

    /* Forcing a frame forces its ancestors too: the outer got content (the inner
     * frame), so it is framed as well. */
    checkEncode("end_keep_commits_the_enclosing_run", "0e160707",
                [](auto &os){
                    os.sequenceBeginLazy(1).sequenceBeginLazy(2).sequenceEndKeep().sequenceEnd();
                });

    /* A run COMMITTED ACROSS A FLUSH BOUNDARY must give the same bytes as the
     * one-shot encode.
     *
     * Note what is deliberately NOT claimed here: that a flush lands *while* a
     * header is still held back. No test can show that, because it is
     * unreachable by construction -- a held-back header occupies no buffer
     * space (the ids are encoder state, `pending_`, not buffer content), and
     * the buffer can only fill through a write, which commits the whole run
     * before its first byte is pushed. So a pending run can never straddle a
     * flush. The reachable neighbour is the one below: the commit itself is cut
     * in half by the buffer end. */
    {
        /* 3-byte window: the flush lands just after the run was committed. */
        std::vector<uint8_t> out;
        uint8_t buf[3];
        sofab::OStreamView os(
            [&out](std::span<const uint8_t> d){ out.insert(out.end(), d.begin(), d.end()); },
            buf, sizeof(buf), 0);
        os.sequenceBeginLazy(1).sequenceBeginLazy(2).sequenceEnd()
          .write(0, uint64_t{42}).sequenceEnd();
        os.flush();
        const std::string got = toHex(std::span<const uint8_t>(out.data(), out.size()));
        ++g_checks;
        if (got != "0e002a07")
        {
            ++g_failures;
            std::printf("FAIL run committed across a 3-byte buffer:\n  expected 0e002a07\n  got      %s\n",
                        got.c_str());
        }
    }
    {
        /* 2-byte window and a three-header run, so the flush falls INSIDE
         * commitPending(): two headers fill the window, the third lands after
         * the flush. Bytes must still equal the one-shot encode. */
        std::vector<uint8_t> out;
        uint8_t buf[2];
        sofab::OStreamView os(
            [&out](std::span<const uint8_t> d){ out.insert(out.end(), d.begin(), d.end()); },
            buf, sizeof(buf), 0);
        os.sequenceBeginLazy(1).sequenceBeginLazy(2).sequenceBeginLazy(3)
          .write(0, uint64_t{42})
          .sequenceEnd().sequenceEnd().sequenceEnd();
        os.flush();
        const std::string got = toHex(std::span<const uint8_t>(out.data(), out.size()));
        /* ids 1/2/3 as sequence-start: (id<<3)|6 = 0e/16/1e; then id 0 unsigned
         * 42 = 00 2a; then three end markers 07. */
        ++g_checks;
        if (got != "0e161e002a070707")
        {
            ++g_failures;
            std::printf("FAIL run commit split by a 2-byte buffer:\n  expected 0e161e002a070707\n  got      %s\n",
                        got.c_str());
        }
        /* and the same ops through a buffer that holds everything */
        checkEncode("same_run_one_shot", "0e161e002a070707",
                    [](auto &o){
                        o.sequenceBeginLazy(1).sequenceBeginLazy(2).sequenceBeginLazy(3)
                         .write(0, uint64_t{42})
                         .sequenceEnd().sequenceEnd().sequenceEnd();
                    });
    }
}

/* --- the hold-back reaches every legal depth (CORELIB_PLAN §6, "How deep the
 *     hold-back reaches"): this port can allocate, so the pending run grows on
 *     demand instead of stopping at a fixed window and framing eagerly beyond
 *     it. Deep-and-contentless is exactly what the old eager fallback got
 *     wrong -- it emitted the empty frames §2 omits. --- */

static void deepHoldBack()
{
    /* 40 levels: past the 32-entry window this port used to have. */
    {
        sofab::OStreamInline<512> os;
        for (int i = 0; i < 40; ++i) os.sequenceBeginLazy(sofab::id(1));
        for (int i = 0; i < 40; ++i) os.sequenceEnd();
        CHECK(os.bytesUsed() == 0, "40 contentless nested sequences encode to zero bytes");
    }

    /* the full MAX_DEPTH, still zero bytes: there is no window left at all. */
    {
        sofab::OStreamInline<2048> os;
        bool allOpened = true;
        for (int i = 0; i < sofab::MAX_DEPTH; ++i)
            if (!os.sequenceBeginLazy(sofab::id(1)).ok()) allOpened = false;
        for (int i = 0; i < sofab::MAX_DEPTH; ++i) os.sequenceEnd();
        CHECK(allOpened, "MAX_DEPTH nested sequences all open");
        CHECK(os.bytesUsed() == 0, "MAX_DEPTH contentless nested sequences encode to zero bytes");
    }

    /* deep WITH content: the whole run is committed, outermost header first, so
     * the bytes are 40 starts + the field + 40 end markers. */
    {
        sofab::OStreamInline<512> os;
        for (int i = 0; i < 40; ++i) os.sequenceBeginLazy(sofab::id(1));
        os.write(0, uint64_t{42});
        for (int i = 0; i < 40; ++i) os.sequenceEnd();
        std::string expect;
        for (int i = 0; i < 40; ++i) expect += "0e";   /* id 1, sequence-start */
        expect += "002a";                              /* id 0 unsigned = 42 */
        for (int i = 0; i < 40; ++i) expect += "07";   /* sequence-end */
        const std::string got = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
        ++g_checks;
        if (got != expect)
        {
            ++g_failures;
            std::printf("FAIL 40-deep run with content:\n  expected %s\n  got      %s\n",
                        expect.c_str(), got.c_str());
        }
    }

    /* Every depth from 1 to 20 behaves the same -- this walks across whatever
     * internal boundary the run has between stream-local and spilled storage,
     * which must not be observable in the bytes. */
    {
        int badDepth = 0;
        for (int d = 1; d <= 20 && !badDepth; ++d)
        {
            sofab::OStreamInline<256> os;
            for (int i = 0; i < d; ++i) os.sequenceBeginLazy(sofab::id(1));
            for (int i = 0; i < d; ++i) os.sequenceEnd();
            if (os.bytesUsed() != 0) badDepth = d;
        }
        ++g_checks;
        if (badDepth)
        {
            ++g_failures;
            std::printf("FAIL contentless nesting emitted bytes at depth %d\n", badDepth);
        }
    }

    /* Unwinding back across that boundary: open ids 1..12, let the innermost 5
     * expire contentless, then write. Exactly the remaining seven headers must
     * appear, outermost first and in order. */
    {
        sofab::OStreamInline<256> os;
        for (sofab::id i = 1; i <= 12; ++i) os.sequenceBeginLazy(i);
        for (int i = 0; i < 5; ++i) os.sequenceEnd();
        os.write(0, uint64_t{42});
        for (int i = 0; i < 7; ++i) os.sequenceEnd();
        const std::string got = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
        /* ids 1..7 as sequence-start ((id<<3)|6), then id 0 unsigned 42, then
         * seven end markers. */
        const char *expect = "0e161e262e363e002a07070707070707";
        ++g_checks;
        if (got != expect)
        {
            ++g_failures;
            std::printf("FAIL partial unwind of a deep run:\n  expected %s\n  got      %s\n",
                        expect, got.c_str());
        }
    }

    /* the innermost of a deep run stays droppable on its own: only the empty
     * level 41 vanishes, the 40 that carry it are framed. */
    {
        sofab::OStreamInline<512> os;
        for (int i = 0; i < 40; ++i) os.sequenceBeginLazy(sofab::id(1));
        os.sequenceBeginLazy(2).sequenceEnd();        /* contentless: dropped */
        os.write(0, uint64_t{42});
        for (int i = 0; i < 40; ++i) os.sequenceEnd();
        std::string expect;
        for (int i = 0; i < 40; ++i) expect += "0e";
        expect += "002a";
        for (int i = 0; i < 40; ++i) expect += "07";
        const std::string got = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
        ++g_checks;
        if (got != expect)
        {
            ++g_failures;
            std::printf("FAIL 40-deep run drops only the empty inner level:\n  expected %s\n  got      %s\n",
                        expect.c_str(), got.c_str());
        }
    }
}

/* --- what happens when a run is committed into a buffer too small for it.
 *
 * commitPending() drops the whole run even when a header write fails halfway
 * through, so the ids not yet written are lost. That is safe for exactly one
 * reason, and this pins the reason rather than the reasoning: the failure sets
 * the sticky failed_ with the cursor at the buffer end, so nothing more can be
 * written anyway and ok() already condemns the output. Concretely: for every
 * capacity below what the message needs, the bytes produced are a byte-exact
 * PREFIX of the full encoding (never a wrong or resumed one), ok() is false, and
 * a further write neither succeeds nor appends. --- */

static void bufferFullCondemnsTheRun()
{
    constexpr int kDepth = 12;

    /* the full, unconstrained encoding of the same op sequence */
    std::string full;
    {
        sofab::OStreamInline<64> os;
        for (int i = 0; i < kDepth; ++i) os.sequenceBeginLazy(sofab::id(1));
        os.write(0, uint64_t{42});
        for (int i = 0; i < kDepth; ++i) os.sequenceEnd();
        CHECK(os.ok(), "buffer-full probe: the reference encode itself succeeds");
        full = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
    }
    const size_t fullBytes = full.size() / 2;

    int badPrefix = 0, badOk = 0, badResume = 0;
    for (size_t cap = 1; cap < fullBytes; ++cap)
    {
        std::vector<uint8_t> buf(cap, 0xAA);
        sofab::OStreamView os(buf.data(), cap, 0);   /* no flush callback */
        for (int i = 0; i < kDepth; ++i) os.sequenceBeginLazy(sofab::id(1));
        os.write(0, uint64_t{42});
        for (int i = 0; i < kDepth; ++i) os.sequenceEnd();

        const std::string got = toHex(std::span<const uint8_t>(os.data(), os.bytesUsed()));
        if (got != full.substr(0, got.size()) && !badPrefix) badPrefix = static_cast<int>(cap);
        if (os.ok() && !badOk) badOk = static_cast<int>(cap);

        /* nothing can be written after the failure: the lost ids could not have
         * reached the wire even if the run had kept them. */
        const size_t before = os.bytesUsed();
        if ((os.write(1, uint64_t{7}).ok() || os.bytesUsed() != before) && !badResume)
            badResume = static_cast<int>(cap);
    }
    CHECK(badPrefix == 0, "truncated commit stays a prefix of the full encoding");
    CHECK(badOk == 0, "a truncated commit always reports ok() == false");
    CHECK(badResume == 0, "nothing more can be written after a truncated commit");

    /* and with exactly enough room the same ops give the full encoding */
    {
        std::vector<uint8_t> buf(fullBytes, 0xAA);
        sofab::OStreamView os(buf.data(), fullBytes, 0);
        for (int i = 0; i < kDepth; ++i) os.sequenceBeginLazy(sofab::id(1));
        os.write(0, uint64_t{42});
        for (int i = 0; i < kDepth; ++i) os.sequenceEnd();
        CHECK(os.ok(), "exact-size buffer: the encode succeeds");
        CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == full,
              "exact-size buffer: the bytes are the full encoding");
    }

    /* with a flush callback the failure is unreachable at ANY window size --
     * the other half of the argument above. */
    {
        int badWindow = 0;
        for (size_t cap = 1; cap <= 4 && !badWindow; ++cap)
        {
            std::vector<uint8_t> out;
            std::vector<uint8_t> buf(cap, 0xAA);
            sofab::OStreamView os([&out](std::span<const uint8_t> d){ out.insert(out.end(), d.begin(), d.end()); },
                                  buf.data(), cap, 0);
            for (int i = 0; i < kDepth; ++i) os.sequenceBeginLazy(sofab::id(1));
            os.write(0, uint64_t{42});
            for (int i = 0; i < kDepth; ++i) os.sequenceEnd();
            os.flush();
            if (!os.ok() || toHex(std::span<const uint8_t>(out.data(), out.size())) != full)
                badWindow = static_cast<int>(cap);
        }
        CHECK(badWindow == 0, "with a flush callback no window size can truncate the run");
    }
}

/* --- the hold-back's own allocation: past the run's inline depth the ids spill
 *     to the heap, and a failed allocation there must be REPORTED (BufferFull +
 *     ok() == false), never fatal. Needs exceptions to observe: the replacement
 *     operator new below throws on demand. --- */

static void holdBackAllocationFailure()
{
#if defined(__cpp_exceptions) && __cpp_exceptions
    sofab::OStreamInline<64> os;

    /* the shallow part of the run must not allocate at all */
    g_allocCount = 0;
    int opened = 0;
    for (int i = 0; i < 8; ++i)
        if (os.sequenceBeginLazy(sofab::id(1)).ok()) ++opened;
    CHECK(opened == 8, "hold-back: the inline levels all open");
    CHECK(g_allocCount == 0, "hold-back: nesting to the inline depth allocates nothing");

    /* the next one spills -- fail that allocation */
    g_failNextAlloc = true;
    const auto refused = os.sequenceBeginLazy(sofab::id(2));
    g_failNextAlloc = false;
    CHECK(refused.code() == sofab::Error::BufferFull,
          "hold-back: a failed spill allocation is reported as BufferFull");
    CHECK(!refused.ok() && !os.ok(),
          "hold-back: a failed spill allocation condemns the stream");

    /* the refused level was NOT opened: writing now commits exactly the eight
     * ids that were accepted. */
    os.write(0, uint64_t{42});
    CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == "0e0e0e0e0e0e0e0e002a",
          "hold-back: a refused open leaves the run untouched");

    /* and a spill that CAN allocate still works (same stream, deeper) */
    {
        sofab::OStreamInline<64> deep;
        int ok = 0;
        for (int i = 0; i < 12; ++i)
            if (deep.sequenceBeginLazy(sofab::id(1)).ok()) ++ok;
        deep.write(0, uint64_t{42});
        for (int i = 0; i < 12; ++i) deep.sequenceEnd();
        CHECK(ok == 12 && deep.ok(), "hold-back: a spill that allocates succeeds");
        CHECK(toHex(std::span<const uint8_t>(deep.data(), deep.bytesUsed())) ==
                  "0e0e0e0e0e0e0e0e0e0e0e0e002a070707070707070707070707",
              "hold-back: the spilled ids reach the wire in order");
    }
#endif
}

/* --- depth bookkeeping: BOTH closers must give the nesting budget back, or a
 *     long run of sibling sequences would eventually be refused although
 *     nothing is open; and a stray close must not underflow it. --- */

static void sequenceDepthBookkeeping()
{
    /* open MAX_DEPTH, close them all, open MAX_DEPTH again -- once per closer.
     * Contentless opens/closes write nothing, so the keep variant is the only
     * one that needs buffer room (255 starts + 255 ends, twice). */
    {
        sofab::OStreamInline<64> os;
        for (int round = 0; round < 2; ++round)
        {
            int opened = 0;
            bool allClosed = true;
            for (int i = 0; i < sofab::MAX_DEPTH; ++i)
                if (os.sequenceBeginLazy(sofab::id(1)).ok()) ++opened;
            CHECK(opened == sofab::MAX_DEPTH, "sequenceEnd: MAX_DEPTH opens available each round");
            for (int i = 0; i < sofab::MAX_DEPTH; ++i)
                if (!os.sequenceEnd().ok()) allClosed = false;
            CHECK(allClosed, "sequenceEnd: every open sequence closes cleanly");
        }
        CHECK(os.bytesUsed() == 0, "sequenceEnd: two full-depth rounds emit nothing");
    }
    {
        sofab::OStreamInline<2048> os;
        for (int round = 0; round < 2; ++round)
        {
            int opened = 0;
            bool allClosed = true;
            for (int i = 0; i < sofab::MAX_DEPTH; ++i)
                if (os.sequenceBeginLazy(sofab::id(1)).ok()) ++opened;
            CHECK(opened == sofab::MAX_DEPTH, "sequenceEndKeep: MAX_DEPTH opens available each round");
            for (int i = 0; i < sofab::MAX_DEPTH; ++i)
                if (!os.sequenceEndKeep().ok()) allClosed = false;
            CHECK(allClosed, "sequenceEndKeep: every open sequence closes cleanly");
        }
        /* every level was forced out, both rounds: 2 * (255 starts + 255 ends) */
        CHECK(os.bytesUsed() == 2 * 2 * size_t(sofab::MAX_DEPTH),
              "sequenceEndKeep: each round frames all MAX_DEPTH levels");
    }

    /* a close with nothing open must not drive the depth negative: after it the
     * full MAX_DEPTH budget is still there. (It does emit a stray end marker --
     * unbalanced calls are caller error -- but the counter stays sane.) */
    {
        sofab::OStreamInline<64> os;
        os.sequenceEnd();
        os.sequenceEndKeep();
        int opened = 0;
        for (int i = 0; i < sofab::MAX_DEPTH + 5; ++i)
            if (os.sequenceBeginLazy(sofab::id(1)).ok()) ++opened;
        CHECK(opened == sofab::MAX_DEPTH, "stray close does not underflow the depth counter");
    }
}

/* --- MAX_DEPTH = 255 (§4.9, §6.2): bounded nesting on encode and decode --- */

static void maxDepth()
{
    CHECK(sofab::MAX_DEPTH == 255, "MAX_DEPTH constant is 255");

    /* encoder refuses to open a 256th nested sequence. */
    {
        sofab::OStreamInline<1024> os;
        sofab::Error firstErr = sofab::Error::None;
        int opened = 0;
        for (int i = 0; i < 300; ++i)
        {
            auto r = os.sequenceBeginLazy(0);
            if (!r.ok()) { firstErr = r.code(); break; }
            ++opened;
        }
        CHECK(opened == 255, "encoder opens exactly MAX_DEPTH sequences");
        CHECK(firstErr == sofab::Error::InvalidArgument, "encoder rejects the 256th with InvalidArgument");
    }

    /* decoder rejects a message nested past MAX_DEPTH with InvalidMessage and
     * never recurses unbounded (would otherwise overflow the native stack). */
    {
        std::vector<uint8_t> deep(300, 0x06); /* 300 bare sequence-start markers */
        struct Empty : sofab::IStreamMessage {
            void deserialize(sofab::IStreamImpl &, sofab::id, size_t, size_t) noexcept override {}
        };
        sofab::IStreamObject<Empty> in;
        auto r = in.feed(deep.data(), deep.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "decoder rejects nesting past MAX_DEPTH");
    }
}

/* --- streaming buffer limit (issue #26): an opt-in cap on how large the
 *     reassembly buffer may grow for a single incomplete top-level field.
 *     Exceeding it fails feed() with Error::LimitExceeded — a receiver-side
 *     *policy* code, deliberately distinct from InvalidMessage (wire
 *     malformation) so a differential fuzzer never conflates the two. The
 *     default (no Limits) is byte-for-byte the old unlimited behaviour. --- */

static void appendVarint(std::vector<uint8_t> &v, uint64_t x)
{
    do { uint8_t b = x & 0x7f; x >>= 7; if (x) b |= 0x80; v.push_back(b); } while (x);
}

static void bufferLimits()
{
    const size_t cap = 64 * 1024; /* 64 KiB */

    /* Claimed-oversize header: a fixlen string declaring a 1 MiB payload that
     * never arrives. The cap is checked the instant the declared length is
     * known, so feed() fails with LimitExceeded before acc_ grows to hold the
     * promised bytes. This is the issue's acceptance case. */
    {
        std::vector<uint8_t> hdr = {0x2a}; /* id 5, fixlen */
        appendVarint(hdr, (static_cast<uint64_t>(1u << 20) << 3) | 2u); /* string, len = 1 MiB */
        sofab::IStreamObject<ScalarMsg> in(sofab::Limits{cap});
        auto r = in.feed(hdr.data(), hdr.size()); /* header only, payload absent */
        CHECK(r.code() == sofab::Error::LimitExceeded, "limit: claimed-oversize header fails immediately");
        CHECK(r.limitExceeded() && !r.invalid() && !r.complete() && !r.incomplete(),
              "limit: LimitExceeded predicates are distinct from invalid()");
        CHECK((*in).s.empty(), "limit: no value delivered for the rejected field");
    }

    /* Chunk-independence: the SAME oversize field dribbled a byte at a time. The
     * split length varint is held as INCOMPLETE until it completes, then the cap
     * fires — the outcome does not depend on how the bytes were framed. */
    {
        std::vector<uint8_t> hdr = {0x2a};
        appendVarint(hdr, (static_cast<uint64_t>(1u << 20) << 3) | 2u);
        sofab::IStreamObject<ScalarMsg> in(sofab::Limits{cap});
        sofab::Error last = sofab::Error::None;
        bool sawIncomplete = false;
        for (size_t i = 0; i < hdr.size(); ++i)
        {
            auto r = in.feed(hdr.data() + i, 1);
            last = r.code();
            if (r.code() == sofab::Error::Incomplete) sawIncomplete = true;
            if (r.code() == sofab::Error::LimitExceeded) break;
        }
        CHECK(sawIncomplete, "limit: dribbled header is INCOMPLETE until the length word lands");
        CHECK(last == sofab::Error::LimitExceeded, "limit: dribbled oversize still ends in LimitExceeded");
    }

    /* Many small fields inside one sequence: no single declared payload crosses
     * the cap, yet their running total does. Fed in small chunks, feed() reports
     * LimitExceeded once the buffered sequence outgrows the cap. */
    {
        const size_t smallCap = 128;
        std::vector<uint8_t> seq = {0x0e};                                    /* id 1, sequence-start */
        for (int i = 0; i < 200; ++i) { seq.push_back(0x00); seq.push_back(0x00); } /* id 0 unsigned = 0 */
        seq.push_back(0x07);                                                  /* sequence-end */
        sofab::IStreamObject<ScalarMsg> in(sofab::Limits{smallCap});
        sofab::Error last = sofab::Error::None;
        for (size_t i = 0; i < seq.size(); i += 8)
        {
            size_t n = seq.size() - i < 8 ? seq.size() - i : 8;
            auto r = in.feed(seq.data() + i, n);
            last = r.code();
            if (r.code() == sofab::Error::LimitExceeded) break;
        }
        CHECK(last == sofab::Error::LimitExceeded, "limit: oversized sequence of small fields is capped");
    }

    /* No-limit pass-through (opt-in): the identical oversize header, with NO cap,
     * is simply INCOMPLETE (awaiting payload) — never LimitExceeded. */
    {
        std::vector<uint8_t> hdr = {0x2a};
        appendVarint(hdr, (static_cast<uint64_t>(1u << 20) << 3) | 2u);
        sofab::IStreamObject<ScalarMsg> in; /* default: uncapped */
        auto r = in.feed(hdr.data(), hdr.size());
        CHECK(r.code() == sofab::Error::Incomplete, "limit: without a cap the oversize header stays INCOMPLETE");
    }

    /* No-limit pass-through, positive: a large field decodes normally to COMPLETE
     * when no cap is configured. */
    {
        std::string big(4000, 'x');
        sofab::OStreamInline<8192> os;
        os.write(5, std::string_view{big});
        sofab::IStreamObject<ScalarMsg> in; /* default: uncapped */
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "limit: default (no cap) decodes a large field COMPLETE");
        CHECK((*in).s == big, "limit: uncapped large field round-trips");
    }

    /* The cap plumbs through IStreamInline too (not just IStreamObject). */
    {
        std::vector<uint8_t> hdr = {0x2a};
        appendVarint(hdr, (static_cast<uint64_t>(1u << 20) << 3) | 2u);
        bool delivered = false;
        sofab::IStreamInline in([&](sofab::id, size_t, size_t) { delivered = true; }, sofab::Limits{cap});
        auto r = in.feed(hdr.data(), hdr.size());
        CHECK(r.code() == sofab::Error::LimitExceeded, "limit: IStreamInline honours the cap");
        CHECK(!delivered, "limit: IStreamInline delivers no field for the rejected header");
    }

    /* A field under the cap decodes COMPLETE even with a cap set — the limit only
     * rejects what exceeds it. */
    {
        std::string s(1000, 'y');
        sofab::OStreamInline<4096> os;
        os.write(5, std::string_view{s});
        sofab::IStreamObject<ScalarMsg> in(sofab::Limits{cap}); /* 64 KiB cap, 1000-byte field */
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "limit: a field under the cap decodes COMPLETE");
        CHECK((*in).s == s, "limit: under-cap field round-trips with a cap set");
    }
}

/* --- strict UTF-8 (spec MESSAGE_SPEC §8, CORELIB_PLAN §6.4). The validator
 *     itself is always available (utf8_valid); the SOFAB_STRICT_UTF8 gate only
 *     decides whether encode/decode invoke it. --- */

/* Encode a single string/blob field (id 5) so it can be fed straight into ScalarMsg. */
static std::vector<uint8_t> stringFieldWire(std::string_view payload, sofab::detail::Fix sub)
{
    std::vector<uint8_t> w;
    w.push_back(static_cast<uint8_t>((5u << 3) | 2u)); /* id 5, Fixlen */
    uint64_t word = (static_cast<uint64_t>(payload.size()) << 3) | static_cast<uint64_t>(sub);
    do { uint8_t b = word & 0x7f; word >>= 7; if (word) b |= 0x80; w.push_back(b); } while (word);
    for (char c : payload) w.push_back(static_cast<uint8_t>(c));
    return w;
}

static void strictUtf8()
{
    using sofab::utf8_valid;

    /* --- validator: accepts well-formed sequences (always compiled in). --- */
    CHECK(utf8_valid(""), "utf8: empty is valid");
    CHECK(utf8_valid("hello sofab"), "utf8: ASCII is valid");
    CHECK(utf8_valid(std::string_view("a\0b", 3)), "utf8: embedded NUL is valid");
    CHECK(utf8_valid("\xC2\xA9"), "utf8: U+00A9 (C2 A9) valid");
    CHECK(utf8_valid("\xE2\x82\xAC"), "utf8: U+20AC euro (E2 82 AC) valid");
    CHECK(utf8_valid("\xF0\x9F\x98\x80"), "utf8: U+1F600 emoji (F0 9F 98 80) valid");
    CHECK(utf8_valid("\xED\x9F\xBF"), "utf8: U+D7FF (ED 9F BF) valid — just below surrogates");
    CHECK(utf8_valid("\xEE\x80\x80"), "utf8: U+E000 (EE 80 80) valid — just above surrogates");
    CHECK(utf8_valid("\xF4\x8F\xBF\xBF"), "utf8: U+10FFFF (F4 8F BF BF) valid — max code point");

    /* --- validator: rejects malformed sequences (security surface). --- */
    CHECK(!utf8_valid(std::string_view("\xC0\x80", 2)), "utf8: reject overlong C0 80 (Modified-UTF-8 NUL)");
    CHECK(!utf8_valid("\xC1\xBF"), "utf8: reject overlong C1 BF");
    CHECK(!utf8_valid("\xE0\x80\x80"), "utf8: reject overlong 3-byte E0 80 80");
    CHECK(!utf8_valid("\xE0\x9F\xBF"), "utf8: reject overlong E0 9F BF");
    CHECK(!utf8_valid(std::string_view("\xF0\x80\x80\x80", 4)), "utf8: reject overlong 4-byte F0 80 80 80");
    CHECK(!utf8_valid("\xF0\x8F\xBF\xBF"), "utf8: reject overlong F0 8F BF BF");
    CHECK(!utf8_valid("\xED\xA0\x80"), "utf8: reject surrogate U+D800 (ED A0 80)");
    CHECK(!utf8_valid("\xED\xBF\xBF"), "utf8: reject surrogate U+DFFF (ED BF BF)");
    CHECK(!utf8_valid("\xF4\x90\x80\x80"), "utf8: reject > U+10FFFF (F4 90 80 80)");
    CHECK(!utf8_valid(std::string_view("\x80", 1)), "utf8: reject bare continuation 0x80");
    CHECK(!utf8_valid(std::string_view("\xFF", 1)), "utf8: reject lone 0xFF");
    CHECK(!utf8_valid(std::string_view("\xF5\x80\x80\x80", 4)), "utf8: reject F5 lead (> range)");
    CHECK(!utf8_valid(std::string_view("\xC2", 1)), "utf8: reject truncated 2-byte C2");
    CHECK(!utf8_valid("\xE2\x82"), "utf8: reject truncated 3-byte E2 82");
    CHECK(!utf8_valid("\xE2\x28\xA1"), "utf8: reject bad continuation E2 28 A1");

    /* --- valid data always round-trips byte-identically, in either build. --- */
    {
        const std::string s = "\xE2\x82\xAC\xF0\x9F\x98\x80 mixed \xC2\xA9"; /* euro emoji ascii copyright */
        sofab::OStreamInline<64> os;
        auto w = os.write(5, std::string_view{s});
        CHECK(w.code() == sofab::Error::None, "utf8: valid multibyte string encodes");
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "utf8: valid multibyte string decodes COMPLETE");
        CHECK((*in).s == s, "utf8: valid multibyte string round-trips identically");
    }

    /* --- embedded U+0000 is valid: encodes and round-trips (not truncated). --- */
    {
        const std::string s("a\0b\0", 4);
        sofab::OStreamInline<64> os;
        auto w = os.write(5, std::string_view{s});
        CHECK(w.code() == sofab::Error::None, "utf8: embedded-NUL string encodes");
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "utf8: embedded-NUL string decodes COMPLETE");
        CHECK((*in).s == s, "utf8: embedded-NUL string round-trips (4 bytes, no truncation)");
    }

    /* --- a valid multi-byte sequence split across feed() stays INCOMPLETE and
     *     completes to COMPLETE — a chunk boundary never forces INVALID. --- */
    {
        auto w = stringFieldWire("\xE2\x82\xAC", sofab::detail::Fix::String); /* 5 bytes total */
        sofab::IStreamObject<ScalarMsg> in;
        auto r1 = in.feed(w.data(), w.size() - 1); /* split mid-sequence (E2 82 | AC) */
        CHECK(r1.code() == sofab::Error::Incomplete, "utf8: cross-chunk split is INCOMPLETE, not INVALID");
        auto r2 = in.feed(w.data() + w.size() - 1, 1);
        CHECK(r2.code() == sofab::Error::None, "utf8: split multibyte completes to COMPLETE");
        CHECK((*in).s == "\xE2\x82\xAC", "utf8: split multibyte decodes correctly");
    }

#if SOFAB_STRICT_UTF8
    /* --- encode rejects invalid UTF-8 with InvalidArgument (strict build). --- */
    {
        sofab::OStreamInline<64> os;
        auto w = os.write(5, std::string_view("\xC0\x80", 2)); /* overlong NUL */
        CHECK(w.code() == sofab::Error::InvalidArgument, "utf8/strict: encode rejects C0 80 with InvalidArgument");
    }
    {
        sofab::OStreamInline<64> os;
        auto w = os.write(5, std::string_view("\xED\xA0\x80", 3)); /* surrogate */
        CHECK(w.code() == sofab::Error::InvalidArgument, "utf8/strict: encode rejects surrogate with InvalidArgument");
    }

    /* --- decode rejects an invalid-UTF-8 materialised string as INVALID. --- */
    {
        auto w = stringFieldWire("\xC0\x80", sofab::detail::Fix::String);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "utf8/strict: decode rejects C0 80 string as INVALID");
        CHECK(r.invalid() && r.status() == sofab::DecodeStatus::Invalid, "utf8/strict: decode reject maps to Invalid status");
    }
    {
        /* truncated-at-end-of-payload (declared length reached mid-sequence) is INVALID. */
        auto w = stringFieldWire("\xE2\x82", sofab::detail::Fix::String);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "utf8/strict: truncated-at-end string is INVALID");
    }

    /* --- a skipped string is never validated (spec §6.4 skip exemption). --- */
    {
        struct SkipAll : sofab::IStreamMessage {
            void deserialize(sofab::IStreamImpl &, sofab::id, size_t, size_t) noexcept override {} /* read nothing */
        };
        auto w = stringFieldWire("\xC0\x80", sofab::detail::Fix::String);
        sofab::IStreamObject<SkipAll> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::None, "utf8/strict: a SKIPPED invalid-UTF-8 string is not validated (COMPLETE)");
    }

    /* --- blob is never validated: same bytes as a blob encode and decode fine. --- */
    {
        const uint8_t raw[] = {0xC0, 0x80};
        sofab::OStreamInline<64> os;
        auto w = os.write(5, raw, static_cast<int32_t>(sizeof raw)); /* blob overload */
        CHECK(w.code() == sofab::Error::None, "utf8/strict: blob write of non-UTF-8 bytes is accepted");
    }
    {
        /* a Blob-subtype fixlen read into a std::string is not validated. */
        auto w = stringFieldWire("\xC0\x80", sofab::detail::Fix::Blob);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::None, "utf8/strict: Blob-subtype payload is not UTF-8 validated");
        CHECK((*in).s == std::string("\xC0\x80", 2), "utf8/strict: Blob payload stored verbatim");
    }
#endif
}

/* --- the message layer: write(id, msg) vs writeLazy(id, msg), and the
 *     wrapper-array collectors StringSeq / BlobSeq / MessageSeq.
 *
 * MESSAGE_SPEC §2 omits a sequence-typed FIELD whose value equals its declared
 * default; a wrapper-array ELEMENT keeps its frame, because element presence is
 * what carries a dynamic array's length (§5.1). The two closers are what
 * implement that split, and picking the wrong one for an element does not cost
 * bytes -- it changes the decoded array's LENGTH. --- */

static std::vector<uint8_t> fromHex(std::string_view h)
{
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
    };
    std::vector<uint8_t> v;
    int hi = -1;
    for (char c : h)                    /* separators are ignored, so the wire can be
                                         * written in readable groups */
    {
        if (!isHex(c)) continue;
        if (hi < 0) { hi = nib(c); continue; }
        v.push_back(static_cast<uint8_t>(hi << 4 | nib(c)));
        hi = -1;
    }
    return v;
}

/* One uint field, all-default when a == 0, serialised sparsely per §2. */
struct SeqRow : sofab::Message
{
    uint64_t a = 0;
    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        return os.writeIf(0, a, a != 0);
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 0) is.read(a);
    }
};

/* A row that is a sub-message FIELD of another row -- for the recursive case:
 * an all-default child field vanishes, but the element framing it does not. */
struct NestRow : sofab::Message
{
    SeqRow child;
    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        return os.writeLazy(1, child); /* FIELD form: drops when all-default */
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 1) is.read(child);
    }
};

/* Collectors are constructed per delivery exactly as generated code does it. */
struct SeqMsg : sofab::IStreamMessage
{
    std::vector<std::string> tags;                  /* id 1 -- StringSeq  */
    std::vector<SeqRow> rows;                       /* id 2 -- MessageSeq */
    std::vector<std::vector<uint8_t>> blobs;        /* id 3 -- BlobSeq    */
    std::vector<std::vector<uint32_t>> matrix;      /* id 4 -- MessageSeq of native-array rows */
    std::vector<NestRow> nested;                    /* id 5 -- MessageSeq */
    uint64_t other = 0;                             /* id 9 -- plain scalar */

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 1: { sofab::StringSeq c{tags};  is.read(c); break; }
            case 2: { sofab::MessageSeq<SeqRow> c; c.out = &rows; is.read(c); break; }
            case 3: { sofab::BlobSeq c{blobs};   is.read(c); break; }
            case 4: { sofab::MessageSeq<std::vector<uint32_t>> c; c.out = &matrix; is.read(c); break; }
            case 5: { sofab::MessageSeq<NestRow> c; c.out = &nested; is.read(c); break; }
            case 9: is.read(other); break;
        }
    }
};

/* The auditor's M3 wire puts the wrapper at field id 1; this type decodes it
 * verbatim, so the exact bytes from the report appear in the test. */
struct RowsAtOneMsg : sofab::IStreamMessage
{
    std::vector<SeqRow> rows;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 1) { sofab::MessageSeq<SeqRow> c; c.out = &rows; is.read(c); }
    }
};

/* Per-element schema bounds: `maxlen: 2` on every element (§7.1). */
struct BoundedSeqMsg : sofab::IStreamMessage
{
    std::vector<std::string> tags;               /* id 1 */
    std::vector<std::vector<uint8_t>> blobs;     /* id 2 */
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 1: { sofab::StringSeq c{tags, -1, 2};  is.read(c); break; }
            case 2: { sofab::BlobSeq   c{blobs, -1, 2}; is.read(c); break; }
        }
    }
};

/* A bounded array: schema `count: 2`, so an element id >= 2 is INVALID (§5.1/§7). */
struct CappedMsg : sofab::IStreamMessage
{
    std::vector<SeqRow> rows;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 1) { sofab::MessageSeq<SeqRow> c; c.out = &rows; c.cap = 2; is.read(c); }
    }
};

static void messageLayerFraming()
{
    const SeqRow dflt{};
    SeqRow seven{}; seven.a = 7;

    /* --- write(id, msg): the ELEMENT form, closed with sequenceEndKeep. --- */

    checkEncode("element_all_default_keeps_its_frame", "0e07",
                [&](auto &os){ os.write(1, dflt); });
    checkEncode("element_with_content", "0e000707",
                [&](auto &os){ os.write(1, seven); });

    /* --- writeLazy(id, msg): the FIELD form, closed with sequenceEnd. The
     *     same all-default value that stays framed above vanishes here. --- */

    checkEncode("field_all_default_is_omitted", "",
                [&](auto &os){ os.writeLazy(1, dflt); });
    checkEncode("field_with_content_is_framed", "0e000707",
                [&](auto &os){ os.writeLazy(1, seven); });

    /* the predicate is recursive: a row whose only child FIELD is all-default is
     * itself all-default, so the whole thing collapses to zero bytes. */
    checkEncode("field_omission_is_recursive", "",
                [&](auto &os){ NestRow n{}; os.writeLazy(1, n); });
    /* ...but as an ELEMENT the same value is `begin end`, its child still gone. */
    checkEncode("element_of_recursively_default_row_keeps_one_frame", "0e07",
                [&](auto &os){ NestRow n{}; os.write(1, n); });

    /* --- the auditor's case, byte for byte: rows = [{}, {}, {a=7}] as a
     *     wrapper array at field id 2. Every leading element is all-default, so
     *     with the FIELD closer they would all disappear and the array would
     *     decode with length 1 instead of 3. --- */
    checkEncode("wrapper_array_of_all_default_rows", "1606070e071600070707",
                [&](auto &os){
                    SeqRow d{}, s{}; s.a = 7;
                    os.sequenceBeginLazy(2)
                        .write(0, d).write(1, d).write(2, s)
                      .sequenceEnd();
                });
    {
        sofab::OStreamInline<64> os;
        SeqRow d{}, s{}; s.a = 7;
        os.sequenceBeginLazy(2).write(0, d).write(1, d).write(2, s).sequenceEnd();
        sofab::IStreamObject<SeqMsg> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.complete(), "wrapper array of all-default rows decodes COMPLETE");
        CHECK((*in).rows.size() == 3, "element framing carries the array length: 3 rows");
        if ((*in).rows.size() == 3)
        {
            CHECK((*in).rows[0].a == 0 && (*in).rows[1].a == 0 && (*in).rows[2].a == 7,
                  "all-default elements decode to default rows, in order");
        }
    }
    {
        /* the pure case: EVERY element all-default. With the field closer the
         * whole array would encode to nothing and decode as length 0. */
        sofab::OStreamInline<64> os;
        SeqRow d{};
        os.sequenceBeginLazy(2).write(0, d).write(1, d).sequenceEnd();
        CHECK(os.bytesUsed() != 0, "an array of only all-default elements is not empty on the wire");
        sofab::IStreamObject<SeqMsg> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).rows.size() == 2, "two all-default elements decode as length 2");
    }
    {
        /* the field form's counterpart: an EMPTY array field (declared default
         * empty) is omitted entirely -- zero bytes, decoding to the empty array. */
        sofab::OStreamInline<64> os;
        os.sequenceBeginLazy(2).sequenceEnd();
        CHECK(os.bytesUsed() == 0, "an empty array field with an empty default is omitted");
        sofab::IStreamObject<SeqMsg> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).rows.empty(), "the omitted array field decodes as the empty array");
    }
    {
        /* a nested-message element, i.e. the recursive case end to end. */
        sofab::OStreamInline<64> os;
        NestRow n0{}, n1{}; n1.child.a = 5;
        os.sequenceBeginLazy(5).write(0, n0).write(1, n1).sequenceEnd();
        CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == "2e06070e0e0005070707",
              "nested-row array: element 0 keeps a bare frame, element 1 carries its child");
        sofab::IStreamObject<SeqMsg> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).nested.size() == 2, "nested-row array decodes as length 2");
        if ((*in).nested.size() == 2)
            CHECK((*in).nested[0].child.a == 0 && (*in).nested[1].child.a == 5,
                  "nested-row array element values survive");
    }
}

/* --- the wrapper-array collectors, including §5.1 id gaps --- */

static void wrapperArrayCollectors()
{
    /* --- StringSeq. §2 omits a default (empty) leaf element, so ids gap; §5.1
     *     requires the decoder to refill the gap with the element default. --- */
    {
        sofab::OStreamInline<64> os;
        const std::vector<std::string> tags{"x", "", "y"};
        os.sequenceBeginLazy(1);
        for (size_t i = 0; i < tags.size(); ++i)
            os.writeIf(sofab::id(i), tags[i], !tags[i].empty()); /* §2: drop the default */
        os.sequenceEnd();
        CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == "0e020a78120a7907",
              "string array omits its default element, leaving an id gap");

        sofab::IStreamObject<SeqMsg> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.complete(), "string array with an id gap decodes COMPLETE");
        CHECK((*in).tags.size() == 3, "StringSeq fills the id gap: length 3, not 2");
        if ((*in).tags.size() == 3)
            CHECK((*in).tags[0] == "x" && (*in).tags[1].empty() && (*in).tags[2] == "y",
                  "StringSeq places each element at its id, gap at the element default");
    }
    {
        /* the auditor's msgA, verbatim */
        sofab::IStreamObject<SeqMsg> in;
        auto a = fromHex("0e020a780a0a7907");
        in.feed(a.data(), a.size());
        CHECK((*in).tags.size() == 2 && (*in).tags[0] == "x" && (*in).tags[1] == "y",
              "StringSeq decodes 0e020a780a0a7907 as [x, y]");
    }
    {
        /* §7.4: a repeated wrapper id REPLACES the array whole -- that is what
         * prepare() is for. */
        sofab::IStreamObject<SeqMsg> in;
        auto w = fromHex("0e020a780a0a7907" "0e020a7a07"); /* [x,y] then [z] */
        in.feed(w.data(), w.size());
        CHECK((*in).tags.size() == 1 && (*in).tags[0] == "z",
              "§7.4: a repeated array field id replaces the whole array");
    }

    /* --- BlobSeq: same placement rules, an interior default element gaps. --- */
    {
        sofab::OStreamInline<64> os;
        const uint8_t b0[] = {1, 2};
        const uint8_t b2[] = {3};
        os.sequenceBeginLazy(3);
        os.write(0, b0, 2);
        /* element 1 is the empty blob = the element default: omitted (§2) */
        os.write(2, b2, 1);
        os.sequenceEnd();
        CHECK(toHex(std::span<const uint8_t>(os.data(), os.bytesUsed())) == "1e02130102120b0307",
              "blob array omits its default element, leaving an id gap");

        sofab::IStreamObject<SeqMsg> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).blobs.size() == 3, "BlobSeq fills the id gap: length 3, not 2");
        if ((*in).blobs.size() == 3)
            CHECK(((*in).blobs[0] == std::vector<uint8_t>{1, 2} &&
                   (*in).blobs[1].empty() &&
                   (*in).blobs[2] == std::vector<uint8_t>{3}),
                  "BlobSeq places each element at its id, gap at the element default");
    }

    /* --- MessageSeq: the element id IS the index here too. A conformant encoder
     *     never gaps a sequence-form element, but a decoder MUST accept the gap
     *     and recover the length as *highest present id + 1* (§5.1) -- appending
     *     instead silently SHORTENS the array. --- */
    {
        /* the auditor's M3 wire, verbatim: elements at id 0 and id 2, id 1 absent */
        sofab::IStreamObject<RowsAtOneMsg> in;
        auto w = fromHex("0e060005071600090707");
        auto r = in.feed(w.data(), w.size());
        CHECK(r.complete(), "MessageSeq id-gap wire decodes COMPLETE");
        CHECK((*in).rows.size() == 3, "MessageSeq fills the id gap: length 3, not 2");
        if ((*in).rows.size() == 3)
            CHECK((*in).rows[0].a == 5 && (*in).rows[1].a == 0 && (*in).rows[2].a == 9,
                  "MessageSeq places each element at its id: [5, 0, 9]");
    }
    {
        /* the same for a native-array row, which takes MessageSeq's resize path. */
        sofab::OStreamInline<64> os;
        const std::vector<uint32_t> r0{1, 2}, r2{3};
        os.sequenceBeginLazy(4).write(0, r0).write(2, r2).sequenceEnd();
        sofab::IStreamObject<SeqMsg> in;
        in.feed(os.data(), os.bytesUsed());
        CHECK((*in).matrix.size() == 3, "MessageSeq of native-array rows fills the id gap");
        if ((*in).matrix.size() == 3)
            CHECK(((*in).matrix[0] == std::vector<uint32_t>{1, 2} &&
                   (*in).matrix[1].empty() &&
                   (*in).matrix[2] == std::vector<uint32_t>{3}),
                  "native-array rows land at their id, gap row stays empty");
    }
    {
        /* an out-of-order / descending id still lands at its index, so a decoder
         * cannot be talked into appending. */
        sofab::IStreamObject<RowsAtOneMsg> in;
        auto w = fromHex("0e" "1600090707"); /* only element id 2 present */
        in.feed(w.data(), w.size());
        CHECK((*in).rows.size() == 3, "a lone element at id 2 decodes as length 3");
        if ((*in).rows.size() == 3)
            CHECK((*in).rows[0].a == 0 && (*in).rows[1].a == 0 && (*in).rows[2].a == 9,
                  "a lone element at id 2 lands at index 2");
    }
    {
        /* §5.1/§7: with a schema `count: 2`, an element id >= 2 is INVALID --
         * decided from the id alone, before the container is grown. */
        sofab::IStreamObject<CappedMsg> in;
        auto w = fromHex("0e060005071600090707");
        auto r = in.feed(w.data(), w.size());
        CHECK(r.invalid(), "count-bounded array: element id 2 with count 2 is INVALID");
    }
    {
        /* and within the bound it still decodes normally. */
        sofab::IStreamObject<CappedMsg> in;
        auto w = fromHex("0e0600050707");
        auto r = in.feed(w.data(), w.size());
        CHECK(r.complete(), "count-bounded array: an in-range element decodes COMPLETE");
        CHECK((*in).rows.size() == 1 && (*in).rows[0].a == 5,
              "count-bounded array: the in-range element lands at index 0");
    }
    /* --- per-element schema bounds ride into the read (§7.1): an over-long
     *     element is INVALID, never truncated. --- */
    {
        sofab::IStreamObject<BoundedSeqMsg> in;
        auto w = fromHex("0e021a61626307");      /* one 3-byte string, maxlen 2 */
        CHECK(in.feed(w.data(), w.size()).invalid(),
              "§7.1: a string element over its maxlen is INVALID");
    }
    {
        sofab::IStreamObject<BoundedSeqMsg> in;
        auto w = fromHex("0e0212616207");        /* one 2-byte string: at the bound */
        CHECK(in.feed(w.data(), w.size()).complete(),
              "§7.1: a string element at its maxlen decodes COMPLETE");
        CHECK((*in).tags.size() == 1 && (*in).tags[0] == "ab", "bounded string element decodes");
    }
    {
        sofab::IStreamObject<BoundedSeqMsg> in;
        auto w = fromHex("16021b01020307");      /* one 3-byte blob, maxlen 2 */
        CHECK(in.feed(w.data(), w.size()).invalid(),
              "§7.1: a blob element over its maxlen is INVALID");
    }
    {
        sofab::IStreamObject<BoundedSeqMsg> in;
        auto w = fromHex("1602130102" "07");
        CHECK(in.feed(w.data(), w.size()).complete(),
              "§7.1: a blob element at its maxlen decodes COMPLETE");
        CHECK(((*in).blobs.size() == 1 && (*in).blobs[0] == std::vector<uint8_t>{1, 2}),
              "bounded blob element decodes");
    }
    {
        /* a blob element cut in half is INCOMPLETE, and completes on the next feed. */
        sofab::IStreamObject<BoundedSeqMsg> in;
        auto head = fromHex("160213 01");
        auto tail = fromHex("0207");
        CHECK(in.feed(head.data(), head.size()).incomplete(),
              "a truncated blob element is INCOMPLETE, not INVALID");
        CHECK(in.feed(tail.data(), tail.size()).complete(),
              "the truncated blob element completes on the next feed");
        CHECK(((*in).blobs.size() == 1 && (*in).blobs[0] == std::vector<uint8_t>{1, 2}),
              "the reassembled blob element decodes correctly");
    }
    {
        /* §7.3: a mis-typed wrapper field is SKIPPED, and must not wipe the
         * array a previous, correctly typed occurrence produced. */
        sofab::IStreamObject<SeqMsg> in;
        auto w = fromHex("0e020a780a0a7907" "0803");   /* [x,y] then id 1 as an unsigned */
        auto r = in.feed(w.data(), w.size());
        CHECK(r.complete(), "§7.3 mis-typed array occurrence is skipped, not an error");
        CHECK(r.skipped() == 1, "§7.3 the mis-typed occurrence is counted as skipped");
        CHECK((*in).tags.size() == 2, "§7.3 a skipped occurrence does not wipe the array");
    }
}

/* --- §7.3 decides BEFORE the §5.1/§7 over-index bound (Crucible F-0041,
 *     corelib-cpp#58).
 *
 * An element header that is wrong twice over -- an id past the schema `count`
 * AND a wire type (or fixlen subtype) that contradicts the declared element type
 * -- must be SKIPPED, exactly as an unknown id is skipped. §7.3's rule is
 * unconditional ("Against a schema bound, this clause wins"), and a field skipped
 * under it never becomes an element: its id is therefore not an array index, and
 * an id that is not an index cannot breach the index bound. §7.4 states the same
 * for the replace-whole rule; CORELIB_PLAN §4.8 gives the reason -- the field was
 * never this array's value.
 *
 * The bound itself is untouched: it still applies to every element that survives
 * the type test, and still without waiting for payload bytes. The wire layout of
 * the isolates mirrors Crucible's Probe: string_array at id 200 (items string,
 * count 5), blob_array at 201 (items blob), struct_array at 202 (items struct),
 * so `c6 0c` / `ce 0c` / `d6 0c` open the three wrappers. --- */

/* struct element of the id-202 wrapper: two scalar fields, §2-sparse. */
struct F41Row : sofab::Message
{
    uint64_t k = 0;
    uint64_t v = 0;
    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        os.writeIf(0, k, k != 0);
        return os.writeIf(1, v, v != 0);
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 0) is.read(k);
        else if (id == 1) is.read(v);
    }
};

/* The three count-5 wrapper arrays, collected exactly as generated code does. */
struct F41Msg : sofab::IStreamMessage
{
    std::vector<std::string> strs;              /* id 200 -- string, count 5 */
    std::vector<std::vector<uint8_t>> blobs;    /* id 201 -- blob,   count 5 */
    std::vector<F41Row> rows;                   /* id 202 -- struct, count 5 */
    uint64_t other = 0;                         /* id 8   -- a plain scalar  */

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 200: { sofab::StringSeq c{strs, 5, 64};  is.read(c); break; }
            case 201: { sofab::BlobSeq   c{blobs, 5, 64}; is.read(c); break; }
            case 202: { sofab::MessageSeq<F41Row> c; c.out = &rows; c.cap = 5; is.read(c); break; }
            case 8:   is.read(other); break;
        }
    }
};

/* Generated code (sofabgen) collects struct wrappers with a helper of its own
 * that carries `cap` but declares no element type, and applies both rules itself
 * -- in the §7.3-first order. A collector that keeps the bound this way must be
 * left to it: the stream applies the bound only for a collector that also says
 * what its elements are, or it would pre-empt the type test at the header. */
struct F41GenSeq : sofab::IStreamMessage
{
    std::vector<F41Row> *out = nullptr;
    long cap = -1;

    void prepare() noexcept { if (out != nullptr) out->clear(); }

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        using Tag = decltype(is.wire());
        if (is.wire() != Tag::SequenceStart) return;                    /* §7.3 */
        if (cap >= 0 && static_cast<long>(id) >= cap) { is.invalidate(); return; }
        while (out->size() <= static_cast<size_t>(id)) out->emplace_back();
        is.read((*out)[id]);
    }
};

struct F41GenMsg : sofab::IStreamMessage
{
    std::vector<F41Row> rows;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 202) { F41GenSeq c; c.out = &rows; c.cap = 5; is.read(c); }
    }
};

static void overIndexSkipOrdering()
{
    auto feed = [](const char *hex) {
        sofab::IStreamObject<F41Msg> in;
        auto w = fromHex(hex);
        auto r = in.feed(w.data(), w.size());
        return std::pair<sofab::IStreamImpl::Result, F41Msg>{r, *in};
    };

    /* ---- THE ISOLATE: id 8 (>= count 5) carrying UNSIGNED where a string is
     *      declared. §7.3 skips it, so no element and no index ever exist and the
     *      whole message is all-default. ---- */
    {
        auto [r, m] = feed("c60c 40 01 07");
        CHECK(r.complete(), "F-0041: over-index + mis-typed string element is skipped, not INVALID");
        CHECK(m.strs.empty(), "F-0041: the skipped element leaves the array empty");
        CHECK(r.skipped() == 1, "F-0041: the skip is counted as a §7.3 skip");
    }

    /* ---- CONTROL 1: over-index but CORRECTLY typed (fixlen word 0x0a = len 1,
     *      subtype string). It survives §7.3, so the bound applies and must still
     *      reject -- this is the whole §5.1/§7 rule. ---- */
    {
        auto [r, m] = feed("c60c 42 0a 41 07");
        (void)m;
        CHECK(r.invalid(), "F-0041 control: a well-typed over-index element is still INVALID");
    }

    /* ---- CONTROL 2: in-range id 2, mis-typed. The plain §7.3 skip, unchanged. ---- */
    {
        auto [r, m] = feed("c60c 10 01 07");
        CHECK(r.complete() && m.strs.empty(), "F-0041 control: in-range mis-typed element is skipped");
    }

    /* ---- the subtype counts too: fixlen matches the declared wire type, but
     *      fixlen word 0x0b = (1<<3)|3 says BLOB where string is declared. §7.3
     *      covers subtype mismatch, so this is skipped like the isolate -- an
     *      implementation gating only on the header wire type still rejects it. ---- */
    {
        auto [r, m] = feed("c60c 42 0b 41 07");
        CHECK(r.complete() && m.strs.empty(),
              "F-0041: over-index element with a contradicting SUBTYPE is skipped");
    }
    {   /* the same subtype mismatch at an in-range id: the §7.3 half alone */
        auto [r, m] = feed("c60c 12 0b 41 07");
        CHECK(r.complete() && m.strs.empty(),
              "F-0041 control: in-range element with a contradicting subtype is skipped");
    }

    /* ---- the only window that relaxes: the message ends BETWEEN an over-index
     *      element header and its fixlen word. The subtype -- and with it whether
     *      the field is an element at all -- is not yet decidable, so this is
     *      INCOMPLETE, not INVALID (§5.2; the analogue of §4.8's ruling for the
     *      fixlen array's two words). ---- */
    {
        auto [r, m] = feed("c60c 42");
        (void)m;
        CHECK(r.incomplete(), "F-0041: truncation before the element's fixlen word is INCOMPLETE");
    }
    /* ...but from the fixlen word on the reject is immediate: a declared length of
     * 33 with no payload byte present is INVALID, never INCOMPLETE. */
    {
        auto [r, m] = feed("c60c 42 8a02");
        (void)m;
        CHECK(r.invalid(), "F-0041: the bound does not wait for the payload once the subtype is known");
    }

    /* ---- the skip leaves nothing behind (§5.1 length, §7.4): a valid element 0
     *      followed by a mis-typed over-index element decodes to the array of just
     *      the valid element -- length 1, not 9, and not an error. ---- */
    {
        auto [r, m] = feed("c60c 02 0a 41 40 01 07");
        CHECK(r.complete(), "F-0041: a valid element followed by a skipped one stays COMPLETE");
        CHECK(m.strs.size() == 1 && m.strs[0] == "A",
              "F-0041: the skipped id does not extend the array (length 1)");
    }

    /* ---- format-level rejects still fire on the skipped field's own metadata:
     *      §7.3 subordinates the SCHEMA bound only (CORELIB_PLAN §4.8). ---- */
    {   /* an over-index array element whose count varint exceeds 64 bits */
        auto [r, m] = feed("c60c 43 ffffffffffffffffffff7f");
        (void)m;
        CHECK(r.invalid(), "F-0041: an over-64-bit varint in a skipped element is still INVALID");
    }
    {   /* reserved fixlen subtype 4 (word 0x0c) at an over-index id */
        auto [r, m] = feed("c60c 42 0c 41 07");
        (void)m;
        CHECK(r.invalid(), "F-0041: a reserved fixlen subtype in a skipped element is still INVALID");
    }
    {   /* fp32 with length 1 (word 0x08) at an over-index id: §4.6 length rule */
        auto [r, m] = feed("c60c 42 08 41 07");
        (void)m;
        CHECK(r.invalid(), "F-0041: a bad fp32 fixlen length in a skipped element is still INVALID");
    }

    /* ---- type-generic, not string-specific: the same three cases on the blob
     *      wrapper (id 201), with the subtype test running the other way. ---- */
    {
        auto [r, m] = feed("ce0c 40 01 07");
        CHECK(r.complete() && m.blobs.empty(), "F-0041: blob wrapper skips the mis-typed over-index element");
    }
    {
        auto [r, m] = feed("ce0c 42 0b 41 07");
        (void)m;
        CHECK(r.invalid(), "F-0041 control: a well-typed over-index blob element is still INVALID");
    }
    {
        auto [r, m] = feed("ce0c 42 0a 41 07");
        CHECK(r.complete() && m.blobs.empty(),
              "F-0041: a STRING-subtyped over-index element in a blob array is skipped");
    }

    /* ---- and on a struct wrapper (id 202), whose elements are sequences: the
     *      header alone settles §7.3 there, so nothing is deferred. ---- */
    {
        auto [r, m] = feed("d60c 40 01 07");
        CHECK(r.complete() && m.rows.empty(), "F-0041: struct wrapper skips the mis-typed over-index element");
    }
    {
        auto [r, m] = feed("d60c 46 07 07");
        (void)m;
        CHECK(r.invalid(), "F-0041 control: a well-typed over-index struct element is still INVALID");
    }

    /* ---- in-range elements of every wrapper decode exactly as before. ---- */
    {
        auto [r, m] = feed("c60c 02 0a41 12 0a42 07  ce0c 02 0b43 07  d60c 06 0005 07 07");
        CHECK(r.complete(), "F-0041: the three wrappers still decode COMPLETE");
        CHECK(m.strs.size() == 3 && m.strs[0] == "A" && m.strs[1].empty() && m.strs[2] == "B",
              "F-0041: in-range string elements land at their index");
        CHECK(m.blobs.size() == 1 && m.blobs[0] == std::vector<uint8_t>{'C'},
              "F-0041: in-range blob element decodes");
        CHECK(m.rows.size() == 1 && m.rows[0].k == 5, "F-0041: in-range struct element decodes");
    }

    /* ---- §7.4: a mis-typed wrapper OCCURRENCE still does not wipe a valid
     *      earlier one -- that gate is upstream of all of this. ---- */
    {
        auto [r, m] = feed("c60c 02 0a41 07  c00c 01");
        CHECK(r.complete(), "F-0041: a mis-typed wrapper occurrence is skipped (§7.4)");
        CHECK(m.strs.size() == 1 && m.strs[0] == "A", "F-0041: it does not wipe the valid earlier array");
    }

    /* ---- the element bound does not leak out of the wrapper: the same id 8 that
     *      is over-index INSIDE the array is an ordinary field id outside it. ---- */
    {
        auto [r, m] = feed("c60c 02 0a41 07  40 09");
        CHECK(r.complete() && m.other == 9, "F-0041: the element bound is not applied at the outer level");
    }

    /* ---- the same three verdicts through a collector that keeps the bound to
     *      itself (the shape generated code uses). ---- */
    {
        auto genFeed = [](const char *hex) {
            sofab::IStreamObject<F41GenMsg> in;
            auto w = fromHex(hex);
            auto r = in.feed(w.data(), w.size());
            return std::pair<sofab::IStreamImpl::Result, F41GenMsg>{r, *in};
        };
        {
            auto [r, m] = genFeed("d60c 40 01 07");
            CHECK(r.complete() && m.rows.empty(),
                  "F-0041: a self-bounding collector still sees the mis-typed over-index element");
        }
        {
            auto [r, m] = genFeed("d60c 46 07 07");
            (void)m;
            CHECK(r.invalid(), "F-0041: a self-bounding collector still rejects a well-typed over-index element");
        }
        {
            auto [r, m] = genFeed("d60c 06 0005 07 07");
            CHECK(r.complete() && m.rows.size() == 1 && m.rows[0].k == 5,
                  "F-0041: a self-bounding collector decodes an in-range element");
        }
    }
}

/* --- the element-index bound is suspended inside a SKIPPED subtree (Crucible
 *     F-0051, corelib-cpp#65).
 *
 * A field skipped inside a wrapper sequence -- because its wire tag contradicts
 * the declared element type (§7.3) or because its id is simply unknown -- never
 * became the array's value, so the fields nested INSIDE it are not the array's
 * elements either. Their ids are child ids of that field, not array indices, and
 * the §5.1/§7 over-index bound must not measure them: it belongs to the wrapper
 * level alone.
 *
 * The control that separates this from an enter-and-bind defect is the child id:
 * with an in-range child the element is skipped correctly, so the subtree is
 * demonstrably never entered as an element -- only the bound leaked in. Same
 * fixture as F-0041 above: string_array at id 200, items string, count 5. --- */

static void skippedSubtreeSuspendsBound()
{
    auto feed = [](const char *hex) {
        sofab::IStreamObject<F41Msg> in;
        auto w = fromHex(hex);
        auto r = in.feed(w.data(), w.size());
        return std::pair<sofab::IStreamImpl::Result, F41Msg>{r, *in};
    };

    /* ---- THE ISOLATE: element index 4 (IN RANGE) opened as a sequence, which
     *      §7.3 skips; inside it a string at child id 5. That 5 is over the
     *      wrapper's count, but it is not an element index -- the field holding
     *      it was never an element. ---- */
    {
        auto [r, m] = feed("c60c 26 2a 0a41 07 07");
        CHECK(r.complete(), "F-0051: an over-index CHILD of a skipped element is not INVALID");
        CHECK(m.strs.empty(), "F-0051: the skipped element leaves the array empty");
        CHECK(r.skipped() == 1, "F-0051: only the element itself is counted as a §7.3 skip");
    }

    /* ---- CONTROL: the same bytes with an IN-RANGE child id. This is what the
     *      isolate must decode to; it also proves the subtree is skipped rather
     *      than entered -- were it entered, the child would bind an element. ---- */
    {
        auto [r, m] = feed("c60c 26 02 0a41 07 07");
        CHECK(r.complete() && m.strs.empty(),
              "F-0051 control: an in-range child of a skipped element decodes to the empty array");
    }

    /* ---- not specific to §7.3: an UNKNOWN id (50) skipped inside the wrapper is
     *      a different reason to skip, same subtree shape, same verdict. ---- */
    {
        auto [r, m] = feed("c60c 9603 2a 0a41 07 07");
        CHECK(r.complete() && m.strs.empty(),
              "F-0051: the bound does not leak into an unknown id's subtree either");
    }

    /* ---- and at depth: a skipped subtree nested inside a skipped subtree. ---- */
    {
        auto [r, m] = feed("c60c 26 2e 2a 0a41 07 07 07");
        CHECK(r.complete() && m.strs.empty(),
              "F-0051: the suspension covers nested skipped subtrees");
    }

    /* ---- the bound is SUSPENDED, not dropped: it is armed again the moment the
     *      skip ends, so a well-typed over-index element AFTER one still rejects
     *      (§5.1/§7). ---- */
    {
        auto [r, m] = feed("c60c 26 2a 0a41 07 42 0a41 07");
        (void)m;
        CHECK(r.invalid(), "F-0051: the bound is restored after the skipped subtree");
    }
    {   /* ...and one BEFORE the skipped subtree is rejected as it always was */
        auto [r, m] = feed("c60c 42 0a41 07 26 2a 0a41 07 07");
        (void)m;
        CHECK(r.invalid(), "F-0051: an over-index element before the skip still rejects");
    }

    /* ---- valid elements around a skipped subtree still land at their index. ---- */
    {
        auto [r, m] = feed("c60c 02 0a41 26 2a 0a41 07 12 0a42 07");
        CHECK(r.complete(), "F-0051: a skipped subtree between valid elements stays COMPLETE");
        CHECK(m.strs.size() == 3 && m.strs[0] == "A" && m.strs[1].empty() && m.strs[2] == "B",
              "F-0051: the skipped subtree does not disturb the surrounding elements");
    }

    /* ---- format-level rejects still fire INSIDE the skipped subtree: §7.3
     *      subordinates the schema bound only (CORELIB_PLAN §4.8). ---- */
    {   /* an over-64-bit varint in the child of a skipped element */
        auto [r, m] = feed("c60c 26 43 ffffffffffffffffffff7f 07 07");
        (void)m;
        CHECK(r.invalid(), "F-0051: an over-64-bit varint inside the skipped subtree is still INVALID");
    }
    {   /* a reserved fixlen subtype (word 0x0c) in the child of a skipped element */
        auto [r, m] = feed("c60c 26 2a 0c41 07 07");
        (void)m;
        CHECK(r.invalid(), "F-0051: a reserved fixlen subtype inside the skipped subtree is still INVALID");
    }

    /* ---- the mirror case, unchanged: a subtree that IS read (a well-typed struct
     *      element of the id-202 wrapper) carries the bound of its own collector,
     *      not the outer wrapper's -- so a child field id past the outer count is
     *      an ordinary unknown field there. ---- */
    {
        auto [r, m] = feed("d60c 06 4009 07 07");
        CHECK(r.complete() && m.rows.size() == 1,
              "F-0051: an over-count child id inside a READ element is an ordinary unknown id");
    }
}

/* --- a nested field that runs out of bytes is unfinished, not declined
 *     (Crucible F-0056, corelib-cpp#71).
 *
 * Inside a sequence, a field the callback did not read is skipped by rewinding to
 * its payload and consuming it by length. A field whose bytes merely ran out looks
 * the same from the outside -- both come back unconsumed -- but it must NOT take
 * that path: the callback has already descended THROUGH the payload, so the
 * current-field metadata (wire type, count, element size) now describes whatever
 * innermost field the descent stopped at, and the rewind re-reads the outer bytes
 * under it.
 *
 * The visible symptom is a verdict decided by bytes no decoder may look at. A
 * fixlen array's payload is raw -- `count x elem_len` bytes consumed by length
 * (§4.8) -- yet re-parsed as varints its CONTENT starts to matter: twelve
 * continuation bytes (`ff`) exceed §4.1's 10-byte varint bound and turn a
 * truncation that is plainly INCOMPLETE into INVALID, while the same twelve bytes
 * with bit 7 clear in every fourth do not. Truncation inside a field that must be
 * skipped is INCOMPLETE (§7, §7.3); the whole top-level field is buffered and
 * delivered again.
 *
 * Wire layout: `a6 06` opens `arrays` (id 100), `56` opens `nested` (id 10), then
 * `05 03 20` is a 3-element fp32 array at id 0, and `04 24` a mistyped ARRAY_SIGNED
 * at that same id -- skipped under §7.3, and truncated where the bytes end. --- */

struct F56Inner : sofab::IStreamMessage
{
    std::vector<float> vals; /* id 0 -- fp32 array, count 5 */
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 0) is.readArray(vals, 5);
    }
};
struct F56Nested : sofab::IStreamMessage
{
    F56Inner inner; /* id 10 */
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 10) is.read(inner);
    }
};
struct F56Msg : sofab::IStreamMessage
{
    F56Nested nested; /* id 100 */
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 100) is.read(nested);
    }
};

static void truncatedNestedFieldIsNotDeclined()
{
    auto feed = [](const char *hex) {
        sofab::IStreamObject<F56Msg> in;
        auto w = fromHex(hex);
        auto r = in.feed(w.data(), w.size());
        return std::pair<sofab::IStreamImpl::Result, F56Msg>{r, *in};
    };

    /* ---- THE ISOLATE: the fp32 payload is twelve continuation bytes. ---- */
    {
        auto [r, m] = feed("a606 56 05 03 20 ffffffff ffffffff ffffffff 04 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056: truncation inside a skipped field after a fixlen array is INCOMPLETE");
    }
    {   /* the other all-continuation payload: the float value is irrelevant */
        auto [r, m] = feed("a606 56 05 03 20 80808080 80808080 80808080 04 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056: an 0x80 payload reaches the same verdict");
    }

    /* ---- CONTROLS: identical length, element count and fixlen word -- only the
     *      payload BYTES differ, and no payload byte may change a verdict. ---- */
    {
        auto [r, m] = feed("a606 56 05 03 20 ffffff7f ffffff7f ffffff7f 04 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056 control: a terminator every fourth byte is the same verdict");
    }
    {   /* 1.0f, an entirely ordinary value */
        auto [r, m] = feed("a606 56 05 03 20 0000803f 0000803f 0000803f 04 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056 control: an ordinary float payload is the same verdict");
    }
    {   /* two elements: eight bytes stay under §4.1's varint bound */
        auto [r, m] = feed("a606 56 05 02 20 ffffffff ffffffff 04 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056 control: below the varint bound is the same verdict");
    }

    /* ---- the trailing field is not what makes it INCOMPLETE: with the array
     *      complete and nothing truncated, the same bytes decode. ---- */
    {
        auto [r, m] = feed("a606 56 05 03 20 0000803f 00000040 00004040 07 07");
        CHECK(r.complete(), "F-0056 control: the untruncated message is COMPLETE");
        CHECK(m.nested.inner.vals.size() == 3 && m.nested.inner.vals[0] == 1.0f &&
              m.nested.inner.vals[1] == 2.0f && m.nested.inner.vals[2] == 3.0f,
              "F-0056 control: the fp32 elements decode");
    }

    /* ---- nor is §7.3: an UNKNOWN id (7) truncated in the same place is a
     *      different reason to skip, same shape, same verdict. ---- */
    {
        auto [r, m] = feed("a606 56 05 03 20 ffffffff ffffffff ffffffff 3c 24 07 07");
        (void)m;
        CHECK(r.incomplete(), "F-0056: an unknown id truncated after the array is INCOMPLETE too");
    }

    /* ---- INCOMPLETE means resumable: the buffered field is delivered again and
     *      the message completes when the missing bytes arrive. ---- */
    {
        sofab::IStreamObject<F56Msg> in;
        auto head = fromHex("a606 56 05 03 20 ffffffff ffffffff ffffffff 04 24 07 07");
        /* 34 more single-byte elements finish the 36-element skipped array */
        std::vector<uint8_t> tail(34, 0x01);
        tail.push_back(0x07);
        tail.push_back(0x07);
        CHECK(in.feed(head.data(), head.size()).incomplete(),
              "F-0056: the truncated tail is buffered, not rejected");
        CHECK(in.feed(tail.data(), tail.size()).complete(),
              "F-0056: the skipped array completes on the next feed");
        CHECK((*in).nested.inner.vals.size() == 3,
              "F-0056: the fp32 array before it survives the re-delivery");
    }

    /* ---- and byte at a time, which drives the resume path on every boundary. ---- */
    {
        sofab::IStreamObject<F56Msg> in;
        auto w = fromHex("a606 56 05 03 20 0000803f 00000040 00004040 04 02 0101 0707");
        bool sawInvalid = false;
        sofab::DecodeStatus last = sofab::DecodeStatus::Incomplete;
        for (uint8_t b : w)
        {
            auto r = in.feed(&b, 1);
            if (r.invalid()) sawInvalid = true;
            last = r.status();
        }
        CHECK(!sawInvalid, "F-0056: dribbled byte by byte, no chunk boundary is INVALID");
        CHECK(last == sofab::DecodeStatus::Complete, "F-0056: the dribbled message ends COMPLETE");
        CHECK((*in).nested.inner.vals.size() == 3 && (*in).nested.inner.vals[2] == 3.0f,
              "F-0056: the dribbled decode produces the same values");
    }
}

/* --- heap-free storage: FixedString / FixedBytes / InlineVector as decode
 *     destinations.
 *
 * The same schema may be lowered to growable containers or to the heap-free
 * ones. Two properties have to hold for that choice to be a STORAGE decision
 * and nothing more:
 *
 *   1. the wire is identical, in both directions;
 *   2. the heap-free decode allocates nothing at all -- which is the whole
 *      point, and is checked here against the operator-new counter rather
 *      than asserted.
 *
 * Everything else is the ordinary spec behaviour, re-checked on the new
 * destinations because they take a different branch inside readString /
 * readBlob: §7.1 rejects an over-capacity payload instead of truncating it,
 * §7.3 leaves a mis-typed field untouched, §5.1 places elements at their id,
 * and strict UTF-8 still rejects. --- */

/* Same message, twice: growable storage and heap-free storage. Field ids and
 * declared bounds are identical, so the two must agree byte for byte. */
struct DynStoreMsg : sofab::Message
{
    std::string name;                            /* id 1 -- maxlen 8  */
    std::vector<uint8_t> sig;                    /* id 2 -- maxlen 4  */
    std::vector<uint32_t> nums;                  /* id 3 -- count 4   */
    std::vector<std::string> tags;               /* id 4 -- count 3, maxlen 2 */
    std::vector<std::vector<uint8_t>> parts;     /* id 5 -- count 3, maxlen 2 */

    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        (void)os.write(1, std::string_view{name});
        (void)os.write(2, sig.data(), static_cast<int32_t>(sig.size()));
        (void)os.write(3, nums);
        (void)os.sequenceBeginLazy(4);
        for (size_t i = 0; i < tags.size(); ++i)
            if (!tags[i].empty() || i + 1 == tags.size())
                (void)os.write(static_cast<sofab::id>(i), std::string_view{tags[i]});
        (void)os.sequenceEnd();
        (void)os.sequenceBeginLazy(5);
        for (size_t i = 0; i < parts.size(); ++i)
            if (!parts[i].empty() || i + 1 == parts.size())
                (void)os.write(static_cast<sofab::id>(i), parts[i].data(),
                               static_cast<int32_t>(parts[i].size()));
        return os.sequenceEnd();
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 1: is.readString(name, 8); break;
            case 2: is.readBlob(sig, 4); break;
            case 3: is.readArray(nums, 4); break;
            case 4: { sofab::StringSeq c{tags, 3, 2};  is.read(c); break; }
            case 5: { sofab::BlobSeq   c{parts, 3, 2}; is.read(c); break; }
        }
    }
};

struct FixedStoreMsg : sofab::Message
{
    sofab::FixedString<8> name;
    sofab::FixedBytes<4> sig;
    sofab::InlineVector<uint32_t, 4> nums;
    sofab::InlineVector<sofab::FixedString<2>, 3> tags;
    sofab::InlineVector<sofab::FixedBytes<2>, 3> parts;

    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        (void)os.write(1, std::string_view{name});
        (void)os.write(2, sig.data(), static_cast<int32_t>(sig.size()));
        (void)os.write(3, nums);
        (void)os.sequenceBeginLazy(4);
        for (size_t i = 0; i < tags.size(); ++i)
            if (!tags[i].empty() || i + 1 == tags.size())
                (void)os.write(static_cast<sofab::id>(i), std::string_view{tags[i]});
        (void)os.sequenceEnd();
        (void)os.sequenceBeginLazy(5);
        for (size_t i = 0; i < parts.size(); ++i)
            if (!parts[i].empty() || i + 1 == parts.size())
                (void)os.write(static_cast<sofab::id>(i), parts[i].data(),
                               static_cast<int32_t>(parts[i].size()));
        return os.sequenceEnd();
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        switch (id)
        {
            case 1: is.readString(name, 8); break;
            case 2: is.readBlob(sig, 4); break;
            case 3: is.readArray(nums, 4); break;
            case 4: { sofab::StringSeq c{tags, 3, 2};  is.read(c); break; }
            case 5: { sofab::BlobSeq   c{parts, 3, 2}; is.read(c); break; }
        }
    }
};

static void heapFreeStorage()
{
    /* --- 1. the two storage modes produce the same bytes --- */
    DynStoreMsg d;
    d.name = "abcdefgh";
    d.sig = {0xde, 0xad, 0xbe, 0xef};
    d.nums = {1, 2, 300};
    d.tags = {"ab", "", "cd"};
    d.parts = {{0x01, 0x02}, {}, {0x03}};

    FixedStoreMsg f;
    f.name = "abcdefgh";
    f.sig = {0xde, 0xad, 0xbe, 0xef};
    f.nums = {1, 2, 300};
    f.tags = {"ab", "", "cd"};
    f.parts = {{0x01, 0x02}, {}, {0x03}};

    uint8_t db[256], fb[256];
    sofab::OStreamView dos{db, sizeof(db)}, fos{fb, sizeof(fb)};
    CHECK(d.serialize(dos).code() == sofab::Error::None, "heapfree: dynamic encode succeeds");
    CHECK(f.serialize(fos).code() == sofab::Error::None, "heapfree: fixed encode succeeds");
    const std::string dHex = toHex({db, dos.bytesUsed()});
    const std::string fHex = toHex({fb, fos.bytesUsed()});
    CHECK(dHex == fHex, "heapfree: fixed storage encodes the same bytes as growable storage");

    /* --- 2. decode into the heap-free destination allocates NOTHING --- */
    FixedStoreMsg in;
    const unsigned long before = g_allocCount;
    {
        sofab::IStreamObject<FixedStoreMsg> is;
        CHECK(is.feed(fb, fos.bytesUsed()).complete(), "heapfree: decode completes");
        CHECK((*is).name == std::string_view{"abcdefgh"}, "heapfree: FixedString value");
        CHECK((*is).sig.size() == 4 && (*is).sig[0] == 0xde && (*is).sig[3] == 0xef,
              "heapfree: FixedBytes value");
        CHECK((*is).nums.size() == 3 && (*is).nums[2] == 300,
              "heapfree: InlineVector native array keeps the wire length");
        CHECK((*is).tags.size() == 3 && (*is).tags[0] == std::string_view{"ab"} &&
                  (*is).tags[1].empty() && (*is).tags[2] == std::string_view{"cd"},
              "heapfree: StringSeq places elements at their id and fills the gap");
        CHECK((*is).parts.size() == 3 && (*is).parts[0].size() == 2 &&
                  (*is).parts[1].empty() && (*is).parts[2].size() == 1,
              "heapfree: BlobSeq places elements at their id and fills the gap");
        in = *is;
    }
    CHECK(g_allocCount == before, "heapfree: decoding into heap-free storage allocates nothing");

    /* Re-encoding what was decoded reproduces the wire exactly. */
    sofab::OStreamView ros{db, sizeof(db)};
    (void)in.serialize(ros);
    CHECK(toHex({db, ros.bytesUsed()}) == fHex, "heapfree: decode -> encode round-trips byte-exact");

    /* --- 3. §7.1: a payload past the destination's capacity is INVALID, never
     *     truncated. `name` is FixedString<8>; feed nine characters with the
     *     declared bound lifted so the capacity itself is what rejects. --- */
    {
        struct NoBoundMsg : sofab::IStreamMessage
        {
            sofab::FixedString<8> name;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id == 1) is.readString(name); /* no declared maxlen */
            }
        };
        uint8_t buf[64];
        sofab::OStreamView os{buf, sizeof(buf)};
        (void)os.write(1, std::string_view{"123456789"}); /* 9 > capacity 8 */
        sofab::IStreamObject<NoBoundMsg> is;
        CHECK(is.feed(buf, os.bytesUsed()).invalid(),
              "heapfree: payload past FixedString capacity is INVALID, not truncated");
        CHECK((*is).name.empty(), "heapfree: a rejected payload leaves the destination untouched");
    }

    /* --- 4. §7.3: a field whose wire type contradicts the declared one is
     *     skipped and the heap-free destination is left alone. --- */
    {
        struct TypedMsg : sofab::IStreamMessage
        {
            sofab::FixedString<8> name{"keep"};
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id == 1) is.readString(name, 8);
            }
        };
        uint8_t buf[64];
        sofab::OStreamView os{buf, sizeof(buf)};
        (void)os.write(1, static_cast<uint64_t>(7)); /* unsigned, not a string */
        sofab::IStreamObject<TypedMsg> is;
        CHECK(is.feed(buf, os.bytesUsed()).complete(),
              "heapfree: a wire-type mismatch is a skip, not an error");
        CHECK((*is).name == std::string_view{"keep"},
              "heapfree: the skipped field leaves the heap-free destination untouched");
    }

#if SOFAB_STRICT_UTF8
    /* --- 5. §6.4 still applies on the heap-free branch. --- */
    {
        struct Utf8Msg : sofab::IStreamMessage
        {
            sofab::FixedString<8> s;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            {
                if (id == 5) is.readString(s, 8);
            }
        };
        /* Built by hand: the encoder refuses to emit C0 80 at all
         * (InvalidArgument, checked in strictUtf8()), so the wire has to come
         * from stringFieldWire, exactly as the growable-destination case does. */
        auto w = stringFieldWire("\xC0\x80", sofab::detail::Fix::String); /* overlong NUL */
        sofab::IStreamObject<Utf8Msg> is;
        CHECK(is.feed(w.data(), w.size()).invalid(),
              "heapfree: invalid UTF-8 into a FixedString is INVALID");
        CHECK((*is).s.empty(), "heapfree: the rejected UTF-8 payload is not stored");
    }
#endif
}

/* --- destination reuse across messages (MESSAGE_SPEC §2 + §5.1).
 *
 * §2 omits an all-default field, so a message that does not carry a field
 * delivers nothing for it and the destination keeps whatever the previous decode
 * left there. For a wrapper array that is new since §2: the collector clears its
 * destination in prepare(), which only runs when the wrapper sequence is
 * PRESENT. §5.1 puts the duty to supply a clean destination on the decoding
 * side, and reset() is how a caller discharges it. --- */

static void destinationReuse()
{
    const auto msgA_tags = fromHex("0e020a780a0a7907"); /* tags = [x, y]       */
    const auto msgA_rows = fromHex("1606070e00070707"); /* rows = [{}, {a=7}]  */
    const auto msgB      = fromHex("4801");             /* only id 9 = 1       */

    /* --- reset() is the supported path: message B decodes on a clean slate,
     *     so an absent array field reads as the empty array §2 requires. --- */
    {
        sofab::IStreamObject<SeqMsg> in;
        in.feed(msgA_tags.data(), msgA_tags.size());
        CHECK((*in).tags.size() == 2, "reuse: message A decodes tags = [x, y]");
        in.reset();
        auto r = in.feed(msgB.data(), msgB.size());
        CHECK(r.complete(), "reuse: message B decodes COMPLETE after reset()");
        CHECK((*in).tags.empty(), "reset(): an absent array field reads as the empty array (§2)");
        CHECK((*in).other == 1, "reset(): message B's own field is decoded");
    }
    {
        sofab::IStreamObject<SeqMsg> in;
        in.feed(msgA_rows.data(), msgA_rows.size());
        CHECK((*in).rows.size() == 2, "reuse: message A decodes 2 rows");
        in.reset();
        in.feed(msgB.data(), msgB.size());
        CHECK((*in).rows.empty(), "reset(): an absent MessageSeq field reads as the empty array");
    }
    {
        /* reset() also clears a scalar the second message does not carry... */
        sofab::IStreamObject<SeqMsg> in;
        auto scalar = fromHex("4809");
        in.feed(scalar.data(), scalar.size());
        CHECK((*in).other == 9, "reuse: message A decodes the scalar");
        in.reset();
        auto empty = fromHex("");
        auto r = in.feed(empty.data(), empty.size());
        CHECK(r.complete(), "reset(): the zero-byte all-default message is COMPLETE (§2)");
        CHECK((*in).other == 0, "reset(): an absent scalar reads as its default");
    }
    {
        /* ...and the decoder's own state: a truncated message A leaves a partial
         * field buffered, which must not bleed into message B. */
        sofab::IStreamObject<SeqMsg> in;
        auto truncated = fromHex("0e020a78" "0a0a"); /* element 1's payload missing */
        auto r = in.feed(truncated.data(), truncated.size());
        CHECK(r.incomplete(), "reuse: a truncated message A is INCOMPLETE");
        in.reset();
        auto r2 = in.feed(msgB.data(), msgB.size());
        CHECK(r2.complete(), "reset(): the buffered partial field is discarded");
        CHECK((*in).other == 1 && (*in).tags.empty(),
              "reset(): message B decodes cleanly after a truncated message A");
        CHECK(in.skipped() == 0, "reset(): the §7.3 skipped counter is cleared");
    }

    /* --- WITHOUT reset(): the contract is that this is NOT a second message but
     *     a continuation of the first, so message A's values survive. Pinned so
     *     the day this changes, it changes deliberately -- and documented as the
     *     reason reset() exists (README "One message per destination"). --- */
    {
        sofab::IStreamObject<SeqMsg> in;
        in.feed(msgA_tags.data(), msgA_tags.size());
        in.feed(msgB.data(), msgB.size());
        CHECK((*in).tags.size() == 2,
              "no reset(): feeds continue ONE message, so the array field persists");
        CHECK((*in).other == 1, "no reset(): the continuation's own field is decoded");
    }
    {
        /* the same continuation semantics is what makes chunked decoding work at
         * all: a field boundary is not a message boundary. */
        sofab::IStreamObject<SeqMsg> in;
        auto whole = fromHex("0e020a780a0a7907" "4801");
        in.feed(whole.data(), 8);
        in.feed(whole.data() + 8, whole.size() - 8);
        CHECK((*in).tags.size() == 2 && (*in).other == 1,
              "one message split at a field boundary decodes as one message");
    }

    /* reset() on a callback-driven stream clears the decoder; the caller's own
     * destinations are its business (there is no destination to reset here). */
    {
        int calls = 0;
        sofab::IStreamInline in([&](sofab::id, size_t, size_t) { ++calls; });
        auto truncated = fromHex("0e020a78" "0a0a");
        CHECK(in.feed(truncated.data(), truncated.size()).incomplete(),
              "IStreamInline: truncated input is INCOMPLETE");
        in.reset();
        auto r = in.feed(msgB.data(), msgB.size());
        CHECK(r.complete(), "IStreamInline: reset() drops the buffered tail");
        CHECK(calls >= 1, "IStreamInline: fields are still delivered after reset()");
    }
}

/* --- varint width sweep ----------------------------------------------------
 *
 * The array element paths encode and decode varints through windowed fast paths
 * that skip the per-byte bounds and overlong tests, and the encoder builds the
 * first eight bytes with a SWAR spread rather than a byte at a time. Those are
 * pure representation changes, so the check is byte-exactness against an
 * independent reference encoder — deliberately written in the "shift, then
 * decide whether to tag" form the fast paths do *not* use — across every varint
 * width, both boundaries of every width, and a pseudo-random spread.
 *
 * The same values are then re-encoded into an exactly-sized buffer and decoded
 * one byte at a time, which drives the non-windowed fallbacks: the encoder's
 * `fit == 0` tail and the decoder's short-window tail loop. */

static void refVarint(std::vector<uint8_t> &o, uint64_t v)
{
    do {
        uint8_t b = static_cast<uint8_t>(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        o.push_back(b);
    } while (v);
}

struct SweepU : sofab::IStreamMessage
{
    std::vector<uint64_t> v;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
    {
        if (id == 1) { v.assign(count, 0); is.read(v); }
    }
};
struct SweepI : sofab::IStreamMessage
{
    std::vector<int64_t> v;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
    {
        if (id == 1) { v.assign(count, 0); is.read(v); }
    }
};

static void varintWidthSweep()
{
    std::vector<uint64_t> vals{0, 1, UINT64_MAX};
    for (int k = 0; k < 64; ++k)
    {
        const uint64_t bit = uint64_t{1} << k;
        vals.push_back(bit);
        vals.push_back(bit - 1);
        vals.push_back(bit + 1);
    }
    for (int k = 1; k <= 9; ++k) /* the varint width boundaries themselves */
    {
        const uint64_t lim = uint64_t{1} << (7 * k);
        vals.push_back(lim - 1);
        vals.push_back(lim);
    }
    uint64_t x = 0x243F6A8885A308D3ull; /* deterministic xorshift64 spread */
    for (int i = 0; i < 512; ++i)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        vals.push_back(x);
    }

    /* expected bytes: header, count, then each element, all from the reference */
    std::vector<uint8_t> want;
    refVarint(want, (uint64_t{1} << 3) | 3 /* Wire::ArrayUnsigned */);
    refVarint(want, vals.size());
    for (uint64_t v : vals) refVarint(want, v);

    std::vector<uint8_t> got(want.size() + 64);
    sofab::OStreamView os(got.data(), got.size());
    os.write(1, std::span<const uint64_t>(vals.data(), vals.size()));
    CHECK(os.ok(), "varint sweep: the unsigned array encodes without error");
    CHECK(os.bytesUsed() == want.size() &&
              std::equal(want.begin(), want.end(), got.begin()),
          "varint sweep: every unsigned width is byte-exact vs the reference");

    sofab::IStreamObject<SweepU> inu;
    CHECK(inu.feed(got.data(), os.bytesUsed()).complete(), "varint sweep: unsigned array decodes COMPLETE");
    CHECK((*inu).v == vals, "varint sweep: every unsigned width round-trips");

    /* signed / zig-zag elements over the same bit patterns */
    std::vector<int64_t> svals;
    for (uint64_t v : vals) svals.push_back(static_cast<int64_t>(v));
    std::vector<uint8_t> swant;
    refVarint(swant, (uint64_t{1} << 3) | 4 /* Wire::ArraySigned */);
    refVarint(swant, svals.size());
    for (int64_t v : svals)
        refVarint(swant, (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));

    std::vector<uint8_t> sgot(swant.size() + 64);
    sofab::OStreamView sos(sgot.data(), sgot.size());
    sos.write(1, std::span<const int64_t>(svals.data(), svals.size()));
    CHECK(sos.ok() && sos.bytesUsed() == swant.size() &&
              std::equal(swant.begin(), swant.end(), sgot.begin()),
          "varint sweep: every signed width is byte-exact vs the reference");

    sofab::IStreamObject<SweepI> ini;
    CHECK(ini.feed(sgot.data(), sos.bytesUsed()).complete(), "varint sweep: signed array decodes COMPLETE");
    CHECK((*ini).v == svals, "varint sweep: every signed width round-trips");

    /* Exactly-sized buffer: the encoder runs out of full-varint windows before
     * the last elements, so they take the checked `fit == 0` path. */
    std::vector<uint8_t> tight(want.size());
    sofab::OStreamView tos(tight.data(), tight.size());
    tos.write(1, std::span<const uint64_t>(vals.data(), vals.size()));
    CHECK(tos.ok() && tos.bytesUsed() == want.size() && tight == want,
          "varint sweep: an exactly-sized buffer produces the same bytes");

    /* One byte at a time: the decoder never sees a full varint window, so every
     * element goes through the short-tail loop and the reassembly path. */
    sofab::IStreamObject<SweepU> inc;
    sofab::DecodeStatus last = sofab::DecodeStatus::Incomplete;
    for (size_t i = 0; i < tight.size(); ++i)
        last = inc.feed(tight.data() + i, 1).status();
    CHECK(last == sofab::DecodeStatus::Complete, "varint sweep: byte-at-a-time decode is COMPLETE");
    CHECK((*inc).v == vals, "varint sweep: byte-at-a-time decode recovers every value");
}

/* --- ARRAY_MAX binds a fixlen array's element_count at EVERY nesting level
 *     (CORELIB_PLAN §4.8 step 1, §6.2). The top-level header path always
 *     enforced it; the nested one (dispatchLevel) did not, and the omission was
 *     not merely a missing INVALID: the skip computes `count * element_size`,
 *     which wraps size_t for a large enough count, so the array was skipped as
 *     though it were empty and a message that must be INVALID decoded COMPLETE.
 *     The integer-array types were never affected — they carry no element size
 *     to multiply by — which is why the two branches drifted apart. --- */

static void nestedArrayCountCeiling()
{
    struct Nested : sofab::IStreamMessage {
        void deserialize(sofab::IStreamImpl &, sofab::id, size_t, size_t) noexcept override {}
    };
    auto outcome = [](const std::vector<uint8_t> &b) {
        sofab::IStreamObject<Nested> in;
        return in.feed(b.data(), b.size()).code();
    };

    /* seq id1 { id0 = fixlen array, count = 2^31, fp32 elements } */
    {
        std::vector<uint8_t> b = {0x0e, 0x05};
        appendVarint(b, uint64_t{1} << 31);          /* one past ARRAY_MAX */
        b.push_back((4u << 3) | 0u);                 /* fp32, 4-byte elements */
        b.push_back(0x07);
        CHECK(outcome(b) == sofab::Error::InvalidMessage,
              "nested fixlen array: count above ARRAY_MAX is INVALID");
    }
    /* The dangerous one: 2^62 elements x 4 bytes is exactly 2^64, so the byte
     * span wraps to zero and the payload skip becomes a no-op. */
    {
        std::vector<uint8_t> b = {0x0e, 0x05};
        appendVarint(b, uint64_t{1} << 62);
        b.push_back((4u << 3) | 0u);
        b.push_back(0x07);
        CHECK(outcome(b) == sofab::Error::InvalidMessage,
              "nested fixlen array: a count whose byte span wraps size_t is INVALID");
    }
    /* Same shape one level deeper, so the check is not merely on the first
     * nested level. */
    {
        std::vector<uint8_t> b = {0x0e, 0x0e, 0x05};
        appendVarint(b, uint64_t{1} << 62);
        b.push_back((8u << 3) | 1u);                 /* fp64 */
        b.push_back(0x07); b.push_back(0x07);
        CHECK(outcome(b) == sofab::Error::InvalidMessage,
              "nested fixlen array: the ceiling binds at depth 2 as well");
    }
    /* Control: a legal nested fixlen array is untouched by the new check. */
    {
        std::vector<uint8_t> b = {0x0e, 0x05, 0x02, (4u << 3) | 0u};
        for (int i = 0; i < 8; ++i) b.push_back(0x00);
        b.push_back(0x07);
        CHECK(outcome(b) == sofab::Error::None,
              "nested fixlen array: a legal two-element fp32 array still decodes");
    }
    /* Control: ARRAY_MAX itself is legal as a count -- the reject is above it,
     * not at it. The payload never arrives, so this is INCOMPLETE, not INVALID. */
    {
        std::vector<uint8_t> b = {0x0e, 0x05};
        appendVarint(b, sofab::ARRAY_MAX);
        b.push_back((4u << 3) | 0u);
        CHECK(outcome(b) == sofab::Error::Incomplete,
              "nested fixlen array: a count of exactly ARRAY_MAX is not itself INVALID");
    }
}

/* --- MIN_OUTPUT_BUFFER (§5.1): the smallest buffer this port accepts FOR
 *     STREAMING, declared so a caller can size a streaming buffer from the API
 *     instead of finding out at runtime. It binds a buffer installed together
 *     with a flush sink and no other buffer at all, and it is enforced where the
 *     buffer is handed over rather than partway through a message. Before the
 *     check existed, a zero-room buffer behind a sink drove pushByte past the
 *     end of the allocation. --- */

static void minOutputBuffer()
{
    static_assert(sofab::MIN_OUTPUT_BUFFER >= 1 && sofab::MIN_OUTPUT_BUFFER <= 20,
                  "§5.1: the declaration must be at least 1 and at most 20");

    const size_t min = sofab::MIN_OUTPUT_BUFFER;

    /* One byte short of the minimum, WITH a sink: rejected at installation. The
     * buffer is deliberately larger than the room granted, so an encoder that
     * ignored the rejection would still have somewhere to scribble and the
     * failure would show up as bytes rather than as a crash. */
    {
        std::vector<uint8_t> buf(min + 8, 0xAA);
        size_t handed = 0;
        sofab::OStreamView os([&handed](std::span<const uint8_t> d){ handed += d.size(); },
                              buf.data(), buf.size(), buf.size() - (min - 1));
        CHECK(!os.ok() && os.error() == sofab::Error::InvalidArgument,
              "MIN_OUTPUT_BUFFER: an undersized buffer with a sink is rejected where it is handed over");
        auto r = os.write(1, uint64_t{5});
        CHECK(r.code() == sofab::Error::InvalidArgument,
              "MIN_OUTPUT_BUFFER: a write on a rejected installation reports the rejection");
        CHECK(handed == 0 && os.bytesUsed() == 0,
              "MIN_OUTPUT_BUFFER: a rejected installation writes and flushes nothing");
    }

    /* An offset past the buffer end goes through the same mechanism. */
    {
        std::vector<uint8_t> buf(4);
        sofab::OStreamView os(buf.data(), buf.size(), 9);
        CHECK(!os.ok() && os.error() == sofab::Error::InvalidArgument,
              "MIN_OUTPUT_BUFFER: an out-of-range start offset is rejected the same way");
    }

    /* The converse, and the reason the constant is confined to the streaming
     * case: the same undersized buffer WITHOUT a sink is accepted, and a message
     * that fits encodes into it. A two-byte message must encode into two bytes
     * whatever this port declares. */
    {
        uint8_t buf[2];
        sofab::OStreamView os(buf, sizeof buf);
        auto r = os.write(0, uint64_t{0});
        CHECK(os.ok() && r.code() == sofab::Error::None && os.bytesUsed() == 2,
              "MIN_OUTPUT_BUFFER: no minimum applies without a sink (2-byte message, 2-byte buffer)");
        CHECK(buf[0] == 0x00 && buf[1] == 0x00,
              "MIN_OUTPUT_BUFFER: the sink-less two-byte encode produces the canonical bytes");
    }

    /* Encode into EXACTLY the declared minimum, over a payload far longer than
     * the buffer, so the divisible run of a string is split across many flushes
     * (§5.1). The concatenation must be byte-identical to the one-shot output. */
    {
        const std::string payload(3000, 'q');
        std::vector<uint8_t> one(4096);
        sofab::OStreamView oneShot(one.data(), one.size());
        oneShot.write(1, payload);
        CHECK(oneShot.ok(), "MIN_OUTPUT_BUFFER: the one-shot reference encode succeeds");
        one.resize(oneShot.bytesUsed());

        std::vector<uint8_t> out, buf(min, 0xAA);
        sofab::OStreamView os([&out](std::span<const uint8_t> d){ out.insert(out.end(), d.begin(), d.end()); },
                              buf.data(), buf.size(), 0);
        os.write(1, payload);
        os.flush();
        CHECK(os.ok() && out == one,
              "MIN_OUTPUT_BUFFER: encoding into exactly the minimum equals the one-shot bytes");
    }
}

/* --- The returning-flush-callback contract (§5.1). A sink either COPIES the
 *     bytes it was handed -- it returns without installing anything, and the
 *     encoder resumes in the same buffer at offset 0 -- or it TAKES the buffer
 *     and must install a replacement before returning. The encoder cannot tell
 *     the two apart by inspection, so the callback states it by what it does.
 *
 *     The start offset belongs to the INSTALLATION, not to the buffer, and is
 *     consumed: that is how a sink re-arms header room in every packet, where a
 *     bare return would not. Previously the cursor was reset to the buffer start
 *     unconditionally after the callback returned, which silently discarded a
 *     replacement's offset -- so only the very first packet ever got its
 *     reservation. --- */

static void flushHandover()
{
    constexpr size_t kCap = 16, kHdr = 4;
    const std::string payload(40, 'x');

    /* The reference: the same message encoded in one pass. */
    std::vector<uint8_t> one(256);
    sofab::OStreamView oneShot(one.data(), one.size());
    oneShot.write(1, payload);
    one.resize(oneShot.bytesUsed());

    /* A TAKING sink: it hands each filled buffer onward, scrubs the storage it
     * gave away, and installs the other buffer with kHdr bytes of header room.
     * An encoder that kept writing into the buffer it gave away would read back
     * the fill pattern; one that dropped the replacement's offset would produce
     * packets with no room in them. */
    {
        auto b1 = std::shared_ptr<uint8_t[]>(new uint8_t[kCap]);
        auto b2 = std::shared_ptr<uint8_t[]>(new uint8_t[kCap]);
        std::vector<std::vector<uint8_t>> packets;
        sofab::OStream *osp = nullptr;
        int taken = 0;
        auto sink = [&](std::span<const uint8_t> d) {
            packets.emplace_back(d.begin(), d.end());
            std::memset((taken % 2) ? b2.get() : b1.get(), 0xEE, kCap); /* scrub what was taken */
            ++taken;
            osp->setBuffer((taken % 2) ? b2 : b1, kCap, kHdr);
        };
        sofab::OStream os{sink, b1, kCap, kHdr};
        osp = &os;
        os.write(1, payload);
        os.flush();

        bool everyPacketHasRoom = !packets.empty();
        std::vector<uint8_t> joined;
        for (const auto &p : packets)
        {
            if (p.size() < kHdr) { everyPacketHasRoom = false; continue; }
            joined.insert(joined.end(), p.begin() + kHdr, p.end());
        }
        CHECK(os.ok(), "flush handover: an encode across a taking sink succeeds");
        CHECK(everyPacketHasRoom,
              "flush handover: a taking sink gets its start offset back in every packet");
        CHECK(joined == one,
              "flush handover: a taking sink's concatenated payload equals the one-shot bytes");
        CHECK(packets.size() > 1,
              "flush handover: the buffer really was handed over more than once");
    }

    /* A COPYING sink: it returns without installing anything, so the same buffer
     * is reused from offset 0. The reservation belongs to the first installation
     * only, so it appears in the first packet and nowhere else. */
    {
        std::vector<uint8_t> buf(kCap, 0xAA), out;
        size_t firstPacket = 0;
        sofab::OStreamView os([&](std::span<const uint8_t> d) {
                                  if (out.empty()) firstPacket = d.size();
                                  out.insert(out.end(), d.begin(), d.end());
                              },
                              buf.data(), kCap, kHdr);
        os.write(1, payload);
        os.flush();
        CHECK(os.ok(), "flush handover: an encode across a copying sink succeeds");
        CHECK(firstPacket == kCap,
              "flush handover: a copying sink's first packet carries the reserved head");
        CHECK(out.size() == one.size() + kHdr &&
              std::memcmp(out.data() + kHdr, one.data(), one.size()) == 0,
              "flush handover: a copying sink resumes at offset 0, so the head is reserved once");
    }

    /* A sink that takes the buffer on its LAST handover is left holding a fresh
     * buffer that contains nothing but its own reservation. Draining that as a
     * packet of its own would invent an empty packet, so the closing flush (and
     * the destructor's) must stay silent. */
    {
        auto b1 = std::shared_ptr<uint8_t[]>(new uint8_t[kCap]);
        auto b2 = std::shared_ptr<uint8_t[]>(new uint8_t[kCap]);
        int calls = 0, afterExplicitFlush = 0;
        {
            sofab::OStream *osp = nullptr;
            int taken = 0;
            auto sink = [&](std::span<const uint8_t>) {
                ++calls;
                ++taken;
                osp->setBuffer((taken % 2) ? b2 : b1, kCap, kHdr);
            };
            sofab::OStream os{sink, b1, kCap, kHdr};
            osp = &os;
            os.write(1, payload);
            os.flush();
            afterExplicitFlush = calls;
        }
        CHECK(afterExplicitFlush > 1, "flush handover: the closing taking sink ran");
        CHECK(calls == afterExplicitFlush,
              "flush handover: a freshly installed buffer holding only its reservation is not flushed");
    }
}

/* --- Chunk lifetime (CORELIB_PLAN §6, §7.2 item 4): a fed chunk is borrowed
 *     ONLY for the duration of the feed() call. Once it returns, the caller may
 *     reuse, overwrite or free that memory and the decoded message must be
 *     unaffected — so a decode copies every string and blob out before returning
 *     rather than binding a destination to chunk memory.
 *
 *     Each chunk here is therefore handed in from a throwaway buffer that is
 *     scrubbed with a fill byte the instant feed() returns. A decoder holding a
 *     slice into it reads back the fill pattern, and nothing else in this file
 *     would notice: the ordinary chunked tests feed from one long-lived buffer,
 *     so a borrowed view stays accidentally valid there.
 *
 *     This is the property that cost read(std::string_view&) its place. That
 *     destination handed back a view into the bytes just parsed, which §6 permits
 *     only for a one-shot decode(buffer) whose buffer the caller keeps alive —
 *     and this port has no such separate entry point, since feed() is the only
 *     way in. It is now a compile error, so the guarantee below holds for every
 *     destination the API still offers. --- */

static void chunkLifetime()
{
    struct Payload : sofab::IStreamMessage
    {
        std::string s;
        std::vector<uint8_t> b;
        std::vector<uint64_t> a;
        sofab::FixedString<32> fs;
        void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        {
            switch (id)
            {
                case 1: is.readString(s); break;
                case 2: is.readBlob(b); break;
                case 3: is.readArray(a); break;
                case 4: is.readString(fs, 32); break;
            }
        }
    };

    /* Long enough that every field straddles several chunks at the small sizes
     * below, so both the in-place path and the reassembly path are exercised. */
    const std::string text(70, 'w');
    std::vector<uint8_t> blob(90);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint64_t> nums;
    for (uint64_t i = 0; i < 40; ++i) nums.push_back(i * 1234567u + i);

    sofab::OStreamInline<1024> os;
    os.write(1, text);
    os.write(2, blob.data(), static_cast<int32_t>(blob.size()));   /* raw-blob overload: not chainable */
    os.write(3, std::span<const uint64_t>(nums.data(), nums.size()))
      .write(4, std::string_view{"fixed-storage destination"});
    CHECK(os.ok(), "chunk lifetime: the reference message encodes");
    const std::vector<uint8_t> msg(os.data(), os.data() + os.bytesUsed());

    for (size_t chunk : {size_t{1}, size_t{3}, size_t{17}, msg.size()})
    {
        sofab::IStreamObject<Payload> in;
        sofab::DecodeStatus last = sofab::DecodeStatus::Incomplete;
        for (size_t i = 0; i < msg.size(); i += chunk)
        {
            const size_t n = std::min(chunk, msg.size() - i);
            /* A fresh allocation per chunk, so the scrub cannot be optimised away
             * and ASan flags a decoder that keeps the pointer past the call. */
            std::vector<uint8_t> tmp(msg.begin() + static_cast<long>(i),
                                     msg.begin() + static_cast<long>(i + n));
            last = in.feed(tmp.data(), tmp.size()).status();
            std::memset(tmp.data(), 0xDD, tmp.size());   /* the chunk is ours again */
        }

        const auto &m = *in;
        const bool ok = last == sofab::DecodeStatus::Complete &&
                        m.s == text &&
                        m.b == blob &&
                        m.a == nums &&
                        m.fs == "fixed-storage destination";
        if (!ok)
            std::printf("  chunk size %zu: status=%d s=%zu/%zu b=%zu/%zu a=%zu/%zu\n",
                        chunk, static_cast<int>(last), m.s.size(), text.size(),
                        m.b.size(), blob.size(), m.a.size(), nums.size());
        CHECK(ok, "chunk lifetime: the decoded message survives every chunk being scrubbed after feed");
    }
}

int main()
{
    encodeVectors();
    roundtripScalars();
    roundtripArrays();
    roundtripNested();
    chunkedDecode();
    rawBlobReadTruncation();
    skippingUnknownFields();
    malformedInput();
    threeValuedOutcomes();
    callbackInvalidate();
    headerFirstBounds();
    elementWidthBound();
    callbackExceedLimit();
    wireTypeGuard();
    zeroLengthForms();
    lazySequenceFraming();
    deepHoldBack();
    bufferFullCondemnsTheRun();
    holdBackAllocationFailure();
    sequenceDepthBookkeeping();
    maxDepth();
    bufferLimits();
    strictUtf8();
    messageLayerFraming();
    wrapperArrayCollectors();
    overIndexSkipOrdering();
    skippedSubtreeSuspendsBound();
    truncatedNestedFieldIsNotDeclined();
    heapFreeStorage();
    destinationReuse();
    varintWidthSweep();
    nestedArrayCountCeiling();
    minOutputBuffer();
    flushHandover();
    chunkLifetime();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
