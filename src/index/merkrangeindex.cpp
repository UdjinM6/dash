// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/merkrangeindex.h>

#include <dbwrapper.h>
#include <fs.h>
#include <hash.h>
#include <node/blockstorage.h>
#include <serialize.h>
#include <util/check.h>
#include <util/system.h>
#include <validation.h>

#include <merk_storage.h>

#include <map>
#include <utility>

using node::UndoReadFromDisk;

namespace {

constexpr uint8_t DB_MERK_ROOT_KEY{'R'};
constexpr uint8_t DB_MERK_ROOT_BY_BLOCK_KEY{'r'};
constexpr uint8_t DB_MERK_COMMIT_IN_PROGRESS{'P'};
constexpr uint8_t DB_VERSION{'V'};
std::map<BlockFilterType, std::unique_ptr<MerkRangeIndex>> g_merk_range_indexes;

bool EnsureDir(const fs::path& path, const char* what)
{
    try {
        fs::create_directories(path);
    } catch (const fs::filesystem_error& e) {
        return error("%s: failed to create %s directory %s: %s", __func__, what, fs::PathToString(path), e.what());
    }
    return true;
}

} // namespace

const std::vector<std::vector<uint8_t>> MerkRangeIndex::MERK_PATH = {
    std::vector<uint8_t>{'m', 'e', 'r', 'k', 'r', 'a', 'n', 'g', 'e'},
};

MerkRangeIndex::MerkRangeIndex(BlockFilterType filter_type,
                               size_t n_cache_size, bool f_memory, bool f_wipe)
    : m_filter_type(filter_type),
      m_cache_size(n_cache_size),
      m_memory(f_memory)
{
    const std::string& filter_name = BlockFilterTypeName(filter_type);
    if (filter_name.empty()) throw std::invalid_argument("unknown filter_type");

    m_name = filter_name + " merk range index";
    m_path = gArgs.GetDataDirNet() / "indexes" / "merkrange" / fs::u8path(filter_name);

    if (!EnsureDir(m_path / "db", "merkrange leveldb")) {
        throw std::runtime_error("failed to create merkrange leveldb directory");
    }
    if (!EnsureDir(m_path / "merk", "merkrange rocksdb")) {
        throw std::runtime_error("failed to create merkrange rocksdb directory");
    }

    m_db = std::make_unique<BaseIndex::DB>(m_path / "db", n_cache_size, f_memory, f_wipe);

    int version = 0;
    if (!m_db->Read(DB_VERSION, version) || version < CURRENT_VERSION) {
        LogPrintf("%s: Outdated or missing version for merkrange index, starting from scratch\n", __func__);
        if (!ResetAndReinitStorage()) {
            throw std::runtime_error("failed to reset merkrange index storage");
        }
        m_db->Write(DB_VERSION, CURRENT_VERSION);
    }

    m_rocksdb = std::make_unique<grovedb::RocksDbWrapper>();
    std::string error;
    if (!m_rocksdb->Open(fs::PathToString(m_path / "merk"), &error)) {
        throw std::runtime_error(strprintf("failed to open merkrange rocksdb: %s", error));
    }
}

MerkRangeIndex::~MerkRangeIndex() = default;

void MerkRangeIndex::SetMerkSnapshotUnlocked(const CBlockIndex* block)
{
    if (!block) {
        m_merk_snapshot_tip_hash.SetNull();
        m_merk_snapshot_tip_height = -1;
        return;
    }
    m_merk_snapshot_tip_hash = block->GetBlockHash();
    m_merk_snapshot_tip_height = block->nHeight;
}

