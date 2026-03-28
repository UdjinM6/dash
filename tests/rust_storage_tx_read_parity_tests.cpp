#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

std::vector<uint8_t> GetOrFail(grovedb::RocksDbWrapper* db,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               bool* found) {
  std::string error;
  std::vector<uint8_t> value;
  if (!db->Get(grovedb::ColumnFamilyKind::kDefault, path, key, &value, found, &error)) {
    Fail("read failed: " + error);
  }
  return value;
}

void RunCppFlow(const std::string& dir) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("open C++ db failed: " + error);
  }
  const std::vector<std::vector<uint8_t>> root_path = {{'r', 'o', 'o', 't'}};
  const std::vector<std::vector<uint8_t>> iter_path = {{'i', 't', 'e', 'r', '_', 'r', 'o', 'o', 't'}};
  const std::vector<std::vector<uint8_t>> other_path = {{'o', 't', 'h', 'e', 'r', '_', 'r', 'o', 'o', 't'}};

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin own-read tx failed: " + error);
    }
    if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, root_path, {'k', '_', 's', 'e', 'l', 'f'},
                {'v', '_', 's', 'e', 'l', 'f'}, &error)) {
      Fail("own-read put failed: " + error);
    }
    std::vector<uint8_t> value;
    bool found = false;
    if (!tx.Get(grovedb::ColumnFamilyKind::kDefault, root_path, {'k', '_', 's', 'e', 'l', 'f'},
                &value, &found, &error)) {
      Fail("own-read get failed: " + error);
    }
    const std::vector<uint8_t> marker = found ? std::vector<uint8_t>{'1'} : std::vector<uint8_t>{'0'};
    if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, root_path, {'m', '_', 'o', 'w', 'n'}, marker, &error)) {
      Fail("own-read marker put failed: " + error);
    }
    if (!tx.Commit(&error)) {
      Fail("own-read commit failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx_reader;
    if (!db.BeginTransaction(&tx_reader, &error)) {
      Fail("begin concurrent reader tx failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx_writer;
    if (!db.BeginTransaction(&tx_writer, &error)) {
      Fail("begin concurrent writer tx failed: " + error);
    }
    if (!tx_writer.Put(grovedb::ColumnFamilyKind::kDefault, root_path,
                       {'k', '_', 'e', 'x', 't', 'e', 'r', 'n', 'a', 'l'},
                       {'v', '_', 'e', 'x', 't', 'e', 'r', 'n', 'a', 'l'},
                       &error)) {
      Fail("concurrent writer put failed: " + error);
    }
    if (!tx_writer.Commit(&error)) {
      Fail("concurrent writer commit failed: " + error);
    }
    std::vector<uint8_t> value;
    bool found = false;
    if (!tx_reader.Get(grovedb::ColumnFamilyKind::kDefault, root_path,
                       {'k', '_', 'e', 'x', 't', 'e', 'r', 'n', 'a', 'l'},
                       &value, &found, &error)) {
      Fail("concurrent reader get failed: " + error);
    }
    const std::vector<uint8_t> marker = found ? std::vector<uint8_t>{'1'} : std::vector<uint8_t>{'0'};
    if (!tx_reader.Put(grovedb::ColumnFamilyKind::kDefault, root_path,
                       {'m', '_', 'c', 'o', 'n', 'c', 'u', 'r', 'r', 'e', 'n', 't'},
                       marker,
                       &error)) {
      Fail("concurrent marker put failed: " + error);
    }
    if (!tx_reader.Commit(&error)) {
      Fail("concurrent reader commit failed: " + error);
    }
  }

  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, iter_path, {'a'}, {'v', 'a'}, &error)) {
    Fail("iter seed a failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, iter_path, {'c'}, {'v', 'c'}, &error)) {
    Fail("iter seed c failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, other_path, {'z'}, {'v', 'z'}, &error)) {
    Fail("iter seed other failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::Transaction tx_iter;
    if (!db.BeginTransaction(&tx_iter, &error)) {
      Fail("begin iter tx failed: " + error);
    }
    if (!tx_iter.Put(grovedb::ColumnFamilyKind::kDefault, iter_path, {'b'}, {'v', 'b'}, &error)) {
      Fail("iter tx put b failed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    if (!tx_iter.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, iter_path, &entries, &error)) {
      Fail("iter tx scan failed: " + error);
    }
    std::string joined;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (i != 0) {
        joined.push_back('|');
      }
      joined.append(reinterpret_cast<const char*>(entries[i].first.data()), entries[i].first.size());
    }
    std::vector<uint8_t> marker(joined.begin(), joined.end());
    if (!tx_iter.Put(grovedb::ColumnFamilyKind::kDefault, root_path, {'m', '_', 'i', 't', 'e', 'r'},
                     marker, &error)) {
      Fail("iter marker put failed: " + error);
    }
    if (!tx_iter.Commit(&error)) {
      Fail("iter tx commit failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx_rb;
    if (!db.BeginTransaction(&tx_rb, &error)) {
      Fail("begin rollback tx failed: " + error);
    }
    if (!tx_rb.Put(grovedb::ColumnFamilyKind::kDefault, root_path, {'k', '_', 'r', 'b'},
                   {'v', '_', 'r', 'b'}, &error)) {
      Fail("rollback tx put failed: " + error);
    }
    if (!tx_rb.Rollback(&error)) {
      Fail("rollback tx rollback failed: " + error);
    }
    std::vector<uint8_t> value;
    bool found = false;
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, root_path, {'k', '_', 'r', 'b'}, &value, &found,
                &error)) {
      Fail("rollback tx post-check failed: " + error);
    }
    const std::vector<uint8_t> marker = found ? std::vector<uint8_t>{'1'} : std::vector<uint8_t>{'0'};
    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, root_path,
                {'m', '_', 'r', 'o', 'l', 'l', 'b', 'a', 'c', 'k'}, marker, &error)) {
      Fail("rollback marker put failed: " + error);
    }
  }
}

void AssertMarkersEqual(const std::string& rust_dir, const std::string& cpp_dir) {
  grovedb::RocksDbWrapper rust_db;
  grovedb::RocksDbWrapper cpp_db;
  std::string error;
  if (!rust_db.Open(rust_dir, &error)) {
    Fail("open rust db failed: " + error);
  }
  if (!cpp_db.Open(cpp_dir, &error)) {
    Fail("open cpp db failed: " + error);
  }
  const std::vector<std::vector<uint8_t>> root_path = {{'r', 'o', 'o', 't'}};
  const std::vector<std::vector<uint8_t>> markers = {
      {'m', '_', 'o', 'w', 'n'},
      {'m', '_', 'c', 'o', 'n', 'c', 'u', 'r', 'r', 'e', 'n', 't'},
      {'m', '_', 'i', 't', 'e', 'r'},
      {'m', '_', 'r', 'o', 'l', 'l', 'b', 'a', 'c', 'k'},
  };
  for (const auto& key : markers) {
    bool rust_found = false;
    bool cpp_found = false;
    const std::vector<uint8_t> rust_value = GetOrFail(&rust_db, root_path, key, &rust_found);
    const std::vector<uint8_t> cpp_value = GetOrFail(&cpp_db, root_path, key, &cpp_found);
    if (rust_found != cpp_found) {
      Fail("marker presence mismatch");
    }
    if (rust_found && rust_value != cpp_value) {
      Fail("marker value mismatch");
    }
  }
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  const std::string rust_dir = MakeTempDir("rust_tx_read_parity_rust");
  const std::string cpp_dir = MakeTempDir("rust_tx_read_parity_cpp");

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_tx_read_writer \"" +
      rust_dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust tx read writer");
  }

  RunCppFlow(cpp_dir);
  AssertMarkersEqual(rust_dir, cpp_dir);
  std::filesystem::remove_all(rust_dir);
  std::filesystem::remove_all(cpp_dir);
  return 0;
}
