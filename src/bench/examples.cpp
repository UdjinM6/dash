// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <validation.h>
#include <util/time.h>

// Sanity test: this should loop ten times, and
// min/max/average should be close to 100ms.
static void Sleep100ms(benchmark::Bench& bench)
{
    bench.run([&] {
        UninterruptibleSleep(std::chrono::milliseconds{100});
    });
}

BENCHMARK(Sleep100ms);

// Extremely fast-running benchmark:
#include <math.h>

volatile double sum = 0.0; // volatile, global so not optimized away

static void Trig(benchmark::Bench& bench)
{
    double d = 0.01;
    bench.run([&] {
        sum += sin(d);
        d += 0.000001;
    });
}

BENCHMARK(Trig);
