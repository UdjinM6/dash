// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data.h>

#include <consensus/validation.h>
#include <instantsend/instantsend.h>
#include <llmq/chainlocks.h>
#include <llmq/context.h>
#include <rpc/blockchain.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <univalue.h>

namespace {

struct TestBlockAndIndex {
    const std::unique_ptr<const TestingSetup> testing_setup{MakeNoLogFileContext<const TestingSetup>(CBaseChainParams::MAIN)};
    CBlock block{};
    uint256 blockHash{};
    CBlockIndex blockindex{};

    TestBlockAndIndex()
    {
        CDataStream stream(benchmark::data::block813851, SER_NETWORK, PROTOCOL_VERSION);
        std::byte a{0};
        stream.write({&a, 1}); // Prevent compaction

        stream >> block;

        blockHash = block.GetHash();
        blockindex.phashBlock = &blockHash;
        blockindex.nBits = 403014710;
    }
};

} // namespace

static void BlockToJsonVerbose(benchmark::Bench& bench)
{
    TestBlockAndIndex data;
    const LLMQContext& llmq_ctx = *data.testing_setup->m_node.llmq_ctx;
    bench.run([&] {
        auto univalue = blockToJSON(data.testing_setup->m_node.chainman->m_blockman, data.block, &data.blockindex, &data.blockindex, *llmq_ctx.clhandler, *llmq_ctx.isman, TxVerbosity::SHOW_DETAILS_AND_PREVOUT);
        ankerl::nanobench::doNotOptimizeAway(univalue);
    });
}

BENCHMARK(BlockToJsonVerbose);

static void BlockToJsonVerboseWrite(benchmark::Bench& bench)
{
    TestBlockAndIndex data;
    const LLMQContext& llmq_ctx = *data.testing_setup->m_node.llmq_ctx;
    auto univalue = blockToJSON(data.testing_setup->m_node.chainman->m_blockman, data.block, &data.blockindex, &data.blockindex, *llmq_ctx.clhandler, *llmq_ctx.isman, TxVerbosity::SHOW_DETAILS_AND_PREVOUT);
    bench.run([&] {
        auto str = univalue.write();
        ankerl::nanobench::doNotOptimizeAway(str);
    });
}

BENCHMARK(BlockToJsonVerboseWrite);
