// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <clientversion.h>
#include <core_io.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <merkleblock.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/standard.h>
#include <sync.h>
#include <util/bip32.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/rpc/util.h>

#include <cstdint>
#include <fstream>
#include <tuple>
#include <string>

#include <univalue.h>

using interfaces::FoundBlock;

namespace wallet {
std::string static EncodeDumpString(const std::string &str) {
    std::stringstream ret;
    for (const unsigned char c : str) {
        if (c <= 32 || c >= 128 || c == '%') {
            ret << '%' << HexStr({&c, 1});
        } else {
            ret << c;
        }
    }
    return ret.str();
}

static std::string DecodeDumpString(const std::string &str) {
    std::stringstream ret;
    for (unsigned int pos = 0; pos < str.length(); pos++) {
        unsigned char c = str[pos];
        if (c == '%' && pos+2 < str.length()) {
            c = (((str[pos+1]>>6)*9+((str[pos+1]-'0')&15)) << 4) |
                ((str[pos+2]>>6)*9+((str[pos+2]-'0')&15));
            pos += 2;
        }
        ret << c;
    }
    return ret.str();
}

static bool GetWalletAddressesForKey(const LegacyScriptPubKeyMan* spk_man, const CWallet& wallet, const CKeyID& keyid, std::string& strAddr, std::string& strLabel) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const PKHash& dest = PKHash(keyid);
    strAddr = EncodeDestination(dest);
    if (const auto* address_book_entry = wallet.FindAddressBookEntry(dest); address_book_entry != nullptr) {
        strLabel = EncodeDumpString(address_book_entry->GetLabel());
        return true;
    }
    return false;
}

static const int64_t TIMESTAMP_MIN = 0;

static void RescanWallet(CWallet& wallet, const WalletRescanReserver& reserver, int64_t time_begin = TIMESTAMP_MIN, bool update = true)
{
    int64_t scanned_time = wallet.RescanFromTime(time_begin, reserver, update);
    if (wallet.IsAbortingRescan()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
    } else if (scanned_time > time_begin) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan was unable to fully rescan the blockchain. Some transactions may be missing.");
    }
}

RPCHelpMan importprivkey()
{
    return RPCHelpMan{"importprivkey",
        "\nAdds a private key (as returned by dumpprivkey) to your wallet. Requires a new wallet backup.\n"
        "Hint: use importmulti to import more than one private key.\n"
    "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
    "may report that the imported key exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
    "Note: This command is only compatible with legacy wallets. Use \"importdescriptors\" with \"combo(X)\" for descriptor wallets.\n",
        {
            {"privkey", RPCArg::Type::STR, RPCArg::Optional::NO, "The private key (see dumpprivkey)"},
            {"label", RPCArg::Type::STR, RPCArg::DefaultHint{"current label if address exists, otherwise \"\""}, "An optional label"},
            {"rescan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Rescan the wallet for transactions"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
    "\nDump a private key\n"
    + HelpExampleCli("dumpprivkey", "\"myaddress\"") +
    "\nImport the private key with rescan\n"
    + HelpExampleCli("importprivkey", "\"mykey\"") +
    "\nImport using a label and without rescan\n"
    + HelpExampleCli("importprivkey", "\"mykey\" \"testing\" false") +
    "\nImport using default blank label and without rescan\n"
    + HelpExampleCli("importprivkey", "\"mykey\" \"\" false") +
    "\nAs a JSON-RPC call\n"
    + HelpExampleRpc("importprivkey", "\"mykey\", \"testing\", false")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
    }

    EnsureLegacyScriptPubKeyMan(*pwallet, true);

    WalletBatch batch(pwallet->GetDatabase());
    WalletRescanReserver reserver(*pwallet);
    bool fRescan = true;
    {
        LOCK(pwallet->cs_wallet);

        EnsureWalletIsUnlocked(*pwallet);

        std::string strSecret = request.params[0].get_str();
        std::string strLabel;
        if (!request.params[1].isNull())
            strLabel = request.params[1].get_str();

        // Whether to perform rescan after import
        if (!request.params[2].isNull())
            fRescan = request.params[2].get_bool();

        if (fRescan && pwallet->chain().havePruned()) {
            // Exit early and print an error.
            // If a block is pruned after this check, we will import the key(s),
            // but fail the rescan with a generic error.
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled when blocks are pruned");
        }

        if (fRescan && !reserver.reserve()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
        }

        CKey key = DecodeSecret(strSecret);
        if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

        CPubKey pubkey = key.GetPubKey();
        CHECK_NONFATAL(key.VerifyPubKey(pubkey));
        PKHash vchAddress = PKHash(pubkey);
        {
            pwallet->MarkDirty();

            if (!request.params[1].isNull() || !pwallet->FindAddressBookEntry(vchAddress)) {
                pwallet->SetAddressBook(vchAddress, strLabel, "receive");
            }

            // Use timestamp of 1 to scan the whole chain
            if (!pwallet->ImportPrivKeys({{ToKeyID(vchAddress), key}}, 1)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
            }
        }
    }
    if (fRescan) {
        RescanWallet(*pwallet, reserver);
    }
    return NullUniValue;
},
    };
}

RPCHelpMan abortrescan()
{
    return RPCHelpMan{"abortrescan",
                "\nStops current wallet rescan triggered by an RPC call, e.g. by an importprivkey call.\n",
                {},
                RPCResult{RPCResult::Type::BOOL, "", "Whether the abort was successful"},
                RPCExamples{
            "\nImport a private key\n"
            + HelpExampleCli("importprivkey", "\"mykey\"") +
            "\nAbort the running wallet rescan\n"
            + HelpExampleCli("abortrescan", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("abortrescan", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    if (!pwallet->IsScanning() || pwallet->IsAbortingRescan()) return false;
    pwallet->AbortRescan();
    return true;
},
    };
}

RPCHelpMan importaddress()
{
    return RPCHelpMan{"importaddress",
        "\nAdds an address or script (in hex) that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n"
    "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
    "may report that the imported address exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
    "If you have the full public key, you should call importpubkey instead of this.\n"
    "Hint: use importmulti to import more than one address.\n"
    "\nNote: If you import a non-standard raw script in hex form, outputs sending to it will be treated\n"
    "as change, and not show up in many RPCs.\n"
    "Note: This command is only compatible with legacy wallets. Use \"importdescriptors\" for descriptor wallets.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Dash address (or hex-encoded script)"},
            {"label", RPCArg::Type::STR, RPCArg::Default{""}, "An optional label"},
            {"rescan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Rescan the wallet for transactions"},
            {"p2sh", RPCArg::Type::BOOL, RPCArg::Default{false}, "Add the P2SH version of the script as well"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
    "\nImport an address with rescan\n"
    + HelpExampleCli("importaddress", "\"myaddress\"") +
    "\nImport using a label without rescan\n"
    + HelpExampleCli("importaddress", "\"myaddress\" \"testing\" false") +
    "\nAs a JSON-RPC call\n"
    + HelpExampleRpc("importaddress", "\"myaddress\", \"testing\", false")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    EnsureLegacyScriptPubKeyMan(*pwallet, true);

    std::string strLabel;
    if (!request.params[1].isNull())
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (!request.params[2].isNull())
        fRescan = request.params[2].get_bool();

    if (fRescan && pwallet->chain().havePruned()) {
        // Exit early and print an error.
        // If a block is pruned after this check, we will import the key(s),
        // but fail the rescan with a generic error.
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled when blocks are pruned");
    }

    WalletRescanReserver reserver(*pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    // Whether to import a p2sh version, too
    bool fP2SH = false;
    if (!request.params[3].isNull())
        fP2SH = request.params[3].get_bool();

    {
        LOCK(pwallet->cs_wallet);

        CTxDestination dest = DecodeDestination(request.params[0].get_str());
        if (IsValidDestination(dest)) {
            if (fP2SH) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot use the p2sh flag with an address - use a script instead");
            }

            pwallet->MarkDirty();

            pwallet->ImportScriptPubKeys(strLabel, {GetScriptForDestination(dest)}, false /* have_solving_data */, true /* apply_label */, 1 /* timestamp */);
        } else if (IsHex(request.params[0].get_str())) {
            std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
            CScript redeem_script(data.begin(), data.end());

            std::set<CScript> scripts = {redeem_script};
            pwallet->ImportScripts(scripts, 0 /* timestamp */);

            if (fP2SH) {
                scripts.insert(GetScriptForDestination(ScriptHash(redeem_script)));
            }

            pwallet->ImportScriptPubKeys(strLabel, scripts, false /* have_solving_data */, true /* apply_label */, 1 /* timestamp */);
        } else {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Dash address or script");
        }
    }
    if (fRescan)
    {
        RescanWallet(*pwallet, reserver);
        {
            LOCK(pwallet->cs_wallet);
            pwallet->ReacceptWalletTransactions();
        }
    }

    return NullUniValue;
},
    };
}

RPCHelpMan importprunedfunds()
{
    return RPCHelpMan{"importprunedfunds",
        "\nImports funds without rescan. Corresponding address or script must previously be included in wallet. Aimed towards pruned wallets. The end-user is responsible to import additional transactions that subsequently spend the imported outputs or rescan after the point in the blockchain the transaction is included.\n",
        {
            {"rawtransaction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A raw transaction in hex funding an already-existing address in wallet"},
            {"txoutproof", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex output from gettxoutproof that contains the transaction"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
    }
    uint256 hashTx = tx.GetHash();

    CDataStream ssMB(ParseHexV(request.params[1], "proof"), SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock;

    //Search partial merkle tree in proof for our transaction and index in valid block
    std::vector<uint256> vMatch;
    std::vector<unsigned int> vIndex;
    if (merkleBlock.txn.ExtractMatches(vMatch, vIndex) != merkleBlock.header.hashMerkleRoot) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Something wrong with merkleblock");
    }

    LOCK(pwallet->cs_wallet);
    int height;
    if (!pwallet->chain().findAncestorByHash(pwallet->GetLastBlockHash(), merkleBlock.header.GetHash(), FoundBlock().height(height))) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");
    }

    std::vector<uint256>::const_iterator it;
    if ((it = std::find(vMatch.begin(), vMatch.end(), hashTx)) == vMatch.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction given doesn't exist in proof");
    }

    unsigned int txnIndex = vIndex[it - vMatch.begin()];

    CTransactionRef tx_ref = MakeTransactionRef(tx);
    if (pwallet->IsMine(*tx_ref)) {
        pwallet->AddToWallet(std::move(tx_ref), TxStateConfirmed{merkleBlock.header.GetHash(), height, static_cast<int>(txnIndex)});
        return NullUniValue;
    }

    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No addresses in wallet correspond to included transaction");
},
    };
}

