#ifndef GROVEDB_CPP_MERK_STORAGE_H
#define GROVEDB_CPP_MERK_STORAGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "merk.h"
#include "rocksdb_wrapper.h"

namespace grovedb {

class MerkStorage {
 public:
  static bool SaveTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       std::string* error);
  static bool SaveTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       OperationCost* cost,
                       std::string* error);
  static bool SaveTree(RocksDbWrapper* storage,
                       RocksDbWrapper::Transaction* transaction,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       std::string* error);
  static bool SaveTree(RocksDbWrapper* storage,
                       RocksDbWrapper::Transaction* transaction,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       OperationCost* cost,
                       std::string* error);
  static bool SaveTreeToBatch(RocksDbWrapper* storage,
                              RocksDbWrapper::Transaction* transaction,
                              const std::vector<std::vector<uint8_t>>& path,
                              MerkTree* tree,
                              RocksDbWrapper::WriteBatch* batch,
                              std::string* error);
  static bool LoadTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       std::string* error);
  static bool LoadTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       OperationCost* cost,
                       std::string* error);
  static bool LoadTree(RocksDbWrapper* storage,
                       RocksDbWrapper::Transaction* transaction,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       std::string* error);
  static bool LoadTree(RocksDbWrapper* storage,
                       RocksDbWrapper::Transaction* transaction,
                       const std::vector<std::vector<uint8_t>>& path,
                       MerkTree* tree,
                       OperationCost* cost,
                       std::string* error);
  static bool ClearTree(RocksDbWrapper* storage,
                        const std::vector<std::vector<uint8_t>>& path,
                        std::string* error);
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_MERK_STORAGE_H
