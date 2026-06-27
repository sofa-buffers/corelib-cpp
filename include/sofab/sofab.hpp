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
    inline constexpr int API_VERSION = 1;

    template <typename>
    inline constexpr bool always_false_v = false;

    enum class Error
    {
        None = 0,
        UsageError = 2,
        BufferFull = 3,
        InvalidArgument = 1,
        InvalidMessage = 4,
    };

    using id = uint32_t;

    /* ---------------------------------------------------------------------- */
    /* wire-format primitives                                                 */
    /* ---------------------------------------------------------------------- */

    namespace detail
    {
        enum class Wire : uint8_t
        {
            Unsigned = 0, Signed = 1, Fixlen = 2,
            ArrayUnsigned = 3, ArraySigned = 4, ArrayFixlen = 5,
            SequenceStart = 6, SequenceEnd = 7,
        };

        enum class Fix : uint8_t { Fp32 = 0, Fp64 = 1, String = 2, Blob = 3 };

        inline constexpr uint32_t kIdMax = 0x7fffffffu; /* INT32_MAX */

        constexpr uint64_t zigzagEncode(int64_t v) noexcept
        {
            return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
        }
        constexpr int64_t zigzagDecode(uint64_t u) noexcept
        {
            return static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
        }

        /* Little-endian store/load of the raw bits of a float/double. */
        template <std::floating_point F>
        constexpr auto floatBits(F v) noexcept
        {
            if constexpr (sizeof(F) == 4) return std::bit_cast<uint32_t>(v);
            else                          return std::bit_cast<uint64_t>(v);
        }
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

    class OStreamImpl
    {
    public:
        using flushCallback = std::function<void(std::span<const uint8_t>)>;

    protected:
        uint8_t *buffer_ = nullptr;   /* start of the active buffer */
        uint8_t *cursor_ = nullptr;   /* current write position */
        uint8_t *end_ = nullptr;      /* one past the buffer */
        flushCallback flushCallback_;

        OStreamImpl() noexcept = default;

        void initBuffer(uint8_t *buffer, size_t buflen, size_t offset) noexcept
        {
            buffer_ = buffer;
            cursor_ = buffer + offset;
            end_ = buffer + buflen;
        }

        /* Encode a varint into a caller stack buffer; returns bytes written. */
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

        /* Bulk write: a single memcpy when the payload fits the current buffer
         * (the common case); only crosses flush boundaries byte-by-byte. */
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

        [[nodiscard]] Error putVarint(uint64_t v) noexcept
        {
            uint8_t tmp[10];
            return pushBytes(tmp, encodeVarint(tmp, v));
        }

        [[nodiscard]] Error putHeader(sofab::id fieldId, detail::Wire type) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            return putVarint((static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(type));
        }

        /* header varint + one value varint emitted as a single bulk write */
        [[nodiscard]] Error writeScalar(sofab::id fieldId, detail::Wire type, uint64_t value) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(type));
            n += encodeVarint(tmp + n, value);
            return pushBytes(tmp, n);
        }

        [[nodiscard]] Error writeFixlen(sofab::id fieldId, detail::Fix ft, const uint8_t *data, size_t len) noexcept
        {
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t tmp[20];
            size_t n = encodeVarint(tmp, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(detail::Wire::Fixlen));
            n += encodeVarint(tmp + n, (static_cast<uint64_t>(len) << 3) | static_cast<uint64_t>(ft));
            if (Error e = pushBytes(tmp, n); e != Error::None) return e;
            return pushBytes(data, len);
        }

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

        template <std::floating_point F>
        [[nodiscard]] Error writeFloatArray(sofab::id fieldId, std::span<const F> elems) noexcept
        {
            constexpr detail::Fix ft = (sizeof(F) == 4) ? detail::Fix::Fp32 : detail::Fix::Fp64;
            if (fieldId > detail::kIdMax) return Error::InvalidArgument;
            uint8_t hdr[20];
            size_t hn = encodeVarint(hdr, (static_cast<uint64_t>(fieldId) << 3) | static_cast<uint64_t>(detail::Wire::ArrayFixlen));
            hn += encodeVarint(hdr + hn, elems.size());
            hn += encodeVarint(hdr + hn, (static_cast<uint64_t>(sizeof(F)) << 3) | static_cast<uint64_t>(ft));
            if (Error e = pushBytes(hdr, hn); e != Error::None) return e;

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
        /* Chainable result: each call short-circuits once an error is latched. */
        class Result
        {
            OStreamImpl &os_;
            Error error_;
            friend class OStreamImpl;
            Result(OStreamImpl &os, Error e) noexcept : os_(os), error_(e) {}

        public:
            template <typename T>
            Result write(sofab::id fieldId, const T &value) noexcept
            {
                if (error_ == Error::None) error_ = os_.write(fieldId, value).error_;
                return *this;
            }
            template <typename T>
            Result writeIf(sofab::id fieldId, const T &value, bool condition) noexcept
            {
                if (error_ == Error::None && condition) error_ = os_.write(fieldId, value).error_;
                return *this;
            }
            Result sequenceBegin(sofab::id fieldId) noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceBegin(fieldId).error_;
                return *this;
            }
            Result sequenceEnd() noexcept
            {
                if (error_ == Error::None) error_ = os_.sequenceEnd().error_;
                return *this;
            }
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            [[nodiscard]] Error code() const noexcept { return error_; }
            bool operator==(Error e) const noexcept { return error_ == e; }
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

        OStreamImpl(const OStreamImpl &) = delete;
        OStreamImpl &operator=(const OStreamImpl &) = delete;
        OStreamImpl(OStreamImpl &&) noexcept = default;
        OStreamImpl &operator=(OStreamImpl &&) noexcept = default;
        virtual ~OStreamImpl() noexcept { flush(); }

        size_t flush() noexcept
        {
            size_t used = static_cast<size_t>(cursor_ - buffer_);
            if (flushCallback_ && used)
                flushCallback_(std::span<const uint8_t>(buffer_, used));
            cursor_ = buffer_;
            return used;
        }

        [[nodiscard]] size_t bytesUsed() const noexcept { return static_cast<size_t>(cursor_ - buffer_); }
        [[nodiscard]] const uint8_t *data() const noexcept { return buffer_; }

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

        Result write(sofab::id fieldId, const void *value, int32_t size) noexcept
        {
            return Result{*this, writeFixlen(fieldId, detail::Fix::Blob,
                          static_cast<const uint8_t *>(value), static_cast<size_t>(size))};
        }

        template <typename T>
        Result writeIf(sofab::id fieldId, const T &value, bool condition) noexcept
        {
            return condition ? write(fieldId, value) : Result{*this, Error::None};
        }

        Result sequenceBegin(sofab::id fieldId) noexcept
        {
            return Result{*this, putHeader(fieldId, detail::Wire::SequenceStart)};
        }
        Result sequenceEnd() noexcept
        {
            return Result{*this, putHeader(0, detail::Wire::SequenceEnd)};
        }
    };

    class OStream : public OStreamImpl
    {
    protected:
        std::shared_ptr<uint8_t[]> bufferOwner_;
        OStream() noexcept = default;

    public:
        explicit OStream(size_t buflen, size_t offset = 0) noexcept
        {
            bufferOwner_ = std::make_shared<uint8_t[]>(buflen);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        OStream(std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
            : bufferOwner_{std::move(buffer)}
        {
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        OStream(flushCallback callback, std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
            : bufferOwner_{std::move(buffer)}
        {
            flushCallback_ = std::move(callback);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        void setBuffer(std::shared_ptr<uint8_t[]> buffer, size_t buflen, size_t offset = 0) noexcept
        {
            bufferOwner_ = std::move(buffer);
            initBuffer(bufferOwner_.get(), buflen, offset);
        }
        [[nodiscard]] std::shared_ptr<uint8_t[]> getBuffer() noexcept { return bufferOwner_; }
    };

    template <size_t N, size_t Offset = 0>
    class OStreamInline : public OStreamImpl
    {
        static_assert(N > 0, "Buffer size N must be greater than zero");
        static_assert(Offset < N, "Offset must be less than buffer size N");
        std::array<uint8_t, N> bufferOwner_{};

    public:
        OStreamInline() noexcept { initBuffer(bufferOwner_.data(), N, Offset); }
        explicit OStreamInline(flushCallback callback) noexcept
        {
            flushCallback_ = std::move(callback);
            initBuffer(bufferOwner_.data(), N, Offset);
        }
    };

    template <class T>
    concept OutputMessage =
        std::derived_from<T, OStreamMessage> &&
        requires { { T::_maxSize } -> std::convertible_to<std::size_t>; } &&
        std::is_same_v<decltype(T::_maxSize), const std::size_t>;

    class OStreamMessage
    {
    protected:
        friend class OStreamImpl;
        virtual OStreamImpl::Result serialize(OStreamImpl &ostream) const noexcept = 0;
    };

    template <OutputMessage MessageType, size_t N = MessageType::_maxSize, size_t Offset = 0>
    class OStreamObject : public OStreamInline<N + Offset, Offset>
    {
        MessageType message_;

    public:
        OStreamObject() noexcept = default;
        explicit OStreamObject(typename OStreamImpl::flushCallback callback) noexcept
            : OStreamInline<N + Offset, Offset>{std::move(callback)} {}

        MessageType &operator->() noexcept { return message_; }

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
    template <typename T>
    concept InputMessage = std::derived_from<T, IStreamMessage>;

    class IStreamImpl
    {
    public:
        class Result
        {
            Error error_;
            friend class IStreamImpl;
            explicit Result(Error e) noexcept : error_(e) {}
        public:
            [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
            [[nodiscard]] bool ok() const noexcept { return error_ == Error::None; }
            [[nodiscard]] Error code() const noexcept { return error_; }
            bool operator==(Error e) const noexcept { return error_ == e; }
            bool operator!=(Error e) const noexcept { return error_ != e; }
        };

    protected:
        /* accumulated input; top-level fields are dispatched as they complete */
        std::vector<uint8_t> acc_;
        size_t topPos_ = 0;

        /* cursor + current-field metadata, valid during a deliver callback */
        const uint8_t *p_ = nullptr;
        const uint8_t *end_ = nullptr;
        detail::Wire type_{};
        detail::Fix fixType_{};
        size_t fixLen_ = 0;       /* payload bytes for fixlen / fixlen-array elem size */
        size_t count_ = 0;        /* array element count */
        bool consumed_ = false;
        bool error_ = false;

        /* deliver target for top-level fields */
        std::function<void(sofab::id, size_t, size_t)> topCallback_;

        IStreamImpl() noexcept = default;

        /* --- low-level cursor reads (bounds-checked) --- */
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
        static bool skipVarint(const uint8_t *&p, const uint8_t *end) noexcept
        {
            while (p < end) if (!(*p++ & 0x80)) return true;
            return false;
        }

        /* Append n bytes to a byte vector. The pragma silences a GCC-13
         * -Wstringop-overflow false positive on vector growth from a pointer. */
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

        /* Advance over one complete field WITHOUT firing callbacks.
         * Returns false if the buffer ends mid-field (need more bytes). */
        bool measureField(const uint8_t *&p, const uint8_t *end) const noexcept
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
                    size_t len = static_cast<size_t>(sub >> 3);
                    if (static_cast<size_t>(end - p) < len) return false;
                    p += len; return true;
                }
                case detail::Wire::ArrayUnsigned:
                case detail::Wire::ArraySigned:
                {
                    uint64_t n; if (!getVarint(p, end, n)) return false;
                    for (uint64_t i = 0; i < n; ++i) if (!skipVarint(p, end)) return false;
                    return true;
                }
                case detail::Wire::ArrayFixlen:
                {
                    uint64_t n; if (!getVarint(p, end, n)) return false;
                    uint64_t sub; if (!getVarint(p, end, sub)) return false;
                    size_t esize = static_cast<size_t>(sub >> 3);
                    size_t bytes = static_cast<size_t>(n) * esize;
                    if (static_cast<size_t>(end - p) < bytes) return false;
                    p += bytes; return true;
                }
                case detail::Wire::SequenceStart:
                {
                    for (;;)
                    {
                        const uint8_t *save = p;
                        uint64_t peek;
                        const uint8_t *q = p;
                        if (!getVarint(q, end, peek)) return false;
                        if (static_cast<detail::Wire>(peek & 0x7) == detail::Wire::SequenceEnd)
                        { p = q; return true; }
                        p = save;
                        if (!measureField(p, end)) return false;
                    }
                }
                case detail::Wire::SequenceEnd:
                    return true;
            }
            return false;
        }

        /* Dispatch fields at one level, firing the given callback per field.
         * `stopAtEnd` true => stop at a SequenceEnd marker (nested level). */
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
                    uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                    count_ = static_cast<size_t>(n);
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

        /* Skip the payload of the current field (cursor at payload start). */
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
                    dispatchLevel([](sofab::id, size_t, size_t) {}, /*stopAtEnd*/ true);
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
                    appendBytes(acc_, stop, static_cast<size_t>(buffer + buflen - stop));
                return Result{Error::None};
            }

            /* Continuation path: append and resume from the buffered tail. */
            appendBytes(acc_, buffer, buflen);
            error_ = false;
            const uint8_t *base = acc_.data();
            const uint8_t *stop = parseTopLevel(base + topPos_, base + acc_.size());
            if (error_) return Result{Error::InvalidMessage};
            topPos_ = static_cast<size_t>(stop - base);
            if (topPos_ == acc_.size()) { acc_.clear(); topPos_ = 0; } /* fully drained */
            return Result{Error::None};
        }

    protected:
        /* Deliver every complete top-level field in [p, end); return the start
         * of the first incomplete field (== end when all were consumed). */
        const uint8_t *parseTopLevel(const uint8_t *p, const uint8_t *end) noexcept
        {
            while (p < end)
            {
                const uint8_t *probe = p;
                if (!measureField(probe, end)) break; /* need more bytes */
                p_ = p;
                end_ = end;
                dispatchOne(topCallback_);
                if (error_) return p;
                p = p_;
            }
            return p;
        }

    public:

        /* Read the current field's value into `value` (call inside a callback). */
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
                dispatchLevel([this, &value](sofab::id i, size_t s, size_t c) {
                    value.deserialize(*this, i, s, c);
                }, /*stopAtEnd*/ true);
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

        /* Read a blob into a caller buffer; returns the number of bytes copied. */
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
        /* deliver exactly one field at the current cursor to `cb` */
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
                uint64_t sub; if (!getVarint(p_, end_, sub)) { error_ = true; return; }
                count_ = static_cast<size_t>(n);
                fixLen_ = static_cast<size_t>(sub >> 3); fixType_ = static_cast<detail::Fix>(sub & 0x7);
            }

            consumed_ = false;
            const uint8_t *payload = p_;
            cb(fieldId, fixLen_, count_);
            if (!consumed_) { p_ = payload; skipPayload(); }
        }
    };

    class IStreamInline : public IStreamImpl
    {
    public:
        using fieldCallback = std::function<void(sofab::id, size_t, size_t)>;

        explicit IStreamInline(fieldCallback callback) noexcept
        {
            topCallback_ = std::move(callback);
        }
    };

    class IStreamMessage
    {
        template <InputMessage MessageType>
        friend class IStreamObject;

    public:
        virtual void deserialize(IStreamImpl &istream, sofab::id id, size_t size, size_t count) noexcept = 0;
        virtual ~IStreamMessage() = default;
    };

    template <InputMessage MessageType>
    class IStreamObject : public IStreamImpl
    {
        MessageType data_;

    public:
        IStreamObject() noexcept
        {
            topCallback_ = [this](sofab::id id, size_t size, size_t count) {
                data_.deserialize(*this, id, size, count);
            };
        }

        MessageType &operator->() noexcept { return data_; }
        const MessageType &operator->() const noexcept { return data_; }
        MessageType &operator*() noexcept { return data_; }
        const MessageType &operator*() const noexcept { return data_; }
    };

} // namespace sofab

/** @} */ // end of defgroup

#endif // SOFAB_HPP
