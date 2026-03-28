#ifndef GROVEDB_CPP_MERK_COSTS_H
#define GROVEDB_CPP_MERK_COSTS_H

#include <cstdint>

#include "cost_utils.h"
#include "merk_node.h"

namespace grovedb {

constexpr uint32_t kHashLengthU32 = 32;
constexpr uint32_t kHashLengthU32X2 = 64;

inline uint32_t FeatureLenForTag(TreeFeatureTypeTag tag) {
  switch (tag) {
    case TreeFeatureTypeTag::kBasic:
      return 1;
    case TreeFeatureTypeTag::kSum:
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      return 9;
    case TreeFeatureTypeTag::kBigSum:
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      return 17;
  }
  return 1;
}

inline uint32_t FeatureCostForTag(TreeFeatureTypeTag tag) {
  switch (tag) {
    case TreeFeatureTypeTag::kBasic:
      return 0;
    case TreeFeatureTypeTag::kSum:
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      return 8;
    case TreeFeatureTypeTag::kBigSum:
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      return 16;
  }
  return 0;
}

inline uint32_t EncodedLinkSize(uint32_t key_len, TreeFeatureTypeTag tag) {
  return key_len + kHashLengthU32 + 4 + FeatureCostForTag(tag);
}

inline uint32_t NodeKeyByteCostSize(uint32_t key_len) {
  return kHashLengthU32 + key_len + RequiredSpaceU32(key_len + kHashLengthU32);
}

inline uint32_t NodeValueByteCostSize(uint32_t key_len,
                                      uint32_t raw_value_len,
                                      TreeFeatureTypeTag tag) {
  uint32_t feature_len = FeatureLenForTag(tag);
  uint32_t value_size = raw_value_len + kHashLengthU32X2 + feature_len;
  uint32_t parent_to_child_cost = EncodedLinkSize(key_len, tag);
  return value_size + RequiredSpaceU32(value_size) + parent_to_child_cost;
}

inline uint32_t NodeByteCostSizeForKeyAndRawValueLengths(uint32_t key_len,
                                                         uint32_t raw_value_len,
                                                         TreeFeatureTypeTag tag) {
  uint32_t node_value_size = NodeValueByteCostSize(key_len, raw_value_len, tag);
  uint32_t node_key_size = NodeKeyByteCostSize(key_len);
  return node_value_size + node_key_size;
}

inline uint32_t LayeredNodeByteCostSizeForKeyAndValueLengths(uint32_t key_len,
                                                             uint32_t value_len,
                                                             TreeFeatureTypeTag tag) {
  uint32_t feature_len = FeatureLenForTag(tag);
  uint32_t node_value_size = value_len + feature_len + kHashLengthU32 + 2;
  uint32_t node_key_size = kHashLengthU32 + key_len +
                           RequiredSpaceU32(key_len + kHashLengthU32);
  uint32_t node_size = node_value_size + node_key_size;
  uint32_t parent_to_child_cost = EncodedLinkSize(key_len, tag);
  return node_size + parent_to_child_cost;
}

inline uint32_t LayeredValueByteCostSizeForKeyAndValueLengths(uint32_t key_len,
                                                              uint32_t value_len,
                                                              TreeFeatureTypeTag tag) {
  uint32_t feature_len = FeatureLenForTag(tag);
  uint32_t node_value_size = value_len + feature_len + kHashLengthU32 + 2;
  uint32_t parent_to_child_cost = EncodedLinkSize(key_len, tag);
  return node_value_size + parent_to_child_cost;
}

inline uint32_t ValueByteCostSizeForKeyAndValueLengths(uint32_t key_len,
                                                       uint32_t value_len,
                                                       TreeFeatureTypeTag tag) {
  (void)key_len;
  uint32_t feature_len = FeatureLenForTag(tag);
  uint32_t value_size = value_len + kHashLengthU32X2 + feature_len;
  return value_size + RequiredSpaceU32(value_size);
}

inline uint32_t ValueByteCostSizeForKeyAndRawValueLengths(uint32_t key_len,
                                                          uint32_t raw_value_len,
                                                          TreeFeatureTypeTag tag) {
  return ValueByteCostSizeForKeyAndValueLengths(
      key_len, raw_value_len + kHashLengthU32X2, tag);
}

}  // namespace grovedb

#endif  // GROVEDB_CPP_MERK_COSTS_H
