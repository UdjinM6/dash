#include "element.h"

#include "binary.h"
#include "cost_utils.h"

#include <cstddef>

namespace grovedb {

namespace {

constexpr uint32_t kTreeCostSize = 3;
constexpr uint32_t kSumItemCostSize = 11;
constexpr uint32_t kSumTreeCostSize = 12;
constexpr uint32_t kBigSumTreeCostSize = 19;
constexpr uint32_t kCountTreeCostSize = 12;
constexpr uint32_t kCountSumTreeCostSize = 21;

bool DecodeVarint(const std::vector<uint8_t>& bytes,
                  size_t* cursor,
                  uint64_t* out,
                  std::string* error) {
  return ReadBincodeVarintU64(bytes, cursor, out, error);
}

bool SkipVarint(const std::vector<uint8_t>& bytes, size_t* cursor, std::string* error) {
  if (cursor == nullptr) {
    if (error) {
      *error = "skip cursor is null";
    }
    return false;
  }
  if (*cursor >= bytes.size()) {
    if (error) {
      *error = "varint truncated";
    }
    return false;
  }
  uint8_t tag = bytes[*cursor];
  (*cursor)++;
  if (tag <= 250) {
    return true;
  }
  size_t byte_len = 0;
  if (tag == 251) {
    byte_len = 2;
  } else if (tag == 252) {
    byte_len = 4;
  } else if (tag == 253) {
    byte_len = 8;
  } else if (tag == 254) {
    byte_len = 16;
  } else {
    if (error) {
      *error = "invalid bincode varint tag";
    }
    return false;
  }
  if (*cursor + byte_len > bytes.size()) {
    if (error) {
      *error = "varint truncated";
    }
    return false;
  }
  *cursor += byte_len;
  return true;
}

bool DecodeElementVariantInternal(const std::vector<uint8_t>& element_bytes,
                                  uint64_t* variant,
                                  std::string* error) {
  if (variant == nullptr) {
    if (error) {
      *error = "variant output is null";
    }
    return false;
  }
  size_t cursor = 0;
  return DecodeVarint(element_bytes, &cursor, variant, error);
}

uint64_t ZigZagEncodeI64(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^
         static_cast<uint64_t>(value >> 63);
}

int64_t ZigZagDecodeI64(uint64_t value) {
  return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

unsigned __int128 ZigZagEncodeI128(__int128 value) {
  return (static_cast<unsigned __int128>(value) << 1) ^
         static_cast<unsigned __int128>(value >> 127);
}

__int128 ZigZagDecodeI128(unsigned __int128 value) {
  unsigned __int128 magnitude = value >> 1;
  if ((value & 1) == 0) {
    return static_cast<__int128>(magnitude);
  }
  return -static_cast<__int128>(magnitude) - 1;
}

void AppendVarint(uint64_t value, std::vector<uint8_t>* out) {
  EncodeBincodeVarintU64(value, out);
}

void AppendVarintU128(unsigned __int128 value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  if (value <= 250) {
    out->push_back(static_cast<uint8_t>(value));
    return;
  }
  if (value <= 0xFFFFu) {
    out->push_back(251);
    EncodeU16BE(static_cast<uint16_t>(value), out);
    return;
  }
  if (value <= 0xFFFFFFFFu) {
    out->push_back(252);
    EncodeU32BE(static_cast<uint32_t>(value), out);
    return;
  }
  if (value <= 0xFFFFFFFFFFFFFFFFull) {
    out->push_back(253);
    EncodeU64BE(static_cast<uint64_t>(value), out);
    return;
  }
  out->push_back(254);
  for (int i = 15; i >= 0; --i) {
    out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
  }
}

bool DecodeByteVector(const std::vector<uint8_t>& bytes,
                      size_t* cursor,
                      std::vector<uint8_t>* out,
                      std::string* error) {
  uint64_t length = 0;
  if (!DecodeVarint(bytes, cursor, &length, error)) {
    return false;
  }
  if (*cursor > bytes.size() || length > static_cast<uint64_t>(bytes.size() - *cursor)) {
    if (error) {
      *error = "vector length exceeds input size";
    }
    return false;
  }
  out->assign(bytes.begin() + static_cast<long>(*cursor),
              bytes.begin() + static_cast<long>(*cursor + length));
  *cursor += static_cast<size_t>(length);
  return true;
}

bool DecodeVarintI64(const std::vector<uint8_t>& bytes,
                     size_t* cursor,
                     int64_t* out,
                     std::string* error) {
  uint64_t raw = 0;
  if (!DecodeVarint(bytes, cursor, &raw, error)) {
    return false;
  }
  if (out) {
    *out = ZigZagDecodeI64(raw);
  }
  return true;
}

bool DecodeVarintI128(const std::vector<uint8_t>& bytes,
                      size_t* cursor,
                      __int128* out,
                      std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "decode cursor or output is null";
    }
    return false;
  }
  if (*cursor >= bytes.size()) {
    if (error) {
      *error = "varint truncated";
    }
    return false;
  }
  uint8_t tag = bytes[*cursor];
  (*cursor)++;
  unsigned __int128 raw = 0;
  if (tag <= 250) {
    raw = tag;
  } else {
    size_t byte_len = 0;
    if (tag == 251) {
      byte_len = 2;
    } else if (tag == 252) {
      byte_len = 4;
    } else if (tag == 253) {
      byte_len = 8;
    } else if (tag == 254) {
      byte_len = 16;
    } else {
      if (error) {
        *error = "invalid bincode varint tag";
      }
      return false;
    }
    if (*cursor + byte_len > bytes.size()) {
      if (error) {
        *error = "varint truncated";
      }
      return false;
    }
    for (size_t i = 0; i < byte_len; ++i) {
      raw = (raw << 8) | bytes[*cursor + i];
    }
    *cursor += byte_len;
  }
  *out = ZigZagDecodeI128(raw);
  return true;
}

bool DecodeOptionBytes(const std::vector<uint8_t>& bytes,
                       size_t* cursor,
                       std::vector<uint8_t>* out,
                       std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "option output is null";
    }
    return false;
  }
  uint64_t option = 0;
  if (!DecodeVarint(bytes, cursor, &option, error)) {
    return false;
  }
  if (option == 0) {
    out->clear();
    return true;
  }
  if (option != 1) {
    if (error) {
      *error = "invalid option tag";
    }
    return false;
  }
  return DecodeByteVector(bytes, cursor, out, error);
}

