#ifndef GROVEDB_CPP_CHUNK_PRODUCER_H
#define GROVEDB_CPP_CHUNK_PRODUCER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "merk.h"

namespace grovedb {

struct SubtreeChunk {
  std::vector<uint8_t> chunk;
  std::optional<size_t> next_index;
  std::optional<size_t> remaining_limit;
};

struct MultiChunk {
  std::vector<uint8_t> chunk;
  std::optional<std::vector<uint8_t>> next_index;
  std::optional<size_t> remaining_limit;
};

class ChunkProducer {
 public:
  ChunkProducer() = default;
  static bool Create(const MerkTree& merk,
                     bool provable_count,
                     ChunkProducer* out,
                     std::string* error);

  size_t Len() const;

  bool ChunkWithIndex(size_t index, SubtreeChunk* out, std::string* error);
  bool Chunk(const std::vector<uint8_t>& chunk_id,
             SubtreeChunk* out,
             std::string* error);

  bool MultiChunkWithLimit(const std::vector<uint8_t>& chunk_id,
                           std::optional<size_t> limit,
                           MultiChunk* out,
                           std::string* error);
  bool MultiChunkWithLimitAndIndex(size_t index,
                                   std::optional<size_t> limit,
                                   MultiChunk* out,
                                   std::string* error);

 private:
  ChunkProducer(const MerkTree& merk, size_t height, bool provable_count);

  bool ChunkInternal(size_t index,
                     const std::vector<bool>& instructions,
                     SubtreeChunk* out,
                     std::string* error);

  const MerkTree* merk_ = nullptr;
  size_t height_ = 0;
  size_t index_ = 1;
  bool provable_count_ = false;
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_CHUNK_PRODUCER_H
