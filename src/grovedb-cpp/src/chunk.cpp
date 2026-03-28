#include "chunk.h"

#include <algorithm>

namespace grovedb {

namespace {

struct BinaryRange {
  size_t start = 0;
  size_t end = 0;

  static bool Create(size_t start,
                     size_t end,
                     BinaryRange* out,
                     std::string* error) {
    if (out == nullptr) {
      if (error) {
        *error = "range output is null";
      }
      return false;
    }
    if (start > end) {
      if (error) {
        *error = "start greater than end";
      }
      return false;
    }
    if (start < 1) {
      if (error) {
        *error = "start must be >= 1";
      }
      return false;
    }
    out->start = start;
    out->end = end;
    return true;
  }

  size_t Len() const { return end - start + 1; }
  bool Odd() const { return (Len() % 2) != 0; }

  bool WhichHalf(size_t value, bool* out) const {
    if (out == nullptr) {
      return false;
    }
    if (value < start || value > end || Odd()) {
      return false;
    }
    size_t half_size = Len() / 2;
    size_t second_half_start = start + half_size;
    *out = (value < second_half_start) ? kChunkLeft : kChunkRight;
    return true;
  }

  bool GetHalf(bool left, BinaryRange* out, std::string* error) const {
    if (out == nullptr) {
      if (error) {
        *error = "range output is null";
      }
      return false;
    }
    if (Odd()) {
      if (error) {
        *error = "cannot split odd range";
      }
      return false;
    }
    size_t half_size = Len() / 2;
    size_t second_half_start = start + half_size;
    if (left) {
      out->start = start;
      out->end = second_half_start - 1;
    } else {
      out->start = second_half_start;
      out->end = end;
    }
    return true;
  }

  bool AdvanceStart(BinaryRange* out, size_t* prev_start, std::string* error) const {
    if (out == nullptr || prev_start == nullptr) {
      if (error) {
        *error = "range output is null";
      }
      return false;
    }
    if (start == end) {
      if (error) {
        *error = "cannot advance start when start == end";
      }
      return false;
    }
    out->start = start + 1;
    out->end = end;
    *prev_start = start;
    return true;
  }
};

std::vector<size_t> ChunkHeightPerLayer(size_t height) {
  if (height == 0) {
    return {};
  }
  size_t two_count = 0;
  size_t three_count = height / 3;
  if (height < 2) {
    two_count = 1;
  } else {
    switch (height % 3) {
      case 0:
        break;
      case 1:
        if (three_count > 0) {
          three_count -= 1;
        }
        two_count += 2;
        break;
      case 2:
        two_count += 1;
        break;
      default:
        break;
    }
  }
  std::vector<size_t> layer_heights(three_count, 3);
  layer_heights.insert(layer_heights.end(), two_count, 2);
  return layer_heights;
}

size_t ExitNodeCount(size_t height) {
  return static_cast<size_t>(1) << height;
}

size_t NumberOfChunksInternal(const std::vector<size_t>& layer_heights) {
  if (layer_heights.empty()) {
    return 0;
  }
  std::vector<size_t> exits;
  exits.reserve(layer_heights.size());
  for (size_t height : layer_heights) {
    exits.push_back(ExitNodeCount(height));
  }
  if (!exits.empty()) {
    exits.pop_back();
  }
  std::vector<size_t> chunk_counts;
  chunk_counts.reserve(exits.size() + 1);
  chunk_counts.push_back(1);
  for (size_t i = 0; i < exits.size(); ++i) {
    size_t prev = chunk_counts[i];
    chunk_counts.push_back(prev * exits[i]);
  }
  size_t total = 0;
  for (size_t count : chunk_counts) {
    total += count;
  }
  return total;
}

}  // namespace

bool GenerateTraversalInstruction(size_t height,
                                  size_t chunk_index,
                                  std::vector<bool>* out,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "instruction output is null";
    }
    return false;
  }
  size_t total = NumberOfChunks(height);
  if (chunk_index < 1 || chunk_index > total) {
    if (error) {
      *error = "chunk id out of bounds";
    }
    return false;
  }
  BinaryRange range;
  if (!BinaryRange::Create(1, total, &range, error)) {
    return false;
  }
  out->clear();
  while (range.Len() > 1) {
    if (range.Odd()) {
      BinaryRange next;
      size_t prev_start = 0;
      if (!range.AdvanceStart(&next, &prev_start, error)) {
        return false;
      }
      range = next;
      if (prev_start == chunk_index) {
        return true;
      }
    } else {
      bool left = false;
      if (!range.WhichHalf(chunk_index, &left)) {
        if (error) {
          *error = "chunk id not in range";
        }
        return false;
      }
      out->push_back(left);
      BinaryRange next;
      if (!range.GetHalf(left, &next, error)) {
        return false;
      }
      range = next;
    }
  }
  return true;
}