RPCHelpMan removeprunedfunds()
{
    return RPCHelpMan{"removeprunedfunds",
        "\nDeletes the specified transaction from the wallet. Meant for use with pruned wallets and as a companion to importprunedfunds. This will affect wallet balances.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex-encoded id of the transaction you are deleting"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
            HelpExampleCli("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"") +
    "\nAs a JSON-RPC call\n"
    + HelpExampleRpc("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    LOCK(pwallet->cs_wallet);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    std::vector<uint256> vHash;
    vHash.push_back(hash);
    std::vector<uint256> vHashOut;

    if (pwallet->ZapSelectTx(vHash, vHashOut) != DBErrors::LOAD_OK) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Could not properly delete the transaction.");
    }

    if(vHashOut.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Transaction does not exist in wallet.");
    }

    return NullUniValue;
},
    };
}

RPCHelpMan importpubkey()
{
    return RPCHelpMan{"importpubkey",
        "\nAdds a public key (in hex) that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n"
        "Hint: use importmulti to import more than one public key.\n"
    "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
    "may report that the imported pubkey exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
    "Note: This command is only compatible with legacy wallets. Use \"importdescriptors\" with \"combo(X)\" for descriptor wallets.\n",
        {
            {"pubkey", RPCArg::Type::STR, RPCArg::Optional::NO, "The hex-encoded public key"},
            {"label", RPCArg::Type::STR, RPCArg::Default{""}, "An optional label"},
            {"rescan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Rescan the wallet for transactions"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
    "\nImport a public key with rescan\n"
    + HelpExampleCli("importpubkey", "\"mypubkey\"") +
    "\nImport using a label without rescan\n"
    + HelpExampleCli("importpubkey", "\"mypubkey\" \"testing\" false") +
    "\nAs a JSON-RPC call\n"
    + HelpExampleRpc("importpubkey", "\"mypubkey\", \"testing\", false")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    EnsureLegacyScriptPubKeyMan(*pwallet, true);

    std::string strLabel;
    if (!request.params[1].isNull())
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (!request.params[2].isNull())
        fRescan = request.params[2].get_bool();

    if (fRescan && pwallet->chain().havePruned()) {
        // Exit early and print an error.
        // If a block is pruned after this check, we will import the key(s),
        // but fail the rescan with a generic error.
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled when blocks are pruned");
    }

    WalletRescanReserver reserver(*pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    if (!IsHex(request.params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey must be a hex string");
    std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
    CPubKey pubKey(data);
    if (!pubKey.IsFullyValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey is not a valid public key");

    {
        LOCK(pwallet->cs_wallet);

        std::set<CScript> script_pub_keys;
        script_pub_keys.insert(GetScriptForDestination(PKHash(pubKey)));

        pwallet->MarkDirty();

        pwallet->ImportScriptPubKeys(strLabel, script_pub_keys, true /* have_solving_data */, true /* apply_label */, 1 /* timestamp */);

        pwallet->ImportPubKeys({pubKey.GetID()}, {{pubKey.GetID(), pubKey}} , {} /* key_origins */, false /* add_keypool */, false /* internal */, 1 /* timestamp */);
    }
    if (fRescan)
    {
        RescanWallet(*pwallet, reserver);
        {
            LOCK(pwallet->cs_wallet);
            pwallet->ReacceptWalletTransactions();
        }
    }

    return NullUniValue;
},
    };
}

RPCHelpMan importwallet()
{
    return RPCHelpMan{"importwallet",
        "\nImports keys from a wallet dump file (see dumpwallet). Requires a new wallet backup to include imported keys.\n"
        "Note: Use \"getwalletinfo\" to query the scanning progress.\n"
        "Note: This command is only compatible with legacy wallets.\n",
        {
            {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet file"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
    "\nDump the wallet\n"
    + HelpExampleCli("dumpwallet", "\"test\"") +
    "\nImport the wallet\n"
    + HelpExampleCli("importwallet", "\"test\"") +
    "\nImport using the json rpc call\n"
    + HelpExampleRpc("importwallet", "\"test\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    EnsureLegacyScriptPubKeyMan(*pwallet, true);

    if (pwallet->chain().havePruned()) {
        // Exit early and print an error.
        // If a block is pruned after this check, we will import the key(s),
        // but fail the rescan with a generic error.
        throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled when blocks are pruned");
    }

    WalletBatch batch(pwallet->GetDatabase());
    WalletRescanReserver reserver(*pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t nTimeBegin = 0;
    bool fGood = true;
    {
        LOCK(pwallet->cs_wallet);

        EnsureWalletIsUnlocked(*pwallet);

        std::ifstream file;
        file.open(fs::u8path(request.params[0].get_str()), std::ios::in | std::ios::ate);
        if (!file.is_open()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");
        }
        CHECK_NONFATAL(pwallet->chain().findBlock(pwallet->GetLastBlockHash(), FoundBlock().time(nTimeBegin)));

        int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
        file.seekg(0, file.beg);

        // Use uiInterface.ShowProgress instead of pwallet.ShowProgress because pwallet.ShowProgress has a cancel button tied to AbortRescan which
        // we don't want for this progress bar showing the import progress. uiInterface.ShowProgress does not have a cancel button.
        pwallet->chain().showProgress(strprintf("%s " + _("Importing…").translated, pwallet->GetDisplayName()), 0, false); // show progress dialog in GUI
        std::vector<std::tuple<CKey, int64_t, bool, std::string>> keys;
        std::vector<std::pair<CScript, int64_t>> scripts;
        while (file.good()) {
            pwallet->chain().showProgress("", std::max(1, std::min(50, (int)(((double)file.tellg() / (double)nFilesize) * 100))), false);
            std::string line;
            std::getline(file, line);
            if (line.empty() || line[0] == '#')
                continue;

            std::vector<std::string> vstr = SplitString(line, ' ');
            if (vstr.size() < 2)
                continue;
            CKey key = DecodeSecret(vstr[0]);
            if (key.IsValid()) {
                int64_t nTime = ParseISO8601DateTime(vstr[1]);
                std::string strLabel;
                bool fLabel = true;
                for (unsigned int nStr = 2; nStr < vstr.size(); nStr++) {
                    if (vstr[nStr].front() == '#')
                        break;
                    if (vstr[nStr] == "change=1")
                        fLabel = false;
                    if (vstr[nStr] == "reserve=1")
                        fLabel = false;
                    if (vstr[nStr].substr(0,6) == "label=") {
                        strLabel = DecodeDumpString(vstr[nStr].substr(6));
                        fLabel = true;
                    }
                }
                keys.push_back(std::make_tuple(key, nTime, fLabel, strLabel));
            } else if(IsHex(vstr[0])) {
                std::vector<unsigned char> vData(ParseHex(vstr[0]));
                CScript script = CScript(vData.begin(), vData.end());
                int64_t birth_time = ParseISO8601DateTime(vstr[1]);
                scripts.push_back(std::pair<CScript, int64_t>(script, birth_time));
            }
        }
        file.close();
        // We now know whether we are importing private keys, so we can error if private keys are disabled
        if (keys.size() > 0 && pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            pwallet->chain().showProgress("", 100, false); // hide progress dialog in GUI
            throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled when private keys are disabled");
        }
        double total = (double)(keys.size() + scripts.size());
        double progress = 0;
        LegacyScriptPubKeyMan* spk_man = pwallet->GetLegacyScriptPubKeyMan();
        if (spk_man == nullptr) {
            if (total > 0) {
                throw JSONRPCError(RPC_WALLET_ERROR, "This type of wallet does not support this command");
            }
        } else {
            LOCK(spk_man->cs_KeyStore);
            for (const auto& key_tuple : keys) {
                pwallet->chain().showProgress("", std::max(50, std::min(75, (int)((progress / total) * 100) + 50)), false);
                const CKey& key = std::get<0>(key_tuple);
                int64_t time = std::get<1>(key_tuple);
                bool has_label = std::get<2>(key_tuple);
                std::string label = std::get<3>(key_tuple);

                CPubKey pubkey = key.GetPubKey();
                CHECK_NONFATAL(key.VerifyPubKey(pubkey));
                PKHash pkhash = PKHash(pubkey);
                CKeyID keyid = pubkey.GetID();
                pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(pkhash));
                if (!pwallet->ImportPrivKeys({{keyid, key}}, time)) {
                    pwallet->WalletLogPrintf("Error importing key for %s\n", EncodeDestination(pkhash));
                    fGood = false;
                    continue;
                }
                if (has_label)
                    pwallet->SetAddressBook(pkhash, label, "receive");

                nTimeBegin = std::min(nTimeBegin, time);
                progress++;
            }
            for (const auto& script_pair : scripts) {
                pwallet->chain().showProgress("", std::max(50, std::min(75, (int)((progress / total) * 100) + 50)), false);
                const CScript& script = script_pair.first;
                int64_t time = script_pair.second;
                if (!pwallet->ImportScripts({script}, time)) {
                    pwallet->WalletLogPrintf("Error importing script %s\n", HexStr(script));
                    fGood = false;
                    continue;
                }
                if (time > 0) {
                    nTimeBegin = std::min(nTimeBegin, time);
                }
                progress++;
            }
            pwallet->chain().showProgress("", 100, false); // hide progress dialog in GUI
        }
    }
    pwallet->chain().showProgress("", 100, false); // hide progress dialog in GUI
    RescanWallet(*pwallet, reserver, nTimeBegin, false /* update */);
    pwallet->MarkDirty();

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys/scripts to wallet");

    return NullUniValue;
},
    };
}

RPCHelpMan importelectrumwallet()
{
    return RPCHelpMan{"importelectrumwallet",
        "\nImports keys from an Electrum wallet export file (.csv or .json)\n"
        "Note: This command is only compatible with legacy wallets.\n",
        {
            {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The Electrum wallet export file, should be in csv or json format"},
            {"index", RPCArg::Type::NUM, RPCArg::Default{0}, "Rescan the wallet for transactions starting from this block index"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
    "\nImport the wallet\n"
    + HelpExampleCli("importelectrumwallet", "\"test.csv\"")
    + HelpExampleCli("importelectrumwallet", "\"test.json\"") +
    "\nImport using the json rpc call\n"
    + HelpExampleRpc("importelectrumwallet", "\"test.csv\"")
    + HelpExampleRpc("importelectrumwallet", "\"test.json\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    if (pwallet->chain().havePruned())
        throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled in pruned mode");

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

    LOCK2(pwallet->cs_wallet, spk_man.cs_KeyStore);

    EnsureWalletIsUnlocked(*pwallet);

    std::ifstream file;
    std::string strFileName = request.params[0].get_str();
    size_t nDotPos = strFileName.find_last_of(".");
    if(nDotPos == std::string::npos)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "File has no extension, should be .json or .csv");

    std::string strFileExt = strFileName.substr(nDotPos+1);
    if(strFileExt != "json" && strFileExt != "csv")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "File has wrong extension, should be .json or .csv");

    file.open(strFileName, std::ios::in | std::ios::ate);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open Electrum wallet export file");

    bool fGood = true;

    WalletBatch batch(pwallet->GetDatabase());

    int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
    file.seekg(0, file.beg);

    pwallet->ShowProgress(_("Importing…").translated, 0); // show progress dialog in GUI

    // Electrum backups were modified to include a prefix before the private key
    // The new format of the private_key field is: "prefix:private key"
    // Where prefix is, for example, "p2pkh" or "p2sh"
    if(strFileExt == "csv") {
        while (file.good()) {
            pwallet->ShowProgress("", std::max(1, std::min(99, (int)(((double)file.tellg() / (double)nFilesize) * 100))));
            std::string line;
            std::getline(file, line);
            if (line.empty() || line == "address,private_key")
                continue;
            std::vector<std::string> vstr = SplitString(line, ',');
            if (vstr.size() < 2)
                continue;
            std::vector<std::string> vstr2 = SplitString(vstr[1], ':');
            CKey key;
            if (vstr2.size() < 1 || vstr2.size() > 2) {
                continue;
            }
            else if (vstr2.size() == 1) {
                // Legacy format with only private key in the private_key field
                key = DecodeSecret(vstr[1]);
            }
            else {
                // New format with "prefix:private key" in the private_key field
                key = DecodeSecret(vstr2[1]);
            }
            if (!key.IsValid()) {
                continue;
            }
            CPubKey pubkey = key.GetPubKey();
            CHECK_NONFATAL(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (spk_man.HaveKey(keyid)) {
                pwallet->WalletLogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(PKHash(keyid)));
                continue;
            }
            pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(PKHash(keyid)));
            if (!spk_man.AddKeyPubKeyWithDB(batch, key, pubkey)) {
                fGood = false;
                continue;
            }
        }
    } else {
        // json
        UniValue data(UniValue::VOBJ);
        {
            auto buffer = std::make_unique<char[]>(nFilesize);
            file.read(buffer.get(), nFilesize);
            if(!data.read(buffer.get()))
                throw JSONRPCError(RPC_TYPE_ERROR, "Cannot parse Electrum wallet export file");
        }

        std::vector<std::string> vKeys = data.getKeys();

        for (size_t i = 0; i < data.size(); i++) {
            pwallet->ShowProgress("", std::max(1, std::min(99, int(i*100/data.size()))));
            if(!data[vKeys[i]].isStr())
                continue;
            std::vector<std::string> vstr2 = SplitString(data[vKeys[i]].get_str(), ':');
            CKey key;
            if (vstr2.size() < 1 || vstr2.size() > 2) {
                continue;
            }
            else if (vstr2.size() == 1) {
                // Legacy format with only private key in the private_key field
                key = DecodeSecret(data[vKeys[i]].get_str());
            }
            else {
                // New format with "prefix:private key" in the private_key field
                key = DecodeSecret(vstr2[1]);
            }
            if (!key.IsValid()) {
                continue;
            }
            CPubKey pubkey = key.GetPubKey();
            CHECK_NONFATAL(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (spk_man.HaveKey(keyid)) {
                pwallet->WalletLogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(PKHash(keyid)));
                continue;
            }
            pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(PKHash(keyid)));
            if (!spk_man.AddKeyPubKeyWithDB(batch, key, pubkey)) {
                fGood = false;
                continue;
            }
        }
    }
    file.close();
    pwallet->ShowProgress("", 100); // hide progress dialog in GUI

    const int32_t tip_height = pwallet->chain().getHeight().value_or(std::numeric_limits<int32_t>::max());

    // Whether to perform rescan after import
    int nStartHeight = 0;
    if (!request.params[1].isNull())
        nStartHeight = request.params[1].get_int();
    if (tip_height < nStartHeight)
        nStartHeight = tip_height;

    // Assume that electrum wallet was created at that block
    int64_t nTimeBegin;
    CHECK_NONFATAL(pwallet->chain().findFirstBlockWithTimeAndHeight(0, nStartHeight, FoundBlock().time(nTimeBegin)));
    spk_man.UpdateTimeFirstKey(nTimeBegin);

    pwallet->WalletLogPrintf("Rescanning %i blocks\n", tip_height - nStartHeight + 1);
    WalletRescanReserver reserver(*pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }
    pwallet->ScanForWalletTransactions(pwallet->chain().getBlockHash(nStartHeight), nStartHeight, {}, reserver, /*fUpdate=*/true, /*save_progress=*/false);

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys to wallet");

    return NullUniValue;
},
    };
}

RPCHelpMan dumpprivkey()
{
    return RPCHelpMan{"dumpprivkey",
        "\nReveals the private key corresponding to 'address'.\n"
        "Then the importprivkey can be used with this output\n"
        "Note: This command is only compatible with legacy wallets.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Dash address for the private key"},
        },
        RPCResult{
            RPCResult::Type::STR, "key", "The private key"
        },
        RPCExamples{
            HelpExampleCli("dumpprivkey", "\"myaddress\"")
    + HelpExampleCli("importprivkey", "\"mykey\"")
    + HelpExampleRpc("dumpprivkey", "\"myaddress\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    const LegacyScriptPubKeyMan& spk_man = EnsureConstLegacyScriptPubKeyMan(*pwallet);

    LOCK2(pwallet->cs_wallet, spk_man.cs_KeyStore);

    EnsureWalletIsUnlocked(*pwallet);

    std::string strAddress = request.params[0].get_str();
    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Dash address");
    }
    const PKHash *pkhash= std::get_if<PKHash>(&dest);
    if (!pkhash) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to a key");
    }
    CKey vchSecret;
    if (!spk_man.GetKey(ToKeyID(*pkhash), vchSecret)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + strAddress + " is not known");
    }
    return EncodeSecret(vchSecret);
},
    };
}

RPCHelpMan dumphdinfo()
{
    return RPCHelpMan{"dumphdinfo",
        "Returns an object containing sensitive private info about this HD wallet.\n"
        "Note: This command is only compatible with legacy wallets.\n",
        {},
        RPCResult{
                RPCResult::Type::OBJ, "", "",
                {
                        {RPCResult::Type::STR_HEX, "hdseed", "The HD seed (BIP32, in hex)"},
                        {RPCResult::Type::STR, "mnemonic", "The mnemonic for this HD wallet (BIP39, english words)"},
                        {RPCResult::Type::STR, "mnemonicpassphrase", "The mnemonic passphrase for this HD wallet (BIP39)"},
                }
        },
        RPCExamples{
            HelpExampleCli("dumphdinfo", "")
    + HelpExampleRpc("dumphdinfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(*pwallet);

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);
    CHDChain hdChainCurrent;
    if (!spk_man.GetHDChain(hdChainCurrent))
        throw JSONRPCError(RPC_WALLET_ERROR, "This wallet is not a HD wallet.");

    if (!spk_man.GetDecryptedHDChain(hdChainCurrent))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot decrypt HD seed");

    SecureString ssMnemonic;
    SecureString ssMnemonicPassphrase;
    hdChainCurrent.GetMnemonic(ssMnemonic, ssMnemonicPassphrase);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("hdseed", HexStr(hdChainCurrent.GetSeed()));
    obj.pushKV("mnemonic", ssMnemonic.c_str());
    obj.pushKV("mnemonicpassphrase", ssMnemonicPassphrase.c_str());

    return obj;
},
    };
}

RPCHelpMan dumpwallet()
{
    return RPCHelpMan{"dumpwallet",
        "\nDumps all wallet keys in a human-readable format to a server-side file. This does not allow overwriting existing files.\n"
        "Imported scripts are included in the dumpfile too, their corresponding addresses will be added automatically by importwallet.\n"
        "Note that if your wallet contains keys which are not derived from your HD seed (e.g. imported keys), these are not covered by\n"
        "only backing up the seed itself, and must be backed up too (e.g. ensure you back up the whole dumpfile).\n"
        "Note: This command is only compatible with legacy wallets.\n",
        {
            {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The filename with path (absolute path recommended)"},
        },
        RPCResult{
                RPCResult::Type::OBJ, "", "",
                {
                        {RPCResult::Type::NUM, "keys", "The number of keys contained in the wallet dump"},
                        {RPCResult::Type::STR, "filename", "The filename with full absolute path"},
                        {RPCResult::Type::STR, "warning", "A warning about not sharing the wallet dump with anyone"},
                }
        },
        RPCExamples{
            HelpExampleCli("dumpwallet", "\"test\"")
    + HelpExampleRpc("dumpwallet", "\"test\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    const CWallet& wallet = *pwallet;
    const LegacyScriptPubKeyMan& spk_man = EnsureConstLegacyScriptPubKeyMan(wallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    LOCK(wallet.cs_wallet);

    EnsureWalletIsUnlocked(wallet);

    fs::path filepath = fs::u8path(request.params[0].get_str());
    filepath = fs::absolute(filepath);

    /* Prevent arbitrary files from being overwritten. There have been reports
     * that users have overwritten wallet files this way:
     * https://github.com/bitcoin/bitcoin/issues/9934
     * It may also avoid other security issues.
     */
    if (fs::exists(filepath)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, filepath.utf8string() + " already exists. If you are sure this is what you want, move it out of the way first");
    }

    std::ofstream file;
    file.open(filepath);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    std::map<CKeyID, int64_t> mapKeyBirth;
    wallet.GetKeyBirthTimes(mapKeyBirth);

    int64_t block_time = 0;
    CHECK_NONFATAL(wallet.chain().findBlock(wallet.GetLastBlockHash(), FoundBlock().time(block_time)));

    // Note: To avoid a lock order issue, access to cs_main must be locked before cs_KeyStore.
    // So we do the two things in this function that lock cs_main first: GetKeyBirthTimes, and findBlock.
    LOCK(spk_man.cs_KeyStore);

    const std::map<CKeyID, int64_t>& mapKeyPool = spk_man.GetAllReserveKeys();
    std::set<CScriptID> scripts = spk_man.GetCScripts();

    // sort time/key pairs
    std::vector<std::pair<int64_t, CKeyID> > vKeyBirth;
    for (const auto& entry : mapKeyBirth) {
        vKeyBirth.push_back(std::make_pair(entry.second, entry.first));
    }
    mapKeyBirth.clear();
    std::sort(vKeyBirth.begin(), vKeyBirth.end());

    // produce output
    file << strprintf("# Wallet dump created by %s %s\n", PACKAGE_NAME, FormatFullVersion());
    file << strprintf("# * Created on %s\n", FormatISO8601DateTime(GetTime()));
    file << strprintf("# * Best block at time of backup was %i (%s),\n", wallet.GetLastBlockHeight(), wallet.GetLastBlockHash().ToString());
    file << strprintf("#   mined on %s\n", FormatISO8601DateTime(block_time));
    file << "\n";

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("dashcoreversion", FormatFullVersion());
    obj.pushKV("lastblockheight", wallet.GetLastBlockHeight());
    obj.pushKV("lastblockhash", wallet.GetLastBlockHash().ToString());
    obj.pushKV("lastblocktime", block_time);

    // add the base58check encoded extended master if the wallet uses HD
    CHDChain hdChainCurrent;
    if (spk_man.GetHDChain(hdChainCurrent))
    {

        if (!spk_man.GetDecryptedHDChain(hdChainCurrent))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot decrypt HD chain");

        SecureString ssMnemonic;
        SecureString ssMnemonicPassphrase;
        hdChainCurrent.GetMnemonic(ssMnemonic, ssMnemonicPassphrase);
        file << "# mnemonic: " << ssMnemonic << "\n";
        file << "# mnemonic passphrase: " << ssMnemonicPassphrase << "\n\n";

        SecureVector vchSeed = hdChainCurrent.GetSeed();
        file << "# HD seed: " << HexStr(vchSeed) << "\n\n";

        CExtKey masterKey;
        masterKey.SetSeed(MakeByteSpan(vchSeed));

        file << "# extended private masterkey: " << EncodeExtKey(masterKey) << "\n";

        CExtPubKey masterPubkey;
        masterPubkey = masterKey.Neuter();

        file << "# extended public masterkey: " << EncodeExtPubKey(masterPubkey) << "\n\n";

        for (size_t i = 0; i < hdChainCurrent.CountAccounts(); ++i)
        {
            CHDAccount acc;
            if(hdChainCurrent.GetAccount(i, acc)) {
                file << "# external chain counter: " << acc.nExternalChainCounter << "\n";
                file << "# internal chain counter: " << acc.nInternalChainCounter << "\n\n";
            } else {
                file << "# WARNING: ACCOUNT " << i << " IS MISSING!" << "\n\n";
            }
        }
        obj.pushKV("hdaccounts", int(hdChainCurrent.CountAccounts()));
    }

    for (std::vector<std::pair<int64_t, CKeyID> >::const_iterator it = vKeyBirth.begin(); it != vKeyBirth.end(); it++) {
        const CKeyID &keyid = it->second;
        std::string strTime = FormatISO8601DateTime(it->first);
        std::string strAddr;
        std::string strLabel;
        CKey key;
        if (spk_man.GetKey(keyid, key)) {
            CKeyMetadata metadata;
            const auto it{spk_man.mapKeyMetadata.find(keyid)};
            if (it != spk_man.mapKeyMetadata.end()) metadata = it->second;
            file << strprintf("%s %s ", EncodeSecret(key), strTime);
            if (GetWalletAddressesForKey(&spk_man, wallet, keyid, strAddr, strLabel)) {
                file << strprintf("label=%s", strLabel);
            } else if (mapKeyPool.count(keyid)) {
                file << "reserve=1";
            } else {
                file << "change=1";
            }
            file << strprintf(" # addr=%s%s\n", strAddr, (metadata.has_key_origin ? " hdkeypath="+WriteHDKeypath(metadata.key_origin.path) : ""));
        }
    }
    file << "\n";
    for (const CScriptID &scriptid : scripts) {
        CScript script;
        std::string create_time = "0";
        std::string address = EncodeDestination(ScriptHash(scriptid));
        // get birth times for scripts with metadata
        auto it = spk_man.m_script_metadata.find(scriptid);
        if (it != spk_man.m_script_metadata.end()) {
            create_time = FormatISO8601DateTime(it->second.nCreateTime);
        }
        if(spk_man.GetCScript(scriptid, script)) {
            file << strprintf("%s %s script=1", HexStr(script), create_time);
            file << strprintf(" # addr=%s\n", address);
        }
    }
    file << "\n";
    file << "# End of dump\n";
    file.close();

    std::string strWarning = strprintf(_("%s file contains all private keys from this wallet. Do not share it with anyone!").translated, request.params[0].get_str());
    obj.pushKV("keys", int(vKeyBirth.size()));
    obj.pushKV("filename", filepath.utf8string());
    obj.pushKV("warning", strWarning);

    return obj;
},
    };
}

struct ImportData
{
    // Input data
    std::unique_ptr<CScript> redeemscript; //!< Provided redeemScript; will be moved to `import_scripts` if relevant.

    // Output data
    std::set<CScript> import_scripts;
    std::map<CKeyID, bool> used_keys; //!< Import these private keys if available (the value indicates whether if the key is required for solvability)
    std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>> key_origins;
};

enum class ScriptContext
{
    TOP, //! Top-level scriptPubKey
    P2SH, //! P2SH redeemScript
};

// Analyse the provided scriptPubKey, determining which keys and which redeem scripts from the ImportData struct are needed to spend it, and mark them as used.
// Returns an error string, or the empty string for success.
static std::string RecurseImportData(const CScript& script, ImportData& import_data, const ScriptContext script_ctx)
{
    // Use Solver to obtain script type and parsed pubkeys or hashes:
    std::vector<std::vector<unsigned char>> solverdata;
    TxoutType script_type = Solver(script, solverdata);

    switch (script_type) {
    case TxoutType::PUBKEY: {
        CPubKey pubkey(solverdata[0]);
        import_data.used_keys.emplace(pubkey.GetID(), false);
        return "";
    }
    case TxoutType::PUBKEYHASH: {
        CKeyID id = CKeyID(uint160(solverdata[0]));
        import_data.used_keys[id] = true;
        return "";
    }
    case TxoutType::SCRIPTHASH: {
        if (script_ctx == ScriptContext::P2SH) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Trying to nest P2SH inside another P2SH");
        CHECK_NONFATAL(script_ctx == ScriptContext::TOP);
        CScriptID id = CScriptID(uint160(solverdata[0]));
        auto subscript = std::move(import_data.redeemscript); // Remove redeemscript from import_data to check for superfluous script later.
        if (!subscript) return "missing redeemscript";
        if (CScriptID(*subscript) != id) return "redeemScript does not match the scriptPubKey";
        import_data.import_scripts.emplace(*subscript);
        return RecurseImportData(*subscript, import_data, ScriptContext::P2SH);
    }
    case TxoutType::MULTISIG: {
        for (size_t i = 1; i + 1< solverdata.size(); ++i) {
            CPubKey pubkey(solverdata[i]);
            import_data.used_keys.emplace(pubkey.GetID(), false);
        }
        return "";
    }
    case TxoutType::NULL_DATA:
        return "unspendable script";
    case TxoutType::NONSTANDARD:
        return "unrecognized script";
    } // no default case, so the compiler can warn about missing cases
    NONFATAL_UNREACHABLE();
}

static UniValue ProcessImportLegacy(ImportData& import_data, std::map<CKeyID, CPubKey>& pubkey_map, std::map<CKeyID, CKey>& privkey_map, std::set<CScript>& script_pub_keys, bool& have_solving_data, const UniValue& data, std::vector<CKeyID>& ordered_pubkeys)
{
    UniValue warnings(UniValue::VARR);

    // First ensure scriptPubKey has either a script or JSON with "address" string
    const UniValue& scriptPubKey = data["scriptPubKey"];
    bool isScript = scriptPubKey.getType() == UniValue::VSTR;
    if (!isScript && !(scriptPubKey.getType() == UniValue::VOBJ && scriptPubKey.exists("address"))) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "scriptPubKey must be string with script or JSON with address string");
    }
    const std::string& output = isScript ? scriptPubKey.get_str() : scriptPubKey["address"].get_str();

    // Optional fields.
    const std::string& strRedeemScript = data.exists("redeemscript") ? data["redeemscript"].get_str() : "";
    const UniValue& pubKeys = data.exists("pubkeys") ? data["pubkeys"].get_array() : UniValue();
    const UniValue& keys = data.exists("keys") ? data["keys"].get_array() : UniValue();
    const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
    const bool watchOnly = data.exists("watchonly") ? data["watchonly"].get_bool() : false;

    if (data.exists("range")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for a non-descriptor import");
    }

    // Generate the script and destination for the scriptPubKey provided
    CScript script;
    if (!isScript) {
        CTxDestination dest = DecodeDestination(output);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address \"" + output + "\"");
        }
        script = GetScriptForDestination(dest);
    } else {
        if (!IsHex(output)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid scriptPubKey \"" + output + "\"");
        }
        std::vector<unsigned char> vData(ParseHex(output));
        script = CScript(vData.begin(), vData.end());
        CTxDestination dest;
        if (!ExtractDestination(script, dest) && !internal) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal must be set to true for nonstandard scriptPubKey imports.");
        }
    }
    script_pub_keys.emplace(script);

    // Parse all arguments
    if (strRedeemScript.size()) {
        if (!IsHex(strRedeemScript)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid redeem script \"" + strRedeemScript + "\": must be hex string");
        }
        auto parsed_redeemscript = ParseHex(strRedeemScript);
        import_data.redeemscript = std::make_unique<CScript>(parsed_redeemscript.begin(), parsed_redeemscript.end());
    }
    for (size_t i = 0; i < pubKeys.size(); ++i) {
        const auto& str = pubKeys[i].get_str();
        if (!IsHex(str)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey \"" + str + "\" must be a hex string");
        }
        auto parsed_pubkey = ParseHex(str);
        CPubKey pubkey(parsed_pubkey);
        if (!pubkey.IsFullyValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey \"" + str + "\" is not a valid public key");
        }
        pubkey_map.emplace(pubkey.GetID(), pubkey);
        ordered_pubkeys.push_back(pubkey.GetID());
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& str = keys[i].get_str();
        CKey key = DecodeSecret(str);
        if (!key.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
        }
        CPubKey pubkey = key.GetPubKey();
        CKeyID id = pubkey.GetID();
        if (pubkey_map.count(id)) {
            pubkey_map.erase(id);
        }
        privkey_map.emplace(id, key);
    }


    // Verify and process input data
    have_solving_data = import_data.redeemscript || pubkey_map.size() || privkey_map.size();
    if (have_solving_data) {
        // Match up data in import_data with the scriptPubKey in script.
        auto error = RecurseImportData(script, import_data, ScriptContext::TOP);

        // Verify whether the watchonly option corresponds to the availability of private keys.
        bool spendable = std::all_of(import_data.used_keys.begin(), import_data.used_keys.end(), [&](const std::pair<CKeyID, bool>& used_key){ return privkey_map.count(used_key.first) > 0; });
        if (!watchOnly && !spendable) {
            warnings.push_back("Some private keys are missing, outputs will be considered watchonly. If this is intentional, specify the watchonly flag.");
        }
        if (watchOnly && spendable) {
            warnings.push_back("All private keys are provided, outputs will be considered spendable. If this is intentional, do not specify the watchonly flag.");
        }

        // Check that all required keys for solvability are provided.
        if (error.empty()) {
            for (const auto& require_key : import_data.used_keys) {
                if (!require_key.second) continue; // Not a required key
                if (pubkey_map.count(require_key.first) == 0 && privkey_map.count(require_key.first) == 0) {
                    error = "some required keys are missing";
                }
            }
        }

        if (!error.empty()) {
            warnings.push_back("Importing as non-solvable: " + error + ". If this is intentional, don't provide any keys, pubkeys, or redeemscript.");
            import_data = ImportData();
            pubkey_map.clear();
            privkey_map.clear();
            have_solving_data = false;
        } else {
            // RecurseImportData() removes any relevant redeemscript from import_data, so we can use that to discover if a superfluous one was provided.
            if (import_data.redeemscript) warnings.push_back("Ignoring redeemscript as this is not a P2SH script.");
            for (auto it = privkey_map.begin(); it != privkey_map.end(); ) {
                auto oldit = it++;
                if (import_data.used_keys.count(oldit->first) == 0) {
                    warnings.push_back("Ignoring irrelevant private key.");
                    privkey_map.erase(oldit);
                }
            }
            for (auto it = pubkey_map.begin(); it != pubkey_map.end(); ) {
                auto oldit = it++;
                auto key_data_it = import_data.used_keys.find(oldit->first);
                if (key_data_it == import_data.used_keys.end() || !key_data_it->second) {
                    warnings.push_back("Ignoring public key \"" + HexStr(oldit->first) + "\" as it doesn't appear inside P2PKH.");
                    pubkey_map.erase(oldit);
                }
            }
        }
    }

    return warnings;
}

