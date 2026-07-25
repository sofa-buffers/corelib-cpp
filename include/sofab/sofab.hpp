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

/**
 * @def SOFAB_STRICT_UTF8
 * @brief Compile-time gate for strict UTF-8 validation of `string` payloads
 *        (spec MESSAGE_SPEC §8, CORELIB_PLAN §6.4).
 *
 * SofaBuffers `string` fields carry UTF-8 text; `blob` is the type for opaque
 * bytes. With the check **ON** (the default, `1`) an invalid-UTF-8 `string` is
 * rejected **symmetrically**: on decode it is the `INVALID` outcome
 * (@ref sofab::Error::InvalidMessage), on encode it is refused with
 * @ref sofab::Error::InvalidArgument. `blob` is never validated in either
 * direction, and a **skipped** `string` is never validated (only a materialised
 * read is).
 *
 * The knob is compile-time because this header carries no runtime encode-side
 * configuration object (an `OStream` is constructed from buffers only), so a
 * single `#define` is the only way to gate encode and decode with one symmetric
 * switch — the "compile-time (`#define`)" option §6.4 explicitly permits for
 * C++. Define `SOFAB_STRICT_UTF8` to `0` before including this header for a
 * documented **non-strict** build: the validation code then folds away entirely
 * (zero `.text`/`.rodata` cost) and payloads are stored verbatim — raw, never
 * lossy. Such a build is expected to still build and conformance-test the
 * check-ON configuration (§6.4).
 */
#ifndef SOFAB_STRICT_UTF8
#  define SOFAB_STRICT_UTF8 1
#endif

namespace sofab
{
    /** Version of the SofaBuffers public API implemented by this header. */
    inline constexpr int API_VERSION = 1;

    /* ---------------------------------------------------------------------- */
    /* Wire-format limits                                                     */
    /*                                                                        */
    /* Fixed by MESSAGE_SPEC §6.2 and identical in every corelib.             */
    /* Not configurable and not a policy: exceeding one is malformed          */
    /* input, not a local decision.                                           */
    /* Receiver-side caps a caller CHOOSES are @ref Limits.                   */
    /* ---------------------------------------------------------------------- */

    /**
     * Largest valid field id (`INT32_MAX`). Encoding a larger id is
     * @ref Error::InvalidArgument; decoding one is @ref Error::InvalidMessage.
     */
    inline constexpr uint32_t ID_MAX = 0x7fffffffu;
    /** Largest fixlen payload byte-length (`INT32_MAX`). */
    inline constexpr uint32_t FIXLEN_MAX = 0x7fffffffu;
    /** Largest array element count (`INT32_MAX`). */
    inline constexpr uint32_t ARRAY_MAX = 0x7fffffffu;
    /**
     * Maximum nested-sequence depth (§4.9). Deeper nesting is rejected
     * (encode: @ref Error::InvalidArgument; decode: @ref Error::InvalidMessage).
     */
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

    /** Status codes returned by the encode/decode API; `None` signals success. */
    enum class Error
    {
        None = 0,            /**< Operation succeeded (decode: `COMPLETE`, §7). */
        BufferFull = 3,      /**< The output buffer filled and no flush callback was set. */
        InvalidArgument = 1, /**< An argument was out of range (e.g. a field id above the limit). */
        InvalidMessage = 4,  /**< The input bytes are malformed (decode: `INVALID`, §7). */
        /**
         * Decode-only: the fed bytes end **inside** a field — a partial varint
         * (§4.1), a fixlen/array payload shorter than declared (§4.6/§4.8), or an
         * open sequence (§4.9).
         *
         * This is `INCOMPLETE` (§7) and **not** an error: the caller owns
         * end-of-input and may feed more bytes. Distinct from both @ref None
         * (`COMPLETE`) and @ref InvalidMessage (`INVALID`).
         */
        Incomplete = 5,
        /**
         * Decode-only: a single field would grow the reassembly buffer past the
         * receiver-configured @ref Limits::max_buffered_field.
         *
         * **Policy, not malformation** — deliberately distinct from
         * @ref InvalidMessage, so a differential fuzzer never reads a local
         * buffering limit as a conformance divergence. The bytes are never clamped
         * or truncated; @ref IStreamImpl::feed simply fails with this code.
         */
        LimitExceeded = 6,
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
        Complete = 0,   /**< Consumed bytes end **exactly** at a field boundary — a valid message. */
        Incomplete = 1, /**< Consumed bytes end **inside** a field or with an open sequence. Not an error. */
        Invalid = 2,    /**< Bytes are malformed **regardless of what follows**. Terminal. */
    };

    /** Field identifier on the wire. Valid range is `[0, INT32_MAX]`. */
    using id = uint32_t;

    /* ---------------------------------------------------------------------- */
    /* wire-format primitives                                                 */
    /* ---------------------------------------------------------------------- */


    /** Implementation details of the wire format; not part of the public API. */
    namespace detail
    {
        /**
         * @brief Wire type stored in the low 3 bits of every field header.
         *
         * Internal. The typed reads compare it themselves (§7.3), so neither generated
         * nor hand-written code has to name it.
         */
        enum class Wire : uint8_t
        {
            Unsigned = 0,      /**< Unsigned integer encoded as a varint. */
            Signed = 1,        /**< Signed integer, zig-zag encoded as a varint. */
            Fixlen = 2,        /**< Length-prefixed payload (float, string or blob). */
            ArrayUnsigned = 3, /**< Count-prefixed array of unsigned varints. */
            ArraySigned = 4,   /**< Count-prefixed array of zig-zag varints. */
            ArrayFixlen = 5,   /**< Count-prefixed array of fixed-size elements. */
            SequenceStart = 6, /**< Opens a nested sub-message. */
            SequenceEnd = 7,   /**< Closes the most recently opened sub-message. */
        };

        /**
         * @brief Sub-type of a length-prefixed (`Fixlen`) payload, stored in the low 3 bits of its length word.
         *
         * Internal, like @ref Wire. §7.3 bounds the type check at wire type *plus* this
         * subtype, since `fp32`/`fp64`/`string`/`blob` share @ref Wire::Fixlen.
         */
        enum class Fix : uint8_t
        {
            Fp32 = 0,   /**< 32-bit IEEE-754 float. */
            Fp64 = 1,   /**< 64-bit IEEE-754 double. */
            String = 2, /**< UTF-8 text. */
            Blob = 3,   /**< Opaque byte string. */
        };


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

        /**
         * @brief Validate that `[data, data+len)` is well-formed UTF-8.
         *
         * A real UTF-8 validator, not a byte-range shortcut — this is a security
         * surface (CORELIB_PLAN §6.4). It **rejects**:
         *  - overlong encodings, including `C0 80` (Java "Modified UTF-8" NUL),
         *    `C1`-lead 2-byte forms, and the overlong 3-/4-byte `E0 80..`,
         *    `F0 80..` forms;
         *  - UTF-16 surrogate code points `U+D800`–`U+DFFF` (`ED A0..ED BF`);
         *  - code points above `U+10FFFF` (`F4 90..` and any `F5`–`FF` lead);
         *  - bare continuation bytes and any sequence truncated before its
         *    declared length ends (truncated-at-end is INVALID).
         * It **accepts** an embedded `U+0000` (a single `00` byte): NUL is valid
         * UTF-8 and representable in the length-framed payload (§6.4).
         *
         * `constexpr` and free of `reinterpret_cast`, so it can be evaluated at
         * compile time and unit-tested directly.
         *
         * @param data Pointer to the candidate bytes (may be `nullptr` iff @p len is 0).
         * @param len  Number of bytes to validate.
         * @return `true` iff every byte forms part of a well-formed UTF-8 sequence.
         */
                /* Hoehrmann UTF-8 DFA: class table (0..255) + transition table.
         * ACCEPT = 0, REJECT = 12. Used only for the multi-byte tail; the
         * ASCII run is skipped 8 bytes at a time by the SWAR loop below. */
        inline constexpr uint8_t utf8Dfa[] = {
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
          1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
          7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
          8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
          10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
          0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
          12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
          12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
          12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
          12,36,12,12,12,12,12,12,12,12,12,12,
        };

