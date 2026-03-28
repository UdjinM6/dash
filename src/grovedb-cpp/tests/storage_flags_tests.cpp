#include "binary.h"
#include "element.h"
#include "storage_flags.h"

#include <array>
#include <cassert>
#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> MakeSingleEpochFlags(uint16_t base_epoch) {
  std::vector<uint8_t> flags;
  flags.push_back(0);
  flags.push_back(static_cast<uint8_t>((base_epoch >> 8) & 0xff));
  flags.push_back(static_cast<uint8_t>(base_epoch & 0xff));
  return flags;
}

std::vector<uint8_t> MakeMultiEpochFlags(uint16_t base_epoch,
                                         uint16_t epoch_index,
                                         uint32_t bytes_at_epoch) {
  std::vector<uint8_t> flags;
  flags.push_back(1);
  flags.push_back(static_cast<uint8_t>((base_epoch >> 8) & 0xff));
  flags.push_back(static_cast<uint8_t>(base_epoch & 0xff));
  flags.push_back(static_cast<uint8_t>((epoch_index >> 8) & 0xff));
  flags.push_back(static_cast<uint8_t>(epoch_index & 0xff));
  grovedb::EncodeVarintU64(bytes_at_epoch, &flags);
  return flags;
}

std::vector<uint8_t> MakeSingleEpochOwnedFlags(uint16_t base_epoch,
                                               uint8_t owner_byte) {
  std::vector<uint8_t> flags;
  flags.push_back(2);
  flags.resize(33, owner_byte);
  flags.push_back(static_cast<uint8_t>((base_epoch >> 8) & 0xff));
  flags.push_back(static_cast<uint8_t>(base_epoch & 0xff));
  return flags;
}

}  // namespace

int main() {
  using grovedb::ExtractFlagsFromElementBytes;
  using grovedb::SplitStorageRemovedBytesFromFlags;
  using grovedb::StorageRemovedBytes;

  {
    std::vector<uint8_t> flags = MakeSingleEpochFlags(2);
    StorageRemovedBytes key_removed;
    StorageRemovedBytes value_removed;
    std::string error;
    assert(SplitStorageRemovedBytesFromFlags(flags, 5, 7, &key_removed, &value_removed, &error));
    assert(key_removed.type == StorageRemovedBytes::Type::kSectioned);
    assert(value_removed.type == StorageRemovedBytes::Type::kSectioned);
    std::array<uint8_t, 32> default_id{};
    assert(key_removed.sectioned_bytes[default_id][2] == 5);
    assert(value_removed.sectioned_bytes[default_id][2] == 7);
  }

  {
    std::vector<uint8_t> flags = MakeMultiEpochFlags(1, 3, 10);
    StorageRemovedBytes key_removed;
    StorageRemovedBytes value_removed;
    std::string error;
    assert(SplitStorageRemovedBytesFromFlags(flags, 0, 9, &key_removed, &value_removed, &error));
    assert(key_removed.type == StorageRemovedBytes::Type::kNone);
    std::array<uint8_t, 32> default_id{};
    assert(value_removed.sectioned_bytes[default_id][3] == 7);
    assert(value_removed.sectioned_bytes[default_id][1] == 2);
  }

  {
    std::vector<uint8_t> flags = MakeSingleEpochOwnedFlags(4, 0x11);
    StorageRemovedBytes key_removed;
    StorageRemovedBytes value_removed;
    std::string error;
    assert(SplitStorageRemovedBytesFromFlags(flags, 3, 5, &key_removed, &value_removed, &error));
    std::array<uint8_t, 32> owner_id{};
    owner_id.fill(0x11);
    assert(key_removed.sectioned_bytes[owner_id][4] == 3);
    assert(value_removed.sectioned_bytes[owner_id][4] == 5);
  }

  {
    std::vector<uint8_t> flags = MakeSingleEpochFlags(5);
    std::vector<uint8_t> element;
    grovedb::EncodeVarintU64(0, &element);
    grovedb::EncodeVarintU64(3, &element);
    element.insert(element.end(), {'a', 'b', 'c'});
    grovedb::EncodeVarintU64(1, &element);
    grovedb::EncodeVarintU64(flags.size(), &element);
    element.insert(element.end(), flags.begin(), flags.end());
    std::vector<uint8_t> extracted;
    std::string error;
    assert(ExtractFlagsFromElementBytes(element, &extracted, &error));
    assert(extracted == flags);
  }

  return 0;
}
