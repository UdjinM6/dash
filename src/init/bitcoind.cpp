// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <interfaces/coinjoin.h>
#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <node/context.h>
#include <util/check.h>
#include <util/system.h>

#include <memory>

using node::NodeContext;

namespace init {
namespace {
class BitcoindInit : public interfaces::Init
{
public:
    BitcoindInit(NodeContext& node) : m_node(node)
    {
        m_node.args = &gArgs;
        m_node.init = this;
    }
    std::unique_ptr<interfaces::Node> makeNode() override { return interfaces::MakeNode(m_node); }
    std::unique_ptr<interfaces::Chain> makeChain() override { return interfaces::MakeChain(m_node); }
    std::unique_ptr<interfaces::CoinJoin::Loader> makeCoinJoinLoader() override
    {
        return interfaces::MakeCoinJoinLoader(m_node);
    }
    std::unique_ptr<interfaces::WalletLoader> makeWalletLoader(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader) override
    {
        return MakeWalletLoader(chain, *Assert(m_node.args), m_node, coinjoin_loader);
    }
    std::unique_ptr<interfaces::Echo> makeEcho() override { return interfaces::MakeEcho(); }
    NodeContext& m_node;
};
} // namespace
} // namespace init

namespace interfaces {
std::unique_ptr<Init> MakeNodeInit(NodeContext& node, int argc, char* argv[], int& exit_status)
{
    return std::make_unique<init::BitcoindInit>(node);
}
} // namespace interfaces
