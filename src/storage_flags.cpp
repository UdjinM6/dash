#include "storage_flags.h"

#include "binary.h"

#include <limits>

namespace grovedb {

namespace {

StorageRemovedBytes SingleStorageRemoval(
    uint32_t removed_bytes,
    uint16_t base_epoch,
    const std::optional<std::array<uint8_t, 32>>& owner_id) {
  if (removed_bytes == 0) {
    return StorageRemovedBytes::None();
  }
  std::map<std::array<uint8_t, 32>, std::unordered_map<uint16_t, uint32_t>> by_owner;
  std::array<uint8_t, 32> identifier{};
  if (owner_id.has_value()) {
    identifier = *owner_id;
  }
  by_owner[identifier][base_epoch] = removed_bytes;
  return StorageRemovedBytes::Sectioned(std::move(by_owner));
}

StorageRemovedBytes SectionedStorageRemoval(
    uint32_t removed_bytes,
    uint16_t base_epoch,
    const std::map<uint16_t, uint32_t>& other_epoch_bytes,
    const std::optional<std::array<uint8_t, 32>>& owner_id) {
  if (removed_bytes == 0) {
    return StorageRemovedBytes::None();
  }
  uint32_t bytes_left = removed_bytes;
  std::unordered_map<uint16_t, uint32_t> removal_by_epoch;
  for (auto it = other_epoch_bytes.rbegin(); it != other_epoch_bytes.rend(); ++it) {
    if (bytes_left == 0) {
      break;
    }
    uint32_t bytes_in_epoch = it->second;
    if (bytes_in_epoch <= bytes_left + kMinimumNonBaseFlagsSize) {
      uint32_t removed = bytes_in_epoch - kMinimumNonBaseFlagsSize;
      removal_by_epoch[it->first] = removed;
      bytes_left -= removed;
    } else {
      removal_by_epoch[it->first] = bytes_left;
      bytes_left = 0;
      break;
    }
  }
  if (bytes_left > 0) {
    removal_by_epoch[base_epoch] += bytes_left;
  }
  std::map<std::array<uint8_t, 32>, std::unordered_map<uint16_t, uint32_t>> by_owner;
  std::array<uint8_t, 32> identifier{};
  if (owner_id.has_value()) {
    identifier = *owner_id;
  }
  by_owner[identifier] = std::move(removal_by_epoch);
  return StorageRemovedBytes::Sectioned(std::move(by_owner));
}

}  // namespace

bool ParseStorageFlags(const std::vector<uint8_t>& data,
                       std::optional<StorageFlags>* out,
                       std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "storage flags output is null";
    }
    return false;
  }
  out->reset();
  if (data.empty()) {
    return true;
  }
  uint8_t type_byte = data[0];
  StorageFlags flags;
  size_t cursor = 0;
  switch (type_byte) {
    case 0: {
      if (data.size() != 3) {
        if (error) {
          *error = "single epoch flags must be 3 bytes";
        }
        return false;
      }
      cursor = 1;
      uint16_t epoch = 0;
      if (!ReadU16BE(data, &cursor, &epoch, error)) {
        return false;
      }
      flags.type = StorageFlags::Type::kSingleEpoch;
      flags.base_epoch = epoch;
      break;
    }
    case 1: {
      if (data.size() < 6) {
        if (error) {
          *error = "multi epoch flags must be at least 6 bytes";
        }
        return false;
      }
      cursor = 1;
      uint16_t base_epoch = 0;
      if (!ReadU16BE(data, &cursor, &base_epoch, error)) {
        return false;
      }
      flags.type = StorageFlags::Type::kMultiEpoch;
      flags.base_epoch = base_epoch;
      while (cursor + 2 < data.size()) {
        uint16_t epoch_index = 0;
        if (!ReadU16BE(data, &cursor, &epoch_index, error)) {
          return false;
        }
        uint64_t bytes_at_epoch = 0;
        if (!ReadVarintU64(data, &cursor, &bytes_at_epoch, error)) {
          return false;
        }
        if (bytes_at_epoch > std::numeric_limits<uint32_t>::max()) {
          if (error) {
            *error = "storage flags bytes overflow";
          }
          return false;
        }
        flags.other_epoch_bytes[epoch_index] = static_cast<uint32_t>(bytes_at_epoch);
      }
      break;
    }
    case 2: {
      if (data.size() != 35) {
        if (error) {
          *error = "single epoch owned flags must be 35 bytes";
        }
        return false;
      }
      std::copy(data.begin() + 1, data.begin() + 33, flags.owner_id.begin());
      flags.has_owner_id = true;
      cursor = 33;
      uint16_t base_epoch = 0;
      if (!ReadU16BE(data, &cursor, &base_epoch, error)) {
        return false;
      }
      flags.type = StorageFlags::Type::kSingleEpochOwned;
      flags.base_epoch = base_epoch;
      break;
    }
    case 3: {
      if (data.size() < 38) {
        if (error) {
          *error = "multi epoch owned flags must be at least 38 bytes";
        }
        return false;
      }
      std::copy(data.begin() + 1, data.begin() + 33, flags.owner_id.begin());
      flags.has_owner_id = true;
      cursor = 33;
      uint16_t base_epoch = 0;
      if (!ReadU16BE(data, &cursor, &base_epoch, error)) {
        return false;
      }
      flags.type = StorageFlags::Type::kMultiEpochOwned;
      flags.base_epoch = base_epoch;
      cursor = 35;
      while (cursor + 2 < data.size()) {
        uint16_t epoch_index = 0;
        if (!ReadU16BE(data, &cursor, &epoch_index, error)) {
          return false;
        }
        uint64_t bytes_at_epoch = 0;
        if (!ReadVarintU64(data, &cursor, &bytes_at_epoch, error)) {
          return false;
        }
        if (bytes_at_epoch > std::numeric_limits<uint32_t>::max()) {
          if (error) {
            *error = "storage flags bytes overflow";
          }
          return false;
        }
        flags.other_epoch_bytes[epoch_index] = static_cast<uint32_t>(bytes_at_epoch);
      }
      break;
    }
    default:
      if (error) {
        *error = "unknown storage flags type";
      }
      return false;
  }
  *out = flags;
  return true;
}