bool DecodeFlagsBytes(const std::vector<uint8_t>& bytes,
                      size_t* cursor,
                      std::vector<uint8_t>* out,
                      std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "flags output is null";
    }
    return false;
  }
  uint64_t option = 0;
  if (!DecodeVarint(bytes, cursor, &option, error)) {
    return false;
  }
  if (option == 0) {
    out->clear();
    return true;
  }
  if (option != 1) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  return DecodeByteVector(bytes, cursor, out, error);
}

bool DecodeByteVectorList(const std::vector<uint8_t>& bytes,
                          size_t* cursor,
                          std::vector<std::vector<uint8_t>>* out,
                          std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  uint64_t count = 0;
  if (!DecodeVarint(bytes, cursor, &count, error)) {
    return false;
  }
  if (count > static_cast<uint64_t>(bytes.size())) {
    if (error) {
      *error = "reference path length too large";
    }
    return false;
  }
  out->clear();
  out->reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    std::vector<uint8_t> segment;
    if (!DecodeByteVector(bytes, cursor, &segment, error)) {
      return false;
    }
    out->push_back(std::move(segment));
  }
  return true;
}

void AppendByteVector(const std::vector<uint8_t>& value, std::vector<uint8_t>* out) {
  AppendVarint(static_cast<uint64_t>(value.size()), out);
  out->insert(out->end(), value.begin(), value.end());
}

void AppendByteVectorList(const std::vector<std::vector<uint8_t>>& value,
                          std::vector<uint8_t>* out) {
  AppendVarint(static_cast<uint64_t>(value.size()), out);
  for (const auto& segment : value) {
    AppendByteVector(segment, out);
  }
}

}  // namespace

