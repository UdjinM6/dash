#ifndef GROVEDB_CPP_MERK_NODE_H
#define GROVEDB_CPP_MERK_NODE_H

#include <cstdint>
#include <string>
#include <vector>

namespace grovedb {

enum class TreeFeatureTypeTag : uint8_t {
  kBasic = 0,
  kSum = 1,
  kBigSum = 2,
  kCount = 3,
  kCountSum = 4,
  kProvableCount = 5,
  kProvableCountSum = 6,
};

struct TreeFeatureType {
  TreeFeatureTypeTag tag = TreeFeatureTypeTag::kBasic;
  int64_t sum = 0;
  __int128 big_sum = 0;
  uint64_t count = 0;
  int64_t sum2 = 0;
};

enum class AggregateDataTag : uint8_t {
  kNone = 0,
  kSum = 1,
  kBigSum = 2,
  kCount = 3,
  kCountSum = 4,
  kProvableCount = 5,
  kProvableCountSum = 6,
};

struct AggregateData {
  AggregateDataTag tag = AggregateDataTag::kNone;
  int64_t sum = 0;
  __int128 big_sum = 0;
  uint64_t count = 0;
  int64_t sum2 = 0;
};

struct Link {
  enum class State : uint8_t {
    kReference = 0,
    kModified = 1,
    kUncommitted = 2,
    kLoaded = 3,
  };

  std::vector<uint8_t> key;
  std::vector<uint8_t> hash;
  uint8_t left_height = 0;
  uint8_t right_height = 0;
  AggregateData aggregate;
  State state = State::kLoaded;
};

struct ChildCostInfo {
  uint32_t key_len = 0;
  uint32_t sum_len = 0;
  bool present = false;
};

struct TreeFeatureCostInfo {
  TreeFeatureTypeTag tag = TreeFeatureTypeTag::kBasic;
  uint32_t sum_len = 0;
  bool present = false;
};

struct KvNode {
  std::vector<uint8_t> key;
  std::vector<uint8_t> value;
  std::vector<uint8_t> kv_hash;
  std::vector<uint8_t> value_hash;
  TreeFeatureType feature_type;
};

struct TreeNodeInner {
  bool has_left = false;
  bool has_right = false;
  Link left;
  Link right;
  KvNode kv;
};

bool EncodeTreeFeatureType(const TreeFeatureType& feature,
                           std::vector<uint8_t>* out,
                           std::string* error);
bool DecodeTreeFeatureType(const std::vector<uint8_t>& data,
                           size_t* cursor,
                           TreeFeatureType* out,
                           std::string* error);
bool EncodeAggregateData(const AggregateData& aggregate,
                         std::vector<uint8_t>* out,
                         std::string* error);
bool DecodeAggregateData(const std::vector<uint8_t>& data,
                         size_t* cursor,
                         AggregateData* out,
                         std::string* error);
bool EncodeLink(const Link& link, std::vector<uint8_t>* out, std::string* error);
bool DecodeLink(const std::vector<uint8_t>& data,
                size_t* cursor,
                Link* out,
                std::string* error);
bool EncodeTreeNodeInner(const TreeNodeInner& node,
                         std::vector<uint8_t>* out,
                         std::string* error);
bool ComputeChildCostInfo(const Link& link, ChildCostInfo* out);
bool ComputeTreeFeatureCostInfo(const TreeFeatureType& feature,
                                TreeFeatureCostInfo* out,
                                std::string* error);
bool DecodeTreeNodeInner(const std::vector<uint8_t>& data,
                         TreeNodeInner* out,
                         std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_MERK_NODE_H
