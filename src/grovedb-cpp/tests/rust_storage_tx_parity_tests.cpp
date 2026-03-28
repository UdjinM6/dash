#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  std::string dir = MakeTempDir("rust_tx_parity");
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_tx_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage tx writer");
  }

  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("open db failed: " + error);
  }

  grovedb::RocksDbWrapper::PrefixedContext ctx(
      &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
  std::vector<uint8_t> value;
  bool found = false;
  if (!ctx.Get({'k', '1'}, &value, &found, &error)) {
    Fail("get k1 failed: " + error);
  }
  if (found) {
    Fail("k1 should be deleted in tx");
  }
  if (!ctx.Get({'k', '2'}, &value, &found, &error)) {
    Fail("get k2 failed: " + error);
  }
  if (!found || value != std::vector<uint8_t>({'v', '2'})) {
    Fail("k2 value mismatch");
  }

  grovedb::RocksDbWrapper::PrefixedContext roots(
      &db, grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}});
  if (!roots.Get({'r', '1'}, &value, &found, &error)) {
    Fail("get roots failed: " + error);
  }
  if (!found || value != std::vector<uint8_t>({'r', 'v', '1'})) {
    Fail("roots value mismatch");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
