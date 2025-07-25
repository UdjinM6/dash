// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_WALLET_H
#define BITCOIN_WALLET_WALLET_H

#include <consensus/amount.h>
#include <fs.h>
#include <governance/common.h>
#include <interfaces/chain.h>
#include <interfaces/coinjoin.h>
#include <interfaces/handler.h>
#include <policy/feerate.h>
#include <psbt.h>
#include <saltedhasher.h>
#include <tinyformat.h>
#include <util/hasher.h>
#include <util/message.h>
#include <util/string.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <util/ui_change_type.h>
#include <validationinterface.h>
#include <wallet/crypter.h>
#include <wallet/coinselection.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/transaction.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <unordered_map>
#include <vector>

#include <boost/signals2/signal.hpp>

class CKey;
class CScript;
class CTxDSIn;
enum class FeeEstimateMode;
struct FeeCalculation;
struct bilingual_str;

using LoadWalletFn = std::function<void(std::unique_ptr<interfaces::Wallet> wallet)>;

namespace wallet {
struct WalletContext;

//! Explicitly unload and delete the wallet.
//  Blocks the current thread after signaling the unload intent so that all
//  wallet pointer owners release the wallet.
//  Note that, when blocking is not required, the wallet is implicitly unloaded
//  by the shared pointer deleter.
void UnloadWallet(std::shared_ptr<CWallet>&& wallet);

bool AddWallet(WalletContext& context, const std::shared_ptr<CWallet>& wallet);
bool RemoveWallet(WalletContext& context, const std::shared_ptr<CWallet>& wallet, std::optional<bool> load_on_start, std::vector<bilingual_str>& warnings);
bool RemoveWallet(WalletContext& context, const std::shared_ptr<CWallet>& wallet, std::optional<bool> load_on_start);
std::vector<std::shared_ptr<CWallet>> GetWallets(WalletContext& context);
std::shared_ptr<CWallet> GetWallet(WalletContext& context, const std::string& name);
std::shared_ptr<CWallet> LoadWallet(WalletContext& context, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings);
std::shared_ptr<CWallet> CreateWallet(WalletContext& context, const std::string& name, std::optional<bool> load_on_start, DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings);
std::shared_ptr<CWallet> RestoreWallet(WalletContext& context, const fs::path& backup_file, const std::string& wallet_name, std::optional<bool> load_on_start, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings);
std::unique_ptr<interfaces::Handler> HandleLoadWallet(WalletContext& context, LoadWalletFn load_wallet);
void NotifyWalletLoaded(WalletContext& context, const std::shared_ptr<CWallet>& wallet);
std::unique_ptr<WalletDatabase> MakeWalletDatabase(const std::string& name, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error);

//! -paytxfee default
constexpr CAmount DEFAULT_PAY_TX_FEE = 0;
//! -fallbackfee default
static const CAmount DEFAULT_FALLBACK_FEE = 1000;
//! -discardfee default
static const CAmount DEFAULT_DISCARD_FEE = 10000;
//! -mintxfee default
static const CAmount DEFAULT_TRANSACTION_MINFEE = 1000;
//! -consolidatefeerate default
static const CAmount DEFAULT_CONSOLIDATE_FEERATE{1000}; // 10 sat/vbyte
/**
 * maximum fee increase allowed to do partial spend avoidance, even for nodes with this feature disabled by default
 *
 * A value of -1 disables this feature completely.
 * A value of 0 (current default) means to attempt to do partial spend avoidance, and use its results if the fees remain *unchanged*
 * A value > 0 means to do partial spend avoidance if the fee difference against a regular coin selection instance is in the range [0..value].
 */
static const CAmount DEFAULT_MAX_AVOIDPARTIALSPEND_FEE = 0;
//! discourage APS fee higher than this amount
constexpr CAmount HIGH_APS_FEE{COIN / 10000};
//! minimum recommended increment for BIP 125 replacement txs
static const CAmount WALLET_INCREMENTAL_RELAY_FEE = 5000;
//! Default for -spendzeroconfchange
static const bool DEFAULT_SPEND_ZEROCONF_CHANGE = true;
//! Default for -walletrejectlongchains
static const bool DEFAULT_WALLET_REJECT_LONG_CHAINS{true};
//! -txconfirmtarget default
static const unsigned int DEFAULT_TX_CONFIRM_TARGET = 6;
static const bool DEFAULT_WALLETBROADCAST = true;
static const bool DEFAULT_DISABLE_WALLET = false;
//! -maxtxfee default
static const CAmount DEFAULT_TRANSACTION_MAXFEE = COIN / 10;
//! Discourage users to set fees higher than this amount (in satoshis) per kB
static const CAmount HIGH_TX_FEE_PER_KB = COIN / 100;
//! -maxtxfee will warn if called with a higher fee than this amount (in satoshis)
static const CAmount HIGH_MAX_TX_FEE = 100 * HIGH_TX_FEE_PER_KB;
//! Pre-calculated constants for input size estimation in *virtual size*
static constexpr size_t DUMMY_NESTED_P2PKH_INPUT_SIZE = 113;

//! if set, all keys will be derived by using BIP39/BIP44
static const bool DEFAULT_USE_HD_WALLET = true;

class CCoinControl;
class CWalletTx;
class ReserveDestination;

extern RecursiveMutex cs_main;

/** (client) version numbers for particular wallet features */
struct CompactTallyItem
{
    CTxDestination txdest;
    CAmount nAmount{0};
    std::vector<COutPoint> outpoints;
    CompactTallyItem() = default;
};

static constexpr uint64_t KNOWN_WALLET_FLAGS =
        WALLET_FLAG_AVOID_REUSE
    |   WALLET_FLAG_BLANK_WALLET
    |   WALLET_FLAG_KEY_ORIGIN_METADATA
    |   WALLET_FLAG_LAST_HARDENED_XPUB_CACHED
    |   WALLET_FLAG_DISABLE_PRIVATE_KEYS
    |   WALLET_FLAG_DESCRIPTORS;

static constexpr uint64_t MUTABLE_WALLET_FLAGS =
        WALLET_FLAG_AVOID_REUSE;

static const std::map<std::string,WalletFlags> WALLET_FLAG_MAP{
    {"avoid_reuse", WALLET_FLAG_AVOID_REUSE},
    {"blank", WALLET_FLAG_BLANK_WALLET},
    {"key_origin_metadata", WALLET_FLAG_KEY_ORIGIN_METADATA},
    {"last_hardened_xpub_cached", WALLET_FLAG_LAST_HARDENED_XPUB_CACHED},
    {"disable_private_keys", WALLET_FLAG_DISABLE_PRIVATE_KEYS},
    {"descriptor_wallet", WALLET_FLAG_DESCRIPTORS},
};

extern const std::map<uint64_t,std::string> WALLET_FLAG_CAVEATS;

/** A wrapper to reserve an address from a wallet
 *
 * ReserveDestination is used to reserve an address. It is passed around
 * during the CreateTransaction/CommitTransaction procedure.
 *
 * Instantiating a ReserveDestination does not reserve an address. To do so,
 * GetReservedDestination() needs to be called on the object. Once an address has been
 * reserved, call KeepDestination() on the ReserveDestination object to make sure it is not
 * returned. Call ReturnDestination() to return the address so it can be re-used (for
 * example, if the address was used in a new transaction
 * and that transaction was not completed and needed to be aborted).
 *
 * If an address is reserved and KeepDestination() is not called, then the address will be
 * returned when the ReserveDestination goes out of scope.
 */
class ReserveDestination
{
protected:
    //! The wallet to reserve from
    const CWallet* const pwallet;
    //! The ScriptPubKeyMan to reserve from. Based on type when GetReservedDestination is called
    ScriptPubKeyMan* m_spk_man{nullptr};

