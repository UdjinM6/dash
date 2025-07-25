// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/siphash.h>
#include <random.h>
#include <span.h>
#include <util/hasher.h>

#include <limits>

SaltedTxidHasher::SaltedTxidHasher() : k0(), k1() {}

SaltedOutpointHasher::SaltedOutpointHasher(bool deterministic) :
    k0(deterministic ? 0x8e819f2607a18de6 : GetRand<uint64_t>()),
    k1(deterministic ? 0xf4020d2e3983b0eb : GetRand<uint64_t>())
{}

SaltedSipHasher::SaltedSipHasher() : m_k0(GetRand<uint64_t>()), m_k1(GetRand<uint64_t>()) {}

size_t SaltedSipHasher::operator()(const Span<const unsigned char>& script) const
{
    return CSipHasher(m_k0, m_k1).Write(script.data(), script.size()).Finalize();
}
