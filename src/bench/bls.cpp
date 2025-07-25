// Copyright (c) 2018-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bls/bls_worker.h>
#include <random.h>
#include <util/time.h>

#include <atomic>
#include <iostream>

static void BuildTestVectors(size_t count, size_t invalidCount,
                             std::vector<CBLSPublicKey>& pubKeys, std::vector<CBLSSecretKey>& secKeys, std::vector<CBLSSignature>& sigs,
                             std::vector<uint256>& msgHashes,
                             std::vector<bool>& invalid)
{
    secKeys.resize(count);
    pubKeys.resize(count);
    sigs.resize(count);
    msgHashes.resize(count);

    invalid.resize(count);
    for (size_t i = 0; i < invalidCount; i++) {
        invalid[i] = true;
    }
    Shuffle(invalid.begin(), invalid.end(), FastRandomContext());

    for (size_t i = 0; i < count; i++) {
        secKeys[i].MakeNewKey();
        pubKeys[i] = secKeys[i].GetPublicKey();
        msgHashes[i] = GetRandHash();
        sigs[i] = secKeys[i].Sign(msgHashes[i], false);

        if (invalid[i]) {
            CBLSSecretKey s;
            s.MakeNewKey();
            sigs[i] = s.Sign(msgHashes[i], false);
        }
    }
}

static void BLS_PubKeyAggregate_Normal(benchmark::Bench& bench)
{
    CBLSSecretKey secKey1, secKey2;
    secKey1.MakeNewKey();
    secKey2.MakeNewKey();
    CBLSPublicKey pubKey1 = secKey1.GetPublicKey();
    CBLSPublicKey pubKey2 = secKey2.GetPublicKey();

    // Benchmark.
    bench.minEpochIterations(bench.output() ? 100 : 1).run([&] {
        pubKey1.AggregateInsecure(pubKey2);
    });
}

static void BLS_SecKeyAggregate_Normal(benchmark::Bench& bench)
{
    CBLSSecretKey secKey1, secKey2;
    secKey1.MakeNewKey();
    secKey2.MakeNewKey();

    // Benchmark.
    bench.run([&] {
        secKey1.AggregateInsecure(secKey2);
    });
}

static void BLS_SignatureAggregate_Normal(benchmark::Bench& bench)
{
    uint256 hash = GetRandHash();
    CBLSSecretKey secKey1, secKey2;
    secKey1.MakeNewKey();
    secKey2.MakeNewKey();
    CBLSSignature sig1 = secKey1.Sign(hash, false);
    CBLSSignature sig2 = secKey2.Sign(hash, false);

    // Benchmark.
    bench.run([&] {
        sig1.AggregateInsecure(sig2);
    });
}

static void BLS_Sign_Normal(benchmark::Bench& bench)
{
    CBLSSecretKey secKey;
    secKey.MakeNewKey();
    CBLSSignature sig;

    // Benchmark.
    bench.minEpochIterations(100).run([&] {
        uint256 hash = GetRandHash();
        sig = secKey.Sign(hash, false);
    });
}

static void BLS_Verify_Normal(benchmark::Bench& bench)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(1000, 10, pubKeys, secKeys, sigs, msgHashes, invalid);

    // Benchmark.
    size_t i = 0;
    bench.minEpochIterations(20).run([&] {
        bool valid = sigs[i].VerifyInsecure(pubKeys[i], msgHashes[i]);
        if (valid && invalid[i]) {
            std::cout << "expected invalid but it is valid" << std::endl;
            assert(false);
        } else if (!valid && !invalid[i]) {
            std::cout << "expected valid but it is invalid" << std::endl;
            assert(false);
        }
        i = (i + 1) % pubKeys.size();
    });
}


static void BLS_Verify_LargeBlock(size_t txCount, benchmark::Bench& bench, uint32_t epoch_iters)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(bench.output() ? txCount : 1, 0, pubKeys, secKeys, sigs, msgHashes, invalid);

    // Benchmark.
    bench.minEpochIterations(bench.output() ? epoch_iters : 1).run([&] {
        for (size_t i = 0; i < pubKeys.size(); i++) {
            bool ok = sigs[i].VerifyInsecure(pubKeys[i], msgHashes[i]);
            assert(ok);
        }
    });
}

