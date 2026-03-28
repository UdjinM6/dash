#ifndef GROVEDB_CPP_CHUNK_RESTORE_H
#define GROVEDB_CPP_CHUNK_RESTORE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "merk.h"
#include "rocksdb_wrapper.h"

namespace grovedb {

class ChunkRestorer {
 public:
  ChunkRestorer() = default;
  bool Init(const std::vector<uint8_t>& expected_root_hash,
            const std::vector<uint8_t>* parent_value_hash,
            std::string* error);
  bool ProcessChunkProof(const std::vector<uint8_t>& proof,
                         std::vector<std::vector<uint8_t>>* next_chunk_ids,
                         std::string* error);
  bool ProcessChunkProofAndStore(const std::vector<uint8_t>& proof,
                                 RocksDbWrapper* storage,
                                 const std::vector<std::vector<uint8_t>>& path,
                                 std::vector<std::vector<uint8_t>>* next_chunk_ids,
                                 std::string* error);
  bool FinalizeToMerkTree(RocksDbWrapper* storage,
                          const std::vector<std::vector<uint8_t>>& path,
                          MerkTree* out,
                          std::string* error);
  bool HasPendingChunks() const;

 private:
  bool ProcessChunkProofInternal(const std::vector<uint8_t>& proof,
                                 std::vector<std::vector<uint8_t>>* next_chunk_ids,
                                 RocksDbWrapper* storage,
                                 const std::vector<std::vector<uint8_t>>* path,
                                 std::string* error);
  std::map<std::string, std::vector<uint8_t>> expected_hashes_;
  std::map<std::string, std::vector<uint8_t>> parent_keys_;
  std::vector<uint8_t> expected_root_hash_;
  std::vector<uint8_t> root_key_;
  std::vector<uint8_t> parent_value_hash_;
  bool has_parent_hash_ = false;
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_CHUNK_RESTORE_H
