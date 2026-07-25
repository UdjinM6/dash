// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_COINJOIN_H
#define BITCOIN_COINJOIN_COINJOIN_H

#include <coinjoin/common.h>

#include <util/helpers.h>

#include <core_io.h>
#include <netaddress.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <saltedhasher.h>
#include <serialize.h>
#include <sync.h>
#include <timedata.h>
#include <unordered_lru_cache.h>
#include <util/hasher.h>
#include <util/translation.h>
#include <version.h>

#include <atomic>
#include <map>
#include <optional>
#include <utility>

#include <univalue.h>

class Chainstate;
class CBLSPublicKey;
class CBlockIndex;
class ChainstateManager;
class CTxMemPool;

namespace chainlock {
class Chainlocks;
} // namespace chainlock

namespace llmq {
class CInstantSendManager;
} // namespace llmq

extern RecursiveMutex cs_main; // NOLINT(readability-redundant-declaration)

// timeouts
static constexpr int COINJOIN_AUTO_TIMEOUT_MIN = 5;
static constexpr int COINJOIN_AUTO_TIMEOUT_MAX = 15;
static constexpr int COINJOIN_QUEUE_TIMEOUT = 30;
static constexpr int COINJOIN_SIGNING_TIMEOUT = 15;

static constexpr size_t COINJOIN_ENTRY_MAX_SIZE = 9;

namespace CoinJoin {
/// Get the minimum/maximum number of participants for the pool
int GetMinPoolParticipants();
int GetMaxPoolParticipants();

/// Maximum number of inputs or outputs across a full pool
inline size_t GetMaxPoolInputOutputCount() { return size_t(GetMaxPoolParticipants()) * COINJOIN_ENTRY_MAX_SIZE; }
} // namespace CoinJoin

// pool responses
enum PoolMessage : int32_t {
    ERR_ALREADY_HAVE,
    ERR_DENOM,
    ERR_ENTRIES_FULL,
    ERR_EXISTING_TX,
    ERR_FEES,
    ERR_INVALID_COLLATERAL,
    ERR_INVALID_INPUT,
    ERR_INVALID_SCRIPT,
    ERR_INVALID_TX,
    ERR_MAXIMUM,
    ERR_MN_LIST,
    ERR_MODE,
    ERR_NON_STANDARD_PUBKEY, // not used
    ERR_NOT_A_MN, // not used
    ERR_QUEUE_FULL,
    ERR_RECENT,
    ERR_SESSION,
    ERR_MISSING_TX,
    ERR_VERSION,
    MSG_NOERR,
    MSG_SUCCESS,
    MSG_ENTRIES_ADDED,
    ERR_SIZE_MISMATCH,
    MSG_POOL_MIN = ERR_ALREADY_HAVE,
    MSG_POOL_MAX = ERR_SIZE_MISMATCH
};
template<> struct is_serializable_enum<PoolMessage> : std::true_type {};

// pool states
enum PoolState : int32_t {
    POOL_STATE_IDLE,
    POOL_STATE_QUEUE,
    POOL_STATE_ACCEPTING_ENTRIES,
    POOL_STATE_SIGNING,
    POOL_STATE_ERROR,
    POOL_STATE_MIN = POOL_STATE_IDLE,
    POOL_STATE_MAX = POOL_STATE_ERROR
};
template<> struct is_serializable_enum<PoolState> : std::true_type {};

// status update message constants
enum PoolStatusUpdate : int32_t {
    STATUS_REJECTED,
    STATUS_ACCEPTED
};
template<> struct is_serializable_enum<PoolStatusUpdate> : std::true_type {};

class CCoinJoinStatusUpdate
{
public:
    int nSessionID{0};
    PoolState nState{POOL_STATE_IDLE};
    int nEntriesCount{0}; // deprecated, kept for backwards compatibility
    PoolStatusUpdate nStatusUpdate{STATUS_ACCEPTED};
    PoolMessage nMessageID{MSG_NOERR};

    constexpr CCoinJoinStatusUpdate() = default;

