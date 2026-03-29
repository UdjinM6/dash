#ifndef GROVEDB_CPP_OPERATION_COST_H
#define GROVEDB_CPP_OPERATION_COST_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cost_utils.h"

namespace grovedb {

constexpr uint16_t kUnknownEpoch = 0xFFFF;

struct StorageRemovedBytes {
  enum class Type {
    kNone,
    kBasic,
    kSectioned,
  };

  Type type = Type::kNone;
  uint32_t basic_bytes = 0;
  std::map<std::array<uint8_t, 32>, std::unordered_map<uint16_t, uint32_t>>
      sectioned_bytes;

  static StorageRemovedBytes None() { return {}; }
  static StorageRemovedBytes Basic(uint32_t bytes) {
    StorageRemovedBytes out;
    out.type = Type::kBasic;
    out.basic_bytes = bytes;
    return out;
  }

  static StorageRemovedBytes Sectioned(
      std::map<std::array<uint8_t, 32>, std::unordered_map<uint16_t, uint32_t>> map) {
    StorageRemovedBytes out;
    out.type = Type::kSectioned;
    out.sectioned_bytes = std::move(map);
    return out;
  }

  bool HasRemoval() const { return TotalRemovedBytes() != 0; }

  uint32_t TotalRemovedBytes() const {
    switch (type) {
      case Type::kNone:
        return 0;
      case Type::kBasic:
        return basic_bytes;
      case Type::kSectioned: {
        uint32_t total = 0;
        for (const auto& entry : sectioned_bytes) {
          for (const auto& epoch_entry : entry.second) {
            total += epoch_entry.second;
          }
        }
        return total;
      }
    }
    return 0;
  }

  void Add(const StorageRemovedBytes& other) {
    if (other.type == Type::kNone) {
      return;
    }
    if (type == Type::kNone) {
      *this = other;
      return;
    }
    if (type == Type::kBasic && other.type == Type::kBasic) {
      basic_bytes += other.basic_bytes;
      return;
    }
    if (type == Type::kBasic && other.type == Type::kSectioned) {
      auto map = other.sectioned_bytes;
      std::array<uint8_t, 32> default_id{};
      auto& epochs = map[default_id];
      epochs[kUnknownEpoch] += basic_bytes;
      type = Type::kSectioned;
      basic_bytes = 0;
      sectioned_bytes = std::move(map);
      return;
    }
    if (type == Type::kSectioned && other.type == Type::kBasic) {
      std::array<uint8_t, 32> default_id{};
      auto& epochs = sectioned_bytes[default_id];
      epochs[kUnknownEpoch] += other.basic_bytes;
      return;
    }
    if (type == Type::kSectioned && other.type == Type::kSectioned) {
      for (const auto& entry : other.sectioned_bytes) {
        auto& target = sectioned_bytes[entry.first];
        for (const auto& epoch_entry : entry.second) {
          target[epoch_entry.first] += epoch_entry.second;
        }
      }
    }
  }
};

struct StorageCost {
  uint32_t added_bytes = 0;
  uint32_t replaced_bytes = 0;
  StorageRemovedBytes removed_bytes{};

  bool Verify(uint32_t len, std::string* error) const {
    if (added_bytes + replaced_bytes == len) {
      return true;
    }
    if (error) {
      *error = "storage cost mismatch";
    }
    return false;
  }

  bool VerifyKeyStorageCost(uint32_t len, bool new_node, std::string* error) const {
    if (!new_node) {
      return true;
    }
    return Verify(len, error);
  }

  void Reset() {
    added_bytes = 0;
    replaced_bytes = 0;
    removed_bytes = StorageRemovedBytes::None();
  }

  void Add(const StorageCost& other) {
    added_bytes += other.added_bytes;
    replaced_bytes += other.replaced_bytes;
    removed_bytes.Add(other.removed_bytes);
  }

  enum class TransitionType {
    kInsertNew,
    kUpdateBiggerSize,
    kUpdateSmallerSize,
    kUpdateSameSize,
    kReplace,
    kDelete,
    kNone,
  };

  TransitionType Transition() const {
    if (added_bytes > 0) {
      if (removed_bytes.HasRemoval()) {
        return TransitionType::kReplace;
      }
      if (replaced_bytes > 0) {
        return TransitionType::kUpdateBiggerSize;
      }
      return TransitionType::kInsertNew;
    }
    if (removed_bytes.HasRemoval()) {
      if (replaced_bytes > 0) {
        return TransitionType::kUpdateSmallerSize;
      }
      return TransitionType::kDelete;
    }
    if (replaced_bytes > 0) {
      return TransitionType::kUpdateSameSize;
    }
    return TransitionType::kNone;
  }

  bool WorseOrEqThan(const StorageCost& other) const {
    return replaced_bytes >= other.replaced_bytes &&
           added_bytes >= other.added_bytes &&
           removed_bytes.TotalRemovedBytes() <= other.removed_bytes.TotalRemovedBytes();
  }

  bool HasStorageChange() const {
    return added_bytes != 0 || removed_bytes.TotalRemovedBytes() != 0;
  }
};

struct KeyValueStorageCost {
  StorageCost key_storage_cost{};
  StorageCost value_storage_cost{};
  bool new_node = false;
  bool needs_value_verification = false;

