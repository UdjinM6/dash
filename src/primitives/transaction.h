// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_TRANSACTION_H
#define BITCOIN_PRIMITIVES_TRANSACTION_H

#include <attributes.h>
#include <consensus/amount.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/** Transaction types */
enum {
    TRANSACTION_NORMAL = 0,
    TRANSACTION_PROVIDER_REGISTER = 1,
    TRANSACTION_PROVIDER_UPDATE_SERVICE = 2,
    TRANSACTION_PROVIDER_UPDATE_REGISTRAR = 3,
    TRANSACTION_PROVIDER_UPDATE_REVOKE = 4,
    TRANSACTION_COINBASE = 5,
    TRANSACTION_QUORUM_COMMITMENT = 6,
    TRANSACTION_MNHF_SIGNAL = 7,
    TRANSACTION_ASSET_LOCK = 8,
    TRANSACTION_ASSET_UNLOCK = 9,
};

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint
{
public:
    uint256 hash;
    uint32_t n;

    static constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

    COutPoint(): n(NULL_INDEX) { }
    COutPoint(const uint256& hashIn, uint32_t nIn): hash(hashIn), n(nIn) { }

    SERIALIZE_METHODS(COutPoint, obj) { READWRITE(obj.hash, obj.n); }

    void SetNull() { hash.SetNull(); n = NULL_INDEX; }
    bool IsNull() const { return (hash.IsNull() && n == NULL_INDEX); }

    friend bool operator<(const COutPoint& a, const COutPoint& b)
    {
        int cmp = a.hash.Compare(b.hash);
        return cmp < 0 || (cmp == 0 && a.n < b.n);
    }

    friend bool operator==(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    friend bool operator!=(const COutPoint& a, const COutPoint& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
    std::string ToStringShort() const;
};

/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;

    /**
     * Setting nSequence to this value for every input in a transaction
     * disables nLockTime/IsFinalTx().
     * It fails OP_CHECKLOCKTIMEVERIFY/CheckLockTime() for any input that has
     * it set (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static const uint32_t SEQUENCE_FINAL = 0xffffffff;
    /**
     * This is the maximum sequence number that enables both nLockTime and
     * OP_CHECKLOCKTIMEVERIFY (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static const uint32_t MAX_SEQUENCE_NONFINAL{SEQUENCE_FINAL - 1};

    // Below flags apply in the context of BIP 68. BIP 68 requires the tx
    // version to be set to 2, or higher.
    /**
     * If this flag is set, CTxIn::nSequence is NOT interpreted as a
     * relative lock-time.
     * It skips SequenceLocks() for any input that has it set (BIP 68).
     * It fails OP_CHECKSEQUENCEVERIFY/CheckSequence() for any input that has
     * it set (BIP 112).
     */
    static const uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG = (1U << 31);

    /**
     * If CTxIn::nSequence encodes a relative lock-time and this flag
     * is set, the relative lock-time has units of 512 seconds,
     * otherwise it specifies blocks with a granularity of 1. */
    static const uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG = (1 << 22);

    /**
     * If CTxIn::nSequence encodes a relative lock-time, this mask is
     * applied to extract that lock-time from the sequence field. */
    static const uint32_t SEQUENCE_LOCKTIME_MASK = 0x0000ffff;

    /**
     * In order to use the same number of bits to encode roughly the
     * same wall-clock duration, and because blocks are naturally
     * limited to occur every 600s on average, the minimum granularity
     * for time-based relative lock-time is fixed at 512 seconds.
     * Converting from CTxIn::nSequence to seconds is performed by
     * multiplying by 512 = 2^9, or equivalently shifting up by
     * 9 bits. */
    static const int SEQUENCE_LOCKTIME_GRANULARITY = 9;

    CTxIn()
    {
        nSequence = SEQUENCE_FINAL;
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);
    CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);

    SERIALIZE_METHODS(CTxIn, obj) { READWRITE(obj.prevout, obj.scriptSig, obj.nSequence); }

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    friend bool operator!=(const CTxIn& a, const CTxIn& b)
    {
        return !(a == b);
    }

    friend bool operator<(const CTxIn& a, const CTxIn& b)
    {
        return a.prevout<b.prevout;
    }

    std::string ToString() const;
};

/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class CTxOut
{
public:
    CAmount nValue;
    CScript scriptPubKey;

    CTxOut()
    {
        SetNull();
    }

    CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn);

    SERIALIZE_METHODS(CTxOut, obj) { READWRITE(obj.nValue, obj.scriptPubKey); }

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull() const
    {
        return (nValue == -1);
    }

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue       == b.nValue &&
                a.scriptPubKey == b.scriptPubKey);
    }

    friend bool operator!=(const CTxOut& a, const CTxOut& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
};

struct CMutableTransaction;

