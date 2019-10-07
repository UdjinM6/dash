// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <outputtype.h>

#include <script/signingprovider.h>
#include <script/standard.h>

#include <assert.h>

CTxDestination AddAndGetDestinationForScript(FillableSigningProvider& keystore, const CScript& script)
{
    // Add script to keystore
    keystore.AddCScript(script);
    ScriptHash sh(script);
    // Note that scripts over 520 bytes are not yet supported.
    keystore.AddCScript(GetScriptForDestination(sh));
    return sh;
}