  static KeyValueStorageCost ForUpdatedRootCost(bool had_old,
                                               uint32_t old_tree_key_len,
                                               uint32_t tree_key_len) {
    KeyValueStorageCost cost;
    if (had_old) {
      cost.key_storage_cost.added_bytes = 0;
      cost.key_storage_cost.replaced_bytes = 34;
      uint32_t new_bytes = tree_key_len + RequiredSpaceU32(tree_key_len);
      uint32_t old_bytes = old_tree_key_len + RequiredSpaceU32(old_tree_key_len);
      if (tree_key_len < old_tree_key_len) {
        cost.value_storage_cost.replaced_bytes = new_bytes;
        cost.value_storage_cost.removed_bytes = StorageRemovedBytes::Basic(old_bytes - new_bytes);
      } else if (tree_key_len == old_tree_key_len) {
        cost.value_storage_cost.replaced_bytes = new_bytes;
      } else {
        cost.value_storage_cost.added_bytes = new_bytes - old_bytes;
        cost.value_storage_cost.replaced_bytes = old_bytes;
      }
      cost.new_node = false;
      cost.needs_value_verification = false;
      return cost;
    }
    cost.key_storage_cost.added_bytes = 34;
    cost.value_storage_cost.added_bytes = tree_key_len + RequiredSpaceU32(tree_key_len);
    cost.new_node = true;
    cost.needs_value_verification = false;
    return cost;
  }

  StorageRemovedBytes CombinedRemovedBytes() const {
    StorageRemovedBytes out = key_storage_cost.removed_bytes;
    out.Add(value_storage_cost.removed_bytes);
    return out;
  }
};

struct OperationCost {
  uint32_t seek_count = 0;
  StorageCost storage_cost{};
  uint64_t storage_loaded_bytes = 0;
  uint32_t hash_node_calls = 0;

  void Reset() {
    seek_count = 0;
    storage_cost.Reset();
    storage_loaded_bytes = 0;
    hash_node_calls = 0;
  }

  bool IsNothing() const {
    return seek_count == 0 && storage_loaded_bytes == 0 &&
           hash_node_calls == 0 && storage_cost.added_bytes == 0 &&
           storage_cost.replaced_bytes == 0 &&
           storage_cost.removed_bytes.TotalRemovedBytes() == 0;
  }

  void Add(const OperationCost& other) {
    seek_count += other.seek_count;
    storage_loaded_bytes += other.storage_loaded_bytes;
    hash_node_calls += other.hash_node_calls;
    storage_cost.Add(other.storage_cost);
  }

  enum class TreeCostType {
    kVarIntAs8Bytes,
    kTwoVarIntsAs16Bytes,
    kFixed16Bytes,
  };

  static uint32_t TreeCostSize(TreeCostType type) {
    switch (type) {
      case TreeCostType::kVarIntAs8Bytes:
        return 8;
      case TreeCostType::kTwoVarIntsAs16Bytes:
        return 16;
      case TreeCostType::kFixed16Bytes:
        return 16;
    }
    return 8;
  }

  using ChildSize = std::pair<uint32_t, uint32_t>;
  using ChildrenSizesWithIsSumTree = std::optional<std::tuple<
      std::optional<std::pair<TreeCostType, uint32_t>>,
      std::optional<ChildSize>,
      std::optional<ChildSize>>>;

  bool AddKeyValueStorageCosts(uint32_t key_len,
                               uint32_t value_len,
                               const ChildrenSizesWithIsSumTree& children_sizes,
                               const std::optional<KeyValueStorageCost>& storage_cost_info,
                               std::string* error) {
    uint32_t paid_key_len = key_len + RequiredSpaceU32(key_len);

    uint32_t final_paid_value_len = 0;
    if (storage_cost_info && !storage_cost_info->needs_value_verification) {
      final_paid_value_len =
          storage_cost_info->value_storage_cost.added_bytes +
          storage_cost_info->value_storage_cost.replaced_bytes;
    } else if (children_sizes.has_value()) {
      uint32_t paid_value_len = value_len;
      auto [sum_tree_info, left_child, right_child] = *children_sizes;
      paid_value_len -= 2;
      if (left_child.has_value()) {
        paid_value_len -= left_child->first;
        paid_value_len -= left_child->second;
      }
      if (right_child.has_value()) {
        paid_value_len -= right_child->first;
        paid_value_len -= right_child->second;
      }
      uint32_t sum_tree_node_size = 0;
      if (sum_tree_info.has_value()) {
        sum_tree_node_size = TreeCostSize(sum_tree_info->first);
        paid_value_len -= sum_tree_info->second;
        paid_value_len += sum_tree_node_size;
      }
      paid_value_len += RequiredSpaceU32(paid_value_len);
      paid_value_len += key_len + 4 + sum_tree_node_size;
      final_paid_value_len = paid_value_len;
    } else {
      final_paid_value_len = value_len + RequiredSpaceU32(value_len);
    }

    std::optional<StorageCost> key_cost;
    std::optional<StorageCost> value_cost;
    if (storage_cost_info.has_value()) {
      const KeyValueStorageCost& info = *storage_cost_info;
      if (!info.key_storage_cost.VerifyKeyStorageCost(paid_key_len, info.new_node, error)) {
        return false;
      }
      if (!info.value_storage_cost.Verify(final_paid_value_len, error)) {
        return false;
      }
      key_cost = info.key_storage_cost;
      value_cost = info.value_storage_cost;
    }

    AddStorageCosts(paid_key_len, key_cost);
    AddStorageCosts(final_paid_value_len, value_cost);
    return true;
  }

  void AddStorageCosts(uint32_t len_with_required_space,
                       const std::optional<StorageCost>& storage_cost_info) {
    if (!storage_cost_info.has_value()) {
      StorageCost add;
      add.added_bytes = len_with_required_space;
      storage_cost.Add(add);
      return;
    }
    storage_cost.Add(*storage_cost_info);
  }
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_OPERATION_COST_H