static UniValue ProcessImportDescriptor(ImportData& import_data, std::map<CKeyID, CPubKey>& pubkey_map, std::map<CKeyID, CKey>& privkey_map, std::set<CScript>& script_pub_keys, bool& have_solving_data, const UniValue& data, std::vector<CKeyID>& ordered_pubkeys)
{
    UniValue warnings(UniValue::VARR);

    const std::string& descriptor = data["desc"].get_str();
    FlatSigningProvider keys;
    std::string error;
    auto parsed_desc = Parse(descriptor, keys, error, /* require_checksum = */ true);
    if (!parsed_desc) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }

    have_solving_data = parsed_desc->IsSolvable();
    const bool watch_only = data.exists("watchonly") ? data["watchonly"].get_bool() : false;

    int64_t range_start = 0, range_end = 0;
    if (!parsed_desc->IsRange() && data.exists("range")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for an un-ranged descriptor");
    } else if (parsed_desc->IsRange()) {
        if (!data.exists("range")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor is ranged, please specify the range");
        }
        std::tie(range_start, range_end) = ParseDescriptorRange(data["range"]);
    }

    const UniValue& priv_keys = data.exists("keys") ? data["keys"].get_array() : UniValue();

    // Expand all descriptors to get public keys and scripts, and private keys if available.
    for (int i = range_start; i <= range_end; ++i) {
        FlatSigningProvider out_keys;
        std::vector<CScript> scripts_temp;
        parsed_desc->Expand(i, keys, scripts_temp, out_keys);
        std::copy(scripts_temp.begin(), scripts_temp.end(), std::inserter(script_pub_keys, script_pub_keys.end()));
        for (const auto& key_pair: out_keys.pubkeys) {
            ordered_pubkeys.push_back(key_pair.first);
        }

        for (const auto& x: out_keys.scripts) {
            import_data.import_scripts.emplace(x.second);
        }

        parsed_desc->ExpandPrivate(i, keys, out_keys);

        std::copy(out_keys.pubkeys.begin(), out_keys.pubkeys.end(), std::inserter(pubkey_map, pubkey_map.end()));
        std::copy(out_keys.keys.begin(), out_keys.keys.end(), std::inserter(privkey_map, privkey_map.end()));
        import_data.key_origins.insert(out_keys.origins.begin(), out_keys.origins.end());
    }

    for (size_t i = 0; i < priv_keys.size(); ++i) {
        const auto& str = priv_keys[i].get_str();
        CKey key = DecodeSecret(str);
        if (!key.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
        }
        CPubKey pubkey = key.GetPubKey();
        CKeyID id = pubkey.GetID();

        // Check if this private key corresponds to a public key from the descriptor
        if (!pubkey_map.count(id)) {
            warnings.push_back("Ignoring irrelevant private key.");
        } else {
            privkey_map.emplace(id, key);
        }
    }

    // Check if all the public keys have corresponding private keys in the import for spendability.
    // This does not take into account threshold multisigs which could be spendable without all keys.
    // Thus, threshold multisigs without all keys will be considered not spendable here, even if they are,
    // perhaps triggering a false warning message. This is consistent with the current wallet IsMine check.
    bool spendable = std::all_of(pubkey_map.begin(), pubkey_map.end(),
        [&](const std::pair<CKeyID, CPubKey>& used_key) {
            return privkey_map.count(used_key.first) > 0;
        }) && std::all_of(import_data.key_origins.begin(), import_data.key_origins.end(),
        [&](const std::pair<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& entry) {
            return privkey_map.count(entry.first) > 0;
        });
    if (!watch_only && !spendable) {
        warnings.push_back("Some private keys are missing, outputs will be considered watchonly. If this is intentional, specify the watchonly flag.");
    }
    if (watch_only && spendable) {
        warnings.push_back("All private keys are provided, outputs will be considered spendable. If this is intentional, do not specify the watchonly flag.");
    }

    return warnings;
}

