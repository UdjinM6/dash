#ifndef GROVEDB_CPP_ROCKSDB_WRAPPER_H
#define GROVEDB_CPP_ROCKSDB_WRAPPER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "operation_cost.h"

namespace grovedb {

struct IteratorCost {
  uint64_t seek_count = 0;
  uint64_t storage_loaded_bytes = 0;
};

using BatchCost = OperationCost;

enum class ColumnFamilyKind {
  kDefault,
  kAux,
  kRoots,
  kMeta,
};

class RocksDbWrapper {
 public:
  class WriteBatch;

  RocksDbWrapper();
  ~RocksDbWrapper();

  bool Open(const std::string& path, std::string* error);
  bool Put(ColumnFamilyKind cf,
           const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           const std::vector<uint8_t>& value,
           std::string* error);
  bool Get(ColumnFamilyKind cf,
           const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           std::vector<uint8_t>* value,
           bool* found,
           std::string* error) const;
  bool Delete(ColumnFamilyKind cf,
              const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* deleted,
              std::string* error);
  bool ScanPrefix(ColumnFamilyKind cf,
                  const std::vector<std::vector<uint8_t>>& path,
                  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                  std::string* error) const;
  bool DeletePrefix(ColumnFamilyKind cf,
                    const std::vector<std::vector<uint8_t>>& path,
                    std::string* error);
  bool Clear(ColumnFamilyKind cf, std::string* error);
  bool Flush(std::string* error);
  bool CreateCheckpoint(const std::string& path, std::string* error);
  class PrefixedContext {
   public:
    PrefixedContext(RocksDbWrapper* storage,
                    ColumnFamilyKind cf,
                    std::vector<std::vector<uint8_t>> path);
    bool Put(const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value,
             std::string* error);
    bool Get(const std::vector<uint8_t>& key,
             std::vector<uint8_t>* value,
             bool* found,
             std::string* error) const;
    bool Delete(const std::vector<uint8_t>& key, bool* deleted, std::string* error);
    bool Scan(std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
              std::string* error) const;

   private:
    RocksDbWrapper* storage_ = nullptr;
    ColumnFamilyKind cf_ = ColumnFamilyKind::kDefault;
    std::vector<std::vector<uint8_t>> path_;
  };
  class PrefixedIterator {
   public:
    PrefixedIterator();
    ~PrefixedIterator();
    bool Init(RocksDbWrapper* storage,
              ColumnFamilyKind cf,
              const std::vector<std::vector<uint8_t>>& path,
              std::string* error);
    bool SeekToFirst(std::string* error);
    bool Seek(const std::vector<uint8_t>& key, std::string* error);
    bool SeekToLast(std::string* error);
    bool SeekForPrev(const std::vector<uint8_t>& key, std::string* error);
    bool Prev(std::string* error);
    bool Valid() const;
    bool Next(std::string* error);
    bool Key(std::vector<uint8_t>* out, std::string* error) const;
    bool Value(std::vector<uint8_t>* out, std::string* error) const;
    const IteratorCost& LastCost() const;

   private:
    struct Impl;
    Impl* impl_;
    IteratorCost last_cost_{};
  };
  class Transaction {
   public:
    Transaction();
    ~Transaction();
    bool IsPoisoned() const;
    void Poison();
    void ClearPoison();
    bool Put(ColumnFamilyKind cf,
             const std::vector<std::vector<uint8_t>>& path,
             const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value,
             std::string* error);
    bool Delete(ColumnFamilyKind cf,
                const std::vector<std::vector<uint8_t>>& path,
                const std::vector<uint8_t>& key,
                std::string* error);
    bool Get(ColumnFamilyKind cf,
             const std::vector<std::vector<uint8_t>>& path,
             const std::vector<uint8_t>& key,
             std::vector<uint8_t>* value,
             bool* found,
             std::string* error) const;
    bool ScanPrefix(ColumnFamilyKind cf,
                    const std::vector<std::vector<uint8_t>>& path,
                    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                    std::string* error) const;
    bool DeletePrefix(ColumnFamilyKind cf,
                      const std::vector<std::vector<uint8_t>>& path,
                      std::string* error);
    bool Commit(std::string* error);
    bool Rollback(std::string* error);

