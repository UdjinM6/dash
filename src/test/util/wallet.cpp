// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <test/util/wallet.h>

#include <key_io.h>
#include <outputtype.h>
#include <script/standard.h>
#ifdef ENABLE_WALLET
#include <util/translation.h>
#include <wallet/wallet.h>
#endif

using wallet::CWallet;

const std::string ADDRESS_B58T_UNSPENDABLE = "yXXXXXXXXXXXXXXXXXXXXXXXXXXXVd2rXU";
const std::string ADDRESS_BCRT1_UNSPENDABLE = "bcrt1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq3xueyj";

#ifdef ENABLE_WALLET
std::string getnewaddress(CWallet& w)
{
    CTxDestination dest;
    bilingual_str error;
    if (!w.GetNewDestination("", dest, error)) assert(false);

    return EncodeDestination(dest);
}

// void importaddress(CWallet& wallet, const std::string& address)
// {
//     auto spk_man = wallet.GetLegacyScriptPubKeyMan();
//     assert(spk_man != nullptr);
//     LOCK2(wallet.cs_wallet, spk_man->cs_KeyStore);
//     const auto dest = DecodeDestination(address);
//     assert(IsValidDestination(dest));
//     const auto script = GetScriptForDestination(dest);
//     wallet.MarkDirty();
//     assert(!spk_man->HaveWatchOnly(script));
//     if (!spk_man->AddWatchOnly(script, 0 /* nCreateTime */)) assert(false);
//     wallet.SetAddressBook(dest, /*label=*/"", "receive");
// }
#endif // ENABLE_WALLET
