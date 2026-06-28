/*!
 * @file perf.cpp
 * @brief SofaBuffers pure-C++20 — combined per-operation cost benchmark.
 *
 * Mirror of bench/c/perf.c and bench/cpp/perf.cpp: encodes/decodes the identical
 * message (same field ids, types and values) through the pure-C++20
 * implementation and prints the identical report. Two metrics per workload:
 *
 *   1. CPU cycles/op  -- cost of the code itself, from the hardware cycle
 *      counter (x86 TSC / AArch64 virtual count). Tracks code, not host clock.
 *   2. Throughput MB/s -- process CPU time (std::clock()). MB = 1e6 bytes.
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

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define PERF_HAVE_CYCLES 1
static inline uint64_t perf_cycles() { return (uint64_t)__rdtsc(); }
#elif defined(__aarch64__)
#define PERF_HAVE_CYCLES 1
static inline uint64_t perf_cycles() { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#else
#define PERF_HAVE_CYCLES 0
static inline uint64_t perf_cycles() { return 0; }
#endif

namespace
{

#define PERF_STRING "perf-benchmark-message"

const uint32_t perf_samples[8] = {1000000u, 2000000u, 3000000u, 4000000u, 5000000u, 6000000u, 7000000u, 8000000u};
const int32_t  perf_deltas[8]  = {-100000, -200000, -300000, -400000, -500000, -600000, -700000, -800000};
const double   perf_fp64[4]    = {3.14159265, 6.28318530, 9.42477795, 12.56637060};

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

size_t perf_encode(uint8_t *buf, size_t buflen)
{
    OStreamRaw os;
    os.init(buf, buflen);
    os.write(1, static_cast<uint32_t>(0xDEADBEEFu));
    os.write(2, static_cast<int32_t>(-12345));
    os.write(3, static_cast<uint64_t>(0x0123456789ABCDEFull));
    os.write(4, static_cast<int64_t>(-5000000000000ll));
    os.write(5, true);
    os.write(6, 3.14159f);
    os.write(7, 2.718281828459045);
    os.write(8, PERF_STRING);
    os.write(9, std::span<const uint32_t>(perf_samples, 8));
    os.write(10, std::span<const int32_t>(perf_deltas, 8));
    os.write(11, std::span<const double>(perf_fp64, 4));
    os.sequenceBegin(12);
    os.write(1, static_cast<uint32_t>(99));
    os.write(2, static_cast<int32_t>(-7));
    os.sequenceEnd();
    return os.bytesUsed();
}

struct PerfOut
{
    uint32_t u32 = 0; int32_t i32 = 0; uint64_t u64 = 0; int64_t i64 = 0;
    bool b = false; float f32 = 0.0f; double f64 = 0.0; std::string str;
    uint32_t samples[8] = {}; int32_t deltas[8] = {}; double fp64[4] = {};
    uint32_t s_u32 = 0; int32_t s_i32 = 0;
};

struct PerfChild : sofab::IStreamMessage
{
    PerfOut *o = nullptr;
    void deserialize(sofab::IStreamImpl &s, sofab::id i, size_t, size_t) noexcept override
    {
        if (i == 1) s.read(o->s_u32);
        else if (i == 2) s.read(o->s_i32);
    }
};

void perf_decode(const uint8_t *buf, size_t len, PerfOut &out)
{
    IStreamRaw is;
    PerfChild child; child.o = &out;
    is.init([&is, &out, &child](sofab::id id, size_t, size_t count) {
        switch (id)
        {
            case 1:  is.read(out.u32); break;
            case 2:  is.read(out.i32); break;
            case 3:  is.read(out.u64); break;
            case 4:  is.read(out.i64); break;
            case 5:  is.read(out.b); break;
            case 6:  is.read(out.f32); break;
            case 7:  is.read(out.f64); break;
            case 8:  is.read(out.str); break;
            case 9:  { std::span<uint32_t> s(out.samples, count); is.read(s); } break;
            case 10: { std::span<int32_t> s(out.deltas, count); is.read(s); } break;
            case 11: { std::span<double> s(out.fp64, count); is.read(s); } break;
            case 12: is.read(child); break;
            default: break;
        }
    });
    is.feed(buf, len);
}

struct PerfResult { unsigned long iters; double cycles_op; double ns_op; double mb_s; };

double cpu_now() { return (double)std::clock() / (double)CLOCKS_PER_SEC; }

void perf_report(const char *what, PerfResult r, size_t bytes)
{
    printf("\n--- perf: %s ---\n", what);
    printf("  iterations    : %lu\n", r.iters);
    printf("  message size  : %zu bytes\n", bytes);
#if PERF_HAVE_CYCLES
    printf("  cycles/op     : %.1f  (hardware cycle counter)\n", r.cycles_op);
#else
    (void)r.cycles_op;
    printf("  cycles/op     : (cycle counter unavailable on this arch)\n");
#endif
    printf("  CPU time/op   : %.1f ns  (process CPU time, not wall-clock)\n", r.ns_op);
    printf("  throughput    : %.1f MB/s  (speedtest, MB = 1e6 bytes)\n", r.mb_s);
}

PerfResult measure_encode(uint8_t *buf, size_t buflen, size_t &msg_size)
{
    volatile size_t sink = 0;
    size_t msg = 0;
    for (unsigned i = 0; i < 1000u; i++) msg = perf_encode(buf, buflen); /* warmup */
    msg_size = msg;

    unsigned long it = 0;
    double el;
    uint64_t c0 = perf_cycles();
    double t0 = cpu_now();
    do { sink += perf_encode(buf, buflen); it++; el = cpu_now() - t0; } while (el < 1.0);
    uint64_t c1 = perf_cycles();
    (void)sink;
    return PerfResult{it, (double)(c1 - c0) / (double)it, el / (double)it * 1e9, (double)msg * (double)it / el / 1e6};
}

PerfResult measure_decode(const uint8_t *buf, size_t len, PerfOut &out)
{
    volatile uint32_t sink = 0;
    for (unsigned i = 0; i < 1000u; i++) perf_decode(buf, len, out); /* warmup */

    unsigned long it = 0;
    double el;
    uint64_t c0 = perf_cycles();
    double t0 = cpu_now();
    do { perf_decode(buf, len, out); sink += out.u32; it++; el = cpu_now() - t0; } while (el < 1.0);
    uint64_t c1 = perf_cycles();
    (void)sink;
    return PerfResult{it, (double)(c1 - c0) / (double)it, el / (double)it * 1e9, (double)len * (double)it / el / 1e6};
}

} // namespace

int main()
{
    static uint8_t buf[512];
    size_t msg_size = 0;

    printf("=== SofaBuffers pure-C++20 per-op cost (cycles/op + throughput MB/s) ===\n");

    PerfResult enc = measure_encode(buf, sizeof buf, msg_size);
    perf_report("serialize (stream API)", enc, msg_size);

    size_t len = perf_encode(buf, sizeof buf);
    PerfOut out;
    PerfResult dec = measure_decode(buf, len, out);
    perf_report("deserialize (stream API)", dec, len);

    printf("\ncycles/op tracks code cost; MB/s is this machine's throughput.\n");
    return 0;
}
