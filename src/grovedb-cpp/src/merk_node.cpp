#include "merk_node.h"

#include "binary.h"
#include "cost_utils.h"

namespace grovedb {
namespace {

bool EncodeVarintI64(int64_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }
  uint64_t zigzag = (static_cast<uint64_t>(value) << 1) ^
                    static_cast<uint64_t>(value >> 63);
  EncodeVarintU64(zigzag, out);
  return true;
}

bool EncodeI128BE(__int128 value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }
  for (int i = 15; i >= 0; --i) {
    out->push_back(static_cast<uint8_t>((static_cast<unsigned __int128>(value) >> (i * 8)) &
                                        0xff));
  }
  return true;
}

bool ReadI128BE(const std::vector<uint8_t>& data,
                size_t* cursor,
                __int128* out,
                std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor + 16 > data.size()) {
    if (error) {
      *error = "i128 truncated";
    }
    return false;
  }
  unsigned __int128 value = 0;
  for (int i = 0; i < 16; ++i) {
    value = (value << 8) | data[*cursor + static_cast<size_t>(i)];
  }
  *cursor += 16;
  *out = static_cast<__int128>(value);
  return true;
}

size_t EncodedTreeFeatureTypeSize(const TreeFeatureType& feature) {
  switch (feature.tag) {
    case TreeFeatureTypeTag::kBasic:
      return 1;
    case TreeFeatureTypeTag::kSum:
      return 1 + VarintLenU64((static_cast<uint64_t>(feature.sum) << 1) ^
                              static_cast<uint64_t>(feature.sum >> 63));
    case TreeFeatureTypeTag::kBigSum:
      return 1 + 16;
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      return 1 + VarintLenU64(feature.count);
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      return 1 + VarintLenU64(feature.count) +
             VarintLenU64((static_cast<uint64_t>(feature.sum) << 1) ^
                          static_cast<uint64_t>(feature.sum >> 63));
  }
  return 1;
}

size_t EncodedAggregateDataSize(const AggregateData& aggregate) {
  switch (aggregate.tag) {
    case AggregateDataTag::kNone:
      return 1;
    case AggregateDataTag::kSum:
      return 1 + VarintLenU64((static_cast<uint64_t>(aggregate.sum) << 1) ^
                              static_cast<uint64_t>(aggregate.sum >> 63));
    case AggregateDataTag::kBigSum:
      return 1 + 16;
    case AggregateDataTag::kCount:
    case AggregateDataTag::kProvableCount:
      return 1 + VarintLenU64(aggregate.count);
    case AggregateDataTag::kCountSum:
    case AggregateDataTag::kProvableCountSum:
      return 1 + VarintLenU64(aggregate.count) +
             VarintLenU64((static_cast<uint64_t>(aggregate.sum2) << 1) ^
                          static_cast<uint64_t>(aggregate.sum2 >> 63));
  }
  return 1;
}

size_t EncodedLinkSize(const Link& link) {
  return 1 + link.key.size() + 32 + 2 + EncodedAggregateDataSize(link.aggregate);
}

}  // namespace

bool EncodeTreeFeatureType(const TreeFeatureType& feature,
                           std::vector<uint8_t>* out,
                           std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "feature type output is null";
    }
    return false;
  }
  out->push_back(static_cast<uint8_t>(feature.tag));
  switch (feature.tag) {
    case TreeFeatureTypeTag::kBasic:
      return true;
    case TreeFeatureTypeTag::kSum:
      return EncodeVarintI64(feature.sum, out);
    case TreeFeatureTypeTag::kBigSum:
      return EncodeI128BE(feature.big_sum, out);
    case TreeFeatureTypeTag::kCount:
      EncodeVarintU64(feature.count, out);
      return true;
    case TreeFeatureTypeTag::kCountSum:
      EncodeVarintU64(feature.count, out);
      return EncodeVarintI64(feature.sum, out);
    case TreeFeatureTypeTag::kProvableCount:
      EncodeVarintU64(feature.count, out);
      return true;
    case TreeFeatureTypeTag::kProvableCountSum:
      EncodeVarintU64(feature.count, out);
      return EncodeVarintI64(feature.sum, out);
  }
  if (error) {
    *error = "unsupported tree feature type";
  }
  return false;
}