bool ValueDefinedCostForSerializedElement(
    const std::vector<uint8_t>& element_bytes,
    std::optional<ValueDefinedCostType>* out,
    std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "value defined cost output is null";
    }
    return false;
  }
  out->reset();
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }

  auto decode_flags_len = [&](uint32_t* flags_len, bool* has_flags) -> bool {
    if (flags_len == nullptr || has_flags == nullptr) {
      if (error) {
        *error = "flags output is null";
      }
      return false;
    }
    uint64_t option = 0;
    if (!DecodeVarint(element_bytes, &cursor, &option, error)) {
      return false;
    }
    if (option == 0) {
      *flags_len = 0;
      *has_flags = false;
      return true;
    }
    if (option != 1) {
      if (error) {
        *error = "invalid flags option tag";
      }
      return false;
    }
    uint64_t flags_len_u64 = 0;
    if (!DecodeVarint(element_bytes, &cursor, &flags_len_u64, error)) {
      return false;
    }
    if (cursor > element_bytes.size() || flags_len_u64 > static_cast<uint64_t>(element_bytes.size() - cursor)) {
      if (error) {
        *error = "flags length exceeds input size";
      }
      return false;
    }
    cursor += static_cast<size_t>(flags_len_u64);
    *flags_len = static_cast<uint32_t>(flags_len_u64);
    *has_flags = true;
    return true;
  };

  uint32_t flags_len = 0;
  bool has_flags = false;
  switch (variant) {
    case 2: {  // Tree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 4: {  // SumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kSumTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 5: {  // BigSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kBigSumTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 6: {  // CountTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kCountTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 7: {  // CountSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kCountSumTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 8: {  // ProvableCountTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kCountTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 10: {  // ProvableCountSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kLayered;
      cost.cost =
          kCountSumTreeCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 3: {  // SumItem
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kSpecialized;
      cost.cost =
          kSumItemCostSize + flags_len + (has_flags ? RequiredSpaceU32(flags_len) : 0);
      *out = cost;
      return true;
    }
    case 9: {  // ItemWithSumItem
      std::vector<uint8_t> item;
      if (!DecodeByteVector(element_bytes, &cursor, &item, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!decode_flags_len(&flags_len, &has_flags)) {
        return false;
      }
      uint32_t item_len = static_cast<uint32_t>(item.size());
      ValueDefinedCostType cost;
      cost.kind = ValueDefinedCostType::Kind::kSpecialized;
      cost.cost = kSumItemCostSize + flags_len +
                  (has_flags ? RequiredSpaceU32(flags_len) : 0) + item_len +
                  RequiredSpaceU32(item_len);
      *out = cost;
      return true;
    }
    default:
      return true;
  }
}

bool ExtractFlagsFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                  std::vector<uint8_t>* flags,
                                  std::string* error) {
  if (flags == nullptr) {
    if (error) {
      *error = "flags output is null";
    }
    return false;
  }
  flags->clear();
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  auto finalize = [&]() -> bool {
    if (cursor != element_bytes.size()) {
      if (error) {
        *error = "extra bytes after element decode";
      }
      return false;
    }
    return true;
  };
  switch (variant) {
    case 0: {  // Item
      std::vector<uint8_t> value;
      if (!DecodeByteVector(element_bytes, &cursor, &value, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 1: {  // Reference
      uint64_t path_kind = 0;
      if (!DecodeVarint(element_bytes, &cursor, &path_kind, error)) {
        return false;
      }
      switch (static_cast<ReferencePathKind>(path_kind)) {
        case ReferencePathKind::kAbsolute: {
          std::vector<std::vector<uint8_t>> ignored_path;
          if (!DecodeByteVectorList(element_bytes, &cursor, &ignored_path, error)) {
            return false;
          }
          break;
        }
        case ReferencePathKind::kUpstreamRootHeight:
        case ReferencePathKind::kUpstreamRootHeightWithParentPathAddition:
        case ReferencePathKind::kUpstreamFromElementHeight: {
          uint64_t height = 0;
          if (!DecodeVarint(element_bytes, &cursor, &height, error)) {
            return false;
          }
          if (height > 0xff) {
            if (error) {
              *error = "reference height out of range";
            }
            return false;
          }
          std::vector<std::vector<uint8_t>> ignored_path;
          if (!DecodeByteVectorList(element_bytes, &cursor, &ignored_path, error)) {
            return false;
          }
          break;
        }
        case ReferencePathKind::kCousin:
        case ReferencePathKind::kSibling: {
          std::vector<uint8_t> ignored_key;
          if (!DecodeByteVector(element_bytes, &cursor, &ignored_key, error)) {
            return false;
          }
          break;
        }
        case ReferencePathKind::kRemovedCousin: {
          std::vector<std::vector<uint8_t>> ignored_path;
          if (!DecodeByteVectorList(element_bytes, &cursor, &ignored_path, error)) {
            return false;
          }
          break;
        }
        default:
          if (error) {
            *error = "reference path kind out of range";
          }
          return false;
      }

      uint64_t hop_option = 0;
      if (!DecodeVarint(element_bytes, &cursor, &hop_option, error)) {
        return false;
      }
      if (hop_option == 1) {
        uint64_t hop = 0;
        if (!DecodeVarint(element_bytes, &cursor, &hop, error)) {
          return false;
        }
        if (hop > 0xff) {
          if (error) {
            *error = "max hop out of range";
          }
          return false;
        }
      } else if (hop_option != 0) {
        if (error) {
          *error = "invalid max hop option tag";
        }
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 2: {  // Tree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 3: {  // SumItem
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 4: {  // SumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 5: {  // BigSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 6: {  // CountTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 7: {  // CountSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 8: {  // ProvableCountTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 9: {  // ItemWithSumItem
      std::vector<uint8_t> value;
      if (!DecodeByteVector(element_bytes, &cursor, &value, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    case 10: {  // ProvableCountSumTree
      std::vector<uint8_t> root_key;
      if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!SkipVarint(element_bytes, &cursor, error)) {
        return false;
      }
      if (!DecodeFlagsBytes(element_bytes, &cursor, flags, error)) {
        return false;
      }
      return finalize();
    }
    default:
      if (error) {
        *error = "unsupported element variant";
      }
      return false;
  }
}

bool DecodeElementVariant(const std::vector<uint8_t>& element_bytes,
                          uint64_t* variant,
                          std::string* error) {
  if (!DecodeElementVariantInternal(element_bytes, variant, error)) {
    return false;
  }
  switch (*variant) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
      return true;
    default:
      if (error) {
        *error = "unsupported element variant";
      }
      return false;
  }
}

bool TreeElementHasRootKey(const std::vector<uint8_t>& element_bytes,
                           bool* has_root_key,
                           std::string* error) {
  if (has_root_key == nullptr) {
    if (error) {
      *error = "root key output is null";
    }
    return false;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariantInternal(element_bytes, &variant, error)) {
    return false;
  }
  switch (variant) {
    case 2:   // Tree
    case 4:   // SumTree
    case 5:   // BigSumTree
    case 6:   // CountTree
    case 7:   // CountSumTree
    case 8:   // ProvableCountTree
    case 10:  // ProvableCountSumTree
      break;
    default:
      *has_root_key = false;
      return true;
  }
  size_t cursor = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  *has_root_key = !root_key.empty();
  return true;
}

bool TreeElementRootKeyLen(const std::vector<uint8_t>& element_bytes,
                           uint32_t* root_key_len,
                           std::string* error) {
  if (root_key_len == nullptr) {
    if (error) {
      *error = "root key length output is null";
    }
    return false;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariantInternal(element_bytes, &variant, error)) {
    return false;
  }
  switch (variant) {
    case 2:   // Tree
    case 4:   // SumTree
    case 5:   // BigSumTree
    case 6:   // CountTree
    case 7:   // CountSumTree
    case 8:   // ProvableCountTree
    case 10:  // ProvableCountSumTree
      break;
    default:
      *root_key_len = 0;
      return true;
  }
  size_t cursor = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  *root_key_len = static_cast<uint32_t>(root_key.size());
  return true;
}

bool DecodeItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                ElementItem* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 0) {
    if (error) {
      *error = "element is not an Item variant";
    }
    return false;
  }

  if (!DecodeByteVector(element_bytes, &cursor, &out->value, error)) {
    return false;
  }

  uint64_t option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &option, error)) {
    return false;
  }
  if (option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }

  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }

  return true;
}

bool DecodeItemWithSumItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                           ElementItemWithSum* out,
                                           std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 9) {
    if (error) {
      *error = "element is not an ItemWithSumItem variant";
    }
    return false;
  }
  if (!DecodeByteVector(element_bytes, &cursor, &out->value, error)) {
    return false;
  }
  if (!DecodeVarintI64(element_bytes, &cursor, &out->sum, error)) {
    return false;
  }
  uint64_t option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &option, error)) {
    return false;
  }
  if (option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeSumItemFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                   ElementSumItem* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 3) {
    if (error) {
      *error = "element is not a SumItem variant";
    }
    return false;
  }
  if (!DecodeVarintI64(element_bytes, &cursor, &out->sum, error)) {
    return false;
  }
  uint64_t option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &option, error)) {
    return false;
  }
  if (option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeReferenceFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     ElementReference* out,
                                     std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 1) {
    if (error) {
      *error = "element is not a Reference variant";
    }
    return false;
  }

  uint64_t path_kind = 0;
  if (!DecodeVarint(element_bytes, &cursor, &path_kind, error)) {
    return false;
  }
  if (path_kind > static_cast<uint64_t>(ReferencePathKind::kSibling)) {
    if (error) {
      *error = "invalid reference path kind";
    }
    return false;
  }
  out->reference_path = ReferencePathType{};
  out->reference_path.kind = static_cast<ReferencePathKind>(path_kind);
  switch (out->reference_path.kind) {
    case ReferencePathKind::kAbsolute:
      if (!DecodeByteVectorList(element_bytes, &cursor, &out->reference_path.path, error)) {
        return false;
      }
      break;
    case ReferencePathKind::kUpstreamRootHeight:
    case ReferencePathKind::kUpstreamRootHeightWithParentPathAddition:
    case ReferencePathKind::kUpstreamFromElementHeight: {
      uint64_t height = 0;
      if (!DecodeVarint(element_bytes, &cursor, &height, error)) {
        return false;
      }
      if (height > 0xff) {
        if (error) {
          *error = "reference height out of range";
        }
        return false;
      }
      out->reference_path.height = static_cast<uint8_t>(height);
      if (!DecodeByteVectorList(element_bytes, &cursor, &out->reference_path.path, error)) {
        return false;
      }
      break;
    }
    case ReferencePathKind::kCousin:
    case ReferencePathKind::kSibling:
      if (!DecodeByteVector(element_bytes, &cursor, &out->reference_path.key, error)) {
        return false;
      }
      break;
    case ReferencePathKind::kRemovedCousin:
      if (!DecodeByteVectorList(element_bytes, &cursor, &out->reference_path.path, error)) {
        return false;
      }
      break;
  }

  uint64_t hop_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &hop_option, error)) {
    return false;
  }
  out->has_max_hop = false;
  out->max_hop = 0;
  if (hop_option == 1) {
    uint64_t hop = 0;
    if (!DecodeVarint(element_bytes, &cursor, &hop, error)) {
      return false;
    }
    if (hop > 0xff) {
      if (error) {
        *error = "max hop out of range";
      }
      return false;
    }
    out->has_max_hop = true;
    out->max_hop = static_cast<uint8_t>(hop);
  } else if (hop_option != 0) {
    if (error) {
      *error = "invalid max hop option tag";
    }
    return false;
  }

  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }

  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }

  return true;
}

