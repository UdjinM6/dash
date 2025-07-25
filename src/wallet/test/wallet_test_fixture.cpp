// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/wallet_test_fixture.h>

#include <scheduler.h>

namespace wallet {
WalletTestingSetup::WalletTestingSetup(const std::string& chainName)
    : TestingSetup(chainName),
      m_coinjoin_loader{interfaces::MakeCoinJoinLoader(m_node)},
      m_wallet_loader{interfaces::MakeWalletLoader(*m_node.chain, *Assert(m_node.args), m_node, *Assert(m_coinjoin_loader))},
      m_wallet(m_node.chain.get(), m_coinjoin_loader.get(), "", m_args, CreateMockWalletDatabase())
{
    m_wallet.LoadWallet();
    m_chain_notifications_handler = m_node.chain->handleNotifications({ &m_wallet, [](CWallet*) {} });
    m_wallet_loader->registerRpcs();
}

WalletTestingSetup::~WalletTestingSetup()
{
    if (m_node.scheduler) m_node.scheduler->stop();
}
} // namespace wallet