static UniValue ProcessImport(CWallet& wallet, const UniValue& data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    UniValue warnings(UniValue::VARR);
    UniValue result(UniValue::VOBJ);

    try {
        const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
        // Internal addresses should not have a label
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal addresses should not have a label");
        }
        const std::string& label = data.exists("label") ? data["label"].get_str() : "";
        const bool add_keypool = data.exists("keypool") ? data["keypool"].get_bool() : false;

        // Add to keypool only works with privkeys disabled
        if (add_keypool && !wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Keys can only be imported to the keypool when private keys are disabled");
        }

        ImportData import_data;
        std::map<CKeyID, CPubKey> pubkey_map;
        std::map<CKeyID, CKey> privkey_map;
        std::set<CScript> script_pub_keys;
        std::vector<CKeyID> ordered_pubkeys;
        bool have_solving_data;

        if (data.exists("scriptPubKey") && data.exists("desc")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Both a descriptor and a scriptPubKey should not be provided.");
        } else if (data.exists("scriptPubKey")) {
            warnings = ProcessImportLegacy(import_data, pubkey_map, privkey_map, script_pub_keys, have_solving_data, data, ordered_pubkeys);
        } else if (data.exists("desc")) {
            warnings = ProcessImportDescriptor(import_data, pubkey_map, privkey_map, script_pub_keys, have_solving_data, data, ordered_pubkeys);
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Either a descriptor or scriptPubKey must be provided.");
        }

        // If private keys are disabled, abort if private keys are being imported
        if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && !privkey_map.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
        }

        // Check whether we have any work to do
        for (const CScript& script : script_pub_keys) {
            if (wallet.IsMine(script) & ISMINE_SPENDABLE) {
                throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script (\"" + HexStr(script) + "\")");
            }
        }

        // All good, time to import
        wallet.MarkDirty();
        if (!wallet.ImportScripts(import_data.import_scripts, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding script to wallet");
        }
        if (!wallet.ImportPrivKeys(privkey_map, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
        }
        if (!wallet.ImportPubKeys(ordered_pubkeys, pubkey_map, import_data.key_origins, add_keypool, internal, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
        }
        if (!wallet.ImportScriptPubKeys(label, script_pub_keys, have_solving_data, !internal, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
        }

        result.pushKV("success", UniValue(true));
    } catch (const UniValue& e) {
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
    } catch (...) {
        result.pushKV("success", UniValue(false));

        result.pushKV("error", JSONRPCError(RPC_MISC_ERROR, "Missing required fields"));
    }
    if (warnings.size()) result.pushKV("warnings", warnings);
    return result;
}

static int64_t GetImportTimestamp(const UniValue& data, int64_t now)
{
    if (data.exists("timestamp")) {
        const UniValue& timestamp = data["timestamp"];
        if (timestamp.isNum()) {
            return timestamp.get_int64();
        } else if (timestamp.isStr() && timestamp.get_str() == "now") {
            return now;
        }
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(timestamp.type())));
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
}

RPCHelpMan importmulti()
{
    return RPCHelpMan{"importmulti",
        "\nImport addresses/scripts (with private or public keys, redeem script (P2SH)), optionally rescanning the blockchain from the earliest creation time of the imported scripts. Requires a new wallet backup.\n"
        "If an address/script is imported without all of the private keys required to spend from that address, it will be watchonly. The 'watchonly' option must be set to true in this case or a warning will be returned.\n"
        "Conversely, if all the private keys are provided and the address/script is spendable, the watchonly option must be set to false, or a warning will be returned.\n"
        "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
        "may report that the imported keys, addresses or scripts exists but related transactions are still missing.\n"
        "Note: This command is only compatible with legacy wallets. Use \"importdescriptors\" for descriptor wallets.\n",
        {
            {"requests", RPCArg::Type::ARR, RPCArg::Optional::NO, "Data to be imported",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"desc", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Descriptor to import. If using descriptor, do not also provide address/scriptPubKey, scripts, or pubkeys"
                            },
                            {"scriptPubKey", RPCArg::Type::STR, RPCArg::Optional::NO, "Type of scriptPubKey (string for script, json for address). Should not be provided if using a descriptor",
                                /* oneline_description */ "", {"\"<script>\" | { \"address\":\"<address>\" }", "string / json"}
                            },
                            {"timestamp", RPCArg::Type::NUM, RPCArg::Optional::NO, "Creation time of the key expressed in " + UNIX_EPOCH_TIME + ",\n"
"                                                              or the string \"now\" to substitute the current synced blockchain time. The timestamp of the oldest\n"
"                                                              key will determine how far back blockchain rescans need to begin for missing wallet transactions.\n"
"                                                              \"now\" can be specified to bypass scanning, for keys which are known to never have been used, and\n"
"                                                              0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest key\n"
"                                                              creation time of all keys being imported by the importmulti call will be scanned.",
                                /* oneline_description */ "", {"timestamp | \"now\"", "integer / string"}
                            },
                            {"redeemscript", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Allowed only if the scriptPubKey is a P2SH address or  a P2SH scriptPubKey"},
                            {"pubkeys", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Array of strings giving pubkeys to import. They must occur in P2PKH or P2WPKH scripts. They are not required when the private key is also provided (see the \"keys\" argument).",
                                {
                                    {"pubKey", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""},
                                }
                            },
                            {"keys", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Array of strings giving private keys whose corresponding public keys must occur in the output or redeemscript.",
                                {
                                    {"key", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""},
                                }
                            },
                            {"range", RPCArg::Type::RANGE, RPCArg::Optional::OMITTED, "If a ranged descriptor is used, this specifies the end or the range (in the form [begin,end]) to import"},
                            {"internal", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stating whether matching outputs should be treated as not incoming payments (also known as change)"},
                            {"watchonly", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stating whether matching outputs should be considered watchonly."},
                            {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Label to assign to the address, only allowed with internal=false"},
                            {"keypool", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stating whether imported public keys should be added to the keypool for when users request new addresses. Only allowed when wallet private keys are disabled"},
                        },
                    },
                },
                "\"requests\""},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                {
                    {"rescan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Stating if should rescan the blockchain after all imports"},
                },
                "\"options\""},
        },
        RPCResult{
                RPCResult::Type::ARR, "", "Response is an array with the same size as the input that has the execution result",
                {
                        {RPCResult::Type::OBJ, "", "",
                         {
                                 {RPCResult::Type::BOOL, "success", ""},
                                 {RPCResult::Type::ARR, "warnings", /* optional */ true, "",
                                  {
                                          {RPCResult::Type::STR, "", ""},
                                  }},
                                 {RPCResult::Type::OBJ, "error", /* optional */ true, "",
                                  {
                                          {RPCResult::Type::ELISION, "", "JSONRPC error"},
                                  }},
                         }},
                }
        },
        RPCExamples{
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }, "
                                  "{ \"scriptPubKey\": { \"address\": \"<my 2nd address>\" }, \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }]' '{ \"rescan\": false}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& mainRequest) -> UniValue
{
    RPCTypeCheck(mainRequest.params, {UniValue::VARR, UniValue::VOBJ});

    const UniValue& requests = mainRequest.params[0];

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(mainRequest);
    if (!pwallet) return NullUniValue;
    CWallet& wallet{*pwallet};

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    EnsureLegacyScriptPubKeyMan(*pwallet, true);

    //Default options
    bool fRescan = true;

    if (!mainRequest.params[1].isNull()) {
        const UniValue& options = mainRequest.params[1];

        if (options.exists("rescan")) {
            fRescan = options["rescan"].get_bool();
        }
    }

    WalletRescanReserver reserver(*pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t now = 0;
    bool fRunScan = false;
    int64_t nLowestTimestamp = 0;
    UniValue response(UniValue::VARR);
    {
        LOCK(pwallet->cs_wallet);
        EnsureWalletIsUnlocked(*pwallet);

        // Verify all timestamps are present before importing any keys.
        CHECK_NONFATAL(pwallet->chain().findBlock(pwallet->GetLastBlockHash(), FoundBlock().time(nLowestTimestamp).mtpTime(now)));
        for (const UniValue& data : requests.getValues()) {
            GetImportTimestamp(data, now);
        }

        const int64_t minimumTimestamp = 1;

        for (const UniValue& data : requests.getValues()) {
            const int64_t timestamp = std::max(GetImportTimestamp(data, now), minimumTimestamp);
            const UniValue result = ProcessImport(*pwallet, data, timestamp);
            response.push_back(result);

            if (!fRescan) {
                continue;
            }

            // If at least one request was successful then allow rescan.
            if (result["success"].get_bool()) {
                fRunScan = true;
            }

            // Get the lowest timestamp.
            if (timestamp < nLowestTimestamp) {
                nLowestTimestamp = timestamp;
            }
        }
    }
    if (fRescan && fRunScan && requests.size()) {
        int64_t scannedTime = pwallet->RescanFromTime(nLowestTimestamp, reserver, true /* update */);
        {
            LOCK(pwallet->cs_wallet);
            pwallet->ReacceptWalletTransactions();
        }

        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scannedTime > nLowestTimestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();
            size_t i = 0;
            for (const UniValue& request : requests.getValues()) {
                // If key creation date is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scannedTime <= GetImportTimestamp(request, now) || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV(
                        "error",
                        JSONRPCError(
                            RPC_MISC_ERROR,
                            strprintf("Rescan failed for key with creation timestamp %d. There was an error reading a "
                                      "block from time %d, which is after or within %d seconds of key creation, and "
                                      "could contain transactions pertaining to the key. As a result, transactions "
                                      "and coins using this key may not appear in the wallet. This error could be "
                                      "caused by pruning or data corruption (see dashd log for details) and could "
                                      "be dealt with by downloading and rescanning the relevant blocks (see -reindex "
                                      "and -rescan options).",
                                GetImportTimestamp(request, now), scannedTime - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)));
                    response.push_back(std::move(result));
                }
                ++i;
            }
        }
    }

    return response;
},
    };
}

