// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <spentindex.h>
#include <timestampindex.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CBlockFileInfo;
class CBlockIndex;
class uint256;
namespace Consensus {
struct Params;
};
struct bilingual_str;

//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 300;
//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 16 << 20;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache (MiB)
static const int64_t nMinDbCache = 4;
//! Max memory allocated to block tree DB specific cache, if no -txindex (MiB)
static const int64_t nMaxBlockDBCache = 2;
//! Max memory allocated to block tree DB specific cache, if -txindex (MiB)
// Unlike for the UTXO database, for the txindex scenario the leveldb cache make
// a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
static const int64_t nMaxTxIndexCache = 1024;
//! Max memory allocated to all block filter index caches combined in MiB.
static const int64_t max_filter_index_cache = 1024;
//! Max memory allocated to coin DB specific cache (MiB)
static const int64_t nMaxCoinsDBCache = 8;

// Actually declared in validation.cpp; can't include because of circular dependency.
extern RecursiveMutex cs_main;

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    std::unique_ptr<CDBWrapper> m_db;
    fs::path m_ldb_path;
    bool m_is_memory;
public:
    /**
     * @param[in] ldb_path    Location in the filesystem where leveldb data will be stored.
     */
    explicit CCoinsViewDB(fs::path ldb_path, size_t nCacheSize, bool fMemory, bool fWipe);


    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, bool erase = true) override;
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();
    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    explicit CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &info);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindexing);
    void ReadReindexing(bool &fReindexing);

    bool ReadSpentIndex(const CSpentIndexKey key, CSpentIndexValue& value);
    bool UpdateSpentIndex(const std::vector<CSpentIndexEntry>& vect);

    bool ReadAddressUnspentIndex(const uint160& addressHash, const AddressType type,
                                 std::vector<CAddressUnspentIndexEntry>& vect);
    bool UpdateAddressUnspentIndex(const std::vector<CAddressUnspentIndexEntry>& vect);

    bool WriteAddressIndex(const std::vector<CAddressIndexEntry>& vect);
    bool EraseAddressIndex(const std::vector<CAddressIndexEntry>& vect);
    bool ReadAddressIndex(const uint160& addressHash, const AddressType type,
                          std::vector<CAddressIndexEntry>& addressIndex,
                          const int32_t start = 0, const int32_t end = 0);

    bool WriteTimestampIndex(const CTimestampIndexKey& timestampIndex);
    bool EraseTimestampIndex(const CTimestampIndexKey& timestampIndex);
    bool ReadTimestampIndex(const uint32_t high, const uint32_t low, std::vector<uint256>& hashes);

    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

std::optional<bilingual_str> CheckLegacyTxindex(CBlockTreeDB& block_tree_db);

#endif // BITCOIN_TXDB_H
