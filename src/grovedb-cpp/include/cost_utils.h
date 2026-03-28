#ifndef GROVEDB_CPP_COST_UTILS_H
#define GROVEDB_CPP_COST_UTILS_H

#include <cstdint>

namespace grovedb {

inline uint32_t VarintLenU64(uint64_t value) {
  if (value < (1ULL << 7)) {
    return 1;
  }
  if (value < (1ULL << 14)) {
    return 2;
  }
  if (value < (1ULL << 21)) {
    return 3;
  }
  if (value < (1ULL << 28)) {
    return 4;
  }
  if (value < (1ULL << 35)) {
    return 5;
  }
  if (value < (1ULL << 42)) {
    return 6;
  }
  if (value < (1ULL << 49)) {
    return 7;
  }
  if (value < (1ULL << 56)) {
    return 8;
  }
  if (value < (1ULL << 63)) {
    return 9;
  }
  return 10;
}

inline uint32_t RequiredSpaceU32(uint32_t value) { return VarintLenU64(value); }

inline uint64_t PaidLen(uint64_t len) { return len + VarintLenU64(len); }

}  // namespace grovedb

#endif  // GROVEDB_CPP_COST_UTILS_H