std::vector<uint8_t> MerkRangeIndex::HeightToKey(int height)
{
    std::vector<uint8_t> out(4);
    out[0] = static_cast<uint8_t>((height >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((height >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((height >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(height & 0xFF);
    return out;
}

bool MerkRangeIndex::ResetAndReinitStorage()
{
    m_db.reset();
    m_rocksdb.reset();

    std::error_code ec;
    fs::remove_all(m_path / "db", ec);
    if (ec) {
        return error("%s: failed to wipe merkrange leveldb directory: %s", __func__, ec.message());
    }
    fs::remove_all(m_path / "merk", ec);
    if (ec) {
        return error("%s: failed to wipe merkrange rocksdb directory: %s", __func__, ec.message());
    }

    if (!EnsureDir(m_path / "db", "merkrange leveldb")) {
        return false;
    }
    if (!EnsureDir(m_path / "merk", "merkrange rocksdb")) {
        return false;
    }

    m_db = std::make_unique<BaseIndex::DB>(m_path / "db", m_cache_size, m_memory, /*f_wipe=*/true);
    m_rocksdb = std::make_unique<grovedb::RocksDbWrapper>();

    std::string open_error;
    if (!m_rocksdb->Open(fs::PathToString(m_path / "merk"), &open_error)) {
        return error("%s: failed to reopen merkrange rocksdb: %s", __func__, open_error);
    }

    m_db->Write(DB_VERSION, CURRENT_VERSION);
    {
        std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
        m_merk = grovedb::MerkTree();
        SetMerkSnapshotUnlocked(nullptr);
    }

    return true;
}

bool MerkRangeIndex::EnsureMerkLoaded()
{
    if (m_db->Exists(DB_MERK_COMMIT_IN_PROGRESS)) {
        LogPrintf("%s: detected incomplete merkrange commit; wiping and rebuilding %s\n", __func__, m_name);
        return ResetAndReinitStorage();
    }

    std::vector<uint8_t> root_key;
    const bool has_root = m_db->Read(DB_MERK_ROOT_KEY, root_key) && !root_key.empty();

    CBlockLocator locator;
    const bool has_locator = m_db->ReadBestBlock(locator) && !locator.IsNull();

    if (!has_root && has_locator) {
        LogPrintf("%s: locator exists without merk root; wiping and rebuilding %s\n", __func__, m_name);
        return ResetAndReinitStorage();
    }

    if (!has_root) {
        return true;
    }

    std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
    std::string error_msg;
    if (!m_merk.LoadEncodedTree(m_rocksdb.get(), MERK_PATH, root_key,
                                grovedb::ColumnFamilyKind::kDefault, /*lazy=*/true, &error_msg)) {
        return error("%s: failed to load merk tree for %s: %s", __func__, m_name, error_msg);
    }

    return true;
}

bool MerkRangeIndex::SaveMerkTree()
{
    std::string error_msg;
    if (!grovedb::MerkStorage::SaveTree(m_rocksdb.get(), MERK_PATH, &m_merk, &error_msg)) {
        return error("%s: failed to save merk tree for %s: %s", __func__, m_name, error_msg);
    }
    return true;
}

bool MerkRangeIndex::Init()
{
    if (!EnsureMerkLoaded()) {
        return false;
    }
    if (!BaseIndex::Init()) {
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
        SetMerkSnapshotUnlocked(CurrentIndex());
    }
    return true;
}

bool MerkRangeIndex::CommitInternal(CDBBatch& batch)
{
    std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
    if (!m_db->Write(DB_MERK_COMMIT_IN_PROGRESS, uint8_t{1}, /*fSync=*/true)) {
        return error("%s: failed to set merkrange commit sentinel for %s", __func__, m_name);
    }

    if (!SaveMerkTree()) {
        return false;
    }

    std::vector<uint8_t> root_key;
    if (m_merk.RootKey(&root_key) && !root_key.empty()) {
        batch.Write(DB_MERK_ROOT_KEY, root_key);
    } else {
        batch.Erase(DB_MERK_ROOT_KEY);
    }
    batch.Erase(DB_MERK_COMMIT_IN_PROGRESS);

    return BaseIndex::CommitInternal(batch);
}

bool MerkRangeIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
{
    CBlockUndo block_undo;
    if (pindex->nHeight > 0 && !UndoReadFromDisk(block_undo, pindex)) {
        return false;
    }

    BlockFilter filter(m_filter_type, block, block_undo);

    std::vector<uint8_t> root_hash;
    {
        std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
        std::string err;
        if (!m_merk.Insert(HeightToKey(pindex->nHeight),
                           std::vector<uint8_t>(filter.GetEncodedFilter().begin(), filter.GetEncodedFilter().end()),
                           &err)) {
            return error("%s: failed to insert filter at height %d: %s", __func__, pindex->nHeight, err);
        }
        SetMerkSnapshotUnlocked(pindex);

        if (!m_merk.GetCachedRootHash(&root_hash, &err) &&
            !m_merk.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &root_hash, &err)) {
            return error("%s: failed to compute merk root hash at height %d: %s", __func__, pindex->nHeight, err);
        }
    }

    if (!m_db->Write(std::make_pair(DB_MERK_ROOT_BY_BLOCK_KEY, pindex->GetBlockHash()), root_hash)) {
        return error("%s: failed to persist merk root hash at height %d", __func__, pindex->nHeight);
    }

    return true;
}

bool MerkRangeIndex::Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip)
{
    assert(current_tip->GetAncestor(new_tip->nHeight) == new_tip);

    {
        std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
        std::string err;
        for (int h = current_tip->nHeight; h > new_tip->nHeight; --h) {
            bool deleted{false};
            if (!m_merk.Delete(HeightToKey(h), &deleted, &err)) {
                return error("%s: failed to delete height %d during reorg: %s", __func__, h, err);
            }
        }
        SetMerkSnapshotUnlocked(new_tip);

        if (!SaveMerkTree()) {
            return false;
        }
    }

    CDBBatch batch(*m_db);
    for (int h = current_tip->nHeight; h > new_tip->nHeight; --h) {
        const CBlockIndex* index = current_tip->GetAncestor(h);
        if (!index || index->nHeight != h) {
            return error("%s: failed to resolve block hash while rewinding at height %d", __func__, h);
        }
        batch.Erase(std::make_pair(DB_MERK_ROOT_BY_BLOCK_KEY, index->GetBlockHash()));
    }
    if (!m_db->WriteBatch(batch)) {
        return error("%s: failed to erase rewound merk root hashes", __func__);
    }

    return BaseIndex::Rewind(current_tip, new_tip);
}

bool MerkRangeIndex::GetMerkRootHash(std::vector<uint8_t>& root_hash_out) const
{
    std::string err;
    {
        std::shared_lock<std::shared_mutex> lock(m_merk_mutex);
        if (m_merk.GetCachedRootHash(&root_hash_out, &err)) {
            return true;
        }
    }

    std::unique_lock<std::shared_mutex> lock(m_merk_mutex);
    if (m_merk.GetCachedRootHash(&root_hash_out, &err)) {
        return true;
    }
    return m_merk.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &root_hash_out, &err);
}

bool MerkRangeIndex::GetMerkRootHashForBlock(const uint256& block_hash, std::vector<uint8_t>& root_hash_out) const
{
    const uint256 active_tip_hash = WITH_LOCK(cs_main, return m_chainstate->m_chain.Tip() ? m_chainstate->m_chain.Tip()->GetBlockHash() : uint256::ZERO;);
    if (active_tip_hash == block_hash) {
        return GetMerkRootHash(root_hash_out);
    }

    return m_db->Read(std::make_pair(DB_MERK_ROOT_BY_BLOCK_KEY, block_hash), root_hash_out);
}

bool MerkRangeIndex::ProduceRangeProof(int start_height,
                                       int end_height,
                                       std::vector<uint8_t>& proof_out,
                                       std::vector<uint8_t>& root_hash_out,
                                       uint256* tip_hash_out) const
{
    if (start_height < 0 || start_height > end_height) {
        return false;
    }

    const std::vector<uint8_t> start_key = HeightToKey(start_height);
    const std::vector<uint8_t> end_key = HeightToKey(end_height);
    {
        std::shared_lock<std::shared_mutex> lock(m_merk_mutex);
        std::string err;
        if (!m_merk.GenerateRangeProof(start_key,
                                       end_key,
                                       /*start_inclusive=*/true,
                                       /*end_inclusive=*/true,
                                       grovedb::MerkTree::ValueHashFn(),
                                       &proof_out,
                                       &root_hash_out,
                                       &err)) {
            return error("%s: failed to generate range proof [%d, %d]: %s", __func__, start_height, end_height, err);
        }
        if (tip_hash_out) {
            *tip_hash_out = m_merk_snapshot_tip_hash;
        }
    }

    return true;
}

bool MerkRangeIndex::GetFilterSizeForHeight(int height, size_t& filter_size_out) const
{
    if (height < 0) {
        return false;
    }

    std::vector<uint8_t> value;
    {
        std::shared_lock<std::shared_mutex> lock(m_merk_mutex);
        if (!m_merk.Get(HeightToKey(height), &value)) {
            return false;
        }
    }
    filter_size_out = value.size();
    return true;
}

MerkRangeIndex* GetMerkRangeIndex(BlockFilterType filter_type)
{
    auto it = g_merk_range_indexes.find(filter_type);
    return it != g_merk_range_indexes.end() ? it->second.get() : nullptr;
}

void ForEachMerkRangeIndex(std::function<void(MerkRangeIndex&)> fn)
{
    for (auto& entry : g_merk_range_indexes) {
        fn(*entry.second);
    }
}

bool InitMerkRangeIndex(BlockFilterType filter_type,
                        size_t n_cache_size, bool f_memory, bool f_wipe)
{
    auto [it, inserted] = g_merk_range_indexes.emplace(filter_type, nullptr);
    if (!inserted) {
        return false;
    }

    try {
        it->second = std::make_unique<MerkRangeIndex>(filter_type, n_cache_size, f_memory, f_wipe);
        return true;
    } catch (const std::exception& e) {
        LogPrintf("%s: failed to initialize merk range index for %s: %s\n", __func__, BlockFilterTypeName(filter_type), e.what());
        g_merk_range_indexes.erase(it);
        return false;
    }
}

bool DestroyMerkRangeIndex(BlockFilterType filter_type)
{
    return g_merk_range_indexes.erase(filter_type) == 1;
}

void DestroyAllMerkRangeIndexes()
{
    g_merk_range_indexes.clear();
}
