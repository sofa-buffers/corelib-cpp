/*!
 * @file bits_equal.hpp
 * @brief Bit-pattern equality for float arrays (`sofab::bitsEqual`).
 *
 * A generated encoder omits a field iff its value equals its declared default
 * (MESSAGE_SPEC §2), and CORELIB_PLAN §4.6 makes floats round-trip bit for bit.
 * An IEEE `==` over the elements contradicts both: `-0.0 == +0.0`, so an array
 * `[-0.0, 1.5]` would be taken for the default `[0.0, 1.5]` and its first element
 * dropped, and `NaN != NaN` would write a default array that holds a NaN.
 *
 * `bitsEqual` is the comparison the omission test needs: equal length and, at
 * every index, an identical IEEE-754 bit pattern (32 bits for `float`, 64 for
 * `double`). `+0.0` and `-0.0` differ; two NaNs are equal exactly when their bit
 * patterns, payload included, are identical. No IEEE `==` is involved.
 *
 * Schema-independent by construction: the element type is a template argument and
 * the bound is the argument's own length, so generated code calls it instead of
 * emitting a loop per field.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SOFAB_BITS_EQUAL_HPP
#define SOFAB_BITS_EQUAL_HPP

#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <ranges>
#include <span>
#include <type_traits>

namespace sofab
{
    namespace detail
    {
        /*! @brief `float` or `double`, the two element types of an fp array. */
        template <typename T>
        concept FloatElement = std::same_as<T, float> || std::same_as<T, double>;

        /*! @brief Length check, then one block compare of the object representation. */
        template <FloatElement T>
        inline bool bitsEqualSpan(std::span<const T> a, std::span<const T> b) noexcept
        {
            if (a.size() != b.size()) { return false; }
            /* memcmp with a null pointer is undefined even for a zero count, and an
             * empty std::vector or a default-constructed span has no storage. */
            if (a.empty()) { return true; }
            return std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
        }
    } // namespace detail

    /*!
     * @brief True iff @p a and @p b have the same length and every element pair has
     *        the same IEEE-754 bit pattern.
     *
     * Accepts anything contiguous and sized whose element type is `float` or
     * `double`, on both sides: `std::vector`, `sofab::InlineVector`, `std::array`,
     * `std::span`, a C array, or a braced list (`bitsEqual(v, {0.0f, 1.5f})`
     * binds a `std::initializer_list<float>`). Both sides must have the same
     * element type. The length is compared first; nothing is allocated or
     * modified.
     *
     * @return `false` on a length mismatch or any differing bit pattern.
     */
    template <std::ranges::contiguous_range A, std::ranges::contiguous_range B>
        requires std::ranges::sized_range<A> && std::ranges::sized_range<B>
              && detail::FloatElement<std::remove_cv_t<std::ranges::range_value_t<A>>>
              && std::same_as<std::remove_cv_t<std::ranges::range_value_t<A>>,
                              std::remove_cv_t<std::ranges::range_value_t<B>>>
    inline bool bitsEqual(const A &a, const B &b) noexcept
    {
        using T = std::remove_cv_t<std::ranges::range_value_t<A>>;
        return detail::bitsEqualSpan<T>(
            std::span<const T>(std::ranges::data(a), std::ranges::size(a)),
            std::span<const T>(std::ranges::data(b), std::ranges::size(b)));
    }

    /*! @brief Braced-list default: `bitsEqual(field, {0.0f, 1.5f})`. */
    template <detail::FloatElement T, std::ranges::contiguous_range A>
        requires std::ranges::sized_range<A>
              && std::same_as<std::remove_cv_t<std::ranges::range_value_t<A>>, T>
    inline bool bitsEqual(const A &a, std::initializer_list<T> b) noexcept
    {
        return detail::bitsEqualSpan<T>(
            std::span<const T>(std::ranges::data(a), std::ranges::size(a)),
            std::span<const T>(b.begin(), b.size()));
    }
} // namespace sofab

#endif // SOFAB_BITS_EQUAL_HPP
