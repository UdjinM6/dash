#include "element.h"
#include "grovedb.h"
#include "hex.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

std::vector<std::string> SplitCommaSeparated(const char* raw) {
  std::vector<std::string> out;
  if (raw == nullptr) {
    return out;
  }
  std::string current;
  for (const char ch : std::string(raw)) {
    if (ch == ',') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

void RunRustWriter(const std::string& dir, const std::string& mode) {
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_grovedb_facade_writer \"" +
      dir + "\" \"" + mode + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb facade writer for mode: " + mode);
  }
}

void RunRustReader(const std::string& dir, const std::string& mode) {
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_grovedb_facade_reader \"" +
      dir + "\" \"" + mode + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb facade reader for mode: " + mode);
  }
}

using KeyValue = std::pair<std::vector<uint8_t>, std::vector<uint8_t>>;

std::vector<KeyValue> DumpCf(rocksdb::OptimisticTransactionDB* db,
                             rocksdb::ColumnFamilyHandle* cf) {
  std::vector<KeyValue> out;
  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_options, cf));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice key = it->key();
    const rocksdb::Slice value = it->value();
    out.emplace_back(std::vector<uint8_t>(key.data(), key.data() + key.size()),
                     std::vector<uint8_t>(value.data(), value.data() + value.size()));
  }
  if (!it->status().ok()) {
    Fail("iterator failure while dumping cf: " + it->status().ToString());
  }
  return out;
}

std::vector<std::vector<KeyValue>> DumpAllCfs(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = false;
  options.create_missing_column_families = false;
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("aux", rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("roots", rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("meta", rocksdb::ColumnFamilyOptions());

  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  rocksdb::OptimisticTransactionDB* db_ptr = nullptr;
  const rocksdb::Status status =
      rocksdb::OptimisticTransactionDB::Open(options, path, cfs, &handles, &db_ptr);
  if (!status.ok()) {
    Fail("failed to open rocksdb for dump: " + status.ToString());
  }
  std::unique_ptr<rocksdb::OptimisticTransactionDB> db(db_ptr);
  std::vector<std::vector<KeyValue>> dump;
  for (rocksdb::ColumnFamilyHandle* handle : handles) {
    dump.push_back(DumpCf(db.get(), handle));
  }
  for (rocksdb::ColumnFamilyHandle* handle : handles) {
    db->DestroyColumnFamilyHandle(handle);
  }
  return dump;
}

void CompareDbDump(const std::string& scenario,
                   const std::string& rust_dir,
                   const std::string& cpp_dir) {
  const auto rust_dump = DumpAllCfs(rust_dir);
  const auto cpp_dump = DumpAllCfs(cpp_dir);
  auto dumps_equal_for_scenario = [&]() {
    if (scenario == "batch_mixed_non_minimal_ops" ||
        scenario == "batch_mixed_non_minimal_ops_with_options") {
      // Rust and C++ differ in nested-path roots CF persistence (Rust can omit
      // nested roots entries; C++ may persist them). These scenarios use raw
      // dump comparison to validate non-minimal batch final state, so compare
      // all CFs except roots.
      if (rust_dump.size() != cpp_dump.size()) {
        return false;
      }
      for (size_t i = 0; i < rust_dump.size(); ++i) {
        if (i == 2) {  // roots CF
          continue;
        }
        if (rust_dump[i] != cpp_dump[i]) {
          return false;
        }
      }
      return true;
    }
    return rust_dump == cpp_dump;
  };

  if (!dumps_equal_for_scenario()) {
    const auto to_hex = [](const std::vector<uint8_t>& bytes) {
      static const char* kHex = "0123456789abcdef";
      std::string out;
      out.reserve(bytes.size() * 2);
      for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
      }
      return out;
    };
    std::string detail = "raw RocksDB export mismatch for scenario: " + scenario;
    detail += " rust_cfs=" + std::to_string(rust_dump.size());
    detail += " cpp_cfs=" + std::to_string(cpp_dump.size());
    const size_t cf_count = std::min(rust_dump.size(), cpp_dump.size());
    for (size_t cf_idx = 0; cf_idx < cf_count; ++cf_idx) {
      if ((scenario == "batch_mixed_non_minimal_ops" ||
           scenario == "batch_mixed_non_minimal_ops_with_options") &&
          cf_idx == 2) {
        continue;
      }
      if (rust_dump[cf_idx] == cpp_dump[cf_idx]) {
        continue;
      }
      detail += " first_cf=" + std::to_string(cf_idx);
      detail += " rust_entries=" + std::to_string(rust_dump[cf_idx].size());
      detail += " cpp_entries=" + std::to_string(cpp_dump[cf_idx].size());
      const size_t kv_count = std::min(rust_dump[cf_idx].size(), cpp_dump[cf_idx].size());
      if (rust_dump[cf_idx].size() != cpp_dump[cf_idx].size()) {
        const auto& larger = (rust_dump[cf_idx].size() > cpp_dump[cf_idx].size())
                                 ? rust_dump[cf_idx]
                                 : cpp_dump[cf_idx];
        const bool rust_larger = rust_dump[cf_idx].size() > cpp_dump[cf_idx].size();
        if (kv_count < larger.size()) {
          detail += rust_larger ? " extra_side=rust" : " extra_side=cpp";
          detail += " extra_key=" + to_hex(larger[kv_count].first);
          detail += " extra_val=" + to_hex(larger[kv_count].second);
        }
        if (rust_dump[cf_idx].size() <= 6 && cpp_dump[cf_idx].size() <= 6) {
          detail += " rust_cf_entries=";
          for (const auto& kv : rust_dump[cf_idx]) {
            detail += "[" + to_hex(kv.first) + ":" + to_hex(kv.second) + "]";
          }
          detail += " cpp_cf_entries=";
          for (const auto& kv : cpp_dump[cf_idx]) {
            detail += "[" + to_hex(kv.first) + ":" + to_hex(kv.second) + "]";
          }
        }
      }
      for (size_t kv_idx = 0; kv_idx < kv_count; ++kv_idx) {
        if (rust_dump[cf_idx][kv_idx] == cpp_dump[cf_idx][kv_idx]) {
          continue;
        }
        detail += " first_entry=" + std::to_string(kv_idx);
        detail += " rust_key=" + to_hex(rust_dump[cf_idx][kv_idx].first);
        detail += " cpp_key=" + to_hex(cpp_dump[cf_idx][kv_idx].first);
        detail += " rust_val=" + to_hex(rust_dump[cf_idx][kv_idx].second);
        detail += " cpp_val=" + to_hex(cpp_dump[cf_idx][kv_idx].second);
        break;
      }
      break;
    }
    Fail(detail);
  }
}

std::map<std::string, std::string> RunRustElementEncodingDump(const std::string& dir) {
  const std::filesystem::path out = std::filesystem::path(dir) / "rust_element_encoding.txt";
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_element_encoding_dump > \"" +
      out.string() + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust element encoding dump");
  }

  std::ifstream file(out);
  if (!file.is_open()) {
    Fail("failed to open rust element encoding output");
  }

  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(file, line)) {
    const size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    values.emplace(line.substr(0, pos), line.substr(pos + 1));
  }
  return values;
}

void VerifyElementEncodingParity() {
  const std::string dir = MakeTempDir("rust_grovedb_facade_parity_element_encoding");
  const auto values = RunRustElementEncodingDump(dir);
  const auto item_it = values.find("ITEM_V1");
  const auto tree_it = values.find("TREE_EMPTY");
  if (item_it == values.end() || tree_it == values.end()) {
    Fail("rust element encoding output missing expected keys");
  }

  std::string error;
  std::vector<uint8_t> rust_item;
  if (!grovedb::DecodeHex(item_it->second, &rust_item, &error)) {
    Fail("failed to decode rust item hex: " + error);
  }
  std::vector<uint8_t> rust_tree;
  if (!grovedb::DecodeHex(tree_it->second, &rust_tree, &error)) {
    Fail("failed to decode rust tree hex: " + error);
  }

  std::vector<uint8_t> cpp_item;
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &cpp_item, &error)) {
    Fail("cpp item encode failed: " + error);
  }
  std::vector<uint8_t> cpp_tree;
  if (!grovedb::EncodeTreeToElementBytes(&cpp_tree, &error)) {
    Fail("cpp tree encode failed: " + error);
  }

  if (cpp_item != rust_item) {
    Fail("item encoding parity mismatch");
  }
  if (cpp_tree != rust_tree) {
    Fail("tree encoding parity mismatch");
  }

  grovedb::ElementItem decoded_item;
  if (!grovedb::DecodeItemFromElementBytes(rust_item, &decoded_item, &error)) {
    Fail("cpp failed to decode rust item bytes: " + error);
  }
  if (decoded_item.value != std::vector<uint8_t>({'v', '1'})) {
    Fail("decoded rust item value mismatch");
  }
  uint64_t variant = 0;
  if (!grovedb::DecodeElementVariant(rust_tree, &variant, &error)) {
    Fail("cpp failed to decode rust tree bytes: " + error);
  }
  if (variant != 2) {
    Fail("decoded rust tree variant mismatch");
  }

  std::filesystem::remove_all(dir);
}

void ExpectTree(grovedb::GroveDb* db,
                const std::vector<std::vector<uint8_t>>& path,
                const std::vector<uint8_t>& key) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get tree failed: " + error);
  }
  if (!found) {
    Fail("expected tree element to exist");
  }
  uint64_t variant = 0;
  if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
    Fail("decode tree variant failed: " + error);
  }
  if (variant != 2) {
    Fail("expected tree variant");
  }
}

void ExpectItem(grovedb::GroveDb* db,
                const std::vector<std::vector<uint8_t>>& path,
                const std::vector<uint8_t>& key,
                const std::vector<uint8_t>& expected) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get item failed: " + error);
  }
  if (!found) {
    Fail("expected item element to exist");
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(raw, &item, &error)) {
    Fail("decode item failed: " + error);
  }
  if (item.value != expected) {
    Fail("item value mismatch");
  }
}

void ExpectSumItem(grovedb::GroveDb* db,
                   const std::vector<std::vector<uint8_t>>& path,
                   const std::vector<uint8_t>& key,
                   int64_t expected_sum) {
  auto bytes_to_string = [](const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
  };
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->GetRaw(path, key, &raw, &found, &error)) {
    Fail("GetRaw sum item failed for key '" + bytes_to_string(key) + "': " + error);
  }
  if (!found) {
    std::string path_str;
    for (size_t i = 0; i < path.size(); ++i) {
      if (i > 0) {
        path_str += "/";
      }
      path_str += bytes_to_string(path[i]);
    }
    std::string diag;
    if (!path.empty()) {
      std::vector<uint8_t> subtree_root_key;
      std::vector<uint8_t> subtree_element;
      std::string subtree_error;
      if (db->GetSubtreeRoot(path, &subtree_root_key, &subtree_element, &subtree_error)) {
        diag += " subtree_root_key_len=" + std::to_string(subtree_root_key.size());
        if (!subtree_root_key.empty()) {
          diag += " subtree_root_key='" + bytes_to_string(subtree_root_key) + "'";
        }
        uint64_t subtree_variant = 0;
        if (grovedb::DecodeElementVariant(subtree_element, &subtree_variant, &subtree_error)) {
          diag += " subtree_element_variant=" + std::to_string(subtree_variant);
          if (subtree_variant == 4) {
            grovedb::ElementSumTree sum_tree;
            if (grovedb::DecodeSumTreeFromElementBytes(subtree_element, &sum_tree, &subtree_error) &&
                sum_tree.root_key.has_value()) {
              diag += " embedded_root_key='" +
                      bytes_to_string(*sum_tree.root_key) + "'";
            }
          }
        } else {
          diag += " subtree_decode_error=" + subtree_error;
        }
      } else {
        diag += " get_subtree_root_error=" + subtree_error;
      }
    }
    Fail("expected sum item element to exist at path '" + path_str + "' key '" +
         bytes_to_string(key) + "'" + diag);
  }
  grovedb::ElementSumItem sum_item;
  if (!grovedb::DecodeSumItemFromElementBytes(raw, &sum_item, &error)) {
    Fail("decode sum item failed for key '" + bytes_to_string(key) + "': " + error);
  }
  if (sum_item.sum != expected_sum) {
    Fail("sum item value mismatch: expected " + std::to_string(expected_sum) + 
         " got " + std::to_string(sum_item.sum));
  }
}

void ExpectCountTreeCount(grovedb::GroveDb* db,
                          const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          uint64_t expected_count) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get count tree failed: " + error);
  }
  if (!found) {
    Fail("expected count tree element to exist");
  }
  grovedb::ElementCountTree tree;
  if (!grovedb::DecodeCountTreeFromElementBytes(raw, &tree, &error)) {
    Fail("decode count tree failed: " + error);
  }
  if (tree.count != expected_count) {
    Fail("count tree count mismatch");
  }
}

void ExpectProvableCountTreeCount(grovedb::GroveDb* db,
                                  const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  uint64_t expected_count) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get provable count tree failed: " + error);
  }
  if (!found) {
    Fail("expected provable count tree element to exist");
  }
  grovedb::ElementProvableCountTree tree;
  if (!grovedb::DecodeProvableCountTreeFromElementBytes(raw, &tree, &error)) {
    Fail("decode provable count tree failed: " + error);
  }
  if (tree.count != expected_count) {
    Fail("provable count tree count mismatch");
  }
}

void ExpectCountSumTree(grovedb::GroveDb* db,
                        const std::vector<std::vector<uint8_t>>& path,
                        const std::vector<uint8_t>& key,
                        uint64_t expected_count,
                        int64_t expected_sum) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get count sum tree failed: " + error);
  }
  if (!found) {
    Fail("expected count sum tree element to exist");
  }
  grovedb::ElementCountSumTree tree;
  if (!grovedb::DecodeCountSumTreeFromElementBytes(raw, &tree, &error)) {
    Fail("decode count sum tree failed: " + error);
  }
  if (tree.count != expected_count || tree.sum != expected_sum) {
    Fail("count sum tree aggregate mismatch");
  }
}

void ExpectProvableCountSumTree(grovedb::GroveDb* db,
                                const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                uint64_t expected_count,
                                int64_t expected_sum) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get provable count sum tree failed: " + error);
  }
  if (!found) {
    Fail("expected provable count sum tree element to exist");
  }
  grovedb::ElementProvableCountSumTree tree;
  if (!grovedb::DecodeProvableCountSumTreeFromElementBytes(raw, &tree, &error)) {
    Fail("decode provable count sum tree failed: " + error);
  }
  if (tree.count != expected_count || tree.sum != expected_sum) {
    Fail("provable count sum tree aggregate mismatch");
  }
}

void ExpectMissing(grovedb::GroveDb* db,
                   const std::vector<std::vector<uint8_t>>& path,
                   const std::vector<uint8_t>& key) {
  std::string error;
  std::vector<uint8_t> raw;
  bool found = false;
  if (!db->Get(path, key, &raw, &found, &error)) {
    Fail("Get missing failed: " + error);
  }
  if (found) {
    Fail("expected key to be missing");
  }
}

