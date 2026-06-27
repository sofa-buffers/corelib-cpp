/*!
 * @file bench.cpp
 * @brief SofaBuffers pure-C++20 — throughput benchmark (MB/s, CPU time).
 *
 * Mirror of bench/c/bench.c and bench/cpp/bench.cpp: identical workloads, data,
 * field ids and values, driven through the pure-C++20 implementation
 * (cpp20/include/sofab/sofab.hpp — no C backend), so the figures are directly
 * comparable to the C corelib and its C++ wrapper.
 *
 * Two modes:
 *   bench               -> timed MB/s table (default, CPU time).
 *   bench <workload>    -> run one operation once and exit; used by Callgrind
 *                          (run_callgrind.sh) to count instructions/op. The
 *                          run_<workload> functions are extern "C" + noinline so
 *                          --toggle-collect=run_<workload> matches the same
 *                          symbol names as the C / C++ benchmarks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <span>
#include <string>

#define N 1000

namespace
{

/* Streams that drive a caller-owned raw buffer (no heap in the measured run). */
class OStreamRaw : public sofab::OStreamImpl
{
public:
    void init(uint8_t *b, size_t n) noexcept { initBuffer(b, n, 0); }
};

class IStreamRaw : public sofab::IStreamImpl
{
public:
    template <class F> void init(F &&cb) noexcept { topCallback_ = std::forward<F>(cb); }
};

/* shared buffers */
uint64_t src[N];
uint8_t  enc_u64_buf[N * 11 + 16];
size_t   enc_u64_used;

uint8_t  typ_buf[256];
size_t   typ_used;
const uint16_t arr16[4] = {10, 20, 30, 40};

/* decode targets */
uint64_t dec_array[N];
struct Targets
{
    uint32_t f1; int32_t f2; bool f3; float f4;
    std::string f5; uint16_t f6[4]; uint32_t s_f1; int32_t s_f2;
} T;

struct ChildMsg : sofab::IStreamMessage
{
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) s.read(T.s_f1);
        else if (i == 2) s.read(T.s_f2);
    }
};
ChildMsg childMsg;

void make_src()
{
    for (int i = 0; i < N; i++)
        src[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
}

void encode_typical(OStreamRaw &os)
{
    os.write(1, static_cast<uint32_t>(0xDEADBEEF));
    os.write(2, static_cast<int32_t>(-12345));
    os.write(3, true);
    os.write(4, 3.14159f);
    os.write(5, "sofab");
    os.write(6, std::span<const uint16_t>(arr16, 4));
    os.sequenceBegin(7);
    os.write(1, static_cast<uint32_t>(99));
    os.write(2, static_cast<int32_t>(-7));
    os.sequenceEnd();
}

double cpu_now() { return (double)std::clock() / (double)CLOCKS_PER_SEC; }

double measure(void (*fn)(), size_t bytes)
{
    fn(); /* warmup */
    double t0 = cpu_now();
    long it = 0;
    double el;
    do { fn(); it++; el = cpu_now() - t0; } while (el < 1.0);
    return (double)bytes * (double)it / el / 1e6; /* MB/s, MB = 1e6 bytes */
}

} // namespace

/* ---- workloads (extern "C" + noinline = stable Callgrind toggle points) --- */

extern "C" __attribute__((noinline)) void run_encode_u64_array()
{
    OStreamRaw os;
    os.init(enc_u64_buf, sizeof enc_u64_buf);
    os.write(1, std::span<const uint64_t>(src, N));
    enc_u64_used = os.bytesUsed();
}

extern "C" __attribute__((noinline)) void run_encode_typical()
{
    OStreamRaw os;
    os.init(typ_buf, sizeof typ_buf);
    encode_typical(os);
    typ_used = os.bytesUsed();
}

extern "C" __attribute__((noinline)) void run_decode_u64_array()
{
    IStreamRaw is;
    is.init([&is](sofab::id id, size_t, size_t count) {
        if (id == 1) { std::span<uint64_t> sp(dec_array, count); is.read(sp); }
    });
    is.feed(enc_u64_buf, enc_u64_used);
}

extern "C" __attribute__((noinline)) void run_decode_typical()
{
    IStreamRaw is;
    is.init([&is](sofab::id id, size_t, size_t) {
        switch (id)
        {
            case 1: is.read(T.f1); break;
            case 2: is.read(T.f2); break;
            case 3: is.read(T.f3); break;
            case 4: is.read(T.f4); break;
            case 5: is.read(T.f5); break;
            case 6: { std::span<uint16_t> sp(T.f6, 4); is.read(sp); } break;
            case 7: is.read(childMsg); break;
            default: break;
        }
    });
    is.feed(typ_buf, typ_used);
}

/* ---- single-shot mode (one operation, for Callgrind instruction counts) -- */

static int run_one(const char *w)
{
    make_src();
    if (!strcmp(w, "encode_u64_array")) {
        run_encode_u64_array();
    } else if (!strcmp(w, "encode_typical")) {
        run_encode_typical();
    } else if (!strcmp(w, "decode_u64_array")) {
        run_encode_u64_array();          /* setup (excluded from collection) */
        run_decode_u64_array();
    } else if (!strcmp(w, "decode_typical")) {
        run_encode_typical();            /* setup (excluded from collection) */
        run_decode_typical();
    } else {
        fprintf(stderr, "unknown workload: %s\n", w);
        return 1;
    }

    size_t bytes = (!strcmp(w, "encode_u64_array") || !strcmp(w, "decode_u64_array"))
                       ? enc_u64_used : typ_used;
    fprintf(stderr, "arr0=%llu f1=%u s_f2=%d str=%.5s BYTES=%zu\n",
            (unsigned long long)dec_array[0], T.f1, T.s_f2, T.f5.c_str(), bytes);
    return 0;
}

int main(int argc, char **argv)
{
    T.f5.reserve(16); /* string read buffer, reserved outside the measured run */

    if (argc >= 2)
        return run_one(argv[1]);

    make_src();
    run_encode_u64_array();
    run_encode_typical();
    size_t ba = enc_u64_used, bt = typ_used;

    printf("=== SofaBuffers pure-C++20 throughput (CPU time, MB/s) ===\n");
    printf("%-26s %12s\n", "Workload", "MB/s");
    printf("%-26s %12s\n", "--------", "----");
    printf("%-26s %12.2f\n", "encode: u64 array (1000)", measure(run_encode_u64_array, ba));
    printf("%-26s %12.2f\n", "encode: typical message",  measure(run_encode_typical, bt));
    printf("%-26s %12.2f\n", "decode: u64 array (1000)", measure(run_decode_u64_array, ba));
    printf("%-26s %12.2f\n", "decode: typical message",  measure(run_decode_typical, bt));
    printf("\nMB = 1e6 bytes. ~1s CPU-time loop per workload.\n");
    return 0;
}