static void BLS_Verify_LargeBlock100(benchmark::Bench& bench)
{
    BLS_Verify_LargeBlock(100, bench, 10);
}

static void BLS_Verify_LargeBlock1000(benchmark::Bench& bench)
{
    BLS_Verify_LargeBlock(1000, bench, 1);
}

static void BLS_Verify_LargeBlockSelfAggregated(size_t txCount, benchmark::Bench& bench, uint32_t epoch_iters)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(bench.output() ? txCount : 1, 0, pubKeys, secKeys, sigs, msgHashes, invalid);

    // Benchmark.
    bench.minEpochIterations(bench.output() ? epoch_iters : 1).run([&] {
        CBLSSignature aggSig = CBLSSignature::AggregateInsecure(sigs);
        bool ok = aggSig.VerifyInsecureAggregated(pubKeys, msgHashes);
        assert(ok);
    });
}

static void BLS_Verify_LargeBlockSelfAggregated100(benchmark::Bench& bench)
{
    BLS_Verify_LargeBlockSelfAggregated(100, bench, 10);
}

static void BLS_Verify_LargeBlockSelfAggregated1000(benchmark::Bench& bench)
{
    BLS_Verify_LargeBlockSelfAggregated(1000, bench, 2);
}

static void BLS_Verify_LargeAggregatedBlock(size_t txCount, benchmark::Bench& bench, uint32_t epoch_iters)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(bench.output() ? txCount : 1, 0, pubKeys, secKeys, sigs, msgHashes, invalid);

    CBLSSignature aggSig = CBLSSignature::AggregateInsecure(sigs);

    // Benchmark.
    bench.minEpochIterations(bench.output() ? epoch_iters : 1).run([&] {
        bool ok = aggSig.VerifyInsecureAggregated(pubKeys, msgHashes);
        assert(ok);
    });
}

static void BLS_Verify_LargeAggregatedBlock100(benchmark::Bench& bench)
{
    BLS_Verify_LargeAggregatedBlock(100, bench, 10);
}

static void BLS_Verify_LargeAggregatedBlock1000(benchmark::Bench& bench)
{
    BLS_Verify_LargeAggregatedBlock(1000, bench, 1);
}

static void BLS_Verify_LargeAggregatedBlock1000PreVerified(benchmark::Bench& bench)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(1000, 0, pubKeys, secKeys, sigs, msgHashes, invalid);

    CBLSSignature aggSig = CBLSSignature::AggregateInsecure(sigs);

    std::set<size_t> prevalidated;

    while (prevalidated.size() < 900) {
        size_t idx = GetRand<size_t>(pubKeys.size());
        if (prevalidated.count(idx)) {
            continue;
        }
        prevalidated.emplace(idx);
    }

    // Benchmark.
    bench.minEpochIterations(bench.output() ? 10 : 1).run([&] {
        std::vector<CBLSPublicKey> nonvalidatedPubKeys;
        std::vector<uint256> nonvalidatedHashes;
        nonvalidatedPubKeys.reserve(pubKeys.size());
        nonvalidatedHashes.reserve(msgHashes.size());

        for (size_t i = 0; i < sigs.size(); i++) {
            if (prevalidated.count(i)) {
                continue;
            }
            nonvalidatedPubKeys.emplace_back(pubKeys[i]);
            nonvalidatedHashes.emplace_back(msgHashes[i]);
        }

        CBLSSignature aggSigCopy = aggSig;
        for (auto idx : prevalidated) {
            aggSigCopy.SubInsecure(sigs[idx]);
        }

        bool valid = aggSigCopy.VerifyInsecureAggregated(nonvalidatedPubKeys, nonvalidatedHashes);
        assert(valid);
    });
}

