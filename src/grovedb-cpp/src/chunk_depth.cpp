#include "chunk_depth.h"

#include <stdexcept>

namespace grovedb {

uint8_t CalculateMaxTreeDepthFromCount(uint64_t count) {
  if (count == 0) {
    return 0;
  }
  uint64_t f_prev = 1;
  uint64_t f_curr = 2;
  uint8_t height = 1;
  while (true) {
    uint64_t f_next = f_prev + f_curr;
    uint64_t next_min_nodes = f_next - 1;
    if (next_min_nodes > count) {
      return height;
    }
    height = static_cast<uint8_t>(height + 1);
    f_prev = f_curr;
    f_curr = f_next;
    if (height >= 92) {
      return height;
    }
  }
}

std::vector<uint8_t> CalculateChunkDepths(uint8_t tree_depth, uint8_t max_depth) {
  if (max_depth == 0) {
    throw std::invalid_argument("max_depth must be > 0");
  }
  if (tree_depth == 0) {
    return {0};
  }
  if (tree_depth <= max_depth) {
    return {tree_depth};
  }
  uint32_t num_chunks = (tree_depth + max_depth - 1) / max_depth;
  uint32_t base_depth = tree_depth / num_chunks;
  uint32_t remainder = tree_depth % num_chunks;
  std::vector<uint8_t> chunks;
  chunks.reserve(num_chunks);
  for (uint32_t i = 0; i < num_chunks; ++i) {
    chunks.push_back(static_cast<uint8_t>(base_depth + (i < remainder ? 1 : 0)));
  }
  return chunks;
}

std::vector<uint8_t> CalculateChunkDepthsWithMinimum(uint8_t tree_depth,
                                                     uint8_t max_depth,
                                                     uint8_t min_depth) {
  if (max_depth == 0) {
    throw std::invalid_argument("max_depth must be > 0");
  }
  if (min_depth == 0) {
    throw std::invalid_argument("min_depth must be > 0");
  }
  if (min_depth > max_depth) {
    throw std::invalid_argument("min_depth must be <= max_depth");
  }
  if (tree_depth <= max_depth) {
    return {tree_depth};
  }
  uint32_t num_chunks = (tree_depth + max_depth - 1) / max_depth;
  std::vector<uint8_t> chunks;
  chunks.reserve(num_chunks);
  uint8_t remaining = tree_depth;
  for (uint32_t i = 0; i < num_chunks; ++i) {
    uint32_t chunks_left = num_chunks - i;
    uint8_t base = static_cast<uint8_t>(remaining / chunks_left);
    bool has_extra = (remaining % chunks_left) > 0;
    uint8_t chunk = static_cast<uint8_t>(base + (has_extra ? 1 : 0));
    if (i == 0) {
      if (chunk < min_depth) {
        chunk = min_depth;
      }
      if (chunk > max_depth) {
        chunk = max_depth;
      }
    } else if (chunk > max_depth) {
      chunk = max_depth;
    }
    chunks.push_back(chunk);
    remaining = static_cast<uint8_t>(remaining - chunk);
  }
  return chunks;
}

}  // namespace grovedb
