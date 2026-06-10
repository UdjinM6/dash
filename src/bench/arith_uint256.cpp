// Copyright (c) 2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <arith_uint256.h>

#include <cstdint>
#include <limits>

// Divisors representative of the difficulty-retarget call sites in pow.cpp:
// all comfortably below uint32_max, so they exercise the fast word-wise path.
static constexpr uint64_t SMALL_DIVISOR{3600};
// A divisor above uint32_max, exercising the base_uint fallback path.
static constexpr uint64_t LARGE_DIVISOR{uint64_t{std::numeric_limits<uint32_t>::max()} + 1};

// Fast path: divide by a 32-bit-range integer divisor (single-word long division).
static void ArithUint256DivU32(benchmark::Bench& bench)
{
    const arith_uint256 base{~arith_uint256(0)};
    bench.run([&] {
        arith_uint256 x{base};
        x /= SMALL_DIVISOR;
        ankerl::nanobench::doNotOptimizeAway(x);
    });
}

// Old path: divide by the same value widened to arith_uint256 (bit-by-bit long division).
static void ArithUint256DivArith(benchmark::Bench& bench)
{
    const arith_uint256 base{~arith_uint256(0)};
    const arith_uint256 divisor{SMALL_DIVISOR};
    bench.run([&] {
        arith_uint256 x{base};
        x /= divisor;
        ankerl::nanobench::doNotOptimizeAway(x);
    });
}

// Fallback path: divisor exceeds uint32_max, so operator/=(uint64_t) defers to
// the base_uint division — should track ArithUint256DivArith.
static void ArithUint256DivU64Fallback(benchmark::Bench& bench)
{
    const arith_uint256 base{~arith_uint256(0)};
    bench.run([&] {
        arith_uint256 x{base};
        x /= LARGE_DIVISOR;
        ankerl::nanobench::doNotOptimizeAway(x);
    });
}

BENCHMARK(ArithUint256DivU32, benchmark::PriorityLevel::HIGH);
BENCHMARK(ArithUint256DivArith, benchmark::PriorityLevel::HIGH);
BENCHMARK(ArithUint256DivU64Fallback, benchmark::PriorityLevel::HIGH);