bool SplitStorageRemovedBytesFromFlags(const std::vector<uint8_t>& flags,
                                       uint32_t removed_key_bytes,
                                       uint32_t removed_value_bytes,
                                       StorageRemovedBytes* key_removed,
                                       StorageRemovedBytes* value_removed,
                                       std::string* error) {
  if (key_removed == nullptr || value_removed == nullptr) {
    if (error) {
      *error = "removed bytes output is null";
    }
    return false;
  }
  if (flags.empty()) {
    *key_removed = removed_key_bytes == 0
                       ? StorageRemovedBytes::None()
                       : StorageRemovedBytes::Basic(removed_key_bytes);
    *value_removed = removed_value_bytes == 0
                         ? StorageRemovedBytes::None()
                         : StorageRemovedBytes::Basic(removed_value_bytes);
    return true;
  }
  std::optional<StorageFlags> parsed;
  if (!ParseStorageFlags(flags, &parsed, error)) {
    return false;
  }
  if (!parsed.has_value()) {
    *key_removed = StorageRemovedBytes::None();
    *value_removed = StorageRemovedBytes::None();
    return true;
  }
  const StorageFlags& storage_flags = *parsed;
  std::optional<std::array<uint8_t, 32>> owner_id;
  if (storage_flags.has_owner_id) {
    owner_id = storage_flags.owner_id;
  }
  if (removed_key_bytes > 0) {
    *key_removed = SingleStorageRemoval(removed_key_bytes, storage_flags.base_epoch, owner_id);
  } else {
    *key_removed = StorageRemovedBytes::None();
  }
  switch (storage_flags.type) {
    case StorageFlags::Type::kSingleEpoch:
      *value_removed = SingleStorageRemoval(removed_value_bytes,
                                            storage_flags.base_epoch,
                                            std::nullopt);
      break;
    case StorageFlags::Type::kSingleEpochOwned:
      *value_removed = SingleStorageRemoval(removed_value_bytes,
                                            storage_flags.base_epoch,
                                            owner_id);
      break;
    case StorageFlags::Type::kMultiEpoch:
      *value_removed = SectionedStorageRemoval(removed_value_bytes,
                                               storage_flags.base_epoch,
                                               storage_flags.other_epoch_bytes,
                                               std::nullopt);
      break;
    case StorageFlags::Type::kMultiEpochOwned:
      *value_removed = SectionedStorageRemoval(removed_value_bytes,
                                               storage_flags.base_epoch,
                                               storage_flags.other_epoch_bytes,
                                               owner_id);
      break;
  }
  return true;
}

}  // namespace grovedb
