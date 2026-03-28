#ifndef GROVEDB_CPP_CHUNK_DEPTH_H
#define GROVEDB_CPP_CHUNK_DEPTH_H

#include <cstdint>
#include <vector>

namespace grovedb {

uint8_t CalculateMaxTreeDepthFromCount(uint64_t count);
std::vector<uint8_t> CalculateChunkDepths(uint8_t tree_depth, uint8_t max_depth);
std::vector<uint8_t> CalculateChunkDepthsWithMinimum(uint8_t tree_depth,
                                                     uint8_t max_depth,
                                                     uint8_t min_depth);

}  // namespace grovedb

#endif  // GROVEDB_CPP_CHUNK_DEPTH_H
