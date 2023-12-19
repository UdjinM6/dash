// Copyright (c) 2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <llmq/context.h>
#include <llmq/params.h>
#include <llmq/quorums.h>
#include <llmq/utils.h>

#include <chainparams.h>

#include <validation.h>

#include <boost/test/unit_test.hpp>

/* TODO: rename this file and test to llmq_utils_test */
BOOST_AUTO_TEST_SUITE(evo_utils_tests)

void Test(llmq::CQuorumManager& qman, NodeContext& node)
{
    using namespace llmq::utils;
    auto tip = node.chainman->ActiveTip();
    const auto& consensus_params = Params().GetConsensus();
    const auto& llmq_params_DIP00024IS_opt = llmq::GetLLMQParams(consensus_params.llmqTypeDIP0024InstantSend);
    const auto& llmq_params_ChainLocks_opt = llmq::GetLLMQParams(consensus_params.llmqTypeChainLocks);
    const auto& llmq_params_Platform_opt = llmq::GetLLMQParams(consensus_params.llmqTypePlatform);
    const auto& llmq_params_Mnhf_opt = llmq::GetLLMQParams(consensus_params.llmqTypeMnhf);
    assert(llmq_params_DIP00024IS_opt.has_value());
    assert(llmq_params_ChainLocks_opt.has_value());
    assert(llmq_params_Platform_opt.has_value());
    assert(llmq_params_Mnhf_opt.has_value());
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_DIP00024IS_opt, qman, tip, false, false), false);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_DIP00024IS_opt, qman, tip, true, false), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_DIP00024IS_opt, qman, tip, true, true), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_ChainLocks_opt, qman, tip, false, false), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_ChainLocks_opt, qman, tip, true, false), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_ChainLocks_opt, qman, tip, true, true), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Platform_opt, qman, tip, false, false), Params().IsTestChain());
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Platform_opt, qman, tip, true, false), Params().IsTestChain());
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Platform_opt, qman, tip, true, true), Params().IsTestChain());
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Mnhf_opt, qman, tip, false, false), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Mnhf_opt, qman, tip, true, false), true);
    BOOST_CHECK_EQUAL(IsQuorumTypeEnabledInternal(*llmq_params_Mnhf_opt, qman, tip, true, true), true);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_regtest, RegTestingSetup)
{
    assert(m_node.llmq_ctx->qman);
    Test(*m_node.llmq_ctx->qman, m_node);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_mainnet, TestingSetup)
{
    assert(m_node.llmq_ctx->qman);
    Test(*m_node.llmq_ctx->qman, m_node);
}

BOOST_AUTO_TEST_SUITE_END()
