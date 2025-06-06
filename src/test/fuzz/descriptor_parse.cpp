// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <key.h>
#include <pubkey.h>
#include <script/descriptor.h>
#include <script/standard.h>
#include <test/fuzz/fuzz.h>

void initialize_descriptor_parse()
{
    ECC_Start();
    SelectParams(CBaseChainParams::MAIN);
}

FUZZ_TARGET(descriptor_parse, .init = initialize_descriptor_parse)
{
    const std::string descriptor(buffer.begin(), buffer.end());
    FlatSigningProvider signing_provider;
    std::string error;
    for (const bool require_checksum : {true, false}) {
        const auto desc = Parse(descriptor, signing_provider, error, require_checksum);
        if (desc) {
            (void)desc->ToString();
            (void)desc->IsRange();
            (void)desc->IsSolvable();
        }
    }
}
