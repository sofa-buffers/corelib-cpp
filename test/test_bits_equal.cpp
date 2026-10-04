/*!
 * @file test_bits_equal.cpp
 * @brief Checks for sofab::bitsEqual, the bit-pattern equality of float arrays.
 *
 * The generated encoder omits an array field iff it equals its default, so the
 * comparison must see what the wire sees: `-0.0` is not `+0.0`, and a NaN equals
 * another NaN only when every bit, payload included, is the same.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what) do { \
    ++g_checks; \
    if (!(cond)) { ++g_failures; std::printf("FAIL: %s (line %d)\n", what, __LINE__); } \
} while (0)

/* The reference: a plain loop over the integer bit patterns. */
template <typename T, typename U>
static bool refEqual(const std::vector<T> &a, const std::vector<T> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<U>(a[i]) != std::bit_cast<U>(b[i])) return false;
    return true;
}

template <typename T> struct Bits;
template <> struct Bits<float>  { using U = std::uint32_t; };
template <> struct Bits<double> { using U = std::uint64_t; };

template <typename T>
static T fromBits(typename Bits<T>::U u) { return std::bit_cast<T>(u); }

template <typename T>
static void checkWidth(const char *tn)
{
    using U = typename Bits<T>::U;
    char msg[160];
    auto say = [&](const char *s) { std::snprintf(msg, sizeof msg, "%s: %s", tn, s); return msg; };

    const T pz = T(0), nz = -T(0);
    const T qnan = std::numeric_limits<T>::quiet_NaN();
    /* same quiet NaN, different payload: set the lowest mantissa bit */
    const T qnan2 = fromBits<T>(static_cast<U>(std::bit_cast<U>(qnan) | U(1)));
    /* signalling NaN: quiet bit clear, payload non-zero */
    const T snan = std::numeric_limits<T>::signaling_NaN();
    const T inf = std::numeric_limits<T>::infinity();
    const T den = std::numeric_limits<T>::denorm_min();

    /* empty and one element */
    {
        std::vector<T> e1, e2;
        CHECK(sofab::bitsEqual(e1, e2), say("empty equals empty"));
        CHECK(!sofab::bitsEqual(e1, std::vector<T>{pz}), say("empty vs one"));
        CHECK(!sofab::bitsEqual(std::vector<T>{pz}, e1), say("one vs empty"));
        CHECK(sofab::bitsEqual(std::vector<T>{T(1.5)}, std::vector<T>{T(1.5)}), say("one equal"));
        CHECK(!sofab::bitsEqual(std::vector<T>{T(1.5)}, std::vector<T>{T(2.5)}), say("one differs"));
    }
    /* signed zero at the first, a middle and the last index */
    for (std::size_t at : {std::size_t{0}, std::size_t{2}, std::size_t{4}}) {
        std::vector<T> a(5, T(1.5)), b(5, T(1.5));
        a[at] = nz; b[at] = pz;
        CHECK(!sofab::bitsEqual(a, b), say("-0.0 vs +0.0 differs"));
        CHECK(!sofab::bitsEqual(b, a), say("+0.0 vs -0.0 differs"));
        b[at] = nz;
        CHECK(sofab::bitsEqual(a, b), say("-0.0 vs -0.0 equal"));
        /* the IEEE compare this replaces says equal: the bug under test */
        a[at] = nz; b[at] = pz;
        CHECK(a == b, say("sanity: IEEE == would call them equal"));
    }
    /* NaN, infinities, subnormals */
    CHECK(sofab::bitsEqual(std::vector<T>{qnan}, std::vector<T>{qnan}), say("same NaN bits equal"));
    CHECK(!sofab::bitsEqual(std::vector<T>{qnan}, std::vector<T>{qnan2}), say("NaN payload differs"));
    CHECK(sofab::bitsEqual(std::vector<T>{snan}, std::vector<T>{snan}), say("same sNaN bits equal"));
    CHECK(!sofab::bitsEqual(std::vector<T>{qnan}, std::vector<T>{snan}), say("qNaN vs sNaN differ"));
    CHECK(sofab::bitsEqual(std::vector<T>{inf, -inf}, std::vector<T>{inf, -inf}), say("infinities equal"));
    CHECK(!sofab::bitsEqual(std::vector<T>{inf}, std::vector<T>{-inf}), say("+inf vs -inf"));
    CHECK(sofab::bitsEqual(std::vector<T>{den, -den}, std::vector<T>{den, -den}), say("subnormals equal"));
    CHECK(!sofab::bitsEqual(std::vector<T>{den}, std::vector<T>{-den}), say("subnormal sign"));
    CHECK(!sofab::bitsEqual(std::vector<T>{den}, std::vector<T>{pz}), say("subnormal vs zero"));

    /* length mismatch both ways, equal prefix */
    {
        std::vector<T> a{T(1), T(2), T(3)}, b{T(1), T(2)};
        CHECK(!sofab::bitsEqual(a, b), say("longer vs shorter"));
        CHECK(!sofab::bitsEqual(b, a), say("shorter vs longer"));
    }
    /* longer than 64 elements, exactly one differing element */
    for (std::size_t n : {std::size_t{65}, std::size_t{256}, std::size_t{1000}}) {
        std::vector<T> a(n);
        for (std::size_t i = 0; i < n; ++i) a[i] = T(i) * T(0.25) - T(7);
        CHECK(sofab::bitsEqual(a, a), say("array against itself"));
        for (std::size_t at : {std::size_t{0}, n / 2, n - 1}) {
            std::vector<T> b = a;
            CHECK(sofab::bitsEqual(a, b), say("long copy equal"));
            b[at] = fromBits<T>(static_cast<U>(std::bit_cast<U>(b[at]) ^ U(1)));
            CHECK(!sofab::bitsEqual(a, b), say("one ULP differs in a long array"));
            b = a; b[at] = (std::bit_cast<U>(a[at]) == std::bit_cast<U>(pz)) ? nz : -a[at];
            CHECK(!sofab::bitsEqual(a, b), say("one sign differs in a long array"));
        }
    }
    /* the container shapes the generated code passes */
    {
        sofab::InlineVector<T, 3> f{pz, T(1.5)};
        CHECK(sofab::bitsEqual(f, sofab::InlineVector<T, 3>{pz, T(1.5)}), say("InlineVector, default"));
        CHECK(sofab::bitsEqual(f, std::vector<T>{pz, T(1.5)}), say("InlineVector vs vector"));
        CHECK(sofab::bitsEqual(f, {pz, T(1.5)}), say("braced default"));
        CHECK(!sofab::bitsEqual(f, {nz, T(1.5)}), say("braced default, -0.0"));
        CHECK(!sofab::bitsEqual(f, {pz}), say("braced default, shorter"));
        f.assign({nz, T(1.5)});
        CHECK(!sofab::bitsEqual(f, sofab::InlineVector<T, 3>{pz, T(1.5)}), say("InlineVector -0.0"));
        std::array<T, 2> arr{pz, T(1.5)};
        const T carr[2] = {pz, T(1.5)};
        CHECK(sofab::bitsEqual(arr, carr), say("std::array vs C array"));
        CHECK(sofab::bitsEqual(std::span<const T>(arr), std::span<T>(arr)), say("span const vs mutable"));
        CHECK(sofab::bitsEqual(std::span<const T>{}, std::vector<T>{}), say("empty span vs empty vector"));
    }
    /* deterministic cross-check against the reference loop */
    {
        std::uint64_t s = 0x9e3779b97f4a7c15ull;
        auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
        bool agree = true;
        for (int round = 0; round < 2000; ++round) {
            std::size_t n = next() % 20;
            std::vector<T> a(n), b;
            for (auto &x : a) {
                /* mix special patterns and arbitrary bits */
                switch (next() % 6) {
                case 0: x = pz; break;
                case 1: x = nz; break;
                case 2: x = qnan; break;
                case 3: x = qnan2; break;
                default: x = fromBits<T>(static_cast<U>(next())); break;
                }
            }
            b = a;
            switch (next() % 4) {
            case 0: break;
            case 1: if (n) b[next() % n] = fromBits<T>(static_cast<U>(next())); break;
            case 2: if (n) { T &x = b[next() % n]; x = std::bit_cast<U>(x) == std::bit_cast<U>(pz) ? nz : pz; } break;
            default: b.push_back(pz); break;
            }
            if (sofab::bitsEqual(a, b) != refEqual<T, U>(a, b)) agree = false;
        }
        CHECK(agree, say("agrees with the reference bit loop"));
    }
}

int main()
{
    checkWidth<float>("fp32");
    checkWidth<double>("fp64");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
