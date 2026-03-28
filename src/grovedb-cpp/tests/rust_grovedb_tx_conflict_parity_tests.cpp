#include "element.h"
#include "grovedb.h"
#include "test_utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

std::map<std::string, bool> RunRustProbe() {
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string db_dir = MakeTempDir("grovedb_cpp_rust_grovedb_tx_probe_" + std::to_string(now));
  const std::string out_file =
      (std::filesystem::path(db_dir) / "rust_tx_probe_output.txt").string();
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_grovedb_tx_semantics_probe \"" +
      db_dir + "\" > \"" + out_file + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust_grovedb_tx_semantics_probe");
  }

  std::ifstream in(out_file);
  if (!in.is_open()) {
    Fail("failed to read rust tx probe output");
  }

  std::map<std::string, bool> rows;
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);
    if (value == "true") {
      rows[key] = true;
    } else if (value == "false") {
      rows[key] = false;
    }
  }

  std::filesystem::remove_all(db_dir);
  return rows;
}

bool GetExpected(const std::map<std::string, bool>& rows, const std::string& key) {
  const auto it = rows.find(key);
  if (it == rows.end()) {
    Fail("missing rust tx probe key: " + key);
  }
  return it->second;
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  const auto expected = RunRustProbe();

  std::string error;
  grovedb::GroveDb db;
  auto now2 = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string dir = MakeTempDir("grovedb_cpp_tx_conflict_parity_" + std::to_string(now2));
  if (!db.Open(dir, &error)) {
    Fail("cpp Open failed: " + error);
  }

  std::vector<uint8_t> tree_element;
  std::vector<uint8_t> item_element;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error) ||
      !grovedb::EncodeItemToElementBytes({'v'}, &item_element, &error)) {
    Fail("cpp element encoding failed: " + error);
  }

  if (!db.Insert({}, {'l', 'e', 'a', 'f', 'A'}, tree_element, &error) ||
      !db.Insert({}, {'l', 'e', 'a', 'f', 'B'}, tree_element, &error)) {
    Fail("cpp setup insert failed: " + error);
  }

  grovedb::GroveDb::Transaction tx_a;
  grovedb::GroveDb::Transaction tx_b;
  if (!db.StartTransaction(&tx_a, &error) || !db.StartTransaction(&tx_b, &error)) {
    Fail("cpp start disjoint tx failed: " + error);
  }
  if (!db.Insert({{'l', 'e', 'a', 'f', 'A'}}, {'a', '1'}, item_element, &tx_a, &error) ||
      !db.Insert({{'l', 'e', 'a', 'f', 'B'}}, {'b', '1'}, item_element, &tx_b, &error)) {
    Fail("cpp disjoint tx insert failed: " + error);
  }
  const bool disjoint_commit_a_ok = db.CommitTransaction(&tx_a, &error);
  const bool disjoint_commit_b_ok = db.CommitTransaction(&tx_b, &error);

  if (disjoint_commit_a_ok != GetExpected(expected, "disjoint_commit_a_ok")) {
    Fail("cpp disjoint_commit_a_ok mismatch vs rust");
  }
  if (disjoint_commit_b_ok != GetExpected(expected, "disjoint_commit_b_ok")) {
    Fail("cpp disjoint_commit_b_ok mismatch vs rust");
  }

  grovedb::GroveDb::Transaction tx_c1;
  grovedb::GroveDb::Transaction tx_c2;
  if (!db.StartTransaction(&tx_c1, &error) || !db.StartTransaction(&tx_c2, &error)) {
    Fail("cpp start same-path tx failed: " + error);
  }
  std::vector<uint8_t> v1;
  std::vector<uint8_t> v2;
  if (!grovedb::EncodeItemToElementBytes({'a'}, &v1, &error) ||
      !grovedb::EncodeItemToElementBytes({'b'}, &v2, &error)) {
    Fail("cpp same-path encode failed: " + error);
  }
  if (!db.Insert({{'l', 'e', 'a', 'f', 'A'}}, {'c', 'o', 'n', 'f', 'l', 'i', 'c', 't'}, v1, &tx_c1,
                 &error) ||
      !db.Insert({{'l', 'e', 'a', 'f', 'A'}}, {'c', 'o', 'n', 'f', 'l', 'i', 'c', 't'}, v2, &tx_c2,
                 &error)) {
    Fail("cpp same-path insert failed: " + error);
  }
  const bool same_path_commit_1_ok = db.CommitTransaction(&tx_c1, &error);
  const bool same_path_commit_2_ok = db.CommitTransaction(&tx_c2, &error);

  if (same_path_commit_1_ok != GetExpected(expected, "same_path_commit_1_ok")) {
    Fail("cpp same_path_commit_1_ok mismatch vs rust");
  }
  if (same_path_commit_2_ok != GetExpected(expected, "same_path_commit_2_ok")) {
    Fail("cpp same_path_commit_2_ok mismatch vs rust");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
