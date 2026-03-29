#ifndef GROVEDB_CPP_ELEMENT_H
#define GROVEDB_CPP_ELEMENT_H

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "value_defined_cost.h"

namespace grovedb {

struct ElementItem {
  std::vector<uint8_t> value;
};

struct ElementItemWithSum {
  std::vector<uint8_t> value;
  int64_t sum = 0;
};

struct ElementSumItem {
  int64_t sum = 0;
};

enum class ReferencePathKind {
  kAbsolute = 0,
  kUpstreamRootHeight = 1,
  kUpstreamRootHeightWithParentPathAddition = 2,
  kUpstreamFromElementHeight = 3,
  kCousin = 4,
  kRemovedCousin = 5,
  kSibling = 6,
};

struct ReferencePathType {
  ReferencePathKind kind = ReferencePathKind::kAbsolute;
  uint8_t height = 0;
  std::vector<std::vector<uint8_t>> path;
  std::vector<uint8_t> key;
};

struct ElementReference {
  ReferencePathType reference_path;
  bool has_max_hop = false;
  uint8_t max_hop = 0;
};

// Decode a GroveDB Element::Item from serialized element bytes.
// Returns false with an error if the format is not supported yet.
bool DecodeItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                ElementItem* out,
                                std::string* error);
bool DecodeItemWithSumItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                           ElementItemWithSum* out,
                                           std::string* error);
bool DecodeSumItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                   ElementSumItem* out,
                                   std::string* error);
bool DecodeReferenceFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     ElementReference* out,
                                     std::string* error);
struct ElementTree {
  std::optional<std::vector<uint8_t>> root_key;
};
bool DecodeTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                ElementTree* out,
                                std::string* error);
struct ElementSumTree {
  std::optional<std::vector<uint8_t>> root_key;
  int64_t sum = 0;
};
bool DecodeSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                   ElementSumTree* out,
                                   std::string* error);
struct ElementCountTree {
  std::optional<std::vector<uint8_t>> root_key;
  uint64_t count = 0;
};
bool DecodeCountTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     ElementCountTree* out,
                                     std::string* error);
struct ElementCountSumTree {
  std::optional<std::vector<uint8_t>> root_key;
  uint64_t count = 0;
  int64_t sum = 0;
};
bool DecodeCountSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                        ElementCountSumTree* out,
                                        std::string* error);
struct ElementBigSumTree {
  std::optional<std::vector<uint8_t>> root_key;
  __int128 sum = 0;
};
bool DecodeBigSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                      ElementBigSumTree* out,
                                      std::string* error);
struct ElementProvableCountTree {
  std::optional<std::vector<uint8_t>> root_key;
  uint64_t count = 0;
};
bool DecodeProvableCountTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                             ElementProvableCountTree* out,
                                             std::string* error);
struct ElementProvableCountSumTree {
  std::optional<std::vector<uint8_t>> root_key;
  uint64_t count = 0;
  int64_t sum = 0;
};
bool DecodeProvableCountSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                                ElementProvableCountSumTree* out,
                                                std::string* error);
bool DecodeElementVariant(const std::vector<uint8_t>& element_bytes,
                          uint64_t* variant,
                          std::string* error);
bool TreeElementHasRootKey(const std::vector<uint8_t>& element_bytes,
                           bool* has_root_key,
                           std::string* error);
bool TreeElementRootKeyLen(const std::vector<uint8_t>& element_bytes,
                           uint32_t* root_key_len,
                           std::string* error);
bool ExtractSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     int64_t* out_sum,
                                     bool* has_sum,
                                     std::string* error);
bool ExtractBigSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                        __int128* out_sum,
                                        bool* has_sum,
                                        std::string* error);
bool ExtractCountValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                       uint64_t* out_count,
                                       bool* has_count,
                                       std::string* error);
bool ExtractCountSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                          uint64_t* out_count,
                                          int64_t* out_sum,
                                          bool* has_count_sum,
                                          std::string* error);
bool ExtractProvableCountSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                                  uint64_t* out_count,
                                                  int64_t* out_sum,
                                                  bool* has_count_sum,
                                                  std::string* error);
bool EncodeItemToElementBytes(const std::vector<uint8_t>& value,
                              std::vector<uint8_t>* out,
                              std::string* error);
bool EncodeItemToElementBytesWithFlags(const std::vector<uint8_t>& value,
                                       const std::vector<uint8_t>& flags,
                                       std::vector<uint8_t>* out,
                                       std::string* error);
bool EncodeSumItemToElementBytes(int64_t sum,
                                 std::vector<uint8_t>* out,
                                 std::string* error);
bool EncodeItemWithSumItemToElementBytes(const std::vector<uint8_t>& value,
                                         int64_t sum,
                                         std::vector<uint8_t>* out,
                                         std::string* error);
bool EncodeReferenceToElementBytes(const ElementReference& reference,
                                   std::vector<uint8_t>* out,
                                   std::string* error);
bool EncodeTreeToElementBytes(std::vector<uint8_t>* out, std::string* error);
bool EncodeTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                         std::vector<uint8_t>* out,
                                         std::string* error);
bool EncodeSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                            int64_t sum,
                                            std::vector<uint8_t>* out,
                                            std::string* error);
bool EncodeBigSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                               __int128 sum,
                                               std::vector<uint8_t>* out,
                                               std::string* error);
bool EncodeCountTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                              uint64_t count,
                                              std::vector<uint8_t>* out,
                                              std::string* error);
bool EncodeCountSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                 uint64_t count,
                                                 int64_t sum,
                                                 std::vector<uint8_t>* out,
                                                 std::string* error);
bool EncodeProvableCountTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                      uint64_t count,
                                                      std::vector<uint8_t>* out,
                                                      std::string* error);
bool EncodeProvableCountSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                         uint64_t count,
                                                         int64_t sum,
                                                         std::vector<uint8_t>* out,
                                                         std::string* error);
bool ValueDefinedCostForSerializedElement(
    const std::vector<uint8_t>& element_bytes,
    std::optional<ValueDefinedCostType>* out,
    std::string* error);
bool ExtractFlagsFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                  std::vector<uint8_t>* flags,
                                  std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_ELEMENT_H
