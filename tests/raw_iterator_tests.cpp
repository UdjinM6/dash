#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

int main() {
  std::string error;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("raw_iter_test_" + std::to_string(now));

  grovedb::RocksDbWrapper db;
  if (!db.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  const std::vector<uint8_t> root = {'r'};
  const std::vector<uint8_t> k1 = {'a'};
  const std::vector<uint8_t> k2 = {'b'};
  const std::vector<uint8_t> k3 = {'c'};
  const std::vector<uint8_t> v1 = {'1'};
  const std::vector<uint8_t> v2 = {'2'};
  const std::vector<uint8_t> v3 = {'3'};

  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, k2, v2, &error) ||
      !db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, k1, v1, &error) ||
      !db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, k3, v3, &error)) {
    Fail("put keys failed: " + error);
  }

  grovedb::RocksDbWrapper::PrefixedIterator it;
  if (!it.Init(&db, grovedb::ColumnFamilyKind::kDefault, {root}, &error)) {
    Fail("iterator init failed: " + error);
  }

  if (!it.SeekToFirst(&error)) {
    Fail("seek to first failed: " + error);
  }
  if (!it.Valid()) {
    Fail("iterator not valid at first");
  }
  std::vector<uint8_t> key;
  if (!it.Key(&key, &error) || key != k1) {
    Fail("seek first key mismatch");
  }
  if (it.LastCost().storage_loaded_bytes == 0) {
    Fail("expected iterator cost after key");
  }

  if (!it.SeekToLast(&error)) {
    Fail("seek to last failed: " + error);
  }
  if (!it.Valid()) {
    Fail("iterator not valid at last");
  }
  if (!it.Key(&key, &error) || key != k3) {
    Fail("seek last key mismatch");
  }

  if (!it.SeekForPrev({'b'}, &error)) {
    Fail("seek for prev failed: " + error);
  }
  if (!it.Valid()) {
    Fail("iterator not valid after seek for prev");
  }
  if (!it.Key(&key, &error) || key != k2) {
    Fail("seek for prev key mismatch");
  }

  if (!it.Prev(&error)) {
    Fail("prev failed: " + error);
  }
  if (!it.Valid()) {
    Fail("iterator not valid after prev");
  }
  if (!it.Key(&key, &error) || key != k1) {
    Fail("prev key mismatch");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
