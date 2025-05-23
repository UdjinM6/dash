// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_INSTANTSEND_H
#define BITCOIN_LLMQ_INSTANTSEND_H

#include <llmq/signing.h>

#include <consensus/params.h>
#include <net_types.h>
#include <primitives/transaction.h>
#include <threadinterrupt.h>
#include <txmempool.h>
#include <unordered_lru_cache.h>

#include <gsl/pointers.h>

#include <atomic>
#include <unordered_map>
#include <unordered_set>

class CBlockIndex;
class CChainState;
class CConnman;
class CDBWrapper;
class CMasternodeSync;
class CSporkManager;
class PeerManager;

namespace llmq
{
class CChainLocksHandler;
class CQuorumManager;
class CSigningManager;
class CSigSharesManager;

struct CInstantSendLock
{
    static constexpr uint8_t CURRENT_VERSION{1};

    uint8_t nVersion{CURRENT_VERSION};
    std::vector<COutPoint> inputs;
    uint256 txid;
    uint256 cycleHash;
    CBLSLazySignature sig;

    CInstantSendLock() = default;

    SERIALIZE_METHODS(CInstantSendLock, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(obj.inputs);
        READWRITE(obj.txid);
        READWRITE(obj.cycleHash);
        READWRITE(obj.sig);
    }

    uint256 GetRequestId() const;
    bool TriviallyValid() const;
};

using CInstantSendLockPtr = std::shared_ptr<CInstantSendLock>;

class CInstantSendDb
{
private:
    mutable Mutex cs_db;

    static constexpr int CURRENT_VERSION{1};

    int best_confirmed_height GUARDED_BY(cs_db) {0};

    std::unique_ptr<CDBWrapper> db GUARDED_BY(cs_db) {nullptr};
    mutable unordered_lru_cache<uint256, CInstantSendLockPtr, StaticSaltedHasher, 10000> islockCache GUARDED_BY(cs_db);
    mutable unordered_lru_cache<uint256, uint256, StaticSaltedHasher, 10000> txidCache GUARDED_BY(cs_db);

