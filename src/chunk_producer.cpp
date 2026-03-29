#include "chunk_producer.h"

#include "binary.h"
#include "chunk.h"
#include "cost_utils.h"

namespace grovedb {

ChunkProducer::ChunkProducer(const MerkTree& merk, size_t height, bool provable_count)
    : merk_(&merk), height_(height), index_(1), provable_count_(provable_count) {}

bool ChunkProducer::Create(const MerkTree& merk,
                           bool provable_count,
                           ChunkProducer* out,
                           std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "chunk producer output is null";
    }
    return false;
  }
  size_t height = static_cast<size_t>(merk.Height());
  if (height == 0) {
    if (error) {
      *error = "cannot create chunk producer for empty tree";
    }
    return false;
  }
  *out = ChunkProducer(merk, height, provable_count);
  return true;
}

size_t ChunkProducer::Len() const {
  return NumberOfChunks(height_);
}

bool ChunkProducer::ChunkInternal(size_t index,
                                  const std::vector<bool>& instructions,
                                  SubtreeChunk* out,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "chunk output is null";
    }
    return false;
  }
  size_t max_chunk_index = Len();
  if (index < 1 || index > max_chunk_index) {
    if (error) {
      *error = "chunk index out of bounds";
    }
    return false;
  }
  index_ = index + 1;

  size_t chunk_height = 0;
  if (!ChunkHeight(height_, index, &chunk_height, error)) {
    return false;
  }

  std::vector<uint8_t> chunk_bytes;
  if (!merk_->GenerateChunkProofAt(instructions,
                                   chunk_height,
                                   nullptr,
                                   provable_count_,
                                   &chunk_bytes,
                                   error)) {
    return false;
  }

  out->chunk = std::move(chunk_bytes);
  if (index_ > max_chunk_index) {
    out->next_index.reset();
  } else {
    out->next_index = index_;
  }
  out->remaining_limit.reset();
  return true;
}

bool ChunkProducer::ChunkWithIndex(size_t index, SubtreeChunk* out, std::string* error) {
  std::vector<bool> instructions;
  if (!GenerateTraversalInstruction(height_, index, &instructions, error)) {
    return false;
  }
  return ChunkInternal(index, instructions, out, error);
}

bool ChunkProducer::Chunk(const std::vector<uint8_t>& chunk_id,
                          SubtreeChunk* out,
                          std::string* error) {
  std::vector<bool> instructions;
  if (!BytesToTraversalInstruction(chunk_id, &instructions, error)) {
    return false;
  }
  size_t index = 0;
  if (!ChunkIndexFromTraversalInstructionWithRecovery(instructions, height_, &index, error)) {
    return false;
  }
  return ChunkInternal(index, instructions, out, error);
}

bool ChunkProducer::MultiChunkWithLimit(const std::vector<uint8_t>& chunk_id,
                                        std::optional<size_t> limit,
                                        MultiChunk* out,
                                        std::string* error) {
  std::vector<bool> instructions;
  if (!BytesToTraversalInstruction(chunk_id, &instructions, error)) {
    return false;
  }
  size_t index = 0;
  if (!ChunkIndexFromTraversalInstruction(instructions, height_, &index, error)) {
    return false;
  }
  return MultiChunkWithLimitAndIndex(index, limit, out, error);
}

bool ChunkProducer::MultiChunkWithLimitAndIndex(size_t index,
                                                std::optional<size_t> limit,
                                                MultiChunk* out,
                                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "multichunk output is null";
    }
    return false;
  }
  size_t max_chunk_index = Len();
  if (index < 1 || index > max_chunk_index) {
    if (error) {
      *error = "chunk index out of bounds";
    }
    return false;
  }

  out->chunk.clear();
  size_t current_index = index;
  std::optional<size_t> remaining_limit = limit;

  while (true) {
    std::vector<bool> instructions;
    if (!GenerateTraversalInstruction(height_, current_index, &instructions, error)) {
      return false;
    }
    std::vector<uint8_t> chunk_bytes;
    size_t chunk_height = 0;
    if (!ChunkHeight(height_, current_index, &chunk_height, error)) {
      return false;
    }
    if (!merk_->GenerateChunkProofAt(instructions,
                                     chunk_height,
                                     nullptr,
                                     provable_count_,
                                     &chunk_bytes,
                                     error)) {
      return false;
    }
    std::vector<uint8_t> chunk_ops_bytes;
    if (!merk_->GenerateChunkOpsAt(instructions,
                                   chunk_height,
                                   nullptr,
                                   provable_count_,
                                   &chunk_ops_bytes,
                                   error)) {
      return false;
    }
    const std::vector<uint8_t> chunk_id = TraversalInstructionToBytes(instructions);
    const size_t chunk_id_len =
        1 + static_cast<size_t>(VarintLenU64(static_cast<uint64_t>(chunk_id.size()))) +
        chunk_id.size();
    const size_t rust_accounted_len = chunk_id_len + chunk_ops_bytes.size();
    if (remaining_limit) {
      if (rust_accounted_len > *remaining_limit) {
        if (out->chunk.empty()) {
          if (error) {
            *error = "limit too small";
          }
          return false;
        }
        break;
      }
      *remaining_limit -= rust_accounted_len;
    }
    out->chunk.insert(out->chunk.end(), chunk_bytes.begin(), chunk_bytes.end());
    current_index += 1;
    if (current_index > max_chunk_index) {
      out->next_index.reset();
      break;
    }
  }

  if (remaining_limit) {
    out->remaining_limit = *remaining_limit;
  } else {
    out->remaining_limit.reset();
  }
  if (current_index > max_chunk_index) {
    out->next_index.reset();
  } else {
    std::vector<bool> next_instr;
    if (!GenerateTraversalInstruction(height_, current_index, &next_instr, error)) {
      return false;
    }
    out->next_index = TraversalInstructionToBytes(next_instr);
  }
  return true;
}

}  // namespace grovedb
