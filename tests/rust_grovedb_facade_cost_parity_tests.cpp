#include "element.h"
#include "grovedb.h"
#include "test_utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

struct CostRow {
  uint32_t seek_count = 0;
  uint64_t storage_loaded_bytes = 0;
  uint32_t hash_node_calls = 0;
  uint32_t added_bytes = 0;
  uint32_t replaced_bytes = 0;
  uint32_t removed_bytes = 0;
};

void RunRustCostWriter(const std::string& db_dir, const std::string& out_file) {
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_grovedb_facade_cost_writer \"" +
      db_dir + "\" > \"" + out_file + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb facade cost writer");
  }
}

std::map<std::string, CostRow> LoadExpected(const std::string& out_file) {
  std::ifstream in(out_file);
  if (!in.is_open()) {
    Fail("failed to open rust facade cost output");
  }
  std::map<std::string, CostRow> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream ss(line);
    std::string name;
    CostRow row;
    if (!std::getline(ss, name, '\t')) {
      continue;
    }
    std::string field;
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row seek field: " + line);
    }
    row.seek_count = static_cast<uint32_t>(std::stoul(field));
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row loaded field: " + line);
    }
    row.storage_loaded_bytes = static_cast<uint64_t>(std::stoull(field));
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row hash field: " + line);
    }
    row.hash_node_calls = static_cast<uint32_t>(std::stoul(field));
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row added field: " + line);
    }
    row.added_bytes = static_cast<uint32_t>(std::stoul(field));
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row replaced field: " + line);
    }
    row.replaced_bytes = static_cast<uint32_t>(std::stoul(field));
    if (!std::getline(ss, field, '\t')) {
      Fail("invalid cost row removed field: " + line);
    }
    row.removed_bytes = static_cast<uint32_t>(std::stoul(field));
    rows.emplace(name, row);
  }
  return rows;
}

bool ExpectCostEqual(const std::string& label,
                     const grovedb::OperationCost& cost,
                     const std::map<std::string, CostRow>& expected,
                     std::set<std::string>* seen) {
  const auto it = expected.find(label);
  if (it == expected.end()) {
    Fail("missing expected row for " + label);
  }
  if (seen != nullptr) {
    seen->insert(label);
  }
  const CostRow& e = it->second;
  const uint32_t removed = cost.storage_cost.removed_bytes.TotalRemovedBytes();
  if (cost.seek_count == e.seek_count && cost.storage_loaded_bytes == e.storage_loaded_bytes &&
      cost.hash_node_calls == e.hash_node_calls && cost.storage_cost.added_bytes == e.added_bytes &&
      cost.storage_cost.replaced_bytes == e.replaced_bytes && removed == e.removed_bytes) {
    return true;
  }
  std::cerr << "cost mismatch for " << label << ": got(seek=" << cost.seek_count
            << ", loaded=" << cost.storage_loaded_bytes << ", hash=" << cost.hash_node_calls
            << ", add=" << cost.storage_cost.added_bytes << ", replace="
            << cost.storage_cost.replaced_bytes << ", remove=" << removed << "), expected(seek="
            << e.seek_count << ", loaded=" << e.storage_loaded_bytes << ", hash="
            << e.hash_node_calls << ", add=" << e.added_bytes << ", replace=" << e.replaced_bytes
            << ", remove=" << e.removed_bytes << ")\n";
  return false;
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string rust_dir = MakeTempDir("rust_grovedb_facade_cost_parity_" + std::to_string(now));
  const std::string out_file = (std::filesystem::path(rust_dir) / "rust_facade_costs.tsv").string();
  RunRustCostWriter(rust_dir, out_file);
  const auto expected = LoadExpected(out_file);
  std::filesystem::remove_all(rust_dir);

  now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string cpp_dir = MakeTempDir("rust_grovedb_facade_cost_parity_" + std::to_string(now));

  std::string error;
  grovedb::GroveDb db;
  if (!db.Open(cpp_dir, &error)) {
    Fail("cpp open failed: " + error);
  }

  std::vector<uint8_t> tree_element;
  std::vector<uint8_t> item_v1;
  std::vector<uint8_t> item_v2;
  std::vector<uint8_t> item_tx;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', '1'}, &item_v1, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', '2'}, &item_v2, &error) ||
      !grovedb::EncodeItemToElementBytes({'t', 'v'}, &item_tx, &error)) {
    Fail("encode element failed: " + error);
  }

  grovedb::OperationCost cost;
  std::set<std::string> seen_labels;
  bool all_match = true;
  if (!db.Insert({}, {'r', 'o', 'o', 't'}, tree_element, &cost, &error)) {
    Fail("cpp insert root failed: " + error);
  }
  all_match &= ExpectCostEqual("insert_root_tree", cost, expected, &seen_labels);

  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v1, &cost, &error)) {
    Fail("cpp insert item failed: " + error);
  }
  all_match &= ExpectCostEqual("insert_item", cost, expected, &seen_labels);

  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v2, &cost, &error)) {
    Fail("cpp replace item failed: " + error);
  }
  all_match &= ExpectCostEqual("replace_item", cost, expected, &seen_labels);

  bool deleted = false;
  if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &cost, &error)) {
    Fail("cpp delete item failed: " + error);
  }
  if (!deleted) {
    Fail("cpp delete item expected deleted=true");
  }
  all_match &= ExpectCostEqual("delete_item", cost, expected, &seen_labels);

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("cpp start tx failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 't', 'x'}, item_tx, &cost, &tx, &error)) {
    Fail("cpp tx insert failed: " + error);
  }
  all_match &= ExpectCostEqual("tx_insert_item", cost, expected, &seen_labels);
  if (!db.RollbackTransaction(&tx, &error)) {
    Fail("cpp rollback tx failed: " + error);
  }

  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '2'}, item_v2, &cost, &error)) {
    Fail("cpp insert item k2 failed: " + error);
  }
  all_match &= ExpectCostEqual("insert_item_k2", cost, expected, &seen_labels);

  if (!db.StartTransaction(&tx, &error)) {
    Fail("cpp start tx for delete failed: " + error);
  }
  if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '2'}, &deleted, &cost, &tx, &error)) {
    Fail("cpp tx delete item failed: " + error);
  }
  all_match &= ExpectCostEqual("tx_delete_item", cost, expected, &seen_labels);
  if (!db.RollbackTransaction(&tx, &error)) {
    Fail("cpp rollback tx delete failed: " + error);
  }

  if (seen_labels.size() != expected.size()) {
    std::string missing;
    for (const auto& [label, _] : expected) {
      if (seen_labels.find(label) == seen_labels.end()) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += label;
      }
    }
    if (!missing.empty()) {
      Fail("rust facade cost rows not asserted by C++ parity test: " + missing);
    }
  }

  std::filesystem::remove_all(cpp_dir);
  if (!all_match) {
    Fail("rust/cpp facade cost parity mismatch");
  }
  return 0;
}