    constexpr CCoinJoinStatusUpdate(int nSessionID, PoolState nState, int nEntriesCount, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) :
        nSessionID(nSessionID),
        nState(nState),
        nEntriesCount(nEntriesCount),
        nStatusUpdate(nStatusUpdate),
        nMessageID(nMessageID) {};

    SERIALIZE_METHODS(CCoinJoinStatusUpdate, obj)
    {
        READWRITE(obj.nSessionID, obj.nState, obj.nStatusUpdate, obj.nMessageID);
    }
};

class CCoinJoinAccept
{
public:
    int nDenom{0};
    CMutableTransaction txCollateral;

    CCoinJoinAccept() = default;

    CCoinJoinAccept(int nDenom, CMutableTransaction txCollateral) :
        nDenom(nDenom),
        txCollateral(std::move(txCollateral)){};

    SERIALIZE_METHODS(CCoinJoinAccept, obj)
    {
        READWRITE(obj.nDenom, obj.txCollateral);
    }

    friend bool operator==(const CCoinJoinAccept& a, const CCoinJoinAccept& b)
    {
        return a.nDenom == b.nDenom && CTransaction(a.txCollateral) == CTransaction(b.txCollateral);
    }
};

// A client's transaction in the mixing pool
class CCoinJoinEntry
{
public:
    std::vector<CTxDSIn> vecTxDSIn;
    std::vector<CTxOut> vecTxOut;
    CTransactionRef txCollateral;
    // memory only
    CService addr;

    CCoinJoinEntry() :
        txCollateral(MakeTransactionRef(CMutableTransaction{}))
    {
    }

    CCoinJoinEntry(std::vector<CTxDSIn> vecTxDSIn, std::vector<CTxOut> vecTxOut, const CTransaction& txCollateral) :
            vecTxDSIn(std::move(vecTxDSIn)),
            vecTxOut(std::move(vecTxOut)),
            txCollateral(MakeTransactionRef(txCollateral))
    {
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << vecTxDSIn << txCollateral << vecTxOut;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const size_t max_count{CoinJoin::GetMaxPoolInputOutputCount()};
        if (!UnserializeVectorWithMaxSize(s, vecTxDSIn, max_count)) {
            throw std::ios_base::failure("CCoinJoinEntry::vecTxDSIn size too large");
        }

        s >> txCollateral;

        if (!UnserializeVectorWithMaxSize(s, vecTxOut, max_count)) {
            throw std::ios_base::failure("CCoinJoinEntry::vecTxOut size too large");
        }
    }

    bool AddScriptSig(const CTxIn& txin);
};


/**
 * A currently in progress mixing merge and denomination information
 */
class CCoinJoinQueue
{
public:
    int nDenom{0};
    COutPoint masternodeOutpoint;
    uint256 m_protxHash;
    int64_t nTime{0};
    bool fReady{false}; //ready for submit
    std::vector<unsigned char> vchSig;
    // memory only
    bool fTried{false};

    CCoinJoinQueue() = default;

    CCoinJoinQueue(int nDenom, const COutPoint& outpoint, const uint256& proTxHash, NodeClock::time_point time, bool fReady) :
        nDenom(nDenom),
        masternodeOutpoint(outpoint),
        m_protxHash(proTxHash),
        nTime(TicksSinceEpoch<std::chrono::seconds>(time)),
        fReady(fReady)
    {
    }

    NodeSeconds Time() const { return NodeSeconds{std::chrono::seconds{nTime}}; }

    SERIALIZE_METHODS(CCoinJoinQueue, obj)
    {
        READWRITE(obj.nDenom, obj.m_protxHash, obj.nTime, obj.fReady);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(obj.vchSig);
        }
    }

    [[nodiscard]] uint256 GetHash() const;
    [[nodiscard]] uint256 GetSignatureHash() const;

    /// Check if we have a valid Masternode address
    [[nodiscard]] bool CheckSignature(const CBLSPublicKey& blsPubKey) const;

    /// Check if a queue is too old or too far into the future
    [[nodiscard]] bool IsTimeOutOfBounds(NodeSeconds current_time) const;
    [[nodiscard]] bool IsTimeOutOfBounds() const;

    [[nodiscard]] std::string ToString() const;

    friend bool operator==(const CCoinJoinQueue& a, const CCoinJoinQueue& b)
    {
        return a.nDenom == b.nDenom && a.masternodeOutpoint == b.masternodeOutpoint && a.nTime == b.nTime && a.fReady == b.fReady;
    }
};