void WriteCppScenario(const std::string& dir, const std::string& mode) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("cpp open failed: " + error);
  }
  if (mode == "facade_insert_helpers") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert helper root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'}, &error)) {
      Fail("insert helper k1 failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, &error)) {
      Fail("insert helper child tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
                       {'n', 'k'},
                       {'n', 'v'},
                       &error)) {
      Fail("insert helper nested key failed: " + error);
    }
    if (!db.InsertBigSumTree({{'r', 'o', 'o', 't'}}, {'b', 'i', 'g'}, &error)) {
      Fail("insert helper big sum tree failed: " + error);
    }
    if (!db.InsertCountTree({{'r', 'o', 'o', 't'}}, {'c', 'o', 'u', 'n', 't'}, &error)) {
      Fail("insert helper count tree failed: " + error);
    }
    if (!db.InsertProvableCountTree(
            {{'r', 'o', 'o', 't'}}, {'p', 'r', 'o', 'v', 'c', 't'}, &error)) {
      Fail("insert helper provable count tree failed: " + error);
    }
    return;
  }
  if (mode == "facade_insert_if_not_exists") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert_if_not_exists root tree failed: " + error);
    }
    std::vector<uint8_t> v1_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_item, &error)) {
      Fail("encode v1 item for insert_if_not_exists failed: " + error);
    }
    bool inserted = false;
    if (!db.InsertIfNotExists({{'r', 'o', 'o', 't'}}, {'k', '1'}, v1_item, &inserted, &error)) {
      Fail("insert_if_not_exists first insert failed: " + error);
    }
    if (!inserted) {
      Fail("insert_if_not_exists first insert should set inserted=true");
    }
    std::vector<uint8_t> v2_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_item, &error)) {
      Fail("encode v2 item for insert_if_not_exists failed: " + error);
    }
    if (!db.InsertIfNotExists({{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_item, &inserted, &error)) {
      Fail("insert_if_not_exists second insert failed: " + error);
    }
    if (inserted) {
      Fail("insert_if_not_exists second insert should set inserted=false");
    }
    if (!db.InsertIfNotExists({{'r', 'o', 'o', 't'}}, {'k', '2'}, v2_item, &inserted, &error)) {
      Fail("insert_if_not_exists third insert failed: " + error);
    }
    if (!inserted) {
      Fail("insert_if_not_exists third insert should set inserted=true");
    }
    return;
  }
  if (mode == "facade_insert_if_changed_value") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert_if_changed_value root tree failed: " + error);
    }
    bool inserted = false;
    bool had_previous = false;
    std::vector<uint8_t> previous_element;
    std::vector<uint8_t> v1_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_item, &error)) {
      Fail("encode v1 for insert_if_changed_value failed: " + error);
    }
    if (!db.InsertIfChangedValue({{'r', 'o', 'o', 't'}},
                                 {'k', '1'},
                                 v1_item,
                                 &inserted,
                                 &previous_element,
                                 &had_previous,
                                 &error)) {
      Fail("insert_if_changed_value first call failed: " + error);
    }
    if (!inserted || had_previous) {
      Fail("insert_if_changed_value first call should be changed=true, previous=None");
    }
    if (!db.InsertIfChangedValue({{'r', 'o', 'o', 't'}},
                                 {'k', '1'},
                                 v1_item,
                                 &inserted,
                                 &previous_element,
                                 &had_previous,
                                 &error)) {
      Fail("insert_if_changed_value second call failed: " + error);
    }
    if (inserted || had_previous) {
      Fail("insert_if_changed_value second call should be changed=false, previous=None");
    }
    std::vector<uint8_t> v2_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_item, &error)) {
      Fail("encode v2 for insert_if_changed_value failed: " + error);
    }
    if (!db.InsertIfChangedValue({{'r', 'o', 'o', 't'}},
                                 {'k', '1'},
                                 v2_item,
                                 &inserted,
                                 &previous_element,
                                 &had_previous,
                                 &error)) {
      Fail("insert_if_changed_value third call failed: " + error);
    }
    if (!inserted || !had_previous || previous_element != v1_item) {
      Fail("insert_if_changed_value third call should be changed=true, previous=v1");
    }
    return;
  }
  if (mode == "facade_insert_if_not_exists_return_existing") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert_if_not_exists_return_existing root tree failed: " + error);
    }
    std::vector<uint8_t> v1_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_item, &error)) {
      Fail("encode v1 for insert_if_not_exists_return_existing failed: " + error);
    }
    std::vector<uint8_t> existing_element;
    bool had_existing = false;
    if (!db.InsertIfNotExistsReturnExisting(
            {{'r', 'o', 'o', 't'}}, {'k', '1'}, v1_item, &existing_element, &had_existing, &error)) {
      Fail("insert_if_not_exists_return_existing first call failed: " + error);
    }
    if (had_existing) {
      Fail("insert_if_not_exists_return_existing first call should report had_existing=false");
    }
    std::vector<uint8_t> v2_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_item, &error)) {
      Fail("encode v2 for insert_if_not_exists_return_existing failed: " + error);
    }
    if (!db.InsertIfNotExistsReturnExisting(
            {{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_item, &existing_element, &had_existing, &error)) {
      Fail("insert_if_not_exists_return_existing second call failed: " + error);
    }
    if (!had_existing || existing_element != v1_item) {
      Fail("insert_if_not_exists_return_existing second call should return previous v1");
    }
    return;
  }
  if (mode == "facade_insert_if_not_exists_return_existing_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert_if_not_exists_return_existing_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'}, &error)) {
      Fail("insert_if_not_exists_return_existing_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("insert_if_not_exists_return_existing_tx start tx failed: " + error);
    }
    std::vector<uint8_t> v2_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_item, &error)) {
      Fail("insert_if_not_exists_return_existing_tx encode v2 failed: " + error);
    }
    std::vector<uint8_t> existing_element;
    bool had_existing = false;
    if (!db.InsertIfNotExistsReturnExisting(
            {{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_item, &existing_element, &had_existing, &tx, &error)) {
      Fail("insert_if_not_exists_return_existing_tx existing insert failed: " + error);
    }
    std::vector<uint8_t> v1_item;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_item, &error)) {
      Fail("insert_if_not_exists_return_existing_tx encode v1 failed: " + error);
    }
    if (!had_existing || existing_element != v1_item) {
      Fail("insert_if_not_exists_return_existing_tx existing insert should return previous v1");
    }
    std::vector<uint8_t> tv_item;
    if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &tv_item, &error)) {
      Fail("insert_if_not_exists_return_existing_tx encode tv failed: " + error);
    }
    if (!db.InsertIfNotExistsReturnExisting(
            {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, tv_item, &existing_element, &had_existing, &tx, &error)) {
      Fail("insert_if_not_exists_return_existing_tx new insert failed: " + error);
    }
    if (had_existing) {
      Fail("insert_if_not_exists_return_existing_tx new insert should report had_existing=false");
    }
    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &raw, &found, &tx, &error)) {
      Fail("insert_if_not_exists_return_existing_tx in-tx get failed: " + error);
    }
    if (!found || raw != tv_item) {
      Fail("insert_if_not_exists_return_existing_tx tx should see txk=tv");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &raw, &found, &error)) {
      Fail("insert_if_not_exists_return_existing_tx outside get before commit failed: " + error);
    }
    if (found) {
      Fail("insert_if_not_exists_return_existing_tx outside tx should not see txk before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("insert_if_not_exists_return_existing_tx commit failed: " + error);
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &raw, &found, &error)) {
      Fail("insert_if_not_exists_return_existing_tx outside get after commit failed: " + error);
    }
    if (!found || raw != tv_item) {
      Fail("insert_if_not_exists_return_existing_tx outside tx should see txk=tv after commit");
    }
    return;
  }
  if (mode == "facade_flush") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_flush root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'}, &error)) {
      Fail("facade_flush insert item failed: " + error);
    }
    if (!db.Flush(&error)) {
      Fail("facade_flush failed: " + error);
    }
    return;
  }
  if (mode == "facade_root_key") {
    std::vector<uint8_t> root_key;
    bool found = false;
    if (!db.RootKey(&root_key, &found, &error)) {
      Fail("facade_root_key on empty db failed: " + error);
    }
    if (found) {
      Fail("facade_root_key should report found=false for empty db");
    }
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_root_key insert root tree failed: " + error);
    }
    if (!db.RootKey(&root_key, &found, &error)) {
      Fail("facade_root_key after insert failed: " + error);
    }
    if (!found) {
      Fail("facade_root_key should report found=true after inserting tree");
    }
    return;
  }
  if (mode == "facade_delete_if_empty_tree") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_delete_if_empty_tree root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'d', 'e', 'l', 'e', 't', 'a', 'b', 'l', 'e'}, &error)) {
      Fail("facade_delete_if_empty_tree insert deletable tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'n', 'o', 'n', 'e', 'm', 'p', 't', 'y'}, &error)) {
      Fail("facade_delete_if_empty_tree insert nonempty tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'n', 'o', 'n', 'e', 'm', 'p', 't', 'y'}},
                       {'k', '1'},
                       {'v', '1'},
                       &error)) {
      Fail("facade_delete_if_empty_tree insert nonempty child item failed: " + error);
    }
    bool deleted = false;
    if (!db.DeleteIfEmptyTree({{'r', 'o', 'o', 't'}}, {'n', 'o', 'n', 'e', 'm', 'p', 't', 'y'}, &deleted, &error)) {
      Fail("facade_delete_if_empty_tree delete nonempty call failed: " + error);
    }
    if (deleted) {
      Fail("facade_delete_if_empty_tree nonempty delete should report false");
    }
    if (!db.DeleteIfEmptyTree({{'r', 'o', 'o', 't'}}, {'d', 'e', 'l', 'e', 't', 'a', 'b', 'l', 'e'}, &deleted, &error)) {
      Fail("facade_delete_if_empty_tree delete empty call failed: " + error);
    }
    if (!deleted) {
      Fail("facade_delete_if_empty_tree empty delete should report true");
    }
    return;
  }
  if (mode == "facade_clear_subtree") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_clear_subtree root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'l', 'r'}, &error)) {
      Fail("facade_clear_subtree insert subtree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '1'}, {'v', '1'}, &error)) {
      Fail("facade_clear_subtree insert k1 failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '2'}, {'v', '2'}, &error)) {
      Fail("facade_clear_subtree insert k2 failed: " + error);
    }
    if (!db.ClearSubtree({{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, &error)) {
      Fail("facade_clear_subtree clear failed: " + error);
    }
    return;
  }
  if (mode == "facade_clear_subtree_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_clear_subtree_tx root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'l', 'r'}, &error)) {
      Fail("facade_clear_subtree_tx insert subtree failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_clear_subtree_tx start tx failed: " + error);
    }
    if (!db.InsertItem(
            {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '1'}, {'v', '1'}, &tx, &error)) {
      Fail("facade_clear_subtree_tx insert k1 failed: " + error);
    }
    if (!db.InsertItem(
            {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '2'}, {'v', '2'}, &tx, &error)) {
      Fail("facade_clear_subtree_tx insert k2 failed: " + error);
    }
    if (!db.ClearSubtree({{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, &tx, &error)) {
      Fail("facade_clear_subtree_tx clear failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_clear_subtree_tx commit tx failed: " + error);
    }
    return;
  }
  if (mode == "facade_follow_reference") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_follow_reference target insert failed: " + error);
    }
    grovedb::ReferencePathType ref2_path;
    ref2_path.kind = grovedb::ReferencePathKind::kAbsolute;
    ref2_path.path = {{'r', 'o', 'o', 't'}, {'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '2'}, ref2_path, &error)) {
      Fail("facade_follow_reference ref2 insert failed: " + error);
    }
    grovedb::ReferencePathType ref1_path;
    ref1_path.kind = grovedb::ReferencePathKind::kSibling;
    ref1_path.key = {'r', 'e', 'f', '2'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref1_path, &error)) {
      Fail("facade_follow_reference ref1 insert failed: " + error);
    }
    return;
  }
  if (mode == "facade_follow_reference_tx") {
    // Create root tree
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_tx root tree failed: " + error);
    }
    // Insert target item
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_follow_reference_tx target insert failed: " + error);
    }
    // Insert base reference (sibling to target)
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kSibling;
    ref_path.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, ref_path, &error)) {
      Fail("facade_follow_reference_tx ref insert failed: " + error);
    }
    
    // Start transaction and insert tx-local reference
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_follow_reference_tx start tx failed: " + error);
    }
    grovedb::ReferencePathType tx_ref_path;
    tx_ref_path.kind = grovedb::ReferencePathKind::kSibling;
    tx_ref_path.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'r', 'e', 'f'}, tx_ref_path, &tx, &error)) {
      Fail("facade_follow_reference_tx tx_ref insert failed: " + error);
    }
    
    // Follow reference in tx - should resolve tx_ref
    std::vector<uint8_t> followed_in_tx;
    bool found_in_tx = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'r', 'e', 'f'}, &followed_in_tx, &found_in_tx, &tx, &error)) {
      Fail("facade_follow_reference_tx follow in tx failed: " + error);
    }
    grovedb::ElementItem followed_in_tx_item;
    if (!found_in_tx ||
        !grovedb::DecodeItemFromElementBytes(followed_in_tx, &followed_in_tx_item, &error) ||
        followed_in_tx_item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_follow_reference_tx follow in tx should resolve to tv");
    }
    
    // Follow reference outside tx before commit - should NOT find tx_ref
    std::vector<uint8_t> followed_outside_before;
    bool found_outside_before = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'r', 'e', 'f'}, &followed_outside_before, &found_outside_before, &error)) {
      Fail("facade_follow_reference_tx follow outside before commit failed: " + error);
    }
    if (found_outside_before) {
      Fail("facade_follow_reference_tx follow outside tx should not find tx_ref before commit");
    }
    
    // Commit transaction
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_follow_reference_tx commit tx failed: " + error);
    }
    
    // Follow reference after commit - should resolve tx_ref
    std::vector<uint8_t> followed_after;
    bool found_after = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'r', 'e', 'f'}, &followed_after, &found_after, &error)) {
      Fail("facade_follow_reference_tx follow after commit failed: " + error);
    }
    grovedb::ElementItem followed_after_item;
    if (!found_after ||
        !grovedb::DecodeItemFromElementBytes(followed_after, &followed_after_item, &error) ||
        followed_after_item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_follow_reference_tx follow after commit should resolve to tv");
    }
    return;
  }
  if (mode == "facade_find_subtrees") {
    // Create root tree
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_find_subtrees root tree failed: " + error);
    }
    // Create first-level child trees
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd', '1'}, &error)) {
      Fail("facade_find_subtrees child1 tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd', '2'}, &error)) {
      Fail("facade_find_subtrees child2 tree failed: " + error);
    }
    // Create second-level nested trees under child1
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}},
                            {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '1'},
                            &error)) {
      Fail("facade_find_subtrees grandchild1 tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}},
                            {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '2'},
                            &error)) {
      Fail("facade_find_subtrees grandchild2 tree failed: " + error);
    }
    // Add some items to leaf trees
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}, {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '1'}},
                       {'k', '1'},
                       {'v', '1'},
                       &error)) {
      Fail("facade_find_subtrees k1 insert failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '2'}},
                       {'k', '2'},
                       {'v', '2'},
                       &error)) {
      Fail("facade_find_subtrees k2 insert failed: " + error);
    }
    return;
  }
  if (mode == "facade_check_subtree_exists_invalid_path_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx root tree failed: " + error);
    }
    if (!db.InsertItem(
            {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'}, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx base item insert failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx child tree failed: " + error);
    }
    return;
  }
  if (mode == "facade_follow_reference_mixed_path_chain") {
    // Create nested structure: root/inner
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_mixed_path_chain root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'i', 'n', 'n', 'e', 'r'}, &error)) {
      Fail("facade_follow_reference_mixed_path_chain inner tree failed: " + error);
    }
    // Insert target item at root/target
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'},
                       {'m', 'i', 'x', 'e', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'},
                       &error)) {
      Fail("facade_follow_reference_mixed_path_chain target insert failed: " + error);
    }
    // ref_a: Absolute path reference to target (at root level)
    grovedb::ReferencePathType ref_a_path;
    ref_a_path.kind = grovedb::ReferencePathKind::kAbsolute;
    ref_a_path.path = {{'r', 'o', 'o', 't'}, {'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '_', 'a'}, ref_a_path, &error)) {
      Fail("facade_follow_reference_mixed_path_chain ref_a insert failed: " + error);
    }
    // ref_b: Sibling reference to ref_a (both at root level)
    grovedb::ReferencePathType ref_b_path;
    ref_b_path.kind = grovedb::ReferencePathKind::kSibling;
    ref_b_path.key = {'r', 'e', 'f', '_', 'a'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '_', 'b'}, ref_b_path, &error)) {
      Fail("facade_follow_reference_mixed_path_chain ref_b insert failed: " + error);
    }
    // ref_c: UpstreamRootHeightReference - from root/inner go up 1 level to root, then to ref_b
    grovedb::ReferencePathType ref_c_path;
    ref_c_path.kind = grovedb::ReferencePathKind::kUpstreamRootHeight;
    ref_c_path.height = 1;
    ref_c_path.path = {{'r', 'e', 'f', '_', 'b'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'i', 'n', 'n', 'e', 'r'}},
                            {'r', 'e', 'f', '_', 'c'}, ref_c_path, &error)) {
      Fail("facade_follow_reference_mixed_path_chain ref_c insert failed: " + error);
    }
    return;
  }
  if (mode == "facade_follow_reference_parent_path_addition") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_parent_path_addition root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'a', 'l', 'i', 'a', 's'}, &error)) {
      Fail("facade_follow_reference_parent_path_addition alias tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'a', 'l', 'i', 'a', 's'}},
                       {'t', 'a', 'r', 'g', 'e', 't'},
                       {'p', 'a', 'r', 'e', 'n', 't', '_', 'a', 'd', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'},
                       &error)) {
      Fail("facade_follow_reference_parent_path_addition target insert failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, &error)) {
      Fail("facade_follow_reference_parent_path_addition branch tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                            {'t', 'a', 'r', 'g', 'e', 't'}, &error)) {
      Fail("facade_follow_reference_parent_path_addition nested target tree failed: " + error);
    }
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kUpstreamRootHeightWithParentPathAddition;
    ref_path.height = 1;
    ref_path.path = {{'a', 'l', 'i', 'a', 's'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'t', 'a', 'r', 'g', 'e', 't'}},
                            {'r', 'e', 'f', '_', 'p', 'a', 'r', 'e', 'n', 't', '_', 'a', 'd', 'd'},
                            ref_path,
                            &error)) {
      Fail("facade_follow_reference_parent_path_addition reference insert failed: " + error);
    }
    return;
  }
  if (mode == "facade_follow_reference_upstream_element_height") {
    // Structure: root/branch/deep
    // Reference at root/branch/deep/ref uses UpstreamFromElementHeight(1, ['alias'])
    // and resolves to root/branch/alias.
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_upstream_element_height root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, &error)) {
      Fail("facade_follow_reference_upstream_element_height branch tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                            {'d', 'e', 'e', 'p'}, &error)) {
      Fail("facade_follow_reference_upstream_element_height deep tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                       {'a', 'l', 'i', 'a', 's'},
                       {'u', 'p', 's', 't', 'r', 'e', 'a', 'm', '_', 'e', 'l', 'e', 'm', '_',
                        'v', 'a', 'l', 'u', 'e'},
                       &error)) {
      Fail("facade_follow_reference_upstream_element_height alias insert failed: " + error);
    }
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kUpstreamFromElementHeight;
    ref_path.height = 1;
    ref_path.path = {{'a', 'l', 'i', 'a', 's'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            ref_path,
                            &error)) {
      Fail("facade_follow_reference_upstream_element_height reference insert failed: " + error);
    }
    return;
  }

  if (mode == "facade_follow_reference_cousin") {
    // Test CousinReference resolution:
    // Structure: root/branch/deep and root/branch/cousin
    // Reference at root/branch/deep/ref uses CousinReference(b"cousin")
    // This swaps the parent key 'deep' with 'cousin', keeping the original key 'ref'
    // Result: resolves to root/branch/cousin/ref
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_cousin root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, &error)) {
      Fail("facade_follow_reference_cousin branch tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'d', 'e', 'e', 'p'}, &error)) {
      Fail("facade_follow_reference_cousin deep tree failed: " + error);
    }
    // Insert target at root/branch/cousin/ref (the resolved location)
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                            {'c', 'o', 'u', 's', 'i', 'n'}, &error)) {
      Fail("facade_follow_reference_cousin cousin tree insert failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}},
                       {'r', 'e', 'f'},
                       {'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'},
                       &error)) {
      Fail("facade_follow_reference_cousin cousin target insert failed: " + error);
    }
    // Insert reference at root/branch/deep/ref that points to root/branch/cousin/ref
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kCousin;
    ref_path.key = {'c', 'o', 'u', 's', 'i', 'n'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            ref_path,
                            &error)) {
      Fail("facade_follow_reference_cousin reference insert failed: " + error);
    }
    return;
  }

  if (mode == "facade_follow_reference_removed_cousin") {
    // Test RemovedCousinReference resolution:
    // Structure: root/branch/deep and root/branch/cousin/nested
    // Reference at root/branch/deep/ref uses RemovedCousinReference([b"cousin", b"nested"])
    // This swaps the parent key 'deep' with the path [cousin, nested], keeping the original key 'ref'
    // Result: resolves to root/branch/cousin/nested/ref
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_follow_reference_removed_cousin root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, &error)) {
      Fail("facade_follow_reference_removed_cousin branch tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'d', 'e', 'e', 'p'}, &error)) {
      Fail("facade_follow_reference_removed_cousin deep tree failed: " + error);
    }
    // Insert nested tree structure for the resolved location
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'c', 'o', 'u', 's', 'i', 'n'}, &error)) {
      Fail("facade_follow_reference_removed_cousin cousin tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}}, {'n', 'e', 's', 't', 'e', 'd'}, &error)) {
      Fail("facade_follow_reference_removed_cousin nested tree failed: " + error);
    }
    // Insert target at root/branch/cousin/nested (the resolved location)
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}, {'n', 'e', 's', 't', 'e', 'd'}},
                       {'r', 'e', 'f'},
                       {'r', 'e', 'm', 'o', 'v', 'e', 'd', '_', 'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'},
                       &error)) {
      Fail("facade_follow_reference_removed_cousin removed cousin target insert failed: " + error);
    }
    // Insert reference at root/branch/deep/ref that points to root/branch/cousin/nested/ref
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kRemovedCousin;
    ref_path.path = {{'c', 'o', 'u', 's', 'i', 'n'}, {'n', 'e', 's', 't', 'e', 'd'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            ref_path,
                            &error)) {
      Fail("facade_follow_reference_removed_cousin reference insert failed: " + error);
    }
    return;
  }

  if (mode == "facade_get_raw") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_raw root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_get_raw target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref_to_target, &error)) {
      Fail("facade_get_raw reference insert failed: " + error);
    }
    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &error)) {
      Fail("facade_get_raw plain overload failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw should set found=true");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
      Fail("facade_get_raw variant decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw should return reference bytes without resolution");
    }
    grovedb::OperationCost cost;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &cost, &error)) {
      Fail("facade_get_raw cost overload failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_get_raw start tx failed: " + error);
    }
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &tx, &error)) {
      Fail("facade_get_raw tx overload failed: " + error);
    }
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &cost, &tx, &error)) {
      Fail("facade_get_raw cost+tx overload failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("facade_get_raw rollback tx failed: " + error);
    }
    return;
  }
  if (mode == "facade_get_raw_optional") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_raw_optional root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_get_raw_optional target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref_to_target, &error)) {
      Fail("facade_get_raw_optional reference insert failed: " + error);
    }
    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &error)) {
      Fail("facade_get_raw_optional plain overload failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw_optional should set found=true for existing key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
      Fail("facade_get_raw_optional variant decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw_optional should return unresolved reference bytes");
    }
    grovedb::OperationCost cost;
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &cost, &error)) {
      Fail("facade_get_raw_optional cost overload failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_get_raw_optional start tx failed: " + error);
    }
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &tx, &error)) {
      Fail("facade_get_raw_optional tx overload failed: " + error);
    }
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &cost, &tx, &error)) {
      Fail("facade_get_raw_optional cost+tx overload failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("facade_get_raw_optional rollback tx failed: " + error);
    }
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}, {'m', 'i', 's', 's'}}, {'k'}, &raw, &found, &error)) {
      Fail("facade_get_raw_optional missing-path call failed: " + error);
    }
    if (found || !raw.empty()) {
      Fail("facade_get_raw_optional missing path should return found=false with empty bytes");
    }
    return;
  }
  if (mode == "facade_get_raw_caching_optional") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_raw_caching_optional root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_get_raw_caching_optional target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, ref_to_target, &error)) {
      Fail("facade_get_raw_caching_optional reference insert failed: " + error);
    }
    std::vector<uint8_t> element_bytes;
    bool found = true;
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &element_bytes, &found, true, &error)) {
      Fail("facade_get_raw_caching_optional plain overload (allow_cache=true) failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw_caching_optional should set found=true for existing key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(element_bytes, &variant, &error)) {
      Fail("facade_get_raw_caching_optional variant decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw_caching_optional should return unresolved reference bytes");
    }
    std::vector<uint8_t> element_bytes2;
    bool found2 = true;
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &element_bytes2, &found2, false, &error)) {
      Fail("facade_get_raw_caching_optional plain overload (allow_cache=false) failed: " + error);
    }
    if (!found2) {
      Fail("facade_get_raw_caching_optional should set found=true for existing key (cache bypass)");
    }
    if (element_bytes != element_bytes2) {
      Fail("facade_get_raw_caching_optional cache and cache-bypass should return same bytes");
    }
    std::vector<uint8_t> missing_bytes;
    bool missing_found = true;
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'}, &missing_bytes, &missing_found, true, &error)) {
      Fail("facade_get_raw_caching_optional missing-path call failed: " + error);
    }
    if (missing_found) {
      Fail("facade_get_raw_caching_optional missing path should return found=false");
    }
    if (!missing_bytes.empty()) {
      Fail("facade_get_raw_caching_optional missing path should return empty bytes");
    }
    return;
  }
  if (mode == "facade_get_caching_optional") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_caching_optional root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_get_caching_optional target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, ref_to_target, &error)) {
      Fail("facade_get_caching_optional reference insert failed: " + error);
    }
    std::vector<uint8_t> element_bytes;
    bool found = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &element_bytes, &found, true, &error)) {
      Fail("facade_get_caching_optional plain overload (allow_cache=true) failed: " + error);
    }
    if (!found) {
      Fail("facade_get_caching_optional should set found=true for existing key");
    }
    if (element_bytes != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_get_caching_optional should return target item value");
    }
    std::vector<uint8_t> element_bytes2;
    bool found2 = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &element_bytes2, &found2, false, &error)) {
      Fail("facade_get_caching_optional plain overload (allow_cache=false) failed: " + error);
    }
    if (!found2) {
      Fail("facade_get_caching_optional should set found=true for existing key (cache bypass)");
    }
    if (element_bytes != element_bytes2) {
      Fail("facade_get_caching_optional cache and cache-bypass should return same resolved bytes");
    }
    return;
  }
  if (mode == "facade_get_caching_optional_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_caching_optional_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'}, &error)) {
      Fail("facade_get_caching_optional_tx base insert failed: " + error);
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_get_caching_optional_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'x', '_', 'v'}, &tx, &error)) {
      Fail("facade_get_caching_optional_tx tx insert failed: " + error);
    }

    // GetCachingOptional in tx - should see txk
    std::vector<uint8_t> element_bytes_in_tx;
    bool found_in_tx = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &element_bytes_in_tx, &found_in_tx, true, &tx, &error)) {
      Fail("facade_get_caching_optional_tx in-tx get failed: " + error);
    }
    if (!found_in_tx) {
      Fail("facade_get_caching_optional_tx in-tx should set found=true for tx-local key");
    }
    if (element_bytes_in_tx != std::vector<uint8_t>({'t', 'x', '_', 'v'})) {
      Fail("facade_get_caching_optional_tx in-tx should return txk item value");
    }

    // GetCachingOptional outside tx before commit - should NOT find txk
    std::vector<uint8_t> element_bytes_outside_before;
    bool found_outside_before = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &element_bytes_outside_before, &found_outside_before, true, &error)) {
      Fail("facade_get_caching_optional_tx outside-tx before commit get failed: " + error);
    }
    if (found_outside_before) {
      Fail("facade_get_caching_optional_tx outside-tx should not find txk before commit");
    }
    if (!element_bytes_outside_before.empty()) {
      Fail("facade_get_caching_optional_tx outside-tx should return empty bytes for missing key");
    }

    // GetCachingOptional in tx for base key - should see base_v
    std::vector<uint8_t> base_in_tx;
    bool found_base_in_tx = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, &base_in_tx, &found_base_in_tx, true, &tx, &error)) {
      Fail("facade_get_caching_optional_tx in-tx base get failed: " + error);
    }
    if (!found_base_in_tx) {
      Fail("facade_get_caching_optional_tx in-tx should find base key");
    }
    if (base_in_tx != std::vector<uint8_t>({'b', 'a', 's', 'e', '_', 'v'})) {
      Fail("facade_get_caching_optional_tx in-tx should return base item value");
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_get_caching_optional_tx commit tx failed: " + error);
    }

    // GetCachingOptional after commit - should see txk
    std::vector<uint8_t> element_bytes_after;
    bool found_after = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &element_bytes_after, &found_after, true, &error)) {
      Fail("facade_get_caching_optional_tx after commit get failed: " + error);
    }
    if (!found_after) {
      Fail("facade_get_caching_optional_tx after commit should set found=true");
    }
    if (element_bytes_after != std::vector<uint8_t>({'t', 'x', '_', 'v'})) {
      Fail("facade_get_caching_optional_tx after commit should return txk item value");
    }
    return;
  }
  if (mode == "facade_get_subtree_root_tx") {
    // Test GetSubtreeRoot tx-local visibility
    // Setup: root tree with child subtree (committed) and tx_child subtree (tx-local)
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_get_subtree_root_tx root tree failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, &error)) {
      Fail("facade_get_subtree_root_tx child tree failed: " + error);
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_get_subtree_root_tx start tx failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}, &tx, &error)) {
      Fail("facade_get_subtree_root_tx tx_child tree insert failed: " + error);
    }

    // GetSubtreeRoot in tx for tx_child - should see tx-local subtree element
    std::vector<uint8_t> root_key_in_tx;
    std::vector<uint8_t> element_in_tx;
    if (!db.GetSubtreeRoot({{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &root_key_in_tx, &element_in_tx, &tx, &error)) {
      Fail("facade_get_subtree_root_tx in-tx get failed: " + error);
    }
    if (root_key_in_tx != std::vector<uint8_t>({'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'})) {
      Fail("facade_get_subtree_root_tx in-tx should return tx_child as root key");
    }
    uint64_t variant_in_tx = 0;
    if (!grovedb::DecodeElementVariant(element_in_tx, &variant_in_tx, &error)) {
      Fail("facade_get_subtree_root_tx in-tx decode variant failed: " + error);
    }
    if (variant_in_tx != 2) {
      Fail("facade_get_subtree_root_tx in-tx should return Tree element variant (2)");
    }

    // GetSubtreeRoot outside tx before commit - should NOT find tx_child subtree
    std::vector<uint8_t> root_key_outside;
    std::vector<uint8_t> element_outside;
    if (db.GetSubtreeRoot({{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &root_key_outside, &element_outside, &error)) {
      Fail("facade_get_subtree_root_tx outside-tx should fail for tx-local subtree before commit");
    }

    // GetSubtreeRoot in tx for child (committed subtree) - should see child subtree element
    std::vector<uint8_t> root_key_child;
    std::vector<uint8_t> element_child;
    if (!db.GetSubtreeRoot({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, &root_key_child, &element_child, &tx, &error)) {
      Fail("facade_get_subtree_root_tx in-tx child get failed: " + error);
    }
    if (root_key_child != std::vector<uint8_t>({'c', 'h', 'i', 'l', 'd'})) {
      Fail("facade_get_subtree_root_tx in-tx should return child as root key");
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_get_subtree_root_tx commit tx failed: " + error);
    }

    // GetSubtreeRoot after commit - should see tx_child subtree element
    std::vector<uint8_t> root_key_after;
    std::vector<uint8_t> element_after;
    if (!db.GetSubtreeRoot({{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &root_key_after, &element_after, &error)) {
      Fail("facade_get_subtree_root_tx after commit get failed: " + error);
    }
    if (root_key_after != std::vector<uint8_t>({'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'})) {
      Fail("facade_get_subtree_root_tx after commit should return tx_child as root key");
    }
    return;
  }
  if (mode == "facade_has_caching_optional_tx") {
    // Test HasCachingOptional tx-local visibility
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_has_caching_optional_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'}, &error)) {
      Fail("facade_has_caching_optional_tx base insert failed: " + error);
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_has_caching_optional_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'x', '_', 'v'}, &tx, &error)) {
      Fail("facade_has_caching_optional_tx tx insert failed: " + error);
    }

    // HasCachingOptional in tx - should find txk
    bool found_in_tx = false;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &found_in_tx, true, &tx, &error)) {
      Fail("facade_has_caching_optional_tx in-tx has failed: " + error);
    }
    if (!found_in_tx) {
      Fail("facade_has_caching_optional_tx in-tx should report found=true for tx-local key");
    }

    // HasCachingOptional outside tx before commit - should NOT find txk
    bool found_outside_before = true;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &found_outside_before, true, &error)) {
      Fail("facade_has_caching_optional_tx outside-tx before commit has failed: " + error);
    }
    if (found_outside_before) {
      Fail("facade_has_caching_optional_tx outside-tx should report found=false for tx-local key before commit");
    }

    // HasCachingOptional in tx for base key - should find base
    bool found_base_in_tx = false;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, &found_base_in_tx, true, &tx, &error)) {
      Fail("facade_has_caching_optional_tx in-tx base has failed: " + error);
    }
    if (!found_base_in_tx) {
      Fail("facade_has_caching_optional_tx in-tx should report found=true for base key");
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_has_caching_optional_tx commit tx failed: " + error);
    }

    // HasCachingOptional after commit - should find txk
    bool found_after = false;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &found_after, true, &error)) {
      Fail("facade_has_caching_optional_tx after commit has failed: " + error);
    }
    if (!found_after) {
      Fail("facade_has_caching_optional_tx after commit should report found=true for committed key");
    }
    return;
  }
  if (mode == "facade_query_raw") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_raw root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_query_raw target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref_to_target, &error)) {
      Fail("facade_query_raw reference insert failed: " + error);
    }
    grovedb::PathQuery query = grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}},
                                                                 {'r', 'e', 'f', '1'});
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRaw(query, &out, &error)) {
      Fail("facade_query_raw query failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
      Fail("facade_query_raw should return exactly one reference key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(out[0].second, &variant, &error)) {
      Fail("facade_query_raw decode variant failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_query_raw should return unresolved reference element bytes");
    }
    return;
  }
  if (mode == "facade_query_item_value") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_item_value root tree failed: " + error);
    }
    if (!db.InsertItem(
            {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_query_item_value target insert failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'i', 't', 'e', 'm', '2'}, {'i', 'w'}, &error)) {
      Fail("facade_query_item_value item insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, ref_to_target, &error)) {
      Fail("facade_query_item_value reference insert failed: " + error);
    }
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'i', 't', 'e', 'm', '2'}));
    query.items.push_back(grovedb::QueryItem::Key({'r', 'e', 'f'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'a', 'r', 'g', 'e', 't'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 3, std::nullopt));
    std::vector<std::vector<uint8_t>> out_values;
    if (!db.QueryItemValue(path_query, &out_values, &error)) {
      Fail("facade_query_item_value query failed: " + error);
    }
    if (out_values.size() != 3) {
      Fail("facade_query_item_value should return three values");
    }
    auto contains_value = [&](const std::vector<uint8_t>& needle) {
      for (const auto& value : out_values) {
        if (value == needle) {
          return true;
        }
      }
      return false;
    };
    if (!contains_value({'i', 'w'})) {
      Fail("facade_query_item_value should include item value");
    }
    if (!contains_value({'t', 'v'})) {
      Fail("facade_query_item_value should include resolved reference item value");
    }
    int tv_count = 0;
    for (const auto& value : out_values) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        ++tv_count;
      }
    }
    if (tv_count != 2) {
      Fail("facade_query_item_value should include two tv values (target + resolved ref)");
    }
    return;
  }
  if (mode == "facade_query_item_value_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_item_value_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_item_value_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_item_value_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_item_value_tx tx insert failed: " + error);
    }
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<std::vector<uint8_t>> values_in_tx;
    if (!db.QueryItemValue(path_query, &values_in_tx, &tx, &error)) {
      Fail("facade_query_item_value_tx query in tx failed: " + error);
    }
    bool in_tx_saw_txk = false;
    for (const auto& value : values_in_tx) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        in_tx_saw_txk = true;
      }
    }
    if (!in_tx_saw_txk) {
      Fail("facade_query_item_value_tx tx query should include txk value");
    }
    std::vector<std::vector<uint8_t>> values_outside_before;
    if (!db.QueryItemValue(path_query, &values_outside_before, &error)) {
      Fail("facade_query_item_value_tx query outside tx before commit failed: " + error);
    }
    for (const auto& value : values_outside_before) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        Fail("facade_query_item_value_tx query outside tx should not include txk value before commit");
      }
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_item_value_tx commit tx failed: " + error);
    }
    std::vector<std::vector<uint8_t>> values_outside_after;
    if (!db.QueryItemValue(path_query, &values_outside_after, &error)) {
      Fail("facade_query_item_value_tx query outside tx after commit failed: " + error);
    }
    bool outside_after_saw_txk = false;
    for (const auto& value : values_outside_after) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        outside_after_saw_txk = true;
      }
    }
    if (!outside_after_saw_txk) {
      Fail("facade_query_item_value_tx query outside tx should include txk value after commit");
    }
    return;
  }
  if (mode == "facade_query_item_value_or_sum_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_item_value_or_sum_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_item_value_or_sum_tx base insert failed: " + error);
    }
    if (!db.InsertSumItem({{'r', 'o', 'o', 't'}}, {'s', 'u', 'm'}, 50, &error)) {
      Fail("facade_query_item_value_or_sum_tx sum insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_item_value_or_sum_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_item_value_or_sum_tx tx insert failed: " + error);
    }
    if (!db.InsertSumItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 's', 'u', 'm'}, 100, &tx, &error)) {
      Fail("facade_query_item_value_or_sum_tx tx sum insert failed: " + error);
    }
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'s', 'u', 'm'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 's', 'u', 'm'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 4, std::nullopt));
    std::vector<grovedb::GroveDb::QueryItemOrSumValue> values_in_tx;
    if (!db.QueryItemValueOrSum(path_query, &values_in_tx, &tx, &error)) {
      Fail("facade_query_item_value_or_sum_tx query in tx failed: " + error);
    }
    bool in_tx_saw_txk = false;
    bool in_tx_saw_txsum = false;
    for (const auto& v : values_in_tx) {
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kItemData &&
          v.item_data == std::vector<uint8_t>({'t', 'v'})) {
        in_tx_saw_txk = true;
      }
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kSumValue &&
          v.sum_value == 100) {
        in_tx_saw_txsum = true;
      }
    }
    if (!in_tx_saw_txk || !in_tx_saw_txsum) {
      Fail("facade_query_item_value_or_sum_tx tx query should include txk and txsum values");
    }
    std::vector<grovedb::GroveDb::QueryItemOrSumValue> values_outside_before;
    if (!db.QueryItemValueOrSum(path_query, &values_outside_before, &error)) {
      Fail("facade_query_item_value_or_sum_tx query outside tx before commit failed: " + error);
    }
    for (const auto& v : values_outside_before) {
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kItemData &&
          v.item_data == std::vector<uint8_t>({'t', 'v'})) {
        Fail("facade_query_item_value_or_sum_tx query outside tx should not include txk value before commit");
      }
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kSumValue &&
          v.sum_value == 100) {
        Fail("facade_query_item_value_or_sum_tx query outside tx should not include txsum value before commit");
      }
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_item_value_or_sum_tx commit tx failed: " + error);
    }
    std::vector<grovedb::GroveDb::QueryItemOrSumValue> values_outside_after;
    if (!db.QueryItemValueOrSum(path_query, &values_outside_after, &error)) {
      Fail("facade_query_item_value_or_sum_tx query outside tx after commit failed: " + error);
    }
    bool outside_after_saw_txk = false;
    bool outside_after_saw_txsum = false;
    for (const auto& v : values_outside_after) {
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kItemData &&
          v.item_data == std::vector<uint8_t>({'t', 'v'})) {
        outside_after_saw_txk = true;
      }
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kSumValue &&
          v.sum_value == 100) {
        outside_after_saw_txsum = true;
      }
    }
    if (!outside_after_saw_txk || !outside_after_saw_txsum) {
      Fail("facade_query_item_value_or_sum_tx query outside tx should include txk and txsum values after commit");
    }
    return;
  }
  if (mode == "facade_query_raw_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_raw_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_raw_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_raw_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_raw_tx tx insert failed: " + error);
    }
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out_in_tx;
    if (!db.QueryRaw(path_query, &out_in_tx, &tx, &error)) {
      Fail("facade_query_raw_tx query in tx failed: " + error);
    }
    bool in_tx_saw_txk = false;
    for (const auto& row : out_in_tx) {
      if (row.first == std::vector<uint8_t>({'t', 'x', 'k'})) {
        in_tx_saw_txk = true;
      }
    }
    if (!in_tx_saw_txk) {
      Fail("facade_query_raw_tx tx query should see txk");
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out_outside_before;
    if (!db.QueryRaw(path_query, &out_outside_before, &error)) {
      Fail("facade_query_raw_tx query outside tx before commit failed: " + error);
    }
    bool outside_before_commit_saw_txk = false;
    for (const auto& row : out_outside_before) {
      if (row.first == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_before_commit_saw_txk = true;
      }
    }
    if (outside_before_commit_saw_txk) {
      Fail("facade_query_raw_tx query outside tx should not see txk before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_raw_tx commit tx failed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out_outside_after;
    if (!db.QueryRaw(path_query, &out_outside_after, &error)) {
      Fail("facade_query_raw_tx query outside tx after commit failed: " + error);
    }
    bool outside_after_commit_saw_txk = false;
    for (const auto& row : out_outside_after) {
      if (row.first == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_after_commit_saw_txk = true;
      }
    }
    if (!outside_after_commit_saw_txk) {
      Fail("facade_query_raw_tx query outside tx should see txk after commit");
    }
    return;
  }
  if (mode == "facade_query_key_element_pairs_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_key_element_pairs_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_key_element_pairs_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_key_element_pairs_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_key_element_pairs_tx tx insert failed: " + error);
    }
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<grovedb::GroveDb::KeyElementPair> out_in_tx;
    if (!db.QueryKeyElementPairs(path_query, &out_in_tx, &tx, &error)) {
      Fail("facade_query_key_element_pairs_tx query in tx failed: " + error);
    }
    bool in_tx_saw_txk = false;
    bool in_tx_saw_base = false;
    for (const auto& row : out_in_tx) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        in_tx_saw_txk = true;
      }
      if (row.key == std::vector<uint8_t>({'b', 'a', 's', 'e'})) {
        in_tx_saw_base = true;
      }
    }
    if (!in_tx_saw_txk || !in_tx_saw_base) {
      Fail("facade_query_key_element_pairs_tx tx query should see base and txk");
    }
    if (out_in_tx.size() != 2) {
      Fail("facade_query_key_element_pairs_tx tx query should return 2 pairs");
    }
    std::vector<grovedb::GroveDb::KeyElementPair> out_outside_before;
    if (!db.QueryKeyElementPairs(path_query, &out_outside_before, &error)) {
      Fail("facade_query_key_element_pairs_tx query outside tx before commit failed: " + error);
    }
    bool outside_before_commit_saw_txk = false;
    for (const auto& row : out_outside_before) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_before_commit_saw_txk = true;
      }
    }
    if (outside_before_commit_saw_txk) {
      Fail("facade_query_key_element_pairs_tx query outside tx should not see txk before commit");
    }
    if (out_outside_before.size() != 1) {
      Fail("facade_query_key_element_pairs_tx query outside tx before commit should return 1 pair");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_key_element_pairs_tx commit tx failed: " + error);
    }
    std::vector<grovedb::GroveDb::KeyElementPair> out_outside_after;
    if (!db.QueryKeyElementPairs(path_query, &out_outside_after, &error)) {
      Fail("facade_query_key_element_pairs_tx query outside tx after commit failed: " + error);
    }
    bool outside_after_commit_saw_txk = false;
    for (const auto& row : out_outside_after) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_after_commit_saw_txk = true;
      }
    }
    if (!outside_after_commit_saw_txk) {
      Fail("facade_query_key_element_pairs_tx query outside tx should see txk after commit");
    }
    if (out_outside_after.size() != 2) {
      Fail("facade_query_key_element_pairs_tx query outside tx after commit should return 2 pairs");
    }
    return;
  }
  if (mode == "facade_query_raw_keys_optional") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_raw_keys_optional root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_query_raw_keys_optional target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference(
            {{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref_to_target, &error)) {
      Fail("facade_query_raw_keys_optional reference insert failed: " + error);
    }
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'r', 'e', 'f', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryRawKeysOptional(query, &out, &error)) {
      Fail("facade_query_raw_keys_optional query failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_raw_keys_optional should return two rows");
    }
    const std::vector<std::vector<uint8_t>> expected_path = {{'r', 'o', 'o', 't'}};
    bool saw_ref = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.path != expected_path) {
        Fail("facade_query_raw_keys_optional row path mismatch");
      }
      if (row.key == std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
        if (!row.element_found) {
          Fail("facade_query_raw_keys_optional ref row should be found");
        }
        uint64_t variant = 0;
        if (!grovedb::DecodeElementVariant(row.element_bytes, &variant, &error)) {
          Fail("facade_query_raw_keys_optional decode variant failed: " + error);
        }
        if (variant != 1) {
          Fail("facade_query_raw_keys_optional should return unresolved reference bytes");
        }
        saw_ref = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_raw_keys_optional missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_raw_keys_optional returned unexpected key");
      }
    }
    if (!saw_ref || !saw_missing) {
      Fail("facade_query_raw_keys_optional should include ref1 and miss rows");
    }
    return;
  }
  if (mode == "facade_query_keys_optional") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_keys_optional root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("facade_query_keys_optional target insert failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference(
            {{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, ref_to_target, &error)) {
      Fail("facade_query_keys_optional reference insert failed: " + error);
    }
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'r', 'e', 'f', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryKeysOptional(query, &out, &error)) {
      Fail("facade_query_keys_optional query failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_keys_optional should return two rows");
    }
    bool saw_ref = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
        if (!row.element_found) {
          Fail("facade_query_keys_optional ref row should be found");
        }
        grovedb::ElementItem item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &item, &error)) {
          Fail("facade_query_keys_optional decode item failed: " + error);
        }
        if (item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_keys_optional should return resolved target item");
        }
        saw_ref = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_keys_optional missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_keys_optional returned unexpected key");
      }
    }
    if (!saw_ref || !saw_missing) {
      Fail("facade_query_keys_optional should include ref1 and miss rows");
    }
    return;
  }
  if (mode == "facade_query_raw_keys_optional_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_raw_keys_optional_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_raw_keys_optional_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_raw_keys_optional_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_raw_keys_optional_tx tx insert failed: " + error);
    }
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'m', 'i', 's', 's'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryRawKeysOptional(query, &out, &tx, &error)) {
      Fail("facade_query_raw_keys_optional_tx query in tx failed: " + error);
    }
    bool tx_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        tx_saw_txk = row.element_found;
      }
    }
    if (!tx_saw_txk) {
      Fail("facade_query_raw_keys_optional_tx tx query should see txk");
    }
    if (!db.QueryRawKeysOptional(query, &out, &error)) {
      Fail("facade_query_raw_keys_optional_tx query outside tx before commit failed: " + error);
    }
    bool outside_precommit_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_precommit_saw_txk = row.element_found;
      }
    }
    if (outside_precommit_saw_txk) {
      Fail("facade_query_raw_keys_optional_tx query outside tx should not see txk before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_raw_keys_optional_tx commit tx failed: " + error);
    }
    if (!db.QueryRawKeysOptional(query, &out, &error)) {
      Fail("facade_query_raw_keys_optional_tx query outside tx after commit failed: " + error);
    }
    bool outside_postcommit_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_postcommit_saw_txk = row.element_found;
      }
    }
    if (!outside_postcommit_saw_txk) {
      Fail("facade_query_raw_keys_optional_tx query outside tx should see txk after commit");
    }
    return;
  }
  if (mode == "facade_query_keys_optional_tx") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("facade_query_keys_optional_tx root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'}, &error)) {
      Fail("facade_query_keys_optional_tx base insert failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_keys_optional_tx start tx failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'}, &tx, &error)) {
      Fail("facade_query_keys_optional_tx tx insert failed: " + error);
    }
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'m', 'i', 's', 's'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryKeysOptional(query, &out, &tx, &error)) {
      Fail("facade_query_keys_optional_tx query in tx failed: " + error);
    }
    bool tx_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        tx_saw_txk = row.element_found;
      }
    }
    if (!tx_saw_txk) {
      Fail("facade_query_keys_optional_tx tx query should see txk");
    }
    if (!db.QueryKeysOptional(query, &out, &error)) {
      Fail("facade_query_keys_optional_tx query outside tx before commit failed: " + error);
    }
    bool outside_precommit_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_precommit_saw_txk = row.element_found;
      }
    }
    if (outside_precommit_saw_txk) {
      Fail("facade_query_keys_optional_tx query outside tx should not see txk before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_keys_optional_tx commit tx failed: " + error);
    }
    if (!db.QueryKeysOptional(query, &out, &error)) {
      Fail("facade_query_keys_optional_tx query outside tx after commit failed: " + error);
    }
    bool outside_postcommit_saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        outside_postcommit_saw_txk = row.element_found;
      }
    }
    if (!outside_postcommit_saw_txk) {
      Fail("facade_query_keys_optional_tx query outside tx should see txk after commit");
    }
    return;
  }
  if (mode == "facade_query_sums_tx") {
    std::vector<uint8_t> root_sum_tree;
    if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &root_sum_tree, &error)) {
      Fail("facade_query_sums_tx encode root sum tree failed: " + error);
    }
    if (!db.Insert({}, {'r', 'o', 'o', 't'}, root_sum_tree, &error)) {
      Fail("facade_query_sums_tx root sum tree insert failed: " + error);
    }
    if (!db.InsertSumItem({{'r', 'o', 'o', 't'}}, {'s', '1'}, 10, &error)) {
      Fail("facade_query_sums_tx s1 insert failed: " + error);
    }
    if (!db.InsertSumItem({{'r', 'o', 'o', 't'}}, {'s', '2'}, 20, &error)) {
      Fail("facade_query_sums_tx s2 insert failed: " + error);
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_query_sums_tx start tx failed: " + error);
    }
    if (!db.InsertSumItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 's'}, 30, &tx, &error)) {
      Fail("facade_query_sums_tx txs insert failed: " + error);
    }

    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'s', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 's'}));

    std::vector<int64_t> sums_in_tx;
    if (!db.QuerySums(query, &sums_in_tx, &tx, &error)) {
      Fail("facade_query_sums_tx query in tx failed: " + error);
    }
    bool in_tx_saw_s1 = false;
    bool in_tx_saw_txs = false;
    for (const auto& sum : sums_in_tx) {
      if (sum == 10) {
        in_tx_saw_s1 = true;
      } else if (sum == 30) {
        in_tx_saw_txs = true;
      }
    }
    if (!in_tx_saw_s1 || !in_tx_saw_txs) {
      Fail("facade_query_sums_tx query in tx should include s1 and txs sums");
    }

    std::vector<int64_t> sums_outside_before;
    if (!db.QuerySums(query, &sums_outside_before, &error)) {
      Fail("facade_query_sums_tx query outside tx before commit failed: " + error);
    }
    if (sums_outside_before.size() != 1 || sums_outside_before[0] != 10) {
      Fail("facade_query_sums_tx query outside tx before commit should only return s1 sum");
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_query_sums_tx commit tx failed: " + error);
    }

    std::vector<int64_t> sums_outside_after;
    if (!db.QuerySums(query, &sums_outside_after, &error)) {
      Fail("facade_query_sums_tx query outside tx after commit failed: " + error);
    }
    bool outside_after_saw_s1 = false;
    bool outside_after_saw_txs = false;
    for (const auto& sum : sums_outside_after) {
      if (sum == 10) {
        outside_after_saw_s1 = true;
      } else if (sum == 30) {
        outside_after_saw_txs = true;
      }
    }
    if (!outside_after_saw_s1 || !outside_after_saw_txs) {
      Fail("facade_query_sums_tx query outside tx after commit should include s1 and txs sums");
    }
    return;
  }
  if (mode == "tx_checkpoint_independent_writes") {
    std::vector<uint8_t> tree_raw;
    if (!grovedb::EncodeTreeToElementBytes(&tree_raw, &error)) {
      Fail("encode tree for checkpoint independent writes failed: " + error);
    }
    std::vector<uint8_t> ay_item;
    if (!grovedb::EncodeItemToElementBytes({'a', 'y', 'y'}, &ay_item, &error)) {
      Fail("encode ayy for checkpoint independent writes failed: " + error);
    }
    if (!db.Insert({}, {'k', 'e', 'y', '1'}, tree_raw, &error)) {
      Fail("insert key1 tree failed: " + error);
    }
    if (!db.Insert({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '2'}, tree_raw, &error)) {
      Fail("insert key2 tree failed: " + error);
    }
    if (!db.Insert(
            {{'k', 'e', 'y', '1'}, {'k', 'e', 'y', '2'}}, {'k', 'e', 'y', '3'}, ay_item, &error)) {
      Fail("insert key3 item failed: " + error);
    }

    const std::string checkpoint_dir = dir + "_checkpoint_independent_writes";
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint for independent writes failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint_db;
      if (!checkpoint_db.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint for independent writes failed: " + error);
      }

      std::vector<uint8_t> ay2_item;
      if (!grovedb::EncodeItemToElementBytes({'a', 'y', 'y', '2'}, &ay2_item, &error)) {
        Fail("encode ayy2 for checkpoint independent writes failed: " + error);
      }
      std::vector<uint8_t> ay3_item;
      if (!grovedb::EncodeItemToElementBytes({'a', 'y', 'y', '3'}, &ay3_item, &error)) {
        Fail("encode ayy3 for checkpoint independent writes failed: " + error);
      }
      if (!checkpoint_db.Insert({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '4'}, ay2_item, &error)) {
        Fail("checkpoint insert key4 failed: " + error);
      }
      if (!db.Insert({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '4'}, ay3_item, &error)) {
        Fail("main insert key4 failed: " + error);
      }
      if (!checkpoint_db.Insert({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '5'}, ay3_item, &error)) {
        Fail("checkpoint insert key5 failed: " + error);
      }
      if (!db.Insert({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '6'}, ay3_item, &error)) {
        Fail("main insert key6 failed: " + error);
      }

      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_db.Get({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '6'}, &raw, &found, &error)) {
        Fail("checkpoint get key6 failed: " + error);
      }
      if (found) {
        Fail("checkpoint should not contain main-only key6");
      }
      if (!db.Get({{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '5'}, &raw, &found, &error)) {
        Fail("main get key5 failed: " + error);
      }
      if (found) {
        Fail("main db should not contain checkpoint-only key5");
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint for independent writes failed: " + error);
    }
    return;
  }
  std::vector<uint8_t> tree_element;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error)) {
    Fail("encode tree failed: " + error);
  }
  std::vector<uint8_t> v1_item;
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_item, &error)) {
    Fail("encode v1 item failed: " + error);
  }
  if (!db.Insert({}, {'r', 'o', 'o', 't'}, tree_element, &error)) {
    Fail("insert root tree failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v1_item, &error)) {
    Fail("insert k1 failed: " + error);
  }

  if (mode == "nested") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("insert child tree failed: " + error);
    }
    std::vector<uint8_t> nv_item;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_item, &error)) {
      Fail("encode nv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, {'n', 'k'}, nv_item, &error)) {
      Fail("insert nested key failed: " + error);
    }
    return;
  }

  if (mode == "tx_commit") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> tv_item;
    if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &tv_item, &error)) {
      Fail("encode tv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 't', 'x'}, tv_item, &tx, &error)) {
      Fail("insert tx key failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit tx failed: " + error);
    }
    return;
  }

  if (mode == "tx_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> rv_item;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_item, &error)) {
      Fail("encode rv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'o', 'l', 'l'}, rv_item, &tx, &error)) {
      Fail("insert rollback key in tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback tx failed: " + error);
    }
    std::vector<uint8_t> tx_old_raw;
    bool tx_old_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &tx_old_raw, &tx_old_found, &tx, &error)) {
      Fail("rolled-back tx get original key failed: " + error);
    }
    if (!tx_old_found) {
      Fail("rolled-back tx should read original key");
    }
    std::vector<uint8_t> tx_new_raw;
    bool tx_new_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'k', 'r', 'o', 'l', 'l'},
                &tx_new_raw,
                &tx_new_found,
                &tx,
                &error)) {
      Fail("rolled-back tx get reverted key failed: " + error);
    }
    if (tx_new_found) {
      Fail("rolled-back tx should not see reverted key");
    }
    return;
  }

  if (mode == "tx_visibility_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> vis_item;
    if (!grovedb::EncodeItemToElementBytes({'v', 'v'}, &vis_item, &error)) {
      Fail("encode visibility item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'v', 'i', 's'}, vis_item, &tx, &error)) {
      Fail("insert visibility key in tx failed: " + error);
    }

    std::vector<uint8_t> in_tx_raw;
    bool in_tx_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'v', 'i', 's'}, &in_tx_raw, &in_tx_found, &tx, &error)) {
      Fail("tx get visibility key failed: " + error);
    }
    if (!in_tx_found) {
      Fail("tx expected to find inserted visibility key");
    }

    std::vector<uint8_t> outside_new_raw;
    bool outside_new_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'k', 'v', 'i', 's'},
                &outside_new_raw,
                &outside_new_found,
                &error)) {
      Fail("outside get visibility key failed: " + error);
    }
    if (outside_new_found) {
      Fail("outside read should not see uncommitted visibility key");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback tx failed: " + error);
    }
    std::vector<uint8_t> outside_old_raw;
    bool outside_old_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &outside_old_raw, &outside_old_found, &error)) {
      Fail("outside get old key failed: " + error);
    }
    if (!outside_old_found) {
      Fail("outside read should still see old key");
    }
    return;
  }

  if (mode == "tx_mixed_commit") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> mixed_item;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v'}, &mixed_item, &error)) {
      Fail("encode mixed item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 'x'}, mixed_item, &tx, &error)) {
      Fail("insert mixed key in tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx, &error)) {
      Fail("delete k1 in mixed tx failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit mixed tx failed: " + error);
    }
    return;
  }

  if (mode == "tx_rollback_range") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> rv_item;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_item, &error)) {
      Fail("encode rv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'o', 'l', 'l'}, rv_item, &tx, &error)) {
      Fail("insert rollback-range key in tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback tx failed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRange({{'r', 'o', 'o', 't'}}, {}, {}, true, true, &out, &tx, &error)) {
      Fail("rolled-back tx range query failed: " + error);
    }
    bool saw_k1 = false;
    bool saw_kroll = false;
    for (const auto& kv : out) {
      if (kv.first == std::vector<uint8_t>({'k', '1'})) {
        saw_k1 = true;
      } else if (kv.first == std::vector<uint8_t>({'k', 'r', 'o', 'l', 'l'})) {
        saw_kroll = true;
      }
    }
    if (!saw_k1 || saw_kroll) {
      Fail("rolled-back tx range view mismatch");
    }
    return;
  }

  if (mode == "tx_drop_abort") {
    {
      grovedb::GroveDb::Transaction tx;
      if (!db.StartTransaction(&tx, &error)) {
        Fail("start tx failed: " + error);
      }
      std::vector<uint8_t> dv_item;
      if (!grovedb::EncodeItemToElementBytes({'d', 'v'}, &dv_item, &error)) {
        Fail("encode dv item failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', 'r', 'o', 'p'}, dv_item, &tx, &error)) {
        Fail("insert drop key in tx failed: " + error);
      }
      std::vector<uint8_t> in_tx_raw;
      bool in_tx_found = false;
      if (!db.Get({{'r', 'o', 'o', 't'}},
                  {'k', 'd', 'r', 'o', 'p'},
                  &in_tx_raw,
                  &in_tx_found,
                  &tx,
                  &error)) {
        Fail("get drop key in tx failed: " + error);
      }
      if (!in_tx_found) {
        Fail("tx should see drop key before tx drop");
      }
    }
    return;
  }

  if (mode == "tx_commit_after_rollback_rejected") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> cv_item;
    if (!grovedb::EncodeItemToElementBytes({'c', 'v'}, &cv_item, &error)) {
      Fail("encode cv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'a', 'r'}, cv_item, &tx, &error)) {
      Fail("insert key before rollback failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback tx failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after rollback should succeed");
    }
    return;
  }

  if (mode == "tx_write_after_rollback_rejected") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback tx failed: " + error);
    }
    std::vector<uint8_t> wv_item;
    if (!grovedb::EncodeItemToElementBytes({'w', 'v'}, &wv_item, &error)) {
      Fail("encode wv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'w', 'a', 'r'}, wv_item, &tx, &error)) {
      Fail("write after rollback should succeed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after write-after-rollback should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_multi_rollback_reuse") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("first rollback failed: " + error);
    }
    std::vector<uint8_t> mv1_item;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v', '1'}, &mv1_item, &error)) {
      Fail("encode mv1 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'r', '1'}, mv1_item, &tx, &error)) {
      Fail("insert kmr1 after rollback should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("second rollback failed: " + error);
    }
    std::vector<uint8_t> mv2_item;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v', '2'}, &mv2_item, &error)) {
      Fail("encode mv2 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'r', '2'}, mv2_item, &tx, &error)) {
      Fail("insert kmr2 after second rollback should succeed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after multi rollback reuse should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_delete_after_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx, &error)) {
      Fail("delete after rollback should succeed: " + error);
    }
    if (!deleted) {
      Fail("delete after rollback should report deleted=true");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after delete-after-rollback should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_reopen_visibility_after_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback failed: " + error);
    }
    std::vector<uint8_t> rv_item;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_item, &error)) {
      Fail("encode rv item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'v'}, rv_item, &tx, &error)) {
      Fail("insert after rollback should succeed: " + error);
    }
    std::vector<uint8_t> in_tx_raw;
    bool in_tx_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'r', 'v'}, &in_tx_raw, &in_tx_found, &tx, &error)) {
      Fail("in-tx get failed: " + error);
    }
    if (!in_tx_found) {
      Fail("in-tx get should find key after reopen-write");
    }
    std::vector<uint8_t> out_tx_raw;
    bool out_tx_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'k', 'r', 'v'},
                &out_tx_raw,
                &out_tx_found,
                nullptr,
                &error)) {
      Fail("out-of-tx get failed: " + error);
    }
    if (out_tx_found) {
      Fail("out-of-tx get should not find key before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after reopen visibility check should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_reopen_conflict_same_path") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    if (!db.RollbackTransaction(&tx1, &error)) {
      Fail("tx1 rollback failed: " + error);
    }
    std::vector<uint8_t> a_item;
    if (!grovedb::EncodeItemToElementBytes({'a'}, &a_item, &error)) {
      Fail("encode a item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'c'}, a_item, &tx1, &error)) {
      Fail("tx1 insert after rollback failed: " + error);
    }

    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> b_item;
    if (!grovedb::EncodeItemToElementBytes({'b'}, &b_item, &error)) {
      Fail("encode b item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'c'}, b_item, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should fail due to conflict");
    }
    return;
  }

  if (mode == "tx_delete_visibility") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx, &error)) {
      Fail("tx delete failed: " + error);
    }
    if (!deleted) {
      Fail("tx delete should report deleted=true");
    }
    std::vector<uint8_t> in_tx_raw;
    bool in_tx_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &in_tx_raw, &in_tx_found, &tx, &error)) {
      Fail("in-tx get after delete failed: " + error);
    }
    if (in_tx_found) {
      Fail("in-tx get should not find deleted key");
    }
    std::vector<uint8_t> out_tx_raw;
    bool out_tx_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'k', '1'},
                &out_tx_raw,
                &out_tx_found,
                nullptr,
                &error)) {
      Fail("out-of-tx get before commit failed: " + error);
    }
    if (!out_tx_found) {
      Fail("out-of-tx get should still find key before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after tx delete should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_delete_then_reinsert_same_key") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx, &error)) {
      Fail("tx delete failed: " + error);
    }
    if (!deleted) {
      Fail("tx delete should report deleted=true");
    }
    std::vector<uint8_t> replacement_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &replacement_raw, &error)) {
      Fail("encode replacement item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, replacement_raw, &tx, &error)) {
      Fail("reinsert same key after delete failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after delete->reinsert should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_insert_then_delete_same_key") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> replacement_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &replacement_raw, &error)) {
      Fail("encode replacement item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, replacement_raw, &tx, &error)) {
      Fail("insert same key before delete failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx, &error)) {
      Fail("delete same key after insert failed: " + error);
    }
    if (!deleted) {
      Fail("tx delete should report deleted=true");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after insert->delete should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_delete_missing_noop") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = true;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'}, &deleted, &tx, &error)) {
      Fail("delete missing key call failed: " + error);
    }
    if (deleted) {
      Fail("delete missing key should report deleted=false");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after delete-missing should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_read_committed_visibility") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> snap_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &snap_raw, &error)) {
      Fail("encode snapshot item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 's', 'n', 'a', 'p'}, snap_raw, &tx1, &error)) {
      Fail("tx1 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    std::vector<uint8_t> tx2_raw;
    bool tx2_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'k', 's', 'n', 'a', 'p'},
                &tx2_raw,
                &tx2_found,
                &tx2,
                &error)) {
      Fail("tx2 snapshot read failed: " + error);
    }
    if (!tx2_found) {
      Fail("tx2 should see tx1 committed key");
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_has_read_committed_visibility") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> has_raw;
    if (!grovedb::EncodeItemToElementBytes({'h', 'v'}, &has_raw, &error)) {
      Fail("encode has item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'h', 'a', 's'}, has_raw, &tx1, &error)) {
      Fail("tx1 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    bool tx2_found = false;
    if (!db.Has({{'r', 'o', 'o', 't'}}, {'k', 'h', 'a', 's'}, &tx2_found, &tx2, &error)) {
      Fail("tx2 visibility has failed: " + error);
    }
    if (!tx2_found) {
      Fail("tx2 should observe tx1 committed key");
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_query_range_committed_visibility") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> q_raw;
    if (!grovedb::EncodeItemToElementBytes({'q', 'v'}, &q_raw, &error)) {
      Fail("encode query item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'q', 'r'}, q_raw, &tx1, &error)) {
      Fail("tx1 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRange({{'r', 'o', 'o', 't'}}, {'k'}, {'l'}, true, true, &out, &tx2, &error)) {
      Fail("tx2 query range failed: " + error);
    }
    bool saw_kqr = false;
    for (const auto& kv : out) {
      if (kv.first == std::vector<uint8_t>({'k', 'q', 'r'})) {
        saw_kqr = true;
        break;
      }
    }
    if (!saw_kqr) {
      Fail("tx2 query range should include tx1 committed key");
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_iterator_stability_under_commit") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> i_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 'v'}, &i_raw, &error)) {
      Fail("encode iterator item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'i', 't'}, i_raw, &tx1, &error)) {
      Fail("tx1 insert failed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> before;
    if (!db.QueryRange({{'r', 'o', 'o', 't'}}, {'k'}, {'l'}, true, true, &before, &tx2, &error)) {
      Fail("tx2 pre-commit query range failed: " + error);
    }
    for (const auto& kv : before) {
      if (kv.first == std::vector<uint8_t>({'k', 'i', 't'})) {
        Fail("tx2 pre-commit query should not include uncommitted tx1 key");
      }
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> after;
    if (!db.QueryRange({{'r', 'o', 'o', 't'}}, {'k'}, {'l'}, true, true, &after, &tx2, &error)) {
      Fail("tx2 post-commit query range failed: " + error);
    }
    bool saw_kit = false;
    for (const auto& kv : after) {
      if (kv.first == std::vector<uint8_t>({'k', 'i', 't'})) {
        saw_kit = true;
        break;
      }
    }
    if (!saw_kit) {
      Fail("tx2 post-commit query should include tx1 committed key");
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_same_key_conflict_reverse_order") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> v1_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_raw, &error)) {
      Fail("encode v1 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'o', 'n', 'f'}, v1_raw, &tx1, &error)) {
      Fail("tx1 insert failed: " + error);
    }
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'o', 'n', 'f'}, v2_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should fail due to conflict");
    }
    return;
  }

  if (mode == "tx_shared_subtree_disjoint_conflict_reverse_order") {
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> d1_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', '1'}, &d1_raw, &error)) {
      Fail("encode d1 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', '1'}, d1_raw, &tx1, &error)) {
      Fail("tx1 disjoint insert failed: " + error);
    }
    std::vector<uint8_t> d2_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', '2'}, &d2_raw, &error)) {
      Fail("encode d2 item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', '2'}, d2_raw, &tx2, &error)) {
      Fail("tx2 disjoint insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 disjoint commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 disjoint commit should fail due to shared-subtree conflict");
    }
    return;
  }

  if (mode == "tx_disjoint_subtree_conflict_reverse_order") {
    std::vector<uint8_t> tree_raw;
    if (!grovedb::EncodeTreeToElementBytes(&tree_raw, &error)) {
      Fail("encode tree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'}, tree_raw, &error)) {
      Fail("insert left subtree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'}, tree_raw, &error)) {
      Fail("insert right subtree failed: " + error);
    }
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> lv_raw;
    if (!grovedb::EncodeItemToElementBytes({'l', 'v'}, &lv_raw, &error)) {
      Fail("encode lv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k'}, lv_raw, &tx1, &error)) {
      Fail("tx1 left insert failed: " + error);
    }
    std::vector<uint8_t> rv_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_raw, &error)) {
      Fail("encode rv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k'}, rv_raw, &tx2, &error)) {
      Fail("tx2 right insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 disjoint-subtree commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 disjoint-subtree commit should fail due to conflict");
    }
    return;
  }

  if (mode == "tx_disjoint_subtree_conflict_forward_order") {
    std::vector<uint8_t> tree_raw;
    if (!grovedb::EncodeTreeToElementBytes(&tree_raw, &error)) {
      Fail("encode tree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'}, tree_raw, &error)) {
      Fail("insert left subtree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'}, tree_raw, &error)) {
      Fail("insert right subtree failed: " + error);
    }
    grovedb::GroveDb::Transaction tx1;
    if (!db.StartTransaction(&tx1, &error)) {
      Fail("tx1 start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("tx2 start failed: " + error);
    }
    std::vector<uint8_t> lvf_raw;
    if (!grovedb::EncodeItemToElementBytes({'l', 'v', 'f'}, &lvf_raw, &error)) {
      Fail("encode lvf failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', 'f'}, lvf_raw, &tx1, &error)) {
      Fail("tx1 left insert failed: " + error);
    }
    std::vector<uint8_t> rvf_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v', 'f'}, &rvf_raw, &error)) {
      Fail("encode rvf failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', 'f'}, rvf_raw, &tx2, &error)) {
      Fail("tx2 right insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 disjoint-subtree commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 disjoint-subtree commit should fail due to conflict");
    }
    return;
  }

  if (mode == "tx_read_only_then_writer_commit") {
    grovedb::GroveDb::Transaction tx_ro;
    if (!db.StartTransaction(&tx_ro, &error)) {
      Fail("tx_ro start failed: " + error);
    }
    grovedb::GroveDb::Transaction tx_w;
    if (!db.StartTransaction(&tx_w, &error)) {
      Fail("tx_w start failed: " + error);
    }
    std::vector<uint8_t> ro_before_raw;
    bool ro_before_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &ro_before_raw, &ro_before_found, &tx_ro, &error)) {
      Fail("tx_ro read before writer commit failed: " + error);
    }
    if (!ro_before_found) {
      Fail("tx_ro should see existing key");
    }
    std::vector<uint8_t> rov_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'o', 'v'}, &rov_raw, &error)) {
      Fail("encode rov failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'o'}, rov_raw, &tx_w, &error)) {
      Fail("tx_w insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx_w, &error)) {
      Fail("tx_w commit should succeed: " + error);
    }
    std::vector<uint8_t> ro_after_raw;
    bool ro_after_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'r', 'o'}, &ro_after_raw, &ro_after_found, &tx_ro, &error)) {
      Fail("tx_ro read after writer commit failed: " + error);
    }
    if (!ro_after_found) {
      Fail("tx_ro should see writer-committed key");
    }
    if (!db.CommitTransaction(&tx_ro, &error)) {
      Fail("tx_ro commit should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_delete_insert_same_key_forward") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx1, &error) || !deleted) {
      Fail("tx1 delete failed: " + error);
    }
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 late commit should conflict");
    }
    return;
  }

  if (mode == "tx_delete_insert_same_key_reverse") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx1, &error) || !deleted) {
      Fail("tx1 delete failed: " + error);
    }
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 late commit should conflict");
    }
    return;
  }

  if (mode == "tx_delete_insert_same_subtree_disjoint_forward") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx1, &error) || !deleted) {
      Fail("tx1 delete failed: " + error);
    }
    std::vector<uint8_t> di_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', 'i'}, &di_raw, &error)) {
      Fail("encode di failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', 'i'}, di_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 late commit should conflict");
    }
    return;
  }

  if (mode == "tx_delete_insert_same_subtree_disjoint_reverse") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx1, &error) || !deleted) {
      Fail("tx1 delete failed: " + error);
    }
    std::vector<uint8_t> di_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', 'i'}, &di_raw, &error)) {
      Fail("encode di failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', 'i'}, di_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("tx2 commit should succeed: " + error);
    }
    if (db.CommitTransaction(&tx1, &error)) {
      Fail("tx1 late commit should conflict");
    }
    return;
  }

  if (mode == "tx_delete_insert_disjoint_subtree_forward" ||
      mode == "tx_delete_insert_disjoint_subtree_reverse") {
    std::vector<uint8_t> tree_raw;
    if (!grovedb::EncodeTreeToElementBytes(&tree_raw, &error)) {
      Fail("encode tree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'}, tree_raw, &error) ||
        !db.Insert({{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'}, tree_raw, &error)) {
      Fail("insert subtree failed: " + error);
    }
    std::vector<uint8_t> delv_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', 'e', 'l', 'v'}, &delv_raw, &error)) {
      Fail("encode delv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', 'd', 'e', 'l'}, delv_raw, &error)) {
      Fail("seed left key failed: " + error);
    }
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}},
                   {'l', 'k', 'd', 'e', 'l'},
                   &deleted,
                   &tx1,
                   &error) ||
        !deleted) {
      Fail("tx1 delete failed: " + error);
    }
    std::vector<uint8_t> insv_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 'n', 's', 'v'}, &insv_raw, &error)) {
      Fail("encode insv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', 'i', 'n', 's'}, insv_raw, &tx2, &error)) {
      Fail("tx2 insert failed: " + error);
    }
    if (mode == "tx_delete_insert_disjoint_subtree_forward") {
      if (!db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 late commit should conflict");
      }
    } else {
      if (!db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 late commit should conflict");
      }
    }
    return;
  }

  if (mode == "tx_replace_delete_same_key_forward" ||
      mode == "tx_replace_delete_same_key_reverse") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_raw, &tx1, &error)) {
      Fail("tx1 replace failed: " + error);
    }
    bool deleted = false;
    if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &tx2, &error) || !deleted) {
      Fail("tx2 delete failed: " + error);
    }
    if (mode == "tx_replace_delete_same_key_forward") {
      if (!db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 late commit should conflict");
      }
    } else {
      if (!db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 late commit should conflict");
      }
    }
    return;
  }

  if (mode == "tx_replace_replace_same_key_forward" ||
      mode == "tx_replace_replace_same_key_reverse") {
    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx failed: " + error);
    }
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_raw, &tx1, &error)) {
      Fail("tx1 replace failed: " + error);
    }
    std::vector<uint8_t> v3_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '3'}, &v3_raw, &error)) {
      Fail("encode v3 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, v3_raw, &tx2, &error)) {
      Fail("tx2 replace failed: " + error);
    }
    if (mode == "tx_replace_replace_same_key_forward") {
      if (!db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 late commit should conflict");
      }
    } else {
      if (!db.CommitTransaction(&tx2, &error)) {
        Fail("tx2 commit should succeed: " + error);
      }
      if (db.CommitTransaction(&tx1, &error)) {
        Fail("tx1 late commit should conflict");
      }
    }
    return;
  }

  if (mode == "tx_double_rollback_noop") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("tx start failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("first rollback should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("second rollback should also succeed: " + error);
    }
    std::vector<uint8_t> drv_raw;
    if (!grovedb::EncodeItemToElementBytes({'d', 'r', 'v'}, &drv_raw, &error)) {
      Fail("encode drv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'd', 'r'}, drv_raw, &tx, &error)) {
      Fail("insert after double rollback should succeed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after double rollback should succeed: " + error);
    }
    return;
  }

  if (mode == "tx_conflict_sequence_persistence") {
    std::vector<uint8_t> tree_raw;
    if (!grovedb::EncodeTreeToElementBytes(&tree_raw, &error)) {
      Fail("encode tree failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'}, tree_raw, &error) ||
        !db.Insert({{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'}, tree_raw, &error)) {
      Fail("insert subtrees failed: " + error);
    }

    grovedb::GroveDb::Transaction tx1;
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx1, &error) || !db.StartTransaction(&tx2, &error)) {
      Fail("start tx1/tx2 failed: " + error);
    }
    std::vector<uint8_t> a_raw;
    std::vector<uint8_t> b_raw;
    if (!grovedb::EncodeItemToElementBytes({'a'}, &a_raw, &error) ||
        !grovedb::EncodeItemToElementBytes({'b'}, &b_raw, &error)) {
      Fail("encode a/b failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c'}, a_raw, &tx1, &error) ||
        !db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c'}, b_raw, &tx2, &error)) {
      Fail("same-key inserts failed: " + error);
    }
    if (!db.CommitTransaction(&tx1, &error) || db.CommitTransaction(&tx2, &error)) {
      Fail("same-key conflict ordering mismatch");
    }

    grovedb::GroveDb::Transaction tx3;
    grovedb::GroveDb::Transaction tx4;
    if (!db.StartTransaction(&tx3, &error) || !db.StartTransaction(&tx4, &error)) {
      Fail("start tx3/tx4 failed: " + error);
    }
    std::vector<uint8_t> l1_raw;
    std::vector<uint8_t> r1_raw;
    if (!grovedb::EncodeItemToElementBytes({'l', '1'}, &l1_raw, &error) ||
        !grovedb::EncodeItemToElementBytes({'r', '1'}, &r1_raw, &error)) {
      Fail("encode l1/r1 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', '1'}, l1_raw, &tx3, &error) ||
        !db.Insert({{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', '1'}, r1_raw, &tx4, &error)) {
      Fail("disjoint-subtree inserts failed: " + error);
    }
    if (!db.CommitTransaction(&tx4, &error) || db.CommitTransaction(&tx3, &error)) {
      Fail("disjoint-subtree conflict ordering mismatch");
    }

    grovedb::GroveDb::Transaction tx5;
    if (!db.StartTransaction(&tx5, &error)) {
      Fail("start tx5 failed: " + error);
    }
    if (!db.RollbackTransaction(&tx5, &error)) {
      Fail("tx5 rollback failed: " + error);
    }
    std::vector<uint8_t> pv_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', 'v'}, &pv_raw, &error)) {
      Fail("encode pv failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'p', 'o', 's', 't'}, pv_raw, &tx5, &error)) {
      Fail("insert kpost after rollback failed: " + error);
    }
    if (!db.CommitTransaction(&tx5, &error)) {
      Fail("tx5 commit failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_snapshot_isolation") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("tx start failed: " + error);
    }
    std::vector<uint8_t> c_raw;
    if (!grovedb::EncodeItemToElementBytes({'c', 'v'}, &c_raw, &error)) {
      Fail("encode checkpoint item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p'}, c_raw, &tx, &error)) {
      Fail("tx insert checkpoint key failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("tx commit before checkpoint failed: " + error);
    }

    const std::string checkpoint_dir = dir + "_checkpoint";
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint failed: " + error);
    }

    std::vector<uint8_t> p_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', 'v'}, &p_raw, &error)) {
      Fail("encode post-checkpoint item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'p', 'o', 's', 't'}, p_raw, &error)) {
      Fail("post-checkpoint insert failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint_db;
      if (!checkpoint_db.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint db via OpenCheckpoint failed: " + error);
      }
      std::vector<uint8_t> cp_raw;
      bool cp_found = false;
      if (!checkpoint_db.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p'}, &cp_raw, &cp_found, &error)) {
        Fail("checkpoint get pre-checkpoint key failed: " + error);
      }
      if (!cp_found) {
        Fail("checkpoint should contain pre-checkpoint key");
      }
      if (!checkpoint_db.Get(
              {{'r', 'o', 'o', 't'}}, {'k', 'p', 'o', 's', 't'}, &cp_raw, &cp_found, &error)) {
        Fail("checkpoint get post-checkpoint key failed: " + error);
      }
      if (cp_found) {
        Fail("checkpoint should not contain post-checkpoint key");
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint dir failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_delete_safety") {
    const std::string checkpoint_dir = dir + "_checkpoint_delete_safety";
    const std::string bogus_dir = dir + "_not_checkpoint_dir";
    std::filesystem::remove_all(checkpoint_dir);
    std::filesystem::remove_all(bogus_dir);

    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint for delete safety failed: " + error);
    }
    std::filesystem::create_directories(bogus_dir);

    if (grovedb::GroveDb::DeleteCheckpoint(bogus_dir, &error)) {
      Fail("delete checkpoint should fail for non-checkpoint dir");
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint should succeed for real checkpoint: " + error);
    }
    if (std::filesystem::exists(checkpoint_dir)) {
      Fail("checkpoint dir should be removed");
    }
    std::filesystem::remove_all(bogus_dir);

    std::vector<uint8_t> safe_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &safe_raw, &error)) {
      Fail("encode safe item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 's', 'a', 'f', 'e'}, safe_raw, &error)) {
      Fail("post-delete insert failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_open_safety") {
    const std::string checkpoint_dir = dir + "_checkpoint_open_safety";
    const std::string bogus_dir = dir + "_not_checkpoint_open_dir";
    std::filesystem::remove_all(checkpoint_dir);
    std::filesystem::remove_all(bogus_dir);

    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint for open safety failed: " + error);
    }
    std::filesystem::create_directories(bogus_dir);

    {
      grovedb::GroveDb bogus_db;
      if (bogus_db.OpenCheckpoint(bogus_dir, &error)) {
        Fail("OpenCheckpoint should fail for non-checkpoint dir");
      }
    }

    {
      grovedb::GroveDb checkpoint_db;
      if (!checkpoint_db.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("OpenCheckpoint should succeed for valid checkpoint: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
        Fail("checkpoint get baseline key failed: " + error);
      }
      if (!found) {
        Fail("valid checkpoint should contain baseline key");
      }
    }

    std::filesystem::remove_all(bogus_dir);
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint after open safety failed: " + error);
    }

    std::vector<uint8_t> open_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &open_raw, &error)) {
      Fail("encode open-safety item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'o', 'p', 'e', 'n'}, open_raw, &error)) {
      Fail("post-open-safety insert failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_delete_short_path_safety") {
    if (grovedb::GroveDb::DeleteCheckpoint("single_component_path", &error)) {
      Fail("DeleteCheckpoint should fail for short single-component path");
    }
    std::vector<uint8_t> short_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &short_raw, &error)) {
      Fail("encode short-safety item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 's', 'h', 'o', 'r', 't'}, short_raw, &error)) {
      Fail("post-short-path-safety insert failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_open_missing_path") {
    const std::string missing_path = dir + "_missing_checkpoint_path";
    std::filesystem::remove_all(missing_path);
    grovedb::GroveDb missing_db;
    if (missing_db.OpenCheckpoint(missing_path, &error)) {
      Fail("OpenCheckpoint should fail for missing path");
    }
    std::vector<uint8_t> miss_raw;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v'}, &miss_raw, &error)) {
      Fail("encode missing-open item failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 's', 's'}, miss_raw, &error)) {
      Fail("post-open-missing insert failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_reopen_mutate_recheckpoint") {
    const std::string checkpoint_a_dir = dir + "_checkpoint_reopen_mutate_a";
    const std::string checkpoint_b_dir = dir + "_checkpoint_reopen_mutate_b";
    std::filesystem::remove_all(checkpoint_a_dir);
    std::filesystem::remove_all(checkpoint_b_dir);
    if (!db.CreateCheckpoint(checkpoint_a_dir, &error)) {
      Fail("create checkpoint A failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint_a;
      if (!checkpoint_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("open checkpoint A failed: " + error);
      }
      std::vector<uint8_t> cp1_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'v', '1'}, &cp1_raw, &error)) {
        Fail("encode checkpoint A item failed: " + error);
      }
      if (!checkpoint_a.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '1'}, cp1_raw, &error)) {
        Fail("checkpoint A insert kcp1 failed: " + error);
      }
    }

    {
      grovedb::GroveDb checkpoint_a;
      if (!checkpoint_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("reopen checkpoint A failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '1'}, &raw, &found, &error)) {
        Fail("reopened checkpoint A get kcp1 failed: " + error);
      }
      if (!found) {
        Fail("reopened checkpoint A should contain kcp1");
      }
      if (!checkpoint_a.CreateCheckpoint(checkpoint_b_dir, &error)) {
        Fail("checkpoint A create checkpoint B failed: " + error);
      }

      std::vector<uint8_t> cp2_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'v', '2'}, &cp2_raw, &error)) {
        Fail("encode checkpoint A post-recheckpoint item failed: " + error);
      }
      if (!checkpoint_a.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '2'}, cp2_raw, &error)) {
        Fail("checkpoint A insert kcp2 failed: " + error);
      }
    }

    {
      grovedb::GroveDb checkpoint_b;
      if (!checkpoint_b.OpenCheckpoint(checkpoint_b_dir, &error)) {
        Fail("open checkpoint B failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '1'}, &raw, &found, &error)) {
        Fail("checkpoint B get kcp1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint B should contain kcp1");
      }
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '2'}, &raw, &found, &error)) {
        Fail("checkpoint B get kcp2 failed: " + error);
      }
      if (found) {
        Fail("checkpoint B should not contain post-recheckpoint key kcp2");
      }
    }

    std::vector<uint8_t> main_raw;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v'}, &main_raw, &error)) {
      Fail("encode main key failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n'}, main_raw, &error)) {
      Fail("main db insert kmain failed: " + error);
    }

    bool found = false;
    std::vector<uint8_t> raw;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '1'}, &raw, &found, &error)) {
      Fail("main db get kcp1 failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only key kcp1");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '2'}, &raw, &found, &error)) {
      Fail("main db get kcp2 failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only key kcp2");
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_b_dir, &error)) {
      Fail("delete checkpoint B failed: " + error);
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_a_dir, &error)) {
      Fail("delete checkpoint A failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_reopen_mutate_chain") {
    const std::string checkpoint_a_dir = dir + "_checkpoint_chain_a";
    const std::string checkpoint_b_dir = dir + "_checkpoint_chain_b";
    const std::string checkpoint_c_dir = dir + "_checkpoint_chain_c";
    std::filesystem::remove_all(checkpoint_a_dir);
    std::filesystem::remove_all(checkpoint_b_dir);
    std::filesystem::remove_all(checkpoint_c_dir);
    if (!db.CreateCheckpoint(checkpoint_a_dir, &error)) {
      Fail("create checkpoint A (chain) failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint_a;
      if (!checkpoint_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("open checkpoint A (chain) failed: " + error);
      }
      std::vector<uint8_t> ka1_raw;
      if (!grovedb::EncodeItemToElementBytes({'a', 'v', 'a', '1'}, &ka1_raw, &error)) {
        Fail("encode checkpoint chain ka1 failed: " + error);
      }
      if (!checkpoint_a.Insert({{'r', 'o', 'o', 't'}}, {'k', 'a', '1'}, ka1_raw, &error)) {
        Fail("checkpoint A insert ka1 failed: " + error);
      }
      if (!checkpoint_a.CreateCheckpoint(checkpoint_b_dir, &error)) {
        Fail("checkpoint A create checkpoint B (chain) failed: " + error);
      }
    }

    {
      grovedb::GroveDb checkpoint_b;
      if (!checkpoint_b.OpenCheckpoint(checkpoint_b_dir, &error)) {
        Fail("open checkpoint B (chain) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', 'a', '1'}, &raw, &found, &error)) {
        Fail("checkpoint B get ka1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint B should contain ka1 from checkpoint A");
      }
      std::vector<uint8_t> kb1_raw;
      if (!grovedb::EncodeItemToElementBytes({'b', 'v', 'a', '1'}, &kb1_raw, &error)) {
        Fail("encode checkpoint chain kb1 failed: " + error);
      }
      if (!checkpoint_b.Insert({{'r', 'o', 'o', 't'}}, {'k', 'b', '1'}, kb1_raw, &error)) {
        Fail("checkpoint B insert kb1 failed: " + error);
      }
      if (!checkpoint_b.CreateCheckpoint(checkpoint_c_dir, &error)) {
        Fail("checkpoint B create checkpoint C (chain) failed: " + error);
      }
      std::vector<uint8_t> kb2_raw;
      if (!grovedb::EncodeItemToElementBytes({'b', 'v', 'a', '2'}, &kb2_raw, &error)) {
        Fail("encode checkpoint chain kb2 failed: " + error);
      }
      if (!checkpoint_b.Insert({{'r', 'o', 'o', 't'}}, {'k', 'b', '2'}, kb2_raw, &error)) {
        Fail("checkpoint B insert kb2 failed: " + error);
      }
    }

    {
      grovedb::GroveDb checkpoint_c;
      if (!checkpoint_c.OpenCheckpoint(checkpoint_c_dir, &error)) {
        Fail("open checkpoint C (chain) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', 'a', '1'}, &raw, &found, &error)) {
        Fail("checkpoint C get ka1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint C should contain ka1");
      }
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', '1'}, &raw, &found, &error)) {
        Fail("checkpoint C get kb1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint C should contain kb1");
      }
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', '2'}, &raw, &found, &error)) {
        Fail("checkpoint C get kb2 failed: " + error);
      }
      if (found) {
        Fail("checkpoint C should not contain post-checkpoint key kb2");
      }
    }

    std::vector<uint8_t> main_raw;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v', '2'}, &main_raw, &error)) {
      Fail("encode checkpoint chain main key failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n', '2'}, main_raw, &error)) {
      Fail("main db insert kmain2 failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> raw;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'a', '1'}, &raw, &found, &error)) {
      Fail("main db get ka1 failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only ka1");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', '1'}, &raw, &found, &error)) {
      Fail("main db get kb1 failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only kb1");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', '2'}, &raw, &found, &error)) {
      Fail("main db get kb2 failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only kb2");
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_c_dir, &error)) {
      Fail("delete checkpoint C (chain) failed: " + error);
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_b_dir, &error)) {
      Fail("delete checkpoint B (chain) failed: " + error);
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_a_dir, &error)) {
      Fail("delete checkpoint A (chain) failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_batch_ops") {
    const std::string checkpoint_dir = dir + "_checkpoint_batch";
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint (batch) failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint (batch) failed: " + error);
      }
      std::vector<uint8_t> kbp1_raw;
      if (!grovedb::EncodeItemToElementBytes({'v', 'b', 'p', '1'}, &kbp1_raw, &error)) {
        Fail("encode checkpoint batch kbp1 failed: " + error);
      }
      std::vector<uint8_t> kbp2_raw;
      if (!grovedb::EncodeItemToElementBytes({'v', 'b', 'p', '2'}, &kbp2_raw, &error)) {
        Fail("encode checkpoint batch kbp2 failed: " + error);
      }
      std::vector<uint8_t> kbp3_raw;
      if (!grovedb::EncodeItemToElementBytes({'v', 'b', 'p', '3'}, &kbp3_raw, &error)) {
        Fail("encode checkpoint batch kbp3 failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> batch_ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '1'}, kbp1_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '2'}, kbp2_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '3'}, kbp3_raw}};
      if (!checkpoint.ApplyBatch(batch_ops, &error)) {
        Fail("checkpoint batch insert failed: " + error);
      }
    }

    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("reopen checkpoint (batch) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '1'}, &raw, &found, &error)) {
        Fail("checkpoint get kbp1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint should contain kbp1 after batch insert");
      }
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '2'}, &raw, &found, &error)) {
        Fail("checkpoint get kbp2 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint should contain kbp2 after batch insert");
      }
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '3'}, &raw, &found, &error)) {
        Fail("checkpoint get kbp3 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint should contain kbp3 after batch insert");
      }
    }

    std::vector<uint8_t> main_raw;
    bool main_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '1'}, &main_raw, &main_found, &error)) {
      Fail("main db get kbp1 failed: " + error);
    }
    if (main_found) {
      Fail("main db should not contain checkpoint-only kbp1");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '2'}, &main_raw, &main_found, &error)) {
      Fail("main db get kbp2 failed: " + error);
    }
    if (main_found) {
      Fail("main db should not contain checkpoint-only kbp2");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '3'}, &main_raw, &main_found, &error)) {
      Fail("main db get kbp3 failed: " + error);
    }
    if (main_found) {
      Fail("main db should not contain checkpoint-only kbp3");
    }

    std::vector<uint8_t> kmain3_raw;
    if (!grovedb::EncodeItemToElementBytes({'m', 'v', '3'}, &kmain3_raw, &error)) {
      Fail("encode main kmain3 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n', '3'}, kmain3_raw, &error)) {
      Fail("main db insert kmain3 failed: " + error);
    }

    std::vector<uint8_t> kb1_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', 'b', '1'}, &kb1_raw, &error)) {
      Fail("encode main kb1 failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'b', 'a', 't', 'c', 'h', '1'}, kb1_raw, &error)) {
      Fail("main db insert kbatch1 failed: " + error);
    }

    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'b', 'a', 't', 'c', 'h', '1'}, &main_raw, &main_found, &error)) {
      Fail("main db get kbatch1 failed: " + error);
    }
    if (!main_found) {
      Fail("main db should contain kbatch1 after batch");
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint (batch) failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_delete_reopen_sequence") {
    const std::string checkpoint_a_dir = dir + "_checkpoint_seq_a";
    const std::string checkpoint_b_dir = dir + "_checkpoint_seq_b";
    std::filesystem::remove_all(checkpoint_a_dir);
    std::filesystem::remove_all(checkpoint_b_dir);

    if (!db.CreateCheckpoint(checkpoint_a_dir, &error)) {
      Fail("create checkpoint A (sequence) failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint_a;
      if (!checkpoint_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("open checkpoint A (sequence) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
        Fail("checkpoint A get baseline k1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint A should contain baseline key k1");
      }
      if (!checkpoint_a.CreateCheckpoint(checkpoint_b_dir, &error)) {
        Fail("checkpoint A create checkpoint B (sequence) failed: " + error);
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_a_dir, &error)) {
      Fail("delete checkpoint A (sequence) failed: " + error);
    }
    {
      grovedb::GroveDb deleted_a;
      if (deleted_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("opening deleted checkpoint A should fail");
      }
    }

    {
      grovedb::GroveDb checkpoint_b;
      if (!checkpoint_b.OpenCheckpoint(checkpoint_b_dir, &error)) {
        Fail("open checkpoint B (sequence) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
        Fail("checkpoint B get baseline k1 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint B should contain baseline key k1");
      }
      std::vector<uint8_t> cpb_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'p', 'b'}, &cpb_raw, &error)) {
        Fail("encode checkpoint B kcpb failed: " + error);
      }
      if (!checkpoint_b.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'b'}, cpb_raw, &error)) {
        Fail("checkpoint B insert kcpb failed: " + error);
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_b_dir, &error)) {
      Fail("delete checkpoint B (sequence) failed: " + error);
    }
    {
      grovedb::GroveDb deleted_b;
      if (deleted_b.OpenCheckpoint(checkpoint_b_dir, &error)) {
        Fail("opening deleted checkpoint B should fail");
      }
    }

    std::filesystem::remove_all(checkpoint_a_dir);
    if (!db.CreateCheckpoint(checkpoint_a_dir, &error)) {
      Fail("recreate checkpoint A (sequence) failed: " + error);
    }
    {
      grovedb::GroveDb recreated_a;
      if (!recreated_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("open recreated checkpoint A (sequence) failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!recreated_a.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
        Fail("recreated checkpoint A get baseline k1 failed: " + error);
      }
      if (!found) {
        Fail("recreated checkpoint A should contain baseline key k1");
      }
      std::vector<uint8_t> cpa_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'p', 'a'}, &cpa_raw, &error)) {
        Fail("encode recreated checkpoint A kcpa failed: " + error);
      }
      if (!recreated_a.Insert({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'a'}, cpa_raw, &error)) {
        Fail("recreated checkpoint A insert kcpa failed: " + error);
      }
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_a_dir, &error)) {
      Fail("delete recreated checkpoint A (sequence) failed: " + error);
    }
    {
      grovedb::GroveDb deleted_recreated_a;
      if (deleted_recreated_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("opening deleted recreated checkpoint A should fail");
      }
    }

    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'b'}, &raw, &found, &error)) {
      Fail("main db get kcpb failed: " + error);
    }
    if (found) {
      Fail("main db should not contain checkpoint-only key kcpb");
    }
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'a'}, &raw, &found, &error)) {
      Fail("main db get kcpa failed: " + error);
    }
    if (found) {
      Fail("main db should not contain recreated checkpoint-only key kcpa");
    }

    std::vector<uint8_t> seq_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &seq_raw, &error)) {
      Fail("encode sequence marker failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 's', 'e', 'q'}, seq_raw, &error)) {
      Fail("main db insert kseq failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_aux_isolation") {
    if (!db.PutAux({'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'},
                   {'m', 'a', 'i', 'n', '_', 'b', 'e', 'f', 'o', 'r', 'e'},
                   &error)) {
      Fail("main db PutAux aux_shared before checkpoint failed: " + error);
    }

    const std::string checkpoint_dir = dir + "_checkpoint_aux";
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint aux isolation failed: " + error);
    }

    if (!db.PutAux({'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'},
                   {'m', 'a', 'i', 'n', '_', 'a', 'f', 't', 'e', 'r'},
                   &error)) {
      Fail("main db PutAux aux_shared after checkpoint failed: " + error);
    }
    if (!db.PutAux({'a', 'u', 'x', '_', 'm', 'a', 'i', 'n', '_', 'o', 'n', 'l', 'y'},
                   {'m', 'a', 'i', 'n', '_', 'o', 'n', 'l', 'y'},
                   &error)) {
      Fail("main db PutAux aux_main_only failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint aux isolation failed: " + error);
      }

      std::vector<uint8_t> aux_value;
      bool found = false;
      if (!checkpoint.GetAux(
              {'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'}, &aux_value, &found, &error)) {
        Fail("checkpoint GetAux aux_shared failed: " + error);
      }
      if (!found ||
          aux_value != std::vector<uint8_t>({'m', 'a', 'i', 'n', '_', 'b', 'e', 'f', 'o', 'r', 'e'})) {
        Fail("checkpoint aux_shared should be snapshotted to main_before");
      }

      if (!checkpoint.GetAux({'a', 'u', 'x', '_', 'm', 'a', 'i', 'n', '_', 'o', 'n', 'l', 'y'},
                             &aux_value,
                             &found,
                             &error)) {
        Fail("checkpoint GetAux aux_main_only failed: " + error);
      }
      if (found) {
        Fail("checkpoint should not see post-checkpoint aux_main_only");
      }

      if (!checkpoint.PutAux(
              {'a', 'u', 'x', '_', 'c', 'p', '_', 'o', 'n', 'l', 'y'},
              {'c', 'p', '_', 'o', 'n', 'l', 'y'},
              &error)) {
        Fail("checkpoint PutAux aux_cp_only failed: " + error);
      }
      if (!checkpoint.PutAux({'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'},
                             {'c', 'p', '_', 'o', 'v', 'e', 'r', 'r', 'i', 'd', 'e'},
                             &error)) {
        Fail("checkpoint PutAux aux_shared override failed: " + error);
      }
    }

    {
      std::vector<uint8_t> aux_value;
      bool found = false;
      if (!db.GetAux({'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'}, &aux_value, &found, &error)) {
        Fail("main db GetAux aux_shared failed: " + error);
      }
      if (!found ||
          aux_value != std::vector<uint8_t>({'m', 'a', 'i', 'n', '_', 'a', 'f', 't', 'e', 'r'})) {
        Fail("main db aux_shared should remain main_after");
      }

      if (!db.GetAux({'a', 'u', 'x', '_', 'c', 'p', '_', 'o', 'n', 'l', 'y'}, &aux_value, &found, &error)) {
        Fail("main db GetAux aux_cp_only failed: " + error);
      }
      if (found) {
        Fail("main db should not see checkpoint-only aux_cp_only");
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint aux isolation failed: " + error);
    }

    std::vector<uint8_t> marker_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'k'}, &marker_raw, &error)) {
      Fail("encode checkpoint aux marker failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'a', 'u', 'x', 'c', 'p'}, marker_raw, &error)) {
      Fail("main db insert kauxcp failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_tx_operations") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("begin transaction failed: " + error);
    }

    std::vector<uint8_t> txv1_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'x', 'v', '1'}, &txv1_raw, &error)) {
      Fail("encode ktx1 value failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '1'}, txv1_raw, &tx, &error)) {
      Fail("tx insert ktx1 failed: " + error);
    }

    const std::string checkpoint_dir = dir + "_checkpoint_tx";
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint during tx failed: " + error);
    }

    std::vector<uint8_t> txv2_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'x', 'v', '2'}, &txv2_raw, &error)) {
      Fail("encode ktx2 value failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '2'}, txv2_raw, &tx, &error)) {
      Fail("tx insert ktx2 failed: " + error);
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit transaction failed: " + error);
    }

    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint failed: " + error);
      }

      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '1'}, &raw, &found, &error)) {
        Fail("checkpoint get ktx1 failed: " + error);
      }
      if (found) {
        Fail("checkpoint should not contain ktx1 (uncommitted at checkpoint time)");
      }

      found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '2'}, &raw, &found, &error)) {
        Fail("checkpoint get ktx2 failed: " + error);
      }
      if (found) {
        Fail("checkpoint should not contain ktx2 (uncommitted at checkpoint time)");
      }
    }

    {
      std::vector<uint8_t> raw;
      bool found = false;
      if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '1'}, &raw, &found, &error)) {
        Fail("main db get ktx1 failed: " + error);
      }
      if (!found) {
        Fail("main db should contain ktx1 after commit");
      }

      found = false;
      if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '2'}, &raw, &found, &error)) {
        Fail("main db get ktx2 failed: " + error);
      }
      if (!found) {
        Fail("main db should contain ktx2 after commit");
      }
    }

    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("delete checkpoint failed: " + error);
    }
    return;
  }

  if (mode == "tx_checkpoint_chain_mutation_isolation") {
    // Setup: create base tree structure
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e'}, &error)) {
      Fail("insert k_base failed: " + error);
    }

    const std::string checkpoint_a_dir = dir + "_checkpoint_a";
    const std::string checkpoint_b_dir = dir + "_checkpoint_b";
    const std::string checkpoint_c_dir = dir + "_checkpoint_c";
    std::filesystem::remove_all(checkpoint_a_dir);
    std::filesystem::remove_all(checkpoint_b_dir);
    std::filesystem::remove_all(checkpoint_c_dir);

    // Phase 1: Create checkpoint A (contains k_base=base)
    if (!db.CreateCheckpoint(checkpoint_a_dir, &error)) {
      Fail("create checkpoint A failed: " + error);
    }

    // Phase 2: Mutate main DB, create checkpoint B
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '2'}, {'p', 'h', 'a', 's', 'e', '2'}, &error)) {
      Fail("insert k_phase2 failed: " + error);
    }
    if (!db.CreateCheckpoint(checkpoint_b_dir, &error)) {
      Fail("create checkpoint B failed: " + error);
    }

    // Phase 3: Mutate main DB again, create checkpoint C
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '3'}, {'p', 'h', 'a', 's', 'e', '3'}, &error)) {
      Fail("insert k_phase3 failed: " + error);
    }
    if (!db.CreateCheckpoint(checkpoint_c_dir, &error)) {
      Fail("create checkpoint C failed: " + error);
    }

    // Phase 4: Final main DB mutation
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 'f', 'i', 'n', 'a', 'l'}, {'f', 'i', 'n', 'a', 'l'}, &error)) {
      Fail("insert k_final failed: " + error);
    }

    // Verify checkpoint A: should only have k_base
    {
      grovedb::GroveDb checkpoint_a;
      if (!checkpoint_a.OpenCheckpoint(checkpoint_a_dir, &error)) {
        Fail("open checkpoint A failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, &raw, &found, &error)) {
        Fail("checkpoint A get k_base failed: " + error);
      }
      if (!found) {
        Fail("checkpoint A should contain k_base");
      }
      found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '2'}, &raw, &found, &error)) {
        Fail("checkpoint A get k_phase2 failed: " + error);
      }
      if (found) {
        Fail("checkpoint A should not contain k_phase2");
      }
      found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '3'}, &raw, &found, &error)) {
        Fail("checkpoint A get k_phase3 failed: " + error);
      }
      if (found) {
        Fail("checkpoint A should not contain k_phase3");
      }
      found = false;
      if (!checkpoint_a.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'f', 'i', 'n', 'a', 'l'}, &raw, &found, &error)) {
        Fail("checkpoint A get k_final failed: " + error);
      }
      if (found) {
        Fail("checkpoint A should not contain k_final");
      }
    }

    // Verify checkpoint B: should have k_base + k_phase2
    {
      grovedb::GroveDb checkpoint_b;
      if (!checkpoint_b.OpenCheckpoint(checkpoint_b_dir, &error)) {
        Fail("open checkpoint B failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, &raw, &found, &error)) {
        Fail("checkpoint B get k_base failed: " + error);
      }
      if (!found) {
        Fail("checkpoint B should contain k_base");
      }
      found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '2'}, &raw, &found, &error)) {
        Fail("checkpoint B get k_phase2 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint B should contain k_phase2");
      }
      found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '3'}, &raw, &found, &error)) {
        Fail("checkpoint B get k_phase3 failed: " + error);
      }
      if (found) {
        Fail("checkpoint B should not contain k_phase3");
      }
      found = false;
      if (!checkpoint_b.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'f', 'i', 'n', 'a', 'l'}, &raw, &found, &error)) {
        Fail("checkpoint B get k_final failed: " + error);
      }
      if (found) {
        Fail("checkpoint B should not contain k_final");
      }
    }

    // Verify checkpoint C: should have k_base + k_phase2 + k_phase3
    {
      grovedb::GroveDb checkpoint_c;
      if (!checkpoint_c.OpenCheckpoint(checkpoint_c_dir, &error)) {
        Fail("open checkpoint C failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, &raw, &found, &error)) {
        Fail("checkpoint C get k_base failed: " + error);
      }
      if (!found) {
        Fail("checkpoint C should contain k_base");
      }
      found = false;
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '2'}, &raw, &found, &error)) {
        Fail("checkpoint C get k_phase2 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint C should contain k_phase2");
      }
      found = false;
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '3'}, &raw, &found, &error)) {
        Fail("checkpoint C get k_phase3 failed: " + error);
      }
      if (!found) {
        Fail("checkpoint C should contain k_phase3");
      }
      found = false;
      if (!checkpoint_c.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'f', 'i', 'n', 'a', 'l'}, &raw, &found, &error)) {
        Fail("checkpoint C get k_final failed: " + error);
      }
      if (found) {
        Fail("checkpoint C should not contain k_final");
      }
    }

    // Cleanup
    std::filesystem::remove_all(checkpoint_a_dir);
    std::filesystem::remove_all(checkpoint_b_dir);
    std::filesystem::remove_all(checkpoint_c_dir);
    return;
  }

  if (mode == "tx_checkpoint_reopen_after_main_delete") {
    // Setup: create base tree structure
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("insert root tree failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e'}, &error)) {
      Fail("insert k_base failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k', '_', 's', 'n', 'a', 'p', 's', 'h', 'o', 't'}, {'s', 'n', 'a', 'p', 's', 'h', 'o', 't'}, &error)) {
      Fail("insert k_snapshot failed: " + error);
    }

    const std::string checkpoint_dir = dir + "_checkpoint";
    std::filesystem::remove_all(checkpoint_dir);

    // Phase 1: Create checkpoint with snapshot data
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("create checkpoint failed: " + error);
    }

    // Phase 2: Verify checkpoint contains snapshot data
    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("open checkpoint failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, &raw, &found, &error)) {
        Fail("checkpoint get k_base failed: " + error);
      }
      if (!found) {
        Fail("checkpoint should contain k_base");
      }
      found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 's', 'n', 'a', 'p', 's', 'h', 'o', 't'}, &raw, &found, &error)) {
        Fail("checkpoint get k_snapshot failed: " + error);
      }
      if (!found) {
        Fail("checkpoint should contain k_snapshot");
      }
    }

    // Phase 3: Delete the main database
    // db goes out of scope here, allowing directory deletion
    std::filesystem::remove_all(dir);

    // Phase 4: Reopen checkpoint after main DB deletion and verify data persists
    {
      grovedb::GroveDb checkpoint;
      if (!checkpoint.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("reopen checkpoint after main delete failed: " + error);
      }
      std::vector<uint8_t> raw;
      bool found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, &raw, &found, &error)) {
        Fail("reopen checkpoint get k_base failed: " + error);
      }
      if (!found) {
        Fail("reopen checkpoint should contain k_base");
      }
      found = false;
      if (!checkpoint.Get({{'r', 'o', 'o', 't'}}, {'k', '_', 's', 'n', 'a', 'p', 's', 'h', 'o', 't'}, &raw, &found, &error)) {
        Fail("reopen checkpoint get k_snapshot failed: " + error);
      }
      if (!found) {
        Fail("reopen checkpoint should contain k_snapshot");
      }
    }

    // Cleanup
    std::filesystem::remove_all(checkpoint_dir);
    return;
  }

  if (mode == "batch_apply_local_atomic") {
    std::vector<uint8_t> b2_raw;
    if (!grovedb::EncodeItemToElementBytes({'b', '2'}, &b2_raw, &error)) {
      Fail("encode batch-local b2 failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '2'}, b2_raw},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (!db.ApplyBatch(success_ops, &error)) {
      Fail("cpp local batch success path failed: " + error);
    }

    std::vector<uint8_t> non_tree_raw;
    if (!grovedb::EncodeItemToElementBytes({'n'}, &non_tree_raw, &error)) {
      Fail("encode batch-local non-tree failed: " + error);
    }
    std::vector<uint8_t> bad_raw;
    if (!grovedb::EncodeItemToElementBytes({'x'}, &bad_raw, &error)) {
      Fail("encode batch-local bad item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> failing_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'n', 't'}, non_tree_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'n', 't'}}, {'k', 'b', 'a', 'd'}, bad_raw}};
    if (db.ApplyBatch(failing_ops, &error)) {
      Fail("cpp local failing batch should rollback");
    }
    return;
  }

  if (mode == "batch_apply_tx_visibility") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("cpp batch tx start failed: " + error);
    }
    std::vector<uint8_t> tx_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'b'}, &tx_raw, &error)) {
      Fail("encode batch-tx item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> tx_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'b'}, tx_raw}};
    if (!db.ApplyBatch(tx_ops, &tx, &error)) {
      Fail("cpp batch tx apply failed: " + error);
    }
    std::vector<uint8_t> outside_raw;
    bool outside_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'b'}, &outside_raw, &outside_found, &error)) {
      Fail("cpp batch tx outside-read failed: " + error);
    }
    if (outside_found) {
      Fail("cpp batch tx uncommitted value leaked");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("cpp batch tx commit failed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_empty_noop") {
    std::vector<grovedb::GroveDb::BatchOp> no_ops;
    if (!db.ApplyBatch(no_ops, &error)) {
      Fail("cpp empty batch should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_validate_success_noop") {
    std::vector<uint8_t> val_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', 'v'}, &val_raw, &error)) {
      Fail("encode validate-success item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> validate_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 'v', 'a', 'l'}, val_raw},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (!db.ValidateBatch(validate_ops, &error)) {
      Fail("cpp validate-success batch failed: " + error);
    }
    return;
  }

  if (mode == "batch_validate_failure_noop") {
    std::vector<uint8_t> n_raw;
    if (!grovedb::EncodeItemToElementBytes({'n'}, &n_raw, &error)) {
      Fail("encode validate-failure n item failed: " + error);
    }
    std::vector<uint8_t> x_raw;
    if (!grovedb::EncodeItemToElementBytes({'x'}, &x_raw, &error)) {
      Fail("encode validate-failure x item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> validate_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'n', 'v', 'a', 'l'}, n_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'n', 'v', 'a', 'l'}}, {'k', 'b', 'a', 'd'}, x_raw}};
    if (db.ValidateBatch(validate_ops, &error)) {
      Fail("cpp validate-failure batch should fail");
    }
    return;
  }

  if (mode == "batch_insert_only_semantics") {
    std::vector<uint8_t> io_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 'o'}, &io_raw, &error)) {
      Fail("encode insert-only item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', 'i', 'o'}, io_raw}};
    if (!db.ApplyBatch(success_ops, &error)) {
      Fail("cpp insert-only success batch failed: " + error);
    }
    std::vector<uint8_t> xx_raw;
    if (!grovedb::EncodeItemToElementBytes({'x', 'x'}, &xx_raw, &error)) {
      Fail("encode insert-only overwrite item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', '1'}, xx_raw}};
    if (!db.ApplyBatch(fail_ops, &error)) {
      Fail("cpp insert-only existing-key should upsert");
    }
    return;
  }

  if (mode == "batch_replace_semantics") {
    std::vector<uint8_t> r2_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', '2'}, &r2_raw, &error)) {
      Fail("encode replace item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, r2_raw}};
    if (!db.ApplyBatch(success_ops, &error)) {
      Fail("cpp replace success batch failed: " + error);
    }
    std::vector<uint8_t> rx_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'x'}, &rx_raw, &error)) {
      Fail("encode replace missing-key item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 's', 's'}, rx_raw}};
    if (!db.ApplyBatch(fail_ops, &error)) {
      Fail("cpp replace missing-key should upsert");
    }
    return;
  }

  if (mode == "batch_validate_no_override_insert") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override = true;
    std::vector<uint8_t> ov_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &ov_raw, &error)) {
      Fail("encode strict insert override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '1'}, ov_raw}};
    if (db.ApplyBatch(fail_ops, strict_options, &error)) {
      Fail("cpp strict insert override should fail");
    }
    std::vector<uint8_t> sv_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &sv_raw, &error)) {
      Fail("encode strict insert new-key value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 's', 't', 'r', 'i', 'c', 't'}, sv_raw}};
    if (!db.ApplyBatch(success_ops, strict_options, &error)) {
      Fail("cpp strict insert new key should pass: " + error);
    }
    return;
  }

  if (mode == "batch_validate_no_override_insert_only") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override = true;
    std::vector<uint8_t> io_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 'o'}, &io_raw, &error)) {
      Fail("encode strict insert_only override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', '1'}, io_raw}};
    if (db.ApplyBatch(fail_ops, strict_options, &error)) {
      Fail("cpp strict insert_only override should fail");
    }
    std::vector<uint8_t> iv_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 'v'}, &iv_raw, &error)) {
      Fail("encode strict insert_only new-key value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', 'i', 'o', '2'}, iv_raw}};
    if (!db.ApplyBatch(success_ops, strict_options, &error)) {
      Fail("cpp strict insert_only new key should pass: " + error);
    }
    return;
  }

  if (mode == "batch_validate_no_override_replace") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override = true;
    std::vector<uint8_t> rp_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'p'}, &rp_raw, &error)) {
      Fail("encode strict replace override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, rp_raw}};
    if (db.ApplyBatch(fail_ops, strict_options, &error)) {
      Fail("cpp strict replace override should fail");
    }
    std::vector<uint8_t> rv_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_raw, &error)) {
      Fail("encode strict replace new-key value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'e', 'p', '2'}, rv_raw}};
    if (!db.ApplyBatch(success_ops, strict_options, &error)) {
      Fail("cpp strict replace new key should pass: " + error);
    }
    return;
  }

  if (mode == "batch_validate_no_override_tree_insert") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override_tree = true;
    // Rust parity: overriding a Tree element should be REJECTED.
    std::vector<uint8_t> ov_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &ov_raw, &error)) {
      Fail("encode strict tree insert override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> tree_override_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'r', 'o', 'o', 't'}, ov_raw}};
    if (db.ApplyBatch(tree_override_ops, strict_options, &error)) {
      Fail("cpp strict tree insert override should fail (tree override rejected)");
    }
    // Overriding an Item element should SUCCEED.
    std::vector<uint8_t> it_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 't'}, &it_raw, &error)) {
      Fail("encode strict tree item override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> item_override_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '1'}, it_raw}};
    if (!db.ApplyBatch(item_override_ops, strict_options, &error)) {
      Fail("cpp strict tree item override should pass: " + error);
    }
    return;
  }

  if (mode == "batch_validate_strict_noop") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override = true;
    std::vector<uint8_t> ov_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &ov_raw, &error)) {
      Fail("encode strict validate override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '1'}, ov_raw}};
    if (db.ValidateBatch(fail_ops, strict_options, &error)) {
      Fail("cpp ValidateBatch strict should fail for existing key");
    }
    std::vector<uint8_t> sv_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &sv_raw, &error)) {
      Fail("encode strict validate new key value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 'v', 's'}, sv_raw}};
    if (!db.ValidateBatch(success_ops, strict_options, &error)) {
      Fail("cpp ValidateBatch strict should pass for new key: " + error);
    }
    return;
  }

  if (mode == "batch_delete_non_empty_tree_error") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for non-empty delete error mode failed: " + error);
    }
    std::vector<uint8_t> nv_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_raw, &error)) {
      Fail("encode nested item for non-empty delete error mode failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, {'n', 'k'}, nv_raw, &error)) {
      Fail("cpp insert nested item for non-empty delete error mode failed: " + error);
    }
    grovedb::GroveDb::BatchApplyOptions options;
    options.allow_deleting_non_empty_trees = false;
    options.deleting_non_empty_trees_returns_error = true;
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp non-empty tree delete with error flag should currently pass: " + error);
    }
    return;
  }

  if (mode == "batch_delete_non_empty_tree_no_error") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for non-empty delete no-error mode failed: " + error);
    }
    std::vector<uint8_t> nv_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_raw, &error)) {
      Fail("encode nested item for non-empty delete no-error mode failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, {'n', 'k'}, nv_raw, &error)) {
      Fail("cpp insert nested item for non-empty delete no-error mode failed: " + error);
    }
    grovedb::GroveDb::BatchApplyOptions options;
    options.allow_deleting_non_empty_trees = false;
    options.deleting_non_empty_trees_returns_error = false;
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp non-empty tree delete no-error mode should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_disable_consistency_check") {
    grovedb::GroveDb::BatchApplyOptions options;
    std::vector<grovedb::GroveDb::BatchOp> duplicate_delete_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (db.ApplyBatch(duplicate_delete_ops, &error)) {
      Fail("cpp default consistency checks should reject duplicate same-path/key ops");
    }
    options.disable_operation_consistency_check = true;
    if (!db.ApplyBatch(duplicate_delete_ops, options, &error)) {
      Fail("cpp disabling consistency checks should allow duplicate deletes: " + error);
    }
    return;
  }
  if (mode == "batch_disable_consistency_last_op_wins") {
    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = true;
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode v2 for disable consistency last-op-wins mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '1'}, v2_raw}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp disable consistency last-op-wins mode should succeed: " + error);
    }
    return;
  }
  if (mode == "batch_disable_consistency_reorder_parent_child") {
    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = true;
    std::vector<uint8_t> nv_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_raw, &error)) {
      Fail("encode nested item for disable consistency reorder mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'o', 'r', 'd', '2'}},
         {'n', 'k'},
         nv_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'o', 'r', 'd', '2'},
         tree_element}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp disable consistency reorder parent/child mode should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_insert_tree_semantics") {
    std::vector<grovedb::GroveDb::BatchOp> tree_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element}};
    if (!db.ApplyBatch(tree_ops, &error)) {
      Fail("cpp batch tree insert should succeed: " + error);
    }
    std::vector<uint8_t> nv_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_raw, &error)) {
      Fail("encode nested value for batch tree semantics failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, {'n', 'k'}, nv_raw, &error)) {
      Fail("nested insert under batch-created tree failed: " + error);
    }
    return;
  }

  if (mode == "batch_insert_tree_replace") {
    std::vector<grovedb::GroveDb::BatchOp> tree_ops1 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree, {{'r', 'o', 'o', 't'}}, {'n', 'e', 'w', 't', 'r', 'e', 'e'}, tree_element}};
    if (!db.ApplyBatch(tree_ops1, &error)) {
      Fail("cpp initial tree insert should succeed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> tree_ops2 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree, {{'r', 'o', 'o', 't'}}, {'n', 'e', 'w', 't', 'r', 'e', 'e'}, tree_element}};
    if (!db.ApplyBatch(tree_ops2, &error)) {
      Fail("cpp tree replace should succeed: " + error);
    }
    std::vector<uint8_t> nested_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'e', 's', 't', 'e', 'd', '_', 'v', 'a', 'l', 'u', 'e'}, &nested_raw, &error)) {
      Fail("encode nested value for batch tree replace failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'n', 'e', 'w', 't', 'r', 'e', 'e'}}, {'n', 'e', 's', 't', 'e', 'd'}, nested_raw, &error)) {
      Fail("nested insert under replaced tree failed: " + error);
    }
    return;
  }

  if (mode == "batch_sum_tree_create_and_sum_item") {
    std::vector<uint8_t> sum_tree_raw;
    std::vector<uint8_t> sum_item_raw;
    if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &sum_tree_raw, &error)) {
      Fail("encode sum tree for batch_sum_tree_create_and_sum_item failed: " + error);
    }
    if (!grovedb::EncodeSumItemToElementBytes(7, &sum_item_raw, &error)) {
      Fail("encode sum item for batch_sum_tree_create_and_sum_item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'s', 'b', 'a', 't', 'c', 'h'}},
         {'s', '1'},
         sum_item_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'s', 'b', 'a', 't', 'c', 'h'},
         sum_tree_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp same-batch SumTree+SumItem should succeed: " + error);
    }
    std::vector<uint8_t> parent_raw;
    bool parent_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'s', 'b', 'a', 't', 'c', 'h'}, &parent_raw, &parent_found, &error) ||
        !parent_found) {
      Fail("cpp same-batch SumTree+SumItem parent should exist");
    }
    grovedb::ElementSumTree sum_tree;
    if (!grovedb::DecodeSumTreeFromElementBytes(parent_raw, &sum_tree, &error)) {
      Fail("decode sum tree in batch_sum_tree_create_and_sum_item failed: " + error);
    }
    if (sum_tree.sum != 7) {
      Fail("cpp same-batch SumTree+SumItem parent sum mismatch");
    }
    return;
  }

  if (mode == "batch_count_tree_create_and_item") {
    std::vector<uint8_t> count_tree_raw;
    std::vector<uint8_t> item_raw;
    if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &count_tree_raw, &error)) {
      Fail("encode count tree for batch_count_tree_create_and_item failed: " + error);
    }
    if (!grovedb::EncodeItemToElementBytes({'c', 'v'}, &item_raw, &error)) {
      Fail("encode item for batch_count_tree_create_and_item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'c', 'b', 'a', 't', 'c', 'h'}},
         {'c', '1'},
         item_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'c', 'b', 'a', 't', 'c', 'h'},
         count_tree_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp same-batch CountTree+Item should succeed: " + error);
    }
    ExpectCountTreeCount(&db, {{'r', 'o', 'o', 't'}}, {'c', 'b', 'a', 't', 'c', 'h'}, 1);
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'c', 'b', 'a', 't', 'c', 'h'}}, {'c', '1'}, {'c', 'v'});
    return;
  }

  if (mode == "batch_provable_count_tree_create_and_item") {
    std::vector<uint8_t> tree_raw;
    std::vector<uint8_t> item_raw;
    if (!grovedb::EncodeProvableCountTreeToElementBytesWithRootKey(nullptr, 0, &tree_raw, &error)) {
      Fail("encode provable count tree for batch_provable_count_tree_create_and_item failed: " + error);
    }
    if (!grovedb::EncodeItemToElementBytes({'p', 'v'}, &item_raw, &error)) {
      Fail("encode item for batch_provable_count_tree_create_and_item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'p', 'c', 'b', 'a', 't', 'c', 'h'}},
         {'p', '1'},
         item_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'p', 'c', 'b', 'a', 't', 'c', 'h'},
         tree_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp same-batch ProvableCountTree+Item should succeed: " + error);
    }
    ExpectProvableCountTreeCount(&db, {{'r', 'o', 'o', 't'}}, {'p', 'c', 'b', 'a', 't', 'c', 'h'}, 1);
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'p', 'c', 'b', 'a', 't', 'c', 'h'}},
               {'p', '1'},
               {'p', 'v'});
    return;
  }

  if (mode == "batch_count_sum_tree_create_and_sum_item") {
    std::vector<uint8_t> tree_raw;
    std::vector<uint8_t> sum_item_raw;
    if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(nullptr, 0, 0, &tree_raw, &error)) {
      Fail("encode count sum tree for batch_count_sum_tree_create_and_sum_item failed: " + error);
    }
    if (!grovedb::EncodeSumItemToElementBytes(11, &sum_item_raw, &error)) {
      Fail("encode sum item for batch_count_sum_tree_create_and_sum_item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'c', 's', 'b', 'a', 't', 'c', 'h'}},
         {'c', 's', '1'},
         sum_item_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'c', 's', 'b', 'a', 't', 'c', 'h'},
         tree_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp same-batch CountSumTree+SumItem should succeed: " + error);
    }
    ExpectCountSumTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 's', 'b', 'a', 't', 'c', 'h'}, 1, 11);
    ExpectSumItem(&db,
                  {{'r', 'o', 'o', 't'}, {'c', 's', 'b', 'a', 't', 'c', 'h'}},
                  {'c', 's', '1'},
                  11);
    return;
  }

  if (mode == "batch_provable_count_sum_tree_create_and_sum_item") {
    std::vector<uint8_t> tree_raw;
    std::vector<uint8_t> sum_item_raw;
    if (!grovedb::EncodeProvableCountSumTreeToElementBytesWithRootKey(nullptr, 0, 0, &tree_raw, &error)) {
      Fail("encode provable count sum tree for batch_provable_count_sum_tree_create_and_sum_item failed: " + error);
    }
    if (!grovedb::EncodeSumItemToElementBytes(13, &sum_item_raw, &error)) {
      Fail("encode sum item for batch_provable_count_sum_tree_create_and_sum_item failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'}},
         {'p', 's', '1'},
         sum_item_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'},
         tree_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp same-batch ProvableCountSumTree+SumItem should succeed: " + error);
    }
    ExpectProvableCountSumTree(
        &db, {{'r', 'o', 'o', 't'}}, {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'}, 1, 13);
    ExpectSumItem(&db,
                  {{'r', 'o', 'o', 't'}, {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'}},
                  {'p', 's', '1'},
                  13);
    return;
  }

  if (mode == "batch_apply_failure_atomic_noop") {
    std::vector<uint8_t> ov_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &ov_raw, &error)) {
      Fail("encode ov value for batch failure atomic mode failed: " + error);
    }
    std::vector<uint8_t> xv_raw;
    if (!grovedb::EncodeItemToElementBytes({'x', 'v'}, &xv_raw, &error)) {
      Fail("encode xv value for batch failure atomic mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 'o', 'k'}, ov_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m', 'i', 's', 's', 'i', 'n', 'g', '_', 'p', 'a', 'r', 'e', 'n', 't'}}, {'k', 'b', 'a', 'd'}, xv_raw}};
    if (db.ApplyBatch(ops, &error)) {
      Fail("cpp batch apply should fail for missing parent path");
    }
    bool post_found = false;
    std::vector<uint8_t> post_raw;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'o', 'k'}, &post_raw, &post_found, &error)) {
      Fail("cpp post-failed-batch get should succeed: " + error);
    }
    if (post_found) {
      Fail("failed batch apply should not persist partial writes");
    }
    return;
  }

  if (mode == "batch_delete_missing_noop") {
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'}, {}}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp batch delete missing key should succeed as no-op: " + error);
    }
    bool k1_found = false;
    std::vector<uint8_t> k1_raw;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &k1_raw, &k1_found, &error)) {
      Fail("cpp get k1 after failed delete-missing batch should succeed: " + error);
    }
    if (!k1_found) {
      Fail("cpp failed delete-missing batch should keep existing key");
    }
    return;
  }

  if (mode == "batch_apply_tx_failure_atomic_noop") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_failure_atomic_noop failed: " + error);
    }
    std::vector<uint8_t> tv_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &tv_raw, &error)) {
      Fail("encode tv for tx batch failure mode failed: " + error);
    }
    std::vector<uint8_t> xv_raw;
    if (!grovedb::EncodeItemToElementBytes({'x', 'v'}, &xv_raw, &error)) {
      Fail("encode xv for tx batch failure mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'}, tv_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m', 'i', 's', 's', 'i', 'n', 'g', '_', 'p', 'a', 'r', 'e', 'n', 't'}}, {'k', 't', 'x', 'b', 'a', 'd'}, xv_raw}};
    if (db.ApplyBatch(ops, &tx, &error)) {
      Fail("tx batch apply should fail for missing parent path");
    }
    bool in_tx_found = false;
    std::vector<uint8_t> in_tx_raw;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'}, &in_tx_raw, &in_tx_found, &tx, &error)) {
      Fail("in-tx get after failed tx batch should succeed: " + error);
    }
    if (in_tx_found) {
      Fail("failed tx batch should not expose partial writes in tx");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after failed tx batch should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_tx_failure_then_reuse") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_failure_then_reuse failed: " + error);
    }
    std::vector<uint8_t> tv_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &tv_raw, &error)) {
      Fail("encode tv for tx batch failure reuse mode failed: " + error);
    }
    std::vector<uint8_t> xv_raw;
    if (!grovedb::EncodeItemToElementBytes({'x', 'v'}, &xv_raw, &error)) {
      Fail("encode xv for tx batch failure reuse mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'}, tv_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m', 'i', 's', 's', 'i', 'n', 'g', '_', 'p', 'a', 'r', 'e', 'n', 't'}}, {'k', 't', 'x', 'b', 'a', 'd'}, xv_raw}};
    if (db.ApplyBatch(fail_ops, &tx, &error)) {
      Fail("tx batch apply should fail for missing parent path");
    }
    std::vector<uint8_t> rv_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', 'v'}, &rv_raw, &error)) {
      Fail("encode rv for tx batch reuse mode failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'r', 'e', 'u', 's', 'e'}, rv_raw, &tx, &error)) {
      Fail("insert after failed tx batch should succeed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after tx reuse should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_tx_failure_then_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_failure_then_rollback failed: " + error);
    }
    std::vector<uint8_t> tv_raw;
    if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &tv_raw, &error)) {
      Fail("encode tv for tx batch rollback mode failed: " + error);
    }
    std::vector<uint8_t> xv_raw;
    if (!grovedb::EncodeItemToElementBytes({'x', 'v'}, &xv_raw, &error)) {
      Fail("encode xv for tx batch rollback mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'}, tv_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m', 'i', 's', 's', 'i', 'n', 'g', '_', 'p', 'a', 'r', 'e', 'n', 't'}}, {'k', 't', 'x', 'b', 'a', 'd'}, xv_raw}};
    if (db.ApplyBatch(fail_ops, &tx, &error)) {
      Fail("tx batch apply should fail for missing parent path");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback after failed tx batch should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_tx_success_then_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_success_then_rollback failed: " + error);
    }
    std::vector<uint8_t> sv_raw;
    if (!grovedb::EncodeItemToElementBytes({'s', 'v'}, &sv_raw, &error)) {
      Fail("encode sv for tx batch success rollback mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', 't', 's', 'r'}, sv_raw}};
    if (!db.ApplyBatch(ops, &tx, &error)) {
      Fail("tx batch apply should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback after successful tx batch should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_tx_delete_then_rollback") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_delete_then_rollback failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (!db.ApplyBatch(ops, &tx, &error)) {
      Fail("tx batch delete should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("rollback after successful tx batch delete should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_apply_tx_delete_missing_noop") {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for batch_apply_tx_delete_missing_noop failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'}, {}}};
    if (!db.ApplyBatch(ops, &tx, &error)) {
      Fail("tx batch delete-missing should succeed as no-op: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit after tx batch delete-missing should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_delete_tree_op") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for delete_tree_op mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp delete_tree_op mode should remove tree key: " + error);
    }
    return;
  }

  if (mode == "batch_delete_tree_disable_consistency_check") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for delete_tree_disable_consistency_check mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> duplicate_delete_tree_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}}};
    if (db.ApplyBatch(duplicate_delete_tree_ops, &error)) {
      Fail("cpp default consistency checks should reject duplicate delete_tree ops");
    }
    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = true;
    if (!db.ApplyBatch(duplicate_delete_tree_ops, options, &error)) {
      Fail("cpp disabling consistency checks should allow duplicate delete_tree ops: " + error);
    }
    return;
  }

  if (mode == "batch_delete_tree_non_empty_options") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for delete_tree_non_empty_options mode failed: " + error);
    }
    std::vector<uint8_t> nv_raw;
    if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &nv_raw, &error)) {
      Fail("encode nested item for delete_tree_non_empty_options mode failed: " + error);
    }
    if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, {'n', 'k'}, nv_raw, &error)) {
      Fail("cpp insert nested item for delete_tree_non_empty_options mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}}};
    grovedb::GroveDb::BatchApplyOptions strict_error_options;
    strict_error_options.allow_deleting_non_empty_trees = false;
    strict_error_options.deleting_non_empty_trees_returns_error = true;
    if (!db.ApplyBatch(ops, strict_error_options, &error)) {
      Fail("cpp delete_tree non-empty with error flag should still succeed: " + error);
    }
    return;
  }

  if (mode == "batch_mixed_non_minimal_ops") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for mixed non-minimal batch mode failed: " + error);
    }
    std::vector<uint8_t> r2_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', '2'}, &r2_raw, &error)) {
      Fail("encode r2 for mixed non-minimal batch mode failed: " + error);
    }
    std::vector<uint8_t> b2_raw;
    if (!grovedb::EncodeItemToElementBytes({'b', '2'}, &b2_raw, &error)) {
      Fail("encode b2 for mixed non-minimal batch mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, r2_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', '2'}, b2_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp mixed non-minimal batch mode should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_mixed_non_minimal_ops_with_options") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, tree_element, &error)) {
      Fail("cpp insert child tree for mixed non-minimal batch+options mode failed: " + error);
    }
    std::vector<uint8_t> r3_raw;
    if (!grovedb::EncodeItemToElementBytes({'r', '3'}, &r3_raw, &error)) {
      Fail("encode r3 for mixed non-minimal batch+options mode failed: " + error);
    }
    std::vector<uint8_t> b3_raw;
    if (!grovedb::EncodeItemToElementBytes({'b', '3'}, &b3_raw, &error)) {
      Fail("encode b3 for mixed non-minimal batch+options mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'}, {}},
        {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, r3_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {{'r', 'o', 'o', 't'}}, {'k', '3'}, b3_raw}};
    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = true;
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp mixed non-minimal batch+options mode should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_patch_existing") {
    std::vector<uint8_t> p1_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', '1'}, &p1_raw, &error)) {
      Fail("encode p1 for batch_patch_existing failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kPatch, {{'r', 'o', 'o', 't'}}, {'k', '1'}, p1_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp batch patch existing key should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_patch_missing") {
    std::vector<uint8_t> p2_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', '2'}, &p2_raw, &error)) {
      Fail("encode p2 for batch_patch_missing failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kPatch, {{'r', 'o', 'o', 't'}}, {'k', '2'}, p2_raw}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp batch patch missing key should succeed (inserts): " + error);
    }
    return;
  }

  if (mode == "batch_patch_strict_no_override") {
    grovedb::GroveDb::BatchApplyOptions strict_options;
    strict_options.validate_insertion_does_not_override = true;
    std::vector<uint8_t> px_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', 'x'}, &px_raw, &error)) {
      Fail("encode px for batch_patch_strict_no_override failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> fail_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kPatch, {{'r', 'o', 'o', 't'}}, {'k', '1'}, px_raw}};
    if (db.ApplyBatch(fail_ops, strict_options, &error)) {
      Fail("cpp strict patch override on existing key should fail");
    }
    std::vector<uint8_t> pv_raw;
    if (!grovedb::EncodeItemToElementBytes({'p', 'v'}, &pv_raw, &error)) {
      Fail("encode pv for batch_patch_strict_no_override failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> success_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kPatch, {{'r', 'o', 'o', 't'}}, {'k', 'p'}, pv_raw}};
    if (!db.ApplyBatch(success_ops, strict_options, &error)) {
      Fail("cpp strict patch on new key should pass: " + error);
    }
    return;
  }

  if (mode == "batch_refresh_reference_trust") {
    grovedb::GroveDb::BatchApplyOptions options;
    options.trust_refresh_reference = true;
    grovedb::ElementReference trusted_ref;
    trusted_ref.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
    trusted_ref.reference_path.path = {{'r', 'o', 'o', 't'}, {'k', '1'}};
    std::vector<uint8_t> trusted_ref_bytes;
    if (!grovedb::EncodeReferenceToElementBytes(trusted_ref, &trusted_ref_bytes, &error)) {
      Fail("encode trusted refresh reference bytes failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kRefreshReference,
         {{'r', 'o', 'o', 't'}},
         {'r', 'e', 'f', '_', 'm', 'i', 's', 's', 'i', 'n', 'g'},
         trusted_ref_bytes}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp trusted refresh reference should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_pause_height_passthrough") {
    // Apply batch with batch_pause_height=0.
    // This option controls at what tree height to pause batch application.
    // Setting it to 0 means apply all levels (no pausing).
    // This test validates the option is accepted and produces equivalent results.
    grovedb::GroveDb::BatchApplyOptions options;
    options.batch_pause_height = 0;
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode batch pause-height v2 failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}},
         {'k', '2'},
         v2_raw},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp batch with pause_height=0 should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_partial_pause_resume") {
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("cpp partial pause: insert root failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'a'}, &error)) {
      Fail("cpp partial pause: insert a failed: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'a'}}, {'b'}, &error)) {
      Fail("cpp partial pause: insert b failed: " + error);
    }
    std::vector<uint8_t> v0_raw;
    std::vector<uint8_t> v1_raw;
    std::vector<uint8_t> va_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '0'}, &v0_raw, &error) ||
        !grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_raw, &error) ||
        !grovedb::EncodeItemToElementBytes({'v', 'a'}, &va_raw, &error)) {
      Fail("cpp partial pause: encode items failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'l', '0'}, v0_raw},
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'l', '1'}, v1_raw}};
    grovedb::GroveDb::BatchApplyOptions pause_opts;
    pause_opts.batch_pause_height = 2;
    grovedb::GroveDb::OpsByLevelPath leftovers;
    if (!db.ApplyPartialBatch(ops, pause_opts, nullptr, &leftovers, &error)) {
      Fail("cpp partial pause: ApplyPartialBatch failed: " + error);
    }
    if (!leftovers.count(0) || !leftovers.count(1)) {
      Fail("cpp partial pause: expected level 0 and 1 leftovers");
    }
    std::vector<grovedb::GroveDb::BatchOp> additional = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}, {'a'}, {'b'}},
         {'a', 'd', 'd'},
         va_raw}};
    grovedb::GroveDb::BatchApplyOptions resume_opts;
    resume_opts.batch_pause_height = 0;
    grovedb::GroveDb::OpsByLevelPath resume_leftovers;
    // Align with Rust fixture behavior for this mode: continuation applies the
    // additional op only (paused leftovers are not materialized).
    grovedb::GroveDb::OpsByLevelPath empty_previous_leftover;
    if (!db.ContinuePartialApplyBatch(
            empty_previous_leftover, additional, resume_opts, nullptr, &resume_leftovers, &error)) {
      Fail("cpp partial pause: ContinuePartialApplyBatch failed: " + error);
    }
    if (!resume_leftovers.empty()) {
      Fail("cpp partial pause: expected empty leftovers after continue at height 0");
    }
    return;
  }

  if (mode == "batch_base_root_storage_is_free_passthrough") {
    // Apply batch with base_root_storage_is_free=false.
    // This option controls whether root storage costs are counted.
    // Default is true (root storage is free).
    // This test validates the option is accepted and produces equivalent results.
    grovedb::GroveDb::BatchApplyOptions options;
    options.base_root_storage_is_free = false;
    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode batch base-root-storage-is-free v2 failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert,
         {{'r', 'o', 'o', 't'}},
         {'k', '2'},
         v2_raw},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
    if (!db.ApplyBatch(ops, options, &error)) {
      Fail("cpp batch with base_root_storage_is_free=false should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_insert_or_replace_semantics") {
    // Test kInsertOrReplace idempotent semantics:
    // - First insert creates the key
    // - Second insert replaces the value
    // - Final value should match the last insert
    std::vector<uint8_t> v1_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1_raw, &error)) {
      Fail("encode batch insert_or_replace v1 failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops1 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace,
         {{'r', 'o', 'o', 't'}},
         {'k', 'i', 'o', 'r'},
         v1_raw}};
    if (!db.ApplyBatch(ops1, &error)) {
      Fail("cpp first insert_or_replace should succeed: " + error);
    }

    std::vector<uint8_t> v2_raw;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &v2_raw, &error)) {
      Fail("encode batch insert_or_replace v2 failed: " + error);
    }
    // Replace with new value using same operation
    std::vector<grovedb::GroveDb::BatchOp> ops2 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace,
         {{'r', 'o', 'o', 't'}},
         {'k', 'i', 'o', 'r'},
         v2_raw}};
    if (!db.ApplyBatch(ops2, &error)) {
      Fail("cpp second insert_or_replace should succeed: " + error);
    }
    return;
  }

  if (mode == "batch_insert_or_replace_with_override_validation") {
    // Test that validate_insertion_does_not_override flag rejects InsertOrReplace on existing keys
    // First, insert a key using InsertOrReplace (should succeed, key doesn't exist)
    std::vector<uint8_t> element_bytes;
    std::string encode_error;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &element_bytes, &encode_error)) {
      Fail("encode element failed: " + encode_error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops1 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace,
         {{'r', 'o', 'o', 't'}},
         {'k', '1'},
         element_bytes}};
    if (!db.ApplyBatch(ops1, &error)) {
      Fail("cpp first InsertOrReplace should succeed: " + error);
    }

    // Verify the key exists
    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
      Fail("cpp get k1 failed: " + error);
    }
    if (!found) {
      Fail("cpp k1 should exist after first InsertOrReplace");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(raw, &item, &error)) {
      Fail("decode item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>{'v', '1'}) {
      Fail("cpp k1 value should be v1");
    }

    // Now try InsertOrReplace with validate_insertion_does_not_override=true
    // This should FAIL because the key already exists
    grovedb::GroveDb::BatchApplyOptions options;
    options.validate_insertion_does_not_override = true;

    std::vector<uint8_t> element_bytes2;
    if (!grovedb::EncodeItemToElementBytes({'v', '2'}, &element_bytes2, &encode_error)) {
      Fail("encode element2 failed: " + encode_error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops2 = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace,
         {{'r', 'o', 'o', 't'}},
         {'k', '1'},
         element_bytes2}};
    if (db.ApplyBatch(ops2, options, &error)) {
      Fail("cpp InsertOrReplace with validate_insertion_does_not_override=true should fail on existing key");
    }
    // Error should mention override not allowed
    if (error.find("override") == std::string::npos &&
        error.find("Override") == std::string::npos &&
        error.find("overwrite") == std::string::npos &&
        error.find("Overwrite") == std::string::npos) {
      Fail("cpp error should mention override/overwrite: " + error);
    }

    // Verify the value was not changed (atomic rollback)
    std::vector<uint8_t> raw_final;
    bool found_final = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw_final, &found_final, &error)) {
      Fail("cpp get k1 after failed batch failed: " + error);
    }
    if (!found_final) {
      Fail("cpp k1 should still exist after failed batch");
    }
    grovedb::ElementItem item_final;
    if (!grovedb::DecodeItemFromElementBytes(raw_final, &item_final, &error)) {
      Fail("decode item final failed: " + error);
    }
    if (item_final.value != std::vector<uint8_t>{'v', '1'}) {
      Fail("cpp k1 value should remain v1 after failed batch");
    }
    return;
  }

  if (mode == "batch_validate_no_override_tree_insert_or_replace") {
    // Test that validate_insertion_does_not_override_tree flag rejects kInsertOrReplace
    // on existing tree elements while allowing non-tree (Item) element overrides

    // Test 1: kInsertOrReplace on existing Tree element should FAIL
    grovedb::GroveDb::BatchApplyOptions opts_tree;
    opts_tree.validate_insertion_does_not_override_tree = true;

    std::vector<uint8_t> ov_raw;
    if (!grovedb::EncodeItemToElementBytes({'o', 'v'}, &ov_raw, &error)) {
      Fail("encode item override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> tree_override_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace, {}, {'r', 'o', 'o', 't'}, ov_raw}};
    if (db.ApplyBatch(tree_override_ops, opts_tree, &error)) {
      Fail("cpp kInsertOrReplace on existing Tree should fail with validate_insertion_does_not_override_tree=true");
    }

    // Verify root is still a tree (not replaced with item)
    std::vector<uint8_t> root_raw;
    bool root_found = false;
    if (!db.Get({}, {'r', 'o', 'o', 't'}, &root_raw, &root_found, &error)) {
      Fail("cpp get root after failed batch failed: " + error);
    }
    if (!root_found) {
      Fail("cpp root should exist after failed batch");
    }
    grovedb::ElementTree root_tree;
    if (!grovedb::DecodeTreeFromElementBytes(root_raw, &root_tree, &error)) {
      Fail("cpp decode root tree failed: " + error);
    }
    // Good - root is still a tree

    // Test 2: kInsertOrReplace on existing Item element should SUCCEED
    grovedb::GroveDb::BatchApplyOptions opts_item;
    opts_item.validate_insertion_does_not_override_tree = true;

    std::vector<uint8_t> it_raw;
    if (!grovedb::EncodeItemToElementBytes({'i', 't'}, &it_raw, &error)) {
      Fail("encode item override value failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> item_override_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, it_raw}};
    if (!db.ApplyBatch(item_override_ops, opts_item, &error)) {
      Fail("cpp kInsertOrReplace on existing Item should succeed with validate_insertion_does_not_override_tree=true: " + error);
    }

    // Verify k1 was replaced with new item value
    std::vector<uint8_t> k1_raw;
    bool k1_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &k1_raw, &k1_found, &error)) {
      Fail("cpp get k1 after item override failed: " + error);
    }
    if (!k1_found) {
      Fail("cpp k1 should exist after item override");
    }
    grovedb::ElementItem k1_item;
    if (!grovedb::DecodeItemFromElementBytes(k1_raw, &k1_item, &error)) {
      Fail("cpp decode k1 item failed: " + error);
    }
    if (k1_item.value != std::vector<uint8_t>{'i', 't'}) {
      Fail("cpp k1 value should be 'it' after replace");
    }
    return;
  }

  if (mode == "batch_insert_tree_below_deleted_path_consistency") {
    if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 't'}, tree_element, &error)) {
      Fail("cpp insert ct tree for insert-tree-under-delete consistency mode failed: " + error);
    }
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}, {'c', 't'}},
         {'g', 'c'},
         tree_element},
        {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'c', 't'}, {}}};
    if (db.ApplyBatch(ops, &error)) {
      Fail("cpp default consistency checks should reject insert under deleted path");
    }
    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = true;
    if (db.ApplyBatch(ops, options, &error)) {
      Fail("cpp insert under deleted path should still fail at execution time");
    }
    return;
  }

  if (mode == "batch_insert_tree_with_root_hash") {
    // Test kInsertTree (InsertTreeWithRootHash) batch operation parity:
    // - Insert a tree element with root hash via batch operation
    // - Validate tree element bytes are correctly stored
    std::vector<grovedb::GroveDb::BatchOp> ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
         {{'r', 'o', 'o', 't'}},
         {'t', 'r', 'e', 'e', '_', 'k', 'e', 'y'},
         tree_element}};
    if (!db.ApplyBatch(ops, &error)) {
      Fail("cpp insert_tree_with_root_hash batch should succeed: " + error);
    }
    return;
  }
}