    mutable unordered_lru_cache<COutPoint, uint256, SaltedOutpointHasher, 10000> outpointCache GUARDED_BY(cs_db);
    void WriteInstantSendLockMined(CDBBatch& batch, const uint256& hash, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(cs_db);

    void RemoveInstantSendLockMined(CDBBatch& batch, const uint256& hash, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(cs_db);

    /**
     * This method removes a InstantSend Lock from the database and is called when a tx with an IS lock is confirmed and Chainlocked
     * @param batch Object used to batch many calls together
     * @param hash The hash of the InstantSend Lock
     * @param islock The InstantSend Lock object itself
     * @param keep_cache Should we still keep corresponding entries in the cache or not
     */
    void RemoveInstantSendLock(CDBBatch& batch, const uint256& hash, CInstantSendLockPtr islock, bool keep_cache = true) EXCLUSIVE_LOCKS_REQUIRED(cs_db);
    /**
     * Marks an InstantSend Lock as archived.
     * @param batch Object used to batch many calls together
     * @param hash The hash of the InstantSend Lock
     * @param nHeight The height that the transaction was included at
     */
    void WriteInstantSendLockArchived(CDBBatch& batch, const uint256& hash, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(cs_db);
    /**
     * Gets a vector of IS Lock hashes of the IS Locks which rely on or are children of the parent IS Lock
     * @param parent The hash of the parent IS Lock
     * @return Returns a vector of IS Lock hashes
     */
    std::vector<uint256> GetInstantSendLocksByParent(const uint256& parent) const EXCLUSIVE_LOCKS_REQUIRED(cs_db);

    /**
     * See GetInstantSendLockByHash
     */
    CInstantSendLockPtr GetInstantSendLockByHashInternal(const uint256& hash, bool use_cache = true) const EXCLUSIVE_LOCKS_REQUIRED(cs_db);

    /**
     * See GetInstantSendLockHashByTxid
     */
    uint256 GetInstantSendLockHashByTxidInternal(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(cs_db);


    void Upgrade(bool unitTests) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);

public:
    explicit CInstantSendDb(bool unitTests, bool fWipe);
    ~CInstantSendDb();

    /**
     * This method is called when an InstantSend Lock is processed and adds the lock to the database
     * @param hash The hash of the InstantSend Lock
     * @param islock The InstantSend Lock object itself
     */
    void WriteNewInstantSendLock(const uint256& hash, const CInstantSendLock& islock) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * This method updates a DB entry for an InstantSend Lock from being not included in a block to being included in a block
     * @param hash The hash of the InstantSend Lock
     * @param nHeight The height that the transaction was included at
     */
    void WriteInstantSendLockMined(const uint256& hash, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Archives and deletes all IS Locks which were mined into a block before nUntilHeight
     * @param nUntilHeight Removes all IS Locks confirmed up until nUntilHeight
     * @return returns an unordered_map of the hash of the IS Locks and a pointer object to the IS Locks for all IS Locks which were removed
     */
    std::unordered_map<uint256, CInstantSendLockPtr, StaticSaltedHasher> RemoveConfirmedInstantSendLocks(int nUntilHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Removes IS Locks from the archive if the tx was confirmed 100 blocks before nUntilHeight
     * @param nUntilHeight the height from which to base the remove of archive IS Locks
     */
    void RemoveArchivedInstantSendLocks(int nUntilHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    void WriteBlockInstantSendLocks(const gsl::not_null<std::shared_ptr<const CBlock>>& pblock, gsl::not_null<const CBlockIndex*> pindexConnected) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    void RemoveBlockInstantSendLocks(const gsl::not_null<std::shared_ptr<const CBlock>>& pblock, gsl::not_null<const CBlockIndex*> pindexDisconnected) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    bool KnownInstantSendLock(const uint256& islockHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Gets the number of IS Locks which have not been confirmed by a block
     * @return size_t value of the number of IS Locks not confirmed by a block
     */
    size_t GetInstantSendLockCount() const EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Gets a pointer to the IS Lock based on the hash
     * @param hash The hash of the IS Lock
     * @param use_cache Should we try using the cache first or not
     * @return A Pointer object to the IS Lock, returns nullptr if it doesn't exist
     */
    CInstantSendLockPtr GetInstantSendLockByHash(const uint256& hash, bool use_cache = true) const EXCLUSIVE_LOCKS_REQUIRED(!cs_db)
    {
        LOCK(cs_db);
        return GetInstantSendLockByHashInternal(hash, use_cache);
    };
    /**
     * Gets an IS Lock hash based on the txid the IS Lock is for
     * @param txid The txid which is being searched for
     * @return Returns the hash the IS Lock of the specified txid, returns uint256() if it doesn't exist
     */
    uint256 GetInstantSendLockHashByTxid(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(!cs_db)
    {
        LOCK(cs_db);
        return GetInstantSendLockHashByTxidInternal(txid);
    };
    /**
     * Gets an IS Lock pointer from the txid given
     * @param txid The txid for which the IS Lock Pointer is being returned
     * @return Returns the IS Lock Pointer associated with the txid, returns nullptr if it doesn't exist
     */
    CInstantSendLockPtr GetInstantSendLockByTxid(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Gets an IS Lock pointer from an input given
     * @param outpoint Since all inputs are really just outpoints that are being spent
     * @return IS Lock Pointer associated with that input.
     */
    CInstantSendLockPtr GetInstantSendLockByInput(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
    /**
     * Called when a ChainLock invalidated a IS Lock, removes any chained/children IS Locks and the invalidated IS Lock
     * @param islockHash IS Lock hash which has been invalidated
     * @param txid Transaction id associated with the islockHash
     * @param nHeight height of the block which received a chainlock and invalidated the IS Lock
     * @return A vector of IS Lock hashes of all IS Locks removed
     */
    std::vector<uint256> RemoveChainedInstantSendLocks(const uint256& islockHash, const uint256& txid, int nHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs_db);
};

class CInstantSendManager : public CRecoveredSigsListener
{
private:
    CInstantSendDb db;

    CChainLocksHandler& clhandler;
    CChainState& m_chainstate;
    CQuorumManager& qman;
    CSigningManager& sigman;
    CSigSharesManager& shareman;
    CSporkManager& spork_manager;
    CTxMemPool& mempool;
    const CMasternodeSync& m_mn_sync;

    const bool m_is_masternode;

    std::thread workThread;
    CThreadInterrupt workInterrupt;

    mutable Mutex cs_inputReqests;

    /**
     * Request ids of inputs that we signed. Used to determine if a recovered signature belongs to an
     * in-progress input lock.
     */
    std::unordered_set<uint256, StaticSaltedHasher> inputRequestIds GUARDED_BY(cs_inputReqests);

    mutable Mutex cs_creating;

    /**
     * These are the islocks that are currently in the middle of being created. Entries are created when we observed
     * recovered signatures for all inputs of a TX. At the same time, we initiate signing of our sigshare for the islock.
     * When the recovered sig for the islock later arrives, we can finish the islock and propagate it.
     */
    std::unordered_map<uint256, CInstantSendLock, StaticSaltedHasher> creatingInstantSendLocks GUARDED_BY(cs_creating);
    // maps from txid to the in-progress islock
    std::unordered_map<uint256, CInstantSendLock*, StaticSaltedHasher> txToCreatingInstantSendLocks GUARDED_BY(cs_creating);

    mutable Mutex cs_pendingLocks;
    // Incoming and not verified yet
    std::unordered_map<uint256, std::pair<NodeId, CInstantSendLockPtr>, StaticSaltedHasher> pendingInstantSendLocks GUARDED_BY(cs_pendingLocks);
    // Tried to verify but there is no tx yet
    std::unordered_map<uint256, std::pair<NodeId, CInstantSendLockPtr>, StaticSaltedHasher> pendingNoTxInstantSendLocks GUARDED_BY(cs_pendingLocks);

    // TXs which are neither IS locked nor ChainLocked. We use this to determine for which TXs we need to retry IS locking
    // of child TXs
    struct NonLockedTxInfo {
        const CBlockIndex* pindexMined;
        CTransactionRef tx;
        std::unordered_set<uint256, StaticSaltedHasher> children;
    };

    mutable Mutex cs_nonLocked;
    std::unordered_map<uint256, NonLockedTxInfo, StaticSaltedHasher> nonLockedTxs GUARDED_BY(cs_nonLocked);
    std::unordered_map<COutPoint, uint256, SaltedOutpointHasher> nonLockedTxsByOutpoints GUARDED_BY(cs_nonLocked);

    mutable Mutex cs_pendingRetry;
    std::unordered_set<uint256, StaticSaltedHasher> pendingRetryTxs GUARDED_BY(cs_pendingRetry);

    mutable Mutex cs_timingsTxSeen;
    std::unordered_map<uint256, int64_t, StaticSaltedHasher> timingsTxSeen GUARDED_BY(cs_timingsTxSeen);

public:
    explicit CInstantSendManager(CChainLocksHandler& _clhandler, CChainState& chainstate, CQuorumManager& _qman,
                                 CSigningManager& _sigman, CSigSharesManager& _shareman, CSporkManager& sporkman,
                                 CTxMemPool& _mempool, const CMasternodeSync& mn_sync, bool is_masternode,
                                 bool unitTests, bool fWipe) :
        db(unitTests, fWipe),
        clhandler(_clhandler),
        m_chainstate(chainstate),
        qman(_qman),
        sigman(_sigman),
        shareman(_shareman),
        spork_manager(sporkman),
        mempool(_mempool),
        m_mn_sync(mn_sync),
        m_is_masternode{is_masternode}
    {
        workInterrupt.reset();
    }
    ~CInstantSendManager() = default;

    void Start(PeerManager& peerman);
    void Stop();
    void InterruptWorkerThread() { workInterrupt(); };

private:
    void ProcessTx(const CTransaction& tx, bool fRetroactive, const Consensus::Params& params)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests);
    bool CheckCanLock(const CTransaction& tx, bool printDebug, const Consensus::Params& params) const;
    bool CheckCanLock(const COutPoint& outpoint, bool printDebug, const uint256& txHash,
                      const Consensus::Params& params) const;

    void HandleNewInputLockRecoveredSig(const CRecoveredSig& recoveredSig, const uint256& txid)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);
    void HandleNewInstantSendLockRecoveredSig(const CRecoveredSig& recoveredSig)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_pendingLocks);

    bool TrySignInputLocks(const CTransaction& tx, bool allowResigning, Consensus::LLMQType llmqType,
                           const Consensus::Params& params) EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests);
    void TrySignInstantSendLock(const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);

    PeerMsgRet ProcessMessageInstantSendLock(const CNode& pfrom, PeerManager& peerman, const CInstantSendLockPtr& islock);
    bool ProcessPendingInstantSendLocks(PeerManager& peerman)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);

    std::unordered_set<uint256, StaticSaltedHasher> ProcessPendingInstantSendLocks(
        const Consensus::LLMQParams& llmq_params, PeerManager& peerman, int signOffset,
        const std::unordered_map<uint256, std::pair<NodeId, CInstantSendLockPtr>, StaticSaltedHasher>& pend, bool ban)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);
    void ProcessInstantSendLock(NodeId from, PeerManager& peerman, const uint256& hash, const CInstantSendLockPtr& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);

    void AddNonLockedTx(const CTransactionRef& tx, const CBlockIndex* pindexMined)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingLocks);
    void RemoveNonLockedTx(const uint256& txid, bool retryChildren)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingRetry);
    void RemoveConflictedTx(const CTransaction& tx)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);
    void TruncateRecoveredSigsForInputs(const CInstantSendLock& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests);

    void RemoveMempoolConflictsForLock(PeerManager& peerman, const uint256& hash, const CInstantSendLock& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);
    void ResolveBlockConflicts(const uint256& islockHash, const CInstantSendLock& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);
    void ProcessPendingRetryLockTxs()
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);