bool DecodeTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                ElementTree* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 2) {
    if (error) {
      *error = "element is not a Tree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                   ElementSumTree* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 4) {
    if (error) {
      *error = "element is not a SumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  int64_t sum = 0;
  if (!DecodeVarintI64(element_bytes, &cursor, &sum, error)) {
    return false;
  }
  out->sum = sum;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeCountTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     ElementCountTree* out,
                                     std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 6) {
    if (error) {
      *error = "element is not a CountTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  uint64_t count = 0;
  if (!DecodeVarint(element_bytes, &cursor, &count, error)) {
    return false;
  }
  out->count = count;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeCountSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                        ElementCountSumTree* out,
                                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 7) {
    if (error) {
      *error = "element is not a CountSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  uint64_t count = 0;
  if (!DecodeVarint(element_bytes, &cursor, &count, error)) {
    return false;
  }
  out->count = count;
  int64_t sum = 0;
  if (!DecodeVarintI64(element_bytes, &cursor, &sum, error)) {
    return false;
  }
  out->sum = sum;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeBigSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                      ElementBigSumTree* out,
                                      std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 5) {
    if (error) {
      *error = "element is not a BigSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  __int128 sum = 0;
  if (!DecodeVarintI128(element_bytes, &cursor, &sum, error)) {
    return false;
  }
  out->sum = sum;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeProvableCountTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                             ElementProvableCountTree* out,
                                             std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 8) {
    if (error) {
      *error = "element is not a ProvableCountTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  uint64_t count = 0;
  if (!DecodeVarint(element_bytes, &cursor, &count, error)) {
    return false;
  }
  out->count = count;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeProvableCountSumTreeFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                                ElementProvableCountSumTree* out,
                                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 10) {
    if (error) {
      *error = "element is not a ProvableCountSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (root_key.empty()) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::move(root_key);
  }
  uint64_t count = 0;
  if (!DecodeVarint(element_bytes, &cursor, &count, error)) {
    return false;
  }
  out->count = count;
  int64_t sum = 0;
  if (!DecodeVarintI64(element_bytes, &cursor, &sum, error)) {
    return false;
  }
  out->sum = sum;
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 1) {
    std::vector<uint8_t> flags;
    if (!DecodeByteVector(element_bytes, &cursor, &flags, error)) {
      return false;
    }
  } else if (flags_option != 0) {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool ExtractSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                     int64_t* out_sum,
                                     bool* has_sum,
                                     std::string* error) {
  if (out_sum == nullptr || has_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  *has_sum = false;
  *out_sum = 0;
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant == 3) {
    if (!DecodeVarintI64(element_bytes, &cursor, out_sum, error)) {
      return false;
    }
    *has_sum = true;
    return true;
  }
  if (variant == 9) {
    std::vector<uint8_t> ignored_value;
    if (!DecodeByteVector(element_bytes, &cursor, &ignored_value, error)) {
      return false;
    }
    if (!DecodeVarintI64(element_bytes, &cursor, out_sum, error)) {
      return false;
    }
    *has_sum = true;
    return true;
  }
  return true;
}

bool ExtractBigSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                        __int128* out_sum,
                                        bool* has_sum,
                                        std::string* error) {
  if (out_sum == nullptr || has_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  *has_sum = false;
  *out_sum = 0;
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant == 3) {
    int64_t sum = 0;
    if (!DecodeVarintI64(element_bytes, &cursor, &sum, error)) {
      return false;
    }
    *out_sum = static_cast<__int128>(sum);
    *has_sum = true;
    return true;
  }
  if (variant == 9) {
    std::vector<uint8_t> ignored_value;
    if (!DecodeByteVector(element_bytes, &cursor, &ignored_value, error)) {
      return false;
    }
    int64_t sum = 0;
    if (!DecodeVarintI64(element_bytes, &cursor, &sum, error)) {
      return false;
    }
    *out_sum = static_cast<__int128>(sum);
    *has_sum = true;
    return true;
  }
  if (variant != 5) {
    return true;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (!DecodeVarintI128(element_bytes, &cursor, out_sum, error)) {
    return false;
  }
  *has_sum = true;
  return true;
}

bool ExtractCountValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                       uint64_t* out_count,
                                       bool* has_count,
                                       std::string* error) {
  if (out_count == nullptr || has_count == nullptr) {
    if (error) {
      *error = "count output is null";
    }
    return false;
  }
  *has_count = false;
  *out_count = 0;
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 6 && variant != 7 && variant != 8 && variant != 10) {
    return true;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  *has_count = true;
  return true;
}

bool ExtractCountSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                          uint64_t* out_count,
                                          int64_t* out_sum,
                                          bool* has_count_sum,
                                          std::string* error) {
  if (out_count == nullptr || out_sum == nullptr || has_count_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  *has_count_sum = false;
  *out_count = 0;
  *out_sum = 0;
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 7 && variant != 10) {
    return true;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  if (!DecodeVarintI64(element_bytes, &cursor, out_sum, error)) {
    return false;
  }
  *has_count_sum = true;
  return true;
}

bool ExtractProvableCountSumValueFromElementBytes(const std::vector<uint8_t>& element_bytes,
                                                  uint64_t* out_count,
                                                  int64_t* out_sum,
                                                  bool* has_count_sum,
                                                  std::string* error) {
  if (out_count == nullptr || out_sum == nullptr || has_count_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  *has_count_sum = false;
  *out_count = 0;
  *out_sum = 0;
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 10) {
    return true;
  }
  std::vector<uint8_t> root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  if (!DecodeVarintI64(element_bytes, &cursor, out_sum, error)) {
    return false;
  }
  *has_count_sum = true;
  return true;
}

bool EncodeItemToElementBytes(const std::vector<uint8_t>& value,
                              std::vector<uint8_t>* out,
                              std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  out->reserve(value.size() + 8);

  AppendVarint(0, out);  // Element::Item variant
  AppendVarint(static_cast<uint64_t>(value.size()), out);
  out->insert(out->end(), value.begin(), value.end());
  AppendVarint(0, out);  // flags option = None

  return true;
}

bool EncodeItemToElementBytesWithFlags(const std::vector<uint8_t>& value,
                                       const std::vector<uint8_t>& flags,
                                       std::vector<uint8_t>* out,
                                       std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  out->reserve(value.size() + flags.size() + 10);

  AppendVarint(0, out);  // Element::Item variant
  AppendVarint(static_cast<uint64_t>(value.size()), out);
  out->insert(out->end(), value.begin(), value.end());
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }

  return true;
}

bool EncodeSumItemToElementBytes(int64_t sum,
                                 std::vector<uint8_t>* out,
                                 std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(3, out);  // Element::SumItem variant
  AppendVarint(ZigZagEncodeI64(sum), out);
  AppendVarint(0, out);  // flags option = None
  return true;
}

bool EncodeItemWithSumItemToElementBytes(const std::vector<uint8_t>& value,
                                         int64_t sum,
                                         std::vector<uint8_t>* out,
                                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(9, out);  // Element::ItemWithSumItem variant
  AppendVarint(static_cast<uint64_t>(value.size()), out);
  out->insert(out->end(), value.begin(), value.end());
  AppendVarint(ZigZagEncodeI64(sum), out);
  AppendVarint(0, out);  // flags option = None
  return true;
}

bool EncodeReferenceToElementBytes(const ElementReference& reference,
                                   std::vector<uint8_t>* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(1, out);  // Element::Reference variant
  AppendVarint(static_cast<uint64_t>(reference.reference_path.kind), out);
  switch (reference.reference_path.kind) {
    case ReferencePathKind::kAbsolute:
      AppendByteVectorList(reference.reference_path.path, out);
      break;
    case ReferencePathKind::kUpstreamRootHeight:
    case ReferencePathKind::kUpstreamRootHeightWithParentPathAddition:
    case ReferencePathKind::kUpstreamFromElementHeight:
      AppendVarint(reference.reference_path.height, out);
      AppendByteVectorList(reference.reference_path.path, out);
      break;
    case ReferencePathKind::kCousin:
    case ReferencePathKind::kSibling:
      AppendByteVector(reference.reference_path.key, out);
      break;
    case ReferencePathKind::kRemovedCousin:
      AppendByteVectorList(reference.reference_path.path, out);
      break;
  }
  if (reference.has_max_hop) {
    AppendVarint(1, out);
    AppendVarint(reference.max_hop, out);
  } else {
    AppendVarint(0, out);
  }
  AppendVarint(0, out);  // flags option = None
  return true;
}

bool EncodeTreeToElementBytes(std::vector<uint8_t>* out, std::string* error) {
  return EncodeTreeToElementBytesWithRootKey(nullptr, out, error);
}

bool EncodeTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                         std::vector<uint8_t>* out,
                                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  out->push_back(0x02);  // Element::Tree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                            int64_t sum,
                                            std::vector<uint8_t>* out,
                                            std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(4, out);  // Element::SumTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(ZigZagEncodeI64(sum), out);
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeBigSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                               __int128 sum,
                                               std::vector<uint8_t>* out,
                                               std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(5, out);  // Element::BigSumTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarintU128(ZigZagEncodeI128(sum), out);
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeCountTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                              uint64_t count,
                                              std::vector<uint8_t>* out,
                                              std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(6, out);  // Element::CountTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeCountSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                 uint64_t count,
                                                 int64_t sum,
                                                 std::vector<uint8_t>* out,
                                                 std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(7, out);  // Element::CountSumTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  AppendVarint(ZigZagEncodeI64(sum), out);
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeProvableCountTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                      uint64_t count,
                                                      std::vector<uint8_t>* out,
                                                      std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(8, out);  // Element::ProvableCountTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  out->push_back(0x00);  // flags option = None
  return true;
}

bool EncodeProvableCountSumTreeToElementBytesWithRootKey(const std::vector<uint8_t>* root_key,
                                                         uint64_t count,
                                                         int64_t sum,
                                                         std::vector<uint8_t>* out,
                                                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(10, out);  // Element::ProvableCountSumTree variant
  if (root_key == nullptr) {
    out->push_back(0x00);  // Option::None
  } else {
    out->push_back(0x01);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  AppendVarint(ZigZagEncodeI64(sum), out);
  out->push_back(0x00);  // flags option = None
  return true;
}

}  // namespace grovedb
