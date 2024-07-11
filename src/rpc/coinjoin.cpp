// Copyright (c) 2019-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/context.h>
#include <validation.h>
#include <coinjoin/context.h>
#include <coinjoin/server.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <util/strencodings.h>

#ifdef ENABLE_WALLET
#include <coinjoin/client.h>
#include <coinjoin/options.h>
#include <interfaces/coinjoin.h>
#include <wallet/rpcwallet.h>
#endif // ENABLE_WALLET

#include <univalue.h>

#ifdef ENABLE_WALLET
static RPCHelpMan coinjoin()
{
            return RPCHelpMan{"coinjoin",
                "\nAvailable commands:\n"
                "  start       - Start mixing\n"
                "  stop        - Stop mixing\n"
                "  reset       - Reset mixing",
                {
                    {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to execute"},
                },
                RPCResults{},
                RPCExamples{""},
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;

    const NodeContext& node = EnsureAnyNodeContext(request.context);

    if (node.mn_activeman) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Client-side mixing is not supported on masternodes");
    }

    if (!CCoinJoinClientOptions::IsEnabled()) {
        if (!gArgs.GetBoolArg("-enablecoinjoin", true)) {
            // otherwise it's on by default, unless cmd line option says otherwise
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Mixing is disabled via -enablecoinjoin=0 command line option, remove it to enable mixing again");
        } else {
            // not enablecoinjoin=false case,
            // most likely something bad happened and we disabled it while running the wallet
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Mixing is disabled due to some internal error");
        }
    }

    auto cj_clientman = node.coinjoin_loader->walletman().Get(wallet->GetName());
    CHECK_NONFATAL(cj_clientman != nullptr);

    if (request.params[0].get_str() == "start") {
        {
            LOCK(wallet->cs_wallet);
            if (wallet->IsLocked(true))
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please unlock wallet for mixing with walletpassphrase first.");
        }

        if (!cj_clientman->StartMixing()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Mixing has been started already.");
        }

        const ChainstateManager& chainman = EnsureChainman(node);
        CTxMemPool& mempool = EnsureMemPool(node);
        CConnman& connman = EnsureConnman(node);
        bool result = cj_clientman->DoAutomaticDenominating(chainman.ActiveChainstate(), connman, mempool);
        return "Mixing " + (result ? "started successfully" : ("start failed: " + cj_clientman->GetStatuses().original + ", will retry"));
    }

    if (request.params[0].get_str() == "stop") {
        cj_clientman->StopMixing();
        return "Mixing was stopped";
    }

    if (request.params[0].get_str() == "reset") {
        cj_clientman->ResetPool();
        return "Mixing was reset";
    }

    return "Unknown command, please see \"help coinjoin\"";
},
    };
}

// From wallet/rpcdump.cpp
extern void RescanWallet(CWallet& wallet, const WalletRescanReserver& reserver, int64_t time_begin = 0, bool update = true);

static RPCHelpMan coinjoinsalt()
{
    return RPCHelpMan{"coinjoinsalt",
        "\nAvailable commands:\n"
        "  generate  - Generate new CoinJoin salt\n"
        "  get       - Fetch existing CoinJoin salt\n"
        "  set       - Set new CoinJoin salt\n",
        {
            {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to execute"},
        },
        RPCResults{},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    throw JSONRPCError(RPC_INVALID_PARAMETER, "Must be a valid command");
},
    };
}

static RPCHelpMan coinjoinsalt_generate()
{
    return RPCHelpMan{"coinjoinsalt generate",
        "\nGenerate new CoinJoin salt and commit to wallet database\n"
        "Cannot generate new salt if CoinJoin mixing is in process or wallet has private keys disabled.\n"
        "The presence of a CoinJoin balance will cause the wallet to rescan.\n",
        {
            {"overwrite", RPCArg::Type::BOOL, /* default */ "false", "Generate new salt even if there is an existing salt and/or there is CoinJoin balance"},
            {"force_rescan", RPCArg::Type::BOOL, /* default */ "false", "Force wallet rescan"}
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "Status of CoinJoin salt generation and commitment"
        },
        RPCExamples{
            HelpExampleCli("coinjoinsalt generate", "")
          + HelpExampleRpc("coinjoinsalt generate", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;

    const auto str_wallet = wallet->GetName();
    if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Wallet \"%s\" has private keys disabled, cannot perform CoinJoin!", str_wallet));
    }

    const bool enable_overwrite{!request.params[0].isNull() ? request.params[0].get_bool() : /* default */ false};
    if (!enable_overwrite && !wallet->GetCoinJoinSalt().IsNull()) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Wallet \"%s\" already has set CoinJoin salt!", str_wallet));
    }

    const NodeContext& node = EnsureAnyNodeContext(request.context);
    if (node.coinjoin_loader != nullptr) {
        auto cj_clientman = node.coinjoin_loader->walletman().Get(wallet->GetName());
        if (cj_clientman != nullptr && cj_clientman->IsMixing()) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               strprintf("Wallet \"%s\" is currently mixing, cannot change salt!", str_wallet));
        }
    }

    const auto wallet_balance{wallet->GetBalance()};
    const bool has_balance{(wallet_balance.m_anonymized
                          + wallet_balance.m_denominated_trusted
                          + wallet_balance.m_denominated_untrusted_pending) > 0};
    if (!enable_overwrite && has_balance) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           strprintf("Wallet \"%s\" has CoinJoin balance, cannot continue!", str_wallet));
    }

    const bool rescan{has_balance || (!request.params[1].isNull() ? request.params[1].get_bool() : /* default */ false)};
    WalletRescanReserver reserver(*wallet);
    if (rescan) {
        if (wallet->chain().havePruned()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled when blocks are pruned");
        }

        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
        }
    }

    if (!wallet->SetCoinJoinSalt(GetRandHash())) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Unable to set new CoinJoin salt for wallet \"%s\"!", str_wallet));
    }

    if (rescan) {
        RescanWallet(*wallet, reserver);
    }

    return true;
},
    };
}

