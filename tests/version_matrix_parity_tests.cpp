#include "grovedb.h"
#include "proof.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {

bool SameVerifiedElements(const std::vector<grovedb::VerifiedPathKeyElement>& lhs,
                          const std::vector<grovedb::VerifiedPathKeyElement>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].path != rhs[i].path || lhs[i].key != rhs[i].key ||
        lhs[i].has_element != rhs[i].has_element ||
        lhs[i].element_bytes != rhs[i].element_bytes) {
      return false;
    }
  }
  return true;
}

void VerifyFixtureForVersion(const std::string& fixture_name,
                             const std::vector<uint8_t>& proof,
                             const grovedb::PathQuery& query,
                             const grovedb::GroveVersion& version,
                             const std::vector<uint8_t>& expected_root,
                             const std::vector<grovedb::VerifiedPathKeyElement>& expected_elements) {
  std::vector<uint8_t> root;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProofForVersion(proof, query, version, &root, &elements, &error)) {
    Fail("version matrix verify failed fixture=" + fixture_name + " version=" + version.ToString() +
         " error=" + error);
  }
  if (root != expected_root) {
    Fail("version matrix root mismatch fixture=" + fixture_name + " version=" + version.ToString());
  }
  if (!SameVerifiedElements(elements, expected_elements)) {
    Fail("version matrix elements mismatch fixture=" + fixture_name + " version=" + version.ToString());
  }
  std::cout << "VERSION_MATRIX fixture=" << fixture_name << " version=" << version.ToString()
            << " status=PASS elements=" << elements.size() << "\n";
}