static void BLS_Verify_Batched(benchmark::Bench& bench)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(bench.output() ? 1000 : 1, 10, pubKeys, secKeys, sigs, msgHashes, invalid);

    // Benchmark.
    size_t i = 0;
    size_t j = 0;
    size_t batchSize = 16;
    bench.minEpochIterations(bench.output() ? 1000 : 1).run([&] {
        j++;
        if ((j % batchSize) != 0) {
            return;
        }

        std::vector<CBLSPublicKey> testPubKeys;
        std::vector<CBLSSignature> testSigs;
        std::vector<uint256> testMsgHashes;
        testPubKeys.reserve(batchSize);
        testSigs.reserve(batchSize);
        testMsgHashes.reserve(batchSize);
        size_t startI = i;
        for (size_t k = 0; k < batchSize; k++) {
            testPubKeys.emplace_back(pubKeys[i]);
            testSigs.emplace_back(sigs[i]);
            testMsgHashes.emplace_back(msgHashes[i]);
            i = (i + 1) % pubKeys.size();
        }

        CBLSSignature batchSig = CBLSSignature::AggregateInsecure(testSigs);
        bool batchValid = batchSig.VerifyInsecureAggregated(testPubKeys, testMsgHashes);
        std::vector<bool> valid;
        if (batchValid) {
            valid.assign(batchSize, true);
        } else {
            for (size_t k = 0; k < batchSize; k++) {
                bool valid1 = testSigs[k].VerifyInsecure(testPubKeys[k], testMsgHashes[k]);
                valid.emplace_back(valid1);
            }
        }
        for (size_t k = 0; k < batchSize; k++) {
            if (valid[k] && invalid[(startI + k) % pubKeys.size()]) {
                std::cout << "expected invalid but it is valid" << std::endl;
                assert(false);
            } else if (!valid[k] && !invalid[(startI + k) % pubKeys.size()]) {
                std::cout << "expected valid but it is invalid" << std::endl;
                assert(false);
            }
        }
    });
}

static void BLS_Verify_BatchedParallel(benchmark::Bench& bench)
{
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<CBLSSecretKey> secKeys;
    std::vector<CBLSSignature> sigs;
    std::vector<uint256> msgHashes;
    std::vector<bool> invalid;
    BuildTestVectors(bench.output() ? 1000 : 1, 10, pubKeys, secKeys, sigs, msgHashes, invalid);

    std::list<std::pair<size_t, std::future<bool>>> futures;

    std::atomic<bool> cancel{false};
    auto cancelCond = [&]() -> bool {
        return cancel.load();
    };

    CBLSWorker blsWorker;
    blsWorker.Start();

    // Benchmark.
    bench.minEpochIterations(bench.output() ? 1000 : 1).run([&] {
        if (futures.size() < 100) {
            while (futures.size() < 10000) {
                size_t i = 0;
                auto f = blsWorker.AsyncVerifySig(sigs[i], pubKeys[i], msgHashes[i], cancelCond);
                futures.emplace_back(std::make_pair(i, std::move(f)));
                i = (i + 1) % pubKeys.size();
            }
        }

        auto fp = std::move(futures.front());
        futures.pop_front();

        size_t j = fp.first;
        bool valid = fp.second.get();

        if (valid && invalid[j]) {
            std::cout << "expected invalid but it is valid" << std::endl;
            assert(false);
        } else if (!valid && !invalid[j]) {
            std::cout << "expected valid but it is invalid" << std::endl;
            assert(false);
        }
    });

    cancel.store(true);
    while (blsWorker.IsAsyncVerifyInProgress())
    {
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    blsWorker.Stop();
}

BENCHMARK(BLS_PubKeyAggregate_Normal)
BENCHMARK(BLS_SecKeyAggregate_Normal)
BENCHMARK(BLS_SignatureAggregate_Normal)
BENCHMARK(BLS_Sign_Normal)
BENCHMARK(BLS_Verify_Normal)
BENCHMARK(BLS_Verify_LargeBlock100)
BENCHMARK(BLS_Verify_LargeBlock1000)
BENCHMARK(BLS_Verify_LargeBlockSelfAggregated100)
BENCHMARK(BLS_Verify_LargeBlockSelfAggregated1000)
BENCHMARK(BLS_Verify_LargeAggregatedBlock100)
BENCHMARK(BLS_Verify_LargeAggregatedBlock1000)
BENCHMARK(BLS_Verify_LargeAggregatedBlock1000PreVerified)
BENCHMARK(BLS_Verify_Batched)
BENCHMARK(BLS_Verify_BatchedParallel)
