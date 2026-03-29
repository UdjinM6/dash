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
  std::string dir = MakeTempDir("rust_tx_rollback_parity");
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_tx_writer \"" +
      dir + "\" rollback";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage tx writer rollback");
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
  if (!ctx.Get({'k', '2'}, &value, &found, &error)) {
    Fail("get k2 failed: " + error);
  }
  if (found) {
    Fail("k2 should be absent after rollback");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
