#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  std::string dir = MakeTempDir("rust_reverse_parity");
  std::string error;
  {
    grovedb::RocksDbWrapper db;
    if (!db.Open(dir, &error)) {
      Fail("open db failed: " + error);
    }

    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k', '1'},
                {'v', '1'}, &error)) {
      Fail("put default failed: " + error);
    }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k', '2'},
              {'v', '2'}, &error)) {
    Fail("put child failed: " + error);
  }
    if (!db.Put(grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}}, {'a', '1'},
                {'a', 'v', '1'}, &error)) {
      Fail("put aux failed: " + error);
    }
    if (!db.Put(grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}}, {'r', '1'},
                {'r', 'v', '1'}, &error)) {
      Fail("put roots failed: " + error);
    }
    if (!db.Put(grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}}, {'m', '1'},
                {'m', 'v', '1'}, &error)) {
      Fail("put meta failed: " + error);
    }
  }

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_reader \"" +
      dir + "\" iter";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage reader");
  }
  cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_reader \"" +
      dir + "\" iter_rev";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage reader reverse");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