bool DecodeTreeFeatureType(const std::vector<uint8_t>& data,
                           size_t* cursor,
                           TreeFeatureType* out,
                           std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "feature type output is null";
    }
    return false;
  }
  if (*cursor >= data.size()) {
    if (error) {
      *error = "feature type truncated";
    }
    return false;
  }
  uint8_t tag = data[*cursor];
  *cursor += 1;
  out->tag = static_cast<TreeFeatureTypeTag>(tag);
  switch (out->tag) {
    case TreeFeatureTypeTag::kBasic:
      return true;
    case TreeFeatureTypeTag::kSum:
      return ReadVarintI64(data, cursor, &out->sum, error);
    case TreeFeatureTypeTag::kBigSum:
      return ReadI128BE(data, cursor, &out->big_sum, error);
    case TreeFeatureTypeTag::kCount:
      return ReadVarintU64(data, cursor, &out->count, error);
    case TreeFeatureTypeTag::kCountSum:
      if (!ReadVarintU64(data, cursor, &out->count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &out->sum, error)) {
        return false;
      }
      out->sum2 = out->sum;
      return true;
    case TreeFeatureTypeTag::kProvableCount:
      return ReadVarintU64(data, cursor, &out->count, error);
    case TreeFeatureTypeTag::kProvableCountSum:
      if (!ReadVarintU64(data, cursor, &out->count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &out->sum, error)) {
        return false;
      }
      out->sum2 = out->sum;
      return true;
  }
  if (error) {
    *error = "unsupported tree feature type";
  }
  return false;
}

bool EncodeAggregateData(const AggregateData& aggregate,
                         std::vector<uint8_t>* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "aggregate output is null";
    }
    return false;
  }
  out->push_back(static_cast<uint8_t>(aggregate.tag));
  switch (aggregate.tag) {
    case AggregateDataTag::kNone:
      return true;
    case AggregateDataTag::kSum:
      return EncodeVarintI64(aggregate.sum, out);
    case AggregateDataTag::kBigSum:
      return EncodeI128BE(aggregate.big_sum, out);
    case AggregateDataTag::kCount:
      EncodeVarintU64(aggregate.count, out);
      return true;
    case AggregateDataTag::kCountSum:
      EncodeVarintU64(aggregate.count, out);
      return EncodeVarintI64(aggregate.sum, out);
    case AggregateDataTag::kProvableCount:
      EncodeVarintU64(aggregate.count, out);
      return true;
    case AggregateDataTag::kProvableCountSum:
      EncodeVarintU64(aggregate.count, out);
      return EncodeVarintI64(aggregate.sum, out);
  }
  if (error) {
    *error = "unsupported aggregate data";
  }
  return false;
}

bool DecodeAggregateData(const std::vector<uint8_t>& data,
                         size_t* cursor,
                         AggregateData* out,
                         std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "aggregate output is null";
    }
    return false;
  }
  if (*cursor >= data.size()) {
    if (error) {
      *error = "aggregate data truncated";
    }
    return false;
  }
  uint8_t tag = data[*cursor];
  *cursor += 1;
  out->tag = static_cast<AggregateDataTag>(tag);
  switch (out->tag) {
    case AggregateDataTag::kNone:
      return true;
    case AggregateDataTag::kSum:
      return ReadVarintI64(data, cursor, &out->sum, error);
    case AggregateDataTag::kBigSum:
      return ReadI128BE(data, cursor, &out->big_sum, error);
    case AggregateDataTag::kCount:
      return ReadVarintU64(data, cursor, &out->count, error);
    case AggregateDataTag::kCountSum:
      if (!ReadVarintU64(data, cursor, &out->count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &out->sum, error)) {
        return false;
      }
      out->sum2 = out->sum;
      return true;
    case AggregateDataTag::kProvableCount:
      return ReadVarintU64(data, cursor, &out->count, error);
    case AggregateDataTag::kProvableCountSum:
      if (!ReadVarintU64(data, cursor, &out->count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &out->sum, error)) {
        return false;
      }
      out->sum2 = out->sum;
      return true;
  }
  if (error) {
    *error = "unsupported aggregate data";
  }
  return false;
}

bool EncodeLink(const Link& link, std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "link output is null";
    }
    return false;
  }
  if (link.key.size() > 255) {
    if (error) {
      *error = "link key too long";
    }
    return false;
  }
  if (link.hash.size() != 32) {
    if (error) {
      *error = "link hash length mismatch";
    }
    return false;
  }
  out->push_back(static_cast<uint8_t>(link.key.size()));
  out->insert(out->end(), link.key.begin(), link.key.end());
  out->insert(out->end(), link.hash.begin(), link.hash.end());
  out->push_back(link.left_height);
  out->push_back(link.right_height);
  return EncodeAggregateData(link.aggregate, out, error);
}

bool DecodeLink(const std::vector<uint8_t>& data,
                size_t* cursor,
                Link* out,
                std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "link output is null";
    }
    return false;
  }
  if (*cursor >= data.size()) {
    if (error) {
      *error = "link truncated";
    }
    return false;
  }
  uint8_t key_len = data[*cursor];
  *cursor += 1;
  if (!ReadBytes(data, cursor, key_len, &out->key, error)) {
    return false;
  }
  if (!ReadBytes(data, cursor, 32, &out->hash, error)) {
    return false;
  }
  if (*cursor + 2 > data.size()) {
    if (error) {
      *error = "link heights truncated";
    }
    return false;
  }
  out->left_height = data[*cursor];
  out->right_height = data[*cursor + 1];
  *cursor += 2;
  if (!DecodeAggregateData(data, cursor, &out->aggregate, error)) {
    return false;
  }
  out->state = Link::State::kLoaded;
  return true;
}