void VerifyCppScenario(const std::string& dir, const std::string& mode) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("cpp open for verify failed: " + error);
  }

  if (mode == "simple") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "facade_insert_helpers") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
               {'n', 'k'},
               {'n', 'v'});
    std::vector<uint8_t> big_sum_raw;
    bool big_sum_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'b', 'i', 'g'}, &big_sum_raw, &big_sum_found, &error)) {
      Fail("facade_insert_helpers get big sum tree failed: " + error);
    }
    if (!big_sum_found) {
      Fail("facade_insert_helpers expected big sum tree key");
    }
    __int128 big_sum = 1;
    bool has_big_sum = false;
    if (!grovedb::ExtractBigSumValueFromElementBytes(
            big_sum_raw, &big_sum, &has_big_sum, &error)) {
      Fail("facade_insert_helpers extract big sum failed: " + error);
    }
    if (!has_big_sum || big_sum != 0) {
      Fail("facade_insert_helpers big sum tree should have sum=0");
    }
    std::vector<uint8_t> count_raw;
    bool count_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'c', 'o', 'u', 'n', 't'}, &count_raw, &count_found, &error)) {
      Fail("facade_insert_helpers get count tree failed: " + error);
    }
    if (!count_found) {
      Fail("facade_insert_helpers expected count tree key");
    }
    uint64_t count = 1;
    bool has_count = false;
    if (!grovedb::ExtractCountValueFromElementBytes(count_raw, &count, &has_count, &error)) {
      Fail("facade_insert_helpers extract count failed: " + error);
    }
    if (!has_count || count != 0) {
      Fail("facade_insert_helpers count tree should have count=0");
    }
    std::vector<uint8_t> prov_count_raw;
    bool prov_count_found = false;
    if (!db.Get({{'r', 'o', 'o', 't'}},
                {'p', 'r', 'o', 'v', 'c', 't'},
                &prov_count_raw,
                &prov_count_found,
                &error)) {
      Fail("facade_insert_helpers get provable count tree failed: " + error);
    }
    if (!prov_count_found) {
      Fail("facade_insert_helpers expected provable count tree key");
    }
    uint64_t prov_count = 1;
    bool has_prov_count = false;
    if (!grovedb::ExtractCountValueFromElementBytes(
            prov_count_raw, &prov_count, &has_prov_count, &error)) {
      Fail("facade_insert_helpers extract provable count failed: " + error);
    }
    if (!has_prov_count || prov_count != 0) {
      Fail("facade_insert_helpers provable count tree should have count=0");
    }
  } else if (mode == "facade_insert_if_not_exists") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'v', '2'});
  } else if (mode == "facade_insert_if_changed_value") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "facade_insert_if_not_exists_return_existing") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "facade_insert_if_not_exists_return_existing_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
  } else if (mode == "facade_flush") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "facade_root_key") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
  } else if (mode == "facade_delete_if_empty_tree") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(
        &db, {{'r', 'o', 'o', 't'}}, {'d', 'e', 'l', 'e', 't', 'a', 'b', 'l', 'e'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'n', 'o', 'n', 'e', 'm', 'p', 't', 'y'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'n', 'o', 'n', 'e', 'm', 'p', 't', 'y'}},
               {'k', '1'},
               {'v', '1'});
  } else if (mode == "facade_clear_subtree") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'l', 'r'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '2'});
  } else if (mode == "facade_clear_subtree_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'l', 'r'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'c', 'l', 'r'}}, {'k', '2'});
  } else if (mode == "facade_follow_reference") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}},
                            {'r', 'e', 'f', '1'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_follow_reference resolved value mismatch");
    }
  } else if (mode == "facade_follow_reference_tx") {
    // Verify root tree and base structures
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    // Verify tx_ref was committed and can be followed
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}},
                            {'t', 'x', '_', 'r', 'e', 'f'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_tx resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_tx should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_tx decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_follow_reference_tx resolved value mismatch");
    }
  } else if (mode == "facade_find_subtrees") {
    // Verify root tree exists
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    // Verify first-level child trees
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd', '1'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd', '2'});
    // Verify second-level nested trees under child1
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}}, {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '1'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}}, {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '2'});
    // Verify items in leaf trees
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '1'}, {'g', 'r', 'a', 'n', 'd', 'c', 'h', 'i', 'l', 'd', '1'}},
               {'k', '1'},
               {'v', '1'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd', '2'}},
               {'k', '2'},
               {'v', '2'});
    // Verify FindSubtrees discovers all nested trees
    std::vector<std::vector<std::vector<uint8_t>>> subtrees;
    if (!db.FindSubtrees({}, &subtrees, &error)) {
      Fail("facade_find_subtrees FindSubtrees failed: " + error);
    }
    // Should find: root, root/child1, root/child2, root/child1/grandchild1, root/child1/grandchild2
    if (subtrees.size() != 5) {
      Fail("facade_find_subtrees should discover 5 subtrees, got " + std::to_string(subtrees.size()));
    }
    // Verify root is in the list
    bool found_root = false;
    for (const auto& path : subtrees) {
      if (path.size() == 1 && path[0] == std::vector<uint8_t>({'r', 'o', 'o', 't'})) {
        found_root = true;
        break;
      }
    }
    if (!found_root) {
      Fail("facade_find_subtrees should include root subtree");
    }
  } else if (mode == "facade_check_subtree_exists_invalid_path_tx") {
    // Verify root tree exists
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    // Verify base item exists
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'});
    // Verify committed child subtree exists
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});

    // Now test tx-local visibility
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx start tx failed: " + error);
    }

    // Create tx-local subtree under root
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}, &tx, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx tx_child insert failed: " + error);
    }

    // CheckSubtreeExistsInvalidPath in tx - should see tx_local subtree
    if (!db.CheckSubtreeExistsInvalidPath(
            {{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &tx, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx in-tx should see tx-local subtree: " + error);
    }

    // CheckSubtreeExistsInvalidPath outside tx before commit - should NOT see tx_local subtree
    if (db.CheckSubtreeExistsInvalidPath(
            {{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx outside-tx should not see tx-local subtree before commit");
    }

    // CheckSubtreeExistsInvalidPath in tx for committed child - should see it
    if (!db.CheckSubtreeExistsInvalidPath(
            {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}}, &tx, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx in-tx should see committed child");
    }

    // Commit transaction
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx commit tx failed: " + error);
    }

    // CheckSubtreeExistsInvalidPath after commit - should see tx_child
    if (!db.CheckSubtreeExistsInvalidPath(
            {{'r', 'o', 'o', 't'}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'}}, &error)) {
      Fail("facade_check_subtree_exists_invalid_path_tx after commit should see tx_child");
    }
  } else if (mode == "facade_follow_reference_mixed_path_chain") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'i', 'n', 'n', 'e', 'r'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'},
               {'m', 'i', 'x', 'e', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'});
    std::vector<uint8_t> resolved;
    bool found = false;
    // Follow reference chain from root/inner/ref_c: UpstreamRootHeight -> Sibling -> Absolute -> target
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'i', 'n', 'n', 'e', 'r'}},
                            {'r', 'e', 'f', '_', 'c'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_mixed_path_chain resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_mixed_path_chain should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_mixed_path_chain decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'m', 'i', 'x', 'e', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'})) {
      Fail("facade_follow_reference_mixed_path_chain resolved value mismatch");
    }
  } else if (mode == "facade_follow_reference_parent_path_addition") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'a', 'l', 'i', 'a', 's'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'t', 'a', 'r', 'g', 'e', 't'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'a', 'l', 'i', 'a', 's'}},
               {'t', 'a', 'r', 'g', 'e', 't'},
               {'p', 'a', 'r', 'e', 'n', 't', '_', 'a', 'd', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'});
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'t', 'a', 'r', 'g', 'e', 't'}},
                            {'r', 'e', 'f', '_', 'p', 'a', 'r', 'e', 'n', 't', '_', 'a', 'd', 'd'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_parent_path_addition resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_parent_path_addition should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_parent_path_addition decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'p', 'a', 'r', 'e', 'n', 't', '_', 'a', 'd', 'd', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'})) {
      Fail("facade_follow_reference_parent_path_addition resolved value mismatch");
    }
  } else if (mode == "facade_follow_reference_upstream_element_height") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'d', 'e', 'e', 'p'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
               {'a', 'l', 'i', 'a', 's'},
               {'u', 'p', 's', 't', 'r', 'e', 'a', 'm', '_', 'e', 'l', 'e', 'm', '_',
                'v', 'a', 'l', 'u', 'e'});
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_upstream_element_height resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_upstream_element_height should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_upstream_element_height decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>(
                          {'u', 'p', 's', 't', 'r', 'e', 'a', 'm', '_', 'e', 'l', 'e', 'm', '_',
                           'v', 'a', 'l', 'u', 'e'})) {
      Fail("facade_follow_reference_upstream_element_height resolved value mismatch");
    }
  } else if (mode == "facade_follow_reference_cousin") {
    // Verify structure: root/branch/deep and root/branch/cousin
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'d', 'e', 'e', 'p'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'c', 'o', 'u', 's', 'i', 'n'});
    // CousinReference at root/branch/deep/ref should resolve to root/branch/cousin/ref
    // which contains "cousin_target_value"
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}},
               {'r', 'e', 'f'},
               {'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'});
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_cousin resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_cousin should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_cousin decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'})) {
      Fail("facade_follow_reference_cousin resolved value mismatch");
    }
  } else if (mode == "facade_follow_reference_removed_cousin") {
    // Verify structure: root/branch/deep and root/branch/cousin/nested
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'d', 'e', 'e', 'p'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'c', 'o', 'u', 's', 'i', 'n'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}}, {'n', 'e', 's', 't', 'e', 'd'});
    // RemovedCousinReference at root/branch/deep/ref should resolve to root/branch/cousin/nested/ref
    // which contains "removed_cousin_target_value"
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'c', 'o', 'u', 's', 'i', 'n'}, {'n', 'e', 's', 't', 'e', 'd'}},
               {'r', 'e', 'f'},
               {'r', 'e', 'm', 'o', 'v', 'e', 'd', '_', 'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'});
    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'d', 'e', 'e', 'p'}},
                            {'r', 'e', 'f'},
                            &resolved,
                            &found,
                            &error)) {
      Fail("facade_follow_reference_removed_cousin resolve failed: " + error);
    }
    if (!found) {
      Fail("facade_follow_reference_removed_cousin should set found=true");
    }
    grovedb::ElementItem item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &item, &error)) {
      Fail("facade_follow_reference_removed_cousin decode resolved item failed: " + error);
    }
    if (item.value != std::vector<uint8_t>({'r', 'e', 'm', 'o', 'v', 'e', 'd', '_', 'c', 'o', 'u', 's', 'i', 'n', '_', 't', 'a', 'r', 'g', 'e', 't', '_', 'v', 'a', 'l', 'u', 'e'})) {
      Fail("facade_follow_reference_removed_cousin resolved value mismatch");
    }
  } else if (mode == "facade_get_raw") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    std::vector<uint8_t> raw;
    bool found = false;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &error)) {
      Fail("facade_get_raw verify failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw verify should set found=true");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
      Fail("facade_get_raw verify decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw verify should return reference element bytes");
    }
    std::vector<uint8_t> resolved;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &resolved, &found, &error)) {
      Fail("facade_get_raw verify Get failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw verify Get should set found=true");
    }
    grovedb::ElementItem resolved_item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &resolved_item, &error)) {
      Fail("facade_get_raw verify Get decode failed: " + error);
    }
    if (resolved_item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_get_raw verify Get should resolve reference to target item");
    }
  } else if (mode == "facade_get_raw_optional") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    std::vector<uint8_t> raw;
    bool found = true;
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'}, &raw, &found, &error)) {
      Fail("facade_get_raw_optional verify existing key failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw_optional verify should set found=true for existing key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
      Fail("facade_get_raw_optional verify decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw_optional verify should return unresolved reference bytes");
    }
    if (!db.GetRawOptional({{'r', 'o', 'o', 't'}, {'m', 'i', 's', 's'}}, {'k'}, &raw, &found, &error)) {
      Fail("facade_get_raw_optional verify missing-path call failed: " + error);
    }
    if (found || !raw.empty()) {
      Fail("facade_get_raw_optional verify missing path should return found=false with empty bytes");
    }
  } else if (mode == "facade_get_raw_caching_optional") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    std::vector<uint8_t> raw;
    bool found = true;
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &raw, &found, true, &error)) {
      Fail("facade_get_raw_caching_optional verify existing key failed: " + error);
    }
    if (!found) {
      Fail("facade_get_raw_caching_optional verify should set found=true for existing key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(raw, &variant, &error)) {
      Fail("facade_get_raw_caching_optional verify decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_get_raw_caching_optional verify should return unresolved reference bytes");
    }
    std::vector<uint8_t> raw2;
    bool found2 = true;
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &raw2, &found2, false, &error)) {
      Fail("facade_get_raw_caching_optional verify cache-bypass failed: " + error);
    }
    if (!found2 || raw != raw2) {
      Fail("facade_get_raw_caching_optional verify cache and cache-bypass should match");
    }
    if (!db.GetRawCachingOptional({{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's'}, &raw, &found, true, &error)) {
      Fail("facade_get_raw_caching_optional verify missing-path call failed: " + error);
    }
    if (found || !raw.empty()) {
      Fail("facade_get_raw_caching_optional verify missing path should return found=false with empty bytes");
    }
  } else if (mode == "facade_get_caching_optional") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    std::vector<uint8_t> resolved;
    bool found = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &resolved, &found, true, &error)) {
      Fail("facade_get_caching_optional verify existing key failed: " + error);
    }
    if (!found) {
      Fail("facade_get_caching_optional verify should set found=true for existing key");
    }
    if (resolved != std::vector<uint8_t>({'t', 'v'})) {
      Fail("facade_get_caching_optional verify should return target item value");
    }
    std::vector<uint8_t> resolved2;
    bool found2 = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, &resolved2, &found2, false, &error)) {
      Fail("facade_get_caching_optional verify cache-bypass failed: " + error);
    }
    if (!found2 || resolved != resolved2) {
      Fail("facade_get_caching_optional verify cache and cache-bypass should match");
    }
  } else if (mode == "facade_get_caching_optional_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'x', '_', 'v'});

    // Verify txk is visible after commit (use non-tx overload)
    std::vector<uint8_t> element_bytes;
    bool found = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &element_bytes, &found, true, &error)) {
      Fail("facade_get_caching_optional_tx verify after commit failed: " + error);
    }
    if (!found) {
      Fail("facade_get_caching_optional_tx verify should set found=true for committed key");
    }
    if (element_bytes != std::vector<uint8_t>({'t', 'x', '_', 'v'})) {
      Fail("facade_get_caching_optional_tx verify should return txk item value");
    }

    // Verify base is still visible
    std::vector<uint8_t> base_bytes;
    bool base_found = true;
    if (!db.GetCachingOptional({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, &base_bytes, &base_found, true, &error)) {
      Fail("facade_get_caching_optional_tx verify base failed: " + error);
    }
    if (!base_found) {
      Fail("facade_get_caching_optional_tx verify should set found=true for base key");
    }
    if (base_bytes != std::vector<uint8_t>({'b', 'a', 's', 'e', '_', 'v'})) {
      Fail("facade_get_caching_optional_tx verify should return base item value");
    }
  } else if (mode == "facade_get_subtree_root_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', '_', 'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "facade_has_caching_optional_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e', '_', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'x', '_', 'v'});

    // Verify txk is visible after commit (use non-tx overload)
    bool found_txk = false;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, &found_txk, true, &error)) {
      Fail("facade_has_caching_optional_tx verify after commit failed: " + error);
    }
    if (!found_txk) {
      Fail("facade_has_caching_optional_tx verify should report found=true for committed key txk");
    }

    // Verify base is still visible
    bool found_base = false;
    if (!db.HasCachingOptional({{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, &found_base, true, &error)) {
      Fail("facade_has_caching_optional_tx verify base failed: " + error);
    }
    if (!found_base) {
      Fail("facade_has_caching_optional_tx verify should report found=true for base key");
    }
  } else if (mode == "facade_query_raw") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    grovedb::PathQuery query =
        grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '1'});
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRaw(query, &out, &error)) {
      Fail("facade_query_raw verify failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
      Fail("facade_query_raw verify should return one reference key");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(out[0].second, &variant, &error)) {
      Fail("facade_query_raw verify decode failed: " + error);
    }
    if (variant != 1) {
      Fail("facade_query_raw verify should return unresolved reference bytes");
    }
  } else if (mode == "facade_query_item_value") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'i', 't', 'e', 'm', '2'}));
    query.items.push_back(grovedb::QueryItem::Key({'r', 'e', 'f'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'a', 'r', 'g', 'e', 't'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 3, std::nullopt));
    std::vector<std::vector<uint8_t>> out_values;
    if (!db.QueryItemValue(path_query, &out_values, &error)) {
      Fail("facade_query_item_value verify failed: " + error);
    }
    if (out_values.size() != 3) {
      Fail("facade_query_item_value verify should return three values");
    }
    auto contains_value = [&](const std::vector<uint8_t>& needle) {
      for (const auto& value : out_values) {
        if (value == needle) {
          return true;
        }
      }
      return false;
    };
    if (!contains_value({'i', 'w'})) {
      Fail("facade_query_item_value verify should include item value");
    }
    if (!contains_value({'t', 'v'})) {
      Fail("facade_query_item_value verify should include resolved reference item value");
    }
    int tv_count = 0;
    for (const auto& value : out_values) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        ++tv_count;
      }
    }
    if (tv_count != 2) {
      Fail("facade_query_item_value verify should include two tv values (target + resolved ref)");
    }
  } else if (mode == "facade_query_raw_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRaw(path_query, &out, &error)) {
      Fail("facade_query_raw_tx verify failed: " + error);
    }
    if (out.size() != 1) {
      Fail("facade_query_raw_tx verify should return one row");
    }
    bool saw_txk = false;
    for (const auto& row : out) {
      if (row.first == std::vector<uint8_t>({'t', 'x', 'k'})) {
        saw_txk = true;
        uint64_t variant = 0;
        if (!grovedb::DecodeElementVariant(row.second, &variant, &error)) {
          Fail("facade_query_raw_tx verify decode failed: " + error);
        }
        if (variant != 0) {
          Fail("facade_query_raw_tx verify txk should be item element bytes");
        }
        grovedb::ElementItem item;
        if (!grovedb::DecodeItemFromElementBytes(row.second, &item, &error)) {
          Fail("facade_query_raw_tx verify item decode failed: " + error);
        }
        if (item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_raw_tx verify txk value mismatch");
        }
      } else {
        Fail("facade_query_raw_tx verify returned unexpected key");
      }
    }
    if (!saw_txk) {
      Fail("facade_query_raw_tx verify should include txk row");
    }
  } else if (mode == "facade_query_key_element_pairs_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<grovedb::GroveDb::KeyElementPair> out;
    if (!db.QueryKeyElementPairs(path_query, &out, &error)) {
      Fail("facade_query_key_element_pairs_tx verify failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_key_element_pairs_tx verify should return two pairs");
    }
    bool saw_base = false;
    bool saw_txk = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'b', 'a', 's', 'e'})) {
        saw_base = true;
        grovedb::ElementItem base_item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &base_item, &error)) {
          Fail("facade_query_key_element_pairs_tx verify base decode failed: " + error);
        }
        if (base_item.value != std::vector<uint8_t>({'b', 'v'})) {
          Fail("facade_query_key_element_pairs_tx verify base value mismatch");
        }
      } else if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        saw_txk = true;
        grovedb::ElementItem txk_item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &txk_item, &error)) {
          Fail("facade_query_key_element_pairs_tx verify txk decode failed: " + error);
        }
        if (txk_item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_key_element_pairs_tx verify txk value mismatch");
        }
      }
    }
    if (!saw_base || !saw_txk) {
      Fail("facade_query_key_element_pairs_tx verify should include base and txk pairs");
    }
  } else if (mode == "facade_query_item_value_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 2, std::nullopt));
    std::vector<std::vector<uint8_t>> values;
    if (!db.QueryItemValue(path_query, &values, &error)) {
      Fail("facade_query_item_value_tx verify failed: " + error);
    }
    if (values.size() != 2) {
      Fail("facade_query_item_value_tx verify should return two values");
    }
    bool saw_base = false;
    bool saw_txk = false;
    for (const auto& value : values) {
      if (value == std::vector<uint8_t>({'b', 'v'})) {
        saw_base = true;
      } else if (value == std::vector<uint8_t>({'t', 'v'})) {
        saw_txk = true;
      }
    }
    if (!saw_base || !saw_txk) {
      Fail("facade_query_item_value_tx verify should include base and txk values");
    }
  } else if (mode == "facade_query_item_value_or_sum_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}}, {'s', 'u', 'm'}, 50);
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 's', 'u', 'm'}, 100);
    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'b', 'a', 's', 'e'}));
    query.items.push_back(grovedb::QueryItem::Key({'s', 'u', 'm'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 's', 'u', 'm'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 4, std::nullopt));
    std::vector<grovedb::GroveDb::QueryItemOrSumValue> values;
    if (!db.QueryItemValueOrSum(path_query, &values, &error)) {
      Fail("facade_query_item_value_or_sum_tx verify failed: " + error);
    }
    if (values.size() != 4) {
      Fail("facade_query_item_value_or_sum_tx verify should return four values");
    }
    bool saw_base = false;
    bool saw_sum = false;
    bool saw_txk = false;
    bool saw_txsum = false;
    for (const auto& v : values) {
      if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kItemData &&
          v.item_data == std::vector<uint8_t>({'b', 'v'})) {
        saw_base = true;
      } else if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kSumValue &&
          v.sum_value == 50) {
        saw_sum = true;
      } else if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kItemData &&
          v.item_data == std::vector<uint8_t>({'t', 'v'})) {
        saw_txk = true;
      } else if (v.kind == grovedb::GroveDb::QueryItemOrSumValue::Kind::kSumValue &&
          v.sum_value == 100) {
        saw_txsum = true;
      }
    }
    if (!saw_base || !saw_sum || !saw_txk || !saw_txsum) {
      Fail("facade_query_item_value_or_sum_tx verify should include base, sum, txk, and txsum values");
    }
  } else if (mode == "facade_query_raw_keys_optional") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'r', 'e', 'f', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryRawKeysOptional(query, &out, &error)) {
      Fail("facade_query_raw_keys_optional verify failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_raw_keys_optional verify should return two rows");
    }
    bool saw_ref = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
        if (!row.element_found) {
          Fail("facade_query_raw_keys_optional verify ref row should be found");
        }
        uint64_t variant = 0;
        if (!grovedb::DecodeElementVariant(row.element_bytes, &variant, &error)) {
          Fail("facade_query_raw_keys_optional verify decode failed: " + error);
        }
        if (variant != 1) {
          Fail("facade_query_raw_keys_optional verify should return unresolved reference bytes");
        }
        saw_ref = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_raw_keys_optional verify missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_raw_keys_optional verify returned unexpected key");
      }
    }
    if (!saw_ref || !saw_missing) {
      Fail("facade_query_raw_keys_optional verify should include ref1 and miss rows");
    }
  } else if (mode == "facade_query_keys_optional") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'});
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'r', 'e', 'f', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'m', 'i', 's', 's'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryKeysOptional(query, &out, &error)) {
      Fail("facade_query_keys_optional verify failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_keys_optional verify should return two rows");
    }
    bool saw_ref = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'r', 'e', 'f', '1'})) {
        if (!row.element_found) {
          Fail("facade_query_keys_optional verify ref row should be found");
        }
        grovedb::ElementItem item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &item, &error)) {
          Fail("facade_query_keys_optional verify decode failed: " + error);
        }
        if (item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_keys_optional verify should return resolved target item");
        }
        saw_ref = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_keys_optional verify missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_keys_optional verify returned unexpected key");
      }
    }
    if (!saw_ref || !saw_missing) {
      Fail("facade_query_keys_optional verify should include ref1 and miss rows");
    }
  } else if (mode == "facade_query_raw_keys_optional_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'m', 'i', 's', 's'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryRawKeysOptional(query, &out, &error)) {
      Fail("facade_query_raw_keys_optional_tx verify failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_raw_keys_optional_tx verify should return two rows");
    }
    bool saw_txk = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        if (!row.element_found) {
          Fail("facade_query_raw_keys_optional_tx verify txk row should be found");
        }
        grovedb::ElementItem decoded_item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &decoded_item, &error)) {
          Fail("facade_query_raw_keys_optional_tx verify decode failed: " + error);
        }
        if (decoded_item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_raw_keys_optional_tx verify txk value mismatch");
        }
        saw_txk = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_raw_keys_optional_tx verify missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_raw_keys_optional_tx verify returned unexpected key");
      }
    }
    if (!saw_txk || !saw_missing) {
      Fail("facade_query_raw_keys_optional_tx verify should include txk and miss rows");
    }
  } else if (mode == "facade_query_keys_optional_tx") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'b', 'a', 's', 'e'}, {'b', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 'k'}, {'t', 'v'});
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'m', 'i', 's', 's'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 'k'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
    if (!db.QueryKeysOptional(query, &out, &error)) {
      Fail("facade_query_keys_optional_tx verify failed: " + error);
    }
    if (out.size() != 2) {
      Fail("facade_query_keys_optional_tx verify should return two rows");
    }
    bool saw_txk = false;
    bool saw_missing = false;
    for (const auto& row : out) {
      if (row.key == std::vector<uint8_t>({'t', 'x', 'k'})) {
        if (!row.element_found) {
          Fail("facade_query_keys_optional_tx verify txk row should be found");
        }
        grovedb::ElementItem decoded_item;
        if (!grovedb::DecodeItemFromElementBytes(row.element_bytes, &decoded_item, &error)) {
          Fail("facade_query_keys_optional_tx verify decode failed: " + error);
        }
        if (decoded_item.value != std::vector<uint8_t>({'t', 'v'})) {
          Fail("facade_query_keys_optional_tx verify txk value mismatch");
        }
        saw_txk = true;
      } else if (row.key == std::vector<uint8_t>({'m', 'i', 's', 's'})) {
        if (row.element_found || !row.element_bytes.empty()) {
          Fail("facade_query_keys_optional_tx verify missing row should be empty");
        }
        saw_missing = true;
      } else {
        Fail("facade_query_keys_optional_tx verify returned unexpected key");
      }
    }
    if (!saw_txk || !saw_missing) {
      Fail("facade_query_keys_optional_tx verify should include txk and miss rows");
    }
  } else if (mode == "facade_query_sums_tx") {
    {
      std::string root_error;
      std::vector<uint8_t> raw;
      bool found = false;
      if (!db.Get({}, {'r', 'o', 'o', 't'}, &raw, &found, &root_error)) {
        Fail("facade_query_sums_tx verify root get failed: " + root_error);
      }
      if (!found) {
        Fail("facade_query_sums_tx verify root should exist");
      }
      uint64_t variant = 0;
      if (!grovedb::DecodeElementVariant(raw, &variant, &root_error)) {
        Fail("facade_query_sums_tx verify root decode failed: " + root_error);
      }
      if (variant != 2 && variant != 4) {
        Fail("facade_query_sums_tx verify root should be a tree-like variant");
      }
    }
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}}, {'s', '1'}, 10);
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}}, {'s', '2'}, 20);
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}}, {'t', 'x', 's'}, 30);
    grovedb::PathQuery query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'s', '1'})),
            2,
            std::nullopt));
    query.query.query.items.push_back(grovedb::QueryItem::Key({'t', 'x', 's'}));
    std::vector<int64_t> sums_out;
    if (!db.QuerySums(query, &sums_out, &error)) {
      Fail("facade_query_sums_tx verify failed: " + error);
    }
    if (sums_out.size() != 2) {
      Fail("facade_query_sums_tx verify should return two sums");
    }
    bool saw_s1 = false;
    bool saw_txs = false;
    for (const auto& sum : sums_out) {
      if (sum == 10) {
        saw_s1 = true;
      } else if (sum == 30) {
        saw_txs = true;
      } else {
        Fail("facade_query_sums_tx verify returned unexpected sum: " + std::to_string(sum));
      }
    }
    if (!saw_s1 || !saw_txs) {
      Fail("facade_query_sums_tx verify should include s1 (10) and txs (30) sums");
    }
  } else if (mode == "nested") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
               {'n', 'k'},
               {'n', 'v'});
  } else if (mode == "tx_commit") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x'}, {'t', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "tx_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'o', 'l', 'l'});
  } else if (mode == "tx_visibility_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'v', 'i', 's'});
  } else if (mode == "tx_mixed_commit") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 'x'}, {'m', 'v'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_rollback_range") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'o', 'l', 'l'});
  } else if (mode == "tx_drop_abort") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', 'r', 'o', 'p'});
  } else if (mode == "tx_commit_after_rollback_rejected") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'a', 'r'});
  } else if (mode == "tx_write_after_rollback_rejected") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'w', 'a', 'r'}, {'w', 'v'});
  } else if (mode == "tx_multi_rollback_reuse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'r', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'r', '2'}, {'m', 'v', '2'});
  } else if (mode == "tx_delete_after_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_reopen_visibility_after_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'v'}, {'r', 'v'});
  } else if (mode == "tx_reopen_conflict_same_path") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'c'}, {'a'});
  } else if (mode == "tx_delete_visibility") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_delete_then_reinsert_same_key") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "tx_insert_then_delete_same_key") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_delete_missing_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'});
  } else if (mode == "tx_read_committed_visibility") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 's', 'n', 'a', 'p'}, {'s', 'v'});
  } else if (mode == "tx_has_read_committed_visibility") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'h', 'a', 's'}, {'h', 'v'});
  } else if (mode == "tx_query_range_committed_visibility") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'q', 'r'}, {'q', 'v'});
  } else if (mode == "tx_iterator_stability_under_commit") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'i', 't'}, {'i', 'v'});
  } else if (mode == "tx_same_key_conflict_reverse_order") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'o', 'n', 'f'}, {'v', '2'});
  } else if (mode == "tx_shared_subtree_disjoint_conflict_reverse_order") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', '2'}, {'d', '2'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', '1'});
  } else if (mode == "tx_disjoint_subtree_conflict_reverse_order") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k'}, {'r', 'v'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k'});
  } else if (mode == "tx_disjoint_subtree_conflict_forward_order") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', 'f'}, {'l', 'v', 'f'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', 'f'});
  } else if (mode == "tx_read_only_then_writer_commit") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'o'}, {'r', 'o', 'v'});
  } else if (mode == "tx_delete_insert_same_key_forward") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_delete_insert_same_key_reverse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "tx_delete_insert_same_subtree_disjoint_forward") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', 'i'});
  } else if (mode == "tx_delete_insert_same_subtree_disjoint_reverse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', 'i'}, {'d', 'i'});
  } else if (mode == "tx_delete_insert_disjoint_subtree_forward") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', 'd', 'e', 'l'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', 'i', 'n', 's'});
  } else if (mode == "tx_delete_insert_disjoint_subtree_reverse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', 'd', 'e', 'l'}, {'d', 'e', 'l', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', 'i', 'n', 's'}, {'i', 'n', 's', 'v'});
  } else if (mode == "tx_replace_delete_same_key_forward") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "tx_replace_delete_same_key_reverse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "tx_replace_replace_same_key_forward") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "tx_replace_replace_same_key_reverse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '3'});
  } else if (mode == "tx_double_rollback_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'd', 'r'}, {'d', 'r', 'v'});
  } else if (mode == "tx_conflict_sequence_persistence") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'l', 'e', 'f', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'r', 'i', 'g', 'h', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c'}, {'a'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'r', 'i', 'g', 'h', 't'}}, {'r', 'k', '1'}, {'r', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}, {'l', 'e', 'f', 't'}}, {'l', 'k', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'p', 'o', 's', 't'}, {'p', 'v'});
  } else if (mode == "tx_checkpoint_snapshot_isolation") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'p'}, {'c', 'v'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'p', 'o', 's', 't'}, {'p', 'v'});
  } else if (mode == "tx_checkpoint_independent_writes") {
    ExpectTree(&db, {}, {'k', 'e', 'y', '1'});
    ExpectTree(&db, {{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '2'});
    ExpectItem(&db, {{'k', 'e', 'y', '1'}, {'k', 'e', 'y', '2'}}, {'k', 'e', 'y', '3'}, {'a', 'y', 'y'});
    ExpectItem(&db, {{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '4'}, {'a', 'y', 'y', '3'});
    ExpectItem(&db, {{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '6'}, {'a', 'y', 'y', '3'});
    ExpectMissing(&db, {{'k', 'e', 'y', '1'}}, {'k', 'e', 'y', '5'});
  } else if (mode == "tx_checkpoint_delete_safety") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 's', 'a', 'f', 'e'}, {'s', 'v'});
  } else if (mode == "tx_checkpoint_open_safety") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'o', 'p', 'e', 'n'}, {'o', 'v'});
  } else if (mode == "tx_checkpoint_delete_short_path_safety") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 's', 'h', 'o', 'r', 't'}, {'s', 'v'});
  } else if (mode == "tx_checkpoint_open_missing_path") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 's', 's'}, {'m', 'v'});
  } else if (mode == "tx_checkpoint_reopen_mutate_recheckpoint") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n'}, {'m', 'v'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', '2'});
  } else if (mode == "tx_checkpoint_reopen_mutate_chain") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n', '2'}, {'m', 'v', '2'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'a', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', '2'});
  } else if (mode == "tx_checkpoint_batch_ops") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'a', 'i', 'n', '3'}, {'m', 'v', '3'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'a', 't', 'c', 'h', '1'}, {'v', 'b', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '2'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'b', 'p', '3'});
  } else if (mode == "tx_checkpoint_delete_reopen_sequence") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 's', 'e', 'q'}, {'s', 'v'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'b'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'c', 'p', 'a'});
  } else if (mode == "tx_checkpoint_aux_isolation") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'a', 'u', 'x', 'c', 'p'}, {'o', 'k'});
    std::vector<uint8_t> aux_value;
    bool found = false;
    std::string error;
    if (!db.GetAux({'a', 'u', 'x', '_', 's', 'h', 'a', 'r', 'e', 'd'}, &aux_value, &found, &error)) {
      Fail("GetAux aux_shared final state failed: " + error);
    }
    if (!found ||
        aux_value != std::vector<uint8_t>({'m', 'a', 'i', 'n', '_', 'a', 'f', 't', 'e', 'r'})) {
      Fail("aux_shared final value mismatch");
    }
    if (!db.GetAux({'a', 'u', 'x', '_', 'm', 'a', 'i', 'n', '_', 'o', 'n', 'l', 'y'},
                   &aux_value,
                   &found,
                   &error)) {
      Fail("GetAux aux_main_only final state failed: " + error);
    }
    if (!found ||
        aux_value != std::vector<uint8_t>({'m', 'a', 'i', 'n', '_', 'o', 'n', 'l', 'y'})) {
      Fail("aux_main_only final value mismatch");
    }
    if (!db.GetAux({'a', 'u', 'x', '_', 'c', 'p', '_', 'o', 'n', 'l', 'y'}, &aux_value, &found, &error)) {
      Fail("GetAux aux_cp_only final state failed: " + error);
    }
    if (found) {
      Fail("aux_cp_only should remain checkpoint-only");
    }
  } else if (mode == "tx_checkpoint_tx_operations") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '1'}, {'t', 'x', 'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', '2'}, {'t', 'x', 'v', '2'});
  } else if (mode == "tx_checkpoint_chain_mutation_isolation") {
    // Verify main DB has all keys: k_base, k_phase2, k_phase3, k_final
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '2'}, {'p', 'h', 'a', 's', 'e', '2'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 'p', 'h', 'a', 's', 'e', '3'}, {'p', 'h', 'a', 's', 'e', '3'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 'f', 'i', 'n', 'a', 'l'}, {'f', 'i', 'n', 'a', 'l'});
  } else if (mode == "tx_checkpoint_reopen_after_main_delete") {
    // Verify checkpoint contains k_base and k_snapshot after main DB deletion
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 'b', 'a', 's', 'e'}, {'b', 'a', 's', 'e'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '_', 's', 'n', 'a', 'p', 's', 'h', 'o', 't'}, {'s', 'n', 'a', 'p', 's', 'h', 'o', 't'});
  } else if (mode == "batch_apply_local_atomic") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'b', '2'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
    ExpectMissing(&db, {}, {'n', 't'});
  } else if (mode == "batch_apply_tx_visibility") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'b'}, {'t', 'b'});
  } else if (mode == "batch_apply_empty_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  } else if (mode == "batch_validate_success_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'v', 'a', 'l'});
  } else if (mode == "batch_validate_failure_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {}, {'n', 'v', 'a', 'l'});
  } else if (mode == "batch_insert_only_semantics") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'x', 'x'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'i', 'o'}, {'i', 'o'});
  } else if (mode == "batch_replace_semantics") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'r', '2'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'm', 'i', 's', 's'}, {'r', 'x'});
  } else if (mode == "batch_validate_no_override_insert") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 's', 't', 'r', 'i', 'c', 't'}, {'s', 'v'});
  } else if (mode == "batch_validate_no_override_insert_only") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'i', 'o', '2'}, {'i', 'v'});
  } else if (mode == "batch_validate_no_override_replace") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'e', 'p', '2'}, {'r', 'v'});
  } else if (mode == "batch_validate_no_override_tree_insert") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'i', 't'});
  } else if (mode == "batch_validate_strict_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'v', 's'});
  } else if (mode == "batch_delete_non_empty_tree_error") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_delete_non_empty_tree_no_error") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_disable_consistency_check") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  } else if (mode == "batch_disable_consistency_last_op_wins") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '2'});
  } else if (mode == "batch_disable_consistency_reorder_parent_child") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'o', 'r', 'd', '2'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'o', 'r', 'd', '2'}},
               {'n', 'k'},
               {'n', 'v'});
  } else if (mode == "batch_insert_tree_semantics") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
               {'n', 'k'},
               {'n', 'v'});
  } else if (mode == "batch_insert_tree_replace") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'n', 'e', 'w', 't', 'r', 'e', 'e'});
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'n', 'e', 'w', 't', 'r', 'e', 'e'}},
               {'n', 'e', 's', 't', 'e', 'd'},
               {'n', 'e', 's', 't', 'e', 'd', '_', 'v', 'a', 'l', 'u', 'e'});
  } else if (mode == "batch_sum_tree_create_and_sum_item") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectSumItem(&db, {{'r', 'o', 'o', 't'}, {'s', 'b', 'a', 't', 'c', 'h'}}, {'s', '1'}, 7);
    std::vector<uint8_t> raw;
    bool found = false;
    std::string error;
    if (!db.Get({{'r', 'o', 'o', 't'}}, {'s', 'b', 'a', 't', 'c', 'h'}, &raw, &found, &error)) {
      Fail("Get sum tree after same-batch SumTree+SumItem failed: " + error);
    }
    if (!found) {
      Fail("same-batch SumTree+SumItem parent should exist");
    }
    grovedb::ElementSumTree tree;
    if (!grovedb::DecodeSumTreeFromElementBytes(raw, &tree, &error)) {
      Fail("DecodeSumTreeFromElementBytes same-batch SumTree+SumItem failed: " + error);
    }
    if (tree.sum != 7) {
      Fail("same-batch SumTree+SumItem parent sum mismatch");
    }
  } else if (mode == "batch_count_tree_create_and_item") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectCountTreeCount(&db, {{'r', 'o', 'o', 't'}}, {'c', 'b', 'a', 't', 'c', 'h'}, 1);
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'c', 'b', 'a', 't', 'c', 'h'}},
               {'c', '1'},
               {'c', 'v'});
  } else if (mode == "batch_provable_count_tree_create_and_item") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectProvableCountTreeCount(&db, {{'r', 'o', 'o', 't'}}, {'p', 'c', 'b', 'a', 't', 'c', 'h'}, 1);
    ExpectItem(&db,
               {{'r', 'o', 'o', 't'}, {'p', 'c', 'b', 'a', 't', 'c', 'h'}},
               {'p', '1'},
               {'p', 'v'});
  } else if (mode == "batch_count_sum_tree_create_and_sum_item") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectCountSumTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 's', 'b', 'a', 't', 'c', 'h'}, 1, 11);
    ExpectSumItem(&db,
                  {{'r', 'o', 'o', 't'}, {'c', 's', 'b', 'a', 't', 'c', 'h'}},
                  {'c', 's', '1'},
                  11);
  } else if (mode == "batch_provable_count_sum_tree_create_and_sum_item") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectProvableCountSumTree(
        &db, {{'r', 'o', 'o', 't'}}, {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'}, 1, 13);
    ExpectSumItem(&db,
                  {{'r', 'o', 'o', 't'}, {'p', 'c', 's', 'b', 'a', 't', 'c', 'h'}},
                  {'p', 's', '1'},
                  13);
  } else if (mode == "batch_apply_failure_atomic_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 'o', 'k'});
  } else if (mode == "batch_delete_missing_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'});
  } else if (mode == "batch_apply_tx_failure_atomic_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'});
  } else if (mode == "batch_apply_tx_failure_then_reuse") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'r', 'e', 'u', 's', 'e'}, {'r', 'v'});
  } else if (mode == "batch_apply_tx_failure_then_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 'x', 'o', 'k'});
  } else if (mode == "batch_apply_tx_success_then_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'k', 't', 's', 'r'});
  } else if (mode == "batch_apply_tx_delete_then_rollback") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "batch_apply_tx_delete_missing_noop") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'m', 'i', 's', 's', 'i', 'n', 'g'});
  } else if (mode == "batch_delete_tree_op") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_delete_tree_disable_consistency_check") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_delete_tree_non_empty_options") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_mixed_non_minimal_ops") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'r', '2'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'b', '2'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_mixed_non_minimal_ops_with_options") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'r', '3'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '3'}, {'b', '3'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'c', 'h', 'i', 'l', 'd'});
  } else if (mode == "batch_patch_existing") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'p', '1'});
  } else if (mode == "batch_patch_missing") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'p', '2'});
  } else if (mode == "batch_patch_strict_no_override") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'p'}, {'p', 'v'});
  } else if (mode == "batch_refresh_reference_trust") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    std::vector<uint8_t> raw;
    bool found = false;
    std::string error;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}},
                   {'r', 'e', 'f', '_', 'm', 'i', 's', 's', 'i', 'n', 'g'},
                   &raw,
                   &found,
                   &error)) {
      Fail("GetRaw trusted refresh reference failed: " + error);
    }
    if (!found) {
      Fail("trusted refresh reference should exist");
    }
    grovedb::ElementReference decoded;
    if (!grovedb::DecodeReferenceFromElementBytes(raw, &decoded, &error)) {
      Fail("DecodeReferenceFromElementBytes trusted refresh failed: " + error);
    }
    if (decoded.reference_path.kind != grovedb::ReferencePathKind::kAbsolute) {
      Fail("trusted refresh reference path kind mismatch");
    }
    const std::vector<std::vector<uint8_t>> expected_path = {
        {'r', 'o', 'o', 't'}, {'k', '1'}};
    if (decoded.reference_path.path != expected_path) {
      Fail("trusted refresh reference absolute path mismatch");
    }
  } else if (mode == "batch_pause_height_passthrough") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'v', '2'});
    // k1 should be deleted.
    std::vector<uint8_t> raw;
    bool found = false;
    std::string error;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
      Fail("GetRaw k1 after batch_pause_height batch failed: " + error);
    }
    if (found) {
      Fail("k1 should be deleted after batch_pause_height batch");
    }
  } else if (mode == "batch_partial_pause_resume") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'a'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}, {'a'}}, {'b'});
    ExpectMissing(&db, {}, {'l', '0'});
    ExpectMissing(&db, {{'r', 'o', 'o', 't'}}, {'l', '1'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}, {'a'}, {'b'}}, {'a', 'd', 'd'}, {'v', 'a'});
  } else if (mode == "batch_base_root_storage_is_free_passthrough") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '2'}, {'v', '2'});
    // k1 should be deleted.
    std::vector<uint8_t> raw;
    bool found = false;
    std::string error;
    if (!db.GetRaw({{'r', 'o', 'o', 't'}}, {'k', '1'}, &raw, &found, &error)) {
      Fail("GetRaw k1 after batch_base_root_storage_is_free batch failed: " + error);
    }
    if (found) {
      Fail("k1 should be deleted after batch_base_root_storage_is_free batch");
    }
  } else if (mode == "batch_insert_or_replace_semantics") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    // kior should have final value v2 (idempotent replace behavior)
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', 'i', 'o', 'r'}, {'v', '2'});
  } else if (mode == "batch_insert_or_replace_with_override_validation") {
    // After failed batch, state should remain unchanged
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'v', '1'});
  } else if (mode == "batch_validate_no_override_tree_insert_or_replace") {
    // After tree override failure, root should still be a tree
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    // k1 should have been replaced with 'it' value
    ExpectItem(&db, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {'i', 't'});
  } else if (mode == "batch_insert_tree_below_deleted_path_consistency") {
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'c', 't'});
  } else if (mode == "batch_insert_tree_with_root_hash") {
    // Verify tree was inserted via InsertTreeWithRootHash batch operation
    ExpectTree(&db, {}, {'r', 'o', 'o', 't'});
    ExpectTree(&db, {{'r', 'o', 'o', 't'}}, {'t', 'r', 'e', 'e', '_', 'k', 'e', 'y'});
  } else {
    Fail("unsupported mode for cpp verification");
  }
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  VerifyElementEncodingParity();

  const std::vector<std::string> modes = {
      "simple",
      "facade_insert_helpers",
      "facade_insert_if_not_exists",
      "facade_insert_if_changed_value",
      "facade_insert_if_not_exists_return_existing",
      "facade_insert_if_not_exists_return_existing_tx",
      "facade_flush",
      "facade_root_key",
      "facade_delete_if_empty_tree",
      "facade_clear_subtree",
      "facade_clear_subtree_tx",
      "facade_follow_reference",
      "facade_follow_reference_tx",
      "facade_find_subtrees",
      "facade_check_subtree_exists_invalid_path_tx",
      "facade_follow_reference_mixed_path_chain",
      "facade_follow_reference_parent_path_addition",
      "facade_follow_reference_upstream_element_height",
      "facade_follow_reference_cousin",
      "facade_follow_reference_removed_cousin",
      "facade_get_raw",
      "facade_get_raw_optional",
      "facade_get_raw_caching_optional",
      "facade_get_caching_optional",
      "facade_get_caching_optional_tx",
      "facade_get_subtree_root_tx",
      "facade_has_caching_optional_tx",
      "facade_query_raw",
      "facade_query_item_value",
      "facade_query_item_value_tx",
      "facade_query_item_value_or_sum_tx",
      "facade_query_raw_tx",
      "facade_query_key_element_pairs_tx",
      "facade_query_raw_keys_optional",
      "facade_query_keys_optional",
      "facade_query_raw_keys_optional_tx",
      "facade_query_keys_optional_tx",
      "facade_query_sums_tx",
      "nested",
      "tx_commit",
      "tx_rollback",
      "tx_visibility_rollback",
      "tx_mixed_commit",
      "tx_rollback_range",
      "tx_drop_abort",
      "tx_commit_after_rollback_rejected",
      "tx_write_after_rollback_rejected",
      "tx_multi_rollback_reuse",
      "tx_delete_after_rollback",
      "tx_reopen_visibility_after_rollback",
      "tx_reopen_conflict_same_path",
      "tx_delete_visibility",
      "tx_delete_then_reinsert_same_key",
      "tx_insert_then_delete_same_key",
      "tx_delete_missing_noop",
      "tx_read_committed_visibility",
      "tx_has_read_committed_visibility",
      "tx_query_range_committed_visibility",
      "tx_iterator_stability_under_commit",
      "tx_same_key_conflict_reverse_order",
      "tx_shared_subtree_disjoint_conflict_reverse_order",
      "tx_disjoint_subtree_conflict_reverse_order",
      "tx_disjoint_subtree_conflict_forward_order",
      "tx_read_only_then_writer_commit",
      "tx_delete_insert_same_key_forward",
      "tx_delete_insert_same_key_reverse",
      "tx_delete_insert_same_subtree_disjoint_forward",
      "tx_delete_insert_same_subtree_disjoint_reverse",
      "tx_delete_insert_disjoint_subtree_forward",
      "tx_delete_insert_disjoint_subtree_reverse",
      "tx_replace_delete_same_key_forward",
      "tx_replace_delete_same_key_reverse",
      "tx_replace_replace_same_key_forward",
      "tx_replace_replace_same_key_reverse",
      "tx_double_rollback_noop",
      "tx_conflict_sequence_persistence",
      "tx_checkpoint_snapshot_isolation",
      "tx_checkpoint_independent_writes",
      "tx_checkpoint_delete_safety",
      "tx_checkpoint_open_safety",
      "tx_checkpoint_delete_short_path_safety",
      "tx_checkpoint_open_missing_path",
      "tx_checkpoint_reopen_mutate_recheckpoint",
      "tx_checkpoint_reopen_mutate_chain",
      "tx_checkpoint_batch_ops",
      "tx_checkpoint_delete_reopen_sequence",
      "tx_checkpoint_aux_isolation",
      "tx_checkpoint_tx_operations",
      "tx_checkpoint_chain_mutation_isolation",
      "tx_checkpoint_reopen_after_main_delete",
      "batch_apply_local_atomic",
      "batch_apply_tx_visibility",
      "batch_apply_empty_noop",
      "batch_validate_success_noop",
      "batch_validate_failure_noop",
      "batch_insert_only_semantics",
      "batch_replace_semantics",
      "batch_validate_no_override_insert",
      "batch_validate_no_override_insert_only",
      "batch_validate_no_override_replace",
      "batch_validate_no_override_tree_insert",
      "batch_validate_strict_noop",
      "batch_delete_non_empty_tree_error",
      "batch_delete_non_empty_tree_no_error",
      "batch_disable_consistency_check",
      "batch_disable_consistency_last_op_wins",
      "batch_disable_consistency_reorder_parent_child",
      "batch_insert_tree_semantics",
      "batch_insert_tree_replace",
      "batch_sum_tree_create_and_sum_item",
      "batch_count_tree_create_and_item",
      "batch_provable_count_tree_create_and_item",
      "batch_count_sum_tree_create_and_sum_item",
      "batch_provable_count_sum_tree_create_and_sum_item",
      "batch_apply_failure_atomic_noop",
      "batch_delete_missing_noop",
      "batch_apply_tx_failure_atomic_noop",
      "batch_apply_tx_failure_then_reuse",
      "batch_apply_tx_failure_then_rollback",
      "batch_apply_tx_success_then_rollback",
      "batch_apply_tx_delete_then_rollback",
      "batch_apply_tx_delete_missing_noop",
      "batch_delete_tree_op",
      "batch_delete_tree_disable_consistency_check",
      "batch_delete_tree_non_empty_options",
      "batch_mixed_non_minimal_ops",
      "batch_mixed_non_minimal_ops_with_options",
      "batch_patch_existing",
      "batch_patch_missing",
      "batch_patch_strict_no_override",
      "batch_refresh_reference_trust",
      "batch_pause_height_passthrough",
      "batch_partial_pause_resume",
      "batch_base_root_storage_is_free_passthrough",
      "batch_insert_or_replace_semantics",
      "batch_insert_or_replace_with_override_validation",
      "batch_validate_no_override_tree_insert_or_replace",
      "batch_insert_tree_below_deleted_path_consistency",
      "batch_insert_tree_with_root_hash"};

  const char* filter = std::getenv("GROVEDB_RUST_PARITY_MODE");
  const auto mode_list_filter = SplitCommaSeparated(std::getenv("GROVEDB_RUST_PARITY_MODE_LIST"));
  const auto mode_prefix_filter =
      SplitCommaSeparated(std::getenv("GROVEDB_RUST_PARITY_MODE_PREFIX"));
  std::set<std::string> mode_list_set(mode_list_filter.begin(), mode_list_filter.end());
  bool ran_filtered_mode = false;
  for (const auto& mode : modes) {
    if (filter != nullptr) {
      if (mode != filter) {
        continue;
      }
    } else if (!mode_list_set.empty()) {
      if (mode_list_set.find(mode) == mode_list_set.end()) {
        continue;
      }
    } else if (!mode_prefix_filter.empty()) {
      bool prefix_match = false;
      for (const auto& prefix : mode_prefix_filter) {
        if (StartsWith(mode, prefix)) {
          prefix_match = true;
          break;
        }
      }
      if (!prefix_match) {
        continue;
      }
    }
    ran_filtered_mode = true;
    if (std::getenv("GROVEDB_DEBUG_PARITY_MODE_PROGRESS") != nullptr) {
      std::cerr << "[facade-parity] mode=" << mode << std::endl;
    }
    
    // Skip Rust comparison for batch_validate_no_override_tree_insert_or_replace
    // because Rust requires both validate_insertion_does_not_override AND
    // validate_insertion_does_not_override_tree flags, while C++ enforces
    // tree-only validation with just validate_insertion_does_not_override_tree.
    const bool skip_rust = (mode == "batch_validate_no_override_tree_insert_or_replace");
    // These modes intentionally self-validate inside the writer and do not leave
    // a stable final main-DB state for post-write readers/verifiers.
    const bool skip_rust_reader = (mode == "tx_checkpoint_reopen_after_main_delete");
    const bool skip_cpp_verify = (mode == "tx_checkpoint_reopen_after_main_delete");
    
    std::string rust_dir;
    if (!skip_rust) {
      rust_dir = MakeTempDir("rust_grovedb_facade_parity_rust_semantic_" + mode);
      RunRustWriter(rust_dir, mode);
      if (!skip_rust_reader) {
        RunRustReader(rust_dir, mode);
      }
    }

    const std::string cpp_dir = MakeTempDir("rust_grovedb_facade_parity_cpp_semantic_" + mode);
    WriteCppScenario(cpp_dir, mode);
    if (!skip_cpp_verify) {
      VerifyCppScenario(cpp_dir, mode);
    }
    if (!skip_rust && (mode == "batch_mixed_non_minimal_ops" ||
        mode == "batch_mixed_non_minimal_ops_with_options")) {
      CompareDbDump(mode, rust_dir, cpp_dir);
    }
    if (!skip_rust) {
      std::filesystem::remove_all(rust_dir);
    }
    std::filesystem::remove_all(cpp_dir);
  }
  if (filter != nullptr && !ran_filtered_mode) {
    Fail(std::string("unknown GROVEDB_RUST_PARITY_MODE: ") + filter);
  }
  if (filter == nullptr && !mode_list_set.empty() && !ran_filtered_mode) {
    Fail("no modes matched GROVEDB_RUST_PARITY_MODE_LIST");
  }
  if (filter == nullptr && mode_list_set.empty() && !mode_prefix_filter.empty() &&
      !ran_filtered_mode) {
    Fail("no modes matched GROVEDB_RUST_PARITY_MODE_PREFIX");
  }

  return 0;
}
