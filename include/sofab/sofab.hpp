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
    /**
     * Largest fixlen payload byte-length (`INT32_MAX`). Encoding a longer
     * payload is @ref Error::InvalidArgument; decoding one is
     * @ref Error::InvalidMessage.
     */
    inline constexpr uint32_t FIXLEN_MAX = 0x7fffffffu;
    /**
     * Largest array element count (`INT32_MAX`). Encoding a longer array is
     * @ref Error::InvalidArgument; decoding one is @ref Error::InvalidMessage.
     */
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
        /**
         * An argument was out of range (e.g. a field id above the limit) — and,
         * on the decode side, §6.3's **third refusal tier**: a value that broke
         * neither the schema bound nor a receiver cap but does not fit the
         * **destination the caller handed over** (§6.6.3).
         *
         * "The third is a mistake in the **call**, not a property of the message
         * or of the deployment, and the other two codes each say something untrue
         * about it. `InvalidMessage` would mark a well-formed message malformed —
         * the same bytes decode for a caller who passes a larger destination, and
         * no §5.2.2 condition is present. `LimitExceeded` would promise a limit to
         * raise that was never configured." (§6.3)
         */
        InvalidArgument = 1,
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
         * Decode-only: a receiver-configured cap (§6.2.1) was exceeded on a field
         * the schema leaves unbounded — the field-span budget
         * @ref Limits::max_buffered_field, or the `dynCap` handed to
         * @ref IStreamImpl::readStringCapped, @ref IStreamImpl::readBlobCapped,
         * @ref IStreamImpl::readArrayCapped or a wrapper-array collector.
         *
         * **Policy, not malformation** — deliberately distinct from
         * @ref InvalidMessage, so a differential fuzzer never reads a local
         * buffering limit as a conformance divergence. The bytes are never clamped
         * or truncated; @ref IStreamImpl::feed simply fails with this code.
         *
         * **Never raised for a field the schema bounds** (§6.2.1/§6.3). A declared
         * `maxlen`/`count` is a statement about *validity* and outranks any
         * receiver-side statement about *capacity*: an over-bound claim is
         * @ref InvalidMessage no matter how the cap is configured, so the same
         * bytes cannot come out as a policy refusal on a receiver that happens to
         * buffer less. Only a schema-**unbounded** field reaches this code.
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
        /**
         * A configured receiver-side limit (§6.2.1) was exceeded on a
         * schema-**unbounded** field. The bytes are **well-formed** — the same
         * message decodes under a looser limit — so this is a *policy* rejection
         * and **not** @ref DecodeStatus::Invalid — §6.3 requires an implementation
         * to "keep the two distinguishable to the caller" and forbids reporting
         * it as `InvalidMessage`. Terminal.
         *
         * §6.3 offers two ways to surface it, "either a **fourth decode
         * outcome**, or a terminal failure carrying the `LimitExceeded` code on
         * the error channel"; this port does both, so a caller switching on
         * @ref sofab::IStreamImpl::Result::status alone cannot mistake it for
         * malformation.
         */
        LimitExceeded = 3,
        /**
         * The value broke neither the schema bound nor a receiver cap, but does
         * not fit the **destination this caller handed over** (§6.6.3, §6.3's
         * third refusal tier). A mistake in the call, not in the message: the
         * same bytes decode for a caller who passes a larger destination.
         * Terminal.
         */
        InvalidArgument = 4,
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
            /* The last two rounds are spelled as an add rather than the
             * split-mask-and-recombine of the first. Once the groups are laid
             * out, the two halves of a round *partition* the live bits — `w`
             * equals `(w & keep) + (w & move)` exactly — so
             * `(w & keep) | ((w & move) << k)` is `w + (2^k - 1) * (w & move)`,
             * which drops one mask and one OR per round. That is three x86
             * instructions off every varint the encoder writes; on the shared
             * `u64 array` workload it is ~8 % of the whole encode. The first
             * round cannot use it: it would need `w` pre-masked to 56 bits in a
             * register of its own, which costs back more than it saves. */
            w += 3u * (w & 0x0FFFC0000FFFC000ull);                                   /* 4 x 14 */
            w += (w & 0x3F803F803F803F80ull);                                        /* 8 x 7  */
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

        /**
         * @brief Store the object representation of a 4- or 8-byte value as
         *        little-endian bytes.
         *
         * The wire is little-endian (§4), so on a little-endian host this is one
         * unaligned store; elsewhere the bytes are laid down individually. The
         * hand-rolled shift loop this replaces did not fold into a store on GCC
         * and cost ten instructions per `fp32` field.
         *
         * @tparam U Unsigned type holding the bits (`uint32_t` or `uint64_t`).
         * @param out Destination, with at least `sizeof(U)` writable bytes.
         * @param bits Value to lay down.
         */
        template <std::unsigned_integral U>
        inline void storeLittleBits(uint8_t *out, U bits) noexcept
        {
            if constexpr (std::endian::native == std::endian::little)
                std::memcpy(out, &bits, sizeof bits);
            else
                for (size_t i = 0; i < sizeof(U); ++i)
                    out[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xffu);
        }

        /** @brief Inverse of @ref storeLittleBits. */
        template <std::unsigned_integral U>
        inline U loadLittleBits(const uint8_t *in) noexcept
        {
            U bits;
            if constexpr (std::endian::native == std::endian::little)
                std::memcpy(&bits, in, sizeof bits);
            else
            {
                bits = 0;
                for (size_t i = 0; i < sizeof(U); ++i)
                    bits |= static_cast<U>(in[i]) << (8 * i);
            }
            return bits;
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

        /** @brief The DFA's ACCEPT and REJECT states (see @ref utf8Dfa). */
        inline constexpr uint32_t UTF8_ACCEPT = 0;
        inline constexpr uint32_t UTF8_REJECT = 12;

        /**
         * @brief Step the UTF-8 DFA over @p len bytes and return the state.
         *
         * The resumable half of @ref utf8Valid, for a `string` payload that arrives
         * across more than one `feed`: §6.4.4 requires that "a chunk boundary MUST
         * NOT affect the outcome" and permits exactly this — "A decoder MAY validate
         * incrementally provided it carries validator state across `feed` calls; no
         * assembly buffer is required."
         *
         * @ref UTF8_REJECT is absorbing, which is what lets the verdict be **held
         * back**: §6.4.4's last row makes a byte that can begin no sequence
         * `INVALID` "reported at payload completion, not before", and a decoder
         * "MUST NOT report INVALID mid-payload … that input is INCOMPLETE until the
         * payload ends". The caller therefore steps the state per chunk and consults
         * it once, when the declared length is reached — where a state other than
         * @ref UTF8_ACCEPT covers both a rejected byte and a sequence the payload
         * ended in the middle of.
         *
         * @param state State from the previous piece (@ref UTF8_ACCEPT to start).
         * @param d Bytes of this piece.
         * @param len Their count.
         * @return The state after them.
         */
        [[nodiscard]] constexpr uint32_t utf8Advance(uint32_t state, const uint8_t *d,
                                                     size_t len) noexcept
        {
            for (size_t i = 0; i < len; ++i)
                state = utf8Dfa[256u + state + utf8Dfa[d[i]]];
            return state;
        }

        [[nodiscard]] constexpr bool utf8Valid(const char *data, size_t len) noexcept
        {
            size_t i = 0;
            while (i < len)
            {
                /* The ASCII run, which is where the time goes: payloads in
                 * practice are overwhelmingly ASCII, and a per-byte DFA over such
                 * a run is slower than skipping it. It is walked in two steps,
                 * word-at-a-time while a whole word is left and byte-at-a-time
                 * over what remains, and — this is the point — each step runs to
                 * completion before the other is considered again. Re-testing the
                 * word condition once per byte, as this loop used to, cost a
                 * short ASCII payload four branches per character: validating the
                 * five-byte string of the shared `typical message` workload took
                 * ~62 instructions, a quarter of the whole encode. */
                if (!std::is_constant_evaluated())
                {
                    /* Spelled `i + 8 <= len`, not `len - i >= 8`: the two are
                     * equal for `i <= len`, but only the first lets GCC's
                     * value-range pass see that eight bytes at `data + i` are
                     * in bounds — the subtraction reads as a possible unsigned
                     * wrap and costs a bogus -Warray-bounds under -Werror. */
                    while (i + 8 <= len)
                    {
                        uint64_t w;
                        __builtin_memcpy(&w, data + i, 8);
                        if (w & 0x8080808080808080ULL) break;
                        i += 8;
                    }
                }
                while (i < len && static_cast<unsigned char>(data[i]) < 0x80) ++i;
                if (i >= len) return true;
                /* multi-byte: step the DFA until it is back at ACCEPT (state 0),
                 * then hand control back to the ASCII scan. REJECT is state 12. */
                uint32_t state = 0;
                do
                {
                    state = utf8Dfa[256u + state + utf8Dfa[static_cast<unsigned char>(data[i])]];
                    if (state == 12) return false;
                    ++i;
                } while (state != 0 && i < len);
                /* Input ended inside a multi-byte sequence: truncated is INVALID. */
                if (state != 0) return false;
            }
            return true;
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

        /**
         * @brief Room a decode destination **already holds** — bytes for a payload,
         *        elements for an array (CORELIB_PLAN §6.6.3).
         *
         * §6.6 forbids the codec growing the caller's destination from a wire
         * number: "the codec allocates nothing itself but **requires a growable
         * destination** and grows it, from a wire count or otherwise — violates".
         * §6.6.3 names the shape that replaces it — a value is delivered "into a
         * destination the caller hands back after being told the announced count,
         * **with the codec refusing a destination too short rather than growing
         * it**" — so every typed read needs one number: how much room is there
         * *now*.
         *
         * Two kinds of destination answer it differently, and the difference is
         * not a policy the codec picks but a fact the type publishes:
         *
         * * a **static capacity** (`T::capacity()` — @ref sofab::FixedString,
         *   @ref sofab::FixedBytes, @ref sofab::InlineVector) is storage the caller already owns
         *   in full. Setting a length inside it allocates nothing, so the room is
         *   the capacity;
         * * anything else (`std::string`, `std::vector`, `std::array`, a bound
         *   span) offers exactly `size()`. For a heap-growable container that is
         *   the point: the codec writes into what the caller sized and never
         *   creates room, so no allocator call can reach the wire.
         *
         * @tparam D Destination type.
         * @param d  The destination.
         * @return Room in bytes (payload) or elements (array).
         */
        template <typename D>
        constexpr size_t destRoom(const D &d) noexcept
        {
            if constexpr (requires { D::capacity(); }) return static_cast<size_t>(D::capacity());
            else                                       return d.size();
        }

        /**
         * @brief Publish the decoded length in a destination that already had the
         *        room — the one container operation the codec performs.
         *
         * Never grows: @ref sofab::detail::destRoom has already been compared against the
         * announced size, so `n` is at most what the destination holds and this is
         * a length store (@ref sofab::FixedString::set_len) or a **shrink**. A shrink
         * cannot allocate — `std::string` / `std::vector` never reallocate for a
         * size below the current one — so §6.6's question, "can a sender make this
         * allocation bigger by sending different bytes?", has no allocation to ask
         * it of.
         *
         * A destination of fixed extent that publishes neither (`std::array`, a
         * bound span) keeps its extent; the value's length is the announced count
         * the caller was told.
         *
         * @param d Destination.
         * @param n Decoded length, already known to fit.
         */
        template <typename D>
        constexpr void destSetLen(D &d, size_t n) noexcept
        {
            if constexpr (requires { d.set_len(n); })      d.set_len(n);
            else if constexpr (requires { d.resize(n); }) { if (d.size() != n) d.resize(n); }
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
         * commits the whole run at once — and it is **fixed-size state**
         * (CORELIB_PLAN §6.6.2), sized to its full extent when the stream is
         * constructed: "The pending run is fixed-size state (§6.6.2): at most
         * `MAX_DEPTH` ids, **sized at construction**. An implementation **MUST**
         * hold back to the full `MAX_DEPTH` (§6.2) and is thereby canonical at
         * every depth." (§6.0.1, "How deep the hold-back reaches"). @ref MAX_DEPTH
         * is what bounds the nesting, hence the run; @ref sequenceBeginLazy
         * refuses past it, so the array below can never be outrun.
         */
        class PendingRun
        {
            /**
             * The whole run, inside the stream object. Deliberately **not**
             * value-initialized: only the first @ref n_ entries are ever read, so
             * zeroing a kilobyte on every stream construction would be a cost with
             * no observable effect (§10 — `encode: typical message` is a
             * three-figure instruction count, and this array is larger than it).
             */
            sofab::id ids_[static_cast<size_t>(MAX_DEPTH)];
            size_t n_ = 0; /**< total ids held back */

        public:
            /** @return `true` while no header is held back. */
            [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
            /** @return How many headers are held back. */
            [[nodiscard]] size_t size() const noexcept { return n_; }
            /** @return The @p i-th held-back id, outermost first. */
            [[nodiscard]] sofab::id operator[](size_t i) const noexcept { return ids_[i]; }
            /**
             * @brief Hold back one more id (the new innermost open sequence).
             *
             * Allocates nothing — the run was sized at construction. The `false`
             * return is kept for the caller that already handles it, but it can
             * only be reached by a push past @ref MAX_DEPTH, which
             * @ref OStreamImpl::sequenceBeginLazy rejects one step earlier.
             *
             * @return `true` if the id is held back, `false` if the run is full.
             */
            [[nodiscard]] bool push(sofab::id v) noexcept
            {
                if (n_ >= static_cast<size_t>(MAX_DEPTH)) [[unlikely]] return false;
                ids_[n_++] = v;
                return true;
            }
            /** Drop the innermost held-back id — its sequence got no content. */
            void pop() noexcept
            {
                if (n_ == 0) return;
                --n_;
            }
            /** Forget the whole run (it has just been written out). */
            void clear() noexcept { n_ = 0; }
        };
        PendingRun pending_;
        /**
         * @brief Sticky first failure of this stream (see @ref ok, @ref error).
         *
         * @ref Error::None while the encode is healthy, and one of §6.3's codes
         * from the moment a write could not be honoured: @ref Error::BufferFull
         * on an overflow with no sink (and on a hold-back run that could not
         * grow), @ref Error::InvalidArgument when a buffer installation was
         * rejected (§5.1 — an out-of-range offset, or less than
         * @ref MIN_OUTPUT_BUFFER usable bytes behind a sink) or when a field the
         * caller asked for was refused (a field id past @ref ID_MAX, a
         * sub-message past @ref MAX_DEPTH, a string that is not valid UTF-8
         * under §6.4, an array of more than @ref ARRAY_MAX elements, a payload
         * past @ref FIXLEN_MAX, a negative raw-blob length past §6.2's fixlen
         * bound).
         * Only the first is kept: once condemned, the stream stays
         * condemned for the reason it was condemned for.
         *
         * Everything that turns this non-None goes through @ref latch, so that
         * the set of failures the verdict covers is the set of failures the
         * write path can report — §5.1's "an encoder that could not write what
         * it was asked to write reports it".
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
         * @brief Record a failure in the sticky verdict and hand it straight back.
         *
         * The one place @ref failure_ is written, so every refusal the write path
         * can return also reaches @ref ok / @ref error. That matters because the
         * per-call @ref Result is routinely discarded — a generated `serialize()`
         * issues its writes one at a time and checks nothing — and without this
         * an encode that dropped a field would come back indistinguishable from a
         * complete one (§5.1: an encoder MUST NOT return partial output as if it
         * were complete).
         *
         * First failure wins: a stream stays condemned for the reason it was
         * condemned for, so a later overflow cannot paper over the refusal that
         * actually cost the message a field.
         *
         * @param e The failure to report; must not be @ref Error::None.
         * @return @p e, unchanged, whatever the verdict already held.
         */
        Error latch(Error e) noexcept
        {
            if (failure_ == Error::None) failure_ = e;
            return e;
        }

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
                (void)latch(Error::InvalidArgument);
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
                    return latch(Error::BufferFull);
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
            if (fieldId > ID_MAX) [[unlikely]] return latch(Error::InvalidArgument);
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
         * @param len Payload length in bytes; must not exceed @ref FIXLEN_MAX.
         * @return @ref Error::InvalidArgument if @p fieldId or @p len is too large,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        [[nodiscard]] Error writeFixlen(sofab::id fieldId, Fix ft, const uint8_t *data, size_t len) noexcept
        {
            if (fieldId > ID_MAX) [[unlikely]] return latch(Error::InvalidArgument);
            /* §6.2: @ref FIXLEN_MAX is a format-wide ceiling, so a longer payload
             * has no fixlen word a reader will take — the one this would emit is
             * `INVALID` for every decoder (§5.2), this library's own included.
             * Refused here, next to the id, and refused BEFORE @ref beforeContent
             * so a held-back sequence run stays held back: the field emits
             * nothing at all rather than a header its payload never follows. */
            if (len > static_cast<size_t>(FIXLEN_MAX)) [[unlikely]] return latch(Error::InvalidArgument);
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
            if (fieldId > ID_MAX) [[unlikely]] return latch(Error::InvalidArgument);
            if (Error e = beforeContent(); e != Error::None) return e;
            auto bits = detail::floatBits(value);
            if (static_cast<size_t>(end_ - cursor_) >= 2 * detail::VARINT_MAX_BYTES + sizeof(F)) [[likely]]
            {
                uint8_t *o = cursor_;
                o += encodeVarintPadded(o, headerWord(fieldId, Wire::Fixlen));
                o += encodeVarintPadded(o, fixlenWord(sizeof(F), ft));
                detail::storeLittleBits(o, bits);
                cursor_ = o + sizeof(F);
                return Error::None;
            }
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, headerWord(fieldId, Wire::Fixlen));
            n += encodeVarint(tmp + n, fixlenWord(sizeof(F), ft));
            detail::storeLittleBits(tmp + n, bits);
            n += sizeof(F);
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
         * @param elems Elements to encode, in order; at most @ref ARRAY_MAX of them.
         * @return @ref Error::InvalidArgument if @p fieldId is too large or
         *         @p elems holds more than @ref ARRAY_MAX elements,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::integral E>
        [[nodiscard]] Error writeIntArray(sofab::id fieldId, std::span<const E> elems) noexcept
        {
            constexpr bool isSigned = std::is_signed_v<E>;
            if (fieldId > ID_MAX) [[unlikely]] return latch(Error::InvalidArgument);
            /* §6.2/§4.7: the element count is bounded by @ref ARRAY_MAX, the same
             * format-wide ceiling the decoder enforces on the count word — see
             * @ref writeFixlen for why it is refused here and not emitted. */
            if (elems.size() > static_cast<size_t>(ARRAY_MAX)) [[unlikely]] return latch(Error::InvalidArgument);
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
         * @param elems Elements to encode, in order; at most @ref ARRAY_MAX of them.
         * @return @ref Error::InvalidArgument if @p fieldId is too large or
         *         @p elems holds more than @ref ARRAY_MAX elements,
         *         @ref Error::BufferFull on overflow, otherwise @ref Error::None.
         */
        template <std::floating_point F>
        [[nodiscard]] Error writeFloatArray(sofab::id fieldId, std::span<const F> elems) noexcept
        {
            constexpr Fix ft = (sizeof(F) == 4) ? Fix::Fp32 : Fix::Fp64;
            if (fieldId > ID_MAX) [[unlikely]] return latch(Error::InvalidArgument);
            /* §6.2/§4.8: as in @ref writeIntArray — the count word is bounded by
             * @ref ARRAY_MAX whatever the element type is. */
            if (elems.size() > static_cast<size_t>(ARRAY_MAX)) [[unlikely]] return latch(Error::InvalidArgument);
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
                    uint8_t tmp[sizeof(F)];
                    detail::storeLittleBits(tmp, detail::floatBits(v));
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
         *
         * **With no flush callback installed this is a no-op** and the buffered
         * message stays exactly where it is, reachable through @ref data /
         * @ref bytesUsed. There is nowhere to drain to, so §5.1's "drain any
         * remaining buffered bytes" has nothing to do, and the cursor only
         * returns to offset 0 after a *handover* the callback returned from —
         * which never happened. Rewinding here would discard the encode and drop
         * the installation's reserved head, so the next write would overwrite
         * both.
         *
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
         * `serialize()` does — so it is **the** verdict to check after encoding:
         * §5.1 forbids handing on partial output as if it were complete, and this
         * is what tells the two apart.
         *
         * It turns false whenever a field the caller asked for did not reach the
         * output: an overflow with no flush callback set (@ref OStreamView with a
         * buffer smaller than the message), a rejected buffer installation, a
         * field id past @ref ID_MAX, a sub-message past @ref MAX_DEPTH, a string
         * that is not valid UTF-8 under §6.4, an array of more than
         * @ref ARRAY_MAX elements or a payload past @ref FIXLEN_MAX, a negative
         * raw-blob length, and — rarely — a
         * @ref sequenceBeginLazy that cannot allocate room to hold a header back.
         *
         * @return true while every write this stream was given has been honoured.
         */
        [[nodiscard]] bool ok() const noexcept { return failure_ == Error::None; }

        /**
         * @brief The code behind @ref ok, latched first-failure-wins.
         *
         * @return @ref Error::BufferFull once a write has overflowed with no sink
         *         (or a hold-back run could not grow), @ref Error::InvalidArgument
         *         when a buffer installation was rejected (§5.1 — an out-of-range
         *         offset, or fewer than @ref MIN_OUTPUT_BUFFER usable bytes behind
         *         a sink) or a field was refused as unencodable (§6.2's
         *         @ref ID_MAX, @ref ARRAY_MAX, @ref FIXLEN_MAX and
         *         @ref MAX_DEPTH, §6.4's UTF-8 rule), else @ref Error::None.
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
                    err = latch(Error::InvalidArgument);
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
         * Like every other refusal it also reaches @ref ok / @ref error: the
         * field the caller asked for is not on the wire, and §5.1 does not let a
         * stream missing a field report success (@ref latch).
         *
         * @param fieldId Field identifier; must not exceed @ref ID_MAX.
         * @param value Pointer to the bytes to copy.
         * @param size Number of bytes to copy; must not be negative.
         * @return A @ref Result carrying @ref Error::None on success, or the error encountered.
         */
        Result write(sofab::id fieldId, const void *value, int32_t size) noexcept
        {
            if (size < 0) [[unlikely]] return Result{*this, latch(Error::InvalidArgument)};
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
         * The hold-back has **no depth window**: the pending run is sized at
         * construction to the full @ref MAX_DEPTH, so the output is canonical at
         * every legal depth and no nesting depth allocates (CORELIB_PLAN §6.0.1,
         * "How deep the hold-back reaches" — only a constrained profile may bound
         * the run below @ref MAX_DEPTH and frame eagerly past the bound, and this
         * port takes no such allowance).
         *
         * @param fieldId Field identifier of the sub-message.
         * @return A @ref Result carrying @ref Error::None on success,
         *         @ref Error::InvalidArgument past @ref MAX_DEPTH or @ref ID_MAX.
         */
        Result sequenceBeginLazy(sofab::id fieldId) noexcept
        {
            if (seqDepth_ >= static_cast<size_t>(MAX_DEPTH)) [[unlikely]]
                return Result{*this, latch(Error::InvalidArgument)};
            if (fieldId > ID_MAX) [[unlikely]]
                return Result{*this, latch(Error::InvalidArgument)};
            /* The run is MAX_DEPTH ids wide and was sized at construction — no
             * window, no eager fallback, no allocator call (CORELIB_PLAN §6.0.1 /
             * §6.6.2). The MAX_DEPTH check above is what keeps it in range, so the
             * push cannot fail; the branch stays because a sequence that was NOT
             * opened must condemn the stream (sticky @ref failure_), the caller's
             * matching close would otherwise end an enclosing sequence instead of
             * this one. */
            if (!pending_.push(fieldId)) [[unlikely]]
                return Result{*this, latch(Error::InvalidArgument)};
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
     * `encodeTo`, or a DMA / transport region a **taking** sink hands on
     * (§5.1.5). Like every stream it allocates nothing at all after
     * construction.
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

        /**
         * @brief Install a different caller buffer, mid-stream.
         *
         * §5.1.1 lists "allow a new output buffer to be installed mid-stream"
         * among a corelib's required capabilities, and §5.1.5 puts the duty on
         * the **sink**: "A sink that takes the buffer MUST install a replacement
         * before returning." This is the call it makes. Without it a taking sink
         * could only be written against @ref OStream — the `shared_ptr` flavour —
         * which is the wrong one for the case a taking sink exists for: memory
         * the caller already owns and hands on (A2-0020).
         *
         * The stream keeps no reference to the buffer it gives up. The new one
         * must outlive the stream, and the same @ref MIN_OUTPUT_BUFFER floor
         * applies as at construction: behind a sink, `buflen - offset` short of
         * it is refused with @ref Error::InvalidArgument rather than partway
         * through a later field (§5.1.4).
         *
         * @param buffer New destination; must outlive this stream.
         * @param buflen Capacity of @p buffer in bytes.
         * @param offset Number of leading bytes to leave untouched before the cursor.
         */
        void setBuffer(uint8_t *buffer, size_t buflen, size_t offset = 0) noexcept
        {
            initBuffer(buffer, buflen, offset);
        }
    };

    /**
     * @brief Output stream whose buffer is stored inline (the *buffer* costs no
     *        heap allocation).
     *
     * The encoded bytes never leave the inline array, and the stream allocates
     * nothing at all — the held-back sequence run is fixed @ref MAX_DEPTH state
     * sized at construction (§6.6.2).
     *
     * Because the buffer **is** the object, this flavour admits only a
     * **copying** sink: there is nothing to hand over, so a sink must copy the
     * span it is given and return without installing anything (§5.1.5's other
     * half). A taking sink wants @ref OStreamView (caller memory, with
     * @ref OStreamView::setBuffer) or @ref OStream (`shared_ptr` memory).
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

        /**
         * @brief Reach the wrapped message, so its fields are `obj->field`.
         *
         * `operator->` is re-applied to what it returns until a pointer comes
         * back, so this has to hand out a *pointer* to the message: returning a
         * reference to a type that has no `operator->` of its own is not a
         * member access at all, it is a compile error at every use.
         *
         * @return Pointer to the wrapped message; never null.
         */
        MessageType *operator->() noexcept { return &message_; }
        /** @copydoc operator->() */
        const MessageType *operator->() const noexcept { return &message_; }

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
     * @brief The one stream-scoped receiver cap: how many bytes a single top-level
     *        field may span.
     *
     * @par This IS a §6.2.1 receiver cap, and is treated as one
     * It is configured by the **deployment**, not by the schema; it binds only a
     * field the schema leaves unbounded; and its breach is a **policy** rejection
     * on bytes that are perfectly well-formed — the same message decodes under a
     * looser number. That is precisely §6.3's `LimitExceeded`, and every rule
     * §6.2.1 states about a receiver limit therefore binds this one:
     *
     * * **the codec holds no limit of its own.** The number arrives as a
     *   constructor argument, is the caller's, and is used for this one decode's
     *   comparisons — §6.2.1's *"supplied per call **or per decode**, used for that
     *   one comparison"*. An @ref IStreamObject / @ref IStreamInline **is** one
     *   decode;
     * * **there is no default.** @ref Limits cannot be default-constructed and the
     *   stream constructors do not default it, so a stream cannot come into
     *   existence without the deployment having stated a number (§6.2.1: a codec
     *   "**MUST NOT** supply a default for one it was not given");
     * * **there is no unlimited mode.** No value of this member is special-cased,
     *   and no sentinel switches the check off. A caller that states `SIZE_MAX`
     *   has stated the platform's own ceiling — the check still runs and simply
     *   never fires — which is a number the *caller* chose, not a mode the codec
     *   offers. §6.2 ceilings are never reported through @ref Error::LimitExceeded
     *   in this library.
     *
     * @par The three `max_dyn_*` caps are not here
     * `max_dyn_array_count`, `max_dyn_string_len` and `max_dyn_blob_len` are
     * **not** members of this struct and never were the stream's to hold. Each is
     * a **parameter** on the call that reads the field it bounds — the `dynCap` of
     * @ref IStreamImpl::readStringCapped, @ref IStreamImpl::readBlobCapped,
     * @ref IStreamImpl::readArrayCapped and of a wrapper-array collector. They are
     * per **field**, so they belong on the read; this one is per **stream**,
     * because a field's span accrues across chunks and across a sequence's
     * children, where no single read exists to carry it.
     */
    struct Limits
    {
        /**
         * @brief Cap on how many bytes a single top-level field may span, in bytes.
         *
         * A receiver policy on the **size of a field**, not on a buffer: the
         * decoder holds no buffer a field could grow (§6.6). It is what bounds the
         * one shape the three per-read `max_dyn_*` caps cannot see — a *sequence*
         * whose bulk accrues field by field, which no single count or length word
         * announces.
         *
         * Checked the moment the size becomes known -- at the header for a fixlen
         * or fixlen-array payload, as it accrues for a sequence -- so an oversized
         * claim fails before its payload is buffered, and even if that payload
         * never arrives.
         *
         * It binds only a field the schema leaves **unbounded**: a declared
         * `maxlen`/`count` decides first, and its violation is
         * @ref Error::InvalidMessage rather than @ref Error::LimitExceeded
         * (§6.2.1/§6.3). An over-cap field is therefore still handed to the deliver
         * callback -- the only place that bound is known -- but with its payload
         * withheld, so it can be judged without being materialised.
         *
         * Bytes are never clamped or truncated; the `feed()` simply fails.
         */
        size_t max_buffered_field;

        /**
         * @brief State the cap. There is no default and no default constructor:
         *        §6.2.1 leaves this library no number to invent.
         *
         * @param maxBufferedField The deployment's byte budget for one top-level
         *        field. Generated code passes `SOFAB_MAX_DYN_BUFFERED_FIELD`; a
         *        hand-written caller states its own. `SIZE_MAX` is a legal choice
         *        and means "this receiver's budget is the platform's ceiling" — a
         *        number the caller stated, not a mode this library offers.
         */
        constexpr explicit Limits(size_t maxBufferedField) noexcept
            : max_buffered_field(maxBufferedField) {}
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
             * @return The outcome as a @ref DecodeStatus, **one value per
             *         category**. The three §5.2 wire outcomes map to
             *         @ref DecodeStatus::Complete / @ref DecodeStatus::Incomplete /
             *         @ref DecodeStatus::Invalid; the two non-wire refusals keep
             *         values of their own — @ref DecodeStatus::LimitExceeded for a
             *         receiver policy cap (§6.2.1) and
             *         @ref DecodeStatus::InvalidArgument for a destination too
             *         short for a well-formed value (§6.6.3).
             *
             * @note These used to fold into @ref DecodeStatus::Invalid "as a coarse
             *       fallback", which §6.2.1 forbids outright — "An implementation
             *       **MUST NOT** report it as `InvalidMessage` and **MUST NOT**
             *       fold it into the `INVALID` outcome" — and `status()` is the
             *       accessor a caller writing `switch (r.status())` reaches for
             *       first (A2-0024).
             */
            [[nodiscard]] DecodeStatus status() const noexcept
            {
                switch (error_)
                {
                    case Error::None:           return DecodeStatus::Complete;
                    case Error::Incomplete:     return DecodeStatus::Incomplete;
                    case Error::LimitExceeded:  return DecodeStatus::LimitExceeded;
                    case Error::InvalidArgument:return DecodeStatus::InvalidArgument;
                    default:                    return DecodeStatus::Invalid;
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
            /** @return `true` if a receiver-side limit (§6.2.1) was exceeded. Distinct from @ref invalid. */
            [[nodiscard]] bool limitExceeded() const noexcept { return error_ == Error::LimitExceeded; }
            /**
             * @return `true` if a well-formed value did not fit the destination
             *         the caller handed over (§6.3's third tier, §6.6.3). Distinct
             *         from both @ref invalid and @ref limitExceeded.
             */
            [[nodiscard]] bool invalidArgument() const noexcept { return error_ == Error::InvalidArgument; }
            /** @return The status code (@ref Error::None, @ref Error::Incomplete, @ref Error::InvalidMessage, @ref Error::LimitExceeded or @ref Error::InvalidArgument). */
            [[nodiscard]] Error code() const noexcept { return error_; }
            /** @return `true` if the status code equals @p e. */
            bool operator==(Error e) const noexcept { return error_ == e; }
            /** @return `true` if the status code differs from @p e. */
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

    protected:
        /**
         * @brief Widest prefix of a wire item that can outlive a chunk boundary,
         *        derived from this format's own constants (§6.6).
         *
         * The parse never backs up over a payload — a `string`, a `blob` or an
         * array element run is written into the caller's destination piece by piece
         * as the pieces arrive (§6.6.2) — so what a chunk boundary can cut in half
         * is only ever a **small item**: a field header, the words between it and
         * its payload, a lone varint element, or a fixed-width scalar. The widest
         * of those is an `ARRAY_FIXLEN` field's prefix: the header word, the
         * element count and a `fixlen_word`, three varints of at most
         * @ref detail::VARINT_MAX_BYTES each.
         *
         * That is what makes @ref carry_ **bounded working state** in §6.6's sense
         * rather than an accumulator: a constant of the specification caps it, no
         * wire number can enlarge it, and it is sized when the decoder is
         * constructed.
         */
        static constexpr size_t CARRY_CAP = 3 * detail::VARINT_MAX_BYTES;

        /**
         * @brief The prefix of one cut item, carried to the next @ref feed.
         *
         * This is what replaced the private reassembly accumulator §6.6.2 forbids:
         * "A payload split across fed chunks has to be joined somewhere. That
         * somewhere is storage the caller supplied — the codec copies each piece
         * into it as the piece arrives … A codec **MUST NOT** grow a private
         * accumulator instead." A fixed array of @ref CARRY_CAP bytes cannot be
         * grown, and a sender cannot make it larger by sending different bytes,
         * which is §6.6's whole test.
         */
        uint8_t carry_[CARRY_CAP];
        size_t carryLen_ = 0; /**< Live bytes of @ref carry_. */

        /**
         * @brief Depth at which the parse is suspended, or -1 when nothing is.
         *
         * The decoder resumes rather than re-parses, so a field that straddles a
         * chunk boundary is never delivered from the beginning again. Three pieces
         * of fixed-size state describe where it stopped: this depth,
         * @ref pathId_ / @ref pathSkip_ for the sequences that are open above it,
         * and @ref pend_ / @ref pendDone_ for the field itself.
         */
        int suspendDepth_ = -1;
        /** @brief Re-descending @ref pathId_ to reach @ref suspendDepth_. */
        bool replay_ = false;
        /**
         * @brief Id of the sequence field entered at each open level (§4.9).
         *
         * A nested sequence cannot be resumed by remembering a byte offset: the
         * handler chain that was descending into it lives on the C++ stack, which
         * a `feed` returning unwinds. What survives is the **path** — one field id
         * per open level — and replaying it re-enters exactly the handlers that
         * were open, because each level's `deserialize` dispatches on the id. At
         * most @ref MAX_DEPTH ids, sized at construction (§6.6).
         */
        sofab::id pathId_[MAX_DEPTH + 1];
        /** @brief That level was entered by @ref skipPayload, not by a handler. */
        bool pathSkip_[MAX_DEPTH + 1];
        /** @brief A field at @ref suspendDepth_ is half-delivered. */
        bool pend_ = false;
        /**
         * @brief That field's header, kept apart from the live @ref type_ /
         *        @ref fixLen_ / @ref count_.
         *
         * The live metadata belongs to whatever field the parser is looking at
         * *now*, and re-descending @ref pathId_ overwrites it with each sequence
         * header it replays. The suspended field's own header therefore has to be
         * kept separately and put back once its level is reached again — it is the
         * header the read will be continued under, and re-parsing it is exactly
         * what there are no bytes for.
         */
        sofab::id pendId_ = 0;
        Wire pendType_{};   /**< @copydoc pendId_ */
        Fix pendFix_{};     /**< @copydoc pendId_ */
        size_t pendLen_ = 0;   /**< @copydoc pendId_ */
        size_t pendCount_ = 0; /**< @copydoc pendId_ */
        /** @brief ...and it was declined, so @ref skipPayload resumes it rather than the handler. */
        bool pendSkip_ = false;
        /**
         * @brief Progress through the pending field: payload bytes already written
         *        into the caller's destination, or array elements already decoded.
         */
        size_t pendDone_ = 0;
        /** @brief Incremental UTF-8 DFA state for the `string` payload in progress (§6.4.4). */
        uint32_t utf8State_ = 0;
        /** @brief Bytes of the current top-level field consumed in earlier chunks (#26). */
        size_t spanCarry_ = 0;
        /** @brief Where that field's span continues in the window being parsed (#26). */
        const uint8_t *spanBase_ = nullptr;

        /* cursor + current-field metadata, valid during a deliver callback */
        const uint8_t *p_ = nullptr;   /**< Read cursor. */
        const uint8_t *end_ = nullptr; /**< One past the last readable byte. */
        Wire type_{};          /**< Wire type of the field being delivered. */
        Fix fixType_{};        /**< Sub-type of the current fixlen field. */
        size_t fixLen_ = 0;            /**< Payload length (fixlen) or element size (fixlen array), in bytes. */
        size_t count_ = 0;             /**< Element count of the current array field. */
        bool consumed_ = false;        /**< Set once the callback has read the current field's value. */
        /**
         * @brief Sticky "this feed was refused" flag, and the **only** one the
         *        parse loops test.
         *
         * Raised by every refusal, whichever of §6.3's three tiers it belongs to:
         * @ref invalidate raises it alone, @ref exceedLimit and
         * @ref rejectDestination raise it alongside their own category flag. That
         * keeps the per-field test in @ref dispatchLevel and @ref parseTopLevel a
         * single load — the categories are read once, by @ref feed, when the
         * parse has already stopped.
         */
        bool error_ = false;
        bool limitExceeded_ = false;   /**< Category: a receiver-side limit was crossed (§6.2.1). Implies @ref error_. */
        /**
         * @brief Category: a well-formed value did not fit the destination the
         *        caller handed over (§6.3's third refusal tier, §6.6.3). Implies
         *        @ref error_.
         *
         * Lives for exactly one @ref feed, like @ref error_ and
         * @ref limitExceeded_, and is latched into @ref terminal_ as
         * @ref Error::InvalidArgument by the feed that saw it.
         */
        bool argError_ = false;
        /**
         * @brief The stream's latched terminal verdict, or @ref Error::None while
         *        the stream is still usable (§5.2, §6.3).
         *
         * @ref error_, @ref limitExceeded_ and @ref argError_ live for exactly one
         * @ref feed; this outlives it. `INVALID` is terminal — §5.2 answers "can
         * more bytes change it?" with "no — terminal" — and `LimitExceeded` is "a
         * terminal, receiver-local policy rejection" (§6.3). `InvalidArgument`
         * (§6.3's third tier) is terminal for the same reason the other two are:
         * the field was refused rather than consumed, and letting a later chunk
         * "recover" it would make the verdict depend on where the chunk boundaries
         * fell. So once any of the three is the answer it stays the answer for
         * every later @ref feed, until @ref reset starts a new message. Without the
         * latch the verdict would depend on where the chunk boundaries fell: the
         * offending bytes are consumed and gone, so the next call would start from
         * a clean slate and resynchronize on whatever follows the fault — exactly
         * the divergence §7.2 item 4 forbids.
         */
        Error terminal_ = Error::None;
        int seqDepth_ = 0;             /**< Current nested-sequence depth during dispatch (§4.9 @ref MAX_DEPTH). */
        size_t skipped_ = 0;           /**< §7.3 type-mismatch skips seen so far (@ref skipped). */
        bool incomplete_ = false;      /**< The field being delivered needs more bytes (§7 INCOMPLETE, not malformed). */
        /**
         * @brief The tightest armed element-index bound of the wrapper sequence
         *        being read (§5.1); -1 = none.
         *
         * A fast gate only — it answers "is any bound crossed?" in one compare.
         * Which of §6.3's three tiers a crossing belongs to is decided by
         * @ref rejectElementIndex from the three bounds below, and only once a
         * crossing has actually happened.
         */
        long elemBound_ = -1;
        long elemSchema_ = -1; /**< Schema `count` N: an id at or past it is `InvalidMessage` (§7.1). */
        long elemDyn_ = -1;    /**< Configured `max_dyn_array_count`: `LimitExceeded` (§6.2.1). */
        long elemDest_ = -1;   /**< The destination container's own capacity: `InvalidArgument` (§6.6.3). */
        int elemWire_ = -1;            /**< §7.3 wire type its elements must carry (a @ref Wire as int); -1 = the collector decides the bound itself. */
        int elemFix_ = -1;             /**< §7.3 fixlen subtype for that element type (a @ref Fix as int); -1 = the element type has none. */
        sofab::id fieldId_ = 0;        /**< Id of the field being delivered. */

        /**
         * @brief The receiver's byte budget for one top-level field
         *        (@ref Limits::max_buffered_field), as this decode was told it.
         *
         * Deliberately **uninitialised here**: there is no value this library may
         * pick (§6.2.1), so the only way to reach it is through the constructor
         * below, which every stream has to go through.
         */
        size_t maxBufferedField_;

        std::function<void(sofab::id, size_t, size_t)> topCallback_; /**< Delivers each top-level field. */

        /**
         * @brief Construct with the receiver's @ref Limits; a derived class installs
         *        @ref topCallback_.
         *
         * There is no default constructor. §6.2.1 leaves this codec no number to
         * invent, so a stream that was told none cannot be built: the omission is a
         * compile error rather than a decode that turns out to have been bounded by
         * nothing.
         */
        explicit IStreamImpl(Limits limits) noexcept
            : maxBufferedField_(limits.max_buffered_field)
        {}

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
            /* Read the first eight bytes as one word and locate the terminator by
             * its clear continuation bit. Everything past it belongs to whatever
             * follows this varint, so it is masked away before the gather. */
            const uint64_t w = detail::loadLittle64(p);
            /* A single byte still wins on its own: one test against the gather's
             * dozen ALU ops. It is taken off the word already loaded rather than
             * from a second byte-sized load of p[0] — the load is on the critical
             * path either way, and one of the two was pure overhead. */
            if (!(w & 0x80u))
            {
                out = w & 0xffu;
                ++p;
                return true;
            }
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
            /* `b9 > 1` is exactly "a payload bit above bit 0, or a continuation
             * into an eleventh byte": the legal tenth bytes are 0x00 and 0x01 and
             * nothing else, so the two tests fold into one compare. */
            if (b9 > 1)
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
            if (b9 > 1) /* see @ref getVarintWindowed */
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
            for (const uint8_t *q = p; q < end; )
            {
                const uint8_t b = *q++;
                v |= static_cast<uint64_t>(b & 0x7f) << shift;
                if (!(b & 0x80))
                {
                    p = q;
                    out = v;
                    return true;
                }
                shift += 7;
            }
            /* Truncated: the cursor does NOT move. A varint cut by a chunk
             * boundary is bounded working state (§6.6.2) that rides in the carry
             * and is read again from its first byte, so every caller wants it left
             * where it started rather than half-consumed. */
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

            for (const uint8_t *q = p; q < end; )
                if (!(*q++ & 0x80)) { p = q; return true; }
            return false; /* truncated: cursor unmoved, see @ref getVarint */
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
         * @brief Load a little-endian float or double from raw bytes.
         * @tparam F Floating-point type to read (`float` or `double`).
         * @param p Pointer to at least `sizeof(F)` readable bytes.
         * @return The decoded value.
         */
        template <std::floating_point F>
        static F loadFloat(const uint8_t *p) noexcept
        {
            using Bits = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
            return detail::bitsFloat<F>(detail::loadLittleBits<Bits>(p));
        }

        /**
         * @brief The §7.3 seam: does the delivered field's wire tag match the one
         *        the caller's read declares?
         *
         * A field's *tag* is its wire type plus, for the fixlen kinds, the subtype
         * — the two are only meaningful together, since `fp32`, `fp64`, `string`
         * and `blob` all share @ref Wire::Fixlen. Every typed @ref read compares
         * the whole tag here, so half a comparison cannot be written.
         *
         * On a mismatch the field is left unconsumed: the parser that delivered it
         * — @ref parseTopLevel at the top level, @ref dispatchLevel inside a
         * sequence — then skips it exactly like an unknown id, which is what
         * MESSAGE_SPEC §7.3 requires — this is **not** an error, and never affects
         * the decode outcome. The skip is counted in @ref skipped_ as a diagnostic.
         *
         * A read may admit *more than one* subtype — `read(std::string&)` takes a
         * `string` or a `blob`, since both are fixlen payloads and a std::string
         * is the documented destination for either. That is what the two-subtype
         * overload is for: it is still one comparison of the whole tag, so a
         * refusal counts exactly one skip, never one per candidate.
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

        /** @copydoc tagMatches(Wire, Fix)
         *  @param wantFix2 A second admissible fixlen subtype. */
        [[nodiscard]] bool tagMatches(Wire wantWire, Fix wantFix, Fix wantFix2) noexcept
        {
            if (type_ == wantWire && (fixType_ == wantFix || fixType_ == wantFix2)) return true;
            ++skipped_;
            return false;
        }

        /**
         * @brief Deliver the current fixlen payload into the destination the
         *        caller handed over (§6.6.3, second shape).
         *
         * The one place a `string` / `blob` payload reaches a destination, so the
         * §6.6 rule has one implementation and cannot hold in one read and be
         * missing from another. In order:
         *
         * 1. **The destination's room** (@ref detail::destRoom) against the
         *    announced length. Too short is §6.3's third refusal tier —
         *    @ref rejectDestination, `InvalidArgument` — because both bounds that
         *    can speak about the *message* have already spoken: the schema
         *    `maxlen` in the caller above, and any configured receiver cap. It is
         *    decided from the **header alone**, before a byte of payload is
         *    consulted, so where the chunk boundaries fall cannot change it.
         * 2. **Availability.** Fewer bytes than the declared length is
         *    `INCOMPLETE`, never an error (§5.2).
         * 3. **UTF-8** (§6.4), for a `string` payload only, and never on skip
         *    (§6.4.5 — this function runs only where a value is materialized).
         * 4. **The copy**, then the length (@ref sofab::detail::destSetLen). The
         *    destination is written exactly once, complete (§6.6.2).
         *
         * @tparam D Destination: @ref sofab::FixedString / @ref sofab::FixedBytes
         *         (static room) or a `std::string` / `std::vector<uint8_t>` the
         *         caller sized.
         * @param value Destination.
         * @param validateUtf8 Whether the payload is a `string` (§6.4).
         * @return `true` when the payload was delivered and the field consumed.
         */
        template <typename D>
        bool readPayload(D &value, bool validateUtf8) noexcept
        {
            const size_t avail = static_cast<size_t>(end_ - p_);
            if (pendDone_ == 0 && avail >= fixLen_) [[likely]]
            {
                /* The whole payload in one piece — every un-chunked feed, and every
                 * chunked one whose boundary did not fall inside this field. One
                 * room test, one validation pass, one copy. */
                if (!roomFor(value, fixLen_)) return false;
#if SOFAB_STRICT_UTF8
                if (validateUtf8 &&
                    !detail::utf8Valid(reinterpret_cast<const char *>(p_), fixLen_))
                { error_ = true; return false; }
#else
                (void)validateUtf8;
#endif
                if (fixLen_) std::memcpy(value.data(), p_, fixLen_);
                detail::destSetLen(value, fixLen_);
                p_ += fixLen_;
                consumed_ = true;
                return true;
            }
            return readPayloadPiece(value, validateUtf8, avail);
        }

        /**
         * @brief Is there room for @p need bytes in the destination the caller
         *        handed over (§6.6.3)?
         *
         * Two kinds of destination answer differently, and the difference is a fact
         * the type publishes rather than a policy this read picks:
         *
         * * **static room** (`D::capacity()`) cannot change between chunks, so the
         *   refusal is decided from the header alone — before a byte of payload is
         *   consulted — and where the chunk boundaries fall cannot change it;
         * * **room the caller sized** is judged against what this delivery can
         *   actually reach: the announced length, clamped to what has arrived. A
         *   declared length whose payload is absent or withheld reaches nothing and
         *   refuses nothing, so a truncated field stays `INCOMPLETE` (§5.2) instead
         *   of being mistaken for a destination that is too small; once the last
         *   piece is here the reach IS the announced length, and a destination
         *   genuinely too short is refused exactly then.
         *
         * @return `false` with @ref rejectDestination raised (`InvalidArgument`).
         */
        template <typename D>
        bool roomFor(D &value, size_t need) noexcept
        {
            if constexpr (requires { D::capacity(); })
            {
                if (fixLen_ > D::capacity()) { rejectDestination(); return false; }
            }
            else if (need > value.size()) { rejectDestination(); return false; }
            return true;
        }

        /**
         * @brief The chunk-straddling half of @ref readPayload — deliver the piece
         *        that has arrived and remember how far the value got.
         *
         * §6.6.2: "A payload split across fed chunks has to be joined somewhere.
         * That somewhere is storage the caller supplied — the codec copies each
         * piece into it as the piece arrives … A codec **MUST NOT** grow a private
         * accumulator instead." So the piece goes straight where the value belongs
         * and only the **length** is published, once, at the end — no caller ever
         * sees a half-written value (§6.6.2's "written exactly once, complete").
         *
         * Cold: it runs only for a field a chunk boundary fell inside.
         */
        template <typename D>
        bool readPayloadPiece(D &value, bool validateUtf8, size_t avail) noexcept
        {
            const size_t done = pendDone_;
            const size_t take = std::min(avail, fixLen_ - done);
            if (!roomFor(value, done + take)) return false;
#if SOFAB_STRICT_UTF8
            /* §6.4.4 permits exactly this — "A decoder MAY validate incrementally
             * provided it carries validator state across `feed` calls; no assembly
             * buffer is required" — and requires the verdict to be taken at payload
             * completion, never mid-payload, so a chunk boundary cannot decide it. */
            if (validateUtf8) utf8State_ = detail::utf8Advance(utf8State_, p_, take);
#else
            (void)validateUtf8;
#endif
            if (take) std::memcpy(value.data() + done, p_, take);
            p_ += take;
            if (done + take < fixLen_)
            {
                pendDone_ = done + take;
                incomplete_ = true;
                return false;
            }
#if SOFAB_STRICT_UTF8
            if (validateUtf8 && utf8State_ != detail::UTF8_ACCEPT)
            { error_ = true; return false; }
#endif
            detail::destSetLen(value, fixLen_);
            pendDone_ = 0;
            utf8State_ = detail::UTF8_ACCEPT;
            consumed_ = true;
            return true;
        }

        /**
         * @brief Would buffering this field cross @ref maxBufferedField_?
         *
         * @param consumed Bytes of the current top-level field already spanned.
         * @param need Further bytes the field is now known to require (a declared
         *        payload length, array byte-span, or per-element lower bound).
         * @return `true` if `consumed + need` exceeds the cap. Overflow-safe: the
         *         addition is never formed, so a caller that stated `SIZE_MAX` as
         *         its budget gets a check that runs and never fires, rather than a
         *         check this library switched off.
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
         * @tparam Cb The callable's own type, so the call is direct. A
         *         `std::function` parameter here cost a construction, a
         *         manager-driven destruction and two indirect hops **per
         *         sub-message read**, and the `std::function` machinery around
         *         it accounted for ~13 % of the `decode: typical message`
         *         profile. The two call sites pass a stateless no-op
         *         (@ref skipPayload) and a two-pointer lambda (@ref read), so
         *         the instantiation count is bounded and the body inlines into
         *         both.
         * @param cb Callback invoked as `(fieldId, size, count)` per field.
         * @param stopAtEnd If `true`, return at a @ref Wire::SequenceEnd
         *        marker (nested level); if `false`, such a marker is a decode error.
         */
        /**
         * @brief A non-owning reference to a level's per-field callback.
         *
         * The parse loops call their callback through a template parameter, so the
         * call is direct and inlines (that is deliberate — a `std::function`
         * parameter there cost ~13 % of `decode: typical message`). The **resume**
         * helpers below want the opposite: they run once per chunk boundary, never
         * in the loop, and compiling a copy of each of them into every one of the
         * dozens of `dispatchLevel` instantiations buys nothing but code size and
         * uncoverable lines. They take this instead — two pointers, one indirect
         * call on a cold path.
         */
        struct FieldCb
        {
            void *ctx;
            void (*fn)(void *, sofab::id, size_t, size_t);
            void operator()(sofab::id id, size_t size, size_t count) const noexcept
            {
                fn(ctx, id, size, count);
            }
        };

        /** @brief Wrap a level's callback as a @ref FieldCb. */
        template <typename Cb>
        static FieldCb wrapCb(Cb &cb) noexcept
        {
            return FieldCb{&cb, [](void *p, sofab::id id, size_t size, size_t count) noexcept {
                               (*static_cast<Cb *>(p))(id, size, count);
                           }};
        }

        /** @brief How a level is being entered (see @ref levelEntry). */
        enum class Entry : uint8_t
        {
            Fresh,   /**< Nothing open here: parse the next field. */
            Descend, /**< The suspended level is deeper: replay one more id. */
            Resume,  /**< This level is it: continue its half-delivered field. */
        };

        /**
         * @brief Decide how this level is being entered, and clear the replay state
         *        once the suspended level is reached.
         *
         * Non-template on purpose: every `dispatchLevel` instantiation asks the
         * same question, and only the answer differs.
         */
        Entry levelEntry() noexcept
        {
            if (!replay_) return Entry::Fresh;
            if (seqDepth_ < suspendDepth_) return Entry::Descend;
            replay_ = false;
            suspendDepth_ = -1;
            return pend_ ? Entry::Resume : Entry::Fresh;
        }

        template <typename Cb>
        void dispatchLevel(Cb &&cb, bool stopAtEnd) noexcept
        {
            /* Re-enter a level the previous chunk left open, rather than parsing
             * its bytes again — which is the whole of §6.6.2's "MUST NOT grow a
             * private accumulator": there is nothing to re-parse from. */
            const Entry entry = levelEntry();
            if (entry != Entry::Fresh) [[unlikely]]
            {
                if (!reenterLevel(wrapCb(cb), entry)) return;
            }

            while (p_ < end_ && !error_ && !incomplete_)
            {
                /* Everything before the delivery is the same code in every
                 * instantiation, and it is where §7.3's element test, §5.1's index
                 * bound and §4.6/§4.8's metadata rules all live, so it is written
                 * once (@ref beginField) rather than copied into each. */
                const Field f = beginField(stopAtEnd);
                if (f == Field::Stop) return;
                const sofab::id fieldId = fieldId_;
                const uint8_t *payload = p_;
                /* a §7.3-skipped element is never delivered; leaving it unconsumed
                 * runs it through the same skip as an unknown id. */
                if (f != Field::SkipElem) cb(fieldId, fixLen_, count_);
                if (!finishField(payload, fieldId)) return;
            }
            closeLevel(stopAtEnd);
        }

        /** @brief What @ref beginField found at this level's next field. */
        enum class Field : uint8_t
        {
            Ready,    /**< Deliver it. */
            SkipElem, /**< §7.3 says it is not this array's element: skip, do not deliver. */
            Stop,     /**< The level is finished, suspended or refused. */
        };

        /**
         * @brief Parse the next field's header and metadata at a nested level, and
         *        apply every rule that is decided before the value.
         *
         * Split out of @ref dispatchLevel so it is compiled **once** rather than
         * once per callback type: the loop's callback is a template parameter so
         * the delivery inlines, but everything around it is the same code in every
         * instantiation, and a dozen copies of it are a dozen copies of the §7.3
         * and §5.1 rules for a reader to keep in step.
         *
         * @param stopAtEnd Whether a @ref Wire::SequenceEnd closes this level.
         * @return @ref Field::Stop when @ref dispatchLevel must return.
         */
        Field beginField(bool stopAtEnd) noexcept
        {
            /* #26: a sequence's own bulk accrues field by field — bound it as it
             * grows, catching many-small-fields that no single payload check
             * would trip. */
            if (spanBase_ && exceedsBuffer(spanned(), 0))
            { exceedLimit(); return Field::Stop; }
            const uint8_t *const fieldStart = p_;
            if (!parseFieldTag())
            {
                /* A cut header is backed up whole: it is at most three varints,
                 * which is what @ref CARRY_CAP is sized for. */
                if (incomplete_) { p_ = fieldStart; suspendAtBoundary(); }
                return Field::Stop;
            }
            const sofab::id fieldId = fieldId_;
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
                else if (type_ != Wire::Fixlen) { rejectElementIndex(fieldId); return Field::Stop; }
                else boundPending = true; /* decided at the fixlen word */
            }

            if (type_ == Wire::SequenceEnd)
            {
                if (!stopAtEnd) error_ = true; /* §7: a dangling end */
                return Field::Stop;
            }

            /* The metadata that precedes the payload — one parser, shared
             * with the top level (@ref parseFieldMeta). It used to be copied
             * out here, and the copy silently lost the §6.2 count ceiling on
             * a nested fixlen array until #103 put it back; a second copy of
             * a validating parser is a hole waiting to reopen. */
            if (!parseFieldMeta())
            {
                if (incomplete_) { p_ = fieldStart; suspendAtBoundary(); }
                return Field::Stop;
            }

            /* the fixlen word is in: §7.3 first, then the §5.1/§7 bound. */
            if (boundPending)
            {
                if (elemFix_ >= 0 && static_cast<int>(fixType_) != elemFix_)
                { skipElem = true; ++skipped_; }
                else { rejectElementIndex(fieldId); return Field::Stop; }
            }

            /* From here the field's metadata is settled, so a chunk boundary
             * inside it no longer costs a rewind: it suspends. The progress
             * counters start at zero for a field nothing has delivered yet;
             * @ref pend_ / @ref pendSkip_ need no reset, since every suspension
             * writes them (@ref suspendHere, @ref suspendAtBoundary). */
            pendDone_ = 0;
            utf8State_ = 0;
            consumed_ = false;
            return skipElem ? Field::SkipElem : Field::Ready;
        }

        /**
         * @brief Settle a field the callback has been offered: skip it if the
         *        callback did not want it, suspend it if its bytes ran out.
         *
         * The callback did not want this field: rewind to the payload — which the
         * declining read left the cursor at anyway — and skip it, resumably, so a
         * long payload nobody asked for is stepped over as it arrives rather than
         * buffered (§6.6.2).
         *
         * §7: the bytes running out INSIDE the field is a different thing. It is
         * unfinished, not declined, and the two must not be confused — hence the
         * `!incomplete_` in the test. Skipping a truncation would rewind into a
         * payload the callback has already parsed *into* and re-read those bytes
         * under the metadata of whatever innermost field the descent left behind,
         * reporting INVALID for a truncation (Crucible F-0056, corelib-cpp#71).
         *
         * @param payload The field's first payload byte.
         * @param fieldId Its id, for the path entry a suspension records.
         * @return `false` when the level must return.
         */
        [[gnu::always_inline]] inline bool finishField(const uint8_t *payload, sofab::id fieldId) noexcept
        {
            bool declined = false;
            if (!incomplete_ && !consumed_)
            {
                declined = true;
                p_ = payload;
                skipPayload();
            }
            if (incomplete_)
            {
                suspendHere(fieldId, declined);
                return false;
            }
            return true;
        }

        /**
         * @brief The bytes ran out with this sequence still open — `INCOMPLETE`,
         *        not malformed (§7). The level is re-entered along @ref pathId_
         *        once the remaining bytes arrive.
         */
        void closeLevel(bool stopAtEnd) noexcept
        {
            if (stopAtEnd && !error_ && !incomplete_)
            {
                incomplete_ = true;
                suspendAtBoundary();
            }
        }

        /** @brief Take the @ref Entry::Descend or @ref Entry::Resume path. */
        bool reenterLevel(FieldCb cb, Entry entry) noexcept
        {
            if (entry == Entry::Descend) return descendResume(cb);
            restorePending();
            return deliverPending(cb, pendSkip_);
        }

        /**
         * @brief Record that this level's current field is the suspension point.
         *
         * The innermost level claims @ref suspendDepth_; every level above it then
         * writes its own @ref pathId_ entry as the stack unwinds, so the path is
         * complete by the time @ref feed sees the `INCOMPLETE`.
         *
         * @param fieldId Id of this level's field, for the @ref pathId_ entry a
         *        level above the suspension records.
         * @param declined Whether the field was being skipped rather than read.
         */
        void suspendHere(sofab::id fieldId, bool declined) noexcept
        {
            if (suspendDepth_ >= 0)
            {
                /* A deeper level is the suspension point; this one is on the path
                 * that reaches it. */
                pathId_[seqDepth_] = fieldId;
                pathSkip_[seqDepth_] = declined;
                return;
            }
            suspendDepth_ = seqDepth_;
            pend_ = true;
            pendSkip_ = declined;
            pendId_ = fieldId;
            pendType_ = type_;
            pendFix_ = fixType_;
            pendLen_ = fixLen_;
            pendCount_ = count_;
        }

        /**
         * @brief Claim the suspension for this level at a **field boundary** —
         *        nothing half-delivered, only a header cut in two or a sequence
         *        still open.
         *
         * The ≤ @ref CARRY_CAP bytes of the cut header ride in @ref carry_ and are
         * parsed again from their first byte; there is no pending field, so nothing
         * is re-delivered.
         */
        void suspendAtBoundary() noexcept
        {
            if (suspendDepth_ >= 0) return;
            suspendDepth_ = seqDepth_;
            pend_ = false;
            pendSkip_ = false;
        }

        /** @brief Put the suspended field's header back before continuing it. */
        void restorePending() noexcept
        {
            fieldId_ = pendId_;
            type_ = pendType_;
            fixType_ = pendFix_;
            fixLen_ = pendLen_;
            count_ = pendCount_;
        }

        /**
         * @brief Re-deliver the half-delivered field at this level.
         *
         * Its metadata (@ref type_, @ref fixLen_, @ref count_, @ref fieldId_) is
         * still the state the previous chunk left, and @ref pendDone_ says how far
         * it got, so the read (or the skip) continues where it stopped instead of
         * starting over.
         *
         * @return `false` when the level must return — the field suspended again,
         *         or the decode failed.
         */
        bool deliverPending(FieldCb cb, bool declined) noexcept
        {
            /* A descent below this field clobbers @ref fieldId_, so the id this
             * level would have to record is taken before the delivery. */
            const sofab::id id = fieldId_;
            consumed_ = false;
            if (declined) skipPayload();
            else          cb(id, fixLen_, count_);
            if (error_) return false;
            if (!incomplete_ && !consumed_ && !declined)
            {
                declined = true;
                skipPayload();
                if (error_) return false;
            }
            if (incomplete_)
            {
                suspendHere(id, declined);
                return false;
            }
            pend_ = false;
            pendSkip_ = false;
            pendDone_ = 0;
            return true;
        }

        /**
         * @brief Re-descend one recorded level of @ref pathId_ towards
         *        @ref suspendDepth_.
         *
         * Replays the sequence field this level was inside when the chunk ran out:
         * either by handing its id back to @p cb — whose `deserialize` dispatches
         * on the id and calls `read` on the same member, re-establishing exactly the
         * handler that was open — or, for a level that was being **skipped**, by
         * re-entering the skip descent directly. Nothing is parsed here: the bytes
         * of the sequence header were consumed by an earlier chunk and are gone.
         *
         * @param cb This level's per-field callback.
         * @return `false` when the level must return.
         */
        bool descendResume(FieldCb cb) noexcept
        {
            const int d = seqDepth_;
            const sofab::id fieldId = pathId_[d];
            const bool declined = pathSkip_[d];
            fieldId_ = fieldId;
            type_ = Wire::SequenceStart;
            fixLen_ = 0;
            count_ = 0;
            if (declined)
            {
                if (seqDepth_ >= MAX_DEPTH) { error_ = true; return false; }
                skipSequence();
            }
            else
            {
                consumed_ = false;
                cb(fieldId, size_t{0}, size_t{0});
            }
            if (error_) return false;
            if (incomplete_)
            {
                suspendHere(fieldId, declined);
                return false;
            }
            return true;
        }

        /**
         * @brief Step over the payload of the current field, resumably.
         *
         * Called for a field the callback declined. Like every read, it never
         * buffers what it has not finished: a payload cut by a chunk boundary
         * leaves its byte (or element) progress in @ref pendDone_ and is continued
         * on the next @ref feed, and a **small** item cut in half — a lone varint,
         * an element — is backed up to its own start so the ≤ @ref CARRY_CAP bytes
         * of it can be carried (§6.6.2).
         *
         * Assumes the cursor sits at the payload's resume point and the field
         * metadata is set.
         */
        void skipPayload() noexcept
        {
            switch (type_)
            {
                case Wire::Unsigned:
                case Wire::Signed:
                {
                    /* One varint, at most @ref detail::VARINT_MAX_BYTES bytes: cut
                     * in half it is backed up whole rather than half-consumed, so
                     * the carry holds it and this runs again from its first byte. */
                    bool ovf = false;
                    if (!skipVarint(p_, end_, &ovf))
                        (ovf ? error_ : incomplete_) = true;
                    break;
                }
                case Wire::Fixlen:
                {
                    /* A payload of any length, stepped over as it arrives. */
                    const size_t left = fixLen_ - pendDone_;
                    const size_t avail = static_cast<size_t>(end_ - p_);
                    if (avail < left)
                    {
                        p_ += avail;
                        pendDone_ += avail;
                        incomplete_ = true;
                        break;
                    }
                    p_ += left;
                    pendDone_ = 0;
                    break;
                }
                case Wire::ArrayUnsigned:
                case Wire::ArraySigned:
                {
                    for (size_t i = pendDone_; i < count_; ++i)
                    {
                        bool ovf = false;
                        if (!skipVarint(p_, end_, &ovf))
                        {
                            if (ovf) { error_ = true; return; }
                            pendDone_ = i;
                            incomplete_ = true;
                            return;
                        }
                    }
                    pendDone_ = 0;
                    break;
                }
                case Wire::ArrayFixlen:
                {
                    const size_t bytes = count_ * fixLen_;
                    const size_t left = bytes - pendDone_;
                    const size_t avail = static_cast<size_t>(end_ - p_);
                    if (avail < left)
                    {
                        p_ += avail;
                        pendDone_ += avail;
                        incomplete_ = true;
                        break;
                    }
                    p_ += left;
                    pendDone_ = 0;
                    break;
                }
                case Wire::SequenceStart:
                {
                    if (seqDepth_ >= MAX_DEPTH) /* §4.9 */
                    {
                        error_ = true;
                        break;
                    }
                    skipSequence();
                    break;
                }
                case Wire::SequenceEnd:
                    break;
            }
        }

        /**
         * @brief Walk a sub-sequence nobody asked for, delivering nothing.
         *
         * Split out of @ref skipPayload because a skipped sequence, like every
         * other construct, has to be **re-enterable**: when a chunk ends inside one
         * the level is recorded in @ref pathSkip_ and @ref descendResume comes back
         * through here, with no callback to replay.
         *
         * §7.3: this sequence is not an element of the enclosing wrapper — and the
         * fields INSIDE it are not that wrapper's elements either. Their ids are
         * child ids of a field that never became a value, not array indices, so the
         * element-index bound must not measure them: it is suspended for the whole
         * subtree and restored after. Without this a child id at or past the
         * wrapper's `count` would trip the §5.1/§7 reject from inside a field §7.3
         * says is not the array's at all (Crucible F-0051, corelib-cpp#65). The
         * suspension is not specific to a §7.3-mistyped element: an unknown id
         * skipped inside the wrapper reaches the same place and is equally not an
         * element. Format-level rejects (§4.9 depth, over-64-bit varint, ...) still
         * fire inside the subtree — §7.3 subordinates the SCHEMA bound only.
         */
        void skipSequence() noexcept
        {
            const long outerBound = elemBound_, outerSchema = elemSchema_;
            const long outerDyn = elemDyn_, outerDest = elemDest_;
            const int outerElemWire = elemWire_, outerElemFix = elemFix_;
            elemBound_ = elemSchema_ = elemDyn_ = elemDest_ = -1;
            elemWire_ = elemFix_ = -1;
            ++seqDepth_;
            dispatchLevel([](sofab::id, size_t, size_t) noexcept {}, /*stopAtEnd*/ true);
            --seqDepth_;
            elemBound_ = outerBound;
            elemSchema_ = outerSchema;
            elemDyn_ = outerDyn;
            elemDest_ = outerDest;
            elemWire_ = outerElemWire;
            elemFix_ = outerElemFix;
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
         * chunks is **resumed** on a later call — its pieces are written into the
         * caller's destination as they arrive, and only what a chunk boundary can
         * cut in half (a field header, the words before a payload, one varint
         * element — at most @ref CARRY_CAP bytes) is carried over. Every chunk is
         * parsed in place; nothing is copied but the value itself.
         *
         * @note A field split across chunks is therefore **delivered once per
         *       chunk that carries part of it**, with the same id and header, and
         *       the typed read continues where it stopped (@ref progress). The
         *       destination has to stay put until the field completes: a member of
         *       the message object does, a local in the callback does not.
         *
         * @param buffer Bytes to decode.
         * @param buflen Number of bytes in @p buffer.
         * @return A @ref Result carrying the three-valued §7 outcome:
         *         @ref Error::None (`COMPLETE`) when the fed bytes end exactly at a
         *         field boundary; @ref Error::Incomplete (`INCOMPLETE`) when they end
         *         **inside** a field (partial varint, short fixlen/array payload) or
         *         with an open sequence — the decoder keeps its place for the next
         *         @ref feed and this is **not** an error; or @ref Error::InvalidMessage
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
            /* §5.2/§6.3: INVALID, LimitExceeded and InvalidArgument are TERMINAL.
             * Once the stream has returned one, no later chunk can change it — the
             * bytes were malformed regardless of what follows, or the receiver's
             * policy already refused them. */
            if (terminal_ != Error::None) [[unlikely]] return Result{terminal_, skipped_};

            /* §5.2.4: an empty feed is the end-of-input probe, and the verdict is
             * read off the decoder's own state. It is also the empty message
             * (MESSAGE_SPEC §2), which is COMPLETE for every schema. */
            if (buflen == 0)
                return Result{atBoundary() ? Error::None : Error::Incomplete, skipped_};

            error_ = false;
            limitExceeded_ = false;
            argError_ = false;

            const uint8_t *cur = buffer;
            size_t left = buflen;

            if (carryLen_) [[unlikely]]
            {
                /* An item was cut in half by the previous chunk boundary. Stitch it
                 * back together in a window of @ref CARRY_CAP bytes on the stack —
                 * never the whole chunk — and parse out of that; whatever of this
                 * chunk the window did not need is then parsed in place, with no
                 * copy at all. At most @ref CARRY_CAP bytes are ever copied,
                 * whatever the caller's chunk size, which is the property §6.6
                 * protects: "can a sender make this allocation bigger by sending
                 * different bytes?" — no. */
                uint8_t stitch[CARRY_CAP];
                const size_t held = carryLen_;
                const size_t take = std::min(CARRY_CAP - held, left);
                std::memcpy(stitch, carry_, held);
                std::memcpy(stitch + held, cur, take);
                const size_t window = held + take;
                carryLen_ = 0;
                const size_t used = parseWindow(stitch, window);
                if (error_) return latchTerminal();
                if (used < held)
                {
                    /* Still short of a complete item, so the parse consumed nothing
                     * — the carry holds exactly one item's prefix — and the chunk
                     * went in whole. */
                    if (take < left) { error_ = true; return latchTerminal(); }
                    std::memcpy(carry_, stitch + used, window - used);
                    carryLen_ = window - used;
                    return Result{Error::Incomplete, skipped_};
                }
                cur += used - held;
                left -= used - held;
                if (left == 0)
                {
                    stash(stitch + used, window - used);
                    return Result{incomplete_ || carryLen_ ? Error::Incomplete : Error::None,
                                  skipped_};
                }
                /* Anything the window left unconsumed is still ahead of `cur` in the
                 * caller's own chunk, so the parse below simply reaches it again. */
            }

            const size_t used = parseWindow(cur, left);
            if (error_) return latchTerminal();
            stash(cur + used, left - used);
            if (error_) return latchTerminal();
            return Result{incomplete_ || carryLen_ ? Error::Incomplete : Error::None, skipped_};
        }

        /**
         * @return `true` when the decoder sits at a clean message boundary: nothing
         *         carried, no open sequence, no half-delivered field (§5.2.4).
         */
        [[nodiscard]] bool atBoundary() const noexcept
        {
            return carryLen_ == 0 && suspendDepth_ < 0;
        }

        /**
         * @brief Drop every byte of decoder state, so the next @ref feed starts a
         *        brand-new message.
         *
         * Discards the buffered partial field, the sticky error/limit flags, the
         * latched terminal verdict (§5.2/§6.3 — this is the only way to clear an
         * `INVALID` or `LimitExceeded` stream), the nesting depth and the §7.3
         * @ref skipped counter. There is no buffer to hand back: the decoder's
         * working state is the fixed-size carry, path and progress it was
         * constructed with (§6.6), so a reset is a handful of stores and the
         * message loop pays no allocator for it.
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
            carryLen_ = 0;
            suspendDepth_ = -1;
            replay_ = false;
            pend_ = false;
            pendSkip_ = false;
            pendDone_ = 0;
            utf8State_ = 0;
            spanCarry_ = 0;
            spanBase_ = nullptr;
            p_ = end_ = nullptr;
            type_ = Wire{};
            fixType_ = Fix{};
            fixLen_ = 0;
            count_ = 0;
            fieldId_ = 0;
            consumed_ = false;
            error_ = false;
            limitExceeded_ = false;
            argError_ = false;
            terminal_ = Error::None;
            incomplete_ = false;
            seqDepth_ = 0;
            skipped_ = 0;
            elemBound_ = -1;
            elemSchema_ = -1;
            elemDyn_ = -1;
            elemDest_ = -1;
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
        void exceedLimit() noexcept { limitExceeded_ = error_ = true; }

        /**
         * @brief Refuse the current field because the caller's **destination** is
         *        too short for it (§6.3's third refusal tier, §6.6.3).
         *
         * The sibling of @ref invalidate and @ref exceedLimit, for the one refusal
         * that is about neither the message nor the deployment: the value broke no
         * schema bound and no configured cap, and simply does not fit the storage
         * this caller handed over. §6.3: "`InvalidMessage` would mark a well-formed
         * message malformed … `LimitExceeded` would promise a limit to raise that
         * was never configured." The surrounding @ref feed stops dispatching and
         * returns @ref Error::InvalidArgument. Idempotent; a no-op outside a
         * @ref feed since every feed clears the flag on entry.
         */
        void rejectDestination() noexcept { argError_ = error_ = true; }

        /**
         * @brief Refuse the current field because the **call** stated no bound for
         *        it at all (§6.3's `InvalidArgument`).
         *
         * The one answer §6.2.1 leaves for a schema-unbounded field read with no
         * receiver cap. This codec has no limit of its own to fall back on, and
         * "an argument a caller may omit is an API affordance, never a licence to
         * decode uncapped" — so the omission is reported rather than obeyed. It is
         * a mistake in the *call*, exactly like a destination too short for the
         * value (@ref rejectDestination): the message is well-formed and the
         * deployment configured nothing, so neither `InvalidMessage` nor
         * `LimitExceeded` would say anything true about it (§6.3's three tiers).
         *
         * Reaching it from generated code means a cap was not emitted; reaching it
         * from a hand-written visitor means the wrong entry point was used — the
         * `…Capped` forms exist so the number cannot be forgotten.
         */
        void rejectUnbounded() noexcept { argError_ = error_ = true; }

        /**
         * @brief Refuse a wrapper-array element whose **index** is at or past a
         *        bound, in the category §6.3 gives that bound.
         *
         * MESSAGE_SPEC §5.1 makes a wrapper array's length *highest present id +
         * 1*, so the element index **is** the length and is what has to be
         * checked — "A limit **MUST** be enforced … before the container it
         * indexes into is extended" (§6.2.1). Three bounds can be armed at once
         * and they do not share a verdict, so they are consulted in §6.3's order:
         * the schema `count` first (`InvalidMessage`), then the configured
         * receiver cap (`LimitExceeded`), then the destination's own capacity
         * (`InvalidArgument`).
         *
         * @param bad The element index that crossed a bound.
         */
        void rejectElementIndex(sofab::id bad) noexcept
        {
            const long i = static_cast<long>(bad);
            if (elemSchema_ >= 0 && i >= elemSchema_)   invalidate();
            else if (elemDyn_ >= 0 && i >= elemDyn_)    exceedLimit();
            else                                        rejectDestination();
        }


    protected:
        /**
         * @brief The bytes of the current top-level field seen so far, across every
         *        chunk it spans (#26, @ref Limits::max_buffered_field).
         *
         * Pointer arithmetic alone stopped being enough once a field could span
         * chunks without being buffered: within one window the span is
         * `p_ - spanBase_`, and what earlier windows already consumed of the same
         * field is carried in @ref spanCarry_. Without that the cap would be a
         * property of the caller's chunking, which §7.2 item 4 forbids.
         */
        [[nodiscard]] size_t spanned() const noexcept
        {
            return spanCarry_ + static_cast<size_t>(p_ - spanBase_);
        }

        /** @brief Latch the refusal this feed produced, in §6.3's order of tiers. */
        Result latchTerminal() noexcept
        {
            terminal_ = limitExceeded_  ? Error::LimitExceeded
                        : argError_     ? Error::InvalidArgument
                                        : Error::InvalidMessage;
            return Result{terminal_, skipped_};
        }

        /**
         * @brief Hold the ≤ @ref CARRY_CAP bytes of a cut item until the next
         *        @ref feed.
         */
        void stash(const uint8_t *p, size_t n) noexcept
        {
            if (n == 0) return;
            if (n > CARRY_CAP) [[unlikely]]
            {
                /* Unreachable: the parse backs up only to the start of a small
                 * item, and @ref CARRY_CAP is derived from the widest of those.
                 * Deliberately not silent — an item wider than this format admits
                 * is malformed either way. */
                error_ = true;
                return;
            }
            std::memcpy(carry_, p, n);
            carryLen_ = n;
        }

        /**
         * @brief Parse one window of bytes, resuming whatever the last one left
         *        open, and report how much of it was consumed.
         *
         * @param p Window start.
         * @param n Window length.
         * @return Bytes consumed. Whatever follows is a cut item, at most
         *         @ref CARRY_CAP bytes of it.
         */
        size_t parseWindow(const uint8_t *p, size_t n) noexcept
        {
            p_ = p;
            end_ = p + n;
            incomplete_ = false;
            replay_ = suspendDepth_ >= 0;
            spanBase_ = p_;
            parseTopLevel();
            if (incomplete_) spanCarry_ += static_cast<size_t>(p_ - spanBase_);
            replay_ = false;
            return static_cast<size_t>(p_ - p);
        }

        /**
         * @brief Deliver every complete top-level field in the current window,
         *        leaving @ref p_ at the first byte not consumed.
         *
         * The same shape as @ref dispatchLevel, and for the same reasons: a level
         * the previous window left open is **re-entered** rather than re-parsed,
         * either by descending @ref pathId_ to the suspended level or — when this
         * level is it — by continuing the field @ref pendDone_ describes.
         */
        void parseTopLevel() noexcept
        {
            sofab::id fieldId = 0;
            bool declined = false;

            const Entry entry = levelEntry();
            if (entry != Entry::Fresh) [[unlikely]]
            {
                if (!reenterLevel(wrapCb(topCallback_), entry)) return;
            }

            const uint8_t *const end = end_;
            while (p_ < end)
            {
                /* Header-first: parse the field's header and metadata, then deliver
                 * immediately. The callback's typed read decides the tag (§7.3), the
                 * schema bound (§7.1/§5.2) and finally whether the payload is here —
                 * in that order, so a bound rejection wins over a truncation without
                 * anyone having to know the schema up front. */
                const uint8_t *fieldStart = p_;
                spanCarry_ = 0;
                spanBase_ = p_;
                incomplete_ = false;
                declined = false;
                if (!parseFieldHeader())
                {
                    /* error_ or incomplete_ set. A cut header is backed up whole:
                     * it is at most three varints, which is what @ref CARRY_CAP is
                     * sized for. */
                    if (incomplete_) { p_ = fieldStart; suspendAtBoundary(); }
                    return;
                }
                fieldId = fieldId_;
                pendDone_ = 0;
                utf8State_ = 0;
                if (exceedsBufferAtHeader())
                {
                    /* §6.2.1/§6.3: a receiver-side cap "MUST NOT be applied to a
                     * field the schema already bounds" — there the schema governs
                     * and an over-bound claim is INVALID, which is why §6.3 says
                     * LimitExceeded is "never raised for a field the schema bounds"
                     * (#86). Only the callback knows the declared maxlen/count, so
                     * the field is offered to it first with its payload WITHHELD:
                     * end_ sits at the payload's first byte, so a typed read still
                     * settles the tag (§7.3) and the bound (§7.1) — both decided
                     * before a byte is copied — and then reports INCOMPLETE instead
                     * of materialising anything. Nothing is copied and nothing is
                     * allocated; the only verdict that can come out of it is the
                     * INVALID the same bytes get on an uncapped stream. */
                    if (schemaBoundsHeader(type_))
                    {
                        consumed_ = false;
                        const uint8_t *held = end_;
                        end_ = p_;
                        topCallback_(fieldId_, fixLen_, count_);
                        end_ = held;
                        incomplete_ = false;
                        pendDone_ = 0;
                        /* The schema bound spoke first — and so does a receiver cap
                         * the callback applied itself. An ARGUMENT refusal does not:
                         * §6.3 puts the configured cap ahead of the caller's
                         * destination, and the cap enforced below is one. */
                        if (error_ && !argError_) return;
                        error_ = false;
                        argError_ = false;
                    }
                    exceedLimit();
                    return;
                }

                consumed_ = false;
                const uint8_t *payload = p_;
                topCallback_(fieldId_, fixLen_, count_);
                if (error_) return;
                if (!incomplete_ && !consumed_)
                {
                    declined = true;
                    p_ = payload;
                    skipPayload();
                    if (error_) return;
                }
                /* Not enough bytes yet: the field keeps its place. What it has
                 * already delivered stays delivered — @ref pendDone_ says how much —
                 * so nothing is re-read and nothing is buffered (§6.6.2). */
                if (incomplete_)
                {
                    suspendHere(fieldId, declined);
                    return;
                }
            }
        }

        /**
         * @brief Parse a field header word into @ref fieldId_ and @ref type_ (§4.3).
         *
         * The **only** place a `(id << 3) | wire_type` word is taken apart. Both
         * parse loops — @ref parseTopLevel through @ref parseFieldHeader, and
         * @ref dispatchLevel inside a sequence — come through here, so the
         * @ref ID_MAX ceiling (§6.2) cannot be present in one and missing from
         * the other.
         *
         * @return `true` when a whole header word was read and its id is legal.
         *         On `false` either @ref error_ (a > 64-bit varint, or an id past
         *         @ref ID_MAX) or @ref incomplete_ (the word is cut short) is set.
         */
        bool parseFieldTag() noexcept
        {
            uint64_t header;
            bool ovf = false;
            if (!getVarint(p_, end_, header, &ovf))
            {
                (ovf ? error_ : incomplete_) = true;
                return false;
            }
            if ((header >> 3) > ID_MAX) /* §6.2/§7 */
            {
                error_ = true;
                return false;
            }
            fieldId_ = static_cast<sofab::id>(header >> 3);
            type_ = static_cast<Wire>(header & 0x7);
            return true;
        }

        /**
         * @brief Parse the word(s) between a field header and its payload —
         *        the fixlen word (§4.6), the array count (§4.7) or both (§4.8).
         *
         * Shared by the two parse loops for the same reason as @ref parseFieldTag:
         * this is where §4.6's subtype/length pairing, §4.8's element-subtype rule
         * and §6.2's @ref ARRAY_MAX ceiling are enforced, and a second copy of it
         * is a place for one of them to go missing — which is exactly how the
         * nested path lost the @ref ARRAY_MAX check on a fixlen array until #103.
         * A varint or sequence header states no metadata, so it falls through.
         *
         * @return `true` when the metadata is complete and well-formed, with
         *         @ref fixLen_, @ref count_ and @ref fixType_ set. On `false`
         *         either @ref error_ or @ref incomplete_ is set.
         */
        bool parseFieldMeta() noexcept
        {
            fixLen_ = 0; count_ = 0;
            bool ovf = false;
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
                    return true;
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
                    return true;
                }
                case Wire::ArrayFixlen:
                {
                    uint64_t n;
                    if (!getVarint(p_, end_, n, &ovf))
                    {
                        (ovf ? error_ : incomplete_) = true;
                        return false;
                    }
                    /* §4.8 step 1: the FORMAT ceiling fires on the count word
                     * whatever the subtype turns out to be, so an absurd count is
                     * rejected before anything is sized from it. Without it the
                     * `count_ * fixLen_` byte-span computed downstream wraps
                     * size_t — a count of 2^62 with 4-byte elements wraps to zero
                     * and the array is skipped as if it were empty, so a message
                     * that must be INVALID decodes COMPLETE (#103). */
                    if (n > ARRAY_MAX) /* §6.2/§7 */
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
                    fixLen_ = static_cast<size_t>(sub >> 3); /* element size */
                    fixType_ = static_cast<Fix>(sub & 0x7);
                    return true;
                }
                default:
                    return true; /* varint and sequence headers carry no metadata */
            }
        }

        /**
         * @brief Parse one **top-level** field header plus the metadata that
         *        precedes its payload, into the current-field members.
         *
         * @ref parseFieldTag + @ref parseFieldMeta, with the one rule that is the
         * top level's own: a @ref Wire::SequenceEnd here closes a sequence that
         * was never opened, which is `INVALID` (§7). Inside a sequence the same
         * marker is the close, which is why @ref dispatchLevel puts its own test
         * between the two calls.
         *
         * @return `true` when the header is complete and well-formed. On `false`
         *         either @ref error_ (malformed) or @ref incomplete_ (more bytes
         *         needed) is set.
         */
        bool parseFieldHeader() noexcept
        {
            if (!parseFieldTag()) return false;
            if (type_ == Wire::SequenceEnd) /* §7: dangling end at the root */
            {
                error_ = true;
                return false;
            }
            return parseFieldMeta();
        }

        /**
         * @brief Does this wire type's header state a size the schema can bound?
         *
         * True for the length-prefixed payload (`maxlen`) and the three array kinds
         * (`count`) — exactly the fields whose §7.1 verdict outranks the field-size
         * cap (#86), and exactly the ones @ref exceedsBufferAtHeader derives a size
         * from. A sequence states no size of its own (it accrues in
         * @ref dispatchLevel) and a varint carries no length word at all.
         *
         * @param w The delivered field's wire type.
         * @return `true` when a declared `maxlen`/`count` could reject this field.
         */
        [[nodiscard]] static constexpr bool schemaBoundsHeader(Wire w) noexcept
        {
            return w == Wire::Fixlen || w == Wire::ArrayUnsigned ||
                   w == Wire::ArraySigned || w == Wire::ArrayFixlen;
        }

        /**
         * @brief Would this field cross @ref maxBufferedField_, judged from its
         *        header alone (#26)?
         *
         * The header states the size exactly for a fixlen or fixlen-array payload;
         * a varint array's count is a lower bound on its bytes. A sequence accrues
         * instead, and is bounded field by field in @ref dispatchLevel.
         *
         * @return `true` when the field must be rejected before its payload is
         *         read.
         */
        [[nodiscard]] bool exceedsBufferAtHeader() noexcept
        {
            const size_t seen = spanned();
            uint64_t need = 0;
            switch (type_)
            {
                case Wire::Fixlen:      need = fixLen_; break;
                case Wire::ArrayFixlen: need = static_cast<uint64_t>(count_) * fixLen_; break;
                case Wire::ArrayUnsigned:
                case Wire::ArraySigned: need = count_; break;
                default:                need = 0; break;
            }
            return exceedsBuffer(seen, need);
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
            /* Elements already decoded by an earlier chunk (§6.6.2): the run
             * continues where it stopped. What it never does is start over — the
             * bytes of the elements already delivered are gone. */
            size_t i = pendDone_;
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
                        if (ovf) { error_ = true; return false; }
                        /* The cut element is left at its first byte: the carry
                         * holds it and the run resumes at element `i`. */
                        pendDone_ = i;
                        incomplete_ = true;
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
                    if (!getVarintWindowed(p_, raw, nullptr))
                    {
                        error_ = true; /* only a > 64-bit varint can fail here */
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
                    bool cut = false;
                    if constexpr (Bounded)
                    {
                        uint64_t raw;
                        if (!getVarint(p_, end_, raw, &ovf)) cut = true;
                        else if (!admits(raw)) { error_ = true; return false; }
                    }
                    else if (!skipVarint(p_, end_, &ovf)) cut = true;
                    if (cut)
                    {
                        if (ovf) { error_ = true; return false; }
                        pendDone_ = i;
                        incomplete_ = true;
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
            pendDone_ = 0;
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
                    /* A varint cut by a chunk boundary stays where it is (§6.6.2's
                     * "partial varint"): its ≤ 9 bytes ride in the carry and the
                     * field is continued, not re-parsed from its header. */
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
                /* §7.3: a std::string destination declares a fixlen *payload* —
                 * `string` or `blob`. Both are byte payloads a std::string owns
                 * verbatim, so either may be read into one (readString/readBlob
                 * are how a caller narrows it to exactly one). `fp32` and `fp64`
                 * share Wire::Fixlen with them but are not payloads: comparing
                 * the wire type alone admitted them and materialised their four
                 * or eight raw value bytes as text — and, since the UTF-8 gate
                 * below is on the subtype, without even validating them. They
                 * are skipped like an unknown id instead. */
                if (!tagMatches(Wire::Fixlen, Fix::String, Fix::Blob))
                    return false; /* §7.3, see the view branch */
                /* §6.6.3: the destination is what the caller handed over, and a
                 * payload longer than its room is refused rather than grown into
                 * — see @ref readPayload. */
                if (!readPayload(value, fixType_ == Fix::String)) return false;
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
                /* ...and only on the FIRST entry. A sequence re-entered along
                 * @ref pathId_ after a chunk boundary is the SAME occurrence, so
                 * resetting the collector here would throw away the elements that
                 * already arrived with the earlier chunks. */
                if (!replay_)
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
                /* Three bounds, three categories (§6.3), one fast gate. A
                 * collector declares the schema `count` as `cap`, the configured
                 * receiver cap as `dynCap` (§6.2.1's `max_dyn_array_count` — the
                 * INDEX is what it binds, a wrapper array having no count header
                 * to bind) and the destination's own capacity as the static
                 * `elemDestCap`. @ref elemBound_ is the tightest armed one, so the
                 * per-element test stays a single compare; @ref rejectElementIndex
                 * sorts out which tier spoke, and only when one has. */
                const long outerSchema = elemSchema_, outerDyn = elemDyn_, outerDest = elemDest_;
                if constexpr (requires { value.cap; }) elemSchema_ = value.cap;
                else                                   elemSchema_ = -1;
                /* No fallback, and no way to omit it either: a collector that
                 * publishes a schema `cap` MUST publish a `dynCap` beside it. The
                 * two are the array's two possible index bounds and exactly one of
                 * them can apply (§6.2.1), so a collector carrying only the first
                 * used to leave the second silently at "no cap" — a duck-typing
                 * miss nothing diagnosed. The stream has no limit of its own to
                 * lend it (§6.2.1 — the codec "MUST NOT supply a default for one it
                 * was not given"), so the omission is a compile error instead. */
                static_assert(!(requires { value.cap; }) || (requires { value.dynCap; }),
                              "A wrapper-array collector that publishes a schema `cap` must also "
                              "publish a receiver `dynCap` (CORELIB_PLAN 6.2.1): without it a "
                              "schema-unbounded array would be bounded by nothing.");
                if constexpr (requires { value.dynCap; }) elemDyn_ = value.dynCap;
                else                                      elemDyn_ = -1;
                if constexpr (requires { T::elemDestCap; }) elemDest_ = static_cast<long>(T::elemDestCap);
                else                                        elemDest_ = -1;
                /* §6.2.1: a receiver limit "MUST NOT be applied to a field the
                 * schema already bounds. There the schema bound governs and its
                 * violation is INVALID." */
                if (elemSchema_ >= 0) elemDyn_ = -1;
                elemBound_ = -1;
                for (long b : {elemSchema_, elemDyn_, elemDest_})
                    if (b >= 0 && (elemBound_ < 0 || b < elemBound_)) elemBound_ = b;
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
                elemSchema_ = outerSchema;
                elemDyn_ = outerDyn;
                elemDest_ = outerDest;
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
                    const size_t bytes = count_ * sizeof(Elem);
                    if (pendDone_ == 0 &&
                        static_cast<size_t>(end_ - p_) >= bytes) [[likely]]
                    {
                        /* The whole payload in one piece: one bulk move, which is
                         * what the fixlen array's wire form exists for. */
                        if constexpr (std::endian::native == std::endian::little)
                        {
                            /* An empty destination span has a null data(), which
                             * memcpy forbids even for a zero length. */
                            if (n) std::memcpy(sp.data(), p_, n * sizeof(Elem)); /* wire == native */
                        }
                        else
                            for (size_t i = 0; i < n; ++i)
                                sp[i] = loadFloat<Elem>(p_ + i * sizeof(Elem));
                        p_ += bytes;
                    }
                    else
                    {
                        /* Split across feeds: element by element into the caller's
                         * destination as each one lands (§6.6.2). A part-arrived
                         * element is left where it is — at most seven bytes, which
                         * the carry holds. */
                        size_t i = pendDone_;
                        while (i < count_)
                        {
                            if (static_cast<size_t>(end_ - p_) < sizeof(Elem))
                            {
                                pendDone_ = i;
                                incomplete_ = true;
                                return false;
                            }
                            if (i < n) sp[i] = loadFloat<Elem>(p_);
                            p_ += sizeof(Elem);
                            ++i;
                        }
                        pendDone_ = 0;
                    }
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
         * @par The three ways this refuses (§6.3), in the only correct order
         *  1. The wire subtype contradicts `string` → the field is **skipped**
         *     (§7.3), and neither bound below is applied to it: "a skipped field
         *     is never capped" (§6.2.1).
         *  2. @p bound is declared and the announced length exceeds it →
         *     `InvalidMessage` (MESSAGE_SPEC §7.1). The schema calls these bytes
         *     invalid.
         *  3. @p bound is negative — the schema declares none — and the announced
         *     length exceeds @p dynCap → @ref Error::LimitExceeded (§6.2.1). The
         *     bytes are well-formed; this receiver declines to hold that much.
         *
         * All three are decided **at the length header**, before the destination is
         * sized (§6.2.1's enforcement point, §5.2.3): @ref sofab::readString, the
         * helper that sizes a growable destination, applies the identical gate
         * before it grows anything.
         *
         * @par Two entry points, so a number cannot be forgotten
         * This overload is the **schema-bounded** read and takes the declared
         * `maxlen` only; @ref readStringCapped is the **schema-unbounded** one and
         * takes the §6.2.1 receiver cap only. Exactly one of the two numbers can
         * ever apply — a receiver limit "**MUST NOT** be applied to a field the
         * schema already bounds" — so the choice belongs in the call's *name*
         * rather than in a pair of arguments where either may be left out. Neither
         * has a default, and neither reads a negative value as "unlimited": a
         * @p bound below zero is not a schema-unbounded read, it is a call that
         * stated no bound at all, and it answers @ref Error::InvalidArgument
         * through @ref rejectUnbounded.
         *
         * @par The bound is passed in, never held
         * §6.2.1 leaves this corelib "the report and the category" and puts the
         * *numbers* in generated code, so @p bound is the caller's, used for this
         * one comparison and not retained. There is no stream-wide fallback of any
         * kind. Passing the number here rather than testing it in front of the call
         * is what keeps case 1 above true: the tag test is inside, so a field the
         * decoder must skip never meets the ceiling.
         *
         * @param[out] value Destination for the decoded text.
         * @param bound Declared `maxlen`. **Required and non-negative**; a
         *              schema-unbounded field is read through
         *              @ref readStringCapped instead.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename S>
        bool readString(S &value, long bound) noexcept
        {
            if (bound < 0) { rejectUnbounded(); return false; }
            return readStringGated(value, bound, -1);
        }

        /**
         * @brief Read the current field as a schema-**unbounded** `string` under
         *        the receiver's `max_dyn_string_len` (§6.2.1).
         *
         * The counterpart of @ref readString for a field whose schema declares no
         * `maxlen`. It refuses in the same order — tag (§7.3), then the cap
         * (@ref Error::LimitExceeded) — and the cap is the caller's, used for this
         * one comparison and not retained.
         *
         * It exists as its own entry point rather than as a defaulted argument
         * because a defaulted one can be left out, and a schema-unbounded read with
         * no cap is the sender choosing how much this process holds. There is
         * nothing to default to: §6.2.1 forbids this codec a limit of its own.
         *
         * @param[out] value Destination for the decoded text.
         * @param dynCap The caller's `max_dyn_string_len`. **Required and
         *               non-negative**; a negative value is a call that stated no
         *               cap and answers @ref Error::InvalidArgument.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename S>
        bool readStringCapped(S &value, long dynCap) noexcept
        {
            if (dynCap < 0) { rejectUnbounded(); return false; }
            return readStringGated(value, -1, dynCap);
        }

    protected:
        /**
         * @brief The body both `string` reads share, so the two cannot answer
         *        differently. Exactly one of @p bound / @p dynCap is non-negative.
         *
         * **Not an entry point.** It carries no check that a number was stated —
         * that is the job of @ref readString and @ref readStringCapped, which is
         * why this is not public: a `(-1, -1)` call is the fail-open shape §6.2.1
         * forbids, and there is deliberately no way to spell it.
         */
        template <typename S>
        bool readStringGated(S &value, long bound, long dynCap) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::String)) return false;      /* §7.3 */
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound))        /* §7.1/§5.2 */
            { error_ = true; return false; }
            /* §6.2.1: consulted only where the schema declares no bound — a
             * receiver limit "MUST NOT be applied to a field the schema already
             * bounds". */
            if (bound < 0 && dynCap >= 0 && fixLen_ > static_cast<size_t>(dynCap))
            { exceedLimit(); return false; }
            /* One delivery path for both storage modes (§6.6.3): the destination
             * is refused when it is shorter than the announced payload, never
             * grown to fit it. Which of the two a destination is — static room or
             * room the caller sized — is a fact @ref detail::destRoom reads off
             * the type, not a policy this read picks. */
            return readPayload(value, /*validateUtf8*/ true);
        }

    public:
        /**
         * @brief Read the current field as a `blob`, or skip it (§7.3).
         *
         * The @ref Fix::Blob counterpart of @ref readString; reads straight into
         * the byte container, with no intermediate `std::string`. Takes a growable
         * `std::vector<uint8_t>` or a heap-free @ref FixedBytes, on the same
         * @ref FixedBytes::set_len test.
         *
         * Refuses in the same three ways, in the same order, as
         * @ref readString — and takes its own §6.2.1 cap the same way. `blob` and
         * `string` are separate limits, so the caller passes
         * `max_dyn_blob_len` here.
         *
         * Split into two entry points on the same terms as @ref readString: this
         * one takes the declared `maxlen`, @ref readBlobCapped takes the receiver
         * cap, and neither has a default.
         *
         * @param[out] value Destination for the decoded bytes.
         * @param bound Declared `maxlen`. **Required and non-negative**; a
         *              schema-unbounded field is read through @ref readBlobCapped.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename B>
        bool readBlob(B &value, long bound) noexcept
        {
            if (bound < 0) { rejectUnbounded(); return false; }
            return readBlobGated(value, bound, -1);
        }

        /**
         * @brief Read the current field as a schema-**unbounded** `blob` under the
         *        receiver's `max_dyn_blob_len` (§6.2.1).
         *
         * The `blob` counterpart of @ref readStringCapped; see it for why the
         * capped read is its own entry point. `blob` and `string` are separate
         * limits, so the caller passes `max_dyn_blob_len` here.
         *
         * @param[out] value Destination for the decoded bytes.
         * @param dynCap The caller's `max_dyn_blob_len`. **Required and
         *               non-negative**; a negative value answers
         *               @ref Error::InvalidArgument.
         * @return `true` when the value was read; `false` when the field was left
         *         for the decoder to skip.
         */
        template <typename B>
        bool readBlobCapped(B &value, long dynCap) noexcept
        {
            if (dynCap < 0) { rejectUnbounded(); return false; }
            return readBlobGated(value, -1, dynCap);
        }

    protected:
        /** @brief The body both `blob` reads share. @copydetails readStringGated */
        template <typename B>
        bool readBlobGated(B &value, long bound, long dynCap) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::Blob)) return false;
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound)) /* §7.1 */
            {
                error_ = true;
                return false;
            }
            if (bound < 0 && dynCap >= 0 && fixLen_ > static_cast<size_t>(dynCap)) /* §6.2.1 */
            {
                exceedLimit();
                return false;
            }
            /* §6.6.3, as in readString — one delivery path, refuse rather than
             * grow. A `blob` is never UTF-8 validated (§6.4). */
            return readPayload(value, /*validateUtf8*/ false);
        }

    public:
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
         * 3. **Configured receiver cap** (@p dynCap, §6.2.1) → `LimitExceeded`.
         *    Deliberately *not* INVALID: the bytes are fine and the same message
         *    decodes under a looser cap.
         * 3b. **The destination's own capacity**, for a destination that publishes
         *    one → `InvalidArgument` (§6.3's third tier, §6.6.3): a count it cannot
         *    hold is refused instead of silently truncated into it (MESSAGE_SPEC
         *    §3), and by this point neither the schema nor the deployment has
         *    anything left to object to — only this caller's storage.
         * 4. **Reset, then fill.** The destination is resized (dynamic container) or
         *    value-initialized (fixed extent) only now, so an occurrence skipped at
         *    step 1 cannot wipe a valid earlier one (§7.4). A destination pre-sized
         *    to a declared `count: N` keeps the element default in the slots past
         *    the wire count `M`, which is the array's length — there is no
         *    fill-to-`N` and no trailing elision to undo (§3).
         * 5. **Declared element width** (@p elem, §7.1) → `INVALID`, per element as
         *    it is decoded. The sibling of the `count` bound at step 2: the same
         *    class of schema fact, arriving through the same call, and the reason it
         *    is applied *here* rather than by the caller — an element cannot be
         *    range-checked after the fact without a wide temporary copy of the whole
         *    array. Unarmed by default, so an omitted bound decodes exactly as before.
         *
         * @param[out] dst        Destination range (fixed extent or resizable).
         * @param schemaCount     Declared `count: N`, or negative when unbounded.
         * Split into two entry points on the same terms as @ref readString: this
         * one takes the declared `count`, @ref readArrayCapped takes the receiver
         * cap, and neither has a default.
         *
         * @param elem            Declared element range (@ref ElemBound), e.g.
         *                        `ElemBound::of<std::uint8_t>()` for `items: u8`.
         *                        Ignored for a float element type, which has no
         *                        narrowing to reject: `fp32`/`fp64` are carried at
         *                        their own width on the wire.
         * @param schemaCount     Declared `count: N`. **Required and non-negative**;
         *                        a schema-unbounded array is read through
         *                        @ref readArrayCapped.
         * @return `true` when the array was read; `false` when it was skipped (§7.3)
         *         or rejected, with the outcome already recorded on the stream.
         */
        template <typename T>
        bool readArray(T &dst, long schemaCount, ElemBound elem = {}) noexcept
        {
            if (schemaCount < 0) { rejectUnbounded(); return false; }
            return readArrayGated(dst, schemaCount, -1, elem);
        }

        /**
         * @brief Read a schema-**unbounded** count-prefixed array under the
         *        receiver's `max_dyn_array_count` (§6.2.1).
         *
         * The counterpart of @ref readArray for an array whose schema declares no
         * `count`. Step 2 of the order above drops out and step 3 decides; the cap
         * is the caller's, used for that one comparison and not retained.
         *
         * @param[out] dst    Destination range (fixed extent or resizable).
         * @param dynCap      The caller's `max_dyn_array_count`. **Required and
         *                    non-negative**; a negative value answers
         *                    @ref Error::InvalidArgument.
         * @param elem        Declared element range (@ref ElemBound).
         * @return `true` when the array was read.
         */
        template <typename T>
        bool readArrayCapped(T &dst, long dynCap, ElemBound elem = {}) noexcept
        {
            if (dynCap < 0) { rejectUnbounded(); return false; }
            return readArrayGated(dst, -1, dynCap, elem);
        }

    protected:
        /** @brief The body both array reads share. @copydetails readStringGated */
        template <typename T>
        bool readArrayGated(T &dst, long schemaCount, long dynCap, ElemBound elem) noexcept
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
            /* §6.2.1: consulted only where the schema declares no `count` — a
             * receiver limit "MUST NOT be applied to a field the schema already
             * bounds", which is the same gate the wrapper-array collectors apply
             * to their own index cap. */
            if (schemaCount < 0 && dynCap >= 0 && count_ > static_cast<size_t>(dynCap))
            {
                exceedLimit();
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
             * The category is §6.3's THIRD tier, and it does not depend on where
             * the ceiling came from: both bounds that speak about the *message*
             * have already spoken by the time this runs — the schema `count` at
             * step 2 and the configured cap at step 3 — so what is left is a
             * destination too short for a value both of them admit. That is
             * `InvalidArgument`: "The third is a mistake in the **call**, not a
             * property of the message or of the deployment, and the other two
             * codes each say something untrue about it." (§6.3). This used to
             * report INVALID under a declared `count` and LimitExceeded without
             * one — two answers, neither of them this one (A2-0022). */
            if constexpr (constexpr long destCap = detail::destCapacity<T>(); destCap >= 0)
            {
                /* A **static** capacity cannot change between chunks, so the
                 * refusal is pulled forward to the header: the destination is
                 * judged before a byte of payload is read, and the verdict cannot
                 * depend on where the chunk boundaries fell. */
                if (count_ > static_cast<size_t>(destCap))
                {
                    rejectDestination();
                    return false;
                }
            }
            else if constexpr (requires { dst.resize(size_t{}); })
            {
                /* Room the caller sized (§6.6.3) — the branch §6.6 is about, since
                 * this is the destination the codec used to grow. It is compared
                 * against what this delivery can actually **reach**: the announced
                 * count, clamped to the elements the bytes in hand could carry (a
                 * varint element is at least one byte, a fixlen one exactly
                 * @ref fixLen_).
                 *
                 * Clamping is what keeps a hostile count from deciding anything: a
                 * declared 2^31 elements whose payload is absent or withheld reaches
                 * nothing, so it neither refuses a sound destination nor asks anyone
                 * to size one — which is also what lets the field-size cap withhold
                 * an over-cap payload while the schema `count` above still gets to
                 * speak (#86). When the payload is complete the reach IS `count_`, so
                 * a destination genuinely too short is refused exactly then, and
                 * refused rather than silently truncated into (MESSAGE_SPEC §3,
                 * issue #81). */
                const uint64_t readable = static_cast<uint64_t>(end_ - p_);
                const uint64_t fillable = type_ == Wire::ArrayFixlen
                                              ? (fixLen_ ? readable / fixLen_ : uint64_t{0})
                                              : readable;
                const size_t reach = static_cast<size_t>(
                    std::min<uint64_t>(static_cast<uint64_t>(count_),
                                       static_cast<uint64_t>(pendDone_) + fillable));
                if (reach > dst.size())
                {
                    rejectDestination();
                    return false;
                }
            }
            /* A destination of fixed extent that publishes neither a capacity nor a
             * resize (`std::array`, a bound span) is @ref read's low-level contract
             * and not this tier's business: it cannot be grown, so §6.6 has nothing
             * to say about it, and the leading elements land in it while the rest is
             * parsed only to stay framed. */
            /* §7.4's reset, and the destination's length, in the one order each
             * kind admits:
             *
             * * **static room** (@ref InlineVector): the storage is already the
             *   caller's in full, so setting the length to `M` is a length store
             *   and nothing is allocated. It happens **before** the fill, because
             *   the span the fill binds is the destination's *current* length;
             * * **fixed extent with no length** (`std::array`, a bound span): put
             *   back to the element default here — the slots past `M` are what
             *   §7.4 would otherwise leave from an earlier message;
             * * **room the caller sized**: the fill writes the `M` elements it was
             *   given room for, and the length is published **after**, as a shrink
             *   (below). Growing it here is exactly what §6.6 forbids. */
            if (pendDone_ == 0)
            {
                /* Only on the FIRST delivery: a field resumed from an earlier chunk
                 * already holds the elements that arrived with it, and resetting
                 * here would throw them away (§6.6.2 — the destination is written
                 * as the pieces arrive, not from scratch each feed). */
                if constexpr (detail::destCapacity<T>() >= 0)      detail::destSetLen(dst, count_);
                else if constexpr (!requires { dst.resize(size_t{}); }) dst = T{};
            }
            const bool ok = [&]() noexcept {
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
            }();
            if (!ok) return false;
            if constexpr (detail::destCapacity<T>() < 0) detail::destSetLen(dst, count_);
            return true;
        }

    public:
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
            const size_t done = pendDone_;
            const size_t avail = static_cast<size_t>(end_ - p_);
            const size_t take = std::min(avail, fixLen_ - done);
            /* The piece that lands inside the caller's buffer is copied; the rest of
             * the payload is stepped over, exactly as the truncating contract above
             * says. Both halves resume, so a blob split across feeds needs no buffer
             * of the decoder's own (§6.6.2). */
            if (done < maxlen && take)
                std::memcpy(static_cast<uint8_t *>(dst) + done, p_,
                            std::min(take, maxlen - done));
            p_ += take;
            if (done + take < fixLen_)
            {
                /* The payload has not fully arrived. That is INCOMPLETE, not an
                 * error: more bytes may complete it, and the field is continued
                 * once they do. Setting error_ here made a truncated blob INVALID
                 * and — because the run is then condemned — unrecoverable even
                 * after the remaining bytes arrived. Matches @ref readBlob and
                 * @ref readString, which guard the identical condition. */
                pendDone_ = done + take;
                incomplete_ = true;
                return 0;
            }
            pendDone_ = 0;
            consumed_ = true;
            return std::min(maxlen, fixLen_);
        }

        /**
         * @return `true` when a read in this delivery consumed the field. `false`
         *         means the field was declined (skipped) or its payload has not
         *         arrived yet, in which case the field is delivered again with the
         *         next chunk and the read continues where it stopped.
         */
        [[nodiscard]] bool consumed() const noexcept { return consumed_; }

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

        /**
         * @brief The payload length the current field's header announces — a
         *        `string`/`blob` byte count, or a fixlen array's **element** width.
         *
         * §6.6.3's "after being told the announced count": the number the caller
         * sizes its destination from before handing it back. Identical to the
         * `size` argument of the deliver callback, reachable from a helper that
         * was not handed it (@ref sofab::readString and friends).
         */
        [[nodiscard]] size_t announcedSize() const noexcept { return fixLen_; }

        /**
         * @brief The element count the current array field's header announces.
         * @copydetails announcedSize
         */
        [[nodiscard]] size_t announcedCount() const noexcept { return count_; }

        /**
         * @brief Bytes of this field readable right now, in the chunk being parsed.
         *
         * What lets a helper sizing a destination clamp an announced count to what
         * the bytes in hand could ever fill, so a hostile count never decides an
         * allocation — the same clamp @ref readArray applies to its own refusal.
         */
        [[nodiscard]] size_t available() const noexcept
        {
            return static_cast<size_t>(end_ - p_);
        }

        /**
         * @brief How much of the current field earlier chunks already delivered —
         *        payload bytes written, or array elements decoded.
         *
         * Zero on a field's first delivery. A field split across `feed` calls is
         * **continued**, not re-parsed (§6.6.2), so a helper sizing the destination
         * has to add what is already in it to what has just arrived; sizing from
         * this chunk alone would ask the codec to fit a value into less room than it
         * has already used.
         */
        [[nodiscard]] size_t progress() const noexcept { return pendDone_; }

        /**
         * @brief `true` when this delivery **re-enters a sequence** an earlier
         *        `feed` left open, rather than announcing a new field.
         *
         * A nested sequence cut by a chunk boundary cannot be resumed from a byte
         * offset — the handler chain that was inside it lives on the C++ stack, and
         * a `feed` returning unwinds it. The decoder therefore replays the field ids
         * of the open levels, so each handler is asked for the same member again and
         * the chain is rebuilt exactly as it was. Nothing about the field is new: the
         * id, the sequence and everything already decoded inside it are the ones the
         * previous chunks produced.
         *
         * A handler that only dispatches on the id — every generated `deserialize`,
         * and any handler written the same way — needs this for nothing: reading the
         * same member again is precisely right, and the codec continues where it
         * stopped. It exists for a handler that keeps a **position** of its own
         * rather than a schema, which must not advance it twice.
         *
         * Distinct from @ref progress, which is about the bytes of one field.
         */
        [[nodiscard]] bool resumed() const noexcept { return replay_; }

        /**
         * @brief The §7.3 and §7.1 decisions @ref readString makes, taken **before
         *        a destination exists**.
         *
         * For a caller that has to place an element before it can offer a
         * destination — @ref StringSeq puts an element at its index, and growing
         * the container for a field that turns out not to be an element would
         * change the array's length (§5.1/§7.4). It answers the same two questions
         * @ref readString answers first, in the same order and through the same
         * seam, so the skip is counted exactly once: the tag (§7.3) and then the
         * declared `maxlen` (§7.1).
         *
         * It takes the §6.2.1 receiver cap on the same terms as @ref readString —
         * passed in, never held, and consulted only where @p bound is negative —
         * so the two seams cannot answer differently.
         *
         * It splits into a bounded and a capped form for the same reason
         * @ref readString does, and refuses a call that states neither the same
         * way (@ref rejectUnbounded).
         *
         * @param bound Declared `maxlen`. **Required and non-negative**;
         *              @ref acceptsStringCapped is the schema-unbounded form.
         * @return `true` when a destination may be placed and handed to
         *         @ref readString.
         */
        [[nodiscard]] bool acceptsString(long bound) noexcept
        {
            if (bound < 0) { rejectUnbounded(); return false; }
            return acceptsStringGated(bound, -1);
        }

        /**
         * @brief @ref acceptsString for a schema-unbounded element, under the
         *        receiver's `max_dyn_string_len` (§6.2.1).
         * @param dynCap The caller's cap. **Required and non-negative.**
         */
        [[nodiscard]] bool acceptsStringCapped(long dynCap) noexcept
        {
            if (dynCap < 0) { rejectUnbounded(); return false; }
            return acceptsStringGated(-1, dynCap);
        }

    protected:
        /** @brief The body both `string` accepts share. */
        [[nodiscard]] bool acceptsStringGated(long bound, long dynCap) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::String)) return false; /* §7.3 */
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound))   /* §7.1 */
            { error_ = true; return false; }
            if (bound < 0 && dynCap >= 0 && fixLen_ > static_cast<size_t>(dynCap)) /* §6.2.1 */
            { exceedLimit(); return false; }
            return true;
        }

    public:
        /** @copydoc acceptsString */
        [[nodiscard]] bool acceptsBlob(long bound) noexcept
        {
            if (bound < 0) { rejectUnbounded(); return false; }
            return acceptsBlobGated(bound, -1);
        }

        /** @copydoc acceptsStringCapped */
        [[nodiscard]] bool acceptsBlobCapped(long dynCap) noexcept
        {
            if (dynCap < 0) { rejectUnbounded(); return false; }
            return acceptsBlobGated(-1, dynCap);
        }

    protected:
        /** @brief The body both `blob` accepts share. */
        [[nodiscard]] bool acceptsBlobGated(long bound, long dynCap) noexcept
        {
            if (!tagMatches(Wire::Fixlen, Fix::Blob)) return false; /* §7.3 */
            if (bound >= 0 && fixLen_ > static_cast<size_t>(bound)) /* §7.1 */
            { error_ = true; return false; }
            if (bound < 0 && dynCap >= 0 && fixLen_ > static_cast<size_t>(dynCap)) /* §6.2.1 */
            { exceedLimit(); return false; }
            return true;
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
         * @brief Construct with the per-field callback and the receiver's decode
         *        limits.
         * @param callback Invoked for each complete top-level field.
         * @param limits The receiver's field-span budget (see @ref Limits).
         *        **Required**: §6.2.1 gives this library no number to default to.
         */
        explicit IStreamInline(fieldCallback callback, Limits limits) noexcept
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
         * @param limits The receiver's field-span budget (see @ref Limits).
         *        **Required**: §6.2.1 gives this library no number to default to.
         */
        explicit IStreamObject(Limits limits) noexcept
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
    /* The static helper layer (§6.6.1)                                       */
    /*                                                                        */
    /* Everything from here to the end of the file is the HELPER layer, not    */
    /* the codec. CORELIB_PLAN §6.6.1 draws the line by ownership: "A helper   */
    /* is part of the corelib -- it ships in the repository and is built with  */
    /* it -- but the codec never uses one directly. Either the caller invokes  */
    /* it, or it is reached from inside a callback the codec made. It may      */
    /* allocate, because whatever it takes belongs to whoever called it."      */
    /*                                                                        */
    /* Nothing in IStreamImpl above calls anything below. These functions are  */
    /* what the GENERATED layer calls instead of the codec's own read*: they   */
    /* size the destination each field lands in -- "the generated object knows */
    /* the schema, sizes and owns the storage each field lands in, then drives */
    /* the codec over it like any other caller" (§6.6.1) -- and then hand it   */
    /* to the codec, which refuses a destination too short rather than growing */
    /* one (§6.6.3).                                                          */
    /* ---------------------------------------------------------------------- */

    namespace detail
    {
        /**
         * @brief Give @p dst room for @p want, if it is a destination the caller
         *        has to size.
         *
         * Helper layer: this is the allocator call §6.6 moved out of the codec, and
         * it is here rather than there because the storage belongs to whoever
         * called this. A destination with **static room** (`T::capacity()`) needs
         * nothing — it already holds what it can ever hold — and one of fixed
         * extent with no `resize` cannot be sized at all.
         *
         * Only ever grows: a destination the caller deliberately sized larger keeps
         * its room, and the decoded length is published by
         * @ref sofab::detail::destSetLen when the value is in.
         *
         * @param dst  Destination.
         * @param want Room the announced value needs, already clamped by the caller
         *             to what the bytes in hand could fill.
         */
        /**
         * @brief Is @p D a destination the **caller** has to size?
         *
         * True only for heap-growable storage: it can be resized and publishes no
         * static capacity. A @ref sofab::FixedString / @ref sofab::FixedBytes /
         * @ref sofab::InlineVector
         * already holds everything it can hold, and a `std::array` or a bound span
         * cannot be sized at all — for those the whole helper below compiles away
         * and generated code pays nothing for calling it.
         */
        template <typename D>
        concept sizable = requires(D &d) { d.resize(size_t{}); } && !requires { D::capacity(); };

        template <typename D>
        void fitDest(D &dst, size_t want) noexcept
        {
            if constexpr (requires { D::capacity(); }) { (void)dst; (void)want; }
            else if constexpr (requires { dst.resize(size_t{}); })
            {
                if (dst.size() < want) dst.resize(want);
            }
        }

        /**
         * @brief Elements of the announced array the bytes in hand could still
         *        carry, clamped to the count itself.
         *
         * The same clamp @ref IStreamImpl::readArray applies to its own refusal, so
         * the helper never sizes a destination the codec would then refuse, and an
         * announced 2^31 whose payload is absent or withheld sizes nothing.
         */
        [[nodiscard]] inline size_t arrayReach(const IStreamImpl &is) noexcept
        {
            const uint64_t readable = static_cast<uint64_t>(is.available());
            const uint64_t fillable = is.wire() == Wire::ArrayFixlen
                                          ? (is.announcedSize()
                                                 ? readable / is.announcedSize()
                                                 : uint64_t{0})
                                          : readable;
            return static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(is.announcedCount()),
                                   static_cast<uint64_t>(is.progress()) + fillable));
        }

        /** @brief Does the delivered tag match the array kind @p Elem declares (§7.3)? */
        template <typename Elem>
        [[nodiscard]] inline bool arrayTagMatches(const IStreamImpl &is) noexcept
        {
            if constexpr (std::is_same_v<Elem, float>)
                return is.wire() == Wire::ArrayFixlen && is.fixType() == Fix::Fp32;
            else if constexpr (std::is_same_v<Elem, double>)
                return is.wire() == Wire::ArrayFixlen && is.fixType() == Fix::Fp64;
            else if constexpr (std::is_unsigned_v<Elem>)
                return is.wire() == Wire::ArrayUnsigned;
            else
                return is.wire() == Wire::ArraySigned;
        }
    } // namespace detail

    /**
     * @brief Read the current field as a `string` into a destination this call
     *        sizes — the generated layer's @ref IStreamImpl::readString.
     *
     * Sizes @p dst for the announced payload and then hands it to the codec, which
     * refuses a destination too short rather than growing one (§6.6.3). For a
     * destination with static room (@ref FixedString) it adds nothing and compiles
     * away; for a `std::string` it is the one place the allocation happens.
     *
     * The sizing runs only once **every** gate @ref IStreamImpl::readString applies
     * has admitted the field — the tag (§7.3), the declared `maxlen` (§7.1) **and**
     * the configured receiver cap @p dynCap (§6.2.1) — so a mistyped, an over-long
     * or an over-cap occurrence leaves @p dst exactly as it was (§7.4). Sizing on
     * a subset of them would defeat the point of a receiver cap: §6.2.1 puts the
     * check "before the allocation it is meant to prevent", and this call *is* that
     * allocation. A schema-unbounded field sized here from the wire-announced
     * length is precisely the sender dictating the receiver's allocation.
     *
     * @par No number can be left out
     * This helper holds no limit and invents none (§6.2.1: a codec "MUST NOT supply
     * a default for one it was not given, MUST NOT read an omitted argument as
     * *unlimited*"), so it does not offer an argument that may be omitted at all.
     * There are two entry points, one per bound that can apply, neither defaulted:
     * ```cpp
     * sofab::readString(is, name, 32);                            // maxlen: 32
     * sofab::readStringCapped(is, name, SOFAB_MAX_DYN_STRING_LEN); // no maxlen
     * ```
     * Sizing @p dst from the wire with neither number stated is precisely the
     * sender dictating the receiver's allocation, so a negative one is refused
     * with @ref Error::InvalidArgument rather than obeyed.
     *
     * @param is Stream delivering the field.
     * @param dst Destination for the decoded text.
     * @param maxlen Declared `maxlen`. **Required and non-negative.**
     * @return `true` when the value was read.
     */
    template <typename S>
    bool readString(IStreamImpl &is, S &dst, long maxlen) noexcept
    {
        if constexpr (detail::sizable<S>)
        if (maxlen >= 0 &&
            is.wire() == detail::Wire::Fixlen && is.fixType() == detail::Fix::String &&
            is.announcedSize() <= static_cast<size_t>(maxlen))
            detail::fitDest(dst, std::min(is.announcedSize(),
                                          is.progress() + is.available()));
        return is.readString(dst, maxlen);
    }

    /**
     * @brief The schema-unbounded @ref sofab::readString: sizes @p dst only once
     *        the §6.2.1 receiver cap has admitted the field.
     * @param is Stream delivering the field.
     * @param dst Destination for the decoded text.
     * @param dynCap The caller's `max_dyn_string_len`. **Required and
     *               non-negative.**
     */
    template <typename S>
    bool readStringCapped(IStreamImpl &is, S &dst, long dynCap) noexcept
    {
        if constexpr (detail::sizable<S>)
        if (dynCap >= 0 &&
            is.wire() == detail::Wire::Fixlen && is.fixType() == detail::Fix::String &&
            is.announcedSize() <= static_cast<size_t>(dynCap))
            detail::fitDest(dst, std::min(is.announcedSize(),
                                          is.progress() + is.available()));
        return is.readStringCapped(dst, dynCap);
    }

    /**
     * @brief The `blob` counterpart of @ref sofab::readString.
     * @copydetails sofab::readString
     */
    template <typename B>
    bool readBlob(IStreamImpl &is, B &dst, long maxlen) noexcept
    {
        if constexpr (detail::sizable<B>)
        if (maxlen >= 0 &&
            is.wire() == detail::Wire::Fixlen && is.fixType() == detail::Fix::Blob &&
            is.announcedSize() <= static_cast<size_t>(maxlen))
            detail::fitDest(dst, std::min(is.announcedSize(),
                                          is.progress() + is.available()));
        return is.readBlob(dst, maxlen);
    }

    /**
     * @brief The `blob` counterpart of @ref sofab::readStringCapped.
     * @copydetails sofab::readStringCapped
     */
    template <typename B>
    bool readBlobCapped(IStreamImpl &is, B &dst, long dynCap) noexcept
    {
        if constexpr (detail::sizable<B>)
        if (dynCap >= 0 &&
            is.wire() == detail::Wire::Fixlen && is.fixType() == detail::Fix::Blob &&
            is.announcedSize() <= static_cast<size_t>(dynCap))
            detail::fitDest(dst, std::min(is.announcedSize(),
                                          is.progress() + is.available()));
        return is.readBlobCapped(dst, dynCap);
    }

    /**
     * @brief Read the current count-prefixed array into a destination this call
     *        sizes — the generated layer's @ref IStreamImpl::readArray.
     *
     * Sizes @p dst for the announced element count, clamped to what the bytes in
     * hand could fill, and only once the tag (§7.3), the schema `count` (§7.1) and
     * the configured receiver cap (§6.2.1) all admit the field — so neither a
     * mistyped occurrence nor a hostile count reaches an allocator. The codec then
     * fills what it was given room for and refuses a destination shorter than the
     * array actually is (§6.6.3).
     *
     * Two entry points, neither defaulted, exactly as for @ref sofab::readString.
     *
     * @param is Stream delivering the field.
     * @param dst Destination range.
     * @param schemaCount Declared `count: N`. **Required and non-negative.**
     * @param elem Declared element range (@ref ElemBound).
     * @return `true` when the array was read.
     */
    template <typename T>
    bool readArray(IStreamImpl &is, T &dst, long schemaCount, ElemBound elem = {}) noexcept
    {
        using Elem = typename T::value_type;
        if constexpr (detail::sizable<T>)
        {
            const size_t n = is.announcedCount();
            if (schemaCount >= 0 && detail::arrayTagMatches<Elem>(is) &&
                n <= static_cast<size_t>(schemaCount))
                detail::fitDest(dst, detail::arrayReach(is));
        }
        return is.readArray(dst, schemaCount, elem);
    }

    /**
     * @brief The schema-unbounded @ref sofab::readArray: sizes @p dst only once the
     *        §6.2.1 receiver cap has admitted the count.
     * @param is Stream delivering the field.
     * @param dst Destination range.
     * @param dynCap The caller's `max_dyn_array_count`. **Required and
     *               non-negative.**
     * @param elem Declared element range (@ref ElemBound).
     */
    template <typename T>
    bool readArrayCapped(IStreamImpl &is, T &dst, long dynCap, ElemBound elem = {}) noexcept
    {
        using Elem = typename T::value_type;
        if constexpr (detail::sizable<T>)
        {
            const size_t n = is.announcedCount();
            if (dynCap >= 0 && detail::arrayTagMatches<Elem>(is) &&
                n <= static_cast<size_t>(dynCap))
                detail::fitDest(dst, detail::arrayReach(is));
        }
        return is.readArrayCapped(dst, dynCap, elem);
    }

    /**
     * @brief @ref IStreamImpl::read with the destination sized first — the
     *        generated layer's untyped read.
     *
     * Dispatches like @ref IStreamImpl::read and adds nothing for a scalar or a
     * sub-message, whose destinations hold no room to size. A `std::string` payload
     * destination and a contiguous element range are sized here, so the codec never
     * has to grow one.
     */
    template <typename T>
    bool read(IStreamImpl &is, T &dst) noexcept
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            if (is.wire() == detail::Wire::Fixlen &&
                (is.fixType() == detail::Fix::String || is.fixType() == detail::Fix::Blob))
                detail::fitDest(dst, std::min(is.announcedSize(),
                                              is.progress() + is.available()));
        }
        else if constexpr (requires(T &d) {
                               typename T::value_type;
                               std::span{d};
                               requires !std::is_const_v<typename T::value_type>;
                               requires !InputMessage<T>;
                           })
        {
            if constexpr (detail::sizable<T>)
                if (detail::arrayTagMatches<typename T::value_type>(is))
                    detail::fitDest(dst, detail::arrayReach(is));
        }
        return is.read(dst);
    }

    /* ---------------------------------------------------------------------- */
    /* Wrapper-sequence collectors and encode helpers                         */
    /* ---------------------------------------------------------------------- */

    namespace detail
    {
        /**
         * @brief Admit or refuse a wrapper-array element **index**, in §6.3's
         *        categories, before the container it indexes into is extended.
         *
         * MESSAGE_SPEC §5.1 makes the array's length *highest present id + 1*, so
         * the index **is** the length: "for a **sequence array** it surfaces the
         * **index** of the element in hand … there being no count header to
         * check", and "A limit **MUST** be enforced … before the container it
         * indexes into is extended" (§6.2.1).
         *
         * The three bounds do not share a verdict, so they are consulted in
         * §6.3's order. A schema `count` shuts the receiver cap out entirely —
         * a receiver limit "**MUST NOT** be applied to a field the schema already
         * bounds" (§6.2.1).
         *
         * The stream applies the same three bounds one step earlier, at the
         * element header (@ref IStreamImpl::rejectElementIndex), which is where a
         * truncated element cannot outrun them. This is the second gate, for a
         * `deserialize` reached directly — it is a public entry point — and for a
         * collector that publishes no element type for the stream to key on.
         *
         * @param is       Stream to record the refusal on.
         * @param id       The element index in hand.
         * @param cap      Schema `count` N, or negative.
         * @param dynCap   The caller's `max_dyn_array_count` (§6.2.1), or
         *                 negative when the caller supplied none — in which case
         *                 **no** index cap applies, because there is none to fall
         *                 back on.
         * @param destCap  The destination container's capacity, or negative.
         * @return `true` when the index may be placed.
         *
         * @par A collector that states no bound at all is refused, not obeyed
         * A wrapper array's length is its highest index, so an unbounded array
         * whose collector carries neither a schema `count` nor a receiver cap grows
         * to whatever index the sender picks — the amplification §6.2.1 exists to
         * stop, from a ~9-byte field. There is nothing to fall back on (§6.2.1
         * forbids this codec a limit of its own), so the call is refused with
         * @ref Error::InvalidArgument. A destination that **cannot grow** is the
         * one exemption: its capacity already bounds the allocation, so no cap is
         * needed for the sender not to choose it.
         */
        inline bool seqIndexAdmitted(IStreamImpl &is, sofab::id id, long cap, long dynCap,
                                     long destCap) noexcept
        {
            const long i = static_cast<long>(id);
            const long dyn = dynCap; /* no stream-wide fallback (§6.2.1) */
            if (cap < 0 && dyn < 0 && destCap < 0)
            {
                is.rejectUnbounded(); /* §6.3 — a mistake in the call */
                return false;
            }
            if (cap >= 0)
            {
                if (i >= cap) { is.invalidate(); return false; } /* §7.1 */
            }
            else if (dyn >= 0 && i >= dyn)
            {
                is.exceedLimit(); /* §6.2.1 — policy, never INVALID */
                return false;
            }
            if (destCap >= 0 && i >= destCap)
            {
                is.rejectDestination(); /* §6.3's third tier */
                return false;
            }
            return true;
        }
    } // namespace detail

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
         * @brief The configured receiver cap on the element **index**
         *        (`max_dyn_array_count`, §6.2.1); negative means none.
         *
         * A wrapper array carries no count header, so there is no count word for a
         * cap to bind: "for a **sequence array** it surfaces the **index** of the
         * element in hand — a wrapper array's length is *highest present id + 1*
         * (MESSAGE_SPEC §5.1), so the index is what has to be checked, there being
         * no count header to check." An id at or past this is
         * @ref Error::LimitExceeded, decided **before** @ref out is extended, so an
         * announced index never becomes an allocation.
         *
         * Deliberately **not** @ref StringSeq::cap — `cap` is the schema `count` and its
         * breach is `InvalidMessage` — the bytes contradict the schema both peers
         * agreed on — while this is a *policy* rejection the same bytes survive
         * under a looser cap, and §6.2.1 forbids folding the two. It is also
         * ignored while `cap` is armed, since a receiver limit "MUST NOT be
         * applied to a field the schema already bounds". The number itself is
         * generated code's to supply (§6.2.1: "The numbers and the allocation are
         * not the codec's"). There is **no default**: the constructor takes it,
         * and a collector built without one does not compile. (corelib-cpp#124)
         */
        long dynCap;

        /**
         * @brief The §6.2.1 receiver cap on one ELEMENT's length, or -1 when the
         *        caller supplied none.
         *
         * @ref dynCap bounds how many elements the array may have; this bounds how
         * long each may be. Both are needed: an array of two elements a gigabyte
         * each is under any index cap. It is consulted only where @ref emax is -1
         * — a receiver limit "MUST NOT be applied to a field the schema already
         * bounds" — and, like every cap here, it is passed in and never held.
         * There is no default: a negative value means the caller stated no cap,
         * and an element read under one answers @ref Error::InvalidArgument
         * rather than being read unbounded.
         */
        long elemDynCap;

        /**
         * @brief The destination's own capacity, or -1 for a growable container.
         *
         * §6.3's third tier: an index the container cannot hold, with neither the
         * schema nor the deployment objecting, is @ref Error::InvalidArgument. For
         * a fixed-capacity container this is also what keeps the placement loop
         * below terminating — @ref InlineVector::emplace_back reuses the last slot
         * once full, so `out.size()` would never pass a large id.
         */
        static constexpr long elemDestCap = detail::destCapacity<C>();

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
         * @param indexCap The caller's `max_dyn_array_count` (@ref dynCap), or -1
         *                 when none was supplied — there is no fallback, so no
         *                 index cap then applies. An element id at or past it is
         *                 @ref Error::LimitExceeded, decided before the container
         *                 grows.
         * @param elemLenCap The caller's `max_dyn_string_len` / `max_dyn_blob_len`
         *                 for one element (@ref elemDynCap), or -1 when none was
         *                 supplied. Consulted only where @p elemMax is -1.
         */
        explicit StringSeq(C &o, long capacity, long elemMax,
                           long indexCap, long elemLenCap) noexcept
            : out(o), cap(capacity), emax(elemMax), dynCap(indexCap),
              elemDynCap(elemLenCap) {}

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
            /* Decide first, place second, read into the placed element last.
             *
             * A §7.3-skipped or §7.1-rejected element must leave the destination
             * untouched — growing the container for a field that is not an element
             * would change the array's length (§5.1, highest present id + 1) — so
             * both decisions are taken through @ref IStreamImpl::acceptsString,
             * before there is a destination at all. It is the same seam
             * @ref IStreamImpl::readString takes them at, in the same order, so the
             * §7.3 skip is counted exactly once.
             *
             * The element is then read **in place** rather than into a temporary
             * that is moved in. A payload split across chunks is written into the
             * caller's destination piece by piece (§6.6.2), and a temporary would
             * not survive the `feed` that carries the first half. */
            /* The element's own bound picks the entry point, exactly as it does
             * for a scalar field: the declared `maxlen` where the schema stated
             * one, the receiver cap where it did not. Never both (§6.2.1). */
            if (emax >= 0 ? !is.acceptsString(emax) : !is.acceptsStringCapped(elemDynCap))
                return;
            if (!detail::seqIndexAdmitted(is, id, cap, dynCap, elemDestCap)) return;
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            auto &slot = out[static_cast<size_t>(id)];
            if (emax >= 0) (void)sofab::readString(is, slot, emax);
            else           (void)sofab::readStringCapped(is, slot, elemDynCap);
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
        /** @copydoc StringSeq::dynCap */
        long dynCap;
        /** @copydoc StringSeq::elemDestCap */
        static constexpr long elemDestCap = detail::destCapacity<C>();

        /** @copydoc StringSeq::elemWire */
        static constexpr int elemWire = static_cast<int>(detail::Wire::Fixlen);
        /** @copydoc StringSeq::elemWire */
        static constexpr int elemFix = static_cast<int>(detail::Fix::Blob);

        /** @copydoc StringSeq::elemDynCap */
        long elemDynCap;

        /** @copydoc StringSeq::StringSeq */
        explicit BlobSeq(C &o, long capacity, long elemMax,
                         long indexCap, long elemLenCap) noexcept
            : out(o), cap(capacity), emax(elemMax), dynCap(indexCap),
              elemDynCap(elemLenCap) {}

        /** @copydoc StringSeq::prepare */
        void prepare() noexcept { out.clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t size, size_t) noexcept override
        {
            (void)size;
            /* Decide, place, read in place -- see StringSeq::deserialize. */
            /* @copydetails StringSeq::deserialize -- the element's own bound picks
             * the entry point. */
            if (emax >= 0 ? !is.acceptsBlob(emax) : !is.acceptsBlobCapped(elemDynCap))
                return; /* §7.3 + §7.1 + §6.2.1 */
            if (!detail::seqIndexAdmitted(is, id, cap, dynCap, elemDestCap)) return;
            while (out.size() <= static_cast<size_t>(id)) out.emplace_back();
            auto &slot = out[static_cast<size_t>(id)];
            if (emax >= 0) (void)sofab::readBlob(is, slot, emax);
            else           (void)sofab::readBlobCapped(is, slot, elemDynCap);
        }
    };

    namespace detail
    {
        /**
         * @brief Whether a @ref MessageSeq template argument names the destination
         *        **container** rather than the element type.
         *
         * @ref MessageSeq is templated on the container, exactly as @ref StringSeq
         * is, which is what lets it collect into an @ref InlineVector as well as a
         * @c std::vector. It used to be templated on the ELEMENT, with
         * `std::vector<T>` hard-wired as the destination, and that spelling keeps
         * working: the two readings are told apart here, from the argument alone.
         *
         * An argument names a container when its `value_type` is something this
         * collector reads as an element — a message, or a row container. The two
         * readings can only collide for an argument whose `value_type` is itself
         * one of those two, i.e. an element that is a sequence of messages or a row
         * of rows; under the element reading such an element is handed to
         * @ref IStreamImpl::read, which has no overload for it, so what the
         * container reading takes over is code that never compiled.
         */
        template <typename A>
        constexpr bool namesSeqContainer() noexcept
        {
            if constexpr (requires { typename A::value_type; })
            {
                using V = typename A::value_type;
                return std::is_base_of_v<IStreamMessage, V> || requires { typename V::value_type; };
            }
            else
            {
                return false;
            }
        }
    } // namespace detail

    /**
     * @brief Collects a struct/union or nested-array wrapper sequence into the
     *        container it is handed.
     *
     * One element is placed and read per child: @ref IStreamImpl::read descends
     * into a struct/union element's own sub-sequence, and a native-scalar row goes
     * through @ref IStreamImpl::readArray — exactly as a top-level field of that
     * type would be read.
     *
     * The target is held by pointer rather than a bound reference so one instance
     * can serve several fields.
     *
     * @tparam A Destination **container**, as @ref StringSeq takes it:
     *           `std::vector<T>` for the growable storage mode,
     *           `InlineVector<T, N>` for the heap-free one; its `value_type` is the
     *           element type. The older ELEMENT spelling `MessageSeq<T>`, which
     *           always collected into a `std::vector<T>`, still compiles — see
     *           @ref detail::namesSeqContainer for how the two are told apart.
     */
    template <typename A>
    struct MessageSeq : IStreamMessage
    {
        /** Destination container: @p A itself, or `std::vector<A>` for the element spelling. */
        using Container = std::conditional_t<detail::namesSeqContainer<A>(), A, std::vector<A>>;
        /** Element type — an @ref IStreamMessage, or a container for a nested-array row. */
        using Elem = typename Container::value_type;

        Container *out = nullptr;  /**< Destination; a null one collects nothing. */

        /**
         * @brief Schema `count` N, or -1; an id at or past N is
         *        @ref Error::InvalidMessage (§5.1/§7.1).
         *
         * It used to default to the destination's own capacity, which bounded a
         * heap-free container but reported the wrong category for it: a
         * destination too short is neither a malformed message nor a configured
         * limit. That bound now lives in @ref elemDestCap, in §6.3's third
         * category, and this member carries only what the **schema** said.
         */
        long cap = -1;
        /**
         * @copydoc StringSeq::dynCap
         *
         * @note This collector is an **aggregate** — callers fill it field by
         *       field — so a member cannot be made mandatory the way
         *       @ref StringSeq's constructor argument is. The sentinel is
         *       therefore still spellable here, and it is **diagnosed** rather
         *       than obeyed: an array with neither a schema `cap` nor a `dynCap`
         *       nor a fixed-capacity destination is refused with
         *       @ref Error::InvalidArgument at its first element
         *       (@ref detail::seqIndexAdmitted). "No cap stated" is never read as
         *       "unlimited" (§6.2.1).
         */
        long dynCap = -1;
        /** @copydoc StringSeq::elemDestCap */
        static constexpr long elemDestCap = detail::destCapacity<Container>();

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
            if constexpr (std::is_base_of_v<IStreamMessage, Elem>)
                return static_cast<int>(detail::Wire::SequenceStart);
            else if constexpr (requires { typename Elem::value_type; })
            {
                using RowElem = typename Elem::value_type;
                if constexpr (std::is_integral_v<RowElem> && !std::is_same_v<RowElem, bool>)
                    return static_cast<int>(std::is_unsigned_v<RowElem> ? detail::Wire::ArrayUnsigned
                                                                       : detail::Wire::ArraySigned);
                else if constexpr (std::is_same_v<RowElem, float> || std::is_same_v<RowElem, double>)
                    return static_cast<int>(detail::Wire::ArrayFixlen);
                else
                    return -1;
            }
            else
                return -1;
        }();

        /** §7.4 replace-whole, and absent ⇒ never called: @copydoc StringSeq::prepare */
        void prepare() noexcept { if (out) out->clear(); }

        void deserialize(IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
        {
            if (!out) return;
            /* §7.3 first, in the same order the stream uses, so the two entry points
             * cannot disagree: an element whose wire type contradicts @ref elemWire
             * is not this array's element at all. It is skipped like an unknown id,
             * which means the destination has to be left exactly as it was -- so the
             * decision comes before the placement below -- and an id that is not an
             * index cannot breach the index bound either. Coming through
             * @ref IStreamImpl::read the same test has already run one step earlier
             * whenever this collector publishes a bound; it is kept here because
             * `deserialize` is public and an unbounded array publishes none. */
            if constexpr (elemWire >= 0)
            {
                if (static_cast<int>(is.wire()) != elemWire) return; /* §7.3 */
            }
            /* §5.1/§6.2.1 over-index reject, decided from the id alone and before
             * the container grows, which is also what keeps an announced index from
             * becoming an allocation — and, for a fixed-capacity container, what
             * keeps the gap fill below terminating at all, its `emplace_back`
             * stopping at the capacity so `out->size()` would never reach the id.
             * Three bounds, three categories (§6.3). */
            if (!detail::seqIndexAdmitted(is, id, cap, dynCap, elemDestCap)) return;
            /* §5.1: the element id IS the array index, so an element is PLACED at
             * `dest[id]`, never appended. The ids may contain gaps -- a decoder
             * MUST accept them and recover a dynamic array's length as *highest
             * present id + 1*, leaving every absent id at the element default.
             * Appending instead would silently SHORTEN the array by the size of
             * the gap: wire `06 0005 07 16 0009 07` (elements at id 0 and id 2,
             * id 1 absent) is the 3-element array `[5, 0, 9]`, not `[5, 9]`.
             * Same placement rule as @ref StringSeq / @ref BlobSeq. */
            while (out->size() <= static_cast<size_t>(id)) (void)out->emplace_back();
            Elem &row = (*out)[static_cast<size_t>(id)];
            if constexpr (std::is_base_of_v<IStreamMessage, Elem>)
            {
                is.read(row); /* the element's own sub-sequence */
            }
            else if constexpr (requires { row.resize(size_t{}); })
            {
                /* A native-scalar row is sized by the wire count, and
                 * @ref IStreamImpl::readArray is what owns that: it settles the
                 * row's array tag, refuses a count the row cannot hold instead of
                 * truncating into it (§3), and grows only as far as the bytes in
                 * hand could ever fill -- so an announced count of 2^31 never
                 * becomes an allocation. A bare `resize(count)` here got all three
                 * wrong. A heap-free row's capacity IS the schema `count` it was
                 * generated for, so it is passed as one and a row past it is the
                 * INVALID of §7.1 rather than a receiver-side LimitExceeded. */
                if constexpr (detail::destCapacity<Elem>() >= 0)
                    (void)sofab::readArray(is, row, detail::destCapacity<Elem>());
                else
                    /* A GROWABLE row has no capacity of its own to be bounded by,
                     * and the schema `count` of the inner array never reaches this
                     * collector (corelib-cpp#124). What is left is the receiver's
                     * own `max_dyn_array_count` -- @ref dynCap -- which is the
                     * right number for a row the schema left unbounded and the
                     * only one this collector holds. It is required, like every
                     * other cap here: a row read with none answers
                     * @ref Error::InvalidArgument rather than letting the wire
                     * count decide how long the row is. */
                    (void)sofab::readArrayCapped(is, row, dynCap);
            }
            else
            {
                /* A fixed-extent row (`std::array`, a bound span) publishes no
                 * resize: read()'s low-level contract fills what it holds. */
                is.read(row);
            }
        }
    };

    /* No encode-side trailing-default trim helper lives here, deliberately.
     *
     * A `trimTail` used to, narrowing a container to its non-default prefix
     * before encode — the fill-to-`N` reading of MESSAGE_SPEC §3 that
     * count-is-capacity superseded. Under the rule as it stands, a `count: N` is
     * a capacity and the wire count `M` IS the array's length, so §3 states the
     * opposite: "A default-valued element stays on the wire, trailing ones
     * included — `M` is the length, so eliding one would shorten the array:
     * `[1, 2, 3, 0, 0]` and `[1, 2, 3]` are different values and encode
     * differently (`M = 5` and `M = 3`)." Trimming was therefore not a size
     * optimization but silent data loss, and @ref OStreamImpl::write emitting the
     * whole container it is handed is exactly right. The shared vector
     * `array_unsigned_trailing_defaults` and trailingDefaultsStayOnTheWire() in
     * test/test_roundtrip.cpp pin this; the latter also fails if such a helper is
     * ever reintroduced into this namespace. */

} // namespace sofab

/** @} */ // end of defgroup

#endif // SOFAB_HPP