    //! The index of the address's key in the keypool
    int64_t nIndex{-1};
    //! The destination
    CTxDestination address;
    //! Whether this is from the internal (change output) keypool
    bool fInternal{false};

public:
    //! Construct a ReserveDestination object. This does NOT reserve an address yet
    explicit ReserveDestination(CWallet* pwallet)
      : pwallet(pwallet)
      { }

    ReserveDestination(const ReserveDestination&) = delete;
    ReserveDestination& operator=(const ReserveDestination&) = delete;

    //! Destructor. If a key has been reserved and not KeepKey'ed, it will be returned to the keypool
    ~ReserveDestination()
    {
        ReturnDestination();
    }

    //! Reserve an address
    bool GetReservedDestination(CTxDestination& pubkey, bool internal);
    //! Return reserved address
    void ReturnDestination();
    //! Keep the address. Do not return its key to the keypool when this object goes out of scope
    void KeepDestination();
};

/** Address book data */
class CAddressBookData
{
private:
    bool m_change{true};
    std::string m_label;
public:
    std::string purpose;

    CAddressBookData() : purpose("unknown") {}

    typedef std::map<std::string, std::string> StringMap;
    StringMap destdata;

    bool IsChange() const { return m_change; }
    const std::string& GetLabel() const { return m_label; }
    void SetLabel(const std::string& label) {
        m_change = false;
        m_label = label;
    }
};

struct CRecipient
{
    CScript scriptPubKey;
    CAmount nAmount;
    bool fSubtractFeeFromAmount;
};

struct WalletTxHasher
{
    StaticSaltedHasher h;
    size_t operator()(const CWalletTx* a) const
    {
        return h(a->GetHash());
    }
};

class WalletRescanReserver; //forward declarations for ScanForWalletTransactions/RescanFromTime
/**
 * A CWallet maintains a set of transactions and balances, and provides the ability to create new transactions.
 */
class CWallet final : public WalletStorage, public interfaces::Chain::Notifications
{
private:
    CKeyingMaterial vMasterKey GUARDED_BY(cs_wallet);

    //! if fOnlyMixingAllowed is true, only mixing should be allowed in unlocked wallet
    bool fOnlyMixingAllowed;

    bool Unlock(const CKeyingMaterial& vMasterKeyIn, bool fForMixingOnly = false, bool accept_no_keys = false);

    std::atomic<bool> fAbortRescan{false}; // reset by WalletRescanReserver::reserve()
    std::atomic<bool> fScanningWallet{false}; // controlled by WalletRescanReserver
    std::atomic<bool> m_attaching_chain{false};
    std::atomic<int64_t> m_scanning_start{0};
    std::atomic<double> m_scanning_progress{0};
    friend class WalletRescanReserver;

    //! the current wallet version: clients below this version are not able to load the wallet
    int nWalletVersion GUARDED_BY(cs_wallet){FEATURE_BASE};

    int64_t nNextResend = 0;
    /** Whether this wallet will submit newly created transactions to the node's mempool and
     * prompt rebroadcasts (see ResendWalletTransactions()). */
    bool fBroadcastTransactions = false;
    // Local time that the tip block was received. Used to schedule wallet rebroadcasts.
    std::atomic<int64_t> m_best_block_time {0};

    mutable bool fAnonymizableTallyCached = false;
    mutable std::vector<CompactTallyItem> vecAnonymizableTallyCached;
    mutable bool fAnonymizableTallyCachedNonDenom = false;
    mutable std::vector<CompactTallyItem> vecAnonymizableTallyCachedNonDenom;

