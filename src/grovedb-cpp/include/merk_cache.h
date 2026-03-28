#ifndef GROVEDB_CPP_MERK_CACHE_H
#define GROVEDB_CPP_MERK_CACHE_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "merk.h"
#include "rocksdb_wrapper.h"

namespace grovedb {

class MerkCache {
 public:
  explicit MerkCache(RocksDbWrapper* storage);

  bool getOrLoad(const std::vector<std::vector<uint8_t>>& path,
                 RocksDbWrapper::Transaction* tx,
                 MerkTree** out,
                 std::string* error);

  bool getOrLoad(const std::vector<std::vector<uint8_t>>& path,
                 MerkTree** out,
                 std::string* error);

  void clear();

  size_t size() const;

  bool contains(const std::vector<std::vector<uint8_t>>& path) const;
  void erase(const std::vector<std::vector<uint8_t>>& path);

  std::vector<std::vector<std::vector<uint8_t>>> getCachedPaths() const;

  void saveState();
  void restoreState();

 private:
  RocksDbWrapper* storage_;
  std::unordered_map<std::string, std::unique_ptr<MerkTree>> merks_;
  std::vector<std::string> saved_paths_;

  static std::string pathToString(const std::vector<std::vector<uint8_t>>& path);
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_MERK_CACHE_H
