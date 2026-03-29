#include "rocksdb_wrapper.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "blake3.h"
#include "cost_utils.h"

#if defined(HAVE_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/write_batch.h>
#endif

namespace grovedb {

namespace {

constexpr size_t kBlakeBlockLen = 64;

static_assert(sizeof(size_t) == 8, "prefix hashing requires 64-bit size_t");
static_assert(std::endian::native == std::endian::little,
              "prefix hashing assumes little-endian byte order");

size_t BlakeBlockCount(size_t len) {
  if (len == 0) {
    return 1;
  }
  return 1 + (len - 1) / kBlakeBlockLen;
}

using grovedb::PaidLen;

bool BuildPrefixBody(const std::vector<std::vector<uint8_t>>& path,
                     size_t* out_segments,
                     std::vector<uint8_t>* out_body,
                     std::string* error) {
  if (out_body == nullptr) {
    if (error) {
      *error = "prefix body output is null";
    }
    return false;
  }
  for (const auto& segment : path) {
    if (segment.size() > 255) {
      if (error) {
        *error = "path segment exceeds 255 bytes";
      }
      return false;
    }
  }
  std::vector<uint8_t> body;
  std::vector<uint8_t> lengths;
  size_t count = 0;
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    const auto& segment = *it;
    body.insert(body.end(), segment.begin(), segment.end());
    lengths.push_back(static_cast<uint8_t>(segment.size()));
    count += 1;
  }
  if (out_segments) {
    *out_segments = count;
  }
  // Rust compatibility: the prefix hash format appends native little-endian
  // 64-bit `size_t` bytes for the segment count.
  const uint8_t* count_bytes = reinterpret_cast<const uint8_t*>(&count);
  body.insert(body.end(), count_bytes, count_bytes + sizeof(size_t));
  body.insert(body.end(), lengths.begin(), lengths.end());
  *out_body = std::move(body);
  return true;
}

bool Blake3Hash(const std::vector<uint8_t>& input,
                std::array<uint8_t, 32>* out,
                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  if (!input.empty()) {
    blake3_hasher_update(&hasher, input.data(), input.size());
  }
  blake3_hasher_finalize(&hasher, out->data(), out->size());
  return true;
}

bool BuildPrefixWithCost(const std::vector<std::vector<uint8_t>>& path,
                         std::array<uint8_t, 32>* out,
                         uint64_t* hash_calls,
                         std::string* error) {
  size_t segments = 0;
  std::vector<uint8_t> body;
  if (!BuildPrefixBody(path, &segments, &body, error)) {
    return false;
  }
  if (segments == 0) {
    if (out == nullptr) {
      if (error) {
        *error = "output is null";
      }
      return false;
    }
    out->fill(0);
    if (hash_calls) {
      *hash_calls = 0;
    }
    return true;
  }
  if (hash_calls) {
    *hash_calls = BlakeBlockCount(body.size());
  }
  return Blake3Hash(body, out, error);
}

#if defined(HAVE_ROCKSDB)
bool DebugMissingRootsGetEnabled() {
  const char* env = std::getenv("GROVEDB_DEBUG_MISSING_ROOTS_GET");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

std::string HexBytes(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}
#endif  // HAVE_ROCKSDB

}  // namespace

struct RocksDbWrapper::Impl {
#if defined(HAVE_ROCKSDB)
  std::unique_ptr<rocksdb::OptimisticTransactionDB> db;
  rocksdb::ColumnFamilyHandle* cf_default = nullptr;
  rocksdb::ColumnFamilyHandle* cf_aux = nullptr;
  rocksdb::ColumnFamilyHandle* cf_roots = nullptr;
  rocksdb::ColumnFamilyHandle* cf_meta = nullptr;
#endif
};

struct RocksDbWrapper::PrefixedIterator::Impl {
#if defined(HAVE_ROCKSDB)
  rocksdb::OptimisticTransactionDB* db = nullptr;
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  std::unique_ptr<rocksdb::Iterator> it;
  std::string prefix;
#endif
};

struct RocksDbWrapper::Transaction::Impl {
#if defined(HAVE_ROCKSDB)
  rocksdb::OptimisticTransactionDB* db = nullptr;
  rocksdb::Transaction* tx = nullptr;
  rocksdb::ColumnFamilyHandle* cf_aux = nullptr;
  rocksdb::ColumnFamilyHandle* cf_roots = nullptr;
  rocksdb::ColumnFamilyHandle* cf_meta = nullptr;
  bool committed = false;
#endif
};

RocksDbWrapper::RocksDbWrapper() : impl_(new Impl()) {}

RocksDbWrapper::~RocksDbWrapper() {
#if defined(HAVE_ROCKSDB)
  if (impl_ && impl_->db) {
    if (impl_->cf_default) {
      impl_->db->DestroyColumnFamilyHandle(impl_->cf_default);
    }
    if (impl_->cf_aux) {
      impl_->db->DestroyColumnFamilyHandle(impl_->cf_aux);
    }
    if (impl_->cf_roots) {
      impl_->db->DestroyColumnFamilyHandle(impl_->cf_roots);
    }
    if (impl_->cf_meta) {
      impl_->db->DestroyColumnFamilyHandle(impl_->cf_meta);
    }
  }
#endif
  delete impl_;
}

RocksDbWrapper::Transaction::Transaction() : impl_(new Impl()) {}

RocksDbWrapper::Transaction::~Transaction() {
#if defined(HAVE_ROCKSDB)
  if (impl_ && impl_->tx != nullptr) {
    if (!impl_->committed) {
      impl_->tx->Rollback();
    }
    delete impl_->tx;
  }
#endif
  delete impl_;
}

bool RocksDbWrapper::Transaction::IsPoisoned() const {
  return poisoned_;
}

void RocksDbWrapper::Transaction::Poison() {
  poisoned_ = true;
}

void RocksDbWrapper::Transaction::ClearPoison() {
  poisoned_ = false;
}

bool RocksDbWrapper::BeginTransaction(Transaction* out, std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (out == nullptr) {
    if (error) {
      *error = "transaction output is null";
    }
    return false;
  }
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (out->impl_ == nullptr) {
    out->impl_ = new Transaction::Impl();
  }
  rocksdb::WriteOptions write_opts;
  out->impl_->db = impl_->db.get();
  out->impl_->cf_aux = impl_->cf_aux;
  out->impl_->cf_roots = impl_->cf_roots;
  out->impl_->cf_meta = impl_->cf_meta;
  out->impl_->committed = false;
  out->impl_->tx = out->impl_->db->BeginTransaction(write_opts);
  if (out->impl_->tx == nullptr) {
    if (error) {
      *error = "failed to begin transaction";
    }
    return false;
  }
  out->impl_->tx->SetSnapshot();
  out->ClearPoison();
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)out;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::Put(ColumnFamilyKind cf,
                                   const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& value,
                                   std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!RocksDbWrapper::BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::Slice value_slice(reinterpret_cast<const char*>(value.data()), value.size());
  rocksdb::Status status = impl_->tx->Put(handle, prefixed_key, value_slice);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  (void)value;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::Delete(ColumnFamilyKind cf,
                                      const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!RocksDbWrapper::BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::Status status = impl_->tx->Delete(handle, prefixed_key);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::Get(ColumnFamilyKind cf,
                                   const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   std::vector<uint8_t>* value,
                                   bool* found,
                                   std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (value == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!RocksDbWrapper::BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  std::string out;
  rocksdb::ReadOptions read_opts;
  rocksdb::Status status = impl_->tx->Get(read_opts, handle, prefixed_key, &out);
  if (status.IsNotFound()) {
    *found = false;
    value->clear();
#if defined(HAVE_ROCKSDB)
    if (cf == ColumnFamilyKind::kRoots && DebugMissingRootsGetEnabled()) {
      std::cerr << "ROOTS_GET_MISS prefix=" << HexBytes(prefix.data(), prefix.size())
                << " key=" << HexBytes(key.data(), key.size())
                << " full=" << HexBytes(reinterpret_cast<const uint8_t*>(prefixed_key.data()),
                                        prefixed_key.size())
                << "\n";
      if (handle != nullptr) {
        std::unique_ptr<rocksdb::Iterator> it(impl_->db->NewIterator(rocksdb::ReadOptions(), handle));
        size_t count = 0;
        for (it->SeekToFirst(); it->Valid() && count < 16; it->Next(), ++count) {
          const rocksdb::Slice k = it->key();
          const rocksdb::Slice v = it->value();
          std::cerr << "ROOTS_CF[" << count << "] "
                    << HexBytes(reinterpret_cast<const uint8_t*>(k.data()), k.size())
                    << " -> "
                    << HexBytes(reinterpret_cast<const uint8_t*>(v.data()), v.size())
                    << "\n";
        }
        if (!it->status().ok()) {
          std::cerr << "ROOTS_CF_ITER_ERR " << it->status().ToString() << "\n";
        }
      }
    }
#endif
    return true;
  }
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  *found = true;
  value->assign(out.begin(), out.end());
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  (void)value;
  (void)found;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::ScanPrefix(
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!RocksDbWrapper::BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefix_str(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::ReadOptions read_opts;
  std::unique_ptr<rocksdb::Iterator> it(impl_->tx->GetIterator(read_opts, handle));
  out->clear();
  for (it->Seek(prefix_str); it->Valid(); it->Next()) {
    rocksdb::Slice key = it->key();
    if (key.size() < prefix_str.size()) {
      break;
    }
    if (!key.starts_with(prefix_str)) {
      break;
    }
    std::vector<uint8_t> key_bytes(key.data() + prefix_str.size(),
                                   key.data() + key.size());
    std::vector<uint8_t> value_bytes(it->value().data(),
                                     it->value().data() + it->value().size());
    out->emplace_back(std::move(key_bytes), std::move(value_bytes));
  }
  rocksdb::Status status = it->status();
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)out;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::DeletePrefix(ColumnFamilyKind cf,
                                             const std::vector<std::vector<uint8_t>>& path,
                                             std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!ScanPrefix(cf, path, &entries, error)) {
    return false;
  }
  for (const auto& kv : entries) {
    if (!Delete(cf, path, kv.first, error)) {
      return false;
    }
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  return false;
#endif
}

bool RocksDbWrapper::Transaction::Commit(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  rocksdb::Status status = impl_->tx->Commit();
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  impl_->committed = true;
  delete impl_->tx;
  impl_->tx = nullptr;
  poisoned_ = false;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::Transaction::Rollback(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (impl_ == nullptr || impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  rocksdb::Status status = impl_->tx->Rollback();
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  delete impl_->tx;
  impl_->tx = nullptr;
  poisoned_ = false;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}
void RocksDbWrapper::WriteBatch::Put(ColumnFamilyKind cf,
                                  const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  const std::vector<uint8_t>& value) {
  Op op;
  op.cf = cf;
  op.path = path;
  op.key = key;
  op.value = value;
  op.is_delete = false;
  op.is_delete_prefix = false;
  ops_.push_back(std::move(op));
}

void RocksDbWrapper::WriteBatch::Delete(ColumnFamilyKind cf,
                                     const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key) {
  Op op;
  op.cf = cf;
  op.path = path;
  op.key = key;
  op.is_delete = true;
  op.is_delete_prefix = false;
  ops_.push_back(std::move(op));
}

void RocksDbWrapper::WriteBatch::DeletePrefix(
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path) {
  Op op;
  op.cf = cf;
  op.path = path;
  op.is_delete = false;
  op.is_delete_prefix = true;
  ops_.push_back(std::move(op));
}

void RocksDbWrapper::WriteBatch::Append(const WriteBatch& other) {
  ops_.insert(ops_.end(), other.ops_.begin(), other.ops_.end());
}

bool RocksDbWrapper::BuildPrefix(const std::vector<std::vector<uint8_t>>& path,
                              std::array<uint8_t, 32>* out,
                              std::string* error) {
  size_t segments = 0;
  std::vector<uint8_t> body;
  if (!BuildPrefixBody(path, &segments, &body, error)) {
    return false;
  }
  if (segments == 0) {
    if (out == nullptr) {
      if (error) {
        *error = "output is null";
      }
      return false;
    }
    out->fill(0);
    return true;
  }
  (void)BlakeBlockCount(body.size());
  return Blake3Hash(body, out, error);
}

bool RocksDbWrapper::Open(const std::string& path, std::string* error) {
#if defined(HAVE_ROCKSDB)
  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.create_missing_column_families = true;
  opts.IncreaseParallelism(2);

  // Avoid constructing descriptors from rvalue ColumnFamilyOptions temporaries.
  const rocksdb::ColumnFamilyOptions cf_opts(opts);
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.reserve(4);
  cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, cf_opts);
  cfs.emplace_back("aux", cf_opts);
  cfs.emplace_back("roots", cf_opts);
  cfs.emplace_back("meta", cf_opts);

  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  rocksdb::OptimisticTransactionDB* db_ptr = nullptr;
  rocksdb::Status status =
      rocksdb::OptimisticTransactionDB::Open(opts, path, cfs, &handles, &db_ptr);
  if (!status.ok()) {
    for (rocksdb::ColumnFamilyHandle* handle : handles) {
      if (db_ptr != nullptr && handle != nullptr) {
        db_ptr->DestroyColumnFamilyHandle(handle);
      }
    }
    delete db_ptr;
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  if (handles.size() < 4) {
    for (rocksdb::ColumnFamilyHandle* handle : handles) {
      if (handle != nullptr) {
        db_ptr->DestroyColumnFamilyHandle(handle);
      }
    }
    delete db_ptr;
    if (error) {
      *error = "rocksdb open returned an unexpected number of column family handles";
    }
    return false;
  }
  impl_->db.reset(db_ptr);
  impl_->cf_default = handles[0];
  impl_->cf_aux = handles[1];
  impl_->cf_roots = handles[2];
  impl_->cf_meta = handles[3];
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)path;
  return false;
#endif
}

bool RocksDbWrapper::Put(ColumnFamilyKind cf,
                      const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value,
                      std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::Slice key_slice(prefixed_key);
  rocksdb::Slice value_slice(reinterpret_cast<const char*>(value.data()), value.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::Status status = impl_->db->Put(rocksdb::WriteOptions(), handle, key_slice, value_slice);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  (void)value;
  return false;
#endif
}

bool RocksDbWrapper::Get(ColumnFamilyKind cf,
                      const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* value,
                      bool* found,
                      std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (value == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  std::string out;
  rocksdb::Status status = impl_->db->Get(rocksdb::ReadOptions(), handle, prefixed_key, &out);
  if (status.IsNotFound()) {
    *found = false;
    value->clear();
    return true;
  }
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  *found = true;
  value->assign(out.begin(), out.end());
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  (void)value;
  (void)found;
  return false;
#endif
}

bool RocksDbWrapper::Delete(ColumnFamilyKind cf,
                         const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         bool* deleted,
                         std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (deleted == nullptr) {
    if (error) {
      *error = "deleted output is null";
    }
    return false;
  }
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  prefixed_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::Status status = impl_->db->Delete(rocksdb::WriteOptions(), handle, prefixed_key);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  *deleted = true;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)key;
  (void)deleted;
  return false;
#endif
}

bool RocksDbWrapper::CommitBatch(const WriteBatch& batch, std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  rocksdb::WriteBatch wb;
  for (const auto& op : batch.ops_) {
    std::array<uint8_t, 32> prefix{};
    if (!BuildPrefix(op.path, &prefix, error)) {
      return false;
    }
    std::string prefix_str(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    rocksdb::ColumnFamilyHandle* handle = nullptr;
    switch (op.cf) {
      case ColumnFamilyKind::kDefault:
        handle = impl_->db->DefaultColumnFamily();
        break;
      case ColumnFamilyKind::kAux:
        handle = impl_->cf_aux;
        break;
      case ColumnFamilyKind::kRoots:
        handle = impl_->cf_roots;
        break;
      case ColumnFamilyKind::kMeta:
        handle = impl_->cf_meta;
        break;
    }
    if (op.is_delete_prefix) {
      rocksdb::ReadOptions read_opts;
      std::unique_ptr<rocksdb::Iterator> it(impl_->db->NewIterator(read_opts, handle));
      for (it->Seek(prefix_str); it->Valid(); it->Next()) {
        rocksdb::Slice key = it->key();
        if (key.size() < prefix_str.size()) {
          break;
        }
        if (!key.starts_with(prefix_str)) {
          break;
        }
        wb.Delete(handle, key);
      }
      rocksdb::Status iter_status = it->status();
      if (!iter_status.ok()) {
        if (error) {
          *error = iter_status.ToString();
        }
        return false;
      }
    } else if (op.is_delete) {
      std::string prefixed_key(prefix_str);
      prefixed_key.append(reinterpret_cast<const char*>(op.key.data()), op.key.size());
      wb.Delete(handle, prefixed_key);
    } else {
      std::string prefixed_key(prefix_str);
      prefixed_key.append(reinterpret_cast<const char*>(op.key.data()), op.key.size());
      rocksdb::Slice value_slice(reinterpret_cast<const char*>(op.value.data()),
                                 op.value.size());
      wb.Put(handle, prefixed_key, value_slice);
    }
  }
  rocksdb::Status status = impl_->db->Write(rocksdb::WriteOptions(), &wb);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)batch;
  return false;
#endif
}

bool RocksDbWrapper::CommitBatch(const WriteBatch& batch,
                              Transaction* transaction,
                              std::string* error) {
  if (transaction == nullptr) {
    return CommitBatch(batch, error);
  }
#if defined(HAVE_ROCKSDB)
  if (transaction->poisoned_) {
    if (error) {
      *error = "transaction is poisoned";
    }
    return false;
  }
  if (transaction->impl_ == nullptr || transaction->impl_->tx == nullptr) {
    if (error) {
      *error = "transaction not initialized";
    }
    return false;
  }
  ColumnFamilyKind cached_cf = ColumnFamilyKind::kDefault;
  const std::vector<std::vector<uint8_t>>* cached_path = nullptr;
  std::string cached_prefix;
  rocksdb::ColumnFamilyHandle* cached_handle = nullptr;
  for (const auto& op : batch.ops_) {
    if (op.is_delete_prefix) {
      if (!transaction->DeletePrefix(op.cf, op.path, error)) {
        return false;
      }
      continue;
    }

    if (cached_path == nullptr || cached_cf != op.cf || *cached_path != op.path) {
      std::array<uint8_t, 32> prefix{};
      if (!BuildPrefix(op.path, &prefix, error)) {
        return false;
      }
      cached_prefix.assign(reinterpret_cast<const char*>(prefix.data()), prefix.size());
      cached_cf = op.cf;
      cached_path = &op.path;
      switch (op.cf) {
        case ColumnFamilyKind::kDefault:
          cached_handle = transaction->impl_->db->DefaultColumnFamily();
          break;
        case ColumnFamilyKind::kAux:
          cached_handle = transaction->impl_->cf_aux;
          break;
        case ColumnFamilyKind::kRoots:
          cached_handle = transaction->impl_->cf_roots;
          break;
        case ColumnFamilyKind::kMeta:
          cached_handle = transaction->impl_->cf_meta;
          break;
      }
    }
    std::string prefixed_key = cached_prefix;
    prefixed_key.append(reinterpret_cast<const char*>(op.key.data()), op.key.size());
    rocksdb::Status status;
    if (op.is_delete) {
      status = transaction->impl_->tx->Delete(cached_handle, prefixed_key);
    } else {
      rocksdb::Slice value_slice(reinterpret_cast<const char*>(op.value.data()), op.value.size());
      status = transaction->impl_->tx->Put(cached_handle, prefixed_key, value_slice);
    }
    if (!status.ok()) {
      if (error) {
        *error = status.ToString();
      }
      return false;
    }
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)batch;
  (void)transaction;
  return false;
#endif
}

static bool GetValueForCost(RocksDbWrapper* storage,
                            RocksDbWrapper::Transaction* transaction,
                            ColumnFamilyKind cf,
                            const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            std::vector<uint8_t>* value,
                            bool* found,
                            std::string* error) {
  if (transaction != nullptr) {
    return transaction->Get(cf, path, key, value, found, error);
  }
  return storage->Get(cf, path, key, value, found, error);
}

bool RocksDbWrapper::CommitBatchWithCost(const WriteBatch& batch,
                                      BatchCost* cost,
                                      std::string* error) {
  return CommitBatchWithCost(batch, nullptr, cost, error);
}

bool RocksDbWrapper::CommitBatchWithCost(const WriteBatch& batch,
                                      Transaction* transaction,
                                      BatchCost* cost,
                                      std::string* error) {
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  *cost = {};
  std::unordered_map<std::string, std::optional<std::vector<uint8_t>>> overlay;
  overlay.reserve(batch.ops_.size());
  std::vector<uint8_t> existing;
  bool found = false;
  for (const auto& op : batch.ops_) {
    if (op.is_delete_prefix) {
      if (error) {
        *error = "batch cost for delete-prefix is unsupported";
      }
      return false;
    }
    std::array<uint8_t, 32> prefix{};
    uint64_t prefix_hash_calls = 0;
    if (!BuildPrefixWithCost(op.path, &prefix, &prefix_hash_calls, error)) {
      return false;
    }
    cost->hash_node_calls += prefix_hash_calls;
    std::string prefixed_key(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    prefixed_key.append(reinterpret_cast<const char*>(op.key.data()), op.key.size());
    std::string overlay_key = std::to_string(static_cast<int>(op.cf));
    overlay_key.push_back(':');
    overlay_key.append(prefixed_key);

    auto overlay_it = overlay.find(overlay_key);
    existing.clear();
    found = false;
    if (overlay_it != overlay.end()) {
      if (overlay_it->second.has_value()) {
        existing = overlay_it->second.value();
        found = true;
      }
    } else {
      cost->seek_count += 1;
      if (!GetValueForCost(this, transaction, op.cf, op.path, op.key, &existing, &found, error)) {
        return false;
      }
      if (found) {
        cost->storage_loaded_bytes += existing.size();
      }
    }

    cost->seek_count += 1;
    uint64_t key_paid = PaidLen(static_cast<uint64_t>(op.key.size()));
    if (op.is_delete) {
      uint64_t existing_len = found ? static_cast<uint64_t>(existing.size()) : 0;
      cost->storage_cost.removed_bytes.Add(
          StorageRemovedBytes::Basic(static_cast<uint32_t>(key_paid + PaidLen(existing_len))));
      overlay[overlay_key] = std::nullopt;
    } else {
      uint64_t value_paid = PaidLen(static_cast<uint64_t>(op.value.size()));
      if (found) {
        cost->storage_cost.replaced_bytes += value_paid;
      } else {
        cost->storage_cost.added_bytes += key_paid + value_paid;
      }
      overlay[overlay_key] = op.value;
    }
  }
  if (transaction != nullptr) {
    return CommitBatch(batch, transaction, error);
  }
  return CommitBatch(batch, error);
}

bool RocksDbWrapper::ScanPrefix(
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  std::array<uint8_t, 32> prefix{};
  if (!BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefix_str(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::ReadOptions read_opts;
  std::unique_ptr<rocksdb::Iterator> it(impl_->db->NewIterator(read_opts, handle));
  out->clear();
  for (it->Seek(prefix_str); it->Valid(); it->Next()) {
    rocksdb::Slice key = it->key();
    if (key.size() < prefix_str.size()) {
      break;
    }
    if (!key.starts_with(prefix_str)) {
      break;
    }
    std::vector<uint8_t> key_bytes(key.data() + prefix_str.size(),
                                   key.data() + key.size());
    std::vector<uint8_t> value_bytes(it->value().data(),
                                     it->value().data() + it->value().size());
    out->emplace_back(std::move(key_bytes), std::move(value_bytes));
  }
  rocksdb::Status status = it->status();
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  (void)out;
  return false;
#endif
}

bool RocksDbWrapper::DeletePrefix(ColumnFamilyKind cf,
                               const std::vector<std::vector<uint8_t>>& path,
                               std::string* error) {
#if defined(HAVE_ROCKSDB)
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!ScanPrefix(cf, path, &entries, error)) {
    return false;
  }
  rocksdb::WriteBatch wb;
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  std::array<uint8_t, 32> prefix{};
  if (!BuildPrefix(path, &prefix, error)) {
    return false;
  }
  std::string prefix_str(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  for (const auto& kv : entries) {
    std::string key(prefix_str);
    key.append(reinterpret_cast<const char*>(kv.first.data()), kv.first.size());
    wb.Delete(handle, key);
  }
  rocksdb::Status status = impl_->db->Write(rocksdb::WriteOptions(), &wb);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  (void)path;
  return false;
#endif
}

bool RocksDbWrapper::Clear(ColumnFamilyKind cf, std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      handle = impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      handle = impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      handle = impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      handle = impl_->cf_meta;
      break;
  }
  rocksdb::ReadOptions read_opts;
  std::unique_ptr<rocksdb::Iterator> it(impl_->db->NewIterator(read_opts, handle));
  rocksdb::WriteBatch wb;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    wb.Delete(handle, it->key());
  }
  rocksdb::Status status = it->status();
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  status = impl_->db->Write(rocksdb::WriteOptions(), &wb);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)cf;
  return false;
#endif
}

bool RocksDbWrapper::Flush(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  rocksdb::Status status = impl_->db->Flush(rocksdb::FlushOptions());
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::CreateCheckpoint(const std::string& path, std::string* error) {
#if defined(HAVE_ROCKSDB)
  constexpr uint64_t kNeverFlushWalForCheckpoint = std::numeric_limits<uint64_t>::max();
  if (!impl_->db) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  rocksdb::Checkpoint* checkpoint_ptr = nullptr;
  rocksdb::Status status = rocksdb::Checkpoint::Create(impl_->db.get(), &checkpoint_ptr);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  std::unique_ptr<rocksdb::Checkpoint> checkpoint(checkpoint_ptr);
  status = checkpoint->CreateCheckpoint(path, kNeverFlushWalForCheckpoint);
  if (!status.ok()) {
    if (error) {
      *error = status.ToString();
    }
    return false;
  }
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)path;
  return false;
#endif
}

RocksDbWrapper::PrefixedContext::PrefixedContext(RocksDbWrapper* storage,
                                              ColumnFamilyKind cf,
                                              std::vector<std::vector<uint8_t>> path)
    : storage_(storage), cf_(cf), path_(std::move(path)) {}

bool RocksDbWrapper::PrefixedContext::Put(const std::vector<uint8_t>& key,
                                       const std::vector<uint8_t>& value,
                                       std::string* error) {
  if (storage_ == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  return storage_->Put(cf_, path_, key, value, error);
}

bool RocksDbWrapper::PrefixedContext::Get(const std::vector<uint8_t>& key,
                                       std::vector<uint8_t>* value,
                                       bool* found,
                                       std::string* error) const {
  if (storage_ == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  return storage_->Get(cf_, path_, key, value, found, error);
}

bool RocksDbWrapper::PrefixedContext::Delete(const std::vector<uint8_t>& key,
                                          bool* deleted,
                                          std::string* error) {
  if (storage_ == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  return storage_->Delete(cf_, path_, key, deleted, error);
}

bool RocksDbWrapper::PrefixedContext::Scan(
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) const {
  if (storage_ == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  return storage_->ScanPrefix(cf_, path_, out, error);
}

RocksDbWrapper::TransactionPrefixedContext::TransactionPrefixedContext(
    Transaction* tx,
    ColumnFamilyKind cf,
    std::vector<std::vector<uint8_t>> path,
    WriteBatch* batch)
    : tx_(tx), cf_(cf), path_(std::move(path)), batch_(batch) {}

bool RocksDbWrapper::TransactionPrefixedContext::Put(const std::vector<uint8_t>& key,
                                                  const std::vector<uint8_t>& value,
                                                  std::string* error) {
  if (tx_ == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (batch_ != nullptr) {
    batch_->Put(cf_, path_, key, value);
    return true;
  }
  return true;
}

bool RocksDbWrapper::TransactionPrefixedContext::Get(const std::vector<uint8_t>& key,
                                                  std::vector<uint8_t>* value,
                                                  bool* found,
                                                  std::string* error) const {
  if (tx_ == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  return tx_->Get(cf_, path_, key, value, found, error);
}

bool RocksDbWrapper::TransactionPrefixedContext::Delete(const std::vector<uint8_t>& key,
                                                     std::string* error) {
  if (tx_ == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (batch_ != nullptr) {
    batch_->Delete(cf_, path_, key);
    return true;
  }
  return true;
}

bool RocksDbWrapper::TransactionPrefixedContext::Scan(
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) const {
  if (tx_ == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  return tx_->ScanPrefix(cf_, path_, out, error);
}

bool RocksDbWrapper::TransactionPrefixedContext::CommitBatchPart(
    const WriteBatch& batch_part,
    std::string* error) {
  if (tx_ == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (batch_ != nullptr) {
    batch_->Append(batch_part);
  }
  return true;
}

RocksDbWrapper::PrefixedIterator::PrefixedIterator() : impl_(new Impl()) {}

RocksDbWrapper::PrefixedIterator::~PrefixedIterator() {
  delete impl_;
}

bool RocksDbWrapper::PrefixedIterator::Init(RocksDbWrapper* storage,
                                         ColumnFamilyKind cf,
                                         const std::vector<std::vector<uint8_t>>& path,
                                         std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (storage == nullptr || storage->impl_ == nullptr || !storage->impl_->db) {
    if (error) {
      *error = "storage not opened";
    }
    return false;
  }
  impl_->db = storage->impl_->db.get();
  switch (cf) {
    case ColumnFamilyKind::kDefault:
      impl_->handle = storage->impl_->db->DefaultColumnFamily();
      break;
    case ColumnFamilyKind::kAux:
      impl_->handle = storage->impl_->cf_aux;
      break;
    case ColumnFamilyKind::kRoots:
      impl_->handle = storage->impl_->cf_roots;
      break;
    case ColumnFamilyKind::kMeta:
      impl_->handle = storage->impl_->cf_meta;
      break;
  }
  std::array<uint8_t, 32> prefix{};
  if (!RocksDbWrapper::BuildPrefix(path, &prefix, error)) {
    return false;
  }
  impl_->prefix.assign(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  impl_->it.reset(impl_->db->NewIterator(rocksdb::ReadOptions(), impl_->handle));
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)storage;
  (void)cf;
  (void)path;
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::SeekToFirst(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  impl_->it->Seek(impl_->prefix);
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Seek(const std::vector<uint8_t>& key,
                                         std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  std::string seek_key = impl_->prefix;
  seek_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  impl_->it->Seek(seek_key);
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)key;
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::SeekToLast(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  std::string prefix = impl_->prefix;
  for (int i = static_cast<int>(prefix.size()) - 1; i >= 0; --i) {
    uint8_t byte = static_cast<uint8_t>(prefix[static_cast<size_t>(i)]);
    byte = static_cast<uint8_t>(byte + 1);
    prefix[static_cast<size_t>(i)] = static_cast<char>(byte);
    if (byte != 0) {
      break;
    }
  }
  impl_->it->SeekForPrev(prefix);
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::SeekForPrev(const std::vector<uint8_t>& key,
                                                std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  std::string seek_key = impl_->prefix;
  seek_key.append(reinterpret_cast<const char*>(key.data()), key.size());
  impl_->it->SeekForPrev(seek_key);
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)key;
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Valid() const {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it || !impl_->it->Valid()) {
    const_cast<PrefixedIterator*>(this)->last_cost_ = {};
    return false;
  }
  rocksdb::Slice key = impl_->it->key();
  bool ok = key.starts_with(impl_->prefix);
  const_cast<PrefixedIterator*>(this)->last_cost_.seek_count = 0;
  const_cast<PrefixedIterator*>(this)->last_cost_.storage_loaded_bytes =
      ok ? static_cast<uint64_t>(key.size()) : 0;
  return ok;
#else
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Next(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  impl_->it->Next();
  if (!impl_->it->status().ok()) {
    if (error) {
      *error = impl_->it->status().ToString();
    }
    return false;
  }
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Prev(std::string* error) {
#if defined(HAVE_ROCKSDB)
  if (!impl_ || !impl_->it) {
    if (error) {
      *error = "iterator not initialized";
    }
    return false;
  }
  impl_->it->Prev();
  if (!impl_->it->status().ok()) {
    if (error) {
      *error = impl_->it->status().ToString();
    }
    return false;
  }
  last_cost_.seek_count = 1;
  last_cost_.storage_loaded_bytes = 0;
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Key(std::vector<uint8_t>* out,
                                        std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (!Valid()) {
    if (error) {
      *error = "iterator not valid";
    }
    return false;
  }
  rocksdb::Slice key = impl_->it->key();
  out->assign(key.data() + impl_->prefix.size(), key.data() + key.size());
  const_cast<PrefixedIterator*>(this)->last_cost_.seek_count = 0;
  const_cast<PrefixedIterator*>(this)->last_cost_.storage_loaded_bytes =
      static_cast<uint64_t>(key.size());
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)out;
  return false;
#endif
}

bool RocksDbWrapper::PrefixedIterator::Value(std::vector<uint8_t>* out,
                                          std::string* error) const {
#if defined(HAVE_ROCKSDB)
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (!Valid()) {
    if (error) {
      *error = "iterator not valid";
    }
    return false;
  }
  rocksdb::Slice value = impl_->it->value();
  out->assign(value.data(), value.data() + value.size());
  const_cast<PrefixedIterator*>(this)->last_cost_.seek_count = 0;
  const_cast<PrefixedIterator*>(this)->last_cost_.storage_loaded_bytes =
      static_cast<uint64_t>(value.size());
  return true;
#else
  if (error) {
    *error = "rocksdb support not enabled";
  }
  (void)out;
  return false;
#endif
}

const IteratorCost& RocksDbWrapper::PrefixedIterator::LastCost() const {
  return last_cost_;
}

}  // namespace grovedb
