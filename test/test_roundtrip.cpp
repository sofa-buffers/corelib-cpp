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

int main()
{
    encodeVectors();
    roundtripScalars();
    roundtripArrays();
    roundtripNested();
    chunkedDecode();
    skippingUnknownFields();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