        [[nodiscard]] constexpr bool utf8Valid(const char *data, size_t len) noexcept
        {
            size_t i = 0;
            uint32_t state = 0;
            while (i < len)
            {
                /* SWAR: skip runs of ASCII 8 bytes at a time. Payloads in
                 * practice are overwhelmingly ASCII, and this is where the
                 * time goes -- a per-byte DFA over the same run is slower. */
                if (!std::is_constant_evaluated())
                {
                    while (i + 8 <= len)
                    {
                        uint64_t w;
                        __builtin_memcpy(&w, data + i, 8);
                        if (w & 0x8080808080808080ULL) break;
                        i += 8;
                    }
                    if (i >= len) break;
                }
                /* Stray ASCII tail: a run shorter than 8 bytes never enters the
                 * SWAR skip, so a mostly-ASCII short payload would otherwise pay
                 * the DFA's two-lookups-per-byte for plain ASCII. Advance ASCII
                 * with one branch, exactly as the pre-DFA scalar loop did; only a
                 * genuine multi-byte lead falls through to the DFA below. */
                if (static_cast<unsigned char>(data[i]) < 0x80) { ++i; continue; }
                /* multi-byte: step the DFA until it is back at ACCEPT, then hand
                 * control to the SWAR loop again. */
                state = utf8Dfa[256u + state + utf8Dfa[static_cast<unsigned char>(data[i])]];
                if (state == 12) return false;
                ++i;
                while (state != 0 && i < len)
                {
                    state = utf8Dfa[256u + state + utf8Dfa[static_cast<unsigned char>(data[i])]];
                    if (state == 12) return false;
                    ++i;
                }
            }
            return state == 0;
        }
    } // namespace detail

    /**
     * @brief Public UTF-8 validity primitive (spec CORELIB_PLAN §6.4).
     *
     * Returns `true` iff @p bytes is well-formed UTF-8 by @ref detail::utf8Valid
     * (rejecting overlong forms, surrogates and out-of-range code points;
     * accepting embedded NUL). Independent of @ref SOFAB_STRICT_UTF8 — this is the
     * validator itself, always available; the compile-time flag only decides
     * whether the encode/decode paths *invoke* it.
     *
     * @param bytes Candidate string payload.
     * @return `true` iff @p bytes is valid UTF-8.
     */
    [[nodiscard]] constexpr bool utf8_valid(std::string_view bytes) noexcept
    {
        return detail::utf8Valid(bytes.data(), bytes.size());
    }

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
        /* The wire tag types are implementation detail (sofab::detail); these
         * aliases keep this class terse without re-exporting the names. */
        using Wire = detail::Wire;
        using Fix = detail::Fix;

    public:
        /** Callback invoked with a span of finished bytes whenever the buffer flushes. */
        using flushCallback = std::function<void(std::span<const uint8_t>)>;

    protected:
        uint8_t *buffer_ = nullptr;   /**< Start of the active buffer. */
        uint8_t *cursor_ = nullptr;   /**< Current write position. */
        uint8_t *end_ = nullptr;      /**< One past the end of the buffer. */
        flushCallback flushCallback_; /**< Invoked when the buffer fills; may be empty. */
        size_t seqDepth_ = 0;         /**< Number of currently-open nested sequences (§4.9 @ref MAX_DEPTH). */
        bool failed_ = false;         /**< Sticky: a write has overflowed (see @ref ok). */

        /** Construct an unattached stream; a derived class must call @ref initBuffer. */
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
                if (!flushCallback_)
                {
                    // Sticky, because a caller may issue writes one at a time and
                    // discard each Result — generated serialize() bodies do — and
                    // then nothing would record that the output was cut short.
                    failed_ = true;
                    return Error::BufferFull;
                }
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
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param type Wire type of the field.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error putHeader(sofab::id fieldId, Wire type) noexcept
        {
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            return putVarint(headerWord(fieldId, type));
        }