    void WorkThreadMain(PeerManager& peerman)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);

    void HandleFullyConfirmedBlock(const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);

public:
    bool IsLocked(const uint256& txHash) const;
    bool IsWaitingForTx(const uint256& txHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    CInstantSendLockPtr GetConflictingLock(const CTransaction& tx) const;

    [[nodiscard]] MessageProcessingResult HandleNewRecoveredSig(const CRecoveredSig& recoveredSig) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_pendingLocks);

    PeerMsgRet ProcessMessage(const CNode& pfrom, PeerManager& peerman, std::string_view msg_type, CDataStream& vRecv);

    void TransactionAddedToMempool(PeerManager& peerman, const CTransactionRef& tx)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);
    void TransactionRemovedFromMempool(const CTransactionRef& tx);
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests, !cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected);

    bool AlreadyHave(const CInv& inv) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    bool GetInstantSendLockByHash(const uint256& hash, CInstantSendLock& ret) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    CInstantSendLockPtr GetInstantSendLockByTxid(const uint256& txid) const;

    void NotifyChainLock(const CBlockIndex* pindexChainLock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);
    void UpdatedBlockTip(const CBlockIndex* pindexNew)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests, !cs_nonLocked, !cs_pendingRetry);

    void RemoveConflictingLock(const uint256& islockHash, const CInstantSendLock& islock);

    size_t GetInstantSendLockCount() const;

    bool IsInstantSendEnabled() const;
    /**
     * If true, MN should sign all transactions, if false, MN should not sign
     * transactions in mempool, but should sign txes included in a block. This
     * allows ChainLocks to continue even while this spork is disabled.
     */
    bool IsInstantSendMempoolSigningEnabled() const;
    bool RejectConflictingBlocks() const;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_INSTANTSEND_H