    /**
     * Used to keep track of spent outpoints, and
     * detect and report conflicts (double-spends or
     * mutated transactions where the mutant gets mined).
     */
    typedef std::unordered_multimap<COutPoint, uint256, SaltedOutpointHasher> TxSpends;
    TxSpends mapTxSpends GUARDED_BY(cs_wallet);
    void AddToSpends(const COutPoint& outpoint, const uint256& wtxid, WalletBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    void AddToSpends(const CWalletTx& wtx, WalletBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    std::set<COutPoint> setWalletUTXO;
    /** Add new UTXOs to the wallet UTXO set
     *
     *  @param[in] tx         Transaction to scan eligible UTXOs from
     *  @param[in] ret_dups   Allow UTXOs already in set to be included in return value
     *  @returns              Set of all new UTXOs (eligible to be) added to set */
    std::set<COutPoint> AddWalletUTXOs(CTransactionRef tx, bool ret_dups) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    mutable std::map<COutPoint, int> mapOutpointRoundsCache GUARDED_BY(cs_wallet);

    /**
     * Add a transaction to the wallet, or update it.  confirm.block_* should
     * be set when the transaction was known to be included in a block.  When
     * block_hash.IsNull(), then wallet state is not updated in AddToWallet, but
     * notifications happen and cached balances are marked dirty.
     *
     * If fUpdate is true, existing transactions will be updated.
     * TODO: One exception to this is that the abandoned state is cleared under the
     * assumption that any further notification of a transaction that was considered
     * abandoned is an indication that it is not safe to be considered abandoned.
     * Abandoned state should probably be more carefully tracked via different
     * chain notifications or by checking mempool presence when necessary.
     *
     * Should be called with rescanning_old_block set to true, if the transaction is
     * not discovered in real time, but during a rescan of old blocks.
     */
    bool AddToWalletIfInvolvingMe(const CTransactionRef& tx, const SyncTxState& state, WalletBatch& batch, bool fUpdate, bool rescanning_old_block) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /** Mark a transaction (and its in-wallet descendants) as conflicting with a particular block. */
    void MarkConflicted(const uint256& hashBlock, int conflicting_height, const uint256& hashTx);

    /** Mark a transaction's inputs dirty, thus forcing the outputs to be recomputed */
    void MarkInputsDirty(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    void SyncMetaData(std::pair<TxSpends::iterator, TxSpends::iterator>) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    void SyncTransaction(const CTransactionRef& tx, const SyncTxState& state, WalletBatch& batch, bool update_tx = true, bool rescanning_old_block = false) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /** WalletFlags set on this wallet. */
    std::atomic<uint64_t> m_wallet_flags{0};

    bool SetAddressBookWithDB(WalletBatch& batch, const CTxDestination& address, const std::string& strName, const std::string& strPurpose);

    //! Unsets a wallet flag and saves it to disk
    void UnsetWalletFlagWithDB(WalletBatch& batch, uint64_t flag);

    //! Unset the blank wallet flag and saves it to disk
    void UnsetBlankWalletFlag(WalletBatch& batch) override;

    // Reset coinjoin and reset key counter
    void NewKeyPoolCallback() override;

    // Decreases amount of nKeysLeftSinceAutoBackup after KeepDestination
    void KeepDestinationCallback(bool erased) override;

    /** Provider of aplication-wide arguments. */
    const ArgsManager& m_args;

    /** Interface for accessing chain state. */
    interfaces::Chain* m_chain;

    /** Interface for accessing CoinJoin state. */
    interfaces::CoinJoin::Loader* m_coinjoin_loader;

    /** Wallet name: relative directory name or "" for default wallet. */
    std::string m_name;

    /** Internal database handle. */
    std::unique_ptr<WalletDatabase> const m_database;

    /**
     * The following is used to keep track of how far behind the wallet is
     * from the chain sync, and to allow clients to block on us being caught up.
     *
     * Processed hash is a pointer on node's tip and doesn't imply that the wallet
     * has scanned sequentially all blocks up to this one.
     */
    uint256 m_last_block_processed GUARDED_BY(cs_wallet);

    /** Pulled from wallet DB ("cj_salt") and used when mixing a random number of rounds.
     *  This salt is needed to prevent an attacker from learning how many extra times
     *  the input was mixed based only on information in the blockchain.
     */
    uint256 nCoinJoinSalt;

    /**
     * Populates nCoinJoinSalt with value from database (and migrates salt stored with legacy key).
     */
    void InitCJSaltFromDb();

    /** Height of last block processed is used by wallet to know depth of transactions
     * without relying on Chain interface beyond asynchronous updates. For safety, we
     * initialize it to -1. Height is a pointer on node's tip and doesn't imply
     * that the wallet has scanned sequentially all blocks up to this one.
     */
    int m_last_block_processed_height GUARDED_BY(cs_wallet) = -1;

    ScriptPubKeyMan* m_external_spk_managers{nullptr};
    ScriptPubKeyMan* m_internal_spk_managers{nullptr};

    // Indexed by a unique identifier produced by each ScriptPubKeyMan using
    // ScriptPubKeyMan::GetID. In many cases it will be the hash of an internal structure
    std::map<uint256, std::unique_ptr<ScriptPubKeyMan>> m_spk_managers;

    /**
     * Catch wallet up to current chain, scanning new blocks, updating the best
     * block locator and m_last_block_processed, and registering for
     * notifications about new blocks and transactions.
     */
    static bool AttachChain(const std::shared_ptr<CWallet>& wallet, interfaces::Chain& chain, bilingual_str& error, std::vector<bilingual_str>& warnings);

public:
    /**
     * Main wallet lock.
     * This lock protects all the fields added by CWallet.
     */
    mutable RecursiveMutex cs_wallet;

    WalletDatabase& GetDatabase() const override
    {
        assert(static_cast<bool>(m_database));
        return *m_database;
    }

    /** Get a name for this wallet for logging/debugging purposes.
     */
    const std::string& GetName() const { return m_name; }

    /**
     * Get an existing CoinJoin salt. Will attempt to read database (and migrate legacy salts) if
     * nCoinJoinSalt is empty but will skip database read if nCoinJoinSalt is populated.
     **/
    const uint256& GetCoinJoinSalt();

    /**
     * Write a new CoinJoin salt. This will directly write the new salt value into the wallet database.
     * Ensuring that undesirable behaviour like overwriting the salt of a wallet that already uses CoinJoin
     * is the responsibility of the caller.
     **/
    bool SetCoinJoinSalt(const uint256& cj_salt);

    // Map from governance object hash to governance object, they are added by gobject_prepare.
    std::map<uint256, Governance::Object> m_gobjects;

    typedef std::map<unsigned int, CMasterKey> MasterKeyMap;
    MasterKeyMap mapMasterKeys;
    unsigned int nMasterKeyMaxID = 0;

    /** Construct wallet with specified name and database implementation. */
    CWallet(interfaces::Chain* chain, interfaces::CoinJoin::Loader* coinjoin_loader, const std::string& name, const ArgsManager& args, std::unique_ptr<WalletDatabase> database)
        : fOnlyMixingAllowed(false),
          m_args(args),
          m_chain(chain),
          m_coinjoin_loader(coinjoin_loader),
          m_name(name),
          m_database(std::move(database))
    {
    }

    ~CWallet()
    {
        // Should not have slots connected at this point.
        assert(NotifyUnload.empty());
    }

    /** Interface to assert chain access */
    bool HaveChain() const { return m_chain ? true : false; }
    bool IsCrypted() const;
    bool IsLocked(bool fForMixing = false) const override;
    bool Lock(bool fForMixing = false);

    void UpdateProgress(const std::string& title, int nProgress) override;

    /* A helper function which loops through wallet UTXOs */
    std::unordered_set<const CWalletTx*, WalletTxHasher> GetSpendableTXs() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /** Map from txid to CWalletTx for all transactions this wallet is
     * interested in, including received and sent transactions. */
    std::unordered_map<uint256, CWalletTx, SaltedTxidHasher> mapWallet GUARDED_BY(cs_wallet);

    typedef std::multimap<int64_t, CWalletTx*> TxItems;
    TxItems wtxOrdered;

    int64_t nOrderPosNext GUARDED_BY(cs_wallet) = 0;
    uint64_t nAccountingEntryNumber = 0;

    std::map<CTxDestination, CAddressBookData> m_address_book GUARDED_BY(cs_wallet);
    const CAddressBookData* FindAddressBookEntry(const CTxDestination&, bool allow_change = false) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /** Set of Coins owned by this wallet that we won't try to spend from. A
     * Coin may be locked if it has already been used to fund a transaction
     * that hasn't confirmed yet. We wouldn't consider the Coin spent already,
     * but also shouldn't try to use it again. */
    std::set<COutPoint> setLockedCoins GUARDED_BY(cs_wallet);

    int64_t nKeysLeftSinceAutoBackup;

    /** Registered interfaces::Chain::Notifications handler. */
    std::unique_ptr<interfaces::Handler> m_chain_notifications_handler;

    /** Interface for accessing chain state. */
    interfaces::Chain& chain() const { assert(m_chain); return *m_chain; }

    /** Interface for accessing CoinJoin state. */
    interfaces::CoinJoin::Loader& coinjoin_loader() { assert(m_coinjoin_loader); return *m_coinjoin_loader; }
    /** Interface for availability status of CoinJoin. */
    bool coinjoin_available() { return m_coinjoin_loader != nullptr; }

    const CWalletTx* GetWalletTx(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    std::set<uint256> GetTxConflicts(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * Return depth of transaction in blockchain:
     * <0  : conflicts with a transaction this deep in the blockchain
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     */
    int GetTxDepthInMainChain(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool IsTxInMainChain(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        return GetTxDepthInMainChain(wtx) > 0;
    }

    bool IsTxLockedByInstantSend(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool IsTxChainLocked(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * @return number of blocks to maturity for this transaction:
     *  0 : is not a coinbase transaction, or is a mature coinbase transaction
     * >0 : is a coinbase transaction which matures in this many blocks
     */
    int GetTxBlocksToMaturity(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool IsTxImmatureCoinBase(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! check whether we support the named feature
    bool CanSupportFeature(enum WalletFeature wf) const override EXCLUSIVE_LOCKS_REQUIRED(cs_wallet) { AssertLockHeld(cs_wallet); return IsFeatureSupported(nWalletVersion, wf); }

    // Coin selection
    bool SelectTxDSInsByDenomination(int nDenom, CAmount nValueMax, std::vector<CTxDSIn>& vecTxDSInRet);
    bool SelectDenominatedAmounts(CAmount nValueMax, std::set<CAmount>& setAmountsRet) const;

    std::vector<CompactTallyItem> SelectCoinsGroupedByAddresses(bool fSkipDenominated = true, bool fAnonymizable = true, bool fSkipUnconfirmed = true, int nMaxOupointsPerAddress = -1) const;

    bool HasCollateralInputs(bool fOnlyConfirmed = true) const;
    int  CountInputsWithAmount(CAmount nInputAmount) const;

    // get the CoinJoin chain depth for a given input
    int GetRealOutpointCoinJoinRounds(const COutPoint& outpoint, int nRounds = 0) const;
    // respect current settings
    int GetCappedOutpointCoinJoinRounds(const COutPoint& outpoint) const;
    // drop the internal cache to let Get...Rounds recalculate CJ balance from scratch and notify UI
    void ClearCoinJoinRoundsCache();

    bool IsDenominated(const COutPoint& outpoint) const;
    bool IsFullyMixed(const COutPoint& outpoint) const;

    bool IsSpent(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    // Whether this or any known UTXO with the same single key has been spent.
    bool IsSpentKey(const CScript& scriptPubKey) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    void SetSpentKeyState(WalletBatch& batch, const uint256& hash, unsigned int n, bool used, std::set<CTxDestination>& tx_destinations) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    void RecalculateMixedCredit(const uint256 hash) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    bool IsLockedCoin(const COutPoint& output) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool LockCoin(const COutPoint& output, WalletBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool UnlockCoin(const COutPoint& output, WalletBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool UnlockAllCoins() EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    std::vector<COutPoint> ListLockedCoins() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    std::vector<COutPoint> ListProTxCoins() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    std::vector<COutPoint> ListProTxCoins(const std::set<COutPoint>& utxos) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    void LockProTxCoins(const std::set<COutPoint>& utxos, WalletBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /*
     * Rescan abort properties
     */
    void AbortRescan() { fAbortRescan = true; }
    bool IsAbortingRescan() const { return fAbortRescan; }
    bool IsScanning() const { return fScanningWallet; }
    int64_t ScanningDuration() const { return fScanningWallet ? GetTimeMillis() - m_scanning_start : 0; }
    double ScanningProgress() const { return fScanningWallet ? (double) m_scanning_progress : 0; }

    //! Upgrade stored CKeyMetadata objects to store key origin info as KeyOriginInfo
    void UpgradeKeyMetadata() EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! Upgrade DescriptorCaches
    void UpgradeDescriptorCache() EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    bool LoadMinVersion(int nVersion) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet) { AssertLockHeld(cs_wallet); nWalletVersion = nVersion; return true; }

    //! Adds a destination data tuple to the store, without saving it to disk
    void LoadDestData(const CTxDestination& dest, const std::string& key, const std::string& value) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! Holds a timestamp at which point the wallet is scheduled (externally) to be relocked. Caller must arrange for actual relocking to occur via Lock().
    int64_t nRelockTime GUARDED_BY(cs_wallet){0};

    // Used to prevent concurrent calls to walletpassphrase RPC.
    Mutex m_unlock_mutex;
    bool Unlock(const SecureString& strWalletPassphrase, bool fForMixingOnly = false, bool accept_no_keys = false);
    bool ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase);
    bool EncryptWallet(const SecureString& strWalletPassphrase);

    void GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    unsigned int ComputeTimeSmart(const CWalletTx& wtx, bool rescanning_old_block) const;

    /**
     * Increment the next transaction order id
     * @return next transaction order id
     */
    int64_t IncOrderPosNext(WalletBatch *batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    DBErrors ReorderTransactions();

    void MarkDirty();

    //! Callback for updating transaction metadata in mapWallet.
    //!
    //! @param wtx - reference to mapWallet transaction to update
    //! @param new_tx - true if wtx is newly inserted, false if it previously existed
    //!
    //! @return true if wtx is changed and needs to be saved to disk, otherwise false
    using UpdateWalletTxFn = std::function<bool(CWalletTx& wtx, bool new_tx)>;

    CWalletTx* AddToWallet(CTransactionRef tx, const TxState& state, const UpdateWalletTxFn& update_wtx=nullptr, bool fFlushOnClose=true, bool rescanning_old_block = false);
    bool LoadToWallet(const uint256& hash, const UpdateWalletTxFn& fill_wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    void transactionAddedToMempool(const CTransactionRef& tx, int64_t nAcceptTime, uint64_t mempool_sequence) override;
    void blockConnected(const CBlock& block, int height) override;
    void blockDisconnected(const CBlock& block, int height) override;
    void updatedBlockTip() override;
    int64_t RescanFromTime(int64_t startTime, const WalletRescanReserver& reserver, bool update);

    struct ScanResult {
        enum { SUCCESS, FAILURE, USER_ABORT } status = SUCCESS;

        //! Hash and height of most recent block that was successfully scanned.
        //! Unset if no blocks were scanned due to read errors or the chain
        //! being empty.
        uint256 last_scanned_block;
        std::optional<int> last_scanned_height;

        //! Height of the most recent block that could not be scanned due to
        //! read errors or pruning. Will be set if status is FAILURE, unset if
        //! status is SUCCESS, and may or may not be set if status is
        //! USER_ABORT.
        uint256 last_failed_block;
    };
    ScanResult ScanForWalletTransactions(const uint256& start_block, int start_height, std::optional<int> max_height, const WalletRescanReserver& reserver, bool fUpdate, const bool save_progress);
    void transactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) override;
    void ReacceptWalletTransactions() EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    void ResendWalletTransactions();

    CAmount GetAnonymizableBalance(bool fSkipDenominated = false, bool fSkipUnconfirmed = true) const;
    float GetAverageAnonymizedRounds() const;
    CAmount GetNormalizedAnonymizedBalance() const;

    /** Fetch the inputs and sign with SIGHASH_ALL. */
    bool SignTransaction(CMutableTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /** Sign the tx given the input coins and sighash. */
    bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors) const;
    SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const;
    /** Sign the payload of special transaction.
      * Because wallet is not aware about special transactions entity,
      * but it should work for any its type, we pass there directly a hash of payload.
      */
    bool SignSpecialTxPayload(const uint256& hash, const CKeyID& keyid, std::vector<unsigned char>& vchSig) const;
    /**
     * Sign a governance vote using wallet signing methods.
     *
     * @param[in] keyID The key ID to use for signing
     * @param[in,out] vote The governance vote to sign (signature is set on success)
     * @return true if signing succeeded, false otherwise
     */
    bool SignGovernanceVote(const CKeyID& keyID, CGovernanceVote& vote) const;

    /**
     * Fills out a PSBT with information from the wallet. Fills in UTXOs if we have
     * them. Tries to sign if sign=true. Sets `complete` if the PSBT is now complete
     * (i.e. has all required signatures or signature-parts, and is ready to
     * finalize.) Sets `error` and returns false if something goes wrong.
     *
     * @param[in]  psbtx PartiallySignedTransaction to fill in
     * @param[out] complete indicates whether the PSBT is now complete
     * @param[in]  sighash_type the sighash type to use when signing (if PSBT does not specify)
     * @param[in]  sign whether to sign or not
     * @param[in]  bip32derivs whether to fill in bip32 derivation information if available
     * @param[out] n_signed the number of inputs signed by this wallet
     * @param[in] finalize whether to create the final scriptSig
     * return error
     */
    [[nodiscard]] TransactionError FillPSBT(PartiallySignedTransaction& psbtx,
                  bool& complete,
                  int sighash_type = 1 /* SIGHASH_ALL */,
                  bool sign = true,
                  bool bip32derivs = true,
                  size_t* n_signed = nullptr,
                  bool finalize = true) const;

    /**
     * Submit the transaction to the node's mempool and then relay to peers.
     * Should be called after CreateTransaction unless you want to abort
     * broadcasting the transaction.
     *
     * @param[in] tx The transaction to be broadcast.
     * @param[in] mapValue key-values to be set on the transaction.
     * @param[in] orderForm BIP 70 / BIP 21 order form details to be set on the transaction.
     */
    void CommitTransaction(CTransactionRef tx, mapValue_t mapValue, std::vector<std::pair<std::string, std::string>> orderForm);

    /** Will SubmitTxMemoryPoolAndRelay() consider wtx if supplied */
    bool CanTxBeResent(const CWalletTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /** Pass this transaction to node for mempool insertion and relay to peers if flag set to true */
    bool SubmitTxMemoryPoolAndRelay(CWalletTx& wtx, bilingual_str& err_string, bool relay) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    bool DummySignTx(CMutableTransaction &txNew, const std::set<CTxOut> &txouts, const CCoinControl* coin_control = nullptr) const
    {
        std::vector<CTxOut> v_txouts(txouts.size());
        std::copy(txouts.begin(), txouts.end(), v_txouts.begin());
        return DummySignTx(txNew, v_txouts, coin_control);
    }
    bool DummySignTx(CMutableTransaction &txNew, const std::vector<CTxOut> &txouts, const CCoinControl* coin_control = nullptr) const;

    bool ImportScripts(const std::set<CScript> scripts, int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool ImportPrivKeys(const std::map<CKeyID, CKey>& privkey_map, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool ImportPubKeys(const std::vector<CKeyID>& ordered_pubkeys, const std::map<CKeyID, CPubKey>& pubkey_map, const std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins, const bool add_keypool, const bool internal, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool ImportScriptPubKeys(const std::string& label, const std::set<CScript>& script_pub_keys, const bool have_solving_data, const bool apply_label, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    CFeeRate m_pay_tx_fee{DEFAULT_PAY_TX_FEE};
    unsigned int m_confirm_target{DEFAULT_TX_CONFIRM_TARGET};
    /** Allow Coin Selection to pick unconfirmed UTXOs that were sent from our own wallet if it
     * cannot fund the transaction otherwise. */
    bool m_spend_zero_conf_change{DEFAULT_SPEND_ZEROCONF_CHANGE};
    bool m_allow_fallback_fee{true}; //!< will be false if -fallbackfee=0
    CFeeRate m_min_fee{DEFAULT_TRANSACTION_MINFEE}; //!< Override with -mintxfee
    /**
     * If fee estimation does not have enough data to provide estimates, use this fee instead.
     * Has no effect if not using fee estimation
     * Override with -fallbackfee
     */
    CFeeRate m_fallback_fee{DEFAULT_FALLBACK_FEE};

     /** If the cost to spend a change output at this feerate is greater than the value of the
      * output itself, just drop it to fees. */
    CFeeRate m_discard_rate{DEFAULT_DISCARD_FEE};

    /** When the actual feerate is less than the consolidate feerate, we will tend to make transactions which
     * consolidate inputs. When the actual feerate is greater than the consolidate feerate, we will tend to make
     * transactions which have the lowest fees.
     */
    CFeeRate m_consolidate_feerate{DEFAULT_CONSOLIDATE_FEERATE};

    /** The maximum fee amount we're willing to pay to prioritize partial spend avoidance. */
    CAmount m_max_aps_fee{DEFAULT_MAX_AVOIDPARTIALSPEND_FEE}; //!< note: this is absolute fee, not fee rate
    /** Absolute maximum transaction fee (in satoshis) used by default for the wallet */
    CAmount m_default_max_tx_fee{DEFAULT_TRANSACTION_MAXFEE};

    size_t KeypoolCountExternalKeys() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool TopUpKeyPool(unsigned int kpSize = 0);

    std::optional<int64_t> GetOldestKeyPoolTime() const;

    // Filter struct for 'ListAddrBookAddresses'
    struct AddrBookFilter {
        // Fetch addresses with the provided label
        std::optional<std::string> m_op_label{std::nullopt};
        // Don't include change addresses by default
        bool ignore_change{true};
    };

    /**
     * Filter and retrieve destinations stored in the addressbook
     */
    std::vector<CTxDestination> ListAddrBookAddresses(const std::optional<AddrBookFilter>& filter) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * Retrieve all the known labels in the address book
     */
    std::set<std::string> ListAddrBookLabels(const std::string& purpose) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * Walk-through the address book entries.
     * Stops when the provided 'ListAddrBookFunc' returns false.
     */
    using ListAddrBookFunc = std::function<void(const CTxDestination& dest, const std::string& label, const std::string& purpose, bool is_change)>;
    void ForEachAddrBookEntry(const ListAddrBookFunc& func) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * Marks all outputs in each one of the destinations dirty, so their cache is
     * reset and does not return outdated information.
     */
    void MarkDestinationsDirty(const std::set<CTxDestination>& destinations) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    bool GetNewDestination(const std::string label, CTxDestination& dest, bilingual_str& error);
    bool GetNewChangeDestination(CTxDestination& dest, bilingual_str& error);

    isminetype IsMine(const CTxDestination& dest) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    isminetype IsMine(const CScript& script) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /**
     * Returns amount of debit if the input matches the
     * filter, otherwise returns 0
     */
    CAmount GetDebit(const CTxIn& txin, const isminefilter& filter) const;
    isminetype IsMine(const CTxOut& txout) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool IsMine(const CTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /** should probably be renamed to IsRelevantToMe */
    bool IsFromMe(const CTransaction& tx) const;
    CAmount GetDebit(const CTransaction& tx, const isminefilter& filter) const;
    void chainStateFlushed(const CBlockLocator& loc) override;

    DBErrors LoadWallet();
    void AutoLockMasternodeCollaterals();
    DBErrors ZapSelectTx(std::vector<uint256>& vHashIn, std::vector<uint256>& vHashOut) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    bool SetAddressBook(const CTxDestination& address, const std::string& strName, const std::string& purpose);

    bool DelAddressBook(const CTxDestination& address);

    bool IsAddressUsed(const CTxDestination& dest) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool SetAddressUsed(WalletBatch& batch, const CTxDestination& dest, bool used) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    std::vector<std::string> GetAddressReceiveRequests() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    bool SetAddressReceiveRequest(WalletBatch& batch, const CTxDestination& dest, const std::string& id, const std::string& value) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    unsigned int GetKeyPoolSize() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! signify that a particular wallet feature is now used.
    void SetMinVersion(enum WalletFeature, WalletBatch* batch_in = nullptr) override;

    //! get the current wallet format (the oldest client version guaranteed to understand this wallet)
    int GetVersion() const { LOCK(cs_wallet); return nWalletVersion; }

    //! Get wallet transactions that conflict with given transaction (spend same outputs)
    std::set<uint256> GetConflicts(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! Check if a given transaction has any of its outputs spent by another transaction in the wallet
    bool HasWalletSpend(const CTransactionRef& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! Flush wallet (bitdb flush)
    void Flush();

    //! Close wallet database
    void Close();

    /** Wallet is about to be unloaded */
    boost::signals2::signal<void ()> NotifyUnload;

    /**
     * Address book entry changed.
     * @note called without lock cs_wallet held.
     */
    boost::signals2::signal<void(const CTxDestination& address,
                                 const std::string& label, bool isMine,
                                 const std::string& purpose, ChangeType status)>
        NotifyAddressBookChanged;

    /**
     * Wallet transaction added, removed or updated.
     * @note called with lock cs_wallet held.
     */
    boost::signals2::signal<void(const uint256& hashTx, ChangeType status)> NotifyTransactionChanged;

    /** Show progress e.g. for rescan */
    boost::signals2::signal<void (const std::string &title, int nProgress)> ShowProgress;

    /** Watch-only address added */
    boost::signals2::signal<void (bool fHaveWatchOnly)> NotifyWatchonlyChanged;

    /** Keypool has new keys */
    boost::signals2::signal<void ()> NotifyCanGetAddressesChanged;

    /** IS-lock received */
    boost::signals2::signal<void ()> NotifyISLockReceived;

    /** ChainLock received */
    boost::signals2::signal<void (int height)> NotifyChainLockReceived;

    /**
     * Wallet status (encrypted, locked) changed.
     * Note: Called without locks held.
     */
    boost::signals2::signal<void (CWallet* wallet)> NotifyStatusChanged;

    /** Inquire whether this wallet broadcasts transactions. */
    bool GetBroadcastTransactions() const { return fBroadcastTransactions; }
    /** Set whether this wallet broadcasts transactions. */
    void SetBroadcastTransactions(bool broadcast) { fBroadcastTransactions = broadcast; }

    /** Return whether transaction can be abandoned */
    bool TransactionCanBeAbandoned(const uint256& hashTx) const;

    /** Return whether transaction can be resent */
    bool TransactionCanBeResent(const uint256& hashTx) const;

    /* Mark a transaction (and it in-wallet descendants) as abandoned so its inputs may be respent. */
    bool AbandonTransaction(const uint256& hashTx);

    /* Resend a transaction */
    bool ResendTransaction(const uint256& hashTx);

    /* Initializes the wallet, returns a new CWallet instance or a null pointer in case of an error */
    static std::shared_ptr<CWallet> Create(WalletContext& context, const std::string& name, std::unique_ptr<WalletDatabase> database, uint64_t wallet_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings);

    /**
     * Wallet post-init setup
     * Gives the wallet a chance to register repetitive tasks and complete post-init tasks
     */
    void postInitProcess();

    /* AutoBackup functionality */
    static void InitAutoBackup();
    bool AutoBackupWallet(const fs::path& wallet_path, bilingual_str& error_string, std::vector<bilingual_str>& warnings);

    bool BackupWallet(const std::string& strDest) const;

    /**
     * HD Wallet Functions
     */

    /* Returns true if HD is enabled */
    bool IsHDEnabled() const;

    // TODO: move it to scriptpubkeyman
    /* Generates a new HD chain */
    bool GenerateNewHDChain(const SecureString& secureMnemonic, const SecureString& secureMnemonicPassphrase, const SecureString& secureWalletPassphrase = "");

    /* Returns true if the wallet can give out new addresses. This means it has keys in the keypool or can generate new keys */
    bool CanGetAddresses(bool internal = false) const;

    void notifyTransactionLock(const CTransactionRef &tx, const std::shared_ptr<const instantsend::InstantSendLock>& islock) override;
    void notifyChainLock(const CBlockIndex* pindexChainLock, const std::shared_ptr<const llmq::CChainLockSig>& clsig) override;

    /** Load a CGovernanceObject into m_gobjects. */
    bool LoadGovernanceObject(const Governance::Object& obj) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /** Store a CGovernanceObject in the wallet database. This should only be used by governance objects that are created by this wallet via `gobject prepare`. */
    bool WriteGovernanceObject(const Governance::Object& obj) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
    /** Returns a vector containing pointers to the governance objects in m_gobjects */
    std::vector<const Governance::Object*> GetGovernanceObjects() EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    /**
     * Blocks until the wallet state is up-to-date to /at least/ the current
     * chain at the time this function is entered
     * Obviously holding cs_main/cs_wallet when going into this call may cause
     * deadlock
     */
    void BlockUntilSyncedToCurrentChain() const LOCKS_EXCLUDED(::cs_main) EXCLUSIVE_LOCKS_REQUIRED(!cs_wallet);

    /** set a single wallet flag */
    void SetWalletFlag(uint64_t flags);

    /** Unsets a single wallet flag */
    void UnsetWalletFlag(uint64_t flag);

    /** check if a certain wallet flag is set */
    bool IsWalletFlagSet(uint64_t flag) const override;

    /** overwrite all flags by the given uint64_t
       flags must be uninitialised (or 0)
       only known flags may be present */
    void InitWalletFlags(uint64_t flags);
    /** Loads the flags into the wallet. (used by LoadWallet) */
    bool LoadWalletFlags(uint64_t flags);

    /** Determine if we are a legacy wallet */
    bool IsLegacy() const;

    /** Returns a bracketed wallet name for displaying in logs, will return [default wallet] if the wallet has no name */
    const std::string GetDisplayName() const override {
        std::string wallet_name = GetName().length() == 0 ? "default wallet" : GetName();
        return strprintf("[%s]", wallet_name);
    };

    /** Prepends the wallet name in logging output to ease debugging in multi-wallet use cases */
    template<typename... Params>
    void WalletLogPrintf(std::string fmt, Params... parameters) const {
        LogPrintf(("%s " + fmt).c_str(), GetDisplayName(), parameters...);
    };

    /** Upgrade the wallet */
    bool UpgradeWallet(int version, bilingual_str& error);

    /** Upgrade non-HD wallet to HD wallet */
    bool UpgradeToHD(const SecureString& secureMnemonic, const SecureString& secureMnemonicPassphrase, const SecureString& secureWalletPassphrase, bilingual_str& error);

    //! Returns all unique ScriptPubKeyMans in m_internal_spk_managers and m_external_spk_managers
    std::set<ScriptPubKeyMan*> GetActiveScriptPubKeyMans() const;

    //! Returns all unique ScriptPubKeyMans
    std::set<ScriptPubKeyMan*> GetAllScriptPubKeyMans() const;

    //! Get the ScriptPubKeyMan for internal/external chain.
    ScriptPubKeyMan* GetScriptPubKeyMan(bool internal) const;

    //! Get the ScriptPubKeyMan for a script
    ScriptPubKeyMan* GetScriptPubKeyMan(const CScript& script) const;
    //! Get the ScriptPubKeyMan by id
    ScriptPubKeyMan* GetScriptPubKeyMan(const uint256& id) const;

    //! Get all of the ScriptPubKeyMans for a script given additional information in sigdata (populated by e.g. a psbt)
    std::set<ScriptPubKeyMan*> GetScriptPubKeyMans(const CScript& script, SignatureData& sigdata) const;

    //! Get the SigningProvider for a script
    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const;
    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script, SignatureData& sigdata) const;

    //! Get the LegacyScriptPubKeyMan which is used for all types, internal, and external.
    LegacyScriptPubKeyMan* GetLegacyScriptPubKeyMan() const;
    LegacyScriptPubKeyMan* GetOrCreateLegacyScriptPubKeyMan();

    //! Make a LegacyScriptPubKeyMan and set it for all types, internal, and external.
    void SetupLegacyScriptPubKeyMan();

    bool WithEncryptionKey(std::function<bool (const CKeyingMaterial&)> cb) const override;

    bool HasEncryptionKeys() const override;

    /** Get last block processed height */
    int GetLastBlockHeight() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        assert(m_last_block_processed_height >= 0);
        return m_last_block_processed_height;
    };
    uint256 GetLastBlockHash() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        assert(m_last_block_processed_height >= 0);
        return m_last_block_processed;
    }
    /** Set last block processed height, currently only use in unit test */
    void SetLastBlockProcessed(int block_height, uint256 block_hash) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        m_last_block_processed_height = block_height;
        m_last_block_processed = block_hash;
    };

    //! Connect the signals from ScriptPubKeyMans to the signals in CWallet
    void ConnectScriptPubKeyManNotifiers();

    //! Instantiate a descriptor ScriptPubKeyMan from the WalletDescriptor and load it
    void LoadDescriptorScriptPubKeyMan(uint256 id, WalletDescriptor& desc);

    //! Adds the active ScriptPubKeyMan for the specified type and internal. Writes it to the wallet file
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void AddActiveScriptPubKeyMan(uint256 id, bool internal);

    //! Loads an active ScriptPubKeyMan for the specified type and internal. (used by LoadWallet)
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void LoadActiveScriptPubKeyMan(uint256 id, bool internal);

    //! Remove specified ScriptPubKeyMan from set of active SPK managers. Writes the change to the wallet file.
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void DeactivateScriptPubKeyMan(uint256 id, bool internal);

    //! Create new DescriptorScriptPubKeyMans and add them to the wallet
    void SetupDescriptorScriptPubKeyMans(const SecureString& mnemonic, const SecureString mnemonic_passphrase) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);

    //! Return the DescriptorScriptPubKeyMan for a WalletDescriptor if it is already in the wallet
    DescriptorScriptPubKeyMan* GetDescriptorScriptPubKeyMan(const WalletDescriptor& desc) const;

    //! Add a descriptor to the wallet, return a ScriptPubKeyMan & associated output type
    ScriptPubKeyMan* AddWalletDescriptor(WalletDescriptor& desc, const FlatSigningProvider& signing_provider, const std::string& label, bool internal) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet);
};

/**
 * Called periodically by the schedule thread. Prompts individual wallets to resend
 * their transactions. Actual rebroadcast schedule is managed by the wallets themselves.
 */
void MaybeResendWalletTxs(WalletContext& context);

/** RAII object to check and reserve a wallet rescan */
class WalletRescanReserver
{
private:
    using Clock = std::chrono::steady_clock;
    using NowFn = std::function<Clock::time_point()>;
    CWallet& m_wallet;
    bool m_could_reserve;
    NowFn m_now;
public:
    explicit WalletRescanReserver(CWallet& w) : m_wallet(w), m_could_reserve(false) {}

    bool reserve()
    {
        assert(!m_could_reserve);
        if (m_wallet.fScanningWallet.exchange(true)) {
            return false;
        }
        m_wallet.m_scanning_start = GetTimeMillis();
        m_wallet.m_scanning_progress = 0;
        m_wallet.fAbortRescan = false;
        m_could_reserve = true;
        return true;
    }

    bool isReserved() const
    {
        return (m_could_reserve && m_wallet.fScanningWallet);
    }

    Clock::time_point now() const { return m_now ? m_now() : Clock::now(); };

    void setNow(NowFn now) { m_now = std::move(now); }

    ~WalletRescanReserver()
    {
        if (m_could_reserve) {
            m_wallet.fScanningWallet = false;
        }
    }

};

//! Add wallet name to persistent configuration so it will be loaded on startup.
bool AddWalletSetting(interfaces::Chain& chain, const std::string& wallet_name);

//! Remove wallet name from persistent configuration so it will not be loaded on startup.
bool RemoveWalletSetting(interfaces::Chain& chain, const std::string& wallet_name);

bool DummySignInput(const SigningProvider& provider, CTxIn &tx_in, const CTxOut &txout, const CCoinControl* coin_control = nullptr);
} // namespace wallet

#endif // BITCOIN_WALLET_WALLET_H
