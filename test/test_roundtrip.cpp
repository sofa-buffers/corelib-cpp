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
#include <span>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

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
          .sequenceBegin(1)
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
      .sequenceBegin(1)
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

    /* Skip an entire unread sub-sequence, then resync on the field after it. */
    {
        sofab::OStreamInline<256> os;
        os.sequenceBegin(1).write(0, uint64_t{7}).write(1, uint64_t{8}).sequenceEnd()
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

    /* Buffered continuation path: the array completes on a later feed; the
     * invalidate must surface through that feed's Result too. */
    {
        const uint8_t bytes[] = {0x03, 0x05, 1, 2, 3, 4, 5};
        sofab::IStreamObject<BoundedArr> in;
        auto r = in.feed(bytes, 3); /* header + count + 1 element: incomplete */
        CHECK(r.code() == sofab::Error::Incomplete, "invalidate: split array first chunk is INCOMPLETE");
        r = in.feed(bytes + 3, sizeof bytes - 3);
        CHECK(r.code() == sofab::Error::InvalidMessage, "invalidate: completing chunk reports INVALID");
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
            if (is.wire() != sofab::Wire::Unsigned) return; /* §7.3: skip on mismatch */
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

    /* Control — the exact mis-decode §7.3 prevents: an unguarded reader pulls the
     * raw varint and applies no zig-zag, yielding 6 where the Signed value is 3.
     * This is the gap the accessor closes. */
    {
        struct UnguardedU : sofab::IStreamMessage
        {
            uint32_t v = 0;
            void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
            { if (id == 0) is.read(v); }
        };
        const uint8_t bytes[] = {0x01, 0x06}; /* Signed, zig-zag 6 = 3 */
        sofab::IStreamObject<UnguardedU> in;
        in.feed(bytes, sizeof bytes);
        CHECK((*in).v == 6, "wire-guard: an unguarded read mis-decodes the Signed value (demonstrates the gap)");
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
                if (id == 0) { if (is.wire() != sofab::Wire::Unsigned) return; is.read(a); readA = true; }
                else if (id == 1) { if (is.wire() != sofab::Wire::Signed) return; is.read(b); }
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
                if (is.wire() != sofab::Wire::Fixlen || is.fixType() != sofab::Fix::String) return;
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
            std::vector<sofab::Wire> w;
            std::vector<sofab::Fix> f;
            void deserialize(sofab::IStreamImpl &is, sofab::id, size_t, size_t) noexcept override
            { w.push_back(is.wire()); f.push_back(is.fixType()); } /* no read(): fields auto-skip */
        };
        sofab::IStreamObject<Recorder> in;
        auto r = in.feed(os.data(), os.bytesUsed());
        CHECK(r.code() == sofab::Error::None, "accessor: readout message is COMPLETE");
        CHECK((*in).w.size() == 4, "accessor: all four fields delivered");
        CHECK((*in).w[0] == sofab::Wire::Unsigned, "accessor: field 0 wire is Unsigned");
        CHECK((*in).w[1] == sofab::Wire::Signed, "accessor: field 1 wire is Signed");
        CHECK((*in).w[2] == sofab::Wire::Fixlen && (*in).f[2] == sofab::Fix::Fp32, "accessor: field 2 is Fixlen/Fp32");
        CHECK((*in).w[3] == sofab::Wire::Fixlen && (*in).f[3] == sofab::Fix::String, "accessor: field 3 is Fixlen/String");
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
    checkEncode("empty_sequence",       "0607", [](auto &os){ os.sequenceBegin(0).sequenceEnd(); });

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
        seq.sequenceBegin(1).sequenceEnd().write(2, int64_t{-7});
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
            auto r = os.sequenceBegin(0);
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
static std::vector<uint8_t> stringFieldWire(std::string_view payload, sofab::Fix sub)
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
        auto w = stringFieldWire("\xE2\x82\xAC", sofab::Fix::String); /* 5 bytes total */
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
        auto w = stringFieldWire("\xC0\x80", sofab::Fix::String);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "utf8/strict: decode rejects C0 80 string as INVALID");
        CHECK(r.invalid() && r.status() == sofab::DecodeStatus::Invalid, "utf8/strict: decode reject maps to Invalid status");
    }
    {
        /* truncated-at-end-of-payload (declared length reached mid-sequence) is INVALID. */
        auto w = stringFieldWire("\xE2\x82", sofab::Fix::String);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::InvalidMessage, "utf8/strict: truncated-at-end string is INVALID");
    }

    /* --- a skipped string is never validated (spec §6.4 skip exemption). --- */
    {
        struct SkipAll : sofab::IStreamMessage {
            void deserialize(sofab::IStreamImpl &, sofab::id, size_t, size_t) noexcept override {} /* read nothing */
        };
        auto w = stringFieldWire("\xC0\x80", sofab::Fix::String);
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
        auto w = stringFieldWire("\xC0\x80", sofab::Fix::Blob);
        sofab::IStreamObject<ScalarMsg> in;
        auto r = in.feed(w.data(), w.size());
        CHECK(r.code() == sofab::Error::None, "utf8/strict: Blob-subtype payload is not UTF-8 validated");
        CHECK((*in).s == std::string("\xC0\x80", 2), "utf8/strict: Blob payload stored verbatim");
    }
#endif
}

int main()
{
    encodeVectors();
    roundtripScalars();
    roundtripArrays();
    roundtripNested();
    chunkedDecode();
    skippingUnknownFields();
    malformedInput();
    threeValuedOutcomes();
    callbackInvalidate();
    callbackExceedLimit();
    wireTypeGuard();
    zeroLengthForms();
    maxDepth();
    bufferLimits();
    strictUtf8();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
