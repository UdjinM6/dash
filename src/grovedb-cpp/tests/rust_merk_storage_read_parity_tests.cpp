#include "hex.h"
#include "merk_storage.h"
#include "test_utils.h"

#include <cstdlib>
#include <fstream>
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
  auto run_case = [](const std::string& mode,
                     const std::vector<std::vector<uint8_t>>& path,
                     const std::string& root_mode,
                     const std::vector<uint8_t>& expected_k1,
                     bool expect_k2_present,
                     bool expect_k3_present) {
    std::string dir = MakeTempDir("rust_merk_read_parity_" + mode);
    std::string cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        dir + "\" " + mode;
    if (std::system(cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer");
    }
    std::string root_hash_path = dir + "/root_hash.txt";
    cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_merk_root_hash \"" +
        dir + "\" \"" + root_hash_path + "\" " + root_mode;
    if (std::system(cmd.c_str()) != 0) {
      Fail("failed to run rust merk root hash");
    }

    grovedb::RocksDbWrapper storage;
    std::string error;
    if (!storage.Open(dir, &error)) {
      Fail("open storage failed: " + error);
    }

    grovedb::MerkTree tree;
    if (!grovedb::MerkStorage::LoadTree(&storage, path, &tree, &error)) {
      Fail("load tree failed: " + error);
    }
    std::vector<uint8_t> value;
    if (!tree.Get({'k', '1'}, &value) || value != expected_k1) {
      Fail("loaded tree has unexpected k1");
    }
    std::vector<uint8_t> k2_value;
    const bool k2_present = tree.Get({'k', '2'}, &k2_value);
    if (k2_present != expect_k2_present) {
      Fail("loaded tree has unexpected k2 presence");
    }
    std::vector<uint8_t> k3_value;
    const bool k3_present = tree.Get({'k', '3'}, &k3_value);
    if (k3_present != expect_k3_present) {
      Fail("loaded tree has unexpected k3 presence");
    }
    if (expect_k2_present && k2_value != std::vector<uint8_t>({'v', '2'})) {
      Fail("loaded tree has unexpected k2 value");
    }
    if (expect_k3_present && k3_value != std::vector<uint8_t>({'v', '3'})) {
      Fail("loaded tree has unexpected k3 value");
    }

    std::ifstream root_hash_file(root_hash_path);
    if (!root_hash_file) {
      Fail("failed to read rust root hash file");
    }
    std::string root_hash_hex;
    std::getline(root_hash_file, root_hash_hex);
    std::vector<uint8_t> expected_root_hash;
    if (!grovedb::DecodeHex(root_hash_hex, &expected_root_hash, &error)) {
      Fail("decode rust root hash failed: " + error);
    }
    std::vector<uint8_t> computed_root_hash;
    if (!tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &computed_root_hash, &error)) {
      Fail("compute c++ root hash failed: " + error);
    }
    if (computed_root_hash != expected_root_hash) {
      Fail("root hash mismatch vs rust");
    }

    std::filesystem::remove_all(dir);
  };

  run_case("merk",
           {{'r', 'o', 'o', 't'}},
           "root",
           {'v', '1'},
           true,
           false);
  run_case("merk_mut",
           {{'r', 'o', 'o', 't'}},
           "root",
           {'v', '1', 'x'},
           false,
           true);
  run_case("merk_child",
           {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
           "child",
           {'v', '1'},
           true,
           false);
  run_case("merk_child_mut",
           {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
           "child",
           {'v', '1', 'x'},
           false,
           true);
  return 0;
}
