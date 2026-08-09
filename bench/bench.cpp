/*!
 * @file bench.cpp
 * @brief SofaBuffers pure-C++20 — throughput benchmark (MB/s, CPU time).
 *
 * Mirror of bench/c/bench.c and bench/cpp/bench.cpp: identical workloads, data,
 * field ids and values, driven through the pure-C++20 implementation
 * (include/sofab/sofab.hpp — no C backend), so the figures are directly
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

#include "bench_common.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#define N 1000

namespace
{

using sofab_bench::IStreamRaw;
using sofab_bench::OStreamRaw;

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
    os.sequenceBeginLazy(7);
    os.write(1, static_cast<uint32_t>(99));
    os.write(2, static_cast<int32_t>(-7));
    os.sequenceEnd();
}

double measure(void (*fn)(), size_t bytes)
{
    fn(); /* warmup */
    return sofab_bench::measureLoop([fn] { fn(); }, bytes).mb_s;
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

/* ---- the workload table: the one definition of what this suite measures ---
 *
 * The timed table, the single-shot Callgrind mode and `--list` all walk this
 * array, so a workload cannot exist in one of them and be missing from another.
 * `bench --list` prints it as "name<TAB>label" lines; run_callgrind.sh and the
 * run_bench_callgrind CMake target take their workload names and their row
 * labels from that output instead of keeping copies (CORELIB_PLAN §10). */

namespace
{

struct Workload
{
    const char *name;    /* CLI name; the Callgrind toggle is run_<name> */
    const char *label;   /* row label, per BENCH_SPEC's output grammar */
    void (*setup)();     /* prepares the input, excluded from collection */
    void (*run)();       /* the measured operation */
    const size_t *bytes; /* message size, valid once setup+run have run */
};

const Workload kWorkloads[] = {
    {"encode_u64_array", "encode: u64 array (1000)", nullptr, run_encode_u64_array, &enc_u64_used},
    {"encode_typical", "encode: typical message", nullptr, run_encode_typical, &typ_used},
    {"decode_u64_array", "decode: u64 array (1000)", run_encode_u64_array, run_decode_u64_array,
     &enc_u64_used},
    {"decode_typical", "decode: typical message", run_encode_typical, run_decode_typical,
     &typ_used},
};

const Workload *find(const char *name)
{
    for (const Workload &w : kWorkloads)
        if (!strcmp(w.name, name)) return &w;
    return nullptr;
}

/* ---- single-shot mode (one operation, for Callgrind instruction counts) -- */

int run_one(const char *name)
{
    const Workload *w = find(name);
    if (!w)
    {
        fprintf(stderr, "unknown workload: %s\n", name);
        return 1;
    }

    make_src();
    if (w->setup) w->setup();
    w->run();

    fprintf(stderr, "arr0=%llu f1=%u s_f2=%d str=%.5s BYTES=%zu\n",
            (unsigned long long)dec_array[0], T.f1, T.s_f2, T.f5.c_str(), *w->bytes);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    T.f5.reserve(16); /* string read buffer, reserved outside the measured run */

    if (argc >= 2)
    {
        if (!strcmp(argv[1], "--list"))
        {
            for (const Workload &w : kWorkloads) printf("%s\t%s\n", w.name, w.label);
            return 0;
        }
        return run_one(argv[1]);
    }

    make_src();
    for (const Workload &w : kWorkloads) /* prime every buffer and message size */
    {
        if (w.setup) w.setup();
        w.run();
    }

    printf("=== SofaBuffers pure-C++20 throughput (CPU time, MB/s) ===\n");
    printf("%-26s %12s\n", "Workload", "MB/s");
    printf("%-26s %12s\n", "--------", "----");
    for (const Workload &w : kWorkloads)
        printf("%-26s %12.2f\n", w.label, measure(w.run, *w.bytes));
    printf("\nMB = 1e6 bytes. ~1s CPU-time loop per workload.\n");
    return 0;
}
