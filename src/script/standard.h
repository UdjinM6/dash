// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_STANDARD_H
#define BITCOIN_SCRIPT_STANDARD_H

#include <script/interpreter.h>
#include <uint256.h>
#include <util/hash_type.h>

#include <variant>
#include <string>


static const bool DEFAULT_ACCEPT_DATACARRIER = true;

class CKeyID;
class CScript;
struct ScriptHash;

/** A reference to a CScript: the Hash160 of its serialization (see script.h) */
class CScriptID : public BaseHash<uint160>
{
public:
    CScriptID() : BaseHash() {}
    explicit CScriptID(const CScript& in);
    explicit CScriptID(const uint160& in) : BaseHash(in) {}
    explicit CScriptID(const ScriptHash& in);
};

/**
 * Default setting for nMaxDatacarrierBytes. 80 bytes of data, +1 for OP_RETURN,
 * +2 for the pushdata opcodes.
 */
static const unsigned int MAX_OP_RETURN_RELAY = 83;

/**
 * A data carrying output is an unspendable output containing data. The script
 * type is designated as TxoutType::NULL_DATA.
 */
extern bool fAcceptDatacarrier;

/** Maximum size of TxoutType::NULL_DATA scripts that this node considers standard. */
extern unsigned nMaxDatacarrierBytes;

/**
 * Mandatory script verification flags that all new blocks must comply with for
 * them to be valid. (but old blocks may not comply with) Currently just P2SH,
 * but in the future other flags may be added.
 *
 * Failing one of these tests may trigger a DoS ban - see CheckInputScripts() for
 * details.
 */
static const unsigned int MANDATORY_SCRIPT_VERIFY_FLAGS = SCRIPT_VERIFY_P2SH;

enum class TxoutType
{
    NONSTANDARD,
    // 'standard' transaction types:
    PUBKEY,
    PUBKEYHASH,
    SCRIPTHASH,
    MULTISIG,
    NULL_DATA, //!< unspendable OP_RETURN script that carries data
};

class CNoDestination {
public:
    friend bool operator==(const CNoDestination &a, const CNoDestination &b) { return true; }
    friend bool operator<(const CNoDestination &a, const CNoDestination &b) { return true; }
};

struct PKHash : public BaseHash<uint160>
{
    PKHash() : BaseHash() {}
    explicit PKHash(const uint160& hash) : BaseHash(hash) {}
    explicit PKHash(const CPubKey& pubkey);
    explicit PKHash(const CKeyID& pubkey_id);
};
CKeyID ToKeyID(const PKHash& key_hash);

struct ScriptHash : public BaseHash<uint160>
{
    ScriptHash() : BaseHash() {}
    // These don't do what you'd expect.
    // Use ScriptHash(GetScriptForDestination(...)) instead.
    explicit ScriptHash(const PKHash& hash) = delete;

    explicit ScriptHash(const uint160& hash) : BaseHash(hash) {}
    explicit ScriptHash(const CScript& script);
    explicit ScriptHash(const CScriptID& script);
};

/**
 * A txout script template with a specific destination. It is either:
 *  * CNoDestination: no destination set
 *  * CKeyID: TxoutType::PUBKEYHASH destination
 *  * CScriptID: TxoutType::SCRIPTHASH destination
 *  A CTxDestination is the internal data type encoded in a bitcoin address
 */
using CTxDestination = std::variant<CNoDestination, PKHash, ScriptHash>;

/** Check whether a CTxDestination is a CNoDestination. */
bool IsValidDestination(const CTxDestination& dest);

/** Get the name of a TxoutType as a C string, or nullptr if unknown. */
std::string GetTxnOutputType(TxoutType t);

/**
 * Parse a scriptPubKey and identify script type for standard scripts. If
 * successful, returns script type and parsed pubkeys or hashes, depending on
 * the type. For example, for a P2SH script, vSolutionsRet will contain the
 * script hash, for P2PKH it will contain the key hash, etc.
 *
 * @param[in]   scriptPubKey   Script to parse
 * @param[out]  vSolutionsRet  Vector of parsed pubkeys and hashes
 * @return                     The script type. TxoutType::NONSTANDARD represents a failed solve.
 */
TxoutType Solver(const CScript& scriptPubKey, std::vector<std::vector<unsigned char>>& vSolutionsRet);

/**
 * Parse a standard scriptPubKey for the destination address. Assigns result to
 * the addressRet parameter and returns true if successful. Currently only works for P2PK,
 * P2PKH, and P2SH scripts.
 */
bool ExtractDestination(const CScript& scriptPubKey, CTxDestination& addressRet);

/**
 * Generate a Bitcoin scriptPubKey for the given CTxDestination. Returns a P2PKH
 * script for a CKeyID destination, a P2SH script for a CScriptID, and an empty
 * script for CNoDestination.
 */
CScript GetScriptForDestination(const CTxDestination& dest);

/** Generate a P2PK script for the given pubkey. */
CScript GetScriptForRawPubKey(const CPubKey& pubkey);

/** Generate a multisig script. */
CScript GetScriptForMultisig(int nRequired, const std::vector<CPubKey>& keys);

#endif // BITCOIN_SCRIPT_STANDARD_H