size_t AssertIsEmptyTreeForVersionMatrix(const std::vector<grovedb::GroveVersion>& versions) {
  const std::string dir = MakeTempDir("version_matrix") + "_is_empty_tree";
  std::string error;
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("version matrix IsEmptyTreeForVersion open failed: " + error);
  }

  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("version matrix IsEmptyTreeForVersion root insert failed: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'e', 'm', 'p'}, &error)) {
    Fail("version matrix IsEmptyTreeForVersion empty subtree insert failed: " + error);
  }

  bool baseline_empty = false;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'e', 'm', 'p'}},
                                grovedb::GroveVersion::V4_0_0(),
                                &baseline_empty,
                                &error)) {
    Fail("version matrix IsEmptyTreeForVersion baseline empty check failed: " + error);
  }
  if (!baseline_empty) {
    Fail("version matrix IsEmptyTreeForVersion expected baseline empty subtree");
  }

  size_t checked = 0;
  for (const auto& version : versions) {
    bool is_empty = false;
    error.clear();
    if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'e', 'm', 'p'}}, version, &is_empty, &error)) {
      Fail("version matrix IsEmptyTreeForVersion empty check failed version=" + version.ToString() +
           " error=" + error);
    }
    if (is_empty != baseline_empty) {
      Fail("version matrix IsEmptyTreeForVersion empty mismatch version=" + version.ToString());
    }
    std::cout << "VERSION_MATRIX API=IsEmptyTreeForVersion phase=empty version=" << version.ToString()
              << " status=PASS\n";
    ++checked;
  }

  if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'e', 'm', 'p'}}, {'k', '1'}, {'v', '1'}, &error)) {
    Fail("version matrix IsEmptyTreeForVersion non-empty setup failed: " + error);
  }

  bool baseline_non_empty = true;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'e', 'm', 'p'}},
                                grovedb::GroveVersion::V4_0_0(),
                                &baseline_non_empty,
                                &error)) {
    Fail("version matrix IsEmptyTreeForVersion baseline non-empty check failed: " + error);
  }
  if (baseline_non_empty) {
    Fail("version matrix IsEmptyTreeForVersion expected baseline non-empty subtree");
  }

  for (const auto& version : versions) {
    bool is_empty = true;
    error.clear();
    if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'e', 'm', 'p'}}, version, &is_empty, &error)) {
      Fail("version matrix IsEmptyTreeForVersion non-empty check failed version=" + version.ToString() +
           " error=" + error);
    }
    if (is_empty != baseline_non_empty) {
      Fail("version matrix IsEmptyTreeForVersion non-empty mismatch version=" + version.ToString());
    }
    std::cout << "VERSION_MATRIX API=IsEmptyTreeForVersion phase=non_empty version="
              << version.ToString() << " status=PASS\n";
    ++checked;
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("version matrix IsEmptyTreeForVersion start tx failed: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'t', 'x', 'e', 'm', 'p'}, &tx, &error)) {
    Fail("version matrix IsEmptyTreeForVersion tx empty setup failed: " + error);
  }
  bool tx_baseline_empty = false;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'t', 'x', 'e', 'm', 'p'}},
                                grovedb::GroveVersion::V4_0_0(),
                                &tx_baseline_empty,
                                &tx,
                                &error)) {
    Fail("version matrix IsEmptyTreeForVersion tx baseline empty check failed: " + error);
  }
  if (!tx_baseline_empty) {
    Fail("version matrix IsEmptyTreeForVersion expected tx baseline empty subtree");
  }

  for (const auto& version : versions) {
    bool is_empty = false;
    error.clear();
    if (!db.IsEmptyTreeForVersion(
            {{'r', 'o', 'o', 't'}, {'t', 'x', 'e', 'm', 'p'}}, version, &is_empty, &tx, &error)) {
      Fail("version matrix IsEmptyTreeForVersion tx empty check failed version=" + version.ToString() +
           " error=" + error);
    }
    if (is_empty != tx_baseline_empty) {
      Fail("version matrix IsEmptyTreeForVersion tx empty mismatch version=" + version.ToString());
    }
    std::cout << "VERSION_MATRIX API=IsEmptyTreeForVersion phase=tx_empty version="
              << version.ToString() << " status=PASS\n";
    ++checked;
  }

  if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'t', 'x', 'e', 'm', 'p'}},
                     {'t', 'k'},
                     {'t', 'v'},
                     &tx,
                     &error)) {
    Fail("version matrix IsEmptyTreeForVersion tx non-empty setup failed: " + error);
  }
  bool tx_baseline_non_empty = true;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}, {'t', 'x', 'e', 'm', 'p'}},
                                grovedb::GroveVersion::V4_0_0(),
                                &tx_baseline_non_empty,
                                &tx,
                                &error)) {
    Fail("version matrix IsEmptyTreeForVersion tx baseline non-empty check failed: " + error);
  }
  if (tx_baseline_non_empty) {
    Fail("version matrix IsEmptyTreeForVersion expected tx baseline non-empty subtree");
  }

  for (const auto& version : versions) {
    bool is_empty = true;
    error.clear();
    if (!db.IsEmptyTreeForVersion(
            {{'r', 'o', 'o', 't'}, {'t', 'x', 'e', 'm', 'p'}}, version, &is_empty, &tx, &error)) {
      Fail("version matrix IsEmptyTreeForVersion tx non-empty check failed version=" +
           version.ToString() + " error=" + error);
    }
    if (is_empty != tx_baseline_non_empty) {
      Fail("version matrix IsEmptyTreeForVersion tx non-empty mismatch version=" + version.ToString());
    }
    std::cout << "VERSION_MATRIX API=IsEmptyTreeForVersion phase=tx_non_empty version="
              << version.ToString() << " status=PASS\n";
    ++checked;
  }

  error.clear();
  if (!db.RollbackTransaction(&tx, &error)) {
    Fail("version matrix IsEmptyTreeForVersion rollback tx failed: " + error);
  }
  std::filesystem::remove_all(dir);
  return checked;
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  const std::string dir = MakeTempDir("version_matrix");
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_grovedb_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb proof writer");
  }

  struct FixtureCase {
    std::string name;
    std::vector<uint8_t> proof;
    grovedb::PathQuery query;
  };

  std::vector<FixtureCase> cases;
  cases.push_back(FixtureCase{
      "grove_proof_present.bin",
      ReadFile(std::filesystem::path(dir) / "grove_proof_present.bin"),
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'}),
  });
  cases.push_back(FixtureCase{
      "grove_proof_absent.bin",
      ReadFile(std::filesystem::path(dir) / "grove_proof_absent.bin"),
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '0'}),
  });

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };

  size_t checked = 0;
  for (const auto& fixture : cases) {
    std::vector<uint8_t> expected_root;
    std::vector<grovedb::VerifiedPathKeyElement> expected_elements;
    std::string error;
    if (!grovedb::VerifyPathQueryProof(
            fixture.proof, fixture.query, &expected_root, &expected_elements, &error)) {
      Fail("direct verify failed fixture=" + fixture.name + " error=" + error);
    }
    for (const auto& version : versions) {
      VerifyFixtureForVersion(fixture.name,
                              fixture.proof,
                              fixture.query,
                              version,
                              expected_root,
                              expected_elements);
      ++checked;
    }
  }

  checked += AssertIsEmptyTreeForVersionMatrix(versions);

  std::cout << "VERSION_MATRIX_SUMMARY checked=" << checked << " status=PASS\n";
  std::filesystem::remove_all(dir);
  return 0;
}