        /**
         * @brief Compose a field header word: the id in the high bits, the wire
         *        type in the low three (§4.1).
         *
         * @param fieldId Field identifier.
         * @param w Wire type of the field that follows.
         * @return The header word, ready for @ref encodeVarint.
         */
        static constexpr uint64_t headerWord(sofab::id fieldId, Wire w) noexcept
        {
            return (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(w);
        }

        /**
         * @brief Compose a fixlen word: the payload length (or element size) in
         *        the high bits, the subtype in the low three (§4.6, §4.8).
         *
         * @param len Payload length in bytes, or the element size for an array.
         * @param ft Fixlen subtype.
         * @return The fixlen word, ready for @ref encodeVarint.
         */
        static constexpr uint64_t fixlenWord(uint64_t len, Fix ft) noexcept
        {
            return (len << 3) | static_cast<uint64_t>(ft);
        }

        /**
         * @brief Write a scalar field: header varint plus one value varint, in a single bulk write.
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param type Wire type (@ref Wire::Unsigned or @ref Wire::Signed).
         * @param value Already-encoded scalar value (zig-zagged for signed fields).
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error writeScalar(sofab::id fieldId, Wire type, uint64_t value) noexcept
        {
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, headerWord(fieldId, type));
            n += encodeVarint(tmp + n, value);
            return pushBytes(tmp, n);
        }

        /**
         * @brief Write a length-prefixed field (string, blob or other fixlen payload).
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param ft Payload sub-type (@ref Fix::String, @ref Fix::Blob, ...).
         * @param data Payload bytes.
         * @param len Payload length in bytes.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error writeFixlen(sofab::id fieldId, Fix ft, const uint8_t *data, size_t len) noexcept
        {
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, headerWord(fieldId, Wire::Fixlen));
            n += encodeVarint(tmp + n, fixlenWord(len, ft));
            if (Error e = pushBytes(tmp, n); e != Error::None) return e;
            return pushBytes(data, len);
        }

        /**
         * @brief Write a single float or double as a little-endian fixlen field.
         * @tparam F Floating-point type (`float` or `double`).
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param value Value to encode.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::floating_point F>
        [[nodiscard]] Error writeFloatScalar(sofab::id fieldId, F value) noexcept
        {
            constexpr Fix ft = (sizeof(F) == 4) ? Fix::Fp32 : Fix::Fp64;
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            auto bits = detail::floatBits(value);
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, headerWord(fieldId, Wire::Fixlen));
            n += encodeVarint(tmp + n, fixlenWord(sizeof(F), ft));
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
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param elems Elements to encode, in order.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::integral E>
        [[nodiscard]] Error writeIntArray(sofab::id fieldId, std::span<const E> elems) noexcept
        {
            constexpr bool isSigned = std::is_signed_v<E>;
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            uint8_t hdr[20];
            size_t hn = encodeVarint(hdr, (static_cast<uint64_t>(fieldId) << 3) |
                        static_cast<uint64_t>(isSigned ? Wire::ArraySigned : Wire::ArrayUnsigned));
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
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param elems Elements to encode, in order.
         * @return @ref Error::InvalidArgument if @p fieldId is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::floating_point F>
        [[nodiscard]] Error writeFloatArray(sofab::id fieldId, std::span<const F> elems) noexcept
        {
            constexpr Fix ft = (sizeof(F) == 4) ? Fix::Fp32 : Fix::Fp64;
            if (fieldId > ID_MAX) return Error::InvalidArgument;
            uint8_t hdr[20];
            size_t hn = encodeVarint(hdr, headerWord(fieldId, Wire::ArrayFixlen));
            hn += encodeVarint(hdr + hn, elems.size());
            /* §4.8: a fixlen array always carries its fixlen_word, even when empty
             * (count == 0), so an empty fp32 and fp64 array stay distinguishable. */
            hn += encodeVarint(hdr + hn, fixlenWord(sizeof(F), ft));
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
            /** @return `true` if no error has been latched. */
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            /** @return `true` if no error has been latched. */
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            /** @return The latched status code (@ref Error::None if all writes succeeded). */
            [[nodiscard]] Error code() const noexcept { return error_; }
            /** @return `true` if the latched code equals @p e. */
            bool operator==(Error e) const noexcept { return error_ == e; }
            /** @return `true` if the latched code differs from @p e. */
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

        OStreamImpl(const OStreamImpl &) = delete;
        OStreamImpl &operator=(const OStreamImpl &) = delete;
        OStreamImpl(OStreamImpl &&) noexcept = default;
        OStreamImpl &operator=(OStreamImpl &&) noexcept = default;
        /** Flushes any buffered bytes through the callback before destruction. */
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

        /** @return Number of bytes written into the buffer since the last flush. */
        [[nodiscard]] size_t bytesUsed() const noexcept { return static_cast<size_t>(cursor_ - buffer_); }
        /** @return Pointer to the start of the buffer; the first @ref bytesUsed bytes are valid. */
        [[nodiscard]] const uint8_t *data() const noexcept { return buffer_; }

        /**
         * @brief Whether every write on this stream has succeeded.
         *
         * Sticky and independent of how the writes were issued — chained, or one
         * at a time with each Result discarded, which is what a generated
         * `serialize()` does. The only way it turns false is an overflow with no
         * flush callback set, so it is the verdict to check after encoding into a
         * buffer that may be smaller than the message (@ref OStreamView).
         *
         * @return true while no write has overflowed.
         */
        [[nodiscard]] bool ok() const noexcept { return !failed_; }

        /** @return @ref Error::BufferFull once a write has overflowed, else @ref Error::None. */
        [[nodiscard]] Error error() const noexcept
        {
            return failed_ ? Error::BufferFull : Error::None;
        }

        /**
         * @brief Write a field, dispatching on the value's type.
         *
         * Handles integers (signed values are zig-zag encoded), `bool`, `float`,
         * `double`, anything convertible to `std::string_view`, contiguous ranges
         * of integers or floats (encoded as arrays), and nested @ref sofab::OStreamMessage
         * objects (encoded as a sub-message). Unsupported types fail to compile.
         *
         * @tparam T Deduced value type.
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
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
                    err = writeScalar(fieldId, Wire::Unsigned, static_cast<uint64_t>(value));
                else
                    err = writeScalar(fieldId, Wire::Signed, detail::zigzagEncode(static_cast<int64_t>(value)));
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                err = writeScalar(fieldId, Wire::Unsigned, value ? 1u : 0u);
            }
            else if constexpr (std::is_same_v<T, float>)  { err = writeFloatScalar(fieldId, value); }
            else if constexpr (std::is_same_v<T, double>) { err = writeFloatScalar(fieldId, value); }
            else if constexpr (std::is_convertible_v<T, std::string_view>)
            {
                std::string_view sv{value};
#if SOFAB_STRICT_UTF8
                /* §6.4: a `string` value that is not valid UTF-8 is refused with
                 * InvalidArgument — this is what enforces MESSAGE_SPEC §8's
                 * producer-side MUST NOT. `blob` (the pointer+size overload) is
                 * never validated. Folds away when the check is compiled out. */
                if (!detail::utf8Valid(sv.data(), sv.size()))
                    err = Error::InvalidArgument;
                else
#endif
                    err = writeFixlen(fieldId, Fix::String,
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
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param value Pointer to the bytes to copy.
         * @param size Number of bytes to copy.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result write(sofab::id fieldId, const void *value, int32_t size) noexcept
        {
            return Result{*this, writeFixlen(fieldId, Fix::Blob,
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
            Result r{*this, putHeader(fieldId, Wire::SequenceStart)};
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
            return Result{*this, putHeader(0, Wire::SequenceEnd)};
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
        std::shared_ptr<uint8_t[]> bufferOwner_; /**< Owned backing storage. */
        /** Construct without a buffer; one must be set via @ref setBuffer. */
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
        /** @return A shared handle to the backing buffer. */
        [[nodiscard]] std::shared_ptr<uint8_t[]> getBuffer() noexcept { return bufferOwner_; }
    };

    /**
     * @brief Output stream whose buffer is stored inline (no heap allocation).
     * @tparam N Buffer capacity in bytes; must be greater than zero.
     * @tparam Offset Number of leading bytes to reserve before the cursor; must be less than @p N.
     */
    /**
     * @brief Output stream over a buffer the caller already owns.
     *
     * Neither allocates nor copies: encoding writes straight into @p buffer. The
     * counterpart to @ref OStreamInline (buffer inside the object) and @ref
     * OStream (buffer held by a `shared_ptr`) — this is the one for a destination
     * that already exists, such as the `dst` of a generated `encodeTo`.
     *
     * The buffer must outlive the stream, and it is **not** restored if encoding
     * fails: overflow leaves the bytes written so far in place and @ref ok false.
     */
    class OStreamView : public OStreamImpl
    {
    public:
        /**
         * @brief Construct over caller storage.
         * @param buffer Destination; must outlive this stream.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to leave untouched before the cursor.
         */
        OStreamView(uint8_t *buffer, size_t buflen, size_t offset = 0) noexcept
        {
            initBuffer(buffer, buflen, offset);
        }

        /**
         * @brief Construct over caller storage with a flush callback.
         * @param callback Invoked with finished bytes whenever the buffer fills.
         * @param buffer Destination; must outlive this stream.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to leave untouched before the cursor.
         */
        OStreamView(flushCallback callback, uint8_t *buffer, size_t buflen,
                    size_t offset = 0) noexcept
        {
            flushCallback_ = std::move(callback);
            initBuffer(buffer, buflen, offset);
        }
    };

    template <size_t N, size_t Offset = 0>
    class OStreamInline : public OStreamImpl
    {
        static_assert(N > 0, "Buffer size N must be greater than zero");
        static_assert(Offset < N, "Offset must be less than buffer size N");
        std::array<uint8_t, N> bufferOwner_{}; /**< Inline backing storage. */

    public:
        /** Construct with no flush callback; overflow returns @ref Error::BufferFull. */
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
        /** Construct with no flush callback. */
        OStreamObject() noexcept = default;
        /**
         * @brief Construct with a flush callback.
         * @param callback Invoked with finished bytes whenever the buffer fills.
         */
        explicit OStreamObject(typename OStreamImpl::flushCallback callback) noexcept
            : OStreamInline<N + Offset, Offset>{std::move(callback)} {}

        /** @return The wrapped message, so its fields can be populated via `obj->field`. */
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

    /* ---------------------------------------------------------------------- */
    /* Receiver-side policy                                                   */
    /*                                                                        */
    /* Unlike the wire-format limits above, these are a LOCAL choice:         */
    /* the bytes are well-formed, this receiver just declines to buffer       */
    /* that much. Hence the separate outcome @ref Error::LimitExceeded,       */
    /* which is not INVALID.                                                  */
    /* ---------------------------------------------------------------------- */

    /**
     * @brief Optional receiver-side decode limits for a streaming input stream.
     *
     * A *mechanism* only: the stream enforces whatever cap it is handed and
     * invents no default. Concrete values are configured in the sofabgen config,
     * baked into generated code as constants and passed to the istream
     * constructors (sofa-buffers/generator#102). The default leaves every limit
     * disabled, so behaviour is byte-for-byte that of an unlimited stream.
     */
    struct Limits
    {
        /**
         * @brief Cap on how large the reassembly buffer may grow for a single
         *        incomplete top-level field, in bytes.
         *
         * Checked the moment the size becomes known -- at the header for a fixlen
         * or fixlen-array payload, as it accrues for a sequence -- so an oversized
         * claim fails before its payload is buffered, and even if that payload
         * never arrives. `SIZE_MAX` (the default) means no cap.
         */
        size_t max_buffered_field = SIZE_MAX;
    };

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
        /* The wire tag types are implementation detail (sofab::detail); these
         * aliases keep this class terse without re-exporting the names. */
        using Wire = detail::Wire;
        using Fix = detail::Fix;

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
            size_t skipped_ = 0;
            friend class IStreamImpl;
            explicit Result(Error e, size_t skipped = 0) noexcept : error_(e), skipped_(skipped) {}
        public:
            /**
             * @return Fields skipped because their wire tag contradicted the type
             *         the callback asked for (MESSAGE_SPEC §7.3) — see
             *         @ref IStreamImpl::skipped. A diagnostic: a non-zero count on
             *         a `COMPLETE` result means the message was valid but did not
             *         match this schema everywhere.
             */
            [[nodiscard]] size_t skipped() const noexcept { return skipped_; }
            /** @return `true` only for `COMPLETE` — the bytes end exactly at a field boundary. */
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            /** @return `true` only for `COMPLETE`; `false` for both @ref incomplete and @ref invalid. */
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            /**
             * @return The three-valued §7 outcome as a @ref DecodeStatus. A
             *         @ref Error::LimitExceeded result is a receiver-side policy
             *         failure, **not** one of the three wire outcomes — it maps to
             *         @ref DecodeStatus::Invalid here only as a coarse fallback;
             *         callers distinguishing policy from malformation must use
             *         @ref code / @ref limitExceeded / @ref invalid.
             */
            [[nodiscard]] DecodeStatus status() const noexcept
            {
                switch (error_)
                {
                    case Error::None:        return DecodeStatus::Complete;
                    case Error::Incomplete:  return DecodeStatus::Incomplete;
                    default:                 return DecodeStatus::Invalid;
                }
            }
            /** @return `true` if the consumed bytes end exactly at a field boundary (`COMPLETE`). */
            [[nodiscard]] bool complete() const noexcept { return error_ == Error::None; }
            /** @return `true` if the bytes end mid-field / with an open sequence (`INCOMPLETE`). Not an error. */
            [[nodiscard]] bool incomplete() const noexcept { return error_ == Error::Incomplete; }
            /**
             * @return `true` if the bytes are malformed (`INVALID`). **False** for a
             *         @ref limitExceeded result — a policy cap is not wire malformation.
             */
            [[nodiscard]] bool invalid() const noexcept { return error_ == Error::InvalidMessage; }
            /** @return `true` if a field exceeded @ref Limits::max_buffered_field. Distinct from @ref invalid. */
            [[nodiscard]] bool limitExceeded() const noexcept { return error_ == Error::LimitExceeded; }
            /** @return The status code (@ref Error::None, @ref Error::Incomplete, @ref Error::InvalidMessage or @ref Error::LimitExceeded). */
            [[nodiscard]] Error code() const noexcept { return error_; }
            /** @return `true` if the status code equals @p e. */
            bool operator==(Error e) const noexcept { return error_ == e; }
            /** @return `true` if the status code differs from @p e. */
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

    protected:
        std::vector<uint8_t> acc_; /**< Buffered bytes spanning @ref feed calls (incomplete trailing field). */
        size_t topPos_ = 0;        /**< Parse offset into @ref acc_ of the next unconsumed top-level field. */

        /* cursor + current-field metadata, valid during a deliver callback */
        const uint8_t *p_ = nullptr;   /**< Read cursor. */
        const uint8_t *end_ = nullptr; /**< One past the last readable byte. */
        Wire type_{};          /**< Wire type of the field being delivered. */
        Fix fixType_{};        /**< Sub-type of the current fixlen field. */
        size_t fixLen_ = 0;            /**< Payload length (fixlen) or element size (fixlen array), in bytes. */
        size_t count_ = 0;             /**< Element count of the current array field. */
        bool consumed_ = false;        /**< Set once the callback has read the current field's value. */
        bool error_ = false;           /**< Sticky decode-error flag for the current @ref feed. */
        bool limitExceeded_ = false;   /**< Sticky flag: a field crossed @ref maxBufferedField_ this @ref feed. */
        int seqDepth_ = 0;             /**< Current nested-sequence depth during dispatch (§4.9 @ref MAX_DEPTH). */
        size_t skipped_ = 0;           /**< §7.3 type-mismatch skips seen so far (@ref skipped). */
        bool incomplete_ = false;      /**< The field being delivered needs more bytes (§7 INCOMPLETE, not malformed). */
        bool declined_ = false;        /**< The buffered field was already offered and not read: skip it, do not deliver again. */
        long elemBound_ = -1;          /**< Element-index bound of the wrapper sequence being read (§5.1); -1 = none. */
        sofab::id fieldId_ = 0;        /**< Id of the field being delivered. */
        const uint8_t *fieldStart_ = nullptr; /**< First byte of that field, for the #26 reassembly cap. */

        /** Cap on the reassembly buffer's growth for one incomplete field (@ref Limits::max_buffered_field). */
        size_t maxBufferedField_ = SIZE_MAX;


        std::function<void(sofab::id, size_t, size_t)> topCallback_; /**< Delivers each top-level field. */

        /** Construct an empty stream; a derived class installs @ref topCallback_. */
        IStreamImpl() noexcept = default;
        /** Construct with receiver-side @ref Limits; a derived class installs @ref topCallback_. */
        explicit IStreamImpl(Limits limits) noexcept : maxBufferedField_(limits.max_buffered_field) {}

        /**
         * @brief Read one base-128 varint, advancing the cursor (bounds-checked).
         * @param[in,out] p Cursor; advanced past the varint on success.
         * @param end One past the last readable byte.
         * @param[out] out Decoded value.
         * @return `true` on success, `false` if the buffer ends mid-varint or it
         *         overflows 64 bits. On overflow (a varint > 64 bits, §4.1) `*overflow`
         *         is set: that is INVALID regardless of what follows, and callers in
         *         callers must distinguish it from a merely-truncated tail.
         */
        static bool getVarint(const uint8_t *&p, const uint8_t *end, uint64_t &out,
                              bool *overflow = nullptr) noexcept
        {
            uint64_t v = 0;
            int shift = 0;
            while (p < end)
            {
                const uint8_t b = *p++;
                /* Reject an overlong (> 64-bit) varint before it silently wraps
                 * (§4.1/§6.3): on the 10th byte only the low bit may be set, so
                 * any payload bit that would spill past bit 63 is INVALID. */
                const int room = 64 - shift;
                if (room < 7 && (static_cast<uint8_t>(b & 0x7f) >> room) != 0)
                {
                    if (overflow) *overflow = true;
                    return false;
                }
                v |= static_cast<uint64_t>(b & 0x7f) << shift;
                if (!(b & 0x80))
                {
                    out = v;
                    return true;
                }
                shift += 7;
                if (shift >= 64)
                {
                    if (overflow) *overflow = true;
                    return false;
                }
            }
            return false;
        }
        /**
         * @brief Advance the cursor past one varint without decoding it (bounds-checked).
         * @param[in,out] p Cursor; advanced past the varint on success.
         * @param end One past the last readable byte.
         * @return `true` on success, `false` if the buffer ends mid-varint or it
         *         overflows 64 bits. As with @ref getVarint, `*overflow` is set on a
         *         > 64-bit varint (§4.1), so an over-long varint is never mistaken
         *         for a truncated tail.
         */
        static bool skipVarint(const uint8_t *&p, const uint8_t *end,
                               bool *overflow = nullptr) noexcept
        {
            int shift = 0;
            while (p < end)
            {
                const uint8_t b = *p++;
                /* Same overlong (> 64-bit) rejection as @ref getVarint (§4.1/§6.3):
                 * a 10th byte with any bit above bit 0 set is INVALID. */
                const int room = 64 - shift;
                if (room < 7 && (static_cast<uint8_t>(b & 0x7f) >> room) != 0)
                {
                    if (overflow) *overflow = true;
                    return false;
                }
                if (!(b & 0x80)) return true;
                shift += 7;
                if (shift >= 64)
                {
                    if (overflow) *overflow = true;
                    return false;
                }
            }
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
            switch (static_cast<Fix>(word & 0x7))
            {
                case Fix::Fp32:   return len == 4;
                case Fix::Fp64:   return len == 8;
                case Fix::String:
                case Fix::Blob:   return len <= FIXLEN_MAX;
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
            switch (static_cast<Fix>(word & 0x7))
            {
                case Fix::Fp32: return esize == 4;
                case Fix::Fp64: return esize == 8;
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

        /**
         * @brief The §7.3 seam: does the delivered field's wire tag match the one
         *        the caller's read declares?
         *
         * A field's *tag* is its wire type plus, for the fixlen kinds, the subtype
         * — the two are only meaningful together, since `fp32`, `fp64`, `string`
         * and `blob` all share @ref Wire::Fixlen. Every typed @ref read compares
         * the whole tag here, so half a comparison cannot be written.
         *
         * On a mismatch the field is left unconsumed: @ref dispatchOne then skips
         * it exactly like an unknown id, which is what MESSAGE_SPEC §7.3 requires
         * — this is **not** an error, and never affects the decode outcome. The
         * skip is counted in @ref skipped_ as a diagnostic.
         *
         * @param wantWire Wire type the read declares.
         * @param wantFix  Fixlen subtype it declares; ignored unless @p wantWire is
         *                 a fixlen kind and the caller separates the subtypes.
         * @return `true` when the caller may consume the field.
         */
        [[nodiscard]] bool tagMatches(Wire wantWire) noexcept
        {
            if (type_ == wantWire) return true;
            ++skipped_;
            return false;
        }

        [[nodiscard]] bool tagMatches(Wire wantWire, Fix wantFix) noexcept
        {
            if (type_ == wantWire && fixType_ == wantFix) return true;
            ++skipped_;
            return false;
        }

        /**
         * @brief Would buffering this field cross @ref maxBufferedField_?
         *
         * @param consumed Bytes of the current top-level field already spanned.
         * @param need Further bytes the field is now known to require (a declared
         *        payload length, array byte-span, or per-element lower bound).
         * @return `true` if `consumed + need` exceeds the cap. Overflow-safe: the
         *         addition is never formed, so a `SIZE_MAX` cap always answers `false`.
         */
        [[nodiscard]] bool exceedsBuffer(size_t consumed, uint64_t need) const noexcept
        {
            if (consumed > maxBufferedField_) return true;
            return need > static_cast<uint64_t>(maxBufferedField_ - consumed);
        }


        /**
         * @brief Decode fields at the current nesting level, firing @p cb for each.
         *
         * For every field the metadata members (@ref type_, @ref fixLen_,
         * @ref count_, ...) are set before @p cb runs; a field whose value the
         * callback does not @ref read is skipped automatically.
         *
         * @param cb Callback invoked as `(fieldId, size, count)` per field.
         * @param stopAtEnd If `true`, return at a @ref Wire::SequenceEnd
         *        marker (nested level); if `false`, such a marker is a decode error.
         */
        void dispatchLevel(const std::function<void(sofab::id, size_t, size_t)> &cb, bool stopAtEnd) noexcept
        {
            while (p_ < end_ && !error_ && !incomplete_)
            {
                /* #26: a sequence's own bulk accrues field by field — bound it as it
                 * grows, catching many-small-fields that no single payload check
                 * would trip. */
                if (maxBufferedField_ != SIZE_MAX && fieldStart_ &&
                    exceedsBuffer(static_cast<size_t>(p_ - fieldStart_), 0))
                { limitExceeded_ = true; return; }
                uint64_t header;
                bool ovf = false;
                if (!getVarint(p_, end_, header, &ovf))
                {
                    (ovf ? error_ : incomplete_) = true;
                    return;
                }
                if ((header >> 3) > ID_MAX)
                {
                    error_ = true;
                    return;
                }
                auto fieldId = static_cast<sofab::id>(header >> 3);
                type_ = static_cast<Wire>(header & 0x7);
                /* §5.1/§7: an element index at or past the declared count is INVALID,
                 * decided on the id alone so a truncated element cannot outrun it. */
                if (elemBound_ >= 0 && type_ != Wire::SequenceEnd &&
                    static_cast<long>(fieldId) >= elemBound_)
                { error_ = true; return; }

                if (type_ == Wire::SequenceEnd)
                {
                    if (stopAtEnd) return;
                    error_ = true; return;
                }

                /* parse the metadata that precedes the payload */
                fixLen_ = 0; count_ = 0;
                if (type_ == Wire::Fixlen)
                {
                    uint64_t sub;
                    if (!getVarint(p_, end_, sub, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return;
                    }
                    if (!fixlenWordValid(sub)) /* §4.6/§7 */
                    {
                        error_ = true;
                        return;
                    }
                    fixLen_ = static_cast<size_t>(sub >> 3);
                    fixType_ = static_cast<Fix>(sub & 0x7);
                }
                else if (type_ == Wire::ArrayUnsigned || type_ == Wire::ArraySigned)
                {
                    uint64_t n;
                    if (!getVarint(p_, end_, n, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return;
                    }
                    if (n > ARRAY_MAX) /* §6.2/§7 */
                    {
                        error_ = true;
                        return;
                    }
                    count_ = static_cast<size_t>(n);
                }
                else if (type_ == Wire::ArrayFixlen)
                {
                    uint64_t n;
                    if (!getVarint(p_, end_, n, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return;
                    }
                    count_ = static_cast<size_t>(n);
                    /* §4.8: the fixlen_word is always present, even for an empty array. */
                    uint64_t sub;
                    if (!getVarint(p_, end_, sub, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return;
                    }
                    if (!arrayFixlenWordValid(sub)) /* §4.8/§7 */
                    {
                        error_ = true;
                        return;
                    }
                    fixLen_ = static_cast<size_t>(sub >> 3); /* element size */
                    fixType_ = static_cast<Fix>(sub & 0x7);
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
            /* §7: the bytes ran out with this sequence still open -- INCOMPLETE,
             * not malformed. The whole top-level field is delivered again once the
             * remaining bytes arrive. */
            if (stopAtEnd && !error_) incomplete_ = true;
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
                case Wire::Unsigned:
                case Wire::Signed:
                {
                    bool ovf = false;
                    if (!skipVarint(p_, end_, &ovf)) (ovf ? error_ : incomplete_) = true;
                    break;
                }
                case Wire::Fixlen:
                    if (static_cast<size_t>(end_ - p_) < fixLen_)
                    {
                        incomplete_ = true;
                        break;
                    }
                    p_ += fixLen_;
                    break;
                case Wire::ArrayUnsigned:
                case Wire::ArraySigned:
                    for (size_t i = 0; i < count_; ++i)
                    {
                        bool ovf = false;
                        if (!skipVarint(p_, end_, &ovf))
                        {
                            (ovf ? error_ : incomplete_) = true;
                            break;
                        }
                    }
                    break;
                case Wire::ArrayFixlen:
                {
                    size_t bytes = count_ * fixLen_;
                    if (static_cast<size_t>(end_ - p_) < bytes)
                    {
                        incomplete_ = true;
                        break;
                    }
                    p_ += bytes;
                    break;
                }
                case Wire::SequenceStart:
                    if (seqDepth_ >= MAX_DEPTH) /* §4.9 */
                    {
                        error_ = true;
                        break;
                    }
                    ++seqDepth_;
                    dispatchLevel([](sofab::id, size_t, size_t) {}, /*stopAtEnd*/ true);
                    --seqDepth_;
                    break;
                case Wire::SequenceEnd:
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
                limitExceeded_ = false;
                const uint8_t *stop = parseTopLevel(buffer, buffer + buflen);
                /* #26: a field over the buffering cap fails as policy — checked
                 * before the incomplete tail is copied into acc_, so a claimed
                 * oversize is rejected even though its payload never arrived. */
                if (limitExceeded_) return Result{Error::LimitExceeded, skipped_};
                if (error_) return Result{Error::InvalidMessage, skipped_};
                if (stop != buffer + buflen)
                {
                    /* §7: bytes remain that begin but do not finish a field (or an
                     * open sequence). Retain the partial tail and report INCOMPLETE —
                     * distinct from COMPLETE, and never folded into INVALID. */
                    appendBytes(acc_, stop, static_cast<size_t>(buffer + buflen - stop));
                    return Result{Error::Incomplete, skipped_};
                }
                return Result{Error::None, skipped_};
            }

            /* Continuation path: append and resume from the buffered tail. */
            appendBytes(acc_, buffer, buflen);
            error_ = false;
            limitExceeded_ = false;
            const uint8_t *base = acc_.data();
            const uint8_t *stop = parseTopLevel(base + topPos_, base + acc_.size());
            /* #26: re-checked from the buffered tail, so the cap is chunk-independent —
             * the same field crosses it whether fed whole or dribbled byte by byte. */
            if (limitExceeded_) return Result{Error::LimitExceeded, skipped_};
            if (error_) return Result{Error::InvalidMessage, skipped_};
            topPos_ = static_cast<size_t>(stop - base);
            if (topPos_ == acc_.size()) /* fully drained: COMPLETE */
            {
                acc_.clear(); topPos_ = 0;
                return Result{Error::None, skipped_};
            }
            return Result{Error::Incomplete, skipped_}; /* §7: a partial field is still buffered */
        }

        /**
         * @brief Mark the running decode INVALID from inside a deliver callback.
         *
         * Sets the sticky decode-error flag, so the surrounding @ref feed stops
         * dispatching further fields and returns @ref Error::InvalidMessage
         * (`INVALID`). For callers that detect malformed content the wire layer
         * cannot judge on its own — e.g. a generated message rejecting an array
         * whose wire element count exceeds its schema `count` capacity, which is
         * INVALID per spec §3/§7 (generator#100). Idempotent; a no-op outside a
         * @ref feed since every feed clears the flag on entry.
         */
        void invalidate() noexcept { error_ = true; }

        /**
         * @brief Report a receiver-side decode-limit violation from inside a
         *        deliver callback.
         *
         * Sets the sticky limit flag, so the surrounding @ref feed stops
         * dispatching further fields and returns @ref Error::LimitExceeded.
         * For callers enforcing *policy* caps the wire layer cannot know —
         * e.g. a generated message rejecting an unbounded (schema-bound-less)
         * array/string/blob whose claimed count/length exceeds a configured
         * decode limit (generator#102). Deliberately distinct from
         * @ref invalidate: the bytes may be perfectly well-formed; rejecting
         * them is receiver policy, not wire malformation (§7). Idempotent; a
         * no-op outside a @ref feed since every feed clears the flag on entry.
         */
        void exceedLimit() noexcept { limitExceeded_ = true; }


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
            const bool capped = maxBufferedField_ != SIZE_MAX;
            while (p < end)
            {
                /* Header-first: parse the field's header and metadata, then deliver
                 * immediately. The callback's typed read decides the tag (§7.3), the
                 * schema bound (§7.1/§5.2) and finally whether the payload is here —
                 * in that order, so a bound rejection wins over a truncation without
                 * anyone having to know the schema up front. */
                const uint8_t *fieldStart = p;
                fieldStart_ = p;
                p_ = p; end_ = end;
                incomplete_ = false;
                if (!parseFieldHeader()) return fieldStart; /* error_ or incomplete_ set */
                if (capped && exceedsBufferAtHeader(fieldStart))
                { limitExceeded_ = true; return fieldStart; }

                consumed_ = false;
                const uint8_t *payload = p_;
                /* A field the callback already declined is skipped without being
                 * offered again -- it said no once, and the answer cannot change.
                 * Only a field whose VALUE was wanted is re-delivered. */
                if (!declined_) topCallback_(fieldId_, fixLen_, count_);
                if (error_ || limitExceeded_) return fieldStart;
                /* Not enough bytes yet: rewind to the field header and buffer it. The
                 * whole field is delivered again once it is complete; every generated
                 * destination is either reset wholesale (readArray, prepare()) or
                 * assigned by id, so re-delivery is idempotent. */
                if (incomplete_)
                {
                    declined_ = false;
                    return fieldStart;
                }
                if (!consumed_)
                {
                    p_ = payload;
                    skipPayload();
                    if (error_ || incomplete_)
                    {
                        declined_ = true;
                        return fieldStart;
                    }
                }
                declined_ = false;
                p = p_;
            }
            return p;
        }

        /**
         * @brief Parse one field header plus the metadata that precedes its
         *        payload, into the current-field members.
         *
         * @return `true` when the header is complete and well-formed. On `false`
         *         either @ref error_ (malformed) or @ref incomplete_ (more bytes
         *         needed) is set.
         */
        bool parseFieldHeader() noexcept
        {
            uint64_t header;
            bool ovf = false;
            if (!getVarint(p_, end_, header, &ovf))
            { if (ovf) error_ = true; else incomplete_ = true; return false; }
            if ((header >> 3) > ID_MAX)
            {
                error_ = true;
                return false;
            }
            fieldId_ = static_cast<sofab::id>(header >> 3);
            type_ = static_cast<Wire>(header & 0x7);
            fixLen_ = 0; count_ = 0;
            switch (type_)
            {
                case Wire::Fixlen:
                {
                    uint64_t sub;
                    if (!getVarint(p_, end_, sub, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (!fixlenWordValid(sub)) /* §4.6/§7 */
                    {
                        error_ = true;
                        return false;
                    }
                    fixLen_ = static_cast<size_t>(sub >> 3);
                    fixType_ = static_cast<Fix>(sub & 0x7);
                    break;
                }
                case Wire::ArrayUnsigned:
                case Wire::ArraySigned:
                {
                    uint64_t n;
                    if (!getVarint(p_, end_, n, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (n > ARRAY_MAX) /* §6.2/§7 */
                    {
                        error_ = true;
                        return false;
                    }
                    count_ = static_cast<size_t>(n);
                    break;
                }
                case Wire::ArrayFixlen:
                {
                    uint64_t n;
                    if (!getVarint(p_, end_, n, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (n > ARRAY_MAX)
                    {
                        error_ = true;
                        return false;
                    }
                    count_ = static_cast<size_t>(n);
                    /* §4.8: the fixlen_word is present even for an empty array. */
                    uint64_t sub;
                    if (!getVarint(p_, end_, sub, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (!arrayFixlenWordValid(sub)) /* §4.8/§7 */
                    {
                        error_ = true;
                        return false;
                    }
                    fixLen_ = static_cast<size_t>(sub >> 3);
                    fixType_ = static_cast<Fix>(sub & 0x7);
                    break;
                }
                case Wire::SequenceEnd:
                    error_ = true; return false; /* §7: dangling end at the root */
                default:
                    break;
            }
            return true;
        }

        /**
         * @brief Would this field cross @ref maxBufferedField_, judged from its
         *        header alone (#26)?
         *
         * The header states the size exactly for a fixlen or fixlen-array payload;
         * a varint array's count is a lower bound on its bytes. A sequence accrues
         * instead, and is bounded field by field in @ref dispatchLevel.
         *
         * @param fieldStart First byte of the field being delivered.
         * @return `true` when the field must be rejected before its bytes are
         *         buffered.
         */
        [[nodiscard]] bool exceedsBufferAtHeader(const uint8_t *fieldStart) noexcept
        {
            const size_t spanned = static_cast<size_t>(p_ - fieldStart);
            uint64_t need = 0;
            switch (type_)
            {
                case Wire::Fixlen:      need = fixLen_; break;
                case Wire::ArrayFixlen: need = static_cast<uint64_t>(count_) * fixLen_; break;
                case Wire::ArrayUnsigned:
                case Wire::ArraySigned: need = count_; break;
                default:                need = 0; break;
            }
            return exceedsBuffer(spanned, need);
        }

    public:

        /**
         * @brief Number of fields skipped this stream because their wire tag
         *        contradicted the type the callback asked for (MESSAGE_SPEC §7.3).
         *
         * A diagnostic only — it never influences the decode outcome. A non-zero
         * count on an otherwise `COMPLETE` message means the peer's schema and
         * this one disagree about a field's type (or a hand-written callback
         * asked for the wrong one).
         *
         * Monotonic over the stream's lifetime — unlike the sticky error flags it
         * is **not** cleared per @ref feed, so a message dribbled in chunk by
         * chunk still reports every skip it caused. @ref Result::skipped carries
         * the same value out of a one-shot decode.
         */
        [[nodiscard]] size_t skipped() const noexcept { return skipped_; }

        /**
         * @brief Read the current field's value, dispatching on @p value's type.
         *
         * Call from inside a deliver callback. Handles integers (signed values are un-zig-zagged),
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
        bool read(T &value) noexcept
        {
            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            {
                constexpr Wire want = std::is_unsigned_v<T> ? Wire::Unsigned : Wire::Signed;
                if (!tagMatches(want)) return false; /* §7.3 */
                uint64_t raw;
                bool ovf = false;
                if (!getVarint(p_, end_, raw, &ovf))
                {
                    (ovf ? error_ : incomplete_) = true;
                    return false;
                }
                if constexpr (std::is_unsigned_v<T>) value = static_cast<T>(raw);
                else                                 value = static_cast<T>(detail::zigzagDecode(raw));
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                /* §4.2: bool travels as an unsigned varint. */
                if (!tagMatches(Wire::Unsigned)) return false; /* §7.3 */
                uint64_t raw;
                bool ovf = false;
                if (!getVarint(p_, end_, raw, &ovf))
                {
                    (ovf ? error_ : incomplete_) = true;
                    return false;
                }
                value = (raw != 0);
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
            {
                constexpr Fix want = std::is_same_v<T, float> ? Fix::Fp32 : Fix::Fp64;
                if (!tagMatches(Wire::Fixlen, want)) return false; /* §7.3 */
                if (static_cast<size_t>(end_ - p_) < sizeof(T))
                {
                    incomplete_ = true;
                    return false;
                }
                value = loadFloat<T>(p_);
                p_ += sizeof(T);
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                /* §7.3: `string` and `blob` share Wire::Fixlen and both materialise
                 * into this type, so only the wire type is checked here; a caller
                 * that must separate the two calls @ref readString / @ref readBlob. */
                if (!tagMatches(Wire::Fixlen)) return false;
                /* zero-copy: the view points into the source buffer, valid as
                 * long as that buffer (or this stream's accumulator) lives. */
                if (static_cast<size_t>(end_ - p_) < fixLen_)
                {
                    incomplete_ = true;
                    return false;
                }
#if SOFAB_STRICT_UTF8
                /* §6.4: a materialised `string` (fixlen subtype String) whose
                 * complete payload is not valid UTF-8 is the INVALID outcome —
                 * surfaced via the sticky decode-error flag (same channel as
                 * @ref invalidate), never a throw. The whole payload is present
                 * here (an incomplete field is buffered, not delivered), so a
                 * cross-chunk split stays INCOMPLETE and only a truncated-at-end
                 * or malformed payload reaches this check. `blob` is never
                 * validated; a skipped field never reaches read(). */
                if (fixType_ == Fix::String &&
                    !detail::utf8Valid(reinterpret_cast<const char *>(p_), fixLen_))
                { error_ = true; return false; }
#endif
                value = std::string_view(reinterpret_cast<const char *>(p_), fixLen_);
                p_ += fixLen_;
                consumed_ = true;
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                if (!tagMatches(Wire::Fixlen)) return false; /* §7.3, see the view branch */
                if (static_cast<size_t>(end_ - p_) < fixLen_)
                {
                    incomplete_ = true;
                    return false;
                }
#if SOFAB_STRICT_UTF8
                /* §6.4: reject an invalid-UTF-8 `string` payload as INVALID (see
                 * the std::string_view branch above for the full rationale).
                 * Gated on the wire subtype so a `blob` read into a std::string
                 * is never validated. */
                if (fixType_ == Fix::String &&
                    !detail::utf8Valid(reinterpret_cast<const char *>(p_), fixLen_))
                { error_ = true; return false; }
#endif
                value.assign(reinterpret_cast<const char *>(p_), fixLen_);
                p_ += fixLen_;
                consumed_ = true;
            }
            else if constexpr (InputMessage<T>)
            {
                if (!tagMatches(Wire::SequenceStart)) return false; /* §7.3 */
                /* §7.4: a wrapper sequence IS the array's value, so a repeated field
                 * id REPLACES it whole. The reset that implements that must run only
                 * once the tag is known to match — an occurrence skipped under §7.3
                 * is not an occurrence and must not wipe a valid earlier one. A
                 * collector that needs the reset says so by declaring prepare();
                 * plain struct/union targets do not and pay nothing, since this is a
                 * template and the call disappears at compile time. */
                if constexpr (requires { value.prepare(); }) value.prepare();
                /* §5.1: in a wrapper sequence the element id IS its index, so the
                 * bound is decidable from the header alone -- before the element's
                 * metadata word, which may not have arrived. A collector declares
                 * it by carrying `cap`; the check then lives here rather than in
                 * the collector, where a truncated element would outrun it. */
                const long outerBound = elemBound_;
                /* consumed_ tracks the field at THIS level. A successful inner read
                 * would otherwise make a still-open sequence look taken, and its
                 * caller would not expect the re-delivery that follows. */
                const bool outerConsumed = consumed_;
                consumed_ = false;
                if constexpr (requires { value.cap; }) elemBound_ = value.cap;
                else                                   elemBound_ = -1;
                /* descend into a nested sequence */
                if (seqDepth_ >= MAX_DEPTH) /* §4.9 */
                {
                    error_ = true;
                    return false;
                }
                ++seqDepth_;
                dispatchLevel([this, &value](sofab::id i, size_t s, size_t c) {
                    value.deserialize(*this, i, s, c);
                }, /*stopAtEnd*/ true);
                elemBound_ = outerBound;
                --seqDepth_;
                /* A sequence cut short is NOT consumed: the whole field is
                 * delivered again once its remaining bytes arrive. */
                if (incomplete_)
                {
                    consumed_ = outerConsumed;
                    return false;
                }
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
                    /* §7.3: the element kind selects the array wire type. */
                    constexpr Wire want = std::is_unsigned_v<Elem> ? Wire::ArrayUnsigned : Wire::ArraySigned;
                    if (!tagMatches(want)) return false;
                    for (size_t i = 0; i < count_; ++i)
                    {
                        uint64_t raw;
                        bool ovf = false;
                        if (!getVarint(p_, end_, raw, &ovf))
                        {
                            (ovf ? error_ : incomplete_) = true;
                            return false;
                        }
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
                    constexpr Fix want = std::is_same_v<Elem, float> ? Fix::Fp32 : Fix::Fp64;
                    if (!tagMatches(Wire::ArrayFixlen, want)) return false; /* §7.3 */
                    size_t bytes = count_ * sizeof(Elem);
                    if (static_cast<size_t>(end_ - p_) < bytes)
                    {
                        incomplete_ = true;
                        return false;
                    }
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
            return true;
        }

        /**
         * @brief Read the current field as a `string`, or skip it (§7.3).
         *
         * `fp32`, `fp64`, `string` and `blob` all share @ref Wire::Fixlen, so a
         * plain @ref read cannot tell which one the caller declared. This overload
         * states it: the value is read only when the wire subtype is
         * @ref Fix::String, otherwise the field is left unconsumed — the decoder
         * then skips it exactly like an unknown id, and @ref skipped counts it.
         *
         * @param[out] value Destination for the decoded text.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        bool readString(std::string &value, long bound = -1) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::String)) return false;      /* §7.3 */
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound))        /* §7.1/§5.2 */
            { error_ = true; return false; }
            return read(value);
        }

        /**
         * @brief Read the current field as a `blob`, or skip it (§7.3).
         *
         * The @ref Fix::Blob counterpart of @ref readString; reads straight into
         * the byte container, with no intermediate `std::string`.
         *
         * @param[out] value Destination for the decoded bytes.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        bool readBlob(std::vector<uint8_t> &value, long bound = -1) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::Blob)) return false;
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound)) /* §7.1 */
            {
                error_ = true;
                return false;
            }
            if (static_cast<size_t>(end_ - p_) < fixLen_)
            {
                incomplete_ = true;
                return false;
            }
            value.assign(p_, p_ + fixLen_);
            p_ += fixLen_;
            consumed_ = true;
            return true;
        }

        /**
         * @brief Read a count-prefixed native array, applying every receiver-side
         *        decision at the one word that decides it.
         *
         * Folds what a caller previously had to spell out in four steps — check the
         * wire tag, check the schema `count`, check a configured policy cap, reset
         * the destination — into one call, in the only order that is correct:
         *
         * 1. **Tag** (§7.3). A contradicting array kind is skipped like an unknown
         *    id; nothing else runs, so neither bound below can be applied to a field
         *    that is not this field's value.
         * 2. **Schema `count`** (§7.1/§5.2) → `INVALID`. Malformed input.
         * 3. **Policy cap** (@p dynCap, generator#102) → `LimitExceeded`. A
         *    receiver-side policy, deliberately *not* INVALID: the bytes are fine.
         * 4. **Reset, then fill.** The destination is resized (dynamic container) or
         *    value-initialized (fixed extent) only now, so an occurrence skipped at
         *    step 1 cannot wipe a valid earlier one (§7.4). A fixed array is refilled
         *    from the element default past the wire count, which is what the
         *    trailing-default-run rule expects (§3).
         *
         * @param[out] dst        Destination range (fixed extent or resizable).
         * @param schemaCount     Declared `count: N`, or negative when unbounded.
         * @param dynCap          Configured `max_dyn_array_count`, or negative.
         * @return `true` when the array was read; `false` when it was skipped (§7.3)
         *         or rejected, with the outcome already recorded on the stream.
         */
        template <typename T>
        bool readArray(T &dst, long schemaCount = -1, long dynCap = -1) noexcept
        {
            using Elem = typename T::value_type;
            if constexpr (std::is_same_v<Elem, float> || std::is_same_v<Elem, double>)
            {
                constexpr Fix want = std::is_same_v<Elem, float> ? Fix::Fp32 : Fix::Fp64;
                if (!tagMatches(Wire::ArrayFixlen, want)) return false;
            }
            else
            {
                constexpr Wire want = std::is_unsigned_v<Elem> ? Wire::ArrayUnsigned : Wire::ArraySigned;
                if (!tagMatches(want)) return false;
            }
            if (schemaCount >= 0 && count_ > static_cast<size_t>(schemaCount))
            {
                error_ = true;
                return false;
            }
            if (dynCap >= 0 && count_ > static_cast<size_t>(dynCap))
            {
                limitExceeded_ = true;
                return false;
            }
            if constexpr (requires { dst.resize(count_); }) dst.resize(count_);
            else                                            dst = T{};
            return read(dst);
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
            if (static_cast<size_t>(end_ - p_) < fixLen_)
            {
                error_ = true;
                return 0;
            }
            std::memcpy(dst, p_, n);
            p_ += fixLen_;
            consumed_ = true;
            return n;
        }

        /**
         * @brief Wire type of the field currently being delivered.
         *
         * Valid inside a deliver callback. Introspection only: the typed reads
         * compare the tag themselves and skip a contradicting field (§7.3), so a
         * caller does not need this to be correct — it is here for diagnostics and
         * for code that wants to branch on the delivered form. The type it returns
         * lives in @ref sofab::detail. Reads no bytes and does not consume the
         * field.
         *
         * @return The delivered field's @ref Wire.
         */
        /**
         * @return `true` when a read in this delivery consumed the field. `false`
         *         means the field was declined (skipped) or its payload has not
         *         arrived yet, in which case it is delivered again later.
         */
        [[nodiscard]] bool consumed() const noexcept { return consumed_; }

        [[nodiscard]] Wire wire() const noexcept { return type_; }

        /**
         * @brief Fixlen sub-type of the field currently being delivered.
         *
         * Only meaningful when @ref wire is a fixlen kind. Introspection only,
         * like @ref wire: `readString` / `readBlob` state the declared subtype and
         * the stream compares it. Reads no bytes and does not consume the field.
         *
         * @return The delivered field's @ref Fix.
         */
        [[nodiscard]] Fix fixType() const noexcept { return fixType_; }

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
            if (!getVarint(p_, end_, header))
            {
                error_ = true;
                return;
            }
            if ((header >> 3) > ID_MAX)
            {
                error_ = true;
                return;
            }
            auto fieldId = static_cast<sofab::id>(header >> 3);
            type_ = static_cast<Wire>(header & 0x7);

            fixLen_ = 0; count_ = 0;
            if (type_ == Wire::Fixlen)
            {
                uint64_t sub;
                if (!getVarint(p_, end_, sub))
                {
                    error_ = true;
                    return;
                }
                fixLen_ = static_cast<size_t>(sub >> 3); fixType_ = static_cast<Fix>(sub & 0x7);
            }
            else if (type_ == Wire::ArrayUnsigned || type_ == Wire::ArraySigned)
            {
                uint64_t n;
                if (!getVarint(p_, end_, n))
                {
                    error_ = true;
                    return;
                }
                count_ = static_cast<size_t>(n);
            }
            else if (type_ == Wire::ArrayFixlen)
            {
                uint64_t n;
                if (!getVarint(p_, end_, n))
                {
                    error_ = true;
                    return;
                }
                count_ = static_cast<size_t>(n);
                /* §4.8: the fixlen_word is always present, even for an empty array. */
                uint64_t sub;
                if (!getVarint(p_, end_, sub))
                {
                    error_ = true;
                    return;
                }
                fixLen_ = static_cast<size_t>(sub >> 3); fixType_ = static_cast<Fix>(sub & 0x7);
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
        /** Callback type invoked per top-level field as `(fieldId, size, count)`. */
        using fieldCallback = std::function<void(sofab::id, size_t, size_t)>;

        /**
         * @brief Construct with the per-field callback and optional decode limits.
         * @param callback Invoked for each complete top-level field.
         * @param limits Receiver-side caps (see @ref Limits); default is uncapped.
         */
        explicit IStreamInline(fieldCallback callback, Limits limits = {}) noexcept
            : IStreamImpl(limits)
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
     * @brief A message that both encodes and decodes: exactly
     *        @ref OStreamMessage + @ref IStreamMessage.
     *
     * Almost every message is both, so spelling out the pair at every declaration
     * is noise. This is an empty intermediate base — no storage, no vtable slot of
     * its own, identical layout to inheriting the two directly — so it is a naming
     * convenience and nothing else:
     *
     * ```cpp
     * struct Telemetry : sofab::Message {
     *     sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override;
     *     void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override;
     * };
     * ```
     *
     * Both @ref sofab::InputMessage and @ref sofab::OutputMessage are satisfied
     * through it. Inherit a single side directly when a type really is one-way.
     */
    struct Message : OStreamMessage, IStreamMessage
    {
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
        /**
         * Construct and route each top-level field into the wrapped message.
         * @param limits Receiver-side caps (see @ref Limits); default is uncapped.
         */
        explicit IStreamObject(Limits limits = {}) noexcept
            : IStreamImpl(limits)
        {
            topCallback_ = [this](sofab::id id, size_t size, size_t count) {
                data_.deserialize(*this, id, size, count);
            };
        }

        /** @return The wrapped message (mutable access). */
        MessageType &operator->() noexcept { return data_; }
        /** @return The wrapped message (const access). */
        const MessageType &operator->() const noexcept { return data_; }
        /** @return Reference to the wrapped message (mutable access). */
        MessageType &operator*() noexcept { return data_; }
        /** @return Reference to the wrapped message (const access). */
        const MessageType &operator*() const noexcept { return data_; }
    };

    /* ---------------------------------------------------------------------- */
    /* Wrapper-sequence collectors and encode helpers                         */
    /* ---------------------------------------------------------------------- */

    /**
     * @brief Collects the elements of a `string` wrapper-sequence array.
     *
     * MESSAGE_SPEC §5 lowers an array of strings to a sequence whose child ids are
     * the element indices, so an element is *placed* at its id rather than
     * appended: a default (empty) element is omitted on the wire (§2), and the gap
     * it leaves is filled with the element default.
     *
     * The schema bounds ride into the read, so a mis-typed element is skipped
     * under §7.3 instead of being measured against bounds that are not its own,
     * and an over-index element is rejected by the stream at the element header.
     */
    struct StringSeq : IStreamMessage
    {
        std::vector<std::string> &out;
        long cap;
        long emax;

        /**
         * @param o Destination vector; elements are placed at their index id.
         * @param capacity Schema `count` N, or -1 for an unbounded array. An
         *                 element id at or past N is INVALID (§5.1/§7), rejected
         *                 before the container grows — which also bounds an
         *                 over-index allocation.
         * @param elemMax Element `maxlen`, or -1. A longer element is INVALID
         *                (§7.1), never truncated.
         */
        explicit StringSeq(std::vector<std::string> &o, long capacity = -1, long elemMax = -1) noexcept
            : out(o), cap(capacity), emax(elemMax) {}

        /**
         * §7.4: the sequence IS the array's value, so a repeated field id replaces
         * it whole. @ref IStreamImpl::read calls this once the SequenceStart tag
         * matched, so a §7.3-skipped occurrence cannot wipe a valid earlier one.
         */
        void prepare() noexcept { out.clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t size, size_t) noexcept override
        {
            /* readString decides both, in the order §5.2 needs and before the
             * payload: the declared subtype (§7.3 -- a mis-typed element is not
             * this array's) and then the element maxlen (§7.1). The over-index
             * reject (§5.1) is enforced by the stream at the element header, from
             * `cap` below, since a truncated element would outrun a check here. */
            (void)size;
            std::string s;
            if (!is.readString(s, emax)) return;
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            out[id] = std::move(s);
        }
    };

    /**
     * The `blob` counterpart of @ref StringSeq; see it for the placement and bound
     * rules.
     */
    struct BlobSeq : IStreamMessage
    {
        std::vector<std::vector<uint8_t>> &out;
        long cap;
        long emax;

        /** @copydoc StringSeq::StringSeq */
        explicit BlobSeq(std::vector<std::vector<uint8_t>> &o, long capacity = -1, long elemMax = -1) noexcept
            : out(o), cap(capacity), emax(elemMax) {}

        void prepare() noexcept { out.clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t size, size_t) noexcept override
        {
            (void)size;
            std::vector<uint8_t> b;
            if (!is.readBlob(b, emax)) return; /* §7.3 + §7.1, see StringSeq */
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            out[id] = std::move(b);
        }
    };

    /**
     * @brief Collects a struct/union or nested-array wrapper sequence into a
     *        `std::vector<T>`.
     *
     * One element is emplaced and read per child: @ref IStreamImpl::read descends
     * into a struct/union element's own sub-sequence, or reads a nested array row,
     * exactly as it would for a scalar field.
     *
     * The target is held by pointer rather than a bound reference so one instance
     * can serve several fields.
     *
     * @tparam T Element type — an @ref IStreamMessage, or a container for a
     *           nested-array row.
     */
    template <typename T>
    struct MessageSeq : IStreamMessage
    {
        std::vector<T> *out = nullptr;
        long cap = -1;   /**< Schema `count` N, or -1; an id at or past N is INVALID (§5.1/§7). */

        void prepare() noexcept { if (out) out->clear(); } /**< §7.4 replace-whole; see @ref StringSeq. */

        void deserialize(IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
        {
            if (cap >= 0 && static_cast<size_t>(id) >= static_cast<size_t>(cap))
            {
                is.invalidate();
                return;
            }
            T &row = out->emplace_back();
            /* A count-less native-array row is a std::vector the span read fills
             * only up to its current size, so size it to the row's wire count
             * first. Struct/union rows and fixed std::array rows have no resize(). */
            if constexpr (requires { row.resize(count); } && !std::is_base_of_v<IStreamMessage, T>)
                row.resize(count);
            is.read(row);
        }
    };

    /**
     * @brief Narrow a fixed-count array to its non-default prefix, for encode.
     *
     * MESSAGE_SPEC §3: a `count: N` array's canonical encoding carries `M` = one
     * past the last element that differs from the element default, and the decoder
     * refills `[M, N)`. @ref OStreamImpl::write emits the whole container it is
     * handed, so the value is narrowed to that prefix first. Only a declared
     * `count: N` array is trimmed — a dynamic array has no N to refill from, so its
     * trailing defaults are significant.
     *
     * The comparison is on the element's **byte image**, never `operator==`: a
     * trailing `-0.0` compares equal to `0.0` but is not the default and must stay
     * on the wire, and the same holds for any NaN payload.
     *
     * @param a Contiguous container of trivially-copyable elements.
     * @return A span over `[0, M)`.
     */
    template <typename C>
    std::span<const typename C::value_type> trimTail(const C &a) noexcept
    {
        using Elem = typename C::value_type;
        const Elem zero{};
        size_t n = a.size();
        while (n > 0 && std::memcmp(&a[n - 1], &zero, sizeof(Elem)) == 0) --n;
        return std::span<const Elem>(a.data(), n);
    }

} // namespace sofab

/** @} */ // end of defgroup

#endif // SOFAB_HPP