static UniValue ProcessDescriptorImport(CWallet& wallet, const UniValue& data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    UniValue warnings(UniValue::VARR);
    UniValue result(UniValue::VOBJ);

    try {
        if (!data.exists("desc")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor not found.");
        }

        const std::string& descriptor = data["desc"].get_str();
        const bool active = data.exists("active") ? data["active"].get_bool() : false;
        const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
        const std::string& label = data.exists("label") ? data["label"].get_str() : "";

        // Parse descriptor string
        FlatSigningProvider keys;
        std::string error;
        auto parsed_desc = Parse(descriptor, keys, error, /* require_checksum = */ true);
        if (!parsed_desc) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
        }

        // Range check
        int64_t range_start = 0, range_end = 1, next_index = 0;
        if (!parsed_desc->IsRange() && data.exists("range")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for an un-ranged descriptor");
        } else if (parsed_desc->IsRange()) {
            if (data.exists("range")) {
                auto range = ParseDescriptorRange(data["range"]);
                range_start = range.first;
                range_end = range.second + 1; // Specified range end is inclusive, but we need range end as exclusive
            } else {
                warnings.push_back("Range not given, using default keypool range");
                range_start = 0;
                range_end = gArgs.GetIntArg("-keypool", DEFAULT_KEYPOOL_SIZE);
            }
            next_index = range_start;

            if (data.exists("next_index")) {
                next_index = data["next_index"].get_int64();
                // bound checks
                if (next_index < range_start || next_index >= range_end) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "next_index is out of range");
                }
            }
        }

        // Active descriptors must be ranged
        if (active && !parsed_desc->IsRange()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Active descriptors must be ranged");
        }

        // Ranged descriptors should not have a label
        if (data.exists("range") && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptors should not have a label");
        }

        // Internal addresses should not have a label either
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal addresses should not have a label");
        }

        // Combo descriptor check
        if (active && !parsed_desc->IsSingleType()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Combo descriptors cannot be set to active");
        }

        // If the wallet disabled private keys, abort if private keys exist
        if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && !keys.keys.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
        }

        // Need to ExpandPrivate to check if private keys are available for all pubkeys
        FlatSigningProvider expand_keys;
        std::vector<CScript> scripts;
        if (!parsed_desc->Expand(0, keys, scripts, expand_keys)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot expand descriptor. Probably because of hardened derivations without private keys provided");
        }
        parsed_desc->ExpandPrivate(0, keys, expand_keys);

        // Check if all private keys are provided
        bool have_all_privkeys = !expand_keys.keys.empty();
        for (const auto& entry : expand_keys.origins) {
            const CKeyID& key_id = entry.first;
            CKey key;
            if (!expand_keys.GetKey(key_id, key)) {
                have_all_privkeys = false;
                break;
            }
        }

        // If private keys are enabled, check some things.
        if (!wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
           if (keys.keys.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import descriptor without private keys to a wallet with private keys enabled");
           }
           if (!have_all_privkeys) {
               warnings.push_back("Not all private keys provided. Some wallet functionality may return unexpected errors");
           }
        }

        WalletDescriptor w_desc(std::move(parsed_desc), timestamp, range_start, range_end, next_index);

        // Check if the wallet already contains the descriptor
        auto existing_spk_manager = wallet.GetDescriptorScriptPubKeyMan(w_desc);
        if (existing_spk_manager) {
            if (!existing_spk_manager->CanUpdateToWalletDescriptor(w_desc, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
        }

        // Add descriptor to the wallet
        auto spk_manager = wallet.AddWalletDescriptor(w_desc, keys, label, internal);
        if (spk_manager == nullptr) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Could not add descriptor '%s'", descriptor));
        }

        // Set descriptor as active if necessary
        if (active) {
            if (!w_desc.descriptor->GetOutputType()) {
                warnings.push_back("Unknown output type, cannot set descriptor to active.");
            } else {
                wallet.AddActiveScriptPubKeyMan(spk_manager->GetID(), internal);
            }
        } else {
            if (w_desc.descriptor->GetOutputType()) {
                wallet.DeactivateScriptPubKeyMan(spk_manager->GetID(), internal);
            }
        }

        result.pushKV("success", UniValue(true));
    } catch (const UniValue& e) {
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
    }
    if (warnings.size()) result.pushKV("warnings", warnings);
    return result;
}

