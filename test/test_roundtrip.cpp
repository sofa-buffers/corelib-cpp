/*!
 * @file test_roundtrip.cpp
 * @brief Standalone checks for the pure-C++20 SofaBuffers implementation.
 *
 * Validates wire compatibility against known byte sequences from the shared
 * conformance vectors (assets/test_vectors.json) and exercises encode/decode
 * round-trips, including nested sequences and arrays.
 *
 * Build & run:
 *   g++ -std=c++20 -I cpp20/include cpp20/test/test_roundtrip.cpp -o /tmp/t && /tmp/t
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
 *     out of bounds; corruption it can detect is surfaced as InvalidMessage, and
 *     anything it cannot yet judge is held as "needs more bytes". --- */

static void malformedInput()
{
    /* Truncated value varint (continuation bit set, then the buffer ends). A
     * streaming decoder treats this as an incomplete tail — no field delivered,
     * no error, no crash. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x08, 0x80}; /* id 1, unsigned, dangling varint */
        auto r = in.feed(bytes, sizeof bytes);
        CHECK(r.code() == sofab::Error::None, "malformed: truncated varint buffered, not errored");
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

    /* Oversized fixlen length: the header claims far more payload than is
     * present. Must be held as incomplete, never read past the buffer. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x2a, static_cast<uint8_t>((200u << 3) | 2u), 'h', 'i'};
        auto r = in.feed(bytes, sizeof bytes); /* id 5, string, len=200 */
        CHECK(r.code() == sofab::Error::None, "malformed: oversized fixlen buffered, no OOB read");
        CHECK((*in).s.empty(), "malformed: oversized fixlen yields no string");
    }

    /* A stray sequence-end marker at top level must be skipped, not crash. */
    {
        sofab::IStreamObject<ScalarMsg> in;
        const uint8_t bytes[] = {0x07};
        in.feed(bytes, sizeof bytes);
        CHECK(true, "malformed: stray sequence-end survived");
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

int main()
{
    encodeVectors();
    roundtripScalars();
    roundtripArrays();
    roundtripNested();
    chunkedDecode();
    skippingUnknownFields();
    malformedInput();
    zeroLengthForms();
    maxDepth();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
