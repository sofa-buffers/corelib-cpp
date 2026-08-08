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
#include <limits>
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
     * @brief Smallest output buffer this port accepts **for streaming** (§5.1).
     *
     * This implementation writes byte-at-a-time whenever the buffer is tight —
     * every write funnels through @ref OStreamImpl::pushBytes, which falls back to
     * a per-byte push that flushes across the boundary — so it splits **atomic
     * units** too and declares the smallest value §5.1 admits, `1`. A caller
     * therefore never has to reserve headroom: any non-empty buffer streams a
     * message of any size, and the bytes are identical to the one-shot path.
     *
     * **It binds a buffer installed together with a flush sink**, and only such a
     * buffer: `buflen - offset >= MIN_OUTPUT_BUFFER` is checked where the buffer is
     * handed over — a constructor or @ref OStream::setBuffer — and never partway
     * through a message. A buffer installed **without** a sink is subject to no
     * minimum at all: no flush can occur, so it either holds the message or reports
     * @ref Error::BufferFull, and a two-byte message still encodes into two bytes.
     *
     * A rejected installation leaves the stream inert and latched at
     * @ref Error::InvalidArgument (@ref OStreamImpl::ok is false,
     * @ref OStreamImpl::error reports it); no write it is then asked for touches
     * the buffer.
     */
    inline constexpr size_t MIN_OUTPUT_BUFFER = 1;

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
        /**
         * Encode-only: the encoder ran out of room. Either the output buffer
         * filled and no flush callback was set, or — the rare second case —
         * @ref OStreamImpl::sequenceBeginLazy could not allocate room to hold one
         * more sequence header back (see @ref OStreamImpl::sequenceBeginLazy).
         */
        BufferFull = 3,
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
         * @brief Widest base-128 varint: ceil(64/7) == 10 bytes (§4.1).
         *
         * The encoder and decoder both use it as the "one more varint definitely
         * fits" window, so a capacity check can be hoisted out of an element loop
         * and amortised over a whole chunk instead of paid per element.
         */
        inline constexpr size_t VARINT_MAX_BYTES = 10;

        /**
         * @brief Store a 64-bit word as eight little-endian bytes.
         *
         * The wire is little-endian (§4), so on a little-endian host this is the
         * native representation and compiles to a single unaligned store; on a
         * big-endian one the word is reversed first. `std::byteswap` is C++23, so
         * the reversal is spelled out.
         */
        inline void storeLittle64(uint8_t *out, uint64_t w) noexcept
        {
            if constexpr (std::endian::native == std::endian::big)
                w = ((w & 0x00000000000000FFull) << 56) | ((w & 0x000000000000FF00ull) << 40) |
                    ((w & 0x0000000000FF0000ull) << 24) | ((w & 0x00000000FF000000ull) << 8) |
                    ((w & 0x000000FF00000000ull) >> 8)  | ((w & 0x0000FF0000000000ull) >> 24) |
                    ((w & 0x00FF000000000000ull) >> 40) | ((w & 0xFF00000000000000ull) >> 56);
            std::memcpy(out, &w, sizeof w);
        }

        /**
         * @brief Spread the low 56 bits of @p v into eight 7-bit groups, one per byte.
         *
         * Byte `i` of the result holds bits `[7i+6 : 7i]` of @p v, with bit 7 of
         * each byte clear — exactly the payload layout of the first eight bytes of
         * a base-128 varint. Three halve-and-shift rounds do it in a handful of
         * ALU ops, which is what lets the encoder emit eight varint bytes as one
         * store instead of eight.
         */
        constexpr uint64_t spread7(uint64_t v) noexcept
        {
            uint64_t w = v & 0x00FFFFFFFFFFFFFFull;                                  /* 56 bits */
            w = (w & 0x000000000FFFFFFFull) | ((w & 0x00FFFFFFF0000000ull) << 4);    /* 2 x 28 */
            w = (w & 0x00003FFF00003FFFull) | ((w & 0x0FFFC0000FFFC000ull) << 2);    /* 4 x 14 */
            w = (w & 0x007F007F007F007Full) | ((w & 0x3F803F803F803F80ull) << 1);    /* 8 x 7  */
            return w;
        }

        /**
         * @brief Inverse of @ref spread7: pack eight bytes' low 7 bits into one value.
         *
         * Byte `i` of @p w contributes bits `[7i+6 : 7i]` of the result; bit 7 of
         * each byte (the varint continuation flag) is discarded by the masks, so
         * the caller need not clear it. The three rounds mirror @ref spread7's,
         * run in the opposite order.
         */
        constexpr uint64_t gather7(uint64_t w) noexcept
        {
            w = (w & 0x007F007F007F007Full) | ((w & 0x7F007F007F007F00ull) >> 1);    /* 4 x 14 */
            w = (w & 0x00003FFF00003FFFull) | ((w & 0x3FFF00003FFF0000ull) >> 2);    /* 2 x 28 */
            w = (w & 0x000000000FFFFFFFull) | ((w & 0x0FFFFFFF00000000ull) >> 4);    /* 56 bits */
            return w;
        }

        /** @brief Load eight little-endian bytes as a 64-bit word (see @ref storeLittle64). */
        inline uint64_t loadLittle64(const uint8_t *in) noexcept
        {
            uint64_t w;
            std::memcpy(&w, in, sizeof w);
            if constexpr (std::endian::native == std::endian::big)
                w = ((w & 0x00000000000000FFull) << 56) | ((w & 0x000000000000FF00ull) << 40) |
                    ((w & 0x0000000000FF0000ull) << 24) | ((w & 0x00000000FF000000ull) << 8) |
                    ((w & 0x000000FF00000000ull) >> 8)  | ((w & 0x0000FF0000000000ull) >> 24) |
                    ((w & 0x00FF000000000000ull) >> 40) | ((w & 0xFF00000000000000ull) >> 56);
            return w;
        }

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

        /**
         * @brief The element ceiling a decode destination @p T publishes, or `-1`
         *        when it publishes none.
         *
         * The bound @ref IStreamImpl::readArray compares the wire count against
         * before it fills anything (MESSAGE_SPEC §3): a heap-free destination that
         * cannot reach the count must reject it, never truncate into it.
         *
         * Told apart by the destination's own capabilities rather than by name, so
         * a caller's own container works too — a **static `capacity()`** is the
         * ceiling (@ref InlineVector and friends: `resize` exists but clamps to
         * `N`), everything else is treated as growable. `T::capacity()` is
         * deliberately spelled as an unqualified static call: `std::vector`'s
         * `capacity()` is a non-static member, so naming it this way is ill-formed
         * and the requirement fails — exactly the growable / heap-free split
         * @ref IStreamImpl::readString already keys on.
         *
         * A destination of **fixed extent that publishes nothing** (`std::array`,
         * a bound span) is not covered here: that is @ref IStreamImpl::read's
         * low-level contract, where the leading elements land in the destination
         * and the rest is parsed only to stay framed.
         */
        template <typename T>
        constexpr long destCapacity() noexcept
        {
            if constexpr (requires { T::capacity(); }) return static_cast<long>(T::capacity());
            else                                       return -1;
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
    /* Heap-free containers                                                   */
    /* ---------------------------------------------------------------------- */

    /*
     * The storage a schema-bounded field lowers to when the caller asks for
     * static storage (generator `allow_dynamic: false`). They are the destination
     * side of the same contract std::string / std::vector satisfy, so the typed
     * reads below accept either and one generated header shape serves both.
     *
     * These three types are DELIBERATELY identical, in name and behaviour, to
     * corelib-c-cpp's: the generator emits the same `sofab::FixedString<N>` /
     * `sofab::FixedBytes<N>` / `sofab::InlineVector<T,N>` member for a bounded
     * field whichever C++ corelib is selected, so the storage decision stays a
     * property of the schema rather than of the runtime. Any behavioural change
     * here has to land in corelib-c-cpp too, or the two profiles stop agreeing on
     * what a schema means.
     */

    /*!
     * @brief Fixed-capacity, heap-free string of up to @p N characters.
     *
     * A drop-in, embedded-friendly stand-in for @c std::string on both the encode
     * and decode paths. The characters live in an inline @c std::array, so an
     * instance allocates nothing, never throws (overflow clamps to @p N), and
     * compiles cleanly under @c -fno-exceptions / @c -fno-rtti. The buffer never
     * moves, so an instance stays a valid decode destination across every
     * @ref IStreamImpl::feed chunk.
     *
     * The storage is @c N+1 bytes: one extra slot always holds a trailing NUL so
     * @ref c_str and the @c std::string_view encode path remain valid even at full
     * length @p N. The buffer is zero-initialised, so the NUL is present from
     * construction and is re-placed by every length-changing operation.
     *
     * @par Generator integration contract
     * Generated code is spelled exactly as for @c std::string, so this type only
     * has to keep the surface the typed calls look for:
     *   - decode emits @c is.readString(s, maxlen); — @ref IStreamImpl::readString
     *     picks the heap-free branch off @ref set_len / @ref capacity, rejects an
     *     over-@p N payload as INVALID (§7.1) rather than truncating it, and fills
     *     @ref data before fixing @ref size;
     *   - encode emits @c os.write(id, s); — the implicit @ref operator std::string_view
     *     routes it through the existing string encode branch, byte-for-byte
     *     identical to the same-content @c std::string.
     *
     * @tparam N  Maximum number of characters (excluding the reserved NUL slot).
     */
    template <std::size_t N>
    class FixedString
    {
        std::array<char, N + 1> buf_{};     //!< Inline storage (+1 for the NUL).
        std::size_t len_ = 0;               //!< Current logical length (<= N).

    public:
        /*! @brief Character type (mirrors @c std::string). */
        using value_type = char;
        /*! @brief Size type (mirrors @c std::string). */
        using size_type = std::size_t;

        /*! @brief Construct an empty string. */
        FixedString() noexcept = default;

        /*!
         * @brief Construct from a NUL-terminated C string (truncated to @p N).
         * @param s  Source string, or @c nullptr for an empty string.
         */
        FixedString(const char *s) noexcept
        {
            assign(s ? std::string_view{s} : std::string_view{});
        }

        /*!
         * @brief Construct from a string view (truncated to @p N).
         * @param sv  Source characters (may contain embedded NULs).
         */
        FixedString(std::string_view sv) noexcept
        {
            assign(sv);
        }

        /*!
         * @brief Construct from a @c std::string (the easy on-ramp; truncated to @p N).
         * @param s  Source string.
         */
        FixedString(const std::string &s) noexcept
        {
            assign(std::string_view{s});
        }

        /*! @brief Assign from a NUL-terminated C string (truncated to @p N). */
        FixedString &operator=(const char *s) noexcept
        {
            assign(s ? std::string_view{s} : std::string_view{});
            return *this;
        }

        /*! @brief Assign from a string view (truncated to @p N). */
        FixedString &operator=(std::string_view sv) noexcept
        {
            assign(sv);
            return *this;
        }

        /*! @brief Assign from a @c std::string (truncated to @p N). */
        FixedString &operator=(const std::string &s) noexcept
        {
            assign(std::string_view{s});
            return *this;
        }

        /*!
         * @brief Replace the contents with @p sv, truncated to @p N characters.
         * @param sv  Source characters (may contain embedded NULs).
         * @return Reference to @c *this.
         */
        FixedString &assign(std::string_view sv) noexcept
        {
            len_ = sv.size() > N ? N : sv.size();
            for (std::size_t i = 0; i < len_; ++i)
            {
                buf_[i] = sv[i];
            }
            buf_[len_] = '\0';
            return *this;
        }

        /*!
         * @brief Decode hook: set the logical length and (re)place the trailing NUL.
         *
         * Fixes @c size() to the decoded payload length (clamped to @p N) and
         * writes the NUL at @c buf_[len_]. @ref IStreamImpl::readString calls it
         * after copying @c [0, size()) and never touches @c buf_[len_], so the NUL
         * survives and @ref c_str stays valid. Re-decoding a shorter value
         * re-terminates here. Its presence is also what marks this type as a
         * heap-free destination to the typed reads (@c requires { set_len(...) }).
         *
         * @param n  Requested logical length (clamped to @p N).
         */
        void set_len(std::size_t n) noexcept
        {
            len_ = n > N ? N : n;
            buf_[len_] = '\0';
        }

        /*! @brief Mutable pointer to the character buffer (decode target). */
        char *data() noexcept { return buf_.data(); }
        /*! @brief Const pointer to the character buffer. */
        const char *data() const noexcept { return buf_.data(); }
        /*! @brief NUL-terminated view of the contents. */
        const char *c_str() const noexcept { return buf_.data(); }

        /*! @brief Number of characters currently stored. */
        std::size_t size() const noexcept { return len_; }
        /*! @brief Alias of @ref size. */
        std::size_t length() const noexcept { return len_; }
        /*! @brief True if the string is empty. */
        bool empty() const noexcept { return len_ == 0; }
        /*! @brief Maximum number of characters (the template parameter @p N). */
        static constexpr std::size_t capacity() noexcept { return N; }
        /*! @brief Alias of @ref capacity. */
        static constexpr std::size_t max_size() noexcept { return N; }

        /*! @brief Access the character at @p i (no bounds checking). */
        char &operator[](std::size_t i) noexcept { return buf_[i]; }
        /*! @brief Access the character at @p i (no bounds checking). */
        const char &operator[](std::size_t i) const noexcept { return buf_[i]; }

        /*! @brief Iterator to the first character. */
        char *begin() noexcept { return buf_.data(); }
        /*! @brief Iterator past the last character. */
        char *end() noexcept { return buf_.data() + len_; }
        /*! @brief Const iterator to the first character. */
        const char *begin() const noexcept { return buf_.data(); }
        /*! @brief Const iterator past the last character. */
        const char *end() const noexcept { return buf_.data() + len_; }

        /*! @brief Reset to an empty string. */
        void clear() noexcept
        {
            len_ = 0;
            buf_[0] = '\0';
        }

        /*! @brief Non-owning view over the current characters. */
        std::string_view view() const noexcept
        {
            return std::string_view{buf_.data(), len_};
        }

        /*!
         * @brief Implicit conversion to @c std::string_view.
         *
         * Gives a cheap non-owning view and makes the existing string encode
         * branch (@c OStreamImpl::write) match a @c FixedString automatically.
         */
        operator std::string_view() const noexcept
        {
            return view();
        }

        /*! @brief Copy the contents into an owning @c std::string (allocates). */
        std::string str() const
        {
            return std::string{buf_.data(), len_};
        }

        /*! @brief Equality against any string view-like operand. */
        bool operator==(std::string_view rhs) const noexcept
        {
            return view() == rhs;
        }

        /*! @brief Inequality against any string view-like operand. */
        bool operator!=(std::string_view rhs) const noexcept
        {
            return view() != rhs;
        }
    };

    /*!
     * @brief Fixed-capacity, heap-free byte blob of up to @p N bytes.
     *
     * The embedded-friendly counterpart of @c std::vector<std::uint8_t> for blob
     * fields, mirroring @ref FixedString for bytes. The payload lives in an inline
     * @c std::array, so an instance allocates nothing and never throws (overflow
     * clamps to @p N). The buffer never moves, so an instance stays a valid decode
     * destination across every @ref IStreamImpl::feed chunk.
     *
     * A @b logical @b length (@ref size, @c <= @p N) is tracked separately from the
     * capacity @p N: a blob shorter than its schema @c maxlen occupies only
     * @ref size bytes on the wire. This is exactly why the type cannot be a plain
     * @c std::array<std::uint8_t,N> (always length @p N) and must not reintroduce
     * the heap of @c std::vector.
     *
     * @par Generator integration contract
     * Generated code is spelled exactly as for @c std::vector<std::uint8_t>:
     *   - encode passes @ref data / @ref size to the blob write, byte-for-byte
     *     identical to the same-content vector;
     *   - decode emits @c is.readBlob(b, maxlen); — @ref IStreamImpl::readBlob picks
     *     the heap-free branch off @ref set_len / @ref capacity and rejects an
     *     over-@p N wire length as INVALID per MESSAGE_SPEC §7.1 instead of
     *     truncating it.
     *
     * @tparam N  Maximum number of bytes.
     */
    template <std::size_t N>
    class FixedBytes
    {
        std::array<std::uint8_t, N> buf_{};     //!< Inline storage.
        std::size_t len_ = 0;                   //!< Current logical length (<= N).

    public:
        /*! @brief Element type (mirrors @c std::vector). */
        using value_type = std::uint8_t;
        /*! @brief Size type (mirrors @c std::vector). */
        using size_type = std::size_t;

        /*! @brief Construct an empty blob. */
        FixedBytes() noexcept = default;

        /*!
         * @brief Construct from a brace-enclosed list of bytes (truncated to @p N).
         *
         * Providing this constructor makes @c FixedBytes a non-aggregate, so a
         * brace-init such as @c b = {1, 2, 3} routes through here and sets
         * @ref size — it cannot silently fill the buffer while leaving the logical
         * length at zero.
         *
         * @param init  Source bytes (excess beyond @p N is dropped).
         */
        FixedBytes(std::initializer_list<std::uint8_t> init) noexcept
        {
            assign(init);
        }

        /*! @brief Replace the contents from a brace-enclosed list (truncated to @p N). */
        FixedBytes &operator=(std::initializer_list<std::uint8_t> init) noexcept
        {
            return assign(init);
        }

        /*! @brief Replace the contents from a brace-enclosed list (truncated to @p N). */
        FixedBytes &assign(std::initializer_list<std::uint8_t> init) noexcept
        {
            len_ = 0;
            for (std::uint8_t b : init)
            {
                if (len_ >= N)
                {
                    break;
                }
                buf_[len_++] = b;
            }
            return *this;
        }

        /*! @brief Mutable pointer to the byte buffer (decode target). */
        std::uint8_t *data() noexcept { return buf_.data(); }
        /*! @brief Const pointer to the byte buffer. */
        const std::uint8_t *data() const noexcept { return buf_.data(); }

        /*! @brief Number of bytes currently stored. */
        std::size_t size() const noexcept { return len_; }
        /*! @brief True if the blob is empty. */
        bool empty() const noexcept { return len_ == 0; }
        /*! @brief Maximum number of bytes (the template parameter @p N). */
        static constexpr std::size_t capacity() noexcept { return N; }
        /*! @brief Alias of @ref capacity. */
        static constexpr std::size_t max_size() noexcept { return N; }

        /*!
         * @brief Decode hook: set the logical length (clamped to @p N).
         *
         * Called by generated decode before binding the buffer, so @ref size
         * reports the field length and @ref data over that many bytes is the
         * fill target.
         *
         * @param n  Requested logical length (clamped to @p N).
         */
        void set_len(std::size_t n) noexcept { len_ = n < N ? n : N; }

        /*! @brief Reset to an empty blob. */
        void clear() noexcept { len_ = 0; }

        /*! @brief Append one byte (no-op once at capacity @p N). */
        void push_back(std::uint8_t b) noexcept
        {
            if (len_ < N)
            {
                buf_[len_++] = b;
            }
        }

        /*! @brief Access the byte at @p i (no bounds checking). */
        std::uint8_t &operator[](std::size_t i) noexcept { return buf_[i]; }
        /*! @brief Access the byte at @p i (no bounds checking). */
        const std::uint8_t &operator[](std::size_t i) const noexcept { return buf_[i]; }

        /*! @brief Iterator to the first byte. */
        std::uint8_t *begin() noexcept { return buf_.data(); }
        /*! @brief Iterator past the last byte. */
        std::uint8_t *end() noexcept { return buf_.data() + len_; }
        /*! @brief Const iterator to the first byte. */
        const std::uint8_t *begin() const noexcept { return buf_.data(); }
        /*! @brief Const iterator past the last byte. */
        const std::uint8_t *end() const noexcept { return buf_.data() + len_; }

        /*! @brief Content equality (same logical length and bytes). */
        bool operator==(const FixedBytes &o) const noexcept
        {
            if (len_ != o.len_)
            {
                return false;
            }
            for (std::size_t i = 0; i < len_; ++i)
            {
                if (buf_[i] != o.buf_[i])
                {
                    return false;
                }
            }
            return true;
        }

        /*! @brief Negated @ref operator==. */
        bool operator!=(const FixedBytes &o) const noexcept { return !(*this == o); }
    };

    /*!
     * @brief Fixed-capacity, heap-free sequence of up to @p N elements of type @p T.
     *
     * The embedded-friendly counterpart of @c std::vector<T> for every array a
     * schema bounds with a @c count: native scalars as well as strings, blobs,
     * structs/unions and nested arrays. Elements live in an inline
     * @c std::array, so the storage never reallocates and an element being filled
     * across @ref IStreamImpl::feed chunks stays address-stable — strictly safer
     * than a @c std::vector + @c reserve.
     *
     * @ref resize is what marks this type as a resizable destination to
     * @ref IStreamImpl::readArray, which sizes it to the wire element count. That
     * count IS the array's length (MESSAGE_SPEC §3); @p N is the schema @c count,
     * a capacity that bounds it and never adds to it.
     *
     * A @b logical @b length (@ref size, @c <= @p N) is tracked separately from the
     * capacity @p N: an array shorter than its schema @c count holds only
     * @ref size elements. This is why the type is neither a plain
     * @c std::array<T,N> (always length @p N) nor a heap-backed @c std::vector.
     *
     * @warning Historically this was an aggregate with a public, default-zero
     * length, so a natural brace-init such as @c v = {a, b, c} silently filled the
     * storage while leaving the logical length at 0 — the field then encoded as
     * empty. The @c initializer_list constructor/assignment below make the type a
     * non-aggregate, so that brace-init now sets @ref size correctly instead of
     * corrupting the wire.
     *
     * @tparam T  Element type.
     * @tparam N  Maximum number of elements.
     */
    template <typename T, std::size_t N>
    class InlineVector
    {
        std::array<T, N> buf_{};    //!< Inline storage.
        std::size_t len_ = 0;       //!< Current logical length (<= N).

    public:
        /*! @brief Element type (mirrors @c std::vector). */
        using value_type = T;
        /*! @brief Size type (mirrors @c std::vector). */
        using size_type = std::size_t;

        /*! @brief Construct an empty sequence. */
        InlineVector() noexcept = default;

        /*!
         * @brief Construct from a brace-enclosed list of elements (truncated to @p N).
         *
         * The presence of this constructor makes @c InlineVector a non-aggregate:
         * @c v = {a, b, c} routes here and sets @ref size, instead of aggregate
         * brace-init filling the storage while leaving the length at 0.
         *
         * @param init  Source elements (excess beyond @p N is dropped).
         */
        InlineVector(std::initializer_list<T> init) noexcept
        {
            assign(init);
        }

        /*! @brief Replace the contents from a brace-enclosed list (truncated to @p N). */
        InlineVector &operator=(std::initializer_list<T> init) noexcept
        {
            return assign(init);
        }

        /*! @brief Replace the contents from a brace-enclosed list (truncated to @p N). */
        InlineVector &assign(std::initializer_list<T> init) noexcept
        {
            len_ = 0;
            for (const T &v : init)
            {
                if (len_ >= N)
                {
                    break;
                }
                buf_[len_++] = v;
            }
            return *this;
        }

        /*! @brief Number of elements currently stored. */
        std::size_t size() const noexcept { return len_; }
        /*! @brief True if the sequence is empty. */
        bool empty() const noexcept { return len_ == 0; }
        /*! @brief Maximum number of elements (the template parameter @p N). */
        static constexpr std::size_t capacity() noexcept { return N; }
        /*! @brief Alias of @ref capacity. */
        static constexpr std::size_t max_size() noexcept { return N; }

        /*! @brief No-op (inline storage never reallocates); present for API parity. */
        void reserve(std::size_t) noexcept {}
        /*! @brief Reset to an empty sequence (logical length only). */
        void clear() noexcept { len_ = 0; }

        /*!
         * @brief Set the logical length to @p n, value-initializing what changes.
         *
         * The @c std::vector member of the container API this type mirrors, and the
         * one @ref IStreamImpl::readArray and the wrapper-array collectors probe
         * for: readArray *resizes* a resizable destination and *value-initializes*
         * a fixed-extent one, and without this method an @c InlineVector matched
         * neither — it fell to the fixed-extent branch, which assigned a
         * default-constructed container and so set the logical length to 0. The
         * decode then bound an empty span and dropped the array silently. With
         * @ref resize present, readArray keeps ownership of the tag / bound /
         * reset / bind order it documents, for inline storage too.
         *
         * Slots that enter or leave the logical range are set to @c T{}, so the
         * elements a shorter value no longer covers cannot be observed through a
         * later grow. @p n above the capacity @p N is clamped to @p N — the callers
         * that can reject an over-capacity count do so before resizing (readArray
         * checks the schema `count` first), and a heap-free container has nowhere
         * to put the excess.
         *
         * @param n  New logical length.
         */
        void resize(std::size_t n) noexcept
        {
            if (n > N)
            {
                n = N;
            }
            for (std::size_t i = n; i < len_; ++i)
            {
                buf_[i] = T{};
            }
            for (std::size_t i = len_; i < n; ++i)
            {
                buf_[i] = T{};
            }
            len_ = n;
        }

        /*!
         * @brief Append a default-constructed element and return a reference to it.
         *
         * The next inline slot is (re)set to @c T{} and bound; once at capacity
         * @p N the last slot is reused so a decode never writes out of bounds.
         * @return Reference to the newly active element.
         */
        T &emplace_back() noexcept
        {
            std::size_t i = len_ < N ? len_++ : N - 1;
            buf_[i] = T{};
            return buf_[i];
        }

        /*! @brief Append a copy of @p v (no-op growth once at capacity @p N). */
        void push_back(const T &v) noexcept { emplace_back() = v; }
        /*! @brief Append @p v by move (no-op growth once at capacity @p N). */
        void push_back(T &&v) noexcept { emplace_back() = static_cast<T &&>(v); }

        /*! @brief Reference to the last element. */
        T &back() noexcept { return buf_[len_ - 1]; }
        /*! @brief Const reference to the last element. */
        const T &back() const noexcept { return buf_[len_ - 1]; }

        /*! @brief Access the element at @p i (no bounds checking). */
        T &operator[](std::size_t i) noexcept { return buf_[i]; }
        /*! @brief Access the element at @p i (no bounds checking). */
        const T &operator[](std::size_t i) const noexcept { return buf_[i]; }

        /*! @brief Mutable pointer to the underlying storage. */
        T *data() noexcept { return buf_.data(); }
        /*! @brief Const pointer to the underlying storage. */
        const T *data() const noexcept { return buf_.data(); }

        /*! @brief Iterator to the first element. */
        T *begin() noexcept { return buf_.data(); }
        /*! @brief Iterator past the last element. */
        T *end() noexcept { return buf_.data() + len_; }
        /*! @brief Const iterator to the first element. */
        const T *begin() const noexcept { return buf_.data(); }
        /*! @brief Const iterator past the last element. */
        const T *end() const noexcept { return buf_.data() + len_; }

        /*! @brief Content equality (same logical length and elements). */
        bool operator==(const InlineVector &o) const noexcept
        {
            if (len_ != o.len_)
            {
                return false;
            }
            for (std::size_t i = 0; i < len_; ++i)
            {
                if (!(buf_[i] == o.buf_[i]))
                {
                    return false;
                }
            }
            return true;
        }

        /*! @brief Negated @ref operator==. */
        bool operator!=(const InlineVector &o) const noexcept { return !(*this == o); }
    };

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
        /**
         * @brief Start offset of the **current installation** (§5.1).
         *
         * Where this installation's writable region begins, so `cursor_ - (buffer_
         * + offset_)` is the number of *message* bytes buffered — as opposed to
         * @ref bytesUsed, which counts from the buffer start because the reserved
         * head is part of the packet a sink is handed. @ref flush uses the former:
         * a freshly installed buffer holding nothing but its own reservation has
         * nothing to drain, and must not be pushed at the sink as an empty packet.
         *
         * The offset is **consumed** by the first flush that returns without an
         * installation, which is why it drops to zero there alongside the cursor.
         */
        size_t offset_ = 0;
        flushCallback flushCallback_; /**< Invoked when the buffer fills; may be empty. */
        size_t seqDepth_ = 0;         /**< Number of currently-open nested sequences (§4.9 @ref MAX_DEPTH). */
        /**
         * @brief The held-back sequence run: ids of the innermost open sequences
         *        whose header has not been written yet (@ref sequenceBeginLazy).
         *
         * Always a contiguous suffix of the open sequences — writing any field
         * commits the whole run at once — and it grows **without a bound of its
         * own**: CORELIB_PLAN §6 ("How deep the hold-back reaches") lets only a
         * heap-free profile stop holding back at some depth and frame eagerly
         * beyond it, at the cost of canonicality. This port can allocate, so it
         * holds back to the full @ref MAX_DEPTH and has no eager fallback path.
         * @ref MAX_DEPTH is what bounds the nesting, hence the run, at 255 ids.
         */
        class PendingRun
        {
            /**
             * Ids up to this depth live inside the stream object; deeper ones
             * spill onto the heap. This is **not** a window on the hold-back —
             * nothing about the emitted bytes changes at the boundary, only
             * where an id is stored — it is the depth up to which the run is
             * free. Sized for the nesting real schemas reach, so the ordinary
             * encode allocates nothing, and a stream that never opens a
             * sequence allocates nothing either.
             */
            static constexpr size_t kInline = 8;
            sofab::id inline_[kInline] = {};
            std::vector<sofab::id> spill_; /**< ids at depth >= kInline, in order */
            size_t n_ = 0;                 /**< total ids held back */

        public:
            /** @return `true` while no header is held back. */
            [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
            /** @return How many headers are held back. */
            [[nodiscard]] size_t size() const noexcept { return n_; }
            /** @return The @p i-th held-back id, outermost first. */
            [[nodiscard]] sofab::id operator[](size_t i) const noexcept
            {
                return i < kInline ? inline_[i] : spill_[i - kInline];
            }
            /**
             * @brief Hold back one more id (the new innermost open sequence).
             *
             * Past `kInline` this grows the spill vector, the one allocation
             * an encode can make. A failed allocation is **reported, not fatal**:
             * the `bad_alloc` is caught here and turned into a `false`, which
             * @ref OStreamImpl::sequenceBeginLazy surfaces as
             * @ref Error::BufferFull. (A build with exceptions disabled has no
             * such option — there `push_back` on a failed allocation terminates,
             * as it does anywhere else in that build.)
             *
             * @return `true` if the id is held back, `false` if it could not be.
             */
            [[nodiscard]] bool push(sofab::id v) noexcept
            {
                if (n_ < kInline) { inline_[n_++] = v; return true; }
#if defined(__cpp_exceptions) && __cpp_exceptions
                try { spill_.push_back(v); }
                catch (...) { return false; }
#else
                spill_.push_back(v);
#endif
                ++n_;
                return true;
            }
            /** Drop the innermost held-back id — its sequence got no content. */
            void pop() noexcept
            {
                if (n_ == 0) return;
                --n_;
                if (n_ >= kInline) spill_.pop_back();
            }
            /** Forget the whole run (it has just been written out). */
            void clear() noexcept { n_ = 0; spill_.clear(); }
        };
        PendingRun pending_;
        /**
         * @brief Sticky first failure of this stream (see @ref ok, @ref error).
         *
         * @ref Error::None while the encode is healthy. Latches
         * @ref Error::BufferFull on an overflow with no sink (and on a hold-back
         * the run could not grow), or @ref Error::InvalidArgument when a buffer
         * installation was rejected (§5.1 — an out-of-range offset, or less than
         * @ref MIN_OUTPUT_BUFFER usable bytes behind a sink). Only the first is
         * kept: once condemned, the stream stays condemned for the reason it was
         * condemned for.
         */
        Error failure_ = Error::None;
        /**
         * @brief Set by @ref initBuffer, cleared right before a flush callback runs.
         *
         * This is how the stream tells the two halves of §5.1's returning-callback
         * contract apart: a sink that returns **without** installing a buffer
         * copied, and the encoder resumes in the same buffer at offset 0; a sink
         * that **took** the buffer installed a replacement, and that installation's
         * own start offset is the cursor. Without it a taking sink's offset would
         * be overwritten the moment the callback returned, so per-packet header
         * room could not be re-armed.
         */
        bool installed_ = false;

        /** Construct an unattached stream; a derived class must call @ref initBuffer. */
        OStreamImpl() noexcept = default;

        /**
         * @brief Point the stream at a buffer and position the write cursor.
         *
         * This is the one place a buffer is handed over, so it is where §5.1's
         * preconditions are checked and where a bad buffer is rejected — never
         * partway through a message. On rejection the stream is left **inert**
         * (cursor at the end, nothing writable) with @ref failure_ latched, so no
         * subsequent write can touch storage that cannot hold it.
         *
         * @param buffer Storage to encode into.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to leave untouched before the cursor.
         */
        void initBuffer(uint8_t *buffer, size_t buflen, size_t offset) noexcept
        {
            installed_ = true;
            /* §5.1: the minimum binds a buffer installed WITH a sink and no other.
             * Without a sink no flush can occur, so nothing can be split and the
             * buffer either holds the message or reports BufferFull — a two-byte
             * message must still encode into a two-byte buffer. */
            if (offset > buflen ||
                (flushCallback_ && buflen - offset < MIN_OUTPUT_BUFFER))
            {
                buffer_ = cursor_ = end_ = buffer;
                offset_ = 0;
                if (failure_ == Error::None) failure_ = Error::InvalidArgument;
                return;
            }
            buffer_ = buffer;
            cursor_ = buffer + offset;
            end_ = buffer + buflen;
            offset_ = offset;
        }

        /**
         * @brief Encode an unsigned value as a base-128 varint.
         * @param out Destination buffer; must hold at least 10 bytes.
         * @param v Value to encode.
         * @return Number of bytes written to @p out (1–10).
         */
        static size_t encodeVarint(uint8_t *out, uint64_t v) noexcept
        {
            /* Continuation-first: every byte but the last is unconditionally
             * tagged, so the loop carries one test (`more to come?`) instead of
             * the two a "shift, then decide whether to tag" body needs. */
            uint8_t *p = out;
            while (v >= 0x80)
            {
                *p++ = static_cast<uint8_t>(static_cast<uint8_t>(v) | 0x80u);
                v >>= 7;
            }
            *p++ = static_cast<uint8_t>(v);
            return static_cast<size_t>(p - out);
        }

        /**
         * @brief Encode a varint into a destination with a full varint window.
         *
         * The caller must guarantee @ref detail::VARINT_MAX_BYTES writable bytes at
         * @p out. That buys a branch-free body: the length comes from the value's
         * bit width in one instruction instead of being discovered a byte at a
         * time, and all ten bytes are then written unconditionally, with only the
         * last one of the *encoded* run untagged. Bytes past the returned length
         * are scratch — the cursor never advances over them, so they are always
         * overwritten by the next value or left outside the message.
         *
         * The single-byte case is peeled off first: it is by far the most common
         * value in real data, and paying ten stores for it would lose more on
         * small-element arrays than the wide case gains.
         *
         * @param out Destination, with at least @ref detail::VARINT_MAX_BYTES bytes.
         * @param v Value to encode.
         * @return Number of meaningful bytes written (1–10).
         */
        static size_t encodeVarintPadded(uint8_t *out, uint64_t v) noexcept
        {
            if (v < 0x80)
            {
                out[0] = static_cast<uint8_t>(v);
                return 1;
            }
            /* The varint length is ceil(bit_width(v) / 7). Spelling the divide as
             * a multiply and a shift is exact for every width 1..64 — check
             * b = 7k and 7k+1 for k = 1..9 and it holds at both sides of every
             * step — and costs a few instructions instead of a division. */
            const size_t bits = 64u - static_cast<size_t>(std::countl_zero(v));
            const size_t n = (bits * 9u + 64u) >> 6;
            /* Bytes 0-7 carry bits 0-55 and go out as one word; bytes 8 and 9
             * carry what is left of a 64-bit value. Everything is tagged as a
             * continuation, then the last byte of the encoded run is untagged. */
            detail::storeLittle64(out, detail::spread7(v) | 0x8080808080808080ull);
            out[8] = static_cast<uint8_t>((static_cast<uint8_t>(v >> 56) & 0x7fu) | 0x80u);
            out[9] = static_cast<uint8_t>(v >> 63);
            out[n - 1] = static_cast<uint8_t>(out[n - 1] & 0x7fu);
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
            /* A condemned stream writes nothing more. Besides keeping a truncated
             * encode a byte-exact prefix of the full one, this is what makes a
             * rejected installation safe: the inert buffer left behind by
             * @ref initBuffer has cursor == end, and without this test the flush
             * path below would fall through to the store and run off it. */
            if (failure_ != Error::None) [[unlikely]] return failure_;
            if (cursor_ == end_)
            {
                if (!flushCallback_)
                {
                    // Sticky, because a caller may issue writes one at a time and
                    // discard each Result — generated serialize() bodies do — and
                    // then nothing would record that the output was cut short.
                    failure_ = Error::BufferFull;
                    return Error::BufferFull;
                }
                /* §5.1, the returning-callback contract. The sink either copied —
                 * it returns without installing anything, and the same buffer is
                 * reused from offset 0 — or it took the buffer and installed a
                 * replacement, whose own start offset is already the cursor and
                 * must not be overwritten here. */
                installed_ = false;
                flushCallback_(std::span<const uint8_t>(buffer_, static_cast<size_t>(cursor_ - buffer_)));
                if (failure_ != Error::None) [[unlikely]] return failure_; /* the sink installed a rejected buffer */
                if (!installed_) { cursor_ = buffer_; offset_ = 0; }        /* the offset is consumed */
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
                /* An empty string or blob passes a null @p data, and memcpy
                 * forbids that even for a zero length (§7.1.4 [mem.req]) — which
                 * UBSan reports on the shared-vector suite. */
                if (len) std::memcpy(cursor_, data, len);
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
         * @brief Write the sequence-end marker (§4.9) — id 0, wire type 7.
         *
         * The **only** header not composed inline by one of the five writers, and
         * deliberately not a general `putHeader(id, type)`: a general one would
         * look like the place to commit a held-back run, and it is not. It is
         * reached from @ref sequenceEnd and @ref sequenceEndKeep only, neither of
         * which may commit here — @ref sequenceEnd only gets here when the run is
         * already empty, and @ref sequenceEndKeep has committed it itself one line
         * earlier. The commit choke point is @ref beforeContent, called by the
         * five inline writers; see the note there.
         *
         * @return @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error putSequenceEnd() noexcept
        {
            return putVarint(headerWord(0, Wire::SequenceEnd));
        }

        /**
         * @brief Write out the held-back sequence headers, outermost first.
         *
         * Runs at most once per non-default sequence, never per field.
         *
         * The run is dropped even when a header write fails partway, and that is
         * deliberate: the only way to fail here is @ref Error::BufferFull, which
         * is unreachable while a flush callback is set and otherwise latches the
         * sticky @ref failure_ with the cursor parked at the buffer end — so every
         * later push fails too and @ref ok is already false. Keeping the leftover
         * ids could therefore only ever emit them into an output that is already
         * condemned. `bufferFullCondemnsTheRun()` in test/test_roundtrip.cpp
         * pins that: a truncated encode stays a byte-exact prefix of the full one
         * and reports `ok() == false`.
         */
        [[nodiscard]] Error commitPending() noexcept
        {
            Error err = Error::None;
            /* Safe to iterate and clear afterwards: putVarint never touches the
             * run (it writes bytes, it does not open or close a sequence). */
            const size_t n = pending_.size();
            for (size_t i = 0; i < n; ++i)
                if ((err = putVarint(headerWord(pending_[i], Wire::SequenceStart))) != Error::None)
                    break;
            pending_.clear();
            return err;
        }

        /**
         * @brief Commit any held-back sequence run before writing content.
         *
         * **This is the choke point** — there is no other. The five writers below
         * (@ref writeScalar, @ref writeFixlen, @ref writeFloatScalar,
         * @ref writeIntArray, @ref writeFloatArray) compose their header inline
         * for speed, so every field written by this library goes through one of
         * them and through this call, and through nothing else: dropping it from
         * any one of the five loses that field's frame, and each of the five has a
         * check in `lazySequenceFraming()` that fails when it does. A
         * @ref sequenceBeginLazy run is framed exactly when the first child field
         * appears (MESSAGE_SPEC §2).
         */
        [[nodiscard]] Error beforeContent() noexcept
        {
            return !pending_.empty() ? commitPending() : Error::None;
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
            if (Error e = beforeContent(); e != Error::None) return e;
            /* Header and value are each at most VARINT_MAX_BYTES, so with room for
             * two of them the field is composed straight into the buffer: no
             * scratch array and no copy out of it. The checked path below still
             * runs at the buffer tail and across every flush boundary. */
            if (static_cast<size_t>(end_ - cursor_) >= 2 * detail::VARINT_MAX_BYTES) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, headerWord(fieldId, type));
                o += encodeVarintPadded(o, value);
                cursor_ = o;
                return Error::None;
            }
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
            if (Error e = beforeContent(); e != Error::None) return e;
            /* As in @ref writeScalar, plus the payload. The room test is split so
             * it cannot overflow on a huge @p len. */
            const size_t room = static_cast<size_t>(end_ - cursor_);
            if (room >= 2 * detail::VARINT_MAX_BYTES && room - 2 * detail::VARINT_MAX_BYTES >= len) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, headerWord(fieldId, Wire::Fixlen));
                o += encodeVarintPadded(o, fixlenWord(len, ft));
                /* An empty string or blob passes a null @p data; memcpy forbids
                 * that even for a zero length (§7.1.4 [mem.req]). */
                if (len) std::memcpy(o, data, len);
                cursor_ = o + len;
                return Error::None;
            }
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
            if (Error e = beforeContent(); e != Error::None) return e;
            auto bits = detail::floatBits(value);
            if (static_cast<size_t>(end_ - cursor_) >= 2 * detail::VARINT_MAX_BYTES + sizeof(F)) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, headerWord(fieldId, Wire::Fixlen));
                o += encodeVarintPadded(o, fixlenWord(sizeof(F), ft));
                for (size_t i = 0; i < sizeof(F); ++i) o[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
                cursor_ = o + sizeof(F);
                return Error::None;
            }
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
            if (Error e = beforeContent(); e != Error::None) return e;
            const uint64_t head = (static_cast<uint64_t>(fieldId) << 3) |
                        static_cast<uint64_t>(isSigned ? Wire::ArraySigned : Wire::ArrayUnsigned);
            if (static_cast<size_t>(end_ - cursor_) >= 2 * detail::VARINT_MAX_BYTES) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, head);
                o += encodeVarintPadded(o, elems.size());
                cursor_ = o;
            }
            else
            {
                uint8_t hdr[20];
                size_t hn = encodeVarint(hdr, head);
                hn += encodeVarint(hdr + hn, elems.size());
                if (Error e = pushBytes(hdr, hn); e != Error::None) return e;
            }

            /* Element loop. The straightforward body — encode into a scratch
             * array, then pushBytes it — costs a bounds check and a variable-length
             * memcpy per element. Instead, ask once how many elements are
             * *guaranteed* to fit (each varint is at most VARINT_MAX_BYTES) and run
             * that many straight into the buffer with no per-element check at all.
             * Only the buffer tail, and every flush boundary, takes the checked
             * path — which is still pushBytes, so flushing behaviour is unchanged. */
            const auto word = [](E v) noexcept -> uint64_t {
                if constexpr (isSigned) return detail::zigzagEncode(static_cast<int64_t>(v));
                else                    return static_cast<uint64_t>(v);
            };
            const size_t n = elems.size();
            for (size_t i = 0; i < n; )
            {
                size_t fit = static_cast<size_t>(end_ - cursor_) / detail::VARINT_MAX_BYTES;
                if (fit == 0) [[unlikely]]
                {
                    uint8_t tmp[detail::VARINT_MAX_BYTES];
                    size_t k = encodeVarint(tmp, word(elems[i]));
                    if (Error e = pushBytes(tmp, k); e != Error::None) return e;
                    ++i;
                    continue;
                }
                if (fit > n - i) fit = n - i;
                uint8_t *out = cursor_;
                for (size_t k = 0; k < fit; ++k, ++i)
                    out += encodeVarintPadded(out, word(elems[i]));
                cursor_ = out;
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
            if (Error e = beforeContent(); e != Error::None) return e;
            /* §4.8: a fixlen array always carries its fixlen_word, even when empty
             * (count == 0), so an empty fp32 and fp64 array stay distinguishable. */
            if (static_cast<size_t>(end_ - cursor_) >= 3 * detail::VARINT_MAX_BYTES) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, headerWord(fieldId, Wire::ArrayFixlen));
                o += encodeVarintPadded(o, elems.size());
                o += encodeVarintPadded(o, fixlenWord(sizeof(F), ft));
                cursor_ = o;
            }
            else
            {
                uint8_t hdr[30];
                size_t hn = encodeVarint(hdr, headerWord(fieldId, Wire::ArrayFixlen));
                hn += encodeVarint(hdr + hn, elems.size());
                hn += encodeVarint(hdr + hn, fixlenWord(sizeof(F), ft));
                if (Error e = pushBytes(hdr, hn); e != Error::None) return e;
            }
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
             * @brief Chain a nested-message field write that is omitted when the
             *        message is all-default (@ref OStreamImpl::writeLazy).
             * @param fieldId Field identifier of the sub-message.
             * @param value Nested message to encode.
             * @return `*this`, for further chaining.
             */
            template <typename T>
            Result writeLazy(sofab::id fieldId, const T &value) noexcept
            {
                if (error_ == Error::None) error_ = os_.writeLazy(fieldId, value).error_;
                return *this;
            }
            /**
             * @brief Chain a frame-keeping close (@ref OStreamImpl::sequenceEndKeep).
             * @return `*this`, for further chaining.
             */
            Result sequenceEndKeep() noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceEndKeep().error_;
                return *this;
            }
            /**
             * @brief Chain the opening of a nested sub-message that is framed only
             *        if it turns out to have content (@ref
             *        OStreamImpl::sequenceBeginLazy).
             * @param fieldId Field identifier of the sub-message.
             * @return `*this`, for further chaining.
             */
            Result sequenceBeginLazy(sofab::id fieldId) noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceBeginLazy(fieldId).error_;
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
            const size_t used = static_cast<size_t>(cursor_ - buffer_);
            /* Only *message* bytes are worth draining. A buffer that was just
             * installed holds nothing but its own reserved head, and handing the
             * sink that head as a packet of its own would invent an empty packet —
             * which the destructor's flush would then do after every taking sink's
             * last handover. */
            if (flushCallback_ && used > offset_)
            {
                /* Same contract as in @ref pushByte: an explicit flush is a flush
                 * like any other, so a sink that takes the buffer here installs a
                 * replacement and that installation's offset stands. */
                installed_ = false;
                flushCallback_(std::span<const uint8_t>(buffer_, used));
                if (!installed_) { cursor_ = buffer_; offset_ = 0; }
            }
            else if (!flushCallback_)
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
         * `serialize()` does. It turns false on an overflow with no flush callback
         * set (and, rarely, when @ref sequenceBeginLazy cannot allocate room to
         * hold a header back), so it is the verdict to check after encoding into a
         * buffer that may be smaller than the message (@ref OStreamView).
         *
         * @return true while no write has overflowed.
         */
        [[nodiscard]] bool ok() const noexcept { return failure_ == Error::None; }

        /**
         * @return The latched first failure: @ref Error::BufferFull once a write
         *         has overflowed, @ref Error::InvalidArgument when a buffer
         *         installation was rejected (§5.1 — an out-of-range offset, or
         *         fewer than @ref MIN_OUTPUT_BUFFER usable bytes behind a sink),
         *         else @ref Error::None.
         */
        [[nodiscard]] Error error() const noexcept { return failure_; }

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
                /* The ELEMENT form: the frame is kept even when the nested message
                 * writes nothing, because element presence carries a wrapper
                 * array's length (MESSAGE_SPEC §5.1). A nested message FIELD, which
                 * may vanish when all-default, is @ref writeLazy. */
                err = sequenceBeginLazy(fieldId).error_;
                if (err == Error::None) err = value.serialize(*this).error_;
                if (err == Error::None) err = sequenceEndKeep().error_;
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
         * @brief Write a nested message **field**, omitting it when it is all-default.
         *
         * Same as @ref write for an @ref OStreamMessage, except it closes with
         * @ref sequenceEnd rather than @ref sequenceEndKeep: the nested `serialize`
         * omits every child that equals its default, so "not one child was written"
         * is exactly "the object equals its declared default" — evaluated per child
         * field, recursively — and the field is then dropped instead of emitted as
         * an empty frame (MESSAGE_SPEC §2).
         *
         * Use it for a `struct`/`union` **field**. Keep plain @ref write for an
         * array **element**: element presence carries a dynamic array's length
         * (§5.1), so an all-default element stays framed.
         *
         * @tparam T A type deriving from @ref OStreamMessage.
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param value Nested message to encode.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        template <typename T>
            requires std::is_base_of_v<OStreamMessage, T>
        Result writeLazy(sofab::id fieldId, const T &value) noexcept
        {
            Error err = sequenceBeginLazy(fieldId).error_;
            if (err == Error::None) err = value.serialize(*this).error_;
            if (err == Error::None) err = sequenceEnd().error_;
            return Result{*this, err};
        }

        /**
         * @brief Write a raw byte blob field.
         *
         * @p size is signed for symmetry with the generated accessors, but §6.2
         * bounds a fixlen payload to `0 .. 2,147,483,647`: a negative length is
         * outside the format and is refused with @ref Error::InvalidArgument
         * (§6.3), emitting nothing, exactly as an unencodable `string` is. The
         * guard has to sit here, before the conversion — `static_cast<size_t>`
         * would turn `-1` into `SIZE_MAX` and the encoder would copy from
         * @p value until the buffer filled, reading past the caller's object.
         *
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param value Pointer to the bytes to copy.
         * @param size Number of bytes to copy; must not be negative.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result write(sofab::id fieldId, const void *value, int32_t size) noexcept
        {
            if (size < 0) [[unlikely]] return Result{*this, Error::InvalidArgument};
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
         * @brief Open a nested sub-message whose header is **held back** until the
         *        sub-message turns out to have content.
         *
         * MESSAGE_SPEC §2 omits a sequence-typed field whose value equals its
         * declared default, and "not one child was written" is exactly that
         * condition — evaluated per child field, recursively, for free. A sequence
         * closed with nothing in it therefore emits **nothing** instead of a
         * two-byte empty frame, and an all-default message becomes the empty byte
         * string.
         *
         * The predicate never touches a byte image of the object, so struct padding
         * cannot influence it and a non-zero nested default is handled by the
         * caller's ordinary per-field test.
         *
         * This is the only way to open a sub-message. How it closes decides whether
         * a contentless one survives: @ref sequenceEnd drops it, @ref sequenceEndKeep
         * forces the frame out.
         *
         * The hold-back has **no depth window**: the pending run grows on demand
         * to whatever nesting @ref MAX_DEPTH allows, so the output is canonical at
         * every legal depth (CORELIB_PLAN §6, "How deep the hold-back reaches" —
         * only a heap-free profile may bound the run and frame eagerly past the
         * bound). Nesting up to @ref PendingRun's inline depth costs no
         * allocation at all; deeper runs spill to the heap. If that allocation
         * fails, the sequence is not opened and the call returns
         * @ref Error::BufferFull with @ref ok turned false — an exceptions-enabled
         * build never terminates over it.
         *
         * @param fieldId Field identifier of the sub-message.
         * @return A @ref Result carrying @ref Error::None on success,
         *         @ref Error::InvalidArgument past @ref MAX_DEPTH or @ref ID_MAX,
         *         @ref Error::BufferFull if the hold-back could not grow.
         */
        Result sequenceBeginLazy(sofab::id fieldId) noexcept
        {
            if (seqDepth_ >= static_cast<size_t>(MAX_DEPTH))
                return Result{*this, Error::InvalidArgument};
            if (fieldId > ID_MAX)
                return Result{*this, Error::InvalidArgument};
            /* The run grows on demand — no window, no eager fallback. The only
             * ceiling is the MAX_DEPTH just checked, so the hold-back is
             * canonical at every legal depth (CORELIB_PLAN §6). Past the run's
             * inline depth that growth allocates; if the allocation fails the
             * sequence is NOT opened and the stream is condemned (sticky
             * @ref failure_), because the caller's matching close would otherwise
             * end an enclosing sequence instead of this one. */
            if (!pending_.push(fieldId))
            {
                if (failure_ == Error::None) failure_ = Error::BufferFull;
                return Result{*this, Error::BufferFull};
            }
            ++seqDepth_;
            return Result{*this, Error::None};
        }
        /**
         * @brief Close the most recently opened sub-message, letting it **vanish**
         *        if it received no content.
         *
         * Use it wherever absence encodes the same value as an empty frame: a
         * `struct`/`union` field, and an array field whose declared `default` is the
         * empty collection (MESSAGE_SPEC §2). Where the frame must be visible, close
         * with @ref sequenceEndKeep instead.
         *
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result sequenceEnd() noexcept
        {
            if (seqDepth_ > 0) --seqDepth_;
            if (!pending_.empty())
            {
                /* The innermost open sequence is the last held-back one: drop it. */
                pending_.pop();
                return Result{*this, Error::None};
            }
            return Result{*this, putSequenceEnd()};
        }

        /**
         * @brief Close the most recently opened sub-message, **keeping** its frame
         *        even when it received no content.
         *
         * Behaves like a write: it first emits any held-back headers — this frame's
         * and every enclosing one's — and then the end marker, so an empty sequence
         * reaches the wire as `begin` + `end`.
         *
         * Required wherever the frame carries information beyond its contents:
         * - a **wrapper-array element** (`struct`/`union`/nested row): element
         *   presence is what carries a dynamic array's length — *highest present id
         *   + 1* (§5.1) — so dropping an all-default element would change the
         *   decoded length, not just the bytes;
         * - an array field already known to **differ from a non-empty declared
         *   `default`**: absence would reconstruct that default, so the empty frame
         *   is the only encoding of "explicitly empty" (§2, §3).
         *
         * The two failure directions are not symmetric, which is why this is the
         * safe choice when in doubt: using it where @ref sequenceEnd would do costs
         * one non-canonical empty frame that a decoder normalizes away, while the
         * reverse silently changes an array's length.
         *
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result sequenceEndKeep() noexcept
        {
            if (seqDepth_ > 0) --seqDepth_;
            if (!pending_.empty())
                if (Error e = commitPending(); e != Error::None)
                    return Result{*this, e};
            return Result{*this, putSequenceEnd()};
        }
    };

    /**
     * @brief Output stream over a caller-supplied buffer whose lifetime is shared.
     *
     * The buffer is adopted from the caller, swappable at runtime via
     * @ref setBuffer, and retrievable with @ref getBuffer so it may be shared with
     * whatever consumes the encoded bytes.
     *
     * @note This stream does **not** allocate the buffer, and no stream in this
     * library does: §5.1 puts every output buffer on the caller's side, so that
     * there is one buffer-ownership model rather than two and a heap-less profile
     * is the plain reading of it rather than a special case. Sizing the storage is
     * the job of the layer that knows the schema — the generated code, which
     * allocates `MAX_SIZE` and installs it without a sink for a bounded schema, or
     * a scratch buffer with an appending sink for an unbounded one. A caller that
     * simply wants a heap buffer writes
     * `sofab::OStream os{std::make_shared<uint8_t[]>(n), n};`.
     */
    class OStream : public OStreamImpl
    {
    protected:
        std::shared_ptr<uint8_t[]> bufferOwner_; /**< Caller storage, kept alive. */
        /** Construct without a buffer; one must be set via @ref setBuffer. */
        OStream() noexcept = default;

    public:
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
         * @brief Install a buffer, mid-stream if need be (§5.1's buffer-set).
         *
         * This is what a flush sink that **takes** the buffer — hands it to a
         * transport, queues it for an asynchronous write, gives it to DMA — must
         * call before returning; returning without calling it means the sink
         * copied, and the encoder reuses the same buffer from offset 0.
         *
         * **The start offset belongs to this installation, not to the buffer**,
         * and it is consumed: passing the *same* buffer again is a new
         * installation like any other, which is how a sink re-arms header room in
         * every flushed packet where a bare return would not.
         *
         * With a sink installed the buffer must leave at least
         * @ref MIN_OUTPUT_BUFFER writable bytes (`buflen - offset`); an undersized
         * one — or an offset past @p buflen — is rejected **here** rather than
         * partway through a message, leaving the stream inert with @ref ok false
         * and @ref error reporting @ref Error::InvalidArgument.
         *
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
     * @brief Output stream over a buffer the caller already owns.
     *
     * Allocates no buffer and copies no payload: encoding writes straight into
     * @p buffer. The counterpart to @ref OStreamInline (buffer inside the object)
     * and @ref OStream (buffer held by a `shared_ptr`) — this is the one for a
     * destination that already exists, such as the `dst` of a generated
     * `encodeTo`. (As with every stream, nesting deeper than the held-back run's
     * inline depth allocates that run — see @ref OStreamImpl::sequenceBeginLazy.)
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

    /**
     * @brief Output stream whose buffer is stored inline (the *buffer* costs no
     *        heap allocation).
     *
     * The encoded bytes never leave the inline array. The one allocation such a
     * stream can still make is the held-back sequence run, and only when the
     * nesting outgrows that run's inline depth (see
     * @ref OStreamImpl::sequenceBeginLazy).
     *
     * @tparam N Buffer capacity in bytes; must be greater than zero.
     * @tparam Offset Number of leading bytes to reserve before the cursor; must be less than @p N.
     */
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
     * @brief The value range a declared integer type admits (MESSAGE_SPEC §7.1).
     *
     * §7/§7.1 turned the declared integer width from a *storage hint* into a
     * **validity bound**: a wire value that does not fit the declared type is
     * `INVALID` alongside `M > N` and `maxlen`, and **MUST NOT** be masked to the
     * width, nor kept. The wire itself carries no width — every integer is a
     * varint read into a 64-bit accumulator — so the width is a **schema** fact
     * and has to reach the decoder the same way `count` and `maxlen` do
     * (@ref IStreamImpl::readArray, @ref IStreamImpl::readString).
     *
     * For a *scalar* a caller can range-check the value once @ref IStreamImpl::read
     * has returned it. An array *element* cannot be checked that way without
     * decoding the whole array into a wider temporary and copying it down —
     * precisely the cost the bulk path exists to avoid — so for arrays the bound
     * rides into the read and is applied where each element is decoded.
     *
     * Default-constructed it is **unarmed**, so an existing call site keeps the
     * behaviour it had.
     *
     * @note The range is inclusive and applied in full: an element below @ref lo
     *       or above @ref hi makes the decode `INVALID`. For an unsigned element
     *       type @ref lo is consulted only when positive, since a varint cannot be
     *       negative.
     */
    struct ElemBound
    {
        int64_t lo = 0;     /**< Smallest admissible value, inclusive. */
        int64_t hi = 0;     /**< Largest admissible value, inclusive. */
        bool armed = false; /**< `false`: no bound — the decode is unchanged. */

        constexpr ElemBound() noexcept = default;

        /** @brief An explicit inclusive range, armed. */
        constexpr ElemBound(int64_t low, int64_t high) noexcept
            : lo(low), hi(high), armed(true) {}

        /**
         * @brief The bound a declared element type @p E implies.
         *
         * **Unarmed for a 64-bit type**, whose range is the accumulator's own and
         * therefore cannot be exceeded — so generated code may hand this in
         * unconditionally and a `u64`/`i64` array pays nothing for it.
         */
        template <typename E>
        static constexpr ElemBound of() noexcept
        {
            if constexpr (sizeof(E) >= sizeof(int64_t)) return ElemBound{};
            else
                return ElemBound{static_cast<int64_t>(std::numeric_limits<E>::min()),
                                 static_cast<int64_t>(std::numeric_limits<E>::max())};
        }
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
        /**
         * @brief The stream's latched terminal verdict, or @ref Error::None while
         *        the stream is still usable (§5.2, §6.3).
         *
         * @ref error_ and @ref limitExceeded_ live for exactly one @ref feed; this
         * outlives it. `INVALID` is terminal — §5.2 answers "can more bytes change
         * it?" with "no — terminal" — and `LimitExceeded` is "a terminal,
         * receiver-local policy rejection" (§6.3). So once either is the answer it
         * stays the answer for every later @ref feed, until @ref reset starts a new
         * message. Without the latch the verdict would depend on where the chunk
         * boundaries fell: feed()'s fast path returns before the offending bytes
         * reach @ref acc_ and the next call starts from a clean slate, while the
         * continuation path re-parses them out of @ref acc_ and keeps failing —
         * exactly the divergence §7.2 item 4 forbids.
         */
        Error terminal_ = Error::None;
        int seqDepth_ = 0;             /**< Current nested-sequence depth during dispatch (§4.9 @ref MAX_DEPTH). */
        size_t skipped_ = 0;           /**< §7.3 type-mismatch skips seen so far (@ref skipped). */
        bool incomplete_ = false;      /**< The field being delivered needs more bytes (§7 INCOMPLETE, not malformed). */
        bool declined_ = false;        /**< The buffered field was already offered and not read: skip it, do not deliver again. */
        long elemBound_ = -1;          /**< Element-index bound of the wrapper sequence being read (§5.1); -1 = none. */
        int elemWire_ = -1;            /**< §7.3 wire type its elements must carry (a @ref Wire as int); -1 = the collector decides the bound itself. */
        int elemFix_ = -1;             /**< §7.3 fixlen subtype for that element type (a @ref Fix as int); -1 = the element type has none. */
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
         * @brief Read one varint, given that a full varint window is in the buffer.
         *
         * The caller must have established that at least @ref detail::VARINT_MAX_BYTES
         * bytes are readable at @p p. That single fact retires both per-byte
         * checks the general path needs: the cursor cannot run past the end
         * inside ten bytes, and the first nine bytes carry at most 63 payload
         * bits, so only the tenth can be overlong (§4.1). With the checks gone
         * the first eight bytes can be taken as one word and unpacked with
         * @ref detail::gather7, leaving only a ninth and tenth byte to guard.
         *
         * @param[in,out] p Cursor; advanced past the varint, including on overflow.
         * @param[out] out Decoded value.
         * @param overflow Set when the varint is wider than 64 bits (INVALID, §6.3).
         * @return `true` on success, `false` on a > 64-bit varint.
         */
        static bool getVarintWindowed(const uint8_t *&p, uint64_t &out, bool *overflow) noexcept
        {
            /* A single byte still wins on its own: it is one load and one compare
             * against the word machinery's load, mask and gather. */
            if (p[0] < 0x80)
            {
                out = p[0];
                ++p;
                return true;
            }
            /* Read the first eight bytes as one word and locate the terminator by
             * its clear continuation bit. Everything past it belongs to whatever
             * follows this varint, so it is masked away before the gather. */
            const uint64_t w = detail::loadLittle64(p);
            const uint64_t term = ~w & 0x8080808080808080ull;
            if (term) [[likely]]
            {
                const unsigned len = (static_cast<unsigned>(std::countr_zero(term)) >> 3) + 1u;
                out = detail::gather7(w & (~uint64_t{0} >> (64u - 8u * len)));
                p += len;
                return true;
            }
            /* Eight continuation bytes: 56 bits are in, and a ninth (and possibly
             * tenth) byte carries the rest. */
            uint64_t v = detail::gather7(w);
            const uint8_t b8 = p[8];
            v |= static_cast<uint64_t>(b8 & 0x7f) << 56;
            if (!(b8 & 0x80))
            {
                p += 9;
                out = v;
                return true;
            }
            /* Tenth byte: 63 bits are already in, so only bit 0 fits. Any higher
             * payload bit — or a continuation into an eleventh byte — is a varint
             * wider than 64 bits, which is INVALID (§4.1/§6.3). The cursor still
             * advances past the offending byte, exactly as the checked loop does,
             * so callers observe the same position either way. */
            const uint8_t b9 = p[9];
            p += 10;
            if ((b9 & 0x7f) > 1 || (b9 & 0x80))
            {
                if (overflow) *overflow = true;
                return false;
            }
            v |= static_cast<uint64_t>(b9) << 63;
            out = v;
            return true;
        }

        /** @brief @ref getVarintWindowed without building the value — see @ref skipVarint. */
        static bool skipVarintWindowed(const uint8_t *&p, bool *overflow) noexcept
        {
            const uint64_t term = ~detail::loadLittle64(p) & 0x8080808080808080ull;
            if (term) [[likely]]
            {
                p += (static_cast<unsigned>(std::countr_zero(term)) >> 3) + 1u;
                return true;
            }
            const uint8_t b8 = p[8];
            if (!(b8 & 0x80))
            {
                p += 9;
                return true;
            }
            const uint8_t b9 = p[9];
            p += 10;
            if ((b9 & 0x7f) > 1 || (b9 & 0x80))
            {
                if (overflow) *overflow = true;
                return false;
            }
            return true;
        }

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
            /* One byte is the overwhelmingly common case away from array
             * payloads — every field header with an id below 16, every short
             * length, every small count and value — so it is tested before
             * anything else and costs a load and a compare. Array element runs do
             * not come through here at all; they drive @ref getVarintWindowed
             * directly under one shared bounds check (see @ref read). */
            if (p < end && *p < 0x80) [[likely]]
            {
                out = *p++;
                return true;
            }
            if (static_cast<size_t>(end - p) >= detail::VARINT_MAX_BYTES)
                return getVarintWindowed(p, out, overflow);

            /* Tail: fewer than VARINT_MAX_BYTES bytes are left, so this loop runs
             * at most nine times and the value it builds carries at most 63 bits.
             * Both §4.1 overlong tests are therefore dead here — the tenth byte
             * that could trip them is by definition not in the buffer — and the
             * only way out other than a terminator byte is running dry, which is
             * a truncated varint (INCOMPLETE, not INVALID, so `overflow` stays
             * untouched). @p overflow is still taken for the fast path above. */
            uint64_t v = 0;
            int shift = 0;
            while (p < end)
            {
                const uint8_t b = *p++;
                v |= static_cast<uint64_t>(b & 0x7f) << shift;
                if (!(b & 0x80))
                {
                    out = v;
                    return true;
                }
                shift += 7;
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
            /* Same three-part shape as @ref getVarint, and for the same reasons. */
            if (p < end && *p < 0x80) [[likely]]
            {
                ++p;
                return true;
            }
            if (static_cast<size_t>(end - p) >= detail::VARINT_MAX_BYTES)
                return skipVarintWindowed(p, overflow);

            while (p < end)
                if (!(*p++ & 0x80)) return true;
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
                const uint8_t *fieldStart = p_;
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
                /* §5.1/§7: an element index at or past the declared count is
                 * INVALID -- but §7.3 decides first. An element header whose wire
                 * type (or, for a fixlen element type, whose fixlen subtype)
                 * contradicts the declared element type MUST be skipped exactly as
                 * an unknown id is skipped, so it never becomes an element and its
                 * id is not an array index the bound could measure. §7.4 states the
                 * same from the other side ("an occurrence skipped under §7.3 is
                 * not an occurrence"), and CORELIB_PLAN §4.8 gives the reason: the
                 * field was never this array's value.
                 *
                 * The bound is therefore applied only to a header that survives the
                 * §7.3 test. For a fixlen element type that is known only after the
                 * fixlen word below, so a message ending BETWEEN the element header
                 * and its fixlen word is INCOMPLETE, not INVALID (§5.2, the analogue
                 * of §4.8's ruling for the fixlen array's two words). From the
                 * fixlen word on the reject is immediate -- it never waits for
                 * payload bytes. Format-level rejects (over-64-bit varint,
                 * ARRAY_MAX, a reserved fixlen subtype, ...) still fire on a skipped
                 * field's own metadata: §7.3 subordinates the SCHEMA bound only.
                 *
                 * A collector that publishes no element type (elemWire_ < 0) keeps
                 * the bound to itself and enforces it in its deserialize. */
                bool skipElem = false;     /* §7.3: not an element of this array */
                bool boundPending = false; /* over-index, subtype not yet known */
                if (elemBound_ >= 0 && elemWire_ >= 0 && type_ != Wire::SequenceEnd &&
                    static_cast<long>(fieldId) >= elemBound_)
                {
                    if (static_cast<int>(type_) != elemWire_) { skipElem = true; ++skipped_; }
                    else if (type_ != Wire::Fixlen) { error_ = true; return; }
                    else boundPending = true; /* decided at the fixlen word */
                }

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
                    /* §4.8 step 1: the FORMAT ceiling fires on the count word
                     * whatever the subtype turns out to be, so an absurd count is
                     * rejected before anything is sized from it. Without this the
                     * `count_ * fixLen_` byte-span below wraps size_t — a count of
                     * 2^62 with 4-byte elements wraps to zero and the array is
                     * skipped as if it were empty, so a message that must be
                     * INVALID decodes COMPLETE. The top-level path
                     * (@ref parseFieldHeader) has always checked this; this
                     * nested one did not. */
                    if (n > ARRAY_MAX) /* §6.2/§7 */
                    {
                        error_ = true;
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

                /* the fixlen word is in: §7.3 first, then the §5.1/§7 bound. */
                if (boundPending)
                {
                    if (elemFix_ >= 0 && static_cast<int>(fixType_) != elemFix_)
                    { skipElem = true; ++skipped_; }
                    else { error_ = true; return; }
                }

                consumed_ = false;
                const uint8_t *payload = p_;
                /* a §7.3-skipped element is never delivered; leaving it unconsumed
                 * runs it through the same skip as an unknown id. */
                if (!skipElem) cb(fieldId, fixLen_, count_);
                /* §7: the bytes ran out INSIDE this field -- it is unfinished, not
                 * declined, and the two must not be confused. The skip below is for
                 * a field the callback did not want; running it here would rewind
                 * to a payload the callback has already parsed *into* and re-read
                 * those bytes under the metadata (@ref type_, @ref count_,
                 * @ref fixLen_) of whatever innermost field the descent left behind
                 * -- so a fixlen array's raw payload gets re-parsed as varints and a
                 * truncation is reported INVALID, decided by payload bytes a
                 * length-consuming reader must never look at (Crucible F-0056,
                 * corelib-cpp#71). The whole top-level field is buffered and
                 * delivered again once the rest arrives, so this level is simply
                 * abandoned where it began. */
                if (incomplete_)
                {
                    p_ = fieldStart;
                    return;
                }

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
                {
                    if (seqDepth_ >= MAX_DEPTH) /* §4.9 */
                    {
                        error_ = true;
                        break;
                    }
                    /* §7.3: this sequence is being skipped, so it is not an element
                     * of the enclosing wrapper -- and the fields INSIDE it are not
                     * that wrapper's elements either. Their ids are child ids of a
                     * field that never became a value, not array indices, so the
                     * element-index bound must not measure them: it is suspended for
                     * the whole subtree and restored after. Without this a child id
                     * at or past the wrapper's `count` would trip the §5.1/§7 reject
                     * from inside a field §7.3 says is not the array's at all
                     * (Crucible F-0051, corelib-cpp#65). The suspension is not
                     * specific to a §7.3-mistyped element: an unknown id skipped
                     * inside the wrapper reaches the same place and is equally not
                     * an element. Format-level rejects (§4.9 depth, over-64-bit
                     * varint, ...) still fire inside the subtree -- §7.3
                     * subordinates the SCHEMA bound only. */
                    const long outerBound = elemBound_;
                    const int outerElemWire = elemWire_, outerElemFix = elemFix_;
                    elemBound_ = -1;
                    elemWire_ = -1;
                    elemFix_ = -1;
                    ++seqDepth_;
                    dispatchLevel([](sofab::id, size_t, size_t) {}, /*stopAtEnd*/ true);
                    --seqDepth_;
                    elemBound_ = outerBound;
                    elemWire_ = outerElemWire;
                    elemFix_ = outerElemFix;
                    break;
                }
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
         *         @ref Error::LimitExceeded is the fourth, receiver-policy outcome
         *         (§6.2.1) — well-formed bytes this receiver refuses to buffer.
         *
         * @note **INVALID and LimitExceeded are terminal.** §5.2 answers "can more
         *       bytes change it?" with "no — terminal" for `INVALID`, and §6.3 calls
         *       `LimitExceeded` "a terminal, receiver-local policy rejection". Once
         *       @ref feed has returned either, the verdict is latched on the
         *       *stream*: every later call returns the same code without parsing a
         *       byte, no further field is delivered, and the input after the fault
         *       is neither buffered nor inspected. @ref reset is the way back — it
         *       is the documented start of a new message anyway. This is what makes
         *       the outcome chunk-independent (§7.2 item 4): the same bytes fed
         *       whole, in odd-sized chunks or one at a time all end on the same
         *       verdict, and garbage prefixed to a valid message can never be
         *       reported as `COMPLETE`.
         *
         * @warning **One message per destination.** Successive @ref feed calls
         *          continue the *same* message — a decoder cannot see a message
         *          boundary, because a message has no framing on the wire and a
         *          zero-byte message is legal (MESSAGE_SPEC §2). Decoding a
         *          *second* message therefore requires a destination reset: call
         *          @ref reset (which for @ref IStreamObject also re-initialises the
         *          wrapped message) or use a fresh stream. MESSAGE_SPEC §5.1 puts
         *          this duty on the decoding side — "supplying a cleanly
         *          initialised destination is the application's responsibility" —
         *          and §2 makes it load-bearing: an all-default field is *absent*
         *          from the bytes, so nothing runs for it and whatever the previous
         *          message left in that member survives.
         */
        Result feed(const uint8_t *buffer, size_t buflen) noexcept
        {
            /* §5.2/§6.3: INVALID and LimitExceeded are TERMINAL. Once the stream has
             * returned one, no later chunk can change it — the bytes were malformed
             * regardless of what follows, or the receiver's policy already refused
             * them. Answering from the latch before parsing is also what keeps the
             * two paths below in agreement: the fast path returns without retaining
             * the offending bytes, so re-parsing would silently "recover" and the
             * outcome would depend on where the chunk boundaries fell (#79). */
            if (terminal_ != Error::None) [[unlikely]] return Result{terminal_, skipped_};

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
                if (limitExceeded_) { terminal_ = Error::LimitExceeded; return Result{terminal_, skipped_}; }
                if (error_) { terminal_ = Error::InvalidMessage; return Result{terminal_, skipped_}; }
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
            if (limitExceeded_) { terminal_ = Error::LimitExceeded; return Result{terminal_, skipped_}; }
            if (error_) { terminal_ = Error::InvalidMessage; return Result{terminal_, skipped_}; }
            topPos_ = static_cast<size_t>(stop - base);
            if (topPos_ == acc_.size()) /* fully drained: COMPLETE */
            {
                acc_.clear(); topPos_ = 0;
                return Result{Error::None, skipped_};
            }
            return Result{Error::Incomplete, skipped_}; /* §7: a partial field is still buffered */
        }

        /**
         * @brief Drop every byte of decoder state, so the next @ref feed starts a
         *        brand-new message.
         *
         * Discards the buffered partial field, the sticky error/limit flags, the
         * latched terminal verdict (§5.2/§6.3 — this is the only way to clear an
         * `INVALID` or `LimitExceeded` stream), the nesting depth and the §7.3
         * @ref skipped counter. The reassembly buffer's
         * *capacity* is deliberately kept: this is the message-loop call, and
         * handing the allocation back only to take it again next message is the one
         * thing it must not cost (`Limits::max_buffered_field` is what bounds that
         * capacity in the first place).
         *
         * **It does not touch the destination** — this class has none; it only
         * dispatches. @ref IStreamObject overrides this to reset the message it
         * owns as well, which is what a caller reusing a decoder wants. A caller
         * driving @ref IStreamInline owns its destinations and must clear them
         * itself.
         *
         * Required between messages: MESSAGE_SPEC §2 omits an all-default field
         * entirely, so an absent field delivers no callback and leaves the previous
         * message's value in place — including a wrapper-array field, whose
         * collector (@ref StringSeq, @ref BlobSeq, @ref MessageSeq) only clears its
         * destination when the wrapper sequence is actually present.
         */
        virtual void reset() noexcept
        {
            acc_.clear();
            topPos_ = 0;
            p_ = end_ = fieldStart_ = nullptr;
            type_ = Wire{};
            fixType_ = Fix{};
            fixLen_ = 0;
            count_ = 0;
            fieldId_ = 0;
            consumed_ = false;
            error_ = false;
            limitExceeded_ = false;
            terminal_ = Error::None;
            incomplete_ = false;
            declined_ = false;
            seqDepth_ = 0;
            skipped_ = 0;
            elemBound_ = -1;
            elemWire_ = -1;
            elemFix_ = -1;
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

        /**
         * @brief Decode the current integer-array payload: @ref count_ varint
         *        elements, the first `sp.size()` of them into @p sp.
         *
         * The tag is already known to match (§7.3) and the cursor sits at the first
         * element. On success the field is @ref consumed_; on failure the stream's
         * @ref error_ / @ref incomplete_ says which.
         *
         * @tparam Bounded Apply @p b, the declared element type's value range
         *         (§7.1): an element outside it makes the decode `INVALID` —
         *         never masked down to @p Elem, never kept. `false` instantiates
         *         the plain decode, which is what @ref read alone can do: it sees
         *         the destination's width, and a destination width is not a
         *         declared width. The two are separate instantiations, so an
         *         unbounded array carries no test for a bound it does not have.
         * @param sp Destination for the leading elements; the rest of @ref count_
         *        is still parsed, to stay framed.
         * @param b The element range, when @p Bounded.
         */
        template <bool Bounded, typename Elem>
        bool readIntElements(std::span<Elem> sp, ElemBound b = {}) noexcept
        {
            const size_t n = sp.size();
            /* §7.1: an element outside the declared range is INVALID. Both
             * alternatives are forbidden by name — masking it to Elem (what a bare
             * static_cast does) and keeping it — so the reject happens before the
             * store, through the same sticky flag as @ref invalidate. */
            auto admits = [b](uint64_t raw) noexcept -> bool {
                if constexpr (std::is_unsigned_v<Elem>)
                    return raw <= static_cast<uint64_t>(b.hi) &&
                           (b.lo <= 0 || raw >= static_cast<uint64_t>(b.lo));
                else
                {
                    const int64_t v = detail::zigzagDecode(raw);
                    return v >= b.lo && v <= b.hi;
                }
            };
            auto store = [&](size_t i, uint64_t raw) noexcept -> bool {
                if constexpr (Bounded)
                    if (!admits(raw)) { error_ = true; return false; }
                if constexpr (std::is_unsigned_v<Elem>) sp[i] = static_cast<Elem>(raw);
                else                                    sp[i] = static_cast<Elem>(detail::zigzagDecode(raw));
                return true;
            };
            /* Two loops rather than one with an `i < n` test inside: the
             * elements that reach the destination and the surplus that is
             * parsed only to stay framed are separate runs, so neither
             * pays for the other's branch. The unbounded surplus skips the value
             * accumulation too — skipVarint applies the identical §4.1
             * length and overflow rules, it just does not build the
             * number it is about to discard. A BOUNDED surplus is decoded
             * instead: §7.1 makes an over-width element INVALID whether or not
             * it had a destination to be stored in. */
            size_t i = 0;
            while (i < n)
            {
                /* One bounds check for a whole run: with R bytes readable,
                 * R / VARINT_MAX_BYTES elements are decodable without any
                 * further test, since each consumes at most that many
                 * bytes. The windowed decoder then carries no per-byte
                 * bookkeeping at all. Only the last few bytes of the
                 * buffer — where a varint may really be cut short — fall
                 * back to the fully checked path. */
                size_t fit = static_cast<size_t>(end_ - p_) / detail::VARINT_MAX_BYTES;
                if (fit == 0) [[unlikely]]
                {
                    uint64_t raw;
                    bool ovf = false;
                    if (!getVarint(p_, end_, raw, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (!store(i, raw)) return false;
                    ++i;
                    continue;
                }
                if (fit > n - i) fit = n - i;
                for (size_t k = 0; k < fit; ++k, ++i)
                {
                    uint64_t raw;
                    bool ovf = false;
                    if (!getVarintWindowed(p_, raw, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    if (!store(i, raw)) return false;
                }
            }
            while (i < count_)
            {
                size_t fit = static_cast<size_t>(end_ - p_) / detail::VARINT_MAX_BYTES;
                bool ovf = false;
                if (fit == 0) [[unlikely]]
                {
                    if constexpr (Bounded)
                    {
                        uint64_t raw;
                        if (!getVarint(p_, end_, raw, &ovf))
                        {
                            (ovf ? error_ : incomplete_) = true;
                            return false;
                        }
                        if (!admits(raw)) { error_ = true; return false; }
                    }
                    else if (!skipVarint(p_, end_, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    ++i;
                    continue;
                }
                if (fit > count_ - i) fit = count_ - i;
                for (size_t k = 0; k < fit; ++k, ++i)
                {
                    if constexpr (Bounded)
                    {
                        uint64_t raw;
                        if (!getVarintWindowed(p_, raw, &ovf))
                        {
                            error_ = true; /* only a > 64-bit varint can fail here */
                            return false;
                        }
                        if (!admits(raw)) { error_ = true; return false; }
                    }
                    else if (!skipVarintWindowed(p_, &ovf))
                    {
                        error_ = true; /* only a > 64-bit varint can fail here */
                        return false;
                    }
                }
            }
            consumed_ = true;
            return true;
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
         * `bool`, `float`, `double`, `std::string`, nested @ref sofab::IStreamMessage
         * objects, and writable contiguous ranges of integers or floats (excess wire
         * elements past the span's capacity are read and discarded). On a malformed
         * or truncated field the stream's error flag is set.
         *
         * Every destination **owns** what it receives. There is deliberately no
         * borrowing destination: a fed chunk is the caller's for the duration of
         * @ref feed only (CORELIB_PLAN §6), so a decoded value must survive the
         * caller reusing, overwriting or freeing that memory the moment `feed`
         * returns.
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
                /* Rejected on purpose, and rejected HERE so the diagnostic names
                 * the replacement instead of surfacing as a span-deduction failure
                 * further down (std::string_view satisfies the contiguous-range
                 * branch below, whose error message would explain nothing).
                 *
                 * This destination used to exist and handed back a view into the
                 * bytes just parsed. CORELIB_PLAN §6 borrows a fed chunk for the
                 * duration of `feed` only: the caller may reuse or free it the
                 * moment `feed` returns, and the decoded message MUST NOT be
                 * affected — so a decoder that binds a `string`/`blob` destination
                 * to chunk memory has to copy out before returning. The spec
                 * exempts a one-shot `decode(buffer)`, where the caller keeps the
                 * whole buffer alive across the call; this port has no such
                 * separate entry point — `feed` is the only way in, and a one-shot
                 * decode is just a single `feed` — so the exemption never applies
                 * and there is no configuration in which the view is safe. */
                static_assert(always_false_v<T>,
                              "IStream::read() does not decode into std::string_view: a fed chunk is "
                              "borrowed only for the duration of feed() (CORELIB_PLAN §6), so a view "
                              "into it dangles as soon as feed() returns. Read into an owning "
                              "destination instead — std::string, std::vector<uint8_t>, "
                              "sofab::FixedString<N>, sofab::FixedBytes<N> — via read(), readString() "
                              "or readBlob().");
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
                 * bound belongs to the stream rather than to the collector, where a
                 * truncated element would outrun it. A collector declares it by
                 * carrying `cap`, and declares the element type the bound is
                 * conditional on (§7.3, see @ref dispatchLevel) by carrying the
                 * static `elemWire` / `elemFix`. Both are picked up by the same
                 * detection; a collector that publishes `cap` without an element
                 * type keeps the bound and applies it in its own deserialize. */
                const long outerBound = elemBound_;
                const int outerElemWire = elemWire_, outerElemFix = elemFix_;
                /* consumed_ tracks the field at THIS level. A successful inner read
                 * would otherwise make a still-open sequence look taken, and its
                 * caller would not expect the re-delivery that follows. */
                const bool outerConsumed = consumed_;
                consumed_ = false;
                if constexpr (requires { value.cap; }) elemBound_ = value.cap;
                else                                   elemBound_ = -1;
                if constexpr (requires { T::elemWire; }) elemWire_ = static_cast<int>(T::elemWire);
                else                                     elemWire_ = -1;
                if constexpr (requires { T::elemFix; })  elemFix_ = static_cast<int>(T::elemFix);
                else                                     elemFix_ = -1;
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
                elemWire_ = outerElemWire;
                elemFix_ = outerElemFix;
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
                    /* No declared element width here: read() is handed a
                     * destination, not a schema. The bounded form is reached
                     * through readArray, which is handed both. */
                    if (!readIntElements<false>(sp.first(n))) return false;
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
                    {
                        /* An empty destination span has a null data(), which
                         * memcpy forbids even for a zero length. */
                        if (n) std::memcpy(sp.data(), p_, n * sizeof(Elem)); /* wire == native */
                    }
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
         * The destination is either a growable one (`std::string`) or a heap-free
         * @ref FixedString, told apart by @ref FixedString::set_len rather than by
         * name, so a caller's own type works too. Both spell the call identically;
         * only where the bytes land differs.
         *
         * @param[out] value Destination for the decoded text.
         * @param bound Declared `maxlen`, or negative when unbounded.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename S>
        bool readString(S &value, long bound = -1) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::String)) return false;      /* §7.3 */
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound))        /* §7.1/§5.2 */
            { error_ = true; return false; }
            if constexpr (requires { value.set_len(size_t{}); S::capacity(); })
            {
                /* A heap-free destination is sized by the schema, so a payload past
                 * its capacity is the §7.1 reject -- never a silent truncation, and
                 * never a resize. Checked before any byte is written, so a rejected
                 * field leaves the destination alone (§7.4). */
                if (fixLen_ > S::capacity()) { error_ = true; return false; }
                if (static_cast<size_t>(end_ - p_) < fixLen_)
                { incomplete_ = true; return false; }
#if SOFAB_STRICT_UTF8
                /* §6.4, same rule as the std::string branch of read(): the subtype
                 * is already known to be Fix::String from tagMatches above, so no
                 * second gate is needed here. */
                if (!detail::utf8Valid(reinterpret_cast<const char *>(p_), fixLen_))
                { error_ = true; return false; }
#endif
                std::memcpy(value.data(), p_, fixLen_);
                value.set_len(fixLen_);
                p_ += fixLen_;
                consumed_ = true;
                return true;
            }
            else return read(value);
        }

        /**
         * @brief Read the current field as a `blob`, or skip it (§7.3).
         *
         * The @ref Fix::Blob counterpart of @ref readString; reads straight into
         * the byte container, with no intermediate `std::string`. Takes a growable
         * `std::vector<uint8_t>` or a heap-free @ref FixedBytes, on the same
         * @ref FixedBytes::set_len test.
         *
         * @param[out] value Destination for the decoded bytes.
         * @param bound Declared `maxlen`, or negative when unbounded.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename B>
        bool readBlob(B &value, long bound = -1) noexcept
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
            if constexpr (requires { value.set_len(size_t{}); B::capacity(); })
            {
                /* §7.1 over-capacity reject, as in readString. */
                if (fixLen_ > B::capacity()) { error_ = true; return false; }
                std::memcpy(value.data(), p_, fixLen_);
                value.set_len(fixLen_);
            }
            else value.assign(p_, p_ + fixLen_);
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
         * 3. **Receiver capacity** → `LimitExceeded`. Two ceilings of one kind: the
         *    configured policy cap (@p dynCap, generator#102), and — for a
         *    destination that publishes one — its own capacity, so a count it
         *    cannot hold is refused instead of silently truncated into it
         *    (MESSAGE_SPEC §3). Deliberately *not* INVALID: the bytes are fine and
         *    the same message decodes into a growable destination. The one
         *    exception is a capacity passed *under* a declared `count`, where the
         *    schema governs and the verdict stays step 2's `INVALID`.
         * 4. **Reset, then fill.** The destination is resized (dynamic container) or
         *    value-initialized (fixed extent) only now, so an occurrence skipped at
         *    step 1 cannot wipe a valid earlier one (§7.4). A fixed array is refilled
         *    from the element default past the wire count, which is what the
         *    trailing-default-run rule expects (§3).
         * 5. **Declared element width** (@p elem, §7.1) → `INVALID`, per element as
         *    it is decoded. The sibling of the `count` bound at step 2: the same
         *    class of schema fact, arriving through the same call, and the reason it
         *    is applied *here* rather than by the caller — an element cannot be
         *    range-checked after the fact without a wide temporary copy of the whole
         *    array. Unarmed by default, so an omitted bound decodes exactly as before.
         *
         * @param[out] dst        Destination range (fixed extent or resizable).
         * @param schemaCount     Declared `count: N`, or negative when unbounded.
         * @param dynCap          Configured `max_dyn_array_count`, or negative.
         * @param elem            Declared element range (@ref ElemBound), e.g.
         *                        `ElemBound::of<std::uint8_t>()` for `items: u8`.
         *                        Ignored for a float element type, which has no
         *                        narrowing to reject: `fp32`/`fp64` are carried at
         *                        their own width on the wire.
         * @return `true` when the array was read; `false` when it was skipped (§7.3)
         *         or rejected, with the outcome already recorded on the stream.
         */
        template <typename T>
        bool readArray(T &dst, long schemaCount = -1, long dynCap = -1,
                       ElemBound elem = {}) noexcept
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
            /* Step 3's second ceiling: the destination's own capacity. MESSAGE_SPEC
             * §3 has a decoder materialize exactly the M elements the wire carries,
             * "the same value on a pre-sized target and on a growable one", so a
             * count a heap-free destination cannot hold is rejected here rather
             * than truncated into it. Nothing downstream can catch it: an
             * InlineVector's resize clamps to N and the fill then binds
             * min(size, count) elements, parsing the surplus only to stay framed —
             * the field is consumed and the decode reports COMPLETE with elements
             * silently missing (issue #81). The same gate readString / readBlob
             * already apply to a FixedString / FixedBytes payload.
             *
             * Which category depends on where the ceiling comes from. With no
             * declared `count` the capacity is the receiver's own technical limit
             * and the message is well-formed — the same bytes decode into a
             * growable destination — so §6.2.1 / §6.3 make it LimitExceeded and
             * forbid folding it into INVALID. With a `count` declared the schema
             * governs (§7.1): a destination narrower than the bound it was
             * generated for cannot hold a legal value either way, and the verdict
             * stays the INVALID that the same field's over-count payload gets at
             * step 2. */
            if constexpr (constexpr long destCap = detail::destCapacity<T>(); destCap >= 0)
            {
                if (count_ > static_cast<size_t>(destCap))
                {
                    (schemaCount >= 0 ? error_ : limitExceeded_) = true;
                    return false;
                }
            }
            if constexpr (requires { dst.resize(count_); }) dst.resize(count_);
            else                                            dst = T{};
            if constexpr (std::is_integral_v<Elem> && !std::is_same_v<Elem, bool>)
            {
                if (elem.armed)
                {
                    /* The tag is already settled above, so the bounded decode is
                     * entered directly rather than through read(). */
                    std::span<Elem> sp{dst};
                    return readIntElements<true>(sp.first(std::min(sp.size(), count_)), elem);
                }
            }
            return read(dst);
        }

        /**
         * @brief Read the current blob field into a caller buffer.
         *
         * Copies up to @p maxlen bytes; the field is consumed regardless of how
         * much fit. Call from inside a deliver callback.
         *
         * Declares a `blob` like @ref readBlob, so it compares the whole tag first
         * (§7.3): any other field — an integer, a sequence, an array, or another
         * fixlen subtype — is left for the decoder to skip, with @p dst untouched.
         * That check is what keeps the cursor sane, because @ref fixLen_ only means
         * "payload length" for a scalar fixlen field: it is 0 for a varint field and
         * the per-ELEMENT width for a fixlen array, so consuming on those tags left
         * the parser standing inside the payload and re-reading it as field headers
         * (issue #80).
         *
         * @param dst Destination buffer.
         * @param maxlen Capacity of @p dst in bytes.
         * @return Number of bytes copied (`min(maxlen, payload length)`), or 0 when
         *         the field was not consumed. Zero is therefore ambiguous on its own —
         *         a zero-length blob also copies nothing, and so does a skipped
         *         mismatch — so a caller that needs to tell them apart consults
         *         @ref consumed().
         */
        size_t read(void *dst, size_t maxlen) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::Blob)) return 0; /* §7.3 */
            size_t n = std::min(maxlen, fixLen_);
            if (static_cast<size_t>(end_ - p_) < fixLen_)
            {
                /* The payload has not fully arrived. That is INCOMPLETE, not an
                 * error: more bytes may complete it, and the field is delivered
                 * again once they do. Setting error_ here made a truncated blob
                 * INVALID and — because the run is then condemned — unrecoverable
                 * even after the remaining bytes arrived. Matches @ref readBlob and
                 * @ref readString, which guard the identical condition. */
                incomplete_ = true;
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
     * @warning **One message per object, unless you @ref reset.** The wrapped
     *          message is populated *in place* across every @ref IStreamImpl::feed,
     *          and MESSAGE_SPEC §2 omits an all-default field from the bytes
     *          altogether — so decoding a second message into the same object
     *          leaves the first message's value in every field the second one does
     *          not carry. That is true of a scalar field (it keeps the old number)
     *          and, since §2, of a wrapper-array field too: its collector clears
     *          the destination only when the wrapper sequence is present, so an
     *          absent array field keeps the previous decode's elements instead of
     *          reading as the empty array §2 requires. Call @ref reset between
     *          messages — it re-initialises the message and the decoder together.
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

        /**
         * @brief Reset the decoder **and** the wrapped message, ready for the next
         *        message.
         *
         * Extends @ref IStreamImpl::reset by value-initialising the owned
         * @p MessageType — the destination side of the contract, which the base
         * class cannot do because it does not own a destination. Its address is
         * unchanged, so the field callback installed at construction stays valid.
         *
         * This is the supported way to decode more than one message with one
         * object; see the class warning for what goes wrong without it.
         */
        void reset() noexcept override
        {
            IStreamImpl::reset();
            std::destroy_at(&data_);
            std::construct_at(&data_);
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
     * and an over-index element that survives that test is rejected by the stream
     * — at the element header for an element type the header settles, and at the
     * fixlen word for a fixlen one, which is where a `string` element's subtype
     * becomes known.
     *
     * @tparam C Destination container. `std::vector<std::string>` for the growable
     *           storage mode, `InlineVector<FixedString<M>, N>` for the heap-free
     *           one; anything with `clear`/`size`/`emplace_back`/`operator[]` whose
     *           `value_type` @ref readString accepts will do. Class template
     *           argument deduction makes the spelling identical either way:
     *           `sofab::StringSeq s{dst, count, maxlen}`.
     */
    template <typename C = std::vector<std::string>>
    struct StringSeq : IStreamMessage
    {
        C &out;
        long cap;
        long emax;

        /**
         * @brief The declared element type, published to the stream (§7.3).
         *
         * The §5.1/§7 over-index reject applies only to an element whose header
         * agrees with this — one that contradicts it is not this array's element at
         * all and is skipped like an unknown id, bound or no bound. The stream
         * picks these up exactly as it picks up @ref cap; they are compile-time
         * constants, so no constructor signature changes.
         */
        static constexpr int elemWire = static_cast<int>(detail::Wire::Fixlen);
        /** @copydoc elemWire */
        static constexpr int elemFix = static_cast<int>(detail::Fix::String);

        /**
         * @param o Destination vector; elements are placed at their index id.
         * @param capacity Schema `count` N, or -1 for an unbounded array. An
         *                 element id at or past N is INVALID (§5.1/§7) once the
         *                 element has passed the §7.3 type test, rejected before
         *                 the container grows — which also bounds an over-index
         *                 allocation.
         * @param elemMax Element `maxlen`, or -1. A longer element is INVALID
         *                (§7.1), never truncated.
         */
        explicit StringSeq(C &o, long capacity = -1, long elemMax = -1) noexcept
            : out(o), cap(capacity), emax(elemMax) {}

        /**
         * §7.4: the sequence IS the array's value, so a repeated field id replaces
         * it whole. @ref IStreamImpl::read calls this once the SequenceStart tag
         * matched, so a §7.3-skipped occurrence cannot wipe a valid earlier one.
         *
         * @warning It runs only when the wrapper sequence is **present**. Since
         *          MESSAGE_SPEC §2 omits an all-default array field, an *absent*
         *          field reaches no collector at all and @p out is left exactly as
         *          it was — so a destination reused across messages must be reset
         *          by its owner first (@ref IStreamObject::reset, §5.1 "supplying a
         *          cleanly initialised destination is the application's
         *          responsibility"). This is not the collector's call to make: it
         *          never learns that the field was absent.
         */
        void prepare() noexcept { out.clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t size, size_t) noexcept override
        {
            /* readString decides both, in the order §5.2 needs and before the
             * payload: the declared subtype (§7.3 -- a mis-typed element is not
             * this array's) and then the element maxlen (§7.1). The over-index
             * reject (§5.1) is enforced by the stream, from `cap` above, and only
             * on an element that passed the same §7.3 test first: at the fixlen
             * word, where the subtype is known. From that word on it is immediate,
             * so a truncated element still cannot outrun it -- only a message
             * ending between the element header and its fixlen word is INCOMPLETE
             * rather than INVALID, since there the subtype, and with it whether the
             * field is an element at all, is not yet decidable. */
            (void)size;
            /* Read into a temporary first, and only then place it: a §7.3-skipped
             * or §7.1-rejected element must leave the destination untouched, and
             * growing the container here would change the array's length (§5.1,
             * highest present id + 1). For the heap-free element types the
             * temporary is a stack object, so this costs no allocation. */
            typename C::value_type s{};
            if (!is.readString(s, emax)) return;
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            out[static_cast<size_t>(id)] = std::move(s);
        }
    };

    /**
     * The `blob` counterpart of @ref StringSeq; see it for the placement and bound
     * rules.
     *
     * @tparam C Destination container — `std::vector<std::vector<uint8_t>>` or
     *           `InlineVector<FixedBytes<M>, N>`; deduced from the constructor.
     */
    template <typename C = std::vector<std::vector<uint8_t>>>
    struct BlobSeq : IStreamMessage
    {
        C &out;
        long cap;
        long emax;

        /** @copydoc StringSeq::elemWire */
        static constexpr int elemWire = static_cast<int>(detail::Wire::Fixlen);
        /** @copydoc StringSeq::elemWire */
        static constexpr int elemFix = static_cast<int>(detail::Fix::Blob);

        /** @copydoc StringSeq::StringSeq */
        explicit BlobSeq(C &o, long capacity = -1, long elemMax = -1) noexcept
            : out(o), cap(capacity), emax(elemMax) {}

        /** @copydoc StringSeq::prepare */
        void prepare() noexcept { out.clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t size, size_t) noexcept override
        {
            (void)size;
            /* Temporary first, then place -- see StringSeq::deserialize. */
            typename C::value_type b{};
            if (!is.readBlob(b, emax)) return; /* §7.3 + §7.1, see StringSeq */
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            out[static_cast<size_t>(id)] = std::move(b);
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

        /**
         * @brief The declared element type, published to the stream (§7.3), as
         *        @ref StringSeq::elemWire is.
         *
         * A struct/union element arrives as a sequence; a nested-array row arrives
         * as the array wire type its element kind selects — the same choice
         * @ref IStreamImpl::read makes when it reads the row. `-1` for a row type
         * neither rule covers, which leaves the bound to the check below.
         */
        static constexpr int elemWire = []() constexpr -> int {
            if constexpr (std::is_base_of_v<IStreamMessage, T>)
                return static_cast<int>(detail::Wire::SequenceStart);
            else if constexpr (requires { typename T::value_type; })
            {
                using Elem = typename T::value_type;
                if constexpr (std::is_integral_v<Elem> && !std::is_same_v<Elem, bool>)
                    return static_cast<int>(std::is_unsigned_v<Elem> ? detail::Wire::ArrayUnsigned
                                                                     : detail::Wire::ArraySigned);
                else if constexpr (std::is_same_v<Elem, float> || std::is_same_v<Elem, double>)
                    return static_cast<int>(detail::Wire::ArrayFixlen);
                else
                    return -1;
            }
            else
                return -1;
        }();

        /** §7.4 replace-whole, and absent ⇒ never called: @copydoc StringSeq::prepare */
        void prepare() noexcept { if (out) out->clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t, size_t count) noexcept override
        {
            /* §5.1/§7 over-index reject, as a backstop for a direct call. Coming
             * through @ref IStreamImpl::read this is unreachable: `cap` is handed
             * to the stream as its element bound and rejected one step earlier,
             * before a truncated element could outrun it -- which is why
             * @ref StringSeq / @ref BlobSeq carry no copy of it. Kept here because
             * `deserialize` is public. §7.3 runs first here too, in the same order
             * the stream uses, so the two entry points cannot disagree: an element
             * whose wire type contradicts @ref elemWire is not an element, and an
             * id that is not an index cannot breach the index bound. */
            if (cap >= 0 && static_cast<size_t>(id) >= static_cast<size_t>(cap))
            {
                if constexpr (elemWire >= 0)
                {
                    if (static_cast<int>(is.wire()) != elemWire) return; /* §7.3 */
                }
                is.invalidate();
                return;
            }
            /* §5.1: the element id IS the array index, so an element is PLACED at
             * `dest[id]`, never appended. The ids may contain gaps -- a decoder
             * MUST accept them and recover a dynamic array's length as *highest
             * present id + 1*, leaving every absent id at the element default.
             * Appending instead would silently SHORTEN the array by the size of
             * the gap: wire `06 0005 07 16 0009 07` (elements at id 0 and id 2,
             * id 1 absent) is the 3-element array `[5, 0, 9]`, not `[5, 9]`.
             * Same placement rule as @ref StringSeq / @ref BlobSeq; the growth is
             * bounded by the `cap` reject above whenever the schema declares a
             * `count`. */
            while (out->size() <= static_cast<size_t>(id)) out->emplace_back();
            T &row = (*out)[id];
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