bool EncodeTreeNodeInner(const TreeNodeInner& node,
                         std::vector<uint8_t>* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "node output is null";
    }
    return false;
  }
  out->clear();
  size_t reserve_size = 1 + 1 + 64 + node.kv.value.size() +
                        EncodedTreeFeatureTypeSize(node.kv.feature_type);
  if (node.has_left) {
    reserve_size += EncodedLinkSize(node.left);
  }
  if (node.has_right) {
    reserve_size += EncodedLinkSize(node.right);
  }
  out->reserve(reserve_size);
  out->push_back(node.has_left ? 1 : 0);
  if (node.has_left) {
    if (!EncodeLink(node.left, out, error)) {
      return false;
    }
  }
  out->push_back(node.has_right ? 1 : 0);
  if (node.has_right) {
    if (!EncodeLink(node.right, out, error)) {
      return false;
    }
  }
  if (!EncodeTreeFeatureType(node.kv.feature_type, out, error)) {
    return false;
  }
  if (node.kv.kv_hash.size() != 32 || node.kv.value_hash.size() != 32) {
    if (error) {
      *error = "kv hash length mismatch";
    }
    return false;
  }
  out->insert(out->end(), node.kv.kv_hash.begin(), node.kv.kv_hash.end());
  out->insert(out->end(), node.kv.value_hash.begin(), node.kv.value_hash.end());
  out->insert(out->end(), node.kv.value.begin(), node.kv.value.end());
  return true;
}

bool DecodeTreeNodeInner(const std::vector<uint8_t>& data,
                         TreeNodeInner* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "node output is null";
    }
    return false;
  }
  size_t cursor = 0;
  if (cursor >= data.size()) {
    if (error) {
      *error = "node truncated";
    }
    return false;
  }
  out->has_left = data[cursor++] != 0;
  if (out->has_left) {
    if (!DecodeLink(data, &cursor, &out->left, error)) {
      return false;
    }
  }
  if (cursor >= data.size()) {
    if (error) {
      *error = "node truncated";
    }
    return false;
  }
  out->has_right = data[cursor++] != 0;
  if (out->has_right) {
    if (!DecodeLink(data, &cursor, &out->right, error)) {
      return false;
    }
  }
  if (!DecodeTreeFeatureType(data, &cursor, &out->kv.feature_type, error)) {
    return false;
  }
  if (!ReadBytes(data, &cursor, 32, &out->kv.kv_hash, error)) {
    return false;
  }
  if (!ReadBytes(data, &cursor, 32, &out->kv.value_hash, error)) {
    return false;
  }
  if (cursor > data.size()) {
    if (error) {
      *error = "node truncated";
    }
    return false;
  }
  out->kv.value.assign(data.begin() + static_cast<long>(cursor), data.end());
  return true;
}

bool ComputeChildCostInfo(const Link& link, ChildCostInfo* out) {
  if (out == nullptr) {
    return false;
  }
  out->present = true;
  out->key_len = static_cast<uint32_t>(link.key.size());
  switch (link.aggregate.tag) {
    case AggregateDataTag::kSum:
      out->sum_len = 8;
      break;
    case AggregateDataTag::kCount:
      out->sum_len = 8;
      break;
    case AggregateDataTag::kProvableCount:
      out->sum_len = 8;
      break;
    case AggregateDataTag::kBigSum:
      out->sum_len = 16;
      break;
    case AggregateDataTag::kCountSum:
      out->sum_len = 16;
      break;
    case AggregateDataTag::kProvableCountSum:
      out->sum_len = 16;
      break;
    case AggregateDataTag::kNone:
      out->sum_len = 0;
      break;
  }
  return true;
}

bool ComputeTreeFeatureCostInfo(const TreeFeatureType& feature,
                                TreeFeatureCostInfo* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "feature cost output is null";
    }
    return false;
  }
  out->present = true;
  out->tag = feature.tag;
  switch (feature.tag) {
    case TreeFeatureTypeTag::kBasic:
      out->present = false;
      out->sum_len = 0;
      return true;
    case TreeFeatureTypeTag::kSum:
      out->sum_len = 8;
      return true;
    case TreeFeatureTypeTag::kCount:
      out->sum_len = 8;
      return true;
    case TreeFeatureTypeTag::kProvableCount:
      out->sum_len = 8;
      return true;
    case TreeFeatureTypeTag::kBigSum:
      out->sum_len = 16;
      return true;
    case TreeFeatureTypeTag::kCountSum:
      out->sum_len = 16;
      return true;
    case TreeFeatureTypeTag::kProvableCountSum:
      out->sum_len = 16;
      return true;
  }
  if (error) {
    *error = "unsupported tree feature type";
  }
  return false;
}

}  // namespace grovedb
