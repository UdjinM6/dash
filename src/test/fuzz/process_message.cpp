// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/consensus.h>
#include <net.h>
#include <net_processing.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <txorphanage.h>
#include <validationinterface.h>
#include <version.h>

#include <llmq/blockprocessor.h>
#include <llmq/chainlocks.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/quorums.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
const TestingSetup* g_setup;
std::string_view LIMIT_TO_MESSAGE_TYPE{};
} // namespace

void initialize_process_message()
{
    if (const auto val{std::getenv("LIMIT_TO_MESSAGE_TYPE")}) {
        LIMIT_TO_MESSAGE_TYPE = val;
        Assert(std::count(getAllNetMessageTypes().begin(), getAllNetMessageTypes().end(), LIMIT_TO_MESSAGE_TYPE)); // Unknown message type passed
    }

    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(
            /*chain_name=*/CBaseChainParams::REGTEST,
            /*extra_args=*/{"-txreconciliation"});
    g_setup = testing_setup.get();
    for (int i = 0; i < 2 * COINBASE_MATURITY; i++) {
        MineBlock(g_setup->m_node, CScript() << OP_TRUE);
    }
    SyncWithValidationInterfaceQueue();
}

FUZZ_TARGET(process_message, .init = initialize_process_message)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    ConnmanTestMsg& connman = *static_cast<ConnmanTestMsg*>(g_setup->m_node.connman.get());
    TestChainState& chainstate = *static_cast<TestChainState*>(&g_setup->m_node.chainman->ActiveChainstate());
    SetMockTime(1610000000); // any time to successfully reset ibd
    chainstate.ResetIbd();

    LOCK(NetEventsInterface::g_msgproc_mutex);

    const std::string random_message_type{fuzzed_data_provider.ConsumeBytesAsString(CMessageHeader::COMMAND_SIZE).c_str()};
    if (!LIMIT_TO_MESSAGE_TYPE.empty() && random_message_type != LIMIT_TO_MESSAGE_TYPE) {
        return;
    }
    CNode& p2p_node = *ConsumeNodeAsUniquePtr(fuzzed_data_provider).release();

    connman.AddTestNode(p2p_node);
    FillNode(fuzzed_data_provider, connman, p2p_node);

    const auto mock_time = ConsumeTime(fuzzed_data_provider);
    SetMockTime(mock_time);

    // fuzzed_data_provider is fully consumed after this call, don't use it
    CDataStream random_bytes_data_stream{fuzzed_data_provider.ConsumeRemainingBytes<unsigned char>(), SER_NETWORK, PROTOCOL_VERSION};
    try {
        g_setup->m_node.peerman->ProcessMessage(p2p_node, random_message_type, random_bytes_data_stream, GetTime<std::chrono::microseconds>(), std::atomic<bool>{false});
    } catch (const std::ios_base::failure& e) {
    }
    g_setup->m_node.peerman->SendMessages(&p2p_node);
    SyncWithValidationInterfaceQueue();
    g_setup->m_node.connman->StopNodes();
}
