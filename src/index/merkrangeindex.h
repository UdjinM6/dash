// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_MERKRANGEINDEX_H
#define BITCOIN_INDEX_MERKRANGEINDEX_H

#include <blockfilter.h>
#include <index/base.h>
#include <sync.h>
#include <uint256.h>

#include <merk.h>
#include <rocksdb_wrapper.h>

#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

static const char* const DEFAULT_MERKRANGEINDEX = "0";

class MerkRangeIndex final : public BaseIndex
{
private:
    static constexpr int CURRENT_VERSION = 1;

    BlockFilterType m_filter_type;
    std::string m_name;
    fs::path m_path;
    size_t m_cache_size;
    bool m_memory;

    std::unique_ptr<BaseIndex::DB> m_db;
    std::unique_ptr<grovedb::RocksDbWrapper> m_rocksdb;
    grovedb::MerkTree m_merk;
    uint256 m_merk_snapshot_tip_hash;
    int m_merk_snapshot_tip_height{-1};

    mutable std::shared_mutex m_merk_mutex;

    static const std::vector<std::vector<uint8_t>> MERK_PATH;

    static std::vector<uint8_t> HeightToKey(int height);

    bool ResetAndReinitStorage();
    bool EnsureMerkLoaded();
    bool SaveMerkTree();
    void SetMerkSnapshotUnlocked(const CBlockIndex* block);

protected:
    bool Init() override;
    bool CommitInternal(CDBBatch& batch) override;
    bool WriteBlock(const CBlock& block, const CBlockIndex* pindex) override;
    bool Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip) override;

    BaseIndex::DB& GetDB() const override { return *m_db; }
    const char* GetName() const override { return m_name.c_str(); }
    bool AllowPrune() const override { return true; }

public:
    explicit MerkRangeIndex(BlockFilterType filter_type,
                            size_t n_cache_size, bool f_memory = false, bool f_wipe = false);
    ~MerkRangeIndex() override;

    BlockFilterType GetFilterType() const { return m_filter_type; }

    bool GetMerkRootHash(std::vector<uint8_t>& root_hash_out) const;
    bool GetMerkRootHashForBlock(const uint256& block_hash, std::vector<uint8_t>& root_hash_out) const;
    bool ProduceRangeProof(int start_height,
                           int end_height,
                           std::vector<uint8_t>& proof_out,
                           std::vector<uint8_t>& root_hash_out,
                           uint256* tip_hash_out = nullptr) const;
    bool GetFilterSizeForHeight(int height, size_t& filter_size_out) const;
};

MerkRangeIndex* GetMerkRangeIndex(BlockFilterType filter_type);
void ForEachMerkRangeIndex(std::function<void(MerkRangeIndex&)> fn);
bool InitMerkRangeIndex(BlockFilterType filter_type,
                        size_t n_cache_size, bool f_memory = false, bool f_wipe = false);
bool DestroyMerkRangeIndex(BlockFilterType filter_type);
void DestroyAllMerkRangeIndexes();

#endif // BITCOIN_INDEX_MERKRANGEINDEX_H
