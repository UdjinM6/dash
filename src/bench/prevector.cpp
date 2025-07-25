// Copyright (c) 2015-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <prevector.h>
#include <serialize.h>
#include <streams.h>
#include <type_traits>
#include <vector>

#include <bench/bench.h>

struct nontrivial_t {
    int x;
    nontrivial_t() :x(-1) {}
    SERIALIZE_METHODS(nontrivial_t, obj) { READWRITE(obj.x); }
};
typedef prevector<28, unsigned char> prevec;

static_assert(!std::is_trivially_default_constructible<nontrivial_t>::value,
              "expected nontrivial_t to not be trivially constructible");

typedef unsigned char trivial_t;
static_assert(std::is_trivially_default_constructible<trivial_t>::value,
              "expected trivial_t to be trivially constructible");

template <typename T>
static void PrevectorDestructor(benchmark::Bench& bench)
{
    bench.batch(2).run([&] {
        prevector<28, T> t0;
        prevector<28, T> t1;
        t0.resize(28);
        t1.resize(29);
    });
}

template <typename T>
static void PrevectorClear(benchmark::Bench& bench)
{
    prevector<28, T> t0;
    prevector<28, T> t1;
    bench.batch(2).run([&] {
        t0.resize(28);
        t0.clear();
        t1.resize(29);
        t1.clear();
    });
}

template <typename T>
static void PrevectorResize(benchmark::Bench& bench)
{
    prevector<28, T> t0;
    prevector<28, T> t1;
    bench.batch(4).run([&] {
        t0.resize(28);
        t0.resize(0);
        t1.resize(29);
        t1.resize(0);
    });
}

template <typename T>
static void PrevectorDeserialize(benchmark::Bench& bench)
{
    CDataStream s0(SER_NETWORK, 0);
    prevector<28, T> t0;
    t0.resize(28);
    for (auto x = 0; x < 900; ++x) {
        s0 << t0;
    }
    t0.resize(100);
    for (auto x = 0; x < 101; ++x) {
        s0 << t0;
    }
    bench.batch(1000).run([&] {
        prevector<28, T> t1;
        for (auto x = 0; x < 1000; ++x) {
            s0 >> t1;
        }
        s0.Rewind();
    });
}

static void PrevectorAssign(benchmark::Bench& bench)
{
    prevec t;
    t.resize(28);
    std::vector<unsigned char> v;
    bench.batch(1000).run([&] {
        prevec::const_iterator b = t.begin() + 5;
        prevec::const_iterator e = b + 20;
        v.assign(b, e);
    });
}

static void PrevectorAssignTo(benchmark::Bench& bench)
{
    prevec t;
    t.resize(28);
    std::vector<unsigned char> v;
    bench.batch(1000).run([&] {
        prevec::const_iterator b = t.begin() + 5;
        prevec::const_iterator e = b + 20;
        t.assign_to(b, e, v);
    });
}

template <typename T>
static void PrevectorFillVectorDirect(benchmark::Bench& bench)
{
    bench.run([&] {
        std::vector<prevector<28, T>> vec;
        for (size_t i = 0; i < 260; ++i) {
            vec.emplace_back();
        }
    });
}


template <typename T>
static void PrevectorFillVectorIndirect(benchmark::Bench& bench)
{
    bench.run([&] {
        std::vector<prevector<28, T>> vec;
        for (size_t i = 0; i < 260; ++i) {
            // force allocation
            vec.emplace_back(29, T{});
        }
    });
}

#define PREVECTOR_TEST(name)                                         \
    static void Prevector##name##Nontrivial(benchmark::Bench& bench) \
    {                                                                \
        Prevector##name<nontrivial_t>(bench);                        \
    }                                                                \
    BENCHMARK(Prevector##name##Nontrivial);                          \
    static void Prevector##name##Trivial(benchmark::Bench& bench)    \
    {                                                                \
        Prevector##name<trivial_t>(bench);                           \
    }                                                                \
    BENCHMARK(Prevector##name##Trivial);

PREVECTOR_TEST(Clear)
PREVECTOR_TEST(Destructor)
PREVECTOR_TEST(Resize)
PREVECTOR_TEST(Deserialize)

BENCHMARK(PrevectorAssign)
BENCHMARK(PrevectorAssignTo)
PREVECTOR_TEST(FillVectorDirect)
PREVECTOR_TEST(FillVectorIndirect)
