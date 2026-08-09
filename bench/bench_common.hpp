/*!
 * @file bench_common.hpp
 * @brief Shared plumbing for the SofaBuffers pure-C++20 benchmark tools.
 *
 * CORELIB_PLAN §10 asks for three benchmark tools (`bench`, `perf`,
 * `run_callgrind.sh`) that follow one specification — BENCH_SPEC.md — so their
 * numbers stay comparable across languages. That only holds while they share one
 * definition of the pieces the spec constrains, so both harnesses take their
 * stream adapters and their timing rules from here rather than keeping a private
 * copy that can drift.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SOFAB_BENCH_COMMON_HPP
#define SOFAB_BENCH_COMMON_HPP

#include "sofab/sofab.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define PERF_HAVE_CYCLES 1
#elif defined(__aarch64__)
#define PERF_HAVE_CYCLES 1
#else
#define PERF_HAVE_CYCLES 0
#endif

namespace sofab_bench
{

/* ---- stream adapters ------------------------------------------------------
 *
 * The benchmarks' only view of the corelib's public surface: streams over a
 * caller-owned raw buffer, so no heap traffic lands in the measured run. One
 * definition, so a change to that surface cannot leave one tool building and the
 * other not. */

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

/* ---- timing harness ------------------------------------------------------- */

/*! Hardware cycle counter (x86 TSC / AArch64 virtual count); 0 where absent. */
inline uint64_t cycles() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    return (uint64_t)__rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

/*! Process CPU time in seconds (not wall-clock). */
inline double cpu_now() noexcept { return (double)std::clock() / (double)CLOCKS_PER_SEC; }

/* Block size for the timed loop: enough iterations that one clock reading is a
 * rounding error against them. std::clock() is a real cost — on a host without
 * a vDSO fast path for CLOCK_PROCESS_CPUTIME_ID it runs to about a microsecond,
 * which is more than an entire `typical message` operation — so reading it once
 * per iteration, as these loops used to, measures mostly the clock. It corrupts
 * cycles/op too, not just the timing: the cycle counter brackets the whole loop,
 * so every clock() call in between is counted as part of the work. The spec asks
 * for a ~1 s CPU-time loop and a warmup; how often the clock is sampled inside
 * that loop is ours to choose. */
constexpr double kBlockSeconds = 0.01; /* clock cost lands under ~0.01% of a block */

/*! Grow a block of back-to-back operations until it spans ::kBlockSeconds. */
template <class F> inline unsigned long calibrateBlock(F &&body)
{
    for (unsigned long block = 1;; block *= 2)
    {
        const double t0 = cpu_now();
        for (unsigned long k = 0; k < block; ++k) body();
        if (cpu_now() - t0 >= kBlockSeconds) return block;
    }
}

struct MeasureResult
{
    unsigned long iters;
    double cycles_op;
    double ns_op;
    double mb_s; /*!< MB = 1e6 bytes, per BENCH_SPEC. */
};

/*! BENCH_SPEC's measured loop: ~1 s of CPU time, clock sampled once per block. */
template <class F> inline MeasureResult measureLoop(F &&body, size_t bytes)
{
    const unsigned long block = calibrateBlock(body);

    unsigned long it = 0;
    double el;
    const uint64_t c0 = cycles();
    const double t0 = cpu_now();
    do {
        for (unsigned long k = 0; k < block; ++k) body();
        it += block;
        el = cpu_now() - t0;
    } while (el < 1.0);
    const uint64_t c1 = cycles();

    return MeasureResult{it, (double)(c1 - c0) / (double)it, el / (double)it * 1e9,
                         (double)bytes * (double)it / el / 1e6};
}

} // namespace sofab_bench

#endif /* SOFAB_BENCH_COMMON_HPP */