static RPCHelpMan coinjoinsalt_get()
{
    return RPCHelpMan{"coinjoinsalt get",
        "\nFetch existing CoinJoin salt\n"
        "Cannot fetch salt if wallet has private keys disabled.\n",
        {},
        RPCResult{
            RPCResult::Type::STR_HEX, "", "CoinJoin salt"
        },
        RPCExamples{
            HelpExampleCli("coinjoinsalt get", "")
          + HelpExampleRpc("coinjoinsalt get", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;

    const auto str_wallet = wallet->GetName();
    if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Wallet \"%s\" has private keys disabled, cannot perform CoinJoin!", str_wallet));
    }

    const auto salt{wallet->GetCoinJoinSalt()};
    if (salt.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           strprintf("Wallet \"%s\" has no CoinJoin salt!", str_wallet));
    }
    return salt.GetHex();
},
    };
}

static RPCHelpMan coinjoinsalt_set()
{
    return RPCHelpMan{"coinjoinsalt set",
        "\nSet new CoinJoin salt\n"
        "Cannot set salt if CoinJoin mixing is in process or wallet has private keys disabled.\n"
        "Will overwrite existing salt. The presence of a CoinJoin balance will cause the wallet to rescan.\n",
        {
            {"salt", RPCArg::Type::STR, RPCArg::Optional::NO, "Desired CoinJoin salt value for the wallet"},
            {"force_overwrite", RPCArg::Type::BOOL, /* default */ "false", "Overwrite salt even if CoinJoin balance present"},
            {"force_rescan", RPCArg::Type::BOOL, /* default */ "false", "Force wallet rescan"}
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "Status of CoinJoin salt change request"
        },
        RPCExamples{
            HelpExampleCli("coinjoinsalt set", "f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16")
          + HelpExampleRpc("coinjoinsalt set", "f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;

    const auto salt{ParseHashV(request.params[0], "salt")};
    const bool enable_overwrite{!request.params[1].isNull() ? request.params[1].get_bool() : /* default */ false};
    if (salt == uint256::ZERO) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Illegal CoinJoin salt value");
    }

    const auto str_wallet = wallet->GetName();
    if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Wallet \"%s\" has private keys disabled, cannot perform CoinJoin!", str_wallet));
    }

    const NodeContext& node = EnsureAnyNodeContext(request.context);
    if (node.coinjoin_loader != nullptr) {
        auto cj_clientman = node.coinjoin_loader->walletman().Get(wallet->GetName());
        if (cj_clientman != nullptr && cj_clientman->IsMixing()) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               strprintf("Wallet \"%s\" is currently mixing, cannot change salt!", str_wallet));
        }
    }

    const auto wallet_balance{wallet->GetBalance()};
    const bool has_balance{(wallet_balance.m_anonymized
                          + wallet_balance.m_denominated_trusted
                          + wallet_balance.m_denominated_untrusted_pending) > 0};
    if (has_balance && !enable_overwrite) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           strprintf("Wallet \"%s\" has CoinJoin balance, cannot continue!", str_wallet));
    }

    const bool rescan{has_balance || (!request.params[2].isNull() ? request.params[2].get_bool() : /* default */ false)};
    WalletRescanReserver reserver(*wallet);
    if (rescan) {
        if (wallet->chain().havePruned()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled when blocks are pruned");
        }

        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
        }
    }

    if (!wallet->SetCoinJoinSalt(salt)) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
                           strprintf("Unable to set new CoinJoin salt for wallet \"%s\"!", str_wallet));
    }

    if (rescan) {
        RescanWallet(*wallet, reserver);
    }

    return true;
},
    };
}
#endif // ENABLE_WALLET

