// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <wallet/wallettool.h>

#include <fs.h>
#include <util/translation.h>
#include <util/system.h>
#include <wallet/dump.h>
#include <wallet/salvage.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

namespace wallet {
namespace WalletTool {

// The standard wallet deleter function blocks on the validation interface
// queue, which doesn't exist for the dash-wallet. Define our own
// deleter here.
static void WalletToolReleaseWallet(CWallet* wallet)
{
    wallet->WalletLogPrintf("Releasing wallet\n");
    wallet->Close();
    delete wallet;
}

static const bool DEFAULT_USE_HD_WALLET{true};

static void WalletCreate(CWallet* wallet_instance, uint64_t wallet_creation_flags)
{
    LOCK(wallet_instance->cs_wallet);
    if (gArgs.GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET)) {
        wallet_instance->SetMinVersion(FEATURE_LATEST);
    } else {
        wallet_instance->SetMinVersion(FEATURE_COMPRPUBKEY);
    }
    wallet_instance->InitWalletFlags(wallet_creation_flags);

    if (!wallet_instance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        // TODO: use here SetupGeneration instead, such as: spk_man->SetupGeneration(false);
        // SetupGeneration is not backported yet
        wallet_instance->SetupLegacyScriptPubKeyMan();
        auto spk_man = wallet_instance->GetOrCreateLegacyScriptPubKeyMan();
        if (gArgs.GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET)) {
            spk_man->GenerateNewHDChain(/*secureMnemonic=*/"", /*secureMnemonicPassphrase=*/"");
        }
    } else {
        wallet_instance->SetupDescriptorScriptPubKeyMans("", "");
    }

    tfm::format(std::cout, "Topping up keypool...\n");
    wallet_instance->TopUpKeyPool();
}

static const std::shared_ptr<CWallet> MakeWallet(const std::string& name, const fs::path& path, const ArgsManager& args, DatabaseOptions options)
{
    DatabaseStatus status;
    bilingual_str error;
    std::unique_ptr<WalletDatabase> database = MakeDatabase(path, options, status, error);
    if (!database) {
        tfm::format(std::cerr, "%s\n", error.original);
        return nullptr;
    }

    // dummy chain interface
    std::shared_ptr<CWallet> wallet_instance{new CWallet(/*chain=*/nullptr, /*coinjoin_loader=*/nullptr, name, args, std::move(database)), WalletToolReleaseWallet};
    DBErrors load_wallet_ret;
    try {
        load_wallet_ret = wallet_instance->LoadWallet();
    } catch (const std::runtime_error&) {
        tfm::format(std::cerr, "Error loading %s. Is wallet being used by another process?\n", name);
        return nullptr;
    }

    if (load_wallet_ret != DBErrors::LOAD_OK) {
        wallet_instance = nullptr;
        if (load_wallet_ret == DBErrors::CORRUPT) {
            tfm::format(std::cerr, "Error loading %s: Wallet corrupted", name);
            return nullptr;
        } else if (load_wallet_ret == DBErrors::NONCRITICAL_ERROR) {
            tfm::format(std::cerr, "Error reading %s! All keys read correctly, but transaction data"
                            " or address book entries might be missing or incorrect.",
                name);
        } else if (load_wallet_ret == DBErrors::TOO_NEW) {
            tfm::format(std::cerr, "Error loading %s: Wallet requires newer version of %s",
                name, PACKAGE_NAME);
            return nullptr;
        } else if (load_wallet_ret == DBErrors::NEED_REWRITE) {
            tfm::format(std::cerr, "Wallet needed to be rewritten: restart %s to complete", PACKAGE_NAME);
            return nullptr;
        } else {
            tfm::format(std::cerr, "Error loading %s", name);
            return nullptr;
        }
    }

    if (options.require_create) WalletCreate(wallet_instance.get(), options.create_flags);

    return wallet_instance;
}