/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
public:
    // Default transaction version.
    static const int32_t CURRENT_VERSION=2;
    // Special transaction version
    static const int32_t SPECIAL_VERSION = 3;

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, CTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const std::vector<CTxIn> vin;
    const std::vector<CTxOut> vout;
    const int16_t nVersion;
    const uint16_t nType;
    const uint32_t nLockTime;
    const std::vector<uint8_t> vExtraPayload; // only available for special transaction types

private:
    /** Memory only. */
    const uint256 hash;

    uint256 ComputeHash() const;

public:
    /** Convert a CMutableTransaction into a CTransaction. */
    explicit CTransaction(const CMutableTransaction& tx);
    explicit CTransaction(CMutableTransaction&& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        int32_t n32bitVersion = this->nVersion | (this->nType << 16);
        s << n32bitVersion;
        s << vin;
        s << vout;
        s << nLockTime;
        if (this->HasExtraPayloadField())
            s << vExtraPayload;
    }

    /** This deserializing constructor is provided instead of an Unserialize method.
     *  Unserialize is not possible, since it would require overwriting const fields. */
    template <typename Stream>
    CTransaction(deserialize_type, Stream& s) : CTransaction(CMutableTransaction(deserialize, s)) {}

    bool IsNull() const {
        return vin.empty() && vout.empty();
    }

    const uint256& GetHash() const LIFETIMEBOUND { return hash; }

    // Return sum of txouts.
    CAmount GetValueOut() const;

    /**
     * Get the total transaction size in bytes, including witness data.
     * "Total Size" defined in BIP141 and BIP144.
     * @return Total transaction size in bytes
     */
    unsigned int GetTotalSize() const;

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull());
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return a.hash == b.hash;
    }

    friend bool operator!=(const CTransaction& a, const CTransaction& b)
    {
        return a.hash != b.hash;
    }

    std::string ToString() const;

    bool IsSpecialTxVersion() const noexcept
    {
        return nVersion >= SPECIAL_VERSION;
    }

    bool IsPlatformTransfer() const noexcept
    {
        return IsSpecialTxVersion() && nType == TRANSACTION_ASSET_UNLOCK;
    }

    bool HasExtraPayloadField() const noexcept
    {
        return IsSpecialTxVersion() && nType != TRANSACTION_NORMAL;
    }
};

/** A mutable version of CTransaction. */
struct CMutableTransaction
{
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    int16_t nVersion;
    uint16_t nType;
    uint32_t nLockTime;
    std::vector<uint8_t> vExtraPayload; // only available for special transaction types

    explicit CMutableTransaction();
    explicit CMutableTransaction(const CTransaction& tx);

    SERIALIZE_METHODS(CMutableTransaction, obj)
    {
        int32_t n32bitVersion;
        SER_WRITE(obj, n32bitVersion = obj.nVersion | (obj.nType << 16));
        READWRITE(n32bitVersion);
        SER_READ(obj, obj.nVersion = (int16_t) (n32bitVersion & 0xffff));
        SER_READ(obj, obj.nType = (uint16_t) ((n32bitVersion >> 16) & 0xffff));
        READWRITE(obj.vin, obj.vout, obj.nLockTime);
        if (obj.nVersion >= CTransaction::SPECIAL_VERSION && obj.nType != TRANSACTION_NORMAL) {
            READWRITE(obj.vExtraPayload);
        }
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, Stream& s) {
        Unserialize(s);
    }

    /** Compute the hash of this CMutableTransaction. This is computed on the
     * fly, as opposed to GetHash() in CTransaction, which uses a cached result.
     */
    uint256 GetHash() const;

    std::string ToString() const;
};

typedef std::shared_ptr<const CTransaction> CTransactionRef;
template <typename Tx> static inline CTransactionRef MakeTransactionRef(Tx&& txIn) { return std::make_shared<const CTransaction>(std::forward<Tx>(txIn)); }

/** Implementation of BIP69
 * https://github.com/bitcoin/bips/blob/master/bip-0069.mediawiki
 */
struct CompareInputBIP69
{
    inline bool operator()(const CTxIn& a, const CTxIn& b) const
    {
        if (a.prevout.hash == b.prevout.hash) return a.prevout.n < b.prevout.n;

        uint256 hasha = a.prevout.hash;
        uint256 hashb = b.prevout.hash;

        typedef std::reverse_iterator<const unsigned char*> rev_it;
        rev_it rita = rev_it(hasha.end());
        rev_it ritb = rev_it(hashb.end());

        return std::lexicographical_compare(rita, rita + hasha.size(), ritb, ritb + hashb.size());
    }
};

struct CompareOutputBIP69
{
    inline bool operator()(const CTxOut& a, const CTxOut& b) const
    {
        return a.nValue < b.nValue || (a.nValue == b.nValue && a.scriptPubKey < b.scriptPubKey);
    }
};

#endif // BITCOIN_PRIMITIVES_TRANSACTION_H