RPCHelpMan importdescriptors() {
            return RPCHelpMan{"importdescriptors",
                "\nImport descriptors. This will trigger a rescan of the blockchain based on the earliest timestamp of all descriptors being imported. Requires a new wallet backup.\n"
            "\nNote: This call can take over an hour to complete if using an early timestamp; during that time, other rpc calls\n"
            "may report that the imported keys, addresses or scripts exist but related transactions are still missing.\n",
                {
                    {"requests", RPCArg::Type::ARR, RPCArg::Optional::NO, "Data to be imported",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "Descriptor to import."},
                                    {"active", RPCArg::Type::BOOL, RPCArg::Default{false}, "Set this descriptor to be the active descriptor for the corresponding output type/externality"},
                                    {"range", RPCArg::Type::RANGE, RPCArg::Optional::OMITTED, "If a ranged descriptor is used, this specifies the end or the range (in the form [begin,end]) to import"},
                                    {"next_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If a ranged descriptor is set to active, this specifies the next index to generate addresses from"},
                                    {"timestamp", RPCArg::Type::NUM, RPCArg::Optional::NO, "Time from which to start rescanning the blockchain for this descriptor, in " + UNIX_EPOCH_TIME + "\n"
        "                                                              Use the string \"now\" to substitute the current synced blockchain time.\n"
        "                                                              \"now\" can be specified to bypass scanning, for outputs which are known to never have been used, and\n"
        "                                                              0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest timestamp\n"
        "                                                              of all descriptors being imported will be scanned.",
                                        /* oneline_description */ "", {"timestamp | \"now\"", "integer / string"}
                                    },
                                    {"internal", RPCArg::Type::BOOL, RPCArg::Default{false}, "Whether matching outputs should be treated as not incoming payments (e.g. change)"},
                                    {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Label to assign to the address, only allowed with internal=false. Disabled for ranged descriptors"},
                                },
                            },
                        },
                        "\"requests\""},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "Response is an array with the same size as the input that has the execution result",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::BOOL, "success", ""},
                            {RPCResult::Type::ARR, "warnings", /* optional */ true, "",
                            {
                                {RPCResult::Type::STR, "", ""},
                            }},
                            {RPCResult::Type::OBJ, "error", /* optional */ true, "",
                            {
                                {RPCResult::Type::ELISION, "", "JSONRPC error"},
                            }},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"internal\": true }, "
                                          "{ \"desc\": \"<my desccriptor 2>\", \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"active\": true, \"range\": [0,100], \"label\": \"<my wallet>\" }]'")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& main_request) -> UniValue
{
    // Acquire the wallet
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(main_request);
    if (!pwallet) return NullUniValue;
    CWallet& wallet{*pwallet};

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    //  Make sure wallet is a descriptor wallet
    if (!pwallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "importdescriptors is not available for non-descriptor wallets");
    }

    RPCTypeCheck(main_request.params, {UniValue::VARR, UniValue::VOBJ});

    WalletRescanReserver reserver(*pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    const UniValue& requests = main_request.params[0];
    const int64_t minimum_timestamp = 1;
    int64_t now = 0;
    int64_t lowest_timestamp = 0;
    bool rescan = false;
    UniValue response(UniValue::VARR);
    {
        LOCK(pwallet->cs_wallet);
        EnsureWalletIsUnlocked(*pwallet);

        CHECK_NONFATAL(pwallet->chain().findBlock(pwallet->GetLastBlockHash(), FoundBlock().time(lowest_timestamp).mtpTime(now)));

        // Get all timestamps and extract the lowest timestamp
        for (const UniValue& request : requests.getValues()) {
            // This throws an error if "timestamp" doesn't exist
            const int64_t timestamp = std::max(GetImportTimestamp(request, now), minimum_timestamp);
            const UniValue result = ProcessDescriptorImport(*pwallet, request, timestamp);
            response.push_back(result);

            if (lowest_timestamp > timestamp ) {
                lowest_timestamp = timestamp;
            }

            // If we know the chain tip, and at least one request was successful then allow rescan
            if (!rescan && result["success"].get_bool()) {
                rescan = true;
            }
        }
        pwallet->ConnectScriptPubKeyManNotifiers();
    }

    // Rescan the blockchain using the lowest timestamp
    if (rescan) {
        int64_t scanned_time = pwallet->RescanFromTime(lowest_timestamp, reserver, true /* update */);
        {
            LOCK(pwallet->cs_wallet);
            pwallet->ReacceptWalletTransactions();
        }

        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }

        if (scanned_time > lowest_timestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();

            // Compose the response
            for (unsigned int i = 0; i < requests.size(); ++i) {
                const UniValue& request = requests.getValues().at(i);

                // If the descriptor timestamp is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scanned_time <= GetImportTimestamp(request, now) || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV(
                        "error",
                        JSONRPCError(
                            RPC_MISC_ERROR,
                            strprintf("Rescan failed for descriptor with timestamp %d. There was an error reading a "
                                      "block from time %d, which is after or within %d seconds of key creation, and "
                                      "could contain transactions pertaining to the desc. As a result, transactions "
                                      "and coins using this desc may not appear in the wallet. This error could be "
                                      "caused by pruning or data corruption (see bitcoind log for details) and could "
                                      "be dealt with by downloading and rescanning the relevant blocks (see -reindex "
                                      "and -rescan options).",
                                GetImportTimestamp(request, now), scanned_time - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)));
                    response.push_back(std::move(result));
                }
            }
        }
    }

    return response;
},
    };
}