   private:
    struct Impl;
    Impl* impl_;
    bool poisoned_ = false;
    friend class RocksDbWrapper;
  };
  class TransactionPrefixedContext {
   public:
    TransactionPrefixedContext(Transaction* tx,
                               ColumnFamilyKind cf,
                               std::vector<std::vector<uint8_t>> path,
                               WriteBatch* batch = nullptr);
    bool Put(const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value,
             std::string* error);
    bool Get(const std::vector<uint8_t>& key,
             std::vector<uint8_t>* value,
             bool* found,
             std::string* error) const;
    bool Delete(const std::vector<uint8_t>& key, std::string* error);
    bool Scan(std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
              std::string* error) const;
    bool CommitBatchPart(const WriteBatch& batch_part, std::string* error);

   private:
    Transaction* tx_ = nullptr;
    ColumnFamilyKind cf_ = ColumnFamilyKind::kDefault;
    std::vector<std::vector<uint8_t>> path_;
    WriteBatch* batch_ = nullptr;
  };
  bool BeginTransaction(Transaction* out, std::string* error);
  PrefixedContext Context(ColumnFamilyKind cf,
                          const std::vector<std::vector<uint8_t>>& path);
  TransactionPrefixedContext Context(Transaction* tx,
                                     ColumnFamilyKind cf,
                                     const std::vector<std::vector<uint8_t>>& path);
  TransactionPrefixedContext Context(Transaction* tx,
                                     ColumnFamilyKind cf,
                                     const std::vector<std::vector<uint8_t>>& path,
                                     WriteBatch* batch);
  class WriteBatch {
   public:
    void Put(ColumnFamilyKind cf,
             const std::vector<std::vector<uint8_t>>& path,
             const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value);
    void Delete(ColumnFamilyKind cf,
                const std::vector<std::vector<uint8_t>>& path,
                const std::vector<uint8_t>& key);
    void DeletePrefix(ColumnFamilyKind cf,
                      const std::vector<std::vector<uint8_t>>& path);
    void Append(const WriteBatch& other);

   private:
    struct Op {
      ColumnFamilyKind cf;
      std::vector<std::vector<uint8_t>> path;
      std::vector<uint8_t> key;
      std::vector<uint8_t> value;
      bool is_delete = false;
      bool is_delete_prefix = false;
    };
    std::vector<Op> ops_;
    friend class RocksDbWrapper;
  };
  bool CommitBatch(const WriteBatch& batch, std::string* error);
  bool CommitBatch(const WriteBatch& batch, Transaction* transaction, std::string* error);
  bool CommitBatchWithCost(const WriteBatch& batch, BatchCost* cost, std::string* error);
  bool CommitBatchWithCost(const WriteBatch& batch,
                           Transaction* transaction,
                           BatchCost* cost,
                           std::string* error);

  static bool BuildPrefix(const std::vector<std::vector<uint8_t>>& path,
                          std::array<uint8_t, 32>* out,
                          std::string* error);

 private:
  struct Impl;
  Impl* impl_;
};

inline RocksDbWrapper::PrefixedContext RocksDbWrapper::Context(
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path) {
  return PrefixedContext(this, cf, path);
}

inline RocksDbWrapper::TransactionPrefixedContext RocksDbWrapper::Context(
    Transaction* tx,
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path) {
  return TransactionPrefixedContext(tx, cf, path);
}

inline RocksDbWrapper::TransactionPrefixedContext RocksDbWrapper::Context(
    Transaction* tx,
    ColumnFamilyKind cf,
    const std::vector<std::vector<uint8_t>>& path,
    WriteBatch* batch) {
  return TransactionPrefixedContext(tx, cf, path, batch);
}

}  // namespace grovedb

#endif  // GROVEDB_CPP_ROCKSDB_WRAPPER_H