bool ChunkHeight(size_t height, size_t chunk_index, size_t* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "height output is null";
    }
    return false;
  }
  std::vector<bool> instruction;
  if (!GenerateTraversalInstruction(height, chunk_index, &instruction, error)) {
    return false;
  }
  size_t remaining_depth = instruction.size() + 1;
  std::vector<size_t> layer_heights = ChunkHeightPerLayer(height);
  size_t layer = 0;
  while (remaining_depth > 1) {
    if (layer >= layer_heights.size()) {
      if (error) {
        *error = "chunk layer out of range";
      }
      return false;
    }
    if (remaining_depth <= layer_heights[layer]) {
      if (error) {
        *error = "chunk depth inconsistent";
      }
      return false;
    }
    remaining_depth -= layer_heights[layer];
    layer += 1;
  }
  if (layer >= layer_heights.size()) {
    if (error) {
      *error = "chunk layer out of range";
    }
    return false;
  }
  *out = layer_heights[layer];
  return true;
}

size_t NumberOfChunks(size_t height) {
  std::vector<size_t> layer_heights = ChunkHeightPerLayer(height);
  return NumberOfChunksInternal(layer_heights);
}

bool NumberOfChunksUnderChunkId(size_t height,
                                size_t chunk_id,
                                size_t* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t chunk_layer = 0;
  std::vector<bool> instruction;
  if (!GenerateTraversalInstruction(height, chunk_id, &instruction, error)) {
    return false;
  }
  size_t remaining_depth = instruction.size() + 1;
  std::vector<size_t> layer_heights = ChunkHeightPerLayer(height);
  while (remaining_depth > 1) {
    if (chunk_layer >= layer_heights.size()) {
      if (error) {
        *error = "chunk layer out of range";
      }
      return false;
    }
    remaining_depth -= layer_heights[chunk_layer];
    chunk_layer += 1;
  }
  if (chunk_layer > layer_heights.size()) {
    if (error) {
      *error = "chunk layer out of range";
    }
    return false;
  }
  std::vector<size_t> subset(layer_heights.begin() + static_cast<long>(chunk_layer),
                             layer_heights.end());
  *out = NumberOfChunksInternal(subset);
  return true;
}

bool ChunkIndexFromTraversalInstruction(const std::vector<bool>& instruction,
                                        size_t height,
                                        size_t* out,
                                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  if (instruction.empty()) {
    *out = 1;
    return true;
  }
  size_t chunk_count = NumberOfChunks(height);
  size_t current_index = 1;
  std::vector<size_t> layer_heights = ChunkHeightPerLayer(height);
  if (layer_heights.empty()) {
    if (error) {
      *error = "empty tree";
    }
    return false;
  }
  size_t last_layer_height = layer_heights.back();
  layer_heights.pop_back();
  if (instruction.size() > height - last_layer_height) {
    if (error) {
      *error = "instruction exceeds last layer root";
    }
    return false;
  }
  size_t traversal_length = instruction.size();
  std::vector<size_t> relevant_layer_heights;
  for (size_t layer_height : layer_heights) {
    if (traversal_length < layer_height) {
      if (error) {
        *error = "instruction should point to chunk boundary";
      }
      return false;
    }
    traversal_length -= layer_height;
    relevant_layer_heights.push_back(layer_height);
    if (traversal_length == 0) {
      break;
    }
  }
  size_t start_index = 0;
  for (size_t layer_height : relevant_layer_heights) {
    size_t end_index = start_index + layer_height;
    size_t offset_multiplier = 0;
    for (size_t i = 0; i < layer_height; ++i) {
      bool instruction_bit = instruction[start_index + i];
      size_t bit_value = 1 - static_cast<size_t>(instruction_bit);
      size_t shift = layer_height - i - 1;
      offset_multiplier += (static_cast<size_t>(1) << shift) * bit_value;
    }
    if (chunk_count % 2 != 0) {
      chunk_count -= 1;
    }
    chunk_count /= ExitNodeCount(layer_height);
    current_index = current_index + offset_multiplier * chunk_count + 1;
    start_index = end_index;
  }
  *out = current_index;
  return true;
}

bool ChunkIndexFromTraversalInstructionWithRecovery(const std::vector<bool>& instruction,
                                                    size_t height,
                                                    size_t* out,
                                                    std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  for (size_t len = instruction.size();; --len) {
    std::vector<bool> slice(instruction.begin(), instruction.begin() + static_cast<long>(len));
    if (ChunkIndexFromTraversalInstruction(slice, height, out, nullptr)) {
      return true;
    }
    if (len == 0) {
      break;
    }
  }
  if (error) {
    *error = "failed to recover chunk index";
  }
  return false;
}

std::vector<uint8_t> TraversalInstructionToBytes(const std::vector<bool>& instruction) {
  std::vector<uint8_t> out;
  out.reserve(instruction.size());
  for (bool bit : instruction) {
    out.push_back(bit ? 1u : 0u);
  }
  return out;
}

bool BytesToTraversalInstruction(const std::vector<uint8_t>& bytes,
                                 std::vector<bool>* out,
                                 std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  out->reserve(bytes.size());
  for (uint8_t byte : bytes) {
    if (byte == 0u) {
      out->push_back(kChunkRight);
    } else if (byte == 1u) {
      out->push_back(kChunkLeft);
    } else {
      if (error) {
        *error = "invalid traversal byte";
      }
      return false;
    }
  }
  return true;
}

}  // namespace grovedb