RPCHelpMan listdescriptors()
{
    return RPCHelpMan{
        "listdescriptors",
        "\nList descriptors imported into a descriptor-enabled wallet.\n",
        {
            {"private", RPCArg::Type::BOOL, RPCArg::Default{false}, "Show private descriptors."}
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR, "wallet_name", "Name of wallet this operation was performed on"},
            {RPCResult::Type::ARR, "descriptors", "Array of descriptor objects",
            {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "desc", "Descriptor string representation"},
                    {RPCResult::Type::STR, "mnemonic", "The mnemonic for this descriptor wallet (BIP39, english words). Presented only if private=true and created with a mnemonic"},
                    {RPCResult::Type::STR, "mnemonicpassphrase", "The mnemonic passphrase for this descriptor wallet (BIP39). Presented only if private=true and created with a mnemonic"},
                    {RPCResult::Type::NUM, "timestamp", "The creation time of the descriptor"},
                    {RPCResult::Type::BOOL, "active", "Whether this descriptor is currently used to generate new addresses"},
                    {RPCResult::Type::BOOL, "internal", /*optional=*/true, "True if this descriptor is used to generate change addresses. False if this descriptor is used to generate receiving addresses; defined only for active descriptors"},
                    {RPCResult::Type::ARR_FIXED, "range", /*optional=*/true, "Defined only for ranged descriptors", {
                        {RPCResult::Type::NUM, "", "Range start inclusive"},
                        {RPCResult::Type::NUM, "", "Range end inclusive"},
                    }},
                    {RPCResult::Type::NUM, "next", /*optional=*/true, "The next index to generate addresses from; defined only for ranged descriptors"},
                }},
            }}
        }},
        RPCExamples{
            HelpExampleCli("listdescriptors", "") + HelpExampleRpc("listdescriptors", "")
            + HelpExampleCli("listdescriptors", "true") + HelpExampleRpc("listdescriptors", "true")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;

    if (!wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "listdescriptors is not available for non-descriptor wallets");
    }

    const bool priv = !request.params[0].isNull() && request.params[0].get_bool();
    if (priv) {
        EnsureWalletIsUnlocked(*wallet);
    }

    LOCK(wallet->cs_wallet);

    UniValue descriptors(UniValue::VARR);
    const auto active_spk_mans = wallet->GetActiveScriptPubKeyMans();
    for (const auto& spk_man : wallet->GetAllScriptPubKeyMans()) {
        const auto desc_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        if (!desc_spk_man) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Unexpected ScriptPubKey manager type.");
        }
        UniValue spk(UniValue::VOBJ);
        LOCK(desc_spk_man->cs_desc_man);
        const auto& wallet_descriptor = desc_spk_man->GetWalletDescriptor();
        std::string descriptor;

        if (!desc_spk_man->GetDescriptorString(descriptor, priv)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Can't get descriptor string.");
        }
        if (priv) {
            SecureString mnemonic;
            SecureString mnemonic_passphrase;
            if (desc_spk_man->GetMnemonicString(mnemonic, mnemonic_passphrase) && !mnemonic.empty()) {
                spk.pushKV("mnemonic", mnemonic.c_str());
                spk.pushKV("mnemonicpassphrase", mnemonic_passphrase.c_str());
            }
        }
        spk.pushKV("desc", descriptor);
        spk.pushKV("timestamp", wallet_descriptor.creation_time);
        const bool active = active_spk_mans.count(desc_spk_man) != 0;
        spk.pushKV("active", active);
        const auto& type = wallet_descriptor.descriptor->GetOutputType();
        if (active && type != std::nullopt) {
            spk.pushKV("internal", wallet->GetScriptPubKeyMan(true) == desc_spk_man);
        }
        if (wallet_descriptor.descriptor->IsRange()) {
            UniValue range(UniValue::VARR);
            range.push_back(wallet_descriptor.range_start);
            range.push_back(wallet_descriptor.range_end - 1);
            spk.pushKV("range", range);
            spk.pushKV("next", wallet_descriptor.next_index);
        }
        descriptors.push_back(spk);
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("wallet_name", wallet->GetName());
    response.pushKV("descriptors", descriptors);

    return response;
},
    };
}