static RPCHelpMan getpoolinfo()
{
    return RPCHelpMan{"getpoolinfo",
                "DEPRECATED. Please use getcoinjoininfo instead.\n",
                {},
                RPCResults{},
                RPCExamples{""},
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "Please use getcoinjoininfo instead");
},
    };
}

static RPCHelpMan getcoinjoininfo()
{
            return RPCHelpMan{"getcoinjoininfo",
                "Returns an object containing an information about CoinJoin settings and state.\n",
                {},
                {
                    RPCResult{"for regular nodes",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::BOOL, "enabled", "Whether mixing functionality is enabled"},
                            {RPCResult::Type::BOOL, "multisession", "Whether CoinJoin Multisession option is enabled"},
                            {RPCResult::Type::NUM, "max_sessions", "How many parallel mixing sessions can there be at once"},
                            {RPCResult::Type::NUM, "max_rounds", "How many rounds to mix"},
                            {RPCResult::Type::NUM, "max_amount", "Target CoinJoin balance in " + CURRENCY_UNIT + ""},
                            {RPCResult::Type::NUM, "denoms_goal", "How many inputs of each denominated amount to target"},
                            {RPCResult::Type::NUM, "denoms_hardcap", "Maximum limit of how many inputs of each denominated amount to create"},
                            {RPCResult::Type::NUM, "queue_size", "How many queues there are currently on the network"},
                            {RPCResult::Type::BOOL, "running", "Whether mixing is currently running"},
                            {RPCResult::Type::ARR, "sessions", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::STR_HEX, "protxhash", "The ProTxHash of the masternode"},
                                    {RPCResult::Type::STR_HEX, "outpoint", "The outpoint of the masternode"},
                                    {RPCResult::Type::STR, "service", "The IP address and port of the masternode"},
                                    {RPCResult::Type::NUM, "denomination", "The denomination of the mixing session in " + CURRENCY_UNIT + ""},
                                    {RPCResult::Type::STR_HEX, "state", "Current state of the mixing session"},
                                    {RPCResult::Type::NUM, "entries_count", "The number of entries in the mixing session"},
                                }},
                            }},
                            {RPCResult::Type::NUM, "keys_left", "How many new keys are left since last automatic backup"},
                            {RPCResult::Type::STR, "warnings", "Warnings if any"},
                        }},
                    RPCResult{"for masternodes",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "queue_size", "How many queues there are currently on the network"},
                            {RPCResult::Type::NUM, "denomination", "The denomination of the mixing session in " + CURRENCY_UNIT + ""},
                            {RPCResult::Type::STR_HEX, "state", "Current state of the mixing session"},
                            {RPCResult::Type::NUM, "entries_count", "The number of entries in the mixing session"},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getcoinjoininfo", "")
            + HelpExampleRpc("getcoinjoininfo", "")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue obj(UniValue::VOBJ);

    const NodeContext& node = EnsureAnyNodeContext(request.context);
    if (node.mn_activeman) {
        node.cj_ctx->server->GetJsonInfo(obj);
        return obj;
    }

#ifdef ENABLE_WALLET
    CCoinJoinClientOptions::GetJsonInfo(obj);

    obj.pushKV("queue_size", node.cj_ctx->queueman->GetQueueSize());

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) {
        return obj;
    }

    auto manager = node.coinjoin_loader->walletman().Get(wallet->GetName());
    CHECK_NONFATAL(manager != nullptr);
    manager->GetJsonInfo(obj);

    obj.pushKV("keys_left",     wallet->nKeysLeftSinceAutoBackup);
    obj.pushKV("warnings",      wallet->nKeysLeftSinceAutoBackup < COINJOIN_KEYS_THRESHOLD_WARNING
                                        ? "WARNING: keypool is almost depleted!" : "");
#endif // ENABLE_WALLET

    return obj;
},
    };
}

void RegisterCoinJoinRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
    { //  category              name                          actor (function)         argNames
      //  ------------------------------------------------------------------------------------------------------
        { "dash",               "getpoolinfo",                &getpoolinfo,            {} },
        { "dash",               "getcoinjoininfo",            &getcoinjoininfo,        {} },
#ifdef ENABLE_WALLET
        { "dash",               "coinjoin",                   &coinjoin,               {"command"} },
        { "dash",               "coinjoinsalt",               &coinjoinsalt,           {"command"} },
        { "dash",               "coinjoinsalt", "generate",   &coinjoinsalt_generate,  {"overwrite", "force_rescan"} },
        { "dash",               "coinjoinsalt", "get",        &coinjoinsalt_get,       {} },
        { "dash",               "coinjoinsalt", "set",        &coinjoinsalt_set,       {"salt", "force_overwrite", "force_rescan"} },
#endif // ENABLE_WALLET
};
// clang-format on
    for (const auto& command : commands) {
        t.appendCommand(command.name, command.subname, &command);
    }
}
