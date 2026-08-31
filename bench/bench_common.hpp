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
#include <cstdlib>
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

    /*!
     * A caller-supplied buffer installed **with a flush sink** — CORELIB_PLAN
     * §5.1's streaming half, and the only configuration in which a message
     * larger than the buffer can be encoded at all. `encode: blob 1MB streaming`
     * is the workload that drives it (BENCH_SPEC), through a buffer of exactly
     * 4096 bytes.
     */
    void init(flushCallback cb, uint8_t *b, size_t n) noexcept
    {
        flushCallback_ = std::move(cb);
        initBuffer(b, n, 0);
    }
};

class IStreamRaw : public sofab::IStreamImpl
{
public:
    /* The bench measures the codec, not the receiver's field-span policy, so it
     * states the platform ceiling: the check runs on every field and never
     * fires. There is no default to fall back on -- CORELIB_PLAN §6.2.1 leaves
     * the library no number to invent. */
    IStreamRaw() noexcept : sofab::IStreamImpl(sofab::Limits{SIZE_MAX}) {}
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

/*!
 * @brief How long one measured loop runs, in CPU seconds.
 *
 * BENCH_SPEC fixes this at ~1 s and that is the default; nothing that publishes
 * a number changes it. The environment override exists for the one caller that
 * is not after a number — `test_bench_tools.sh`, which holds the printed tables
 * against BENCH_SPEC's output grammar and would otherwise spend a minute of CI
 * producing figures it discards. Driving the real loop over a token budget keeps
 * that check on the same code path as the real run.
 */
inline double loopSeconds() noexcept
{
    static const double v = [] {
        const char *e = std::getenv("SOFAB_BENCH_SECONDS");
        const double d = e ? std::atof(e) : 0.0;
        return d > 0.0 ? d : 1.0;
    }();
    return v;
}

/* Block size for the timed loop: enough iterations that one clock reading is a
 * rounding error against them. std::clock() is a real cost — on a host without
 * a vDSO fast path for CLOCK_PROCESS_CPUTIME_ID it runs to about a microsecond,
 * which is more than an entire `typical message` operation — so reading it once
 * per iteration, as these loops used to, measures mostly the clock. It corrupts
 * cycles/op too, not just the timing: the cycle counter brackets the whole loop,
 * so every clock() call in between is counted as part of the work. The spec asks
 * for a ~1 s CPU-time loop and a warmup; how often the clock is sampled inside
 * that loop is ours to choose. */
inline double blockSeconds() noexcept
{
    const double s = loopSeconds() / 100.0; /* clock cost under ~0.01% of a block */
    return s < 0.01 ? s : 0.01;
}

/*! Grow a block of back-to-back operations until it spans ::blockSeconds. */
template <class F> inline unsigned long calibrateBlock(F &&body)
{
    const double target = blockSeconds();
    for (unsigned long block = 1;; block *= 2)
    {
        const double t0 = cpu_now();
        for (unsigned long k = 0; k < block; ++k) body();
        if (cpu_now() - t0 >= target) return block;
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
    const double budget = loopSeconds();

    unsigned long it = 0;
    double el;
    const uint64_t c0 = cycles();
    const double t0 = cpu_now();
    do {
        for (unsigned long k = 0; k < block; ++k) body();
        it += block;
        el = cpu_now() - t0;
    } while (el < budget);
    const uint64_t c1 = cycles();

    return MeasureResult{it, (double)(c1 - c0) / (double)it, el / (double)it * 1e9,
                         (double)bytes * (double)it / el / 1e6};
}

} // namespace sofab_bench

#endif /* SOFAB_BENCH_COMMON_HPP */
