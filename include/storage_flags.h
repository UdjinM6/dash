#ifndef GROVEDB_CPP_STORAGE_FLAGS_H
#define GROVEDB_CPP_STORAGE_FLAGS_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "operation_cost.h"

namespace grovedb {

constexpr uint32_t kMinimumNonBaseFlagsSize = 3;

struct StorageFlags {
  enum class Type {
    kSingleEpoch,
    kMultiEpoch,
    kSingleEpochOwned,
    kMultiEpochOwned,
  };

  Type type = Type::kSingleEpoch;
  uint16_t base_epoch = 0;
  std::map<uint16_t, uint32_t> other_epoch_bytes;
  bool has_owner_id = false;
  std::array<uint8_t, 32> owner_id{};
};

bool ParseStorageFlags(const std::vector<uint8_t>& data,
                       std::optional<StorageFlags>* out,
                       std::string* error);

bool SplitStorageRemovedBytesFromFlags(const std::vector<uint8_t>& flags,
                                       uint32_t removed_key_bytes,
                                       uint32_t removed_value_bytes,
                                       StorageRemovedBytes* key_removed,
                                       StorageRemovedBytes* value_removed,
                                       std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_STORAGE_FLAGS_H
