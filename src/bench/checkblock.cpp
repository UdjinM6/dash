// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <stats/client.h>
#include <streams.h>
#include <validation.h>

// These are the two major time-sinks which happen after we have fully received
// a block off the wire, but before we can relay the block on to peers using
// compact block relay.

static void DeserializeBlockTest(benchmark::Bench& bench)
{
    CDataStream stream(benchmark::data::block813851, SER_NETWORK, PROTOCOL_VERSION);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    bench.unit("block").run([&] {
        CBlock block;
        stream >> block;
        bool rewound = stream.Rewind(benchmark::data::block813851.size());
        assert(rewound);
    });
}

static void DeserializeAndCheckBlockTest(benchmark::Bench& bench)
{
    CDataStream stream(benchmark::data::block813851, SER_NETWORK, PROTOCOL_VERSION);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    ArgsManager bench_args;
    const auto chainParams = CreateChainParams(bench_args, CBaseChainParams::MAIN);
    // CheckBlock calls g_stats_client internally, we aren't using a testing setup
    // so we need to do this manually.
    ::g_stats_client = InitStatsClient(bench_args);

    bench.unit("block").run([&] {
        CBlock block; // Note that CBlock caches its checked state, so we need to recreate it here
        stream >> block;
        bool rewound = stream.Rewind(benchmark::data::block813851.size());
        assert(rewound);

        BlockValidationState validationState;
        bool checked = CheckBlock(block, validationState, chainParams->GetConsensus(), block.GetBlockTime());
        assert(checked);
    });
}

BENCHMARK(DeserializeBlockTest);
BENCHMARK(DeserializeAndCheckBlockTest);