/** Helper class to store mixing transaction (tx) information.
 */
class CCoinJoinBroadcastTx
{
private:
    // memory only
    // when corresponding tx is 0-confirmed or conflicted, nConfirmedHeight is std::nullopt
    std::optional<int> nConfirmedHeight{std::nullopt};

public:
    CTransactionRef tx;
    COutPoint masternodeOutpoint;
    uint256 m_protxHash;
    std::vector<unsigned char> vchSig;
    int64_t sigTime{0};
    CCoinJoinBroadcastTx() :
        tx(MakeTransactionRef(CMutableTransaction{}))
    {
    }

    CCoinJoinBroadcastTx(CTransactionRef _tx, const COutPoint& _outpoint, const uint256& proTxHash, NodeClock::time_point time) :
        tx(std::move(_tx)),
        masternodeOutpoint(_outpoint),
        m_protxHash(proTxHash),
        sigTime(TicksSinceEpoch<std::chrono::seconds>(time))
    {
    }

    SERIALIZE_METHODS(CCoinJoinBroadcastTx, obj)
    {
        READWRITE(obj.tx, obj.m_protxHash);

        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(obj.vchSig);
        }
        READWRITE(obj.sigTime);
    }

    friend bool operator==(const CCoinJoinBroadcastTx& a, const CCoinJoinBroadcastTx& b)
    {
        return *a.tx == *b.tx;
    }
    friend bool operator!=(const CCoinJoinBroadcastTx& a, const CCoinJoinBroadcastTx& b)
    {
        return !(a == b);
    }
    explicit operator bool() const
    {
        return *this != CCoinJoinBroadcastTx();
    }

    [[nodiscard]] uint256 GetSignatureHash() const;

    [[nodiscard]] bool CheckSignature(const CBLSPublicKey& blsPubKey) const;

    [[nodiscard]] const std::optional<int>& GetConfirmedHeight() const { return nConfirmedHeight; }
    void SetConfirmedHeight(std::optional<int> nConfirmedHeightIn) { assert(nConfirmedHeightIn == std::nullopt || *nConfirmedHeightIn > 0); nConfirmedHeight = nConfirmedHeightIn; }
    [[nodiscard]] bool IsValidStructure() const;
};

// base class
class CCoinJoinBaseSession
{
protected:
    mutable Mutex cs_coinjoin;

    std::vector<CCoinJoinEntry> vecEntries GUARDED_BY(cs_coinjoin); // Masternode/clients entries

    std::atomic<PoolState> nState{POOL_STATE_IDLE}; // should be one of the POOL_STATE_XXX values
    std::atomic<int64_t> nTimeLastSuccessfulStep{0}; // the time when last successful mixing step was performed

    std::atomic<int> nSessionID{0}; // 0 if no mixing session is active

    CMutableTransaction finalMutableTransaction GUARDED_BY(cs_coinjoin); // the finalized transaction ready for signing

    virtual void SetNull() EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    bool IsValidInOuts(Chainstate& active_chainstate, const llmq::CInstantSendManager& isman,
                       const CTxMemPool& mempool, const std::vector<CTxIn>& vin, const std::vector<CTxOut>& vout,
                       PoolMessage& nMessageIDRet, bool* fConsumeCollateralRet) const;

public:
    int nSessionDenom{0}; // Users must submit a denom matching this

    CCoinJoinBaseSession() = default;
    virtual ~CCoinJoinBaseSession() = default;

    int GetState() const { return nState; }
    std::string GetStateString() const;

