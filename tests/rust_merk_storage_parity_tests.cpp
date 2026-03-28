#include "merk_storage.h"
#include "hex.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
std::string Hex(const std::vector<uint8_t>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::vector<uint8_t> ReadOrFail(grovedb::RocksDbWrapper* db,
                                grovedb::ColumnFamilyKind cf,
                                const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                const std::string& label) {
  std::string error;
  std::vector<uint8_t> value;
  bool found = false;
  if (!db->Get(cf, path, key, &value, &found, &error)) {
    Fail("read " + label + " failed: " + error);
  }
  if (!found) {
    Fail("missing " + label);
  }
  return value;
}

std::vector<uint8_t> ComputeRootHashOrFail(grovedb::RocksDbWrapper* db,
                                           const std::vector<std::vector<uint8_t>>& path) {
  std::string error;
  grovedb::MerkTree tree;
  if (!grovedb::MerkStorage::LoadTree(db, path, &tree, &error)) {
    Fail("load tree for root hash failed: " + error);
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }
  return root_hash;
}

std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> ExportEncodedNodesOrFail(
    grovedb::RocksDbWrapper* db, const std::vector<std::vector<uint8_t>>& path) {
  std::string error;
  grovedb::MerkTree tree;
  if (!grovedb::MerkStorage::LoadTree(db, path, &tree, &error)) {
    Fail("load tree for encoded export failed: " + error);
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
  std::vector<uint8_t> root_key;
  if (!tree.ExportEncodedNodes(&out, &root_key, tree.GetValueHashFn(), &error)) {
    Fail("export encoded nodes failed: " + error);
  }
  if (root_key.empty()) {
    Fail("export encoded nodes returned empty root key");
  }
  return out;
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  auto run_case = [](const std::string& mode,
                     const std::vector<std::vector<uint8_t>>& merk_path,
                     const std::string& root_hash_mode) {
    std::string dir = MakeTempDir("rust_merk_parity_" + mode);
    std::string error;

    const bool is_incremental_save = (mode == "merk_incremental_save");
    const bool is_tx_lifecycle = (mode == "merk_tx_lifecycle");
    const bool is_child_clear_lifecycle = (mode == "merk_child_clear_lifecycle");
    const bool is_lifecycle =
        (mode == "merk_lifecycle" || mode == "merk_child_lifecycle");
    const bool is_multi_reopen = (mode == "merk_multi_reopen");
    if (is_multi_reopen) {
      // Phase 1: insert k1=v1, k2=v2, commit
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (multi_reopen phase 1) failed: " + error);
        }
        grovedb::MerkTree tree_local;
        if (!tree_local.Insert({'k', '1'}, {'v', '1'}, &error) ||
            !tree_local.Insert({'k', '2'}, {'v', '2'}, &error)) {
          Fail("insert multi_reopen phase 1 tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &tree_local, &error)) {
          Fail("save multi_reopen phase 1 tree failed: " + error);
        }
      }
      // Phase 2: reopen, clear, reopen, insert k3=v3, commit
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (multi_reopen phase 2) failed: " + error);
        }
        grovedb::MerkTree loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("load multi_reopen phase 2 tree failed: " + error);
        }
        if (!grovedb::MerkStorage::ClearTree(&storage_local, merk_path, &error)) {
          Fail("clear multi_reopen tree failed: " + error);
        }
        // Reopen after clear and insert
        grovedb::MerkTree after_clear;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &after_clear, &error)) {
          Fail("load after clear multi_reopen failed: " + error);
        }
        if (!after_clear.Insert({'k', '3'}, {'v', '3'}, &error)) {
          Fail("insert multi_reopen phase 2 k3 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &after_clear, &error)) {
          Fail("save multi_reopen phase 2 tree failed: " + error);
        }
      }
      // Phase 3: reopen, verify only k3 exists (k1,k2 cleared)
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (multi_reopen phase 3) failed: " + error);
        }
        grovedb::MerkTree final_loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &final_loaded, &error)) {
          Fail("load multi_reopen phase 3 tree failed: " + error);
        }
        std::vector<uint8_t> v3;
        if (!final_loaded.Get({'k', '3'}, &v3) || v3 != std::vector<uint8_t>({'v', '3'})) {
          Fail("multi_reopen phase 3: k3 should be v3");
        }
        std::vector<uint8_t> should_be_missing;
        if (final_loaded.Get({'k', '1'}, &should_be_missing) || !should_be_missing.empty()) {
          Fail("multi_reopen phase 3: k1 should not exist");
        }
        if (final_loaded.Get({'k', '2'}, &should_be_missing) || !should_be_missing.empty()) {
          Fail("multi_reopen phase 3: k2 should not exist");
        }
      }
    } else if (is_incremental_save) {
      // Phase 1: insert k1=v1, k2=v2, k3=v3
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (incremental phase 1) failed: " + error);
        }
        grovedb::MerkTree tree_local;
        if (!tree_local.Insert({'k', '1'}, {'v', '1'}, &error) ||
            !tree_local.Insert({'k', '2'}, {'v', '2'}, &error) ||
            !tree_local.Insert({'k', '3'}, {'v', '3'}, &error)) {
          Fail("insert incremental phase 1 tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &tree_local, &error)) {
          Fail("save incremental phase 1 tree failed: " + error);
        }
      }
      // Phase 2: reopen, replace k1=v1m, delete k3, insert k4=v4
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (incremental phase 2) failed: " + error);
        }
        grovedb::MerkTree loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("load incremental phase 2 tree failed: " + error);
        }
        if (!loaded.Insert({'k', '1'}, {'v', '1', 'm'}, &error)) {
          Fail("replace incremental k1 failed: " + error);
        }
        bool deleted = false;
        if (!loaded.Delete({'k', '3'}, &deleted, &error)) {
          Fail("delete incremental k3 failed: " + error);
        }
        if (!deleted) {
          Fail("incremental k3 delete did not report deleted");
        }
        if (!loaded.Insert({'k', '4'}, {'v', '4'}, &error)) {
          Fail("insert incremental k4 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("save incremental phase 2 tree failed: " + error);
        }
      }
      // Phase 3: reopen, replace k2=v2m, insert k5=v5
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (incremental phase 3) failed: " + error);
        }
        grovedb::MerkTree loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("load incremental phase 3 tree failed: " + error);
        }
        if (!loaded.Insert({'k', '2'}, {'v', '2', 'm'}, &error)) {
          Fail("replace incremental k2 failed: " + error);
        }
        if (!loaded.Insert({'k', '5'}, {'v', '5'}, &error)) {
          Fail("insert incremental k5 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("save incremental phase 3 tree failed: " + error);
        }
      }
    } else if (is_tx_lifecycle) {
      // Phase 1: insert k1,k2 and persist.
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (tx lifecycle phase 1) failed: " + error);
        }
        grovedb::MerkTree tree_local;
        if (!tree_local.Insert({'k', '1'}, {'v', '1'}, &error) ||
            !tree_local.Insert({'k', '2'}, {'v', '2'}, &error)) {
          Fail("insert tx lifecycle phase 1 tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &tree_local, &error)) {
          Fail("save tx lifecycle phase 1 tree failed: " + error);
        }
      }
      // Phase 2: reopen, mutate (replace k1->v1m, delete k2, insert k3), persist.
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (tx lifecycle phase 2) failed: " + error);
        }
        grovedb::MerkTree loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("load tx lifecycle phase 2 tree failed: " + error);
        }
        // Replace k1 value.
        if (!loaded.Insert({'k', '1'}, {'v', '1', 'm'}, &error)) {
          Fail("replace tx lifecycle k1 failed: " + error);
        }
        // Delete k2.
        bool deleted = false;
        if (!loaded.Delete({'k', '2'}, &deleted, &error)) {
          Fail("delete tx lifecycle k2 failed: " + error);
        }
        if (!deleted) {
          Fail("tx lifecycle k2 delete did not report deleted");
        }
        // Insert k3.
        if (!loaded.Insert({'k', '3'}, {'v', '3'}, &error)) {
          Fail("insert tx lifecycle k3 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &loaded, &error)) {
          Fail("save tx lifecycle phase 2 tree failed: " + error);
        }
      }
    } else if (is_child_clear_lifecycle) {
      const std::vector<std::vector<uint8_t>> parent_path = {{'r', 'o', 'o', 't'}};
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (child clear lifecycle initial) failed: " + error);
        }
        grovedb::MerkTree parent_tree;
        if (!parent_tree.Insert({'k', '1'}, {'v', '1'}, &error) ||
            !parent_tree.Insert({'k', '2'}, {'v', '2'}, &error)) {
          Fail("insert child clear lifecycle parent tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, parent_path, &parent_tree, &error)) {
          Fail("save child clear lifecycle parent tree failed: " + error);
        }
        grovedb::MerkTree child_tree;
        if (!child_tree.Insert({'c', 'k', '1'}, {'c', 'v', '1'}, &error) ||
            !child_tree.Insert({'c', 'k', '2'}, {'c', 'v', '2'}, &error)) {
          Fail("insert child clear lifecycle child tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &child_tree, &error)) {
          Fail("save child clear lifecycle child tree failed: " + error);
        }
      }
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (child clear lifecycle clear) failed: " + error);
        }
        if (!grovedb::MerkStorage::ClearTree(&storage_local, merk_path, &error)) {
          Fail("clear child clear lifecycle child tree failed: " + error);
        }
      }
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (child clear lifecycle reinsert) failed: " + error);
        }
        grovedb::MerkTree child_after_clear;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &child_after_clear, &error)) {
          Fail("load child clear lifecycle child tree after clear failed: " + error);
        }
        std::vector<uint8_t> should_be_missing;
        if (child_after_clear.Get({'c', 'k', '1'}, &should_be_missing) ||
            child_after_clear.Get({'c', 'k', '2'}, &should_be_missing)) {
          Fail("child clear lifecycle child tree should be empty after clear");
        }
        grovedb::MerkTree reinserted_child;
        if (!reinserted_child.Insert({'c', 'k', '3'}, {'c', 'v', '3'}, &error)) {
          Fail("insert child clear lifecycle ck3 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &reinserted_child, &error)) {
          Fail("save child clear lifecycle child tree after reinsert failed: " + error);
        }
      }
    } else if (is_lifecycle) {
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (lifecycle initial) failed: " + error);
        }
        grovedb::MerkTree tree_local;
        if (!tree_local.Insert({'k', '1'}, {'v', '1'}, &error) ||
            !tree_local.Insert({'k', '2'}, {'v', '2'}, &error)) {
          Fail("insert lifecycle initial tree failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &tree_local, &error)) {
          Fail("save lifecycle initial tree failed: " + error);
        }
      }
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (lifecycle clear) failed: " + error);
        }
        grovedb::MerkTree clear_loaded;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &clear_loaded, &error)) {
          Fail("load lifecycle tree before clear failed: " + error);
        }
        std::vector<uint8_t> pre_clear_value;
        if (!clear_loaded.Get({'k', '1'}, &pre_clear_value) ||
            pre_clear_value != std::vector<uint8_t>({'v', '1'})) {
          Fail("lifecycle pre-clear state mismatch");
        }
        if (!grovedb::MerkStorage::ClearTree(&storage_local, merk_path, &error)) {
          Fail("clear lifecycle tree failed: " + error);
        }
      }
      {
        grovedb::RocksDbWrapper storage_local;
        if (!storage_local.Open(dir, &error)) {
          Fail("open storage (lifecycle final) failed: " + error);
        }
        grovedb::MerkTree after_clear;
        if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &after_clear, &error)) {
          Fail("load lifecycle tree after clear failed: " + error);
        }
        std::vector<uint8_t> should_be_missing;
        if (after_clear.Get({'k', '1'}, &should_be_missing) ||
            after_clear.Get({'k', '2'}, &should_be_missing)) {
          Fail("lifecycle tree should be empty after clear");
        }
        grovedb::MerkTree final_tree;
        if (!final_tree.Insert({'k', '1'}, {'v', '1', 'r'}, &error)) {
          Fail("insert lifecycle reloaded k1 failed: " + error);
        }
        if (!final_tree.Insert({'k', '3'}, {'v', '3'}, &error)) {
          Fail("insert lifecycle reloaded k3 failed: " + error);
        }
        if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &final_tree, &error)) {
          Fail("save lifecycle final tree failed: " + error);
        }
      }
    } else {
      grovedb::RocksDbWrapper storage_local;
      if (!storage_local.Open(dir, &error)) {
        Fail("open storage failed: " + error);
      }

      grovedb::MerkTree tree_local;
      if (!tree_local.Insert({'k', '1'}, {'v', '1'}, &error) ||
          !tree_local.Insert({'k', '2'}, {'v', '2'}, &error)) {
        Fail("insert tree failed: " + error);
      }
      const bool is_mut = (mode == "merk_mut" || mode == "merk_child_mut");
      if (is_mut) {
        bool deleted = false;
        if (!tree_local.Insert({'k', '1'}, {'v', '1', 'x'}, &error)) {
          Fail("replace tree key failed: " + error);
        }
        if (!tree_local.Delete({'k', '2'}, &deleted, &error)) {
          Fail("delete tree key failed: " + error);
        }
        if (!deleted) {
          Fail("delete tree key did not report deleted");
        }
        if (!tree_local.Insert({'k', '3'}, {'v', '3'}, &error)) {
          Fail("insert tree k3 failed: " + error);
        }
      }
      if (!grovedb::MerkStorage::SaveTree(&storage_local, merk_path, &tree_local, &error)) {
        Fail("save tree failed: " + error);
      }
    }

    std::string cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_reader \"" +
        dir + "\" " + mode;
    if (std::system(cmd.c_str()) != 0) {
      Fail("failed to run rust storage reader " + mode);
    }

    const std::string root_hash_path = dir + "/root_hash_cpp_written.txt";
    cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_merk_root_hash \"" +
        dir + "\" \"" + root_hash_path + "\" " + root_hash_mode;
    if (std::system(cmd.c_str()) != 0) {
      Fail("failed to run rust merk root hash");
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

    grovedb::RocksDbWrapper storage_local;
    if (!storage_local.Open(dir, &error)) {
      Fail("reopen storage failed: " + error);
    }
    grovedb::MerkTree tree_local;
    if (!grovedb::MerkStorage::LoadTree(&storage_local, merk_path, &tree_local, &error)) {
      Fail("reload tree failed: " + error);
    }
    std::vector<uint8_t> computed_root_hash;
    if (!tree_local.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &computed_root_hash, &error)) {
      Fail("compute c++ root hash failed: " + error);
    }
    if (computed_root_hash != expected_root_hash) {
      Fail("root hash mismatch vs rust for c++ persisted merk");
    }
    if (is_lifecycle) {
      std::filesystem::remove_all(dir);
      return;
    }

    const std::string rust_dir = MakeTempDir("rust_merk_parity_rust_" + mode);
    cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        rust_dir + "\" " + mode;
    if (std::system(cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer " + mode);
    }

    grovedb::RocksDbWrapper rust_storage;
    if (!rust_storage.Open(rust_dir, &error)) {
      Fail("open rust-written storage failed: " + error);
    }

    std::vector<uint8_t> cpp_root_key =
        ReadOrFail(&storage_local, grovedb::ColumnFamilyKind::kRoots, merk_path, {'r'}, "cpp root key");
    if (std::getenv("GROVEDB_DEBUG_MERK_ROOT_KEY_TEST") != nullptr) {
      std::array<uint8_t, 32> prefix{};
      std::string prefix_error;
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> roots_scanned;
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> default_scanned;
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> meta_scanned;
      std::string roots_scan_error;
      std::string default_scan_error;
      std::string meta_scan_error;
      std::string prefix_hex = "build_prefix_failed";
      if (grovedb::RocksDbWrapper::BuildPrefix(merk_path, &prefix, &prefix_error)) {
        prefix_hex = Hex(std::vector<uint8_t>(prefix.begin(), prefix.end()));
      } else {
        prefix_hex += ":" + prefix_error;
      }
      bool roots_scan_ok = rust_storage.Context(grovedb::ColumnFamilyKind::kRoots, merk_path)
                               .Scan(&roots_scanned, &roots_scan_error);
      bool default_scan_ok = rust_storage.Context(grovedb::ColumnFamilyKind::kDefault, merk_path)
                                 .Scan(&default_scanned, &default_scan_error);
      bool meta_scan_ok = rust_storage.Context(grovedb::ColumnFamilyKind::kMeta, merk_path)
                              .Scan(&meta_scanned, &meta_scan_error);
      std::cerr << "MERK_ROOT_DEBUG prefix=" << prefix_hex
                << " roots_ok=" << (roots_scan_ok ? "1" : "0")
                << " roots_n=" << roots_scanned.size()
                << " default_ok=" << (default_scan_ok ? "1" : "0")
                << " default_n=" << default_scanned.size()
                << " meta_ok=" << (meta_scan_ok ? "1" : "0")
                << " meta_n=" << meta_scanned.size();
      if (!roots_scan_ok) {
        std::cerr << " roots_err=" << roots_scan_error;
      }
      if (!default_scan_ok) {
        std::cerr << " default_err=" << default_scan_error;
      }
      if (!meta_scan_ok) {
        std::cerr << " meta_err=" << meta_scan_error;
      }
      for (size_t i = 0; i < roots_scanned.size() && i < 8; ++i) {
        std::cerr << " roots[" << i << "]k=" << Hex(roots_scanned[i].first)
                  << " v=" << Hex(roots_scanned[i].second);
      }
      for (size_t i = 0; i < default_scanned.size() && i < 8; ++i) {
        std::cerr << " default[" << i << "]k=" << Hex(default_scanned[i].first)
                  << " v=" << Hex(default_scanned[i].second);
      }
      for (size_t i = 0; i < meta_scanned.size() && i < 8; ++i) {
        std::cerr << " meta[" << i << "]k=" << Hex(meta_scanned[i].first)
                  << " v=" << Hex(meta_scanned[i].second);
      }
      std::cerr << "\n";
    }
    std::vector<uint8_t> rust_root_key = ReadOrFail(
        &rust_storage, grovedb::ColumnFamilyKind::kRoots, merk_path, {'r'}, "rust root key");

    std::vector<uint8_t> cpp_root_node = ReadOrFail(
        &storage_local, grovedb::ColumnFamilyKind::kDefault, merk_path, cpp_root_key, "cpp root node");
    std::vector<uint8_t> rust_root_node =
        ReadOrFail(&rust_storage,
                   grovedb::ColumnFamilyKind::kDefault,
                   merk_path,
                   rust_root_key,
                   "rust root node");

    if (cpp_root_key != rust_root_key) {
      Fail("root key bytes mismatch between c++ and rust merk persistence");
    }
    if (cpp_root_node != rust_root_node) {
      Fail("encoded root node bytes mismatch between c++ and rust merk persistence");
    }

    const std::vector<uint8_t> cpp_written_hash = ComputeRootHashOrFail(&storage_local, merk_path);
    const std::vector<uint8_t> rust_written_hash = ComputeRootHashOrFail(&rust_storage, merk_path);
    if (cpp_written_hash != rust_written_hash) {
      Fail("semantic root hash mismatch between c++ and rust merk persistence");
    }

    const auto cpp_encoded_nodes = ExportEncodedNodesOrFail(&storage_local, merk_path);
    const auto rust_encoded_nodes = ExportEncodedNodesOrFail(&rust_storage, merk_path);
    if (cpp_encoded_nodes != rust_encoded_nodes) {
      Fail("encoded node set mismatch between c++ and rust merk persistence");
    }

    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(rust_dir);
  };

  run_case("merk", {{'r', 'o', 'o', 't'}}, "root");
  run_case("merk_child", {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, "child");
  run_case("merk_mut", {{'r', 'o', 'o', 't'}}, "root");
  run_case("merk_child_mut", {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, "child");
  run_case("merk_lifecycle", {{'r', 'o', 'o', 't'}}, "root");
  run_case("merk_child_lifecycle", {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, "child");
  run_case("merk_child_clear_lifecycle", {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, "child");
  run_case("merk_tx_lifecycle", {{'r', 'o', 'o', 't'}}, "root");
  run_case("merk_incremental_save", {{'r', 'o', 'o', 't'}}, "root");
  run_case("merk_multi_reopen", {{'r', 'o', 'o', 't'}}, "root");
  return 0;
}