static void WalletShowInfo(CWallet* wallet_instance)
{
    // lock required because of some AssertLockHeld()
    LOCK(wallet_instance->cs_wallet);

    CHDChain hdChainTmp;
    tfm::format(std::cout, "Wallet info\n===========\n");
    tfm::format(std::cout, "Name: %s\n", wallet_instance->GetName());
    tfm::format(std::cout, "Format: %s\n", wallet_instance->GetDatabase().Format());
    tfm::format(std::cout, "Descriptors: %s\n", wallet_instance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) ? "yes" : "no");
    tfm::format(std::cout, "Encrypted: %s\n", wallet_instance->IsCrypted() ? "yes" : "no");
    tfm::format(std::cout, "HD (hd seed available): %s\n", wallet_instance->IsHDEnabled() ? "yes" : "no");
    tfm::format(std::cout, "Keypool Size: %u\n", wallet_instance->GetKeyPoolSize());
    tfm::format(std::cout, "Transactions: %zu\n", wallet_instance->mapWallet.size());
    tfm::format(std::cout, "Address Book: %zu\n", wallet_instance->m_address_book.size());
}

bool ExecuteWalletToolFunc(const ArgsManager& args, const std::string& command)
{
    if (args.IsArgSet("-format") && command != "createfromdump") {
        tfm::format(std::cerr, "The -format option can only be used with the \"createfromdump\" command.\n");
        return false;
    }
    if (args.IsArgSet("-dumpfile") && command != "dump" && command != "createfromdump") {
        tfm::format(std::cerr, "The -dumpfile option can only be used with the \"dump\" and \"createfromdump\" commands.\n");
        return false;
    }
    if (args.IsArgSet("-descriptors") && command != "create") {
        tfm::format(std::cerr, "The -descriptors option can only be used with the 'create' command.\n");
        return false;
    }
    if (command == "create" && !args.IsArgSet("-wallet")) {
        tfm::format(std::cerr, "Wallet name must be provided when creating a new wallet.\n");
        return false;
    }
    const std::string name = args.GetArg("-wallet", "");
    const fs::path path = fsbridge::AbsPathJoin(GetWalletDir(), fs::PathFromString(name));

    if (command == "create") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_create = true;
        if (args.GetBoolArg("-descriptors", false)) {
            options.create_flags |= WALLET_FLAG_DESCRIPTORS;
            options.require_format = DatabaseFormat::SQLITE;
        }

        const std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, args, options);
        if (wallet_instance) {
            WalletShowInfo(wallet_instance.get());
            wallet_instance->Close();
        }
    } else if (command == "info") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        const std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, args, options);
        if (!wallet_instance) return false;
        WalletShowInfo(wallet_instance.get());
        wallet_instance->Close();
    } else if (command == "salvage") {
#ifdef USE_BDB
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        bool ret = RecoverDatabaseFile(args, path, error, warnings);
        if (!ret) {
            for (const auto& warning : warnings) {
                tfm::format(std::cerr, "%s\n", warning.original);
            }
            if (!error.empty()) {
                tfm::format(std::cerr, "%s\n", error.original);
            }
        }
        return ret;
#else
        tfm::format(std::cerr, "Salvage command is not available as BDB support is not compiled");
        return false;
#endif
    } else if (command == "wipetxes") {
#ifdef USE_BDB
        DatabaseOptions options;
        options.require_existing = true;
        const std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, args, options);
        if (wallet_instance == nullptr) return false;

        std::vector<uint256> vHash;
        std::vector<uint256> vHashOut;

        LOCK(wallet_instance->cs_wallet);

        for (auto& [txid, _] : wallet_instance->mapWallet) {
            vHash.push_back(txid);
        }

        if (wallet_instance->ZapSelectTx(vHash, vHashOut) != DBErrors::LOAD_OK) {
            tfm::format(std::cerr, "Could not properly delete transactions");
            wallet_instance->Close();
            return false;
        }

        wallet_instance->Close();
        return vHashOut.size() == vHash.size();
#else
        tfm::format(std::cerr, "Wipetxes command is not available as BDB support is not compiled");
        return false;
#endif
    } else if (command == "dump") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        const std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, args, options);
        if (!wallet_instance) return false;
        bilingual_str error;
        bool ret = DumpWallet(args, *wallet_instance, error);
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
            return ret;
        }
        tfm::format(std::cout, "The dumpfile may contain private keys. To ensure the safety of your Bitcoin, do not share the dumpfile.\n");
        return ret;
    } else if (command == "createfromdump") {
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        bool ret = CreateFromDump(args, name, path, error, warnings);
        for (const auto& warning : warnings) {
            tfm::format(std::cout, "%s\n", warning.original);
        }
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
        }
        return ret;
    } else {
        tfm::format(std::cerr, "Invalid command: %s\n", command);
        return false;
    }

    return true;
}
} // namespace WalletTool
} // namespace wallet