    int GetEntriesCount() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin) { LOCK(cs_coinjoin); return vecEntries.size(); }
    int GetEntriesCountLocked() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin) { return vecEntries.size(); }
};

class CoinJoinQueueManager
{
private:
    mutable Mutex cs_vecqueue;

    // The current mixing sessions in progress on the network
    std::vector<CCoinJoinQueue> vecCoinJoinQueue GUARDED_BY(cs_vecqueue);

public:
    void SetNull() EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);

    //! Remove timed-out queue entries. Call periodically (e.g. every second).
    void CheckQueue() EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);

    int GetQueueSize() const EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue) { LOCK(cs_vecqueue); return vecCoinJoinQueue.size(); }
    bool GetQueueItemAndTry(CCoinJoinQueue& dsqRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);

    bool HasQueue(const uint256& queueHash) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
    {
        LOCK(cs_vecqueue);
        return std::any_of(vecCoinJoinQueue.begin(), vecCoinJoinQueue.end(),
                           [&queueHash](auto q) { return q.GetHash() == queueHash; });
    }
    std::optional<CCoinJoinQueue> GetQueueFromHash(const uint256& queueHash) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
    {
        LOCK(cs_vecqueue);
        return util::find_if_opt(vecCoinJoinQueue, [&queueHash](const auto& q) { return q.GetHash() == queueHash; });
    }

    //! True if any queue entry matches the given masternode outpoint and readiness state.
    //! Used to detect when a masternode is broadcasting queues too quickly.
    bool HasQueueFromMasternode(const COutPoint& outpoint, bool fReady) const EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
    {
        LOCK(cs_vecqueue);
        return std::any_of(vecCoinJoinQueue.begin(), vecCoinJoinQueue.end(),
                           [&](const auto& q) { return q.masternodeOutpoint == outpoint && q.fReady == fReady; });
    }
    //! TRY_LOCK variant: returns nullopt if lock can't be acquired; true if any queue entry has this
    //! outpoint (any readiness).
    [[nodiscard]] std::optional<bool> TryHasQueueFromMasternode(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);
    //! TRY_LOCK combined duplicate check: returns nullopt if lock can't be acquired; true if dsq is
    //! an exact duplicate or the masternode is sending too many dsqs with the same readiness.
    [[nodiscard]] std::optional<bool> TryCheckDuplicate(const CCoinJoinQueue& dsq) const EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);

    //! Append a queue entry (caller must have already checked for duplicates).
    void AddQueue(CCoinJoinQueue dsq) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
    {
        LOCK(cs_vecqueue);
        vecCoinJoinQueue.push_back(std::move(dsq));
    }
    //! TRY_LOCK variant of AddQueue: returns false if the lock cannot be acquired.
    bool TryAddQueue(CCoinJoinQueue dsq) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue);
};

