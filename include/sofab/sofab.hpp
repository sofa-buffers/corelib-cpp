/*!
 * @file sofab.hpp
 * @brief SofaBuffers — pure C++20 implementation (no C backend).
 *
 * A from-scratch, header-only implementation of the SofaBuffers wire format in
 * modern C++20. It mirrors the public API of the C-backed `sofab/sofab.hpp`
 * (same `sofab::OStream` / `sofab::IStream*` surface) but shares no code with
 * the C library — encoding and decoding are implemented directly here.
 *
 * Design:
 *  - Encoding stays fully streamable: an `OStream` writes into a caller buffer
 *    and invokes a flush callback when it fills, so a message can exceed RAM.
 *  - Decoding is optimised for the common case where the whole message is
 *    already in contiguous memory: a protobuf-style cursor advances a pointer
 *    over the buffer (no per-byte state machine). The streaming `feed()` API is
 *    retained — bytes are accumulated and complete top-level fields are
 *    dispatched as they become available — so chunked input still works.
 *  - Modern techniques: `std::span`, `std::bit_cast`, concepts, `if constexpr`,
 *    `[[nodiscard]]`. Endianness is handled explicitly (LE on the wire) so no
 *    host-endian branching is needed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SOFAB_HPP
#define SOFAB_HPP

/**
 * @defgroup cpp20_api C++20 API
 * @{
 */

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sofab
{
    /// Version of the SofaBuffers public API implemented by this header.
    inline constexpr int API_VERSION = 1;

    /// Largest valid field id (`INT32_MAX`); see §6.2 of the spec.
    inline constexpr uint32_t ID_MAX = 0x7fffffffu;
    /// Largest fixlen payload byte-length (`INT32_MAX`); see §6.2 of the spec.
    inline constexpr uint32_t FIXLEN_MAX = 0x7fffffffu;
    /// Largest array element count (`INT32_MAX`); see §6.2 of the spec.
    inline constexpr uint32_t ARRAY_MAX = 0x7fffffffu;
    /// Maximum nested-sequence depth (§4.9, §6.2). Deeper nesting is rejected
    /// (encode: @ref Error::InvalidArgument; decode: @ref Error::InvalidMessage).
    inline constexpr int MAX_DEPTH = 255;

    /**
     * @brief Always-false trait used to trigger `static_assert` in the
     *        otherwise-unreachable branch of an `if constexpr` chain.
     *
     * Because the value depends on the template parameter, the assertion only
     * fires when that branch is actually instantiated, which is the idiomatic
     * way to reject unsupported types at compile time.
     *
     * @tparam T The type the dependent assertion is bound to.
     */
    template <typename>
    inline constexpr bool always_false_v = false;

    /// Status codes returned by the encode/decode API; `None` signals success.
    enum class Error
    {
        None = 0,            ///< Operation succeeded (decode: `COMPLETE`, §7).
        UsageError = 2,      ///< The API was used incorrectly (e.g. out-of-order calls).
        BufferFull = 3,      ///< The output buffer filled and no flush callback was set.
        InvalidArgument = 1, ///< An argument was out of range (e.g. a field id above the limit).
        InvalidMessage = 4,  ///< The input bytes are malformed (decode: `INVALID`, §7).
        Incomplete = 5,      ///< Decode-only: the fed bytes end **inside** a field — a partial
                             ///< varint (§4.1), a fixlen/array payload shorter than declared (§4.6/§4.8),
                             ///< or an open (unclosed) sequence (§4.9). This is `INCOMPLETE` (§7): **not**
                             ///< an error — the caller owns end-of-input and may feed more bytes. It is
                             ///< distinct from both @ref None (`COMPLETE`) and @ref InvalidMessage (`INVALID`).
    };

    /**
     * @brief Three-valued decode outcome of a @ref sofab::IStreamImpl::feed call (spec §7).
     *
     * The decoder reports where the consumed bytes ended, with **no** separate
     * `finish`/`finalize` step (§7.1): the same three outcomes apply to a one-shot
     * buffer and to chunked streaming. Whether an @ref Incomplete result is
     * acceptable is the caller's decision — a streaming caller reads it as "feed
     * me the next chunk", a whole-message caller reads it as a truncated message.
     */
    enum class DecodeStatus
    {
        Complete = 0,   ///< Consumed bytes end **exactly** at a field boundary — a valid message.
        Incomplete = 1, ///< Consumed bytes end **inside** a field or with an open sequence. Not an error.
        Invalid = 2,    ///< Bytes are malformed **regardless of what follows**. Terminal.
    };

    /// Field identifier on the wire. Valid range is `[0, INT32_MAX]`.
    using id = uint32_t;

    /* ---------------------------------------------------------------------- */
    /* wire-format primitives                                                 */
    /* ---------------------------------------------------------------------- */

    /// Implementation details of the wire format; not part of the public API.
    namespace detail
    {
        /// Wire type stored in the low 3 bits of every field header.
        enum class Wire : uint8_t
        {
            Unsigned = 0,      ///< Unsigned integer encoded as a varint.
            Signed = 1,        ///< Signed integer, zig-zag encoded as a varint.
            Fixlen = 2,        ///< Length-prefixed payload (float, string or blob).
            ArrayUnsigned = 3, ///< Count-prefixed array of unsigned varints.
            ArraySigned = 4,   ///< Count-prefixed array of zig-zag varints.
            ArrayFixlen = 5,   ///< Count-prefixed array of fixed-size elements.
            SequenceStart = 6, ///< Opens a nested sub-message.
            SequenceEnd = 7,   ///< Closes the most recently opened sub-message.
        };

        /// Sub-type of a length-prefixed (`Fixlen`) payload, stored in the low 3 bits of its length word.
        enum class Fix : uint8_t
        {
            Fp32 = 0,   ///< 32-bit IEEE-754 float.
            Fp64 = 1,   ///< 64-bit IEEE-754 double.
            String = 2, ///< UTF-8 text.
            Blob = 3,   ///< Opaque byte string.
        };

        /// Largest permitted field id (`INT32_MAX`); larger ids are rejected with @ref Error::InvalidArgument.
        inline constexpr uint32_t kIdMax = 0x7fffffffu; /* INT32_MAX */

        /**
         * @brief Map a signed integer to an unsigned one with the zig-zag scheme.
         *
         * Small-magnitude values of either sign map to small unsigned values, so
         * they encode to short varints.
         *
         * @param v Signed value to transform.
         * @return The zig-zag-encoded unsigned representation.
         */
        constexpr uint64_t zigzagEncode(int64_t v) noexcept
        {
            return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
        }
        /**
         * @brief Inverse of @ref zigzagEncode.
         * @param u Zig-zag-encoded unsigned value.
         * @return The original signed value.
         */
        constexpr int64_t zigzagDecode(uint64_t u) noexcept
        {
            return static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
        }

        /**
         * @brief Reinterpret a float/double as the unsigned integer holding its bits.
         * @tparam F Floating-point type (`float` or `double`).
         * @param v Value whose object representation is extracted.
         * @return `uint32_t` for a 4-byte `F`, otherwise `uint64_t`.
         */
        template <std::floating_point F>
        constexpr auto floatBits(F v) noexcept
        {
            if constexpr (sizeof(F) == 4) return std::bit_cast<uint32_t>(v);
            else                          return std::bit_cast<uint64_t>(v);
        }
        /**
         * @brief Reconstruct a floating-point value from its raw bits.
         * @tparam F Target floating-point type.
         * @tparam U Unsigned integer type holding the bit pattern.
         * @param bits Object representation produced by @ref floatBits.
         * @return The floating-point value those bits encode.
         */
        template <std::floating_point F, std::unsigned_integral U>
        constexpr F bitsFloat(U bits) noexcept
        {
            return std::bit_cast<F>(bits);
        }
    } // namespace detail

    /* ---------------------------------------------------------------------- */
    /* OStream                                                                */
    /* ---------------------------------------------------------------------- */

    class OStreamMessage;

    /**
     * @brief Base of the output streams: encodes values into a caller-provided buffer.
     *
     * Holds the write cursor and the encoding logic but owns no storage itself;
     * the buffer is supplied by a derived class (@ref OStream, @ref OStreamInline).
     * When the buffer fills, the optional flush callback is invoked with the bytes
     * accumulated so far and the cursor rewinds, so a message may exceed the buffer
     * (and even RAM). Without a callback, overflow returns @ref Error::BufferFull.
     *
     * Write calls return a chainable @ref Result that latches the first error.
     */
    class OStreamImpl
    {
    public:
        /// Callback invoked with a span of finished bytes whenever the buffer flushes.
        using flushCallback = std::function<void(std::span<const uint8_t>)>;

    protected:
        uint8_t *buffer_ = nullptr;   ///< Start of the active buffer.
        uint8_t *cursor_ = nullptr;   ///< Current write position.
        uint8_t *end_ = nullptr;      ///< One past the end of the buffer.
        flushCallback flushCallback_; ///< Invoked when the buffer fills; may be empty.
        size_t seqDepth_ = 0;         ///< Number of currently-open nested sequences (§4.9 @ref MAX_DEPTH).

        /// Construct an unattached stream; a derived class must call @ref initBuffer.
        OStreamImpl() noexcept = default;

        /**
         * @brief Point the stream at a buffer and position the write cursor.
         * @param buffer Storage to encode into.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to leave untouched before the cursor.
         */
        void initBuffer(uint8_t *buffer, size_t buflen, size_t offset) noexcept
        {
            buffer_ = buffer;
            cursor_ = buffer + offset;
            end_ = buffer + buflen;
        }

        /**
         * @brief Encode an unsigned value as a base-128 varint.
         * @param out Destination buffer; must hold at least 10 bytes.
         * @param v Value to encode.
         * @return Number of bytes written to @p out (1–10).
         */
        static size_t encodeVarint(uint8_t *out, uint64_t v) noexcept
        {
            size_t n = 0;
            do {
                uint8_t b = static_cast<uint8_t>(v & 0x7f);
                v >>= 7;
                if (v) b |= 0x80;
                out[n++] = b;
            } while (v);
            return n;
        }

        /**
         * @brief Append a single byte, flushing first if the buffer is full.
         * @param b Byte to write.
         * @return @ref Error::None on success, or @ref Error::BufferFull if the
         *         buffer is full and no flush callback is set.
         */
        [[nodiscard]] Error pushByte(uint8_t b) noexcept
        {
            if (cursor_ == end_)
            {
                if (!flushCallback_) return Error::BufferFull;
                flushCallback_(std::span<const uint8_t>(buffer_, static_cast<size_t>(cursor_ - buffer_)));
                cursor_ = buffer_;
            }
            *cursor_++ = b;
            return Error::None;
        }

        /**
         * @brief Append a run of bytes.
         *
         * Uses a single `memcpy` when the payload fits the remaining buffer (the
         * common case); otherwise falls back to a byte-by-byte copy that flushes
         * across buffer boundaries.
         *
         * @param data Source bytes.
         * @param len Number of bytes to append.
         * @return @ref Error::None on success, or @ref Error::BufferFull if the
         *         buffer fills with no flush callback set.
         */
        [[nodiscard]] Error pushBytes(const uint8_t *data, size_t len) noexcept
        {
            if (static_cast<size_t>(end_ - cursor_) >= len) [[likely]]
            {
                std::memcpy(cursor_, data, len);
                cursor_ += len;
                return Error::None;
            }
            for (size_t i = 0; i < len; ++i)
                if (Error e = pushByte(data[i]); e != Error::None) return e;
            return Error::None;
        }

        /**
         * @brief Encode a value as a varint and append it to the buffer.
         * @param v Value to write.
         * @return @ref Error::None on success, or @ref Error::BufferFull on overflow.
         */
        [[nodiscard]] Error putVarint(uint64_t v) noexcept
        {
            uint8_t tmp[10];
            return pushBytes(tmp, encodeVarint(tmp, v));
        }

        /**
         * @brief Write a field header (field id and wire type) as one varint.
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param type Wire type of the field.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error putHeader(sofab::id fieldId, detail::Wire type) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            return putVarint((static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(type));
        }

        /**
         * @brief Write a scalar field: header varint plus one value varint, in a single bulk write.
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param type Wire type (@ref detail::Wire::Unsigned or @ref detail::Wire::Signed).
         * @param value Already-encoded scalar value (zig-zagged for signed fields).
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error writeScalar(sofab::id fieldId, detail::Wire type, uint64_t value) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(type));
            n += encodeVarint(tmp + n, value);
            return pushBytes(tmp, n);
        }

        /**
         * @brief Write a length-prefixed field (string, blob or other fixlen payload).
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param ft Payload sub-type (@ref detail::Fix::String, @ref detail::Fix::Blob, ...).
         * @param data Payload bytes.
         * @param len Payload length in bytes.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error writeFixlen(sofab::id fieldId, detail::Fix ft, const uint8_t *data, size_t len) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(detail::Wire::Fixlen));
            n += encodeVarint(tmp + n, (static_cast<uint64_t>(len) << 3) | static_cast<uint64_t>(ft));
            if (Error e = pushBytes(tmp, n); e != Error::None) return e;
            return pushBytes(data, len);
        }

        /**
         * @brief Write a single float or double as a little-endian fixlen field.
         * @tparam F Floating-point type (`float` or `double`).
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param value Value to encode.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::floating_point F>
        [[nodiscard]] Error writeFloatScalar(sofab::id fieldId, F value) noexcept
        {
            constexpr detail::Fix ft = (sizeof(F) == 4) ? detail::Fix::Fp32 : detail::Fix::Fp64;
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            auto bits = detail::floatBits(value);
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(detail::Wire::Fixlen));
            n += encodeVarint(tmp + n, (static_cast<uint64_t>(sizeof(F)) << 3) | static_cast<uint64_t>(ft));
            for (size_t i = 0; i < sizeof(F); ++i) tmp[n++] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
            return pushBytes(tmp, n);
        }

        /**
         * @brief Write an array of integers as a count-prefixed run of varints.
         *
         * The wire type is chosen from the element's signedness; signed elements
         * are zig-zag encoded.
         *
         * @tparam E Integral element type.
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param elems Elements to encode, in order.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::integral E>
        [[nodiscard]] Error writeIntArray(sofab::id fieldId, std::span<const E> elems) noexcept
        {
            constexpr bool isSigned = std::is_signed_v<E>;
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t hdr[20];
            size_t hn = encodeVarint(hdr, (static_cast<uint64_t>(fieldId) << 3) |
                        static_cast<uint64_t>(isSigned ? detail::Wire::ArraySigned : detail::Wire::ArrayUnsigned));
            hn += encodeVarint(hdr + hn, elems.size());
            if (Error e = pushBytes(hdr, hn); e != Error::None) return e;
            for (E v : elems)
            {
                uint8_t tmp[10];
                size_t n = isSigned ? encodeVarint(tmp, detail::zigzagEncode(static_cast<int64_t>(v)))
                                    : encodeVarint(tmp, static_cast<uint64_t>(v));
                if (Error e = pushBytes(tmp, n); e != Error::None) return e;
            }
            return Error::None;
        }

        /**
         * @brief Write an array of floats or doubles as a count-prefixed fixlen array.
         *
         * On little-endian hosts the payload is copied in one block since the wire
         * layout matches native memory; on big-endian hosts each element is byte-swapped.
         *
         * @tparam F Floating-point element type (`float` or `double`).
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param elems Elements to encode, in order.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::floating_point F>
        [[nodiscard]] Error writeFloatArray(sofab::id fieldId, std::span<const F> elems) noexcept
        {
            constexpr detail::Fix ft = (sizeof(F) == 4) ? detail::Fix::Fp32 : detail::Fix::Fp64;
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t hdr[20];
            size_t hn = encodeVarint(hdr, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(detail::Wire::ArrayFixlen));
            hn += encodeVarint(hdr + hn, elems.size());
            /* §4.8: a fixlen array always carries its fixlen_word, even when empty
             * (count == 0), so an empty fp32 and fp64 array stay distinguishable. */
            hn += encodeVarint(hdr + hn, (static_cast<uint64_t>(sizeof(F)) << 3) | static_cast<uint64_t>(ft));
            if (Error e = pushBytes(hdr, hn); e != Error::None) return e;
            if (elems.empty()) return Error::None; /* fixlen_word emitted; no payload */

            if constexpr (std::endian::native == std::endian::little)
            {
                /* wire bytes == native bytes: copy the whole payload at once */
                return pushBytes(reinterpret_cast<const uint8_t *>(elems.data()), elems.size() * sizeof(F));
            }
            else
            {
                for (F v : elems)
                {
                    auto bits = detail::floatBits(v);
                    uint8_t tmp[sizeof(F)];
                    for (size_t i = 0; i < sizeof(F); ++i) tmp[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
                    if (Error e = pushBytes(tmp, sizeof(F)); e != Error::None) return e;
                }
                return Error::None;
            }
        }

    public:
        /**
         * @brief Chainable result of a write operation.
         *
         * Each chained call is a no-op once an error has been latched, so a
         * sequence of writes can be expressed fluently and the first failure
         * inspected at the end via @ref ok / @ref code.
         */
        class Result
        {
            OStreamImpl &os_;
            Error error_;
            friend class OStreamImpl;
            Result(OStreamImpl &os, Error e) noexcept : os_(os), error_(e) {}

        public:
            /**
             * @brief Chain another field write onto the same stream.
             * @tparam T Value type accepted by @ref OStreamImpl::write.
             * @param fieldId Field identifier.
             * @param value Value to encode.
             * @return `*this`, for further chaining.
             */
            template <typename T>
            Result write(sofab::id fieldId, const T &value) noexcept
            {
                if (error_ == Error::None) error_ = os_.write(fieldId, value).error_;
                return *this;
            }
            /**
             * @brief Chain a field write that only happens when @p condition holds.
             * @tparam T Value type accepted by @ref OStreamImpl::write.
             * @param fieldId Field identifier.
             * @param value Value to encode when @p condition is true.
             * @param condition Write the field only if this is true.
             * @return `*this`, for further chaining.
             */
            template <typename T>
            Result writeIf(sofab::id fieldId, const T &value, bool condition) noexcept
            {
                if (error_ == Error::None && condition) error_ = os_.write(fieldId, value).error_;
                return *this;
            }
            /**
             * @brief Chain the opening of a nested sub-message.
             * @param fieldId Field identifier of the sub-message.
             * @return `*this`, for further chaining.
             */
            Result sequenceBegin(sofab::id fieldId) noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceBegin(fieldId).error_;
                return *this;
            }
            /**
             * @brief Chain the closing of the current nested sub-message.
             * @return `*this`, for further chaining.
             */
            Result sequenceEnd() noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceEnd().error_;
                return *this;
            }
            /// @return `true` if no error has been latched.
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            /// @return `true` if no error has been latched.
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            /// @return The latched status code (@ref Error::None if all writes succeeded).
            [[nodiscard]] Error code() const noexcept { return error_; }
            /// @return `true` if the latched code equals @p e.
            bool operator==(Error e) const noexcept { return error_ == e; }
            /// @return `true` if the latched code differs from @p e.
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

        OStreamImpl(const OStreamImpl &) = delete;
        OStreamImpl &operator=(const OStreamImpl &) = delete;
        OStreamImpl(OStreamImpl &&) noexcept = default;
        OStreamImpl &operator=(OStreamImpl &&) noexcept = default;
        /// Flushes any buffered bytes through the callback before destruction.
        virtual ~OStreamImpl() noexcept { flush(); }

        /**
         * @brief Hand any buffered bytes to the flush callback and rewind the cursor.
         * @return Number of bytes that were buffered before the flush.
         */
        size_t flush() noexcept
        {
            size_t used = static_cast<size_t>(cursor_ - buffer_);
            if (flushCallback_ && used)
                flushCallback_(std::span<const uint8_t>(buffer_, used));
            cursor_ = buffer_;
            return used;
        }

        /// @return Number of bytes written into the buffer since the last flush.
        [[nodiscard]] size_t bytesUsed() const noexcept { return static_cast<size_t>(cursor_ - buffer_); }
        /// @return Pointer to the start of the buffer; the first @ref bytesUsed bytes are valid.
        [[nodiscard]] const uint8_t *data() const noexcept { return buffer_; }

        /**
         * @brief Write a field, dispatching on the value's type.
         *
         * Handles integers (signed values are zig-zag encoded), `bool`, `float`,
         * `double`, anything convertible to `std::string_view`, contiguous ranges
         * of integers or floats (encoded as arrays), and nested @ref sofab::OStreamMessage
         * objects (encoded as a sub-message). Unsupported types fail to compile.
         *
         * @tparam T Deduced value type.
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param value Value to encode.
         * @return A @ref Result carrying @ref Error::None on success, or the first
         *         error encountered.
         */
        template <typename T>
        Result write(sofab::id fieldId, const T &value) noexcept
        {
            Error err = Error::None;

            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            {
                if constexpr (std::is_unsigned_v<T>)
                    err = writeScalar(fieldId, detail::Wire::Unsigned, static_cast<uint64_t>(value));
                else
                    err = writeScalar(fieldId, detail::Wire::Signed, detail::zigzagEncode(static_cast<int64_t>(value)));
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                err = writeScalar(fieldId, detail::Wire::Unsigned, value ? 1u : 0u);
            }
            else if constexpr (std::is_same_v<T, float>)  { err = writeFloatScalar(fieldId, value); }
            else if constexpr (std::is_same_v<T, double>) { err = writeFloatScalar(fieldId, value); }
            else if constexpr (std::is_convertible_v<T, std::string_view>)
            {
                std::string_view sv{value};
                err = writeFixlen(fieldId, detail::Fix::String,
                                  reinterpret_cast<const uint8_t *>(sv.data()), sv.size());
            }
            else if constexpr (std::is_base_of_v<OStreamMessage, T>)
            {
                err = sequenceBegin(fieldId).error_;
                if (err == Error::None) err = value.serialize(*this).error_;
                if (err == Error::None) err = sequenceEnd().error_;
            }
            else if constexpr (requires { typename T::value_type; std::span{std::declval<const T &>()}; })
            {
                using Elem = typename T::value_type;
                std::span<const Elem> sp{value};
                if constexpr (std::is_integral_v<Elem> && !std::is_same_v<Elem, bool>)
                    err = writeIntArray(fieldId, sp);
                else if constexpr (std::is_same_v<Elem, float> || std::is_same_v<Elem, double>)
                    err = writeFloatArray(fieldId, sp);
                else
                    static_assert(always_false_v<T>, "Unsupported span element type in OStream::write()");
            }
            else
            {
                static_assert(always_false_v<T>, "Unsupported type passed to OStream::write()");
            }

            return Result{*this, err};
        }

        /**
         * @brief Write a raw byte blob field.
         * @param fieldId Field identifier; must not exceed @ref detail::kIdMax.
         * @param value Pointer to the bytes to copy.
         * @param size Number of bytes to copy.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result write(sofab::id fieldId, const void *value, int32_t size) noexcept
        {
            return Result{*this, writeFixlen(fieldId, detail::Fix::Blob,
                          static_cast<const uint8_t *>(value), static_cast<size_t>(size))};
        }

        /**
         * @brief Write a field only when @p condition holds.
         * @tparam T Value type accepted by @ref write.
         * @param fieldId Field identifier.
         * @param value Value to encode when @p condition is true.
         * @param condition Write the field only if this is true.
         * @return The result of the write, or a success @ref Result if skipped.
         */
        template <typename T>
        Result writeIf(sofab::id fieldId, const T &value, bool condition) noexcept
        {
            return condition ? write(fieldId, value) : Result{*this, Error::None};
        }

        /**
         * @brief Open a nested sub-message under @p fieldId.
         *
         * Fields written after this call belong to the sub-message until the
         * matching @ref sequenceEnd.
         *
         * @param fieldId Field identifier of the sub-message.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result sequenceBegin(sofab::id fieldId) noexcept
        {
            /* §4.9/§6.2: never open more than MAX_DEPTH nested sequences. */
            if (seqDepth_ >= static_cast<size_t>(MAX_DEPTH))
                return Result{*this, Error::InvalidArgument};
            Result r{*this, putHeader(fieldId, detail::Wire::SequenceStart)};
            if (r.ok()) ++seqDepth_;
            return r;
        }
        /**
         * @brief Close the most recently opened sub-message.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result sequenceEnd() noexcept
        {
            if (seqDepth_ > 0) --seqDepth_;
            return Result{*this, putHeader(0, detail::Wire::SequenceEnd)};
        }
    };

    /**
     * @brief Output stream backed by a heap buffer held in a `shared_ptr`.
     *
     * The buffer can be allocated by the stream, adopted from the caller, or
     * swapped at runtime via @ref setBuffer, and retrieved with @ref getBuffer
     * so it may be shared with whatever consumes the encoded bytes.
     */
    class OStream : public OStreamImpl
    {
    protected:
        std::shared_ptr<uint8_t[]> bufferOwner_; ///< Owned backing storage.
        /// Construct without a buffer; one must be set via @ref setBuffer.
        OStream() noexcept = default;

    public:
        /**
         * @brief Construct with a freshly allocated buffer.
         * @param buflen Buffer capacity in bytes.
         * @param offset Number of leading bytes to reserve before the write cursor.
         */
        explicit OStream(size_t buflen, size_t offset = 0) noexcept
        {
            bufferOwner_ = std::make_shared<uint8_t[]>(buflen);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        /**
         * @brief Construct over a caller-supplied buffer.
         * @param buffer Backing storage to adopt.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to reserve before the write cursor.
         */
        OStream(std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
            : bufferOwner_{std::move(buffer)}
        {
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        /**
         * @brief Construct over a caller-supplied buffer with a flush callback.
         * @param callback Invoked with finished bytes whenever the buffer fills.
         * @param buffer Backing storage to adopt.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to reserve before the write cursor.
         */
        OStream(flushCallback callback, std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
            : bufferOwner_{std::move(buffer)}
        {
            flushCallback_ = std::move(callback);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        /**
         * @brief Replace the backing buffer and reset the write cursor.
         * @param buffer New backing storage to adopt.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to reserve before the write cursor.
         */
        void setBuffer(std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
        {
            bufferOwner_ = std::move(buffer);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        /// @return A shared handle to the backing buffer.
        [[nodiscard]] std::shared_ptr<uint8_t[]> getBuffer() noexcept { return bufferOwner_; }
    };

    /**
     * @brief Output stream whose buffer is stored inline (no heap allocation).
     * @tparam N Buffer capacity in bytes; must be greater than zero.
     * @tparam Offset Number of leading bytes to reserve before the cursor; must be less than @p N.
     */
    template <size_t N, size_t Offset = 0>
    class OStreamInline : public OStreamImpl
    {
        static_assert(N > 0, "Buffer size N must be greater than zero");
        static_assert(Offset < N, "Offset must be less than buffer size N");
        std::array<uint8_t, N> bufferOwner_{}; ///< Inline backing storage.

    public:
        /// Construct with no flush callback; overflow returns @ref Error::BufferFull.
        OStreamInline() noexcept { initBuffer(bufferOwner_.data(), N, Offset); }
        /**
         * @brief Construct with a flush callback.
         * @param callback Invoked with finished bytes whenever the buffer fills.
         */
        explicit OStreamInline(flushCallback callback) noexcept
        {
            flushCallback_ = std::move(callback);
            initBuffer(bufferOwner_.data(), N, Offset);
        }
    };

    /**
     * @brief Constrains a serialisable message: derives from @ref sofab::OStreamMessage
     *        and exposes a `static constexpr std::size_t _maxSize`.
     * @tparam T Candidate message type.
     */
    template <class T>
    concept OutputMessage =
        std::derived_from<T, OStreamMessage> &&
        requires { { T::_maxSize } -> std::convertible_to<std::size_t>; } &&
        std::is_same_v<decltype(T::_maxSize), const std::size_t>;

    /**
     * @brief Base class for user-defined messages that can serialise themselves.
     *
     * Derive from this and implement @ref serialize to write the message's fields;
     * the object can then be passed to @ref OStreamImpl::write to be encoded as a
     * nested sub-message.
     */
    class OStreamMessage
    {
    protected:
        friend class OStreamImpl;
        /**
         * @brief Write this message's fields into @p ostream.
         * @param ostream Stream to encode into.
         * @return A @ref OStreamImpl::Result carrying the outcome.
         */
        virtual OStreamImpl::Result serialize(OStreamImpl &ostream) const noexcept = 0;
    };

    /**
     * @brief Bundles a message with an inline output buffer sized for it.
     *
     * Combines an @ref OStreamInline buffer with an instance of @p MessageType,
     * so a message can be populated through `operator->` and then encoded in one
     * @ref serialize call without managing a separate stream and buffer.
     *
     * @tparam MessageType A type satisfying @ref sofab::OutputMessage.
     * @tparam N Buffer capacity in bytes; defaults to `MessageType::_maxSize`.
     * @tparam Offset Number of leading bytes to reserve before the cursor.
     */
    template <OutputMessage MessageType, size_t N = MessageType::_maxSize, size_t Offset = 0>
    class OStreamObject : public OStreamInline<N + Offset, Offset>
    {
        MessageType message_;

    public:
        /// Construct with no flush callback.
        OStreamObject() noexcept = default;
        /**
         * @brief Construct with a flush callback.
         * @param callback Invoked with finished bytes whenever the buffer fills.
         */
        explicit OStreamObject(typename OStreamImpl::flushCallback callback) noexcept
            : OStreamInline<N + Offset, Offset>{std::move(callback)} {}

        /// @return The wrapped message, so its fields can be populated via `obj->field`.
        MessageType &operator->() noexcept { return message_; }

        /**
         * @brief Serialise the wrapped message into the inline buffer and flush.
         * @return A @ref OStreamImpl::Result carrying the outcome.
         */
        OStreamImpl::Result serialize() noexcept
        {
            auto result = message_.serialize(static_cast<OStreamImpl &>(*this));
            OStreamImpl::flush();
            return result;
        }
    };

    /* ---------------------------------------------------------------------- */
    /* IStream — protobuf-style cursor decoder                                */
    /* ---------------------------------------------------------------------- */

    class IStreamMessage;
    /**
     * @brief Constrains a deserialisable message: must derive from @ref sofab::IStreamMessage.
     * @tparam T Candidate message type.
     */
    template <typename T>
    concept InputMessage = std::derived_from<T, IStreamMessage>;

    /**
     * @brief Base of the input streams: decodes fields from fed bytes.
     *
     * Bytes are supplied through @ref feed, which may be called repeatedly with
     * chunks. A whole message handed in at once is parsed in place with no copy;
     * an incomplete trailing field is buffered and resumed on the next @ref feed.
     * Each complete top-level field is delivered to a callback, inside which
     * @ref read pulls the field's value out at the matching type.
     */
    class IStreamImpl
    {
    public:
        /**
         * @brief Three-valued outcome of a @ref feed call (spec §7).
         *
         * Carries one of @ref Error::None (`COMPLETE`), @ref Error::Incomplete
         * (`INCOMPLETE`) or @ref Error::InvalidMessage (`INVALID`). Read it either
         * as an @ref Error @ref code, as a @ref DecodeStatus via @ref status, or
         * through the boolean predicates @ref complete / @ref incomplete /
         * @ref invalid. @ref incomplete is **not** an error — the bytes merely end
         * mid-field and more may follow.
         */
        class Result
        {
            Error error_;
            friend class IStreamImpl;
            explicit Result(Error e) noexcept : error_(e) {}
        public:
            /// @return `true` only for `COMPLETE` — the bytes end exactly at a field boundary.
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            /// @return `true` only for `COMPLETE`; `false` for both @ref incomplete and @ref invalid.
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            /// @return The three-valued outcome as a @ref DecodeStatus (§7).
            [[nodiscard]] DecodeStatus status() const noexcept
            {
                switch (error_)
                {
                    case Error::None:        return DecodeStatus::Complete;
                    case Error::Incomplete:  return DecodeStatus::Incomplete;
                    default:                 return DecodeStatus::Invalid;
                }
            }
            /// @return `true` if the consumed bytes end exactly at a field boundary (`COMPLETE`).
            [[nodiscard]] bool complete() const noexcept { return error_ == Error::None; }
            /// @return `true` if the bytes end mid-field / with an open sequence (`INCOMPLETE`). Not an error.
            [[nodiscard]] bool incomplete() const noexcept { return error_ == Error::Incomplete; }
            /// @return `true` if the bytes are malformed (`INVALID`).
            [[nodiscard]] bool invalid() const noexcept { return error_ == Error::InvalidMessage; }
            /// @return The status code (@ref Error::None, @ref Error::Incomplete or @ref Error::InvalidMessage).
            [[nodiscard]] Error code() const noexcept { return error_; }
            /// @return `true` if the status code equals @p e.
            bool operator==(Error e) const noexcept { return error_ == e; }
            /// @return `true` if the status code differs from @p e.
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

    protected:
        std::vector<uint8_t> acc_; ///< Buffered bytes spanning @ref feed calls (incomplete trailing field).
        size_t topPos_ = 0;        ///< Parse offset into @ref acc_ of the next unconsumed top-level field.

        /* cursor + current-field metadata, valid during a deliver callback */
        const uint8_t *p_ = nullptr;   ///< Read cursor.
        const uint8_t *end_ = nullptr; ///< One past the last readable byte.
        detail::Wire type_{};          ///< Wire type of the field being delivered.
        detail::Fix fixType_{};        ///< Sub-type of the current fixlen field.
        size_t fixLen_ = 0;            ///< Payload length (fixlen) or element size (fixlen array), in bytes.
        size_t count_ = 0;             ///< Element count of the current array field.
        bool consumed_ = false;        ///< Set once the callback has read the current field's value.
        bool error_ = false;           ///< Sticky decode-error flag for the current @ref feed.
        int seqDepth_ = 0;             ///< Current nested-sequence depth during dispatch (§4.9 @ref MAX_DEPTH).

        std::function<void(sofab::id, size_t, size_t)> topCallback_; ///< Delivers each top-level field.

        /// Construct an empty stream; a derived class installs @ref topCallback_.
        IStreamImpl() noexcept = default;

        /**
         * @brief Read one base-128 varint, advancing the cursor (bounds-checked).
         * @param[in,out] p Cursor; advanced past the varint on success.
         * @param end One past the last readable byte.
         * @param[out] out Decoded value.
         * @return `true` on success, `false` if the buffer ends mid-varint or it overflows 64 bits.
         */
        static bool getVarint(const uint8_t *&p, const uint8_t *end, uint64_t &out) noexcept
        {
            uint64_t v = 0; int shift = 0;
            while (p < end)
            {
                uint8_t b = *p++;
                v |= static_cast<uint64_t>(b & 0x7f) << shift;
                if (!(b & 0x80)) { out = v; return true; }
                shift += 7;
                if (shift >= 64) return false;
            }
            return false;
        }
        /**
         * @brief Advance the cursor past one varint without decoding it (bounds-checked).
         * @param[in,out] p Cursor; advanced past the varint on success.
         * @param end One past the last readable byte.
         * @return `true` on success, `false` if the buffer ends mid-varint.
         */
        static bool skipVarint(const uint8_t *&p, const uint8_t *end) noexcept
        {
            while (p < end) if (!(*p++ & 0x80)) return true;
            return false;
        }

        /**
         * @brief Validate a single `Fixlen` field's length-and-subtype word (§4.6, §6.2).
         *
         * A well-formed word carries a defined subtype (0–3) whose length is legal
         * for that subtype: a `Fp32` payload is exactly 4 bytes, `Fp64` exactly 8,
         * and `String`/`Blob` any length up to @ref FIXLEN_MAX. Subtypes `0b100`..`0b111`
         * are reserved. A violation is malformed **regardless of what follows**
         * (spec §7 `INVALID`), so it must be rejected rather than mistaken for a
         * truncated (`INCOMPLETE`) field.
         *
         * @param word The decoded `fixlen_word` (`(length << 3) | subtype`).
         * @return `true` if the subtype/length pair is legal for a scalar fixlen field.
         */
        static bool fixlenWordValid(uint64_t word) noexcept
        {
            uint64_t len = word >> 3;
            switch (static_cast<detail::Fix>(word & 0x7))
            {
                case detail::Fix::Fp32:   return len == 4;
                case detail::Fix::Fp64:   return len == 8;
                case detail::Fix::String:
                case detail::Fix::Blob:   return len <= FIXLEN_MAX;
                default:                  return false; /* reserved subtype (§4.6) */
            }
        }

        /**
         * @brief Validate a `ArrayFixlen` element word (§4.8, §6.2).
         *
         * A fixlen array may only carry fixed-width elements — `Fp32` (4 bytes) or
         * `Fp64` (8 bytes). Dynamic subtypes (`String`, `Blob`) and the reserved
         * subtypes are **not** permitted as array elements (§4.8); such a word is
         * `INVALID`.
         *
         * @param word The decoded per-element `fixlen_word`.
         * @return `true` if the subtype/element-size pair is legal for a fixlen array.
         */
        static bool arrayFixlenWordValid(uint64_t word) noexcept
        {
            uint64_t esize = word >> 3;
            switch (static_cast<detail::Fix>(word & 0x7))
            {
                case detail::Fix::Fp32: return esize == 4;
                case detail::Fix::Fp64: return esize == 8;
                default:                return false; /* string/blob/reserved (§4.8) */
            }
        }

        /**
         * @brief Append @p n bytes to a byte vector.
         *
         * The surrounding pragma silences a GCC-13 `-Wstringop-overflow` false
         * positive triggered by growing the vector from a raw pointer.
         *
         * @param v Vector to extend.
         * @param p Source bytes.
         * @param n Number of bytes to append.
         */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
        static void appendBytes(std::vector<uint8_t> &v, const uint8_t *p, size_t n)
        {
            size_t old = v.size();
            v.resize(old + n);
            if (n) std::memcpy(v.data() + old, p, n);
        }
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
        /**
         * @brief Load a little-endian float or double from raw bytes.
         * @tparam F Floating-point type to read (`float` or `double`).
         * @param p Pointer to at least `sizeof(F)` readable bytes.
         * @return The decoded value.
         */
        template <std::floating_point F>
        static F loadFloat(const uint8_t *p) noexcept
        {
            if constexpr (sizeof(F) == 4)
            {
                uint32_t b = 0;
                for (int i = 0; i < 4; ++i) b |= static_cast<uint32_t>(p[i]) << (8 * i);
                return detail::bitsFloat<F>(b);
            }
            else
            {
                uint64_t b = 0;
                for (int i = 0; i < 8; ++i) b |= static_cast<uint64_t>(p[i]) << (8 * i);
                return detail::bitsFloat<F>(b);
            }
        }

        /**
         * @brief Advance over one complete field (including nested sequences) without firing callbacks.
         *
         * Used to confirm a whole top-level field is present before delivering it.
         *
         * @param[in,out] p Cursor; advanced past the field on success.
         * @param end One past the last readable byte.
         * @param depth Current nesting depth; nesting past @ref MAX_DEPTH sets the
         *        error flag (§4.9) so the caller reports @ref Error::InvalidMessage.
         * @return `true` if a full field was spanned, `false` if the buffer ends
         *         mid-field or the error flag was set (check @ref error_ to tell them apart).
         */
        bool measureField(const uint8_t *&p, const uint8_t *end, int depth = 0) noexcept
        {
            uint64_t header;
            if (!getVarint(p, end, header)) return false;
            auto type = static_cast<detail::Wire>(header & 0x7);
            switch (type)
            {
                case detail::Wire::Unsigned:
                case detail::Wire::Signed:
                    return skipVarint(p, end);
                case detail::Wire::Fixlen:
                {
                    uint64_t sub; if (!getVarint(p, end, sub)) return false;
                    /* §4.6/§7: a bad subtype or an fp length that isn't 4/8 is
                     * INVALID regardless of what follows — not a truncated field. */
                    if (!fixlenWordValid(sub)) { error_ = true; return false; }
                    size_t len = static_cast<size_t>(sub >> 3);
                    if (static_cast<size_t>(end - p) < len) return false;
                    p += len; return true;
                }
                case detail::Wire::ArrayUnsigned:
                case detail::Wire::ArraySigned:
                {
                    uint64_t n; if (!getVarint(p, end, n)) return false;
                    /* §6.2/§7: a count above ARRAY_MAX is INVALID (and guards the
                     * skip loop below from a malformed, unbounded element count). */
                    if (n > ARRAY_MAX) { error_ = true; return false; }
                    for (uint64_t i = 0; i < n; ++i) if (!skipVarint(p, end)) return false;
                    return true;
                }
                case detail::Wire::ArrayFixlen:
                {
                    uint64_t n; if (!getVarint(p, end, n)) return false;
                    if (n > ARRAY_MAX) { error_ = true; return false; } /* §6.2/§7 */
                    /* §4.8: the fixlen_word is always present, even for a zero-count array. */
                    uint64_t sub; if (!getVarint(p, end, sub)) return false;
                    if (!arrayFixlenWordValid(sub)) { error_ = true; return false; } /* §4.8/§7 */
                    size_t esize = static_cast<size_t>(sub >> 3);
                    size_t bytes = static_cast<size_t>(n) * esize;
                    if (static_cast<size_t>(end - p) < bytes) return false;
                    p += bytes; return true;
                }
                case detail::Wire::SequenceStart:
                {
                    /* §4.9: reject nesting past MAX_DEPTH instead of recursing unbounded. */
                    if (depth + 1 > MAX_DEPTH) { error_ = true; return false; }
                    for (;;)
                    {
                        const uint8_t *save = p;
                        uint64_t peek;
                        const uint8_t *q = p;
                        if (!getVarint(q, end, peek)) return false;
                        if (static_cast<detail::Wire>(peek & 0x7) == detail::Wire::SequenceEnd)
                        { p = q; return true; }
                        p = save;
                        if (!measureField(p, end, depth + 1)) return false;
                    }
                }
                case detail::Wire::SequenceEnd:
                    /* §7: a sequence-end marker with no open sequence is INVALID.
                     * A properly-nested end is consumed by its parent's peek loop
                     * above, so reaching here means a dangling end at this level. */
                    error_ = true; return false;
            }
            return false;
        }

        /**
         * @brief Decode fields at the current nesting level, firing @p cb for each.
         *
         * For every field the metadata members (@ref type_, @ref fixLen_,
         * @ref count_, ...) are set before @p cb runs; a field whose value the
         * callback does not @ref read is skipped automatically.
         *
         * @param cb Callback invoked as `(fieldId, size, count)` per field.
         * @param stopAtEnd If `true`, return at a @ref detail::Wire::SequenceEnd
         *        marker (nested level); if `false`, such a marker is a decode error.
         */
        void dispatchLevel(const std::function<void(sofab::id, size_t, size_t)> &cb, bool stopAtEnd) noexcept
        {
            while (p_ < end_ && !error_)
            {
                uint64_t header;
                if (!getVarint(p_, end_, header)) { error_ = true; return; }
                auto fieldId = static_cast<sofab::id>(header >> 3);
                type_ = static_cast<detail::Wire>(header & 0x7);

                if (type_ == detail::Wire::SequenceEnd)
                {
                    if (stopAtEnd) return;
                    error_ = true; return;
                }

                /* parse the metadata that precedes the payload */
                fixLen_ = 0; count_ = 0;
                if (type_ == detail::Wire::Fixlen)
                {
                    uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                    fixLen_ = static_cast<size_t>(sub >> 3);
                    fixType_ = static_cast<detail::Fix>(sub & 0x7);
                }
                else if (type_ == detail::Wire::ArrayUnsigned || type_ == detail::Wire::ArraySigned)
                {
                    uint64_t n; if (!getVarint(p_, end_, n)) { error_ = true; return; }
                    count_ = static_cast<size_t>(n);
                }
                else if (type_ == detail::Wire::ArrayFixlen)
                {
                    uint64_t n; if (!getVarint(p_, end_, n)) { error_ = true; return; }
                    count_ = static_cast<size_t>(n);
                    /* §4.8: the fixlen_word is always present, even for an empty array. */
                    uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                    fixLen_ = static_cast<size_t>(sub >> 3); /* element size */
                    fixType_ = static_cast<detail::Fix>(sub & 0x7);
                }

                consumed_ = false;
                const uint8_t *payload = p_;
                cb(fieldId, fixLen_, count_);

                if (!consumed_)
                {
                    p_ = payload;
                    skipPayload();
                }
            }
        }

        /**
         * @brief Skip the payload of the current field, leaving the cursor at the next field.
         *
         * Called for fields the user callback chose not to read. Assumes the cursor
         * sits at the start of the payload and the field metadata is set.
         */
        void skipPayload() noexcept
        {
            switch (type_)
            {
                case detail::Wire::Unsigned:
                case detail::Wire::Signed:
                    if (!skipVarint(p_, end_)) error_ = true;
                    break;
                case detail::Wire::Fixlen:
                    if (static_cast<size_t>(end_ - p_) < fixLen_) { error_ = true; break; }
                    p_ += fixLen_;
                    break;
                case detail::Wire::ArrayUnsigned:
                case detail::Wire::ArraySigned:
                    for (size_t i = 0; i < count_; ++i) if (!skipVarint(p_, end_)) { error_ = true; break; }
                    break;
                case detail::Wire::ArrayFixlen:
                {
                    size_t bytes = count_ * fixLen_;
                    if (static_cast<size_t>(end_ - p_) < bytes) { error_ = true; break; }
                    p_ += bytes;
                    break;
                }
                case detail::Wire::SequenceStart:
                    if (seqDepth_ >= MAX_DEPTH) { error_ = true; break; } /* §4.9 */
                    ++seqDepth_;
                    dispatchLevel([](sofab::id, size_t, size_t) {}, /*stopAtEnd*/ true);
                    --seqDepth_;
                    break;
                case detail::Wire::SequenceEnd:
                    break;
            }
        }

    public:
        IStreamImpl(const IStreamImpl &) = delete;
        IStreamImpl &operator=(const IStreamImpl &) = delete;
        IStreamImpl(IStreamImpl &&) noexcept = default;
        IStreamImpl &operator=(IStreamImpl &&) noexcept = default;
        virtual ~IStreamImpl() = default;

        /**
         * @brief Feed bytes into the decoder, delivering every complete top-level field.
         *
         * May be called repeatedly with successive chunks; a field split across
         * chunks is buffered internally and completed on a later call. When nothing
         * is buffered, the chunk is parsed in place without copying.
         *
         * @param buffer Bytes to decode.
         * @param buflen Number of bytes in @p buffer.
         * @return A @ref Result carrying the three-valued §7 outcome:
         *         @ref Error::None (`COMPLETE`) when the fed bytes end exactly at a
         *         field boundary; @ref Error::Incomplete (`INCOMPLETE`) when they end
         *         **inside** a field (partial varint, short fixlen/array payload) or
         *         with an open sequence — the partial tail is buffered for the next
         *         @ref feed and is **not** an error; or @ref Error::InvalidMessage
         *         (`INVALID`) when the bytes are malformed regardless of what follows.
         */
        Result feed(const uint8_t *buffer, size_t buflen) noexcept
        {
            /* Fast path: nothing buffered. Parse straight over the caller's
             * memory — no copy, no allocation. This is the common case (a whole
             * message handed in at once). Only an incomplete trailing field is
             * copied into the accumulator for the next feed(). */
            if (acc_.empty()) [[likely]]
            {
                error_ = false;
                const uint8_t *stop = parseTopLevel(buffer, buffer + buflen);
                if (error_) return Result{Error::InvalidMessage};
                if (stop != buffer + buflen)
                {
                    /* §7: bytes remain that begin but do not finish a field (or an
                     * open sequence). Retain the partial tail and report INCOMPLETE —
                     * distinct from COMPLETE, and never folded into INVALID. */
                    appendBytes(acc_, stop, static_cast<size_t>(buffer + buflen - stop));
                    return Result{Error::Incomplete};
                }
                return Result{Error::None};
            }

            /* Continuation path: append and resume from the buffered tail. */
            appendBytes(acc_, buffer, buflen);
            error_ = false;
            const uint8_t *base = acc_.data();
            const uint8_t *stop = parseTopLevel(base + topPos_, base + acc_.size());
            if (error_) return Result{Error::InvalidMessage};
            topPos_ = static_cast<size_t>(stop - base);
            if (topPos_ == acc_.size()) { acc_.clear(); topPos_ = 0; return Result{Error::None}; } /* fully drained: COMPLETE */
            return Result{Error::Incomplete}; /* §7: a partial field is still buffered */
        }

    protected:
        /**
         * @brief Deliver every complete top-level field in `[p, end)`.
         * @param p Start of the bytes to parse.
         * @param end One past the last readable byte.
         * @return The start of the first incomplete field (equals @p end when all
         *         bytes were consumed).
         */
        const uint8_t *parseTopLevel(const uint8_t *p, const uint8_t *end) noexcept
        {
            while (p < end)
            {
                const uint8_t *probe = p;
                if (!measureField(probe, end))
                {
                    if (error_) return p; /* malformed (e.g. nesting past MAX_DEPTH) */
                    break;                /* need more bytes */
                }
                p_ = p;
                end_ = end;
                dispatchOne(topCallback_);
                if (error_) return p;
                p = p_;
            }
            return p;
        }

    public:

        /**
         * @brief Read the current field's value, dispatching on @p value's type.
         *
         * Call from inside a deliver callback. The requested type must match the
         * field's wire type. Handles integers (signed values are un-zig-zagged),
         * `bool`, `float`, `double`, `std::string`, `std::string_view` (zero-copy,
         * valid while the source bytes live), nested @ref sofab::IStreamMessage objects,
         * and writable contiguous ranges of integers or floats (excess wire
         * elements past the span's capacity are read and discarded). On a malformed
         * or truncated field the stream's error flag is set.
         *
         * @tparam T Type to decode into.
         * @param[out] value Destination for the decoded value.
         */
        template <typename T>
        void read(T &value) noexcept
        {
            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            {
                uint64_t raw;
                if (!getVarint(p_, end_, raw)) { error_ = true; return; }
                if constexpr (std::is_unsigned_v<T>) value = static_cast<T>(raw);
                else                                 value = static_cast<T>(detail::zigzagDecode(raw));
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                uint64_t raw;
                if (!getVarint(p_, end_, raw)) { error_ = true; return; }
                value = (raw != 0);
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
            {
                if (static_cast<size_t>(end_ - p_) < sizeof(T)) { error_ = true; return; }
                value = loadFloat<T>(p_);
                p_ += sizeof(T);
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                /* zero-copy: the view points into the source buffer, valid as
                 * long as that buffer (or this stream's accumulator) lives. */
                if (static_cast<size_t>(end_ - p_) < fixLen_) { error_ = true; return; }
                value = std::string_view(reinterpret_cast<const char *>(p_), fixLen_);
                p_ += fixLen_;
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                if (static_cast<size_t>(end_ - p_) < fixLen_) { error_ = true; return; }
                value.assign(reinterpret_cast<const char *>(p_), fixLen_);
                p_ += fixLen_;
                consumed_ = true;
            }
            else if constexpr (InputMessage<T>)
            {
                /* descend into a nested sequence */
                if (seqDepth_ >= MAX_DEPTH) { error_ = true; return; } /* §4.9 */
                ++seqDepth_;
                dispatchLevel([this, &value](sofab::id i, size_t s, size_t c) {
                    value.deserialize(*this, i, s, c);
                }, /*stopAtEnd*/ true);
                --seqDepth_;
                consumed_ = true;
            }
            else if constexpr (requires { typename T::value_type; std::span{std::declval<T &>()}; } &&
                               !std::is_const_v<typename T::value_type>)
            {
                using Elem = typename T::value_type;
                std::span<Elem> sp{value};
                size_t n = std::min(sp.size(), count_);
                if constexpr (std::is_integral_v<Elem> && !std::is_same_v<Elem, bool>)
                {
                    for (size_t i = 0; i < count_; ++i)
                    {
                        uint64_t raw;
                        if (!getVarint(p_, end_, raw)) { error_ = true; return; }
                        if (i < n)
                        {
                            if constexpr (std::is_unsigned_v<Elem>) sp[i] = static_cast<Elem>(raw);
                            else                                    sp[i] = static_cast<Elem>(detail::zigzagDecode(raw));
                        }
                    }
                    consumed_ = true;
                }
                else if constexpr (std::is_same_v<Elem, float> || std::is_same_v<Elem, double>)
                {
                    size_t bytes = count_ * sizeof(Elem);
                    if (static_cast<size_t>(end_ - p_) < bytes) { error_ = true; return; }
                    if constexpr (std::endian::native == std::endian::little)
                        std::memcpy(sp.data(), p_, n * sizeof(Elem)); /* wire == native */
                    else
                        for (size_t i = 0; i < n; ++i) sp[i] = loadFloat<Elem>(p_ + i * sizeof(Elem));
                    p_ += bytes;
                    consumed_ = true;
                }
                else
                {
                    static_assert(always_false_v<T>, "Unsupported span element type in IStream::read()");
                }
            }
            else
            {
                static_assert(always_false_v<T>, "Unsupported type passed to IStream::read()");
            }
        }

        /**
         * @brief Read the current blob field into a caller buffer.
         *
         * Copies up to @p maxlen bytes; the field is consumed regardless of how
         * much fit. Call from inside a deliver callback.
         *
         * @param dst Destination buffer.
         * @param maxlen Capacity of @p dst in bytes.
         * @return Number of bytes copied (`min(maxlen, payload length)`), or 0 on a truncated field.
         */
        size_t read(void *dst, size_t maxlen) noexcept
        {
            size_t n = std::min(maxlen, fixLen_);
            if (static_cast<size_t>(end_ - p_) < fixLen_) { error_ = true; return 0; }
            std::memcpy(dst, p_, n);
            p_ += fixLen_;
            consumed_ = true;
            return n;
        }

    private:
        /**
         * @brief Decode the field at the cursor, set its metadata and deliver it to @p cb.
         *
         * A field whose value @p cb does not @ref read is skipped automatically.
         *
         * @param cb Callback invoked as `(fieldId, size, count)` for the field.
         */
        void dispatchOne(const std::function<void(sofab::id, size_t, size_t)> &cb) noexcept
        {
            uint64_t header;
            if (!getVarint(p_, end_, header)) { error_ = true; return; }
            auto fieldId = static_cast<sofab::id>(header >> 3);
            type_ = static_cast<detail::Wire>(header & 0x7);

            fixLen_ = 0; count_ = 0;
            if (type_ == detail::Wire::Fixlen)
            {
                uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                fixLen_ = static_cast<size_t>(sub >> 3); fixType_ = static_cast<detail::Fix>(sub & 0x7);
            }
            else if (type_ == detail::Wire::ArrayUnsigned || type_ == detail::Wire::ArraySigned)
            {
                uint64_t n; if (!getVarint(p_, end_, n)) { error_ = true; return; }
                count_ = static_cast<size_t>(n);
            }
            else if (type_ == detail::Wire::ArrayFixlen)
            {
                uint64_t n; if (!getVarint(p_, end_, n)) { error_ = true; return; }
                count_ = static_cast<size_t>(n);
                /* §4.8: the fixlen_word is always present, even for an empty array. */
                uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                fixLen_ = static_cast<size_t>(sub >> 3); fixType_ = static_cast<detail::Fix>(sub & 0x7);
            }

            consumed_ = false;
            const uint8_t *payload = p_;
            cb(fieldId, fixLen_, count_);
            if (!consumed_) { p_ = payload; skipPayload(); }
        }
    };

    /**
     * @brief Input stream that delivers each top-level field to a user callback.
     *
     * The lightweight way to decode: supply a callback, then drive bytes through
     * @ref feed and pull values out with @ref read inside the callback.
     */
    class IStreamInline : public IStreamImpl
    {
    public:
        /// Callback type invoked per top-level field as `(fieldId, size, count)`.
        using fieldCallback = std::function<void(sofab::id, size_t, size_t)>;

        /**
         * @brief Construct with the per-field callback.
         * @param callback Invoked for each complete top-level field.
         */
        explicit IStreamInline(fieldCallback callback) noexcept
        {
            topCallback_ = std::move(callback);
        }
    };

    /**
     * @brief Base class for user-defined messages that can deserialise themselves.
     *
     * Derive from this and implement @ref deserialize to read the message's fields;
     * the type can then be decoded directly (e.g. via @ref IStreamObject or by
     * reading a nested sub-message with @ref IStreamImpl::read).
     */
    class IStreamMessage
    {
        template <InputMessage MessageType>
        friend class IStreamObject;

    public:
        /**
         * @brief Consume one of this message's fields from @p istream.
         *
         * Invoked once per field of the message; use @p id to dispatch and
         * @ref IStreamImpl::read to pull the value.
         *
         * @param istream Stream positioned at the field's value.
         * @param id Identifier of the field being delivered.
         * @param size Payload length (fixlen) or element size (fixlen array), in bytes.
         * @param count Element count for array fields, otherwise 0.
         */
        virtual void deserialize(IStreamImpl &istream, sofab::id id, size_t size, size_t count) noexcept = 0;
        virtual ~IStreamMessage() = default;
    };

    /**
     * @brief Holds a message and routes decoded top-level fields into it.
     *
     * Owns a @p MessageType instance and wires its @ref sofab::IStreamMessage::deserialize
     * as the per-field callback, so feeding bytes populates the message directly.
     * The decoded message is reached through `operator->` / `operator*`.
     *
     * @tparam MessageType A type satisfying @ref sofab::InputMessage.
     */
    template <InputMessage MessageType>
    class IStreamObject : public IStreamImpl
    {
        MessageType data_;

    public:
        /// Construct and route each top-level field into the wrapped message.
        IStreamObject() noexcept
        {
            topCallback_ = [this](sofab::id id, size_t size, size_t count) {
                data_.deserialize(*this, id, size, count);
            };
        }

        /// @return The wrapped message (mutable access).
        MessageType &operator->() noexcept { return data_; }
        /// @return The wrapped message (const access).
        const MessageType &operator->() const noexcept { return data_; }
        /// @return Reference to the wrapped message (mutable access).
        MessageType &operator*() noexcept { return data_; }
        /// @return Reference to the wrapped message (const access).
        const MessageType &operator*() const noexcept { return data_; }
    };

} // namespace sofab

/** @} */ // end of defgroup

#endif // SOFAB_HPP