RPCHelpMan backupwallet()
{
    return RPCHelpMan{"backupwallet",
        "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n",
        {
            {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination directory or file"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
            HelpExampleCli("backupwallet", "\"backup.dat\"")
    + HelpExampleRpc("backupwallet", "\"backup.dat\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return NullUniValue;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
},
    };
}

RPCHelpMan restorewallet()
{
    return RPCHelpMan{
        "restorewallet",
        "\nRestore and loads a wallet from backup.\n",
        {
            {"wallet_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name that will be applied to the restored wallet"},
            {"backup_file", RPCArg::Type::STR, RPCArg::Optional::NO, "The backup file that will be used to restore the wallet."},
            {"load_on_startup", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED_NAMED_ARG, "Save wallet name to persistent settings and load on startup. True to add wallet to startup list, false to remove, null to leave unchanged."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "The wallet name if restored successfully."},
                {RPCResult::Type::STR, "warning", "Warning message if wallet was not loaded cleanly."},
            }
        },
        RPCExamples{
            HelpExampleCli("restorewallet", "\"testwallet\" \"home\\backups\\backup-file.bak\"")
            + HelpExampleRpc("restorewallet", "\"testwallet\" \"home\\backups\\backup-file.bak\"")
            + HelpExampleCliNamed("restorewallet", {{"wallet_name", "testwallet"}, {"backup_file", "home\\backups\\backup-file.bak\""}, {"load_on_startup", true}})
            + HelpExampleRpcNamed("restorewallet", {{"wallet_name", "testwallet"}, {"backup_file", "home\\backups\\backup-file.bak\""}, {"load_on_startup", true}})
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    WalletContext& context = EnsureWalletContext(request.context);

    auto backup_file = fs::u8path(request.params[1].get_str());

    std::string wallet_name = request.params[0].get_str();

    std::optional<bool> load_on_start = request.params[2].isNull() ? std::nullopt : std::optional<bool>(request.params[2].get_bool());

    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;

    const std::shared_ptr<CWallet> wallet = RestoreWallet(context, backup_file, wallet_name, load_on_start, status, error, warnings);

    HandleWalletError(wallet, status, error);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", Join(warnings, Untranslated("\n")).original);

    return obj;

},
    };
}
} // namespace wallet