/**
 * Remembers the final mixing transactions this node took part in, so that it can avoid
 * announcing them to the rest of the network.
 *
 * A participant learns the session transaction directly from the coordinating masternode,
 * long before the network sees it through the coordinator's deliberately delayed inventory
 * relay. Announcing it that early would make the participant look like the transaction's
 * origin to a network-wide observer, which -- since every participant of a round would do so
 * at the same time -- would expose the whole participant set of that round. That is precisely
 * what mixing exists to hide, so we stay quiet and let the coordinator publish. Propagation
 * does not suffer: the transaction reaches the network through the coordinator either way.
 *
 * A participant cannot identify the session transaction by its id: Dash has no segwit, so
 * signatures are covered by the txid, and a participant only ever holds its own -- the
 * coordinator merges everyone else's in afterwards. Sessions are therefore recognised by the
 * outpoints of the inputs we signed, and NoteTransaction() resolves those to a txid once the
 * transaction itself shows up, so that the relay path stays a single hash lookup. Matching costs
 * one probe per input of each newly accepted transaction; only the relay path is a plain lookup.
 *
 * Matching on inputs is deliberately broad. Session signatures are made with
 * SIGHASH_ALL|SIGHASH_ANYONECANPAY, so they commit to their own input and to the outputs but not
 * to the other inputs -- which means the coordinator can build variants of the session
 * transaction that still spend ours -- as can anyone else who has seen the signature, by copying
 * it into a variant of their own. Every such variant is derived from our session, so suppressing
 * all of them is the point. For the same reason outpoints are kept after a session fails: a usable
 * signature is still out there, and forgetting the outpoint would let a variant be published that
 * we then announce. Our own ordinary payments are unaffected: they are broadcast through the
 * wallet rather than accepted from a peer, so they never pass through NoteTransaction().
 *
 * Both sets are LRU caches, so entries age out once they stop being used rather than merely
 * because newer entries came along. Suppression is still not unlimited: a transaction nobody
 * mentions for MAX_TRACKED newer insertions is forgotten, and could then be announced by us.
 *
 * Suppression also expires on its own, after a randomised delay of hours. Without that, a
 * coordinator could announce the finished transaction to its participants alone and to nobody
 * else: every participant would suppress it, so it would sit unconfirmed in exactly their
 * mempools, holding their mixing inputs hostage until it expired from the mempool days later.
 * Our own relay used to be the redundancy that made such silence harmless, and suppressing it
 * is what takes that away, so the suppression has to end by itself. Once it does, the wallet's
 * ordinary rebroadcast picks the transaction up and announces it.
 *
 * The delay is long enough that the round is unambiguously over by the time it elapses, so the
 * simultaneity that identifies a participant set is gone. On a healthy round it never matters:
 * the transaction confirms in a block or two, and a confirmed transaction is never rebroadcast.
 * When it does matter, the participants' deadlines are independent, so whichever elapses first
 * publishes the transaction for everyone and only that one node -- picked at random among them,
 * rather than all of them -- says anything at all. Expiry only lifts the suppression: the
 * announcement itself waits for the wallet's next rebroadcast, an hour to three later on its own
 * randomised timer, so the delay below is a lower bound on how long we stay quiet rather than the
 * moment we speak.
 */
class CoinJoinSessionTxTracker
{
public:
    //! How many signed outpoints, and how many matched transactions, to remember -- each cache
    //! holds up to this many entries, at a few tens of bytes apiece. A mixing round contributes
    //! one entry per input of ours to the outpoint cache but only one to the txid cache, so the
    //! outpoint cache turns over first and is what bounds how far back suppression reaches.
    //! Passed as the caches' truncation threshold as well, so that they hold at most this many
    //! entries instead of growing to twice as many before trimming.
    static constexpr size_t MAX_TRACKED{1000};

    //! Bounds on how long a transaction stays suppressed. Anything comfortably past the end of a
    //! round will do; these are hours so that a stuck transaction is not held back for long, and
    //! randomised so that participants of the same round do not come off suppression together.
    static constexpr int64_t MIN_SUPPRESS_SECONDS{2 * 60 * 60};
    static constexpr int64_t MAX_SUPPRESS_SECONDS{6 * 60 * 60};

private:
    mutable Mutex cs_tracker;

    //! Mutable because the LRU bookkeeping updates on lookup, as in the guarded-mutable caches
    //! in InstantSend and masternode metadata.
    mutable unordered_lru_cache<COutPoint, bool, SaltedOutpointHasher, MAX_TRACKED, MAX_TRACKED>
        m_signed_outpoints GUARDED_BY(cs_tracker);
    //! Maps a transaction we took part in to the time its suppression stops applying.
    mutable Uint256LruHashMap<int64_t, MAX_TRACKED, MAX_TRACKED> m_txids GUARDED_BY(cs_tracker);
    //! Draws the suppression delays. Kept as a member rather than reaching for GetRand() so that
    //! it can be seeded deterministically: GetRand() builds a fresh context per call, which under
    //! g_mock_deterministic_tests returns the same number every time and would leave every
    //! deadline identical -- the one thing the delay must not be.
    FastRandomContext m_rng GUARDED_BY(cs_tracker);

public:
    //! deterministic_rng is for tests that need a reproducible spread of deadlines; left false,
    //! the context seeds itself securely on first use.
    explicit CoinJoinSessionTxTracker(bool deterministic_rng = false) :
        m_rng{deterministic_rng} {}

    //! Record the outpoints of the inputs this node signed for a mixing session.
    void NoteSignedOutpoints(const std::vector<COutPoint>& outpoints) EXCLUSIVE_LOCKS_REQUIRED(!cs_tracker)
    {
        LOCK(cs_tracker);
        for (const auto& outpoint : outpoints) {
            m_signed_outpoints.insert(outpoint, true);
        }
    }

    //! If this transaction spends an outpoint we signed for in a session, remember it as ours and
    //! set the time its suppression stops applying. A transaction we already know keeps the
    //! deadline it was given, so that repeated notes cannot hold it back. Only one evicted from
    //! the cache and then accepted afresh can pick up a new deadline, and being accepted again
    //! means it had left our mempool in the meantime.
    void NoteTransaction(const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(!cs_tracker)
    {
        LOCK(cs_tracker);
        const uint256& txid = tx.GetHash();
        if (m_txids.exists(txid)) return;
        for (const auto& txin : tx.vin) {
            if (m_signed_outpoints.exists(txin.prevout)) {
                const int64_t delay{MIN_SUPPRESS_SECONDS +
                                    int64_t(m_rng.randrange(MAX_SUPPRESS_SECONDS - MIN_SUPPRESS_SECONDS + 1))};
                m_txids.insert(txid, GetTime() + delay);
                return;
            }
        }
    }

    //! True while this node must stay quiet about a transaction it took part in, as established
    //! by an earlier NoteTransaction(). Goes false once the suppression deadline passes, so that
    //! a transaction nobody else published still gets announced eventually. Looking it up marks
    //! it as still relevant.
    bool Contains(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(!cs_tracker)
    {
        LOCK(cs_tracker);
        int64_t suppress_until{0};
        if (!m_txids.get(txid, suppress_until)) return false;
        return GetTime() < suppress_until;
    }
};

// Various helpers and dstx manager implementation
namespace CoinJoin
{
    bilingual_str GetMessageByID(PoolMessage nMessageID);

    constexpr CAmount GetMaxPoolAmount() { return COINJOIN_ENTRY_MAX_SIZE * vecStandardDenominations.front(); }

    /// If the collateral is valid given by a client
    bool IsCollateralValid(ChainstateManager& chainman, const llmq::CInstantSendManager& isman,
                           const CTxMemPool& mempool, const CTransaction& txCollateral);
}

class CDSTXManager
{
    const chainlock::Chainlocks& m_chainlocks;
    Mutex cs_mapdstx;
    std::map<uint256, CCoinJoinBroadcastTx> mapDSTX GUARDED_BY(cs_mapdstx);

public:
    CDSTXManager(const CDSTXManager&) = delete;
    CDSTXManager& operator=(const CDSTXManager&) = delete;
    CDSTXManager(const chainlock::Chainlocks& chainlocks);
    ~CDSTXManager();

    void AddDSTX(const CCoinJoinBroadcastTx& dstx) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    CCoinJoinBroadcastTx GetDSTX(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);

    // CDSNotificationInterface
    void UpdatedBlockTip(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    void NotifyChainLock(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    void TransactionAddedToMempool(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex*)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);

private:
    bool IsTxExpired(const CCoinJoinBroadcastTx& tx, const CBlockIndex* pindex) const EXCLUSIVE_LOCKS_REQUIRED(cs_mapdstx);
    void CheckDSTXes(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs_mapdstx);
    void UpdateDSTXConfirmedHeight(const CTransactionRef& tx, std::optional<int> nHeight)
        EXCLUSIVE_LOCKS_REQUIRED(cs_mapdstx);
};

bool ATMPIfSaneFee(ChainstateManager& chainman, const CTransactionRef& tx, bool test_accept = false)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

#endif // BITCOIN_COINJOIN_COINJOIN_H
