#include "element.h"
#include "grovedb.h"
#include "proof.h"
#include "test_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {

const grovedb::VerifiedPathKeyElement* FindElementByPathAndKey(
    const std::vector<grovedb::VerifiedPathKeyElement>& elements,
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& key) {
  for (const auto& element : elements) {
    if (element.path == path && element.key == key) {
      return &element;
    }
  }
  return nullptr;
}

const grovedb::GroveLayerProof* FindLowerLayer(const grovedb::GroveLayerProof& layer,
                                               const std::vector<uint8_t>& key) {
  for (const auto& entry : layer.lower_layers) {
    if (entry.first == key) {
      return entry.second.get();
    }
  }
  return nullptr;
}

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

std::string HexPrefix(const std::vector<uint8_t>& bytes, size_t max_len = 16) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  const size_t limit = std::min(bytes.size(), max_len);
  out.reserve(limit * 2);
  for (size_t i = 0; i < limit; ++i) {
    const uint8_t b = bytes[i];
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

std::string ElementsDebugString(
    const std::vector<grovedb::VerifiedPathKeyElement>& elements) {
  auto to_string = [](const std::vector<uint8_t>& bytes) -> std::string {
    return std::string(bytes.begin(), bytes.end());
  };
  std::string out;
  for (const auto& element : elements) {
    out += "[path=";
    for (size_t i = 0; i < element.path.size(); ++i) {
      if (i != 0) {
        out += "/";
      }
      out += to_string(element.path[i]);
    }
    out += ",key=" + to_string(element.key) + "]";
  }
  return out;
}


void AssertDecodedLayerStructure(const std::vector<uint8_t>& proof,
                                 bool expect_branch_subpath) {
  grovedb::GroveLayerProof root_layer;
  std::string error;
  if (!grovedb::DecodeGroveDbProof(proof, &root_layer, &error)) {
    Fail("decode grove proof failed: " + error);
  }
  const auto* root_subtree = FindLowerLayer(root_layer, {'r', 'o', 'o', 't'});
  if (root_subtree == nullptr) {
    Fail("decoded proof missing root subtree layer");
  }
  const auto* key_layer = FindLowerLayer(*root_subtree, {'k', 'e', 'y'});
  if (key_layer == nullptr) {
    Fail("decoded proof missing key subtree layer");
  }
  const auto* branch_layer = FindLowerLayer(*key_layer, {'b', 'r', 'a', 'n', 'c', 'h'});
  if (expect_branch_subpath && branch_layer == nullptr) {
    Fail("decoded proof missing expected branch subpath layer");
  }
}

void AssertPresentProof(const std::vector<uint8_t>& proof,
                       const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("present path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("present path query root hash mismatch");
  }
  if (elements.size() != 1) {
    Fail("present path query expected one element");
  }
  const auto& first = elements.front();
  if (!first.has_element || first.key != std::vector<uint8_t>({'k', '2'})) {
    Fail("present path query returned unexpected key element");
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(first.element_bytes, &item, &error)) {
    Fail("present path query element decode failed: " + error);
  }
  if (item.value != std::vector<uint8_t>({'v', '2'})) {
    Fail("present path query decoded value mismatch");
  }
}

void AssertAggregateTreeProof(const std::vector<uint8_t>& proof,
                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key(
      {'s', 'u', 'm', '_', 't', 'r', 'e', 'e'}));
  query.items.push_back(grovedb::QueryItem::Key(
      {'c', 'o', 'u', 'n', 't', '_', 't', 'r', 'e', 'e'}));
  query.items.push_back(grovedb::QueryItem::Key(
      {'b', 'i', 'g', '_', 's', 'u', 'm', '_', 't', 'r', 'e', 'e'}));
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {}, grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("aggregate tree proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("aggregate tree proof root hash mismatch");
  }
  const auto* sum_tree = FindElementByPathAndKey(elements, {}, {'s', 'u', 'm', '_', 't', 'r', 'e', 'e'});
  const auto* count_tree = FindElementByPathAndKey(
      elements, {}, {'c', 'o', 'u', 'n', 't', '_', 't', 'r', 'e', 'e'});
  const auto* big_sum_tree = FindElementByPathAndKey(
      elements, {}, {'b', 'i', 'g', '_', 's', 'u', 'm', '_', 't', 'r', 'e', 'e'});
  if (sum_tree == nullptr || count_tree == nullptr || big_sum_tree == nullptr) {
    Fail("aggregate tree proof missing expected keys: " + ElementsDebugString(elements));
  }
  if (!sum_tree->has_element || !count_tree->has_element || !big_sum_tree->has_element) {
    Fail("aggregate tree proof expected present elements");
  }

  grovedb::ElementSumTree decoded_sum_tree;
  if (!grovedb::DecodeSumTreeFromElementBytes(sum_tree->element_bytes, &decoded_sum_tree, &error)) {
    Fail("aggregate tree proof sum-tree decode failed: " + error);
  }
  if (decoded_sum_tree.sum != 7) {
    Fail("aggregate tree proof sum-tree aggregate mismatch");
  }

  grovedb::ElementCountTree decoded_count_tree;
  if (!grovedb::DecodeCountTreeFromElementBytes(
          count_tree->element_bytes, &decoded_count_tree, &error)) {
    Fail("aggregate tree proof count-tree decode failed: " + error);
  }
  if (decoded_count_tree.count != 2) {
    Fail("aggregate tree proof count-tree aggregate mismatch");
  }

  grovedb::ElementBigSumTree decoded_big_sum_tree;
  if (!grovedb::DecodeBigSumTreeFromElementBytes(
          big_sum_tree->element_bytes, &decoded_big_sum_tree, &error)) {
    Fail("aggregate tree proof big-sum-tree decode failed: " + error);
  }
  if (decoded_big_sum_tree.sum != 0) {
    Fail("aggregate tree proof big-sum-tree aggregate mismatch");
  }
}

void AssertAbsenceProof(const std::vector<uint8_t>& proof,
                        const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '0'});
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("absence path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("absence path query root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("absence path query expected no elements");
  }
}

void AssertProveQueryForVersionParity(const std::filesystem::path& fixture_dir,
                                      const std::vector<uint8_t>& rust_proof,
                                      const std::vector<uint8_t>& expected_root_hash) {
  const std::filesystem::path cpp_fixture_path =
      fixture_dir / "cpp_prove_query_for_version_fixture_db";
  std::filesystem::remove_all(cpp_fixture_path);
  std::filesystem::create_directories(cpp_fixture_path);

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_fixture_path.string(), &error)) {
    Fail("failed to open C++ prove-query fixture db: " + error);
  }
  std::vector<uint8_t> item_element;
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_element, &error)) {
    Fail("failed to encode C++ prove-query fixture element: " + error);
  }
  if (!db.Insert({}, {'k', '1'}, item_element, &error)) {
    Fail("failed to insert C++ prove-query fixture element: " + error);
  }

  grovedb::PathQuery query = grovedb::PathQuery::NewSingleKey({}, {'k', '1'});
  std::vector<uint8_t> baseline_root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> baseline_elements;
  if (!grovedb::VerifyPathQueryProof(
          rust_proof, query, &baseline_root_hash, &baseline_elements, &error)) {
    Fail("failed to verify rust fixture proof baseline for ProveQueryForVersion parity: " + error);
  }

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };

  for (const auto& version : versions) {
    std::vector<uint8_t> rust_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> rust_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            rust_proof, query, version, &rust_root_hash, &rust_elements, &error)) {
      Fail("failed to verify rust fixture proof for ProveQueryForVersion parity at version " +
           version.ToString() + ": " + error);
    }

    if (rust_root_hash != baseline_root_hash ||
        !SameVerifiedElements(rust_elements, baseline_elements)) {
      Fail("version-gated rust fixture verification diverged from baseline at version " +
           version.ToString());
    }
    if (rust_root_hash != expected_root_hash) {
      Fail("rust fixture proof root hash mismatches expected root hash at version " +
           version.ToString());
    }

    std::vector<uint8_t> cpp_proof;
    if (!db.ProveQueryForVersion(query, version, &cpp_proof, &error)) {
      Fail("C++ ProveQueryForVersion failed on rust fixture db at version " +
           version.ToString() + ": " + error);
    }
    if (cpp_proof.empty()) {
      Fail("C++ ProveQueryForVersion produced empty proof at version " + version.ToString());
    }
    if (cpp_proof != rust_proof) {
      Fail("C++ ProveQueryForVersion proof bytes mismatch rust fixture at version " +
           version.ToString() + " (cpp_size=" + std::to_string(cpp_proof.size()) +
           ", rust_size=" + std::to_string(rust_proof.size()) +
           ", cpp_prefix=" + HexPrefix(cpp_proof) +
           ", rust_prefix=" + HexPrefix(rust_proof) + ")");
    }

    std::vector<uint8_t> cpp_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> cpp_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            cpp_proof, query, version, &cpp_root_hash, &cpp_elements, &error)) {
      Fail("failed to verify C++ generated proof for ProveQueryForVersion parity at version " +
           version.ToString() + ": " + error);
    }

    if (cpp_root_hash != rust_root_hash ||
        !SameVerifiedElements(cpp_elements, rust_elements)) {
      Fail("C++ generated proof verification mismatches Rust fixture proof verification at version " +
           version.ToString());
    }
    if (cpp_elements.size() != 1) {
      Fail("ProveQueryForVersion parity expected exactly one verified element at version " +
           version.ToString());
    }
    if (cpp_elements.front().key != std::vector<uint8_t>({'k', '1'}) ||
        !cpp_elements.front().has_element || cpp_elements.front().element_bytes != item_element) {
      Fail("C++ generated proof verified result mismatches expected fixture element at version " +
           version.ToString());
    }
  }
}

void AssertProveQueryForVersionLayeredParity(const std::filesystem::path& fixture_dir,
                                             const std::vector<uint8_t>& rust_proof,
                                             const std::vector<uint8_t>& expected_root_hash) {
  const std::filesystem::path cpp_fixture_path =
      fixture_dir / "cpp_prove_query_for_version_layered_fixture_db";
  std::filesystem::remove_all(cpp_fixture_path);
  std::filesystem::create_directories(cpp_fixture_path);

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_fixture_path.string(), &error)) {
    Fail("failed to open C++ layered prove-query fixture db: " + error);
  }

  std::vector<uint8_t> empty_tree;
  if (!grovedb::EncodeTreeToElementBytes(&empty_tree, &error)) {
    Fail("failed to encode layered fixture tree element: " + error);
  }
  std::vector<uint8_t> item_x;
  if (!grovedb::EncodeItemToElementBytes({'9'}, &item_x, &error)) {
    Fail("failed to encode layered fixture x element: " + error);
  }
  std::vector<uint8_t> item_y;
  if (!grovedb::EncodeItemToElementBytes({'8'}, &item_y, &error)) {
    Fail("failed to encode layered fixture y element: " + error);
  }
  if (!db.Insert({}, {'r', 'o', 'o', 't'}, empty_tree, &error)) {
    Fail("failed to insert layered fixture root tree: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'}, empty_tree, &error)) {
    Fail("failed to insert layered fixture key tree: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}},
                 {'b', 'r', 'a', 'n', 'c', 'h'},
                 empty_tree,
                 &error)) {
    Fail("failed to insert layered fixture branch tree: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'x'},
                 item_x,
                 &error)) {
    Fail("failed to insert layered fixture x item: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'y'},
                 item_y,
                 &error)) {
    Fail("failed to insert layered fixture y item: " + error);
  }

  grovedb::Query query_root = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  query_root.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>({{'b', 'r', 'a', 'n', 'c', 'h'}});
  query_root.default_subquery_branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleKey({'x'}));
  grovedb::PathQuery query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(query_root, std::nullopt, std::nullopt));

  std::vector<uint8_t> baseline_root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> baseline_elements;
  if (!grovedb::VerifyPathQueryProof(
          rust_proof, query, &baseline_root_hash, &baseline_elements, &error)) {
    Fail("failed to verify rust layered fixture proof baseline: " + error);
  }
  if (baseline_root_hash != expected_root_hash) {
    Fail("rust layered fixture root hash mismatches expected root hash");
  }

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };

  for (const auto& version : versions) {
    std::vector<uint8_t> rust_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> rust_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            rust_proof, query, version, &rust_root_hash, &rust_elements, &error)) {
      Fail("failed to verify rust layered fixture proof for version " +
           version.ToString() + ": " + error);
    }
    if (rust_root_hash != baseline_root_hash ||
        !SameVerifiedElements(rust_elements, baseline_elements)) {
      Fail("version-gated rust layered fixture verification diverged from baseline at version " +
           version.ToString());
    }

    std::vector<uint8_t> cpp_proof;
    if (!db.ProveQueryForVersion(query, version, &cpp_proof, &error)) {
      Fail("C++ layered ProveQueryForVersion failed at version " +
           version.ToString() + ": " + error);
    }
    if (cpp_proof.empty()) {
      Fail("C++ layered ProveQueryForVersion produced empty proof at version " +
           version.ToString());
    }
    std::vector<uint8_t> cpp_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> cpp_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            cpp_proof, query, version, &cpp_root_hash, &cpp_elements, &error)) {
      Fail("failed to verify C++ layered generated proof for version " +
           version.ToString() + ": " + error);
    }
    if (!SameVerifiedElements(cpp_elements, rust_elements)) {
      Fail("C++ layered generated proof verification mismatches Rust elements at version " +
           version.ToString() + " (cpp_root=" + HexPrefix(cpp_root_hash, 32) +
           ", rust_root=" + HexPrefix(rust_root_hash, 32) +
           ", cpp_elements=" + ElementsDebugString(cpp_elements) +
           ", rust_elements=" + ElementsDebugString(rust_elements) + ")");
    }
    if (cpp_root_hash.size() != 32) {
      Fail("C++ layered generated proof should return 32-byte root hash at version " +
           version.ToString());
    }
    const auto* x = FindElementByPathAndKey(
        cpp_elements,
        {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
        {'x'});
    if (x == nullptr || !x->has_element || x->element_bytes != item_x) {
      Fail("C++ layered generated proof verified result shape mismatch at version " +
           version.ToString());
    }
    if (cpp_proof != rust_proof) {
      Fail("layered ProveQueryForVersion byte mismatch at version " + version.ToString());
    }
    grovedb::GroveLayerProof cpp_layer;
    grovedb::GroveLayerProof rust_layer;
    std::string decode_err;
    if (!grovedb::DecodeGroveDbProof(cpp_proof, &cpp_layer, &decode_err)) {
      Fail("failed to decode C++ layered proof at version " + version.ToString() +
           ": " + decode_err);
    }
    if (!grovedb::DecodeGroveDbProof(rust_proof, &rust_layer, &decode_err)) {
      Fail("failed to decode Rust layered proof at version " + version.ToString() +
           ": " + decode_err);
    }

    const auto* cpp_root_layer = FindLowerLayer(cpp_layer, {'r', 'o', 'o', 't'});
    const auto* rust_root_layer = FindLowerLayer(rust_layer, {'r', 'o', 'o', 't'});
    if (cpp_root_layer == nullptr || rust_root_layer == nullptr) {
      Fail("missing root lower layer in layered proof decode at version " + version.ToString());
    }
    const auto* cpp_key_layer = FindLowerLayer(*cpp_root_layer, {'k', 'e', 'y'});
    const auto* rust_key_layer = FindLowerLayer(*rust_root_layer, {'k', 'e', 'y'});
    if (cpp_key_layer == nullptr || rust_key_layer == nullptr) {
      Fail("missing key lower layer in layered proof decode at version " + version.ToString());
    }
    const auto* cpp_branch_layer =
        FindLowerLayer(*cpp_key_layer, {'b', 'r', 'a', 'n', 'c', 'h'});
    const auto* rust_branch_layer =
        FindLowerLayer(*rust_key_layer, {'b', 'r', 'a', 'n', 'c', 'h'});
    if (cpp_branch_layer == nullptr || rust_branch_layer == nullptr) {
      Fail("missing branch lower layer in layered proof decode at version " + version.ToString());
    }

    if (cpp_layer.merk_proof != rust_layer.merk_proof ||
        cpp_root_layer->merk_proof != rust_root_layer->merk_proof ||
        cpp_key_layer->merk_proof != rust_key_layer->merk_proof ||
        cpp_branch_layer->merk_proof != rust_branch_layer->merk_proof) {
      Fail("layered ProveQueryForVersion decoded layer mismatch at version " +
           version.ToString());
    }
  }
}

void AssertProveQueryForVersionLimitedRangeParity(const std::filesystem::path& fixture_dir,
                                                  const std::vector<uint8_t>& rust_proof,
                                                  const std::vector<uint8_t>& expected_root_hash) {
  const std::filesystem::path cpp_fixture_path =
      fixture_dir / "cpp_prove_query_for_version_limit_fixture_db";
  std::filesystem::remove_all(cpp_fixture_path);
  std::filesystem::create_directories(cpp_fixture_path);

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_fixture_path.string(), &error)) {
    Fail("failed to open C++ limited-range prove-query fixture db: " + error);
  }
  std::vector<uint8_t> item_a;
  std::vector<uint8_t> item_b;
  std::vector<uint8_t> item_c;
  if (!grovedb::EncodeItemToElementBytes({'v', 'a'}, &item_a, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', 'b'}, &item_b, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', 'c'}, &item_c, &error)) {
    Fail("failed to encode limited-range fixture elements: " + error);
  }
  if (!db.Insert({}, {'a'}, item_a, &error) || !db.Insert({}, {'b'}, item_b, &error) ||
      !db.Insert({}, {'c'}, item_c, &error)) {
    Fail("failed to insert limited-range fixture items: " + error);
  }

  grovedb::PathQuery query = grovedb::PathQuery::New(
      {}, grovedb::SizedQuery::New(
              grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeInclusive({'a'}, {'c'})),
              2,
              std::nullopt));

  std::vector<uint8_t> baseline_root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> baseline_elements;
  if (!grovedb::VerifyPathQueryProof(
          rust_proof, query, &baseline_root_hash, &baseline_elements, &error)) {
    Fail("failed to verify rust limited-range fixture proof baseline: " + error);
  }
  if (baseline_root_hash != expected_root_hash) {
    Fail("rust limited-range fixture root hash mismatches expected root hash");
  }

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(), grovedb::GroveVersion{4, 1, 0}, grovedb::GroveVersion{5, 0, 0}};
  for (const auto& version : versions) {
    std::vector<uint8_t> cpp_proof;
    if (!db.ProveQueryForVersion(query, version, &cpp_proof, &error)) {
      Fail("C++ limited-range ProveQueryForVersion failed at version " + version.ToString() +
           ": " + error);
    }
    if (cpp_proof != rust_proof) {
      Fail("limited-range ProveQueryForVersion byte mismatch at version " + version.ToString() +
           " (cpp_size=" + std::to_string(cpp_proof.size()) +
           ", rust_size=" + std::to_string(rust_proof.size()) + ")");
    }
    std::vector<uint8_t> cpp_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> cpp_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            cpp_proof, query, version, &cpp_root_hash, &cpp_elements, &error)) {
      Fail("failed to verify C++ limited-range generated proof at version " + version.ToString() +
           ": " + error);
    }
    if (cpp_root_hash != expected_root_hash) {
      Fail("C++ limited-range generated proof root hash mismatch at version " + version.ToString());
    }
    std::vector<uint8_t> rust_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> rust_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            rust_proof, query, version, &rust_root_hash, &rust_elements, &error)) {
      Fail("failed to verify rust limited-range fixture proof at version " + version.ToString() +
           ": " + error);
    }
    if (cpp_root_hash != rust_root_hash || !SameVerifiedElements(cpp_elements, rust_elements)) {
      Fail("limited-range proof verification mismatch at version " + version.ToString());
    }
  }
}

void AssertProveQueryAbsenceTerminalRangeParity(const std::filesystem::path& fixture_dir,
                                                const std::vector<uint8_t>& rust_proof,
                                                const std::vector<uint8_t>& expected_root_hash) {
  const std::filesystem::path cpp_fixture_path = fixture_dir / "cpp_absence_terminal_range_fixture_db";
  std::filesystem::remove_all(cpp_fixture_path);
  std::filesystem::create_directories(cpp_fixture_path);

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_fixture_path.string(), &error)) {
    Fail("failed to open C++ absence-terminal-range fixture db: " + error);
  }
  std::vector<uint8_t> item_a;
  std::vector<uint8_t> item_c;
  if (!grovedb::EncodeItemToElementBytes({'v', 'a'}, &item_a, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', 'c'}, &item_c, &error)) {
    Fail("failed to encode absence-terminal-range fixture elements: " + error);
  }
  if (!db.Insert({}, {'a'}, item_a, &error) || !db.Insert({}, {'c'}, item_c, &error)) {
    Fail("failed to insert absence-terminal-range fixture items: " + error);
  }

  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleQueryItem({}, grovedb::QueryItem::RangeInclusive({'a'}, {'c'}));

  std::vector<uint8_t> cpp_proof;
  if (!db.ProveQuery(query, &cpp_proof, &error)) {
    Fail("C++ ProveQuery absence-terminal-range failed: " + error);
  }
  if (cpp_proof != rust_proof) {
    Fail("absence-terminal-range ProveQuery byte mismatch (cpp_size=" +
         std::to_string(cpp_proof.size()) + ", rust_size=" + std::to_string(rust_proof.size()) +
         ")");
  }

  std::vector<uint8_t> cpp_root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> cpp_elements;
  if (!grovedb::VerifyPathQueryProof(cpp_proof, query, &cpp_root_hash, &cpp_elements, &error)) {
    Fail("failed to verify C++ absence-terminal-range generated proof: " + error);
  }
  if (cpp_root_hash != expected_root_hash) {
    Fail("C++ absence-terminal-range generated proof root hash mismatch");
  }
}

void AssertRootRangeAbsenceProveQueryParityOnRustFixture(const std::filesystem::path& fixture_dir,
                                                         const std::vector<uint8_t>& rust_proof,
                                                         const grovedb::QueryItem& item,
                                                         const std::string& label) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(fixture_dir.string(), &error)) {
    Fail(label + " failed to open rust root proof fixture db: " + error);
  }
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'}}, item);
  std::vector<uint8_t> cpp_proof;
  if (!db.ProveQuery(query, &cpp_proof, &error)) {
    Fail(label + " C++ ProveQuery failed on rust fixture db: " + error);
  }
  if (cpp_proof != rust_proof) {
    Fail(label + " ProveQuery byte mismatch on rust fixture db (cpp_size=" +
         std::to_string(cpp_proof.size()) + ", rust_size=" + std::to_string(rust_proof.size()) + ")");
  }
}

void AssertRootRangeProveQueryForVersionParityOnRustFixture(
    const std::filesystem::path& fixture_dir,
    const std::vector<uint8_t>& rust_proof,
    const grovedb::QueryItem& item,
    const std::string& label) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(fixture_dir.string(), &error)) {
    Fail(label + " failed to open rust root proof fixture db: " + error);
  }
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'}}, item);

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };
  for (const auto& version : versions) {
    std::vector<uint8_t> cpp_proof;
    if (!db.ProveQueryForVersion(query, version, &cpp_proof, &error)) {
      Fail(label + " ProveQueryForVersion failed on rust fixture db at version " +
           version.ToString() + ": " + error);
    }
    if (cpp_proof != rust_proof) {
      Fail(label + " ProveQueryForVersion byte mismatch on rust fixture db at version " +
           version.ToString() + " (cpp_size=" + std::to_string(cpp_proof.size()) +
           ", rust_size=" + std::to_string(rust_proof.size()) + ")");
    }
  }
}

void AssertPathQueryProveQueryParityOnRustFixture(const std::filesystem::path& fixture_dir,
                                                  const std::vector<uint8_t>& rust_proof,
                                                  const grovedb::PathQuery& path_query,
                                                  const std::string& label) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(fixture_dir.string(), &error)) {
    Fail(label + " failed to open rust fixture db: " + error);
  }
  std::vector<uint8_t> cpp_proof;
  if (!db.ProveQuery(path_query, &cpp_proof, &error)) {
    Fail(label + " C++ ProveQuery failed on rust fixture db: " + error);
  }
  if (cpp_proof != rust_proof) {
    Fail(label + " ProveQuery byte mismatch on rust fixture db (cpp_size=" +
         std::to_string(cpp_proof.size()) + ", rust_size=" + std::to_string(rust_proof.size()) + ")");
  }
}

void AssertPathQueryProveQueryForVersionParityOnRustFixture(
    const std::filesystem::path& fixture_dir,
    const std::vector<uint8_t>& rust_proof,
    const grovedb::PathQuery& path_query,
    const std::string& label) {
  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(fixture_dir.string(), &error)) {
    Fail(label + " failed to open rust fixture db: " + error);
  }
  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };
  for (const auto& version : versions) {
    std::vector<uint8_t> cpp_proof;
    if (!db.ProveQueryForVersion(path_query, version, &cpp_proof, &error)) {
      Fail(label + " ProveQueryForVersion failed on rust fixture db at version " +
           version.ToString() + ": " + error);
    }
    if (cpp_proof != rust_proof) {
      Fail(label + " ProveQueryForVersion byte mismatch on rust fixture db at version " +
           version.ToString() + " (cpp_size=" + std::to_string(cpp_proof.size()) +
           ", rust_size=" + std::to_string(rust_proof.size()) + ")");
    }
  }
}

grovedb::PathQuery MakeRootKeyBranchSubqueryPathQuery(const grovedb::QueryItem& branch_item) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(branch_item);
  query.default_subquery_branch.subquery = std::move(branch_query);
  return grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
}

grovedb::PathQuery MakeRootKeyBranchSubqueryPathQueryWithLimit(
    const grovedb::QueryItem& branch_item,
    uint16_t limit) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(branch_item);
  query.default_subquery_branch.subquery = std::move(branch_query);
  return grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), limit, std::nullopt));
}

void AssertRootRangeAbsenceProof(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash,
                                 const grovedb::QueryItem& item,
                                 const std::string& label) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'}}, item);
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail(label + " verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail(label + " root hash mismatch");
  }
  if (!elements.empty()) {
    Fail(label + " expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertRootRangeProof(const std::vector<uint8_t>& proof,
                          const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}},
      grovedb::QueryItem::RangeInclusive({'k', '1'}, {'k', '2'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range path query root hash mismatch");
  }
  const auto* k1 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  const auto* key_tree =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'});
  if (k1 == nullptr || k2 == nullptr || key_tree != nullptr || !k1->has_element ||
      !k2->has_element) {
    Fail("root range path query result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(k1->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '1'})) {
    Fail("root range path query decoded value mismatch for k1");
  }
  if (!grovedb::DecodeItemFromElementBytes(k2->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '2'})) {
    Fail("root range path query decoded value mismatch for k2");
  }
  if (elements.size() != 2) {
    Fail("root range path query expected exactly 2 elements");
  }
}

void AssertRootRangeDescProof(const std::vector<uint8_t>& proof,
                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query =
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeInclusive({'k', '1'}, {'k', '2'}));
  query.left_to_right = false;
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("root descending range path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root descending range path query root hash mismatch");
  }
  for (const auto& element : elements) {
    if (element.key != std::vector<uint8_t>({'k', '1'}) &&
        element.key != std::vector<uint8_t>({'k', '2'})) {
      Fail("root descending range path query contained out-of-range key: " +
           ElementsDebugString(elements));
    }
  }
  if (elements.size() >= 2 &&
      elements[0].key == std::vector<uint8_t>({'k', '1'}) &&
      elements[1].key == std::vector<uint8_t>({'k', '2'})) {
    Fail("root descending range path query order mismatch: " + ElementsDebugString(elements));
  }
  for (const auto& element : elements) {
    if (!element.has_element) {
      Fail("root descending range path query expected materialized elements");
    }
  }
}

void AssertRootRangeExclusiveProof(const std::vector<uint8_t>& proof,
                                   const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::Range({'k', '1'}, {'k', '3'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root exclusive-range path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root exclusive-range path query root hash mismatch");
  }
  const std::vector<uint8_t> k1 = {'k', '1'};
  const std::vector<uint8_t> k3 = {'k', '3'};
  bool saw_k1 = false;
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root exclusive-range path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root exclusive-range expected materialized elements");
    }
    if (!(k1 <= element.key && element.key < k3)) {
      Fail("root exclusive-range returned out-of-range key: " + ElementsDebugString(elements));
    }
    if (element.key == k1) {
      saw_k1 = true;
    }
    if (element.key == std::vector<uint8_t>({'k', '2'})) {
      saw_k2 = true;
    }
  }
  if (!saw_k1 || !saw_k2) {
    Fail("root exclusive-range expected keys k1 and k2");
  }
}

void AssertRootRangeToInclusiveProof(const std::vector<uint8_t>& proof,
                                     const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeToInclusive({'k', '2'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-to-inclusive path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-to-inclusive path query root hash mismatch");
  }
  const std::vector<uint8_t> k1 = {'k', '1'};
  const std::vector<uint8_t> k2 = {'k', '2'};
  bool saw_k1 = false;
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-to-inclusive path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-to-inclusive expected materialized elements");
    }
    if (element.key != k1 && element.key != k2) {
      Fail("root range-to-inclusive out-of-range key: " + ElementsDebugString(elements));
    }
    if (element.key == k1) {
      saw_k1 = true;
    }
    if (element.key == k2) {
      saw_k2 = true;
    }
  }
  if (!saw_k1 || !saw_k2) {
    Fail("root range-to-inclusive expected both k1 and k2");
  }
}

void AssertRootRangeAfterProof(const std::vector<uint8_t>& proof,
                               const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeAfter({'k', '1'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-after path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-after path query root hash mismatch");
  }
  const std::vector<uint8_t> k1 = {'k', '1'};
  bool saw_any = false;
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-after path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-after expected materialized elements");
    }
    if (!(k1 < element.key)) {
      Fail("root range-after returned key not strictly after k1: " + ElementsDebugString(elements));
    }
    saw_any = true;
    if (element.key == std::vector<uint8_t>({'k', '2'})) {
      saw_k2 = true;
    }
  }
  if (!saw_any || !saw_k2) {
    Fail("root range-after expected at least key k2");
  }
}

void AssertRootRangeToProof(const std::vector<uint8_t>& proof,
                            const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeTo({'k', '2'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-to path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-to path query root hash mismatch");
  }
  const std::vector<uint8_t> k2 = {'k', '2'};
  bool saw_k1 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-to path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-to expected materialized elements");
    }
    if (!(element.key < k2)) {
      Fail("root range-to returned key not strictly before k2: " + ElementsDebugString(elements));
    }
    if (element.key == std::vector<uint8_t>({'k', '1'})) {
      saw_k1 = true;
    }
  }
  if (!saw_k1) {
    Fail("root range-to expected key k1");
  }
}

void AssertRootRangeFromProof(const std::vector<uint8_t>& proof,
                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeFrom({'k', '2'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-from path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-from path query root hash mismatch");
  }
  const std::vector<uint8_t> k2 = {'k', '2'};
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-from path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-from expected materialized elements");
    }
    if (element.key < k2) {
      Fail("root range-from returned key below k2: " + ElementsDebugString(elements));
    }
    if (element.key == std::vector<uint8_t>({'k', '2'})) {
      saw_k2 = true;
    }
  }
  if (!saw_k2) {
    Fail("root range-from expected key k2");
  }
}

void AssertRootRangeAfterToProof(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeAfterTo({'k', '1'}, {'k', '3'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-after-to path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-after-to path query root hash mismatch");
  }
  const std::vector<uint8_t> k1 = {'k', '1'};
  const std::vector<uint8_t> k3 = {'k', '3'};
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-after-to path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-after-to expected materialized elements");
    }
    if (!(k1 < element.key && element.key < k3)) {
      Fail("root range-after-to returned out-of-range key: " + ElementsDebugString(elements));
    }
    if (element.key == std::vector<uint8_t>({'k', '2'})) {
      saw_k2 = true;
    }
  }
  if (!saw_k2) {
    Fail("root range-after-to expected key k2");
  }
}

void AssertRootRangeAfterToInclusiveProof(const std::vector<uint8_t>& proof,
                                          const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}},
      grovedb::QueryItem::RangeAfterToInclusive({'k', '1'}, {'k', '3'}));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-after-to-inclusive path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-after-to-inclusive path query root hash mismatch");
  }
  const std::vector<uint8_t> k1 = {'k', '1'};
  const std::vector<uint8_t> k3 = {'k', '3'};
  bool saw_k2 = false;
  for (const auto& element : elements) {
    if (element.path != std::vector<std::vector<uint8_t>>{{'r', 'o', 'o', 't'}}) {
      Fail("root range-after-to-inclusive path mismatch: " + ElementsDebugString(elements));
    }
    if (!element.has_element) {
      Fail("root range-after-to-inclusive expected materialized elements");
    }
    if (!(k1 < element.key && element.key <= k3)) {
      Fail("root range-after-to-inclusive returned out-of-range key: " +
           ElementsDebugString(elements));
    }
    if (element.key == std::vector<uint8_t>({'k', '2'})) {
      saw_k2 = true;
    }
  }
  if (!saw_k2) {
    Fail("root range-after-to-inclusive expected key k2");
  }
}

void AssertRootRangeFullProof(const std::vector<uint8_t>& proof,
                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeFull());
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("root range-full path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root range-full path query root hash mismatch");
  }
  const auto* k1 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  const auto* key_tree = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'});
  if (k1 == nullptr || k2 == nullptr || key_tree != nullptr || !k1->has_element || !k2->has_element) {
    Fail("root range-full path query result shape mismatch: " + ElementsDebugString(elements));
  }
  if (elements.size() != 2) {
    Fail("root range-full path query expected exactly 2 elements");
  }
}

void AssertRootMultiKeyProof(const std::vector<uint8_t>& proof,
                             const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', '1'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  grovedb::SizedQuery sized = grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt);
  grovedb::PathQuery path_query = grovedb::PathQuery::New({{'r', 'o', 'o', 't'}}, std::move(sized));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("root multi-key path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("root multi-key path query root hash mismatch");
  }
  if (elements.size() != 2) {
    std::cerr << "root multi-key: expected 2 elements, got " << elements.size()
              << ": " << ElementsDebugString(elements) << "\n";
    Fail("root multi-key expected exactly 2 elements, got " + std::to_string(elements.size()));
  }
  const auto* k1 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k1 == nullptr || k2 == nullptr || !k1->has_element || !k2->has_element) {
    Fail("root multi-key path query result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(k1->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '1'})) {
    Fail("root multi-key decoded value mismatch for k1");
  }
  if (!grovedb::DecodeItemFromElementBytes(k2->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '2'})) {
    Fail("root multi-key decoded value mismatch for k2");
  }
}

void AssertNestedPathRangeFullProof(const std::vector<uint8_t>& proof,
                                    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      grovedb::QueryItem::RangeFull());
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("nested-path range-full path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("nested-path range-full path query root hash mismatch");
  }
  const auto* x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (x == nullptr || y == nullptr || !x->has_element || !y->has_element || elements.size() != 2) {
    Fail("nested-path range-full path query result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(x->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'9'})) {
    Fail("nested-path range-full decoded x value mismatch");
  }
  if (!grovedb::DecodeItemFromElementBytes(y->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'8'})) {
    Fail("nested-path range-full decoded y value mismatch");
  }
}

void AssertSubqueryParentBehavior(const std::vector<uint8_t>& proof,
                                  bool add_parent,
                                  const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query root_query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  root_query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  root_query.add_parent_tree_on_subquery = add_parent;
  grovedb::PathQuery query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(root_query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("subquery path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path query root hash mismatch");
  }

  const auto* child_a =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}}, {'a'});
  const auto* child_b =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}}, {'b'});
  if (child_a == nullptr || child_b == nullptr) {
    Fail("subquery path query missing child keys: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(child_a->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'1'})) {
    Fail("subquery child a decode mismatch");
  }
  if (!grovedb::DecodeItemFromElementBytes(child_b->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'2'})) {
    Fail("subquery child b decode mismatch");
  }

  const auto* parent =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'});
  if (add_parent) {
    if (parent == nullptr) {
      Fail("subquery with add_parent should include parent tree: " +
           ElementsDebugString(elements));
    }
    if (parent->has_element) {
      uint64_t variant = 0;
      if (!grovedb::DecodeElementVariant(parent->element_bytes, &variant, &error) ||
          variant != 2) {
        Fail("subquery parent tree variant mismatch");
      }
    }
    if (elements.size() != 3) {
      Fail("subquery with add_parent should yield exactly 3 elements");
    }
  } else {
    if (parent != nullptr) {
      Fail("subquery without add_parent should not include parent tree");
    }
    if (elements.size() != 2) {
      Fail("subquery without add_parent should yield exactly 2 elements");
    }
  }
}

void AssertConditionalSubqueryOverride(const std::vector<uint8_t>& proof,
                                       const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', '2'}), grovedb::SubqueryBranch()});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery root hash mismatch");
  }
  const auto* child_a =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}}, {'a'});
  const auto* child_b =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}}, {'b'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_a == nullptr || child_b == nullptr || k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(k2->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '2'})) {
    Fail("conditional subquery k2 value mismatch");
  }
  if (elements.size() != 3) {
    Fail("conditional subquery expected exactly 3 elements");
  }
}

void AssertSubqueryPathTraversal(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path traversal verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path traversal root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_x == nullptr || child_y == nullptr || !child_x->has_element || !child_y->has_element) {
    Fail("subquery path traversal result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(child_x->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'9'})) {
    Fail("subquery path x value mismatch");
  }
  if (!grovedb::DecodeItemFromElementBytes(child_y->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'8'})) {
    Fail("subquery path y value mismatch");
  }
  if (elements.size() != 2) {
    Fail("subquery path traversal expected exactly 2 elements");
  }
}

void AssertSubqueryPathRangeAfterTraversal(const std::vector<uint8_t>& proof,
                                           const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfter({'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  if (child_y == nullptr || child_x != nullptr || !child_y->has_element) {
    Fail("subquery path range-after result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterAbsenceTraversal(const std::vector<uint8_t>& proof,
                                                  const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfter({'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-after absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterLimitTraversal(const std::vector<uint8_t>& proof,
                                                const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfter({'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after limit root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_y == nullptr || child_z != nullptr || !child_y->has_element) {
    Fail("subquery path range-after limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeToInclusiveTraversal(const std::vector<uint8_t>& proof,
                                                 const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeToInclusive({'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-to-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-to-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_x == nullptr || child_y != nullptr || !child_x->has_element) {
    Fail("subquery path range-to-inclusive result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeToInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeToInclusive({'w'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-to-inclusive absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-to-inclusive absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-to-inclusive absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeToTraversal(const std::vector<uint8_t>& proof,
                                        const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeTo({'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-to verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-to root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_x == nullptr || child_y != nullptr || !child_x->has_element) {
    Fail("subquery path range-to result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeToAbsenceTraversal(const std::vector<uint8_t>& proof,
                                               const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeTo({'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-to absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-to absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-to absence expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeToLimitTraversal(const std::vector<uint8_t>& proof,
                                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeTo({'z'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::make_optional(1), std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-to limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-to limit root hash mismatch");
  }
  if (elements.size() != 1) {
    Fail("subquery path range-to limit expected 1 element: " +
         std::to_string(elements.size()));
  }
  if (elements[0].key != std::vector<uint8_t>{'x'}) {
    Fail("subquery path range-to limit expected key x");
  }
}

void AssertSubqueryPathRangeFromTraversal(const std::vector<uint8_t>& proof,
                                          const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeFrom({'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-from verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-from root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_y == nullptr || child_x != nullptr || !child_y->has_element) {
    Fail("subquery path range-from result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeFromLimitTraversal(const std::vector<uint8_t>& proof,
                                               const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeFrom({'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::make_optional(1), std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-from limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-from limit root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_x == nullptr || !child_x->has_element || child_y != nullptr || child_z != nullptr ||
      elements.size() != 1) {
    Fail("subquery path range-from limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeFromAbsenceTraversal(const std::vector<uint8_t>& proof,
                                                 const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeFrom({'z'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-from absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-from absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-from absence expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeFromLimitAbsenceTraversal(const std::vector<uint8_t>& proof,
                                                      const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeFrom({'z'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::make_optional(1), std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-from limit absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-from limit absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-from limit absence expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeTraversal(const std::vector<uint8_t>& proof,
                                      const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::Range({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_x == nullptr || child_y != nullptr || !child_x->has_element) {
    Fail("subquery path range result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeLimitTraversal(const std::vector<uint8_t>& proof,
                                           const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::Range({'a'}, {'z'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range limit root hash mismatch");
  }
  // range(a..z) with limit=1: first outer key is "key",
  // descending into root/key/branch gives items in range (a, z) exclusive.
  // With limit=1, we expect exactly one element (the first match).
  size_t element_count = 0;
  for (const auto& element : elements) {
    if (element.has_element) {
      element_count++;
    }
  }
  if (element_count != 1) {
    Fail("subquery path range limit expected exactly 1 element, got " +
         std::to_string(element_count) + ": " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAbsenceTraversal(const std::vector<uint8_t>& proof,
                                             const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::Range({'w'}, {'x'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range absence expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeInclusiveTraversal(const std::vector<uint8_t>& proof,
                                               const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeInclusive({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_x == nullptr || child_y == nullptr || !child_x->has_element || !child_y->has_element) {
    Fail("subquery path range-inclusive result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeInclusive({'w'}, {'w'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-inclusive absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-inclusive absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-inclusive absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeInclusive({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-inclusive limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-inclusive limit root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  if (child_x == nullptr || !child_x->has_element) {
    Fail("subquery path range-inclusive limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
  if (elements.size() != 1) {
    Fail("subquery path range-inclusive limit expected exactly one element: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToTraversal(const std::vector<uint8_t>& proof,
                                             const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfterTo({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after-to verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after-to root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-after-to expected empty result: " + ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfterTo({'y'}, {'z'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after-to absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after-to absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-after-to absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfterTo({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::make_optional(1), std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after-to limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after-to limit root hash mismatch");
  }
  // RangeAfterTo(x, y) is exclusive on both ends; with branch keys {x, y} it yields no children.
  if (!elements.empty()) {
    Fail("subquery path range-after-to limit expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToInclusiveTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after-to-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after-to-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (child_y == nullptr || child_x != nullptr || !child_y->has_element) {
    Fail("subquery path range-after-to-inclusive result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  auto branch_query = std::make_unique<grovedb::Query>();
  branch_query->items.push_back(grovedb::QueryItem::RangeAfterToInclusive({'y'}, {'y'}));
  query.default_subquery_branch.subquery = std::move(branch_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path range-after-to-inclusive absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path range-after-to-inclusive absence root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("subquery path range-after-to-inclusive absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathWithParentTraversal(const std::vector<uint8_t>& proof,
                                           const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.add_parent_tree_on_subquery = true;

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path+parent verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path+parent root hash mismatch");
  }
  const auto* parent =
      FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'});
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (parent == nullptr || child_x == nullptr || child_y == nullptr ||
      !child_x->has_element || !child_y->has_element) {
    Fail("subquery path+parent result shape mismatch: " + ElementsDebugString(elements));
  }
  if (elements.size() != 3) {
    Fail("subquery path+parent expected exactly 3 elements");
  }
}

void AssertSubqueryPathLimitTraversal(const std::vector<uint8_t>& proof,
                                      const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path limit root hash mismatch");
  }
  const std::vector<std::vector<uint8_t>> expected_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  const auto* child_x = FindElementByPathAndKey(elements, expected_path, {'x'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_x == nullptr || child_z != nullptr) {
    Fail("subquery path limit result shape mismatch: " + ElementsDebugString(elements));
  }
  if (!child_x->has_element) {
    Fail("subquery path limit expected materialized element");
  }
}

void AssertSubqueryPathLimitDescTraversal(const std::vector<uint8_t>& proof,
                                          const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.left_to_right = false;

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery path descending limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery path descending limit root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  if (child_x == nullptr || !child_x->has_element || elements.size() != 1) {
    Fail("subquery path descending limit result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertReferenceChainWrappedLimitTraversal(const std::vector<uint8_t>& proof,
                                               const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query = grovedb::Query::NewSingleKey({'r', 'e', 'f', 'a'});
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.add_parent_tree_on_subquery = true;

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference chain wrapped+limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference chain wrapped+limit root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("reference chain wrapped+limit expected no verified elements");
  }
}

void AssertReferenceHopLimitRejected(const std::vector<uint8_t>& proof,
                                     const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query = grovedb::Query::NewSingleKey(
      {'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'});
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.add_parent_tree_on_subquery = true;

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference hop-limit query verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference hop-limit query root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("reference hop-limit query expected no verified elements");
  }
}

void AssertReferenceHopLimitMaterializedChain(const std::vector<uint8_t>& proof,
                                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(
      grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'}));
  query.items.push_back(
      grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference hop-limit materialized-chain verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference hop-limit materialized-chain root hash mismatch");
  }
  const auto* hop_b = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}}, {'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'b'});
  if (hop_b == nullptr || !hop_b->has_element) {
    Fail("reference hop-limit materialized-chain expected sibling key to remain verifiable: " +
         ElementsDebugString(elements));
  }
}

void AssertReferenceConditionalSubqueryLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(
      grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference conditional subquery limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference conditional subquery limit root hash mismatch");
  }
  const auto* ref_hop_a = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}}, {'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'});
  if (ref_hop_a == nullptr || !ref_hop_a->has_element) {
    Fail("reference conditional subquery limit expected ref_hop_a element: " +
         ElementsDebugString(elements));
  }
  const auto* x = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'x'});
  // Rust proof shape currently materializes the selected reference parent under the top-level
  // limit but omits nested lower layers for the conditional subquery branch in this scenario.
  if (x != nullptr) {
    Fail("reference conditional subquery limit unexpectedly materialized nested x element: " +
         ElementsDebugString(elements));
  }
  if (elements.size() != 1) {
    Fail("reference conditional subquery limit expected exactly 1 element (ref_hop_a): " +
         ElementsDebugString(elements));
  }
}

void AssertReferenceEmptyKeySiblingTraversal(const std::vector<uint8_t>& proof,
                                             const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query = grovedb::Query::NewSingleKey({});
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("empty-key sibling reference query verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("empty-key sibling reference query root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("empty-key sibling reference should verify with empty nested results: " +
         ElementsDebugString(elements));
  }

  const std::vector<grovedb::GroveVersion> versions = {
      grovedb::GroveVersion::V4_0_0(),
      grovedb::GroveVersion{4, 1, 0},
      grovedb::GroveVersion{5, 0, 0},
  };
  for (const auto& version : versions) {
    std::vector<uint8_t> version_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> version_elements;
    if (!grovedb::VerifyPathQueryProofForVersion(
            proof, path_query, version, &version_root_hash, &version_elements, &error)) {
      Fail("empty-key sibling reference query versioned verification failed at version " +
           version.ToString() + ": " + error);
    }
    if (version_root_hash != root_hash || !SameVerifiedElements(version_elements, elements)) {
      Fail("empty-key sibling reference versioned verification diverged at version " +
           version.ToString());
    }
  }
}

void AssertConditionalSubqueryPathTraversal(const std::vector<uint8_t>& proof,
                                            const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x == nullptr || child_y == nullptr || k2 == nullptr ||
      !child_x->has_element || !child_y->has_element || !k2->has_element) {
    Fail("conditional subquery_path result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(k2->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'v', '2'})) {
    Fail("conditional subquery_path k2 value mismatch");
  }
  if (elements.size() != 3) {
    Fail("conditional subquery_path expected exactly 3 elements");
  }
}

void AssertConditionalSubqueryPathRangeAfterTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfter({'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_y == nullptr || child_x != nullptr || k2 == nullptr || !child_y->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range-after result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeAfterAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfter({'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-after absence missing k2: " +
         ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range-after absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeAfterLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfter({'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after limit root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_y == nullptr || child_z != nullptr || !child_y->has_element) {
    Fail("conditional subquery_path range-after limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeAfterLimitAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfter({'z'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after limit absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after limit absence root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_y != nullptr || child_z != nullptr || !elements.empty()) {
    Fail("conditional subquery_path range-after limit absence expected empty result: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeToInclusiveTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeToInclusive({'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-to-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-to-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x == nullptr || child_y != nullptr || k2 == nullptr || !child_x->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range-to-inclusive result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeToInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeToInclusive({'w'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-to-inclusive absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-to-inclusive absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-to-inclusive absence missing k2: " +
         ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail(
          "conditional subquery_path range-to-inclusive absence expected empty key branch result: "
          + ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeFromTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeFrom({'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-from verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-from root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_y == nullptr || child_x != nullptr || k2 == nullptr || !child_y->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range-from result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeFromAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeFrom({'z'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-from absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-from absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-from absence missing k2: " + ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range-from absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeToTraversal(const std::vector<uint8_t>& proof,
                                                   const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeTo({'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-to verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-to root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x == nullptr || child_y != nullptr || k2 == nullptr || !child_x->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range-to result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeToAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeTo({'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-to absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-to absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-to absence missing k2: " + ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range-to absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeTraversal(const std::vector<uint8_t>& proof,
                                                 const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::Range({'x'}, {'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x == nullptr || child_y != nullptr || k2 == nullptr || !child_x->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range result shape mismatch: " + ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::Range({'w'}, {'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range absence missing k2: " + ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeInclusiveTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeInclusive({'x'}, {'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x == nullptr || child_y == nullptr || k2 == nullptr || !child_x->has_element ||
      !child_y->has_element || !k2->has_element) {
    Fail("conditional subquery_path range-inclusive result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeInclusive({'w'}, {'w'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-inclusive absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-inclusive absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-inclusive absence missing k2: " +
         ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range-inclusive absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeAfterToTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfterTo({'x'}, {'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after-to verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after-to root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_x != nullptr || child_y != nullptr || k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-after-to result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeAfterToAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfterTo({'y'}, {'z'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after-to absence verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after-to absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-after-to absence missing k2: " +
         ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail("conditional subquery_path range-after-to absence expected empty key branch result: " +
           ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeAfterToInclusiveTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after-to-inclusive verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after-to-inclusive root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (child_y == nullptr || child_x != nullptr || k2 == nullptr || !child_y->has_element ||
      !k2->has_element) {
    Fail("conditional subquery_path range-after-to-inclusive result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeAfterToInclusiveAbsenceTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', '2'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeAfterToInclusive({'y'}, {'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-after-to-inclusive absence verification failed: " +
         error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-after-to-inclusive absence root hash mismatch");
  }
  const auto* k2 = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '2'});
  if (k2 == nullptr || !k2->has_element) {
    Fail("conditional subquery_path range-after-to-inclusive absence missing k2: " +
         ElementsDebugString(elements));
  }
  const std::vector<std::vector<uint8_t>> key_branch_path = {
      {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
  for (const auto& element : elements) {
    if (element.path == key_branch_path) {
      Fail(
          "conditional subquery_path range-after-to-inclusive absence expected empty key branch result: "
          + ElementsDebugString(elements));
    }
  }
}

void AssertConditionalSubqueryPathRangeToInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeToInclusive({'x'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-to-inclusive limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-to-inclusive limit root hash mismatch");
  }
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_x == nullptr || child_z != nullptr || !child_x->has_element) {
    Fail("conditional subquery_path range-to-inclusive limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathRangeFromLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>();
  branch.subquery->items.push_back(grovedb::QueryItem::RangeFrom({'y'}));
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("conditional subquery_path range-from limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("conditional subquery_path range-from limit root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_y == nullptr || child_z != nullptr || !child_y->has_element) {
    Fail("conditional subquery_path range-from limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertConditionalSubqueryPathLimitWithInner(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash,
    const grovedb::QueryItem& inner_item,
    const std::optional<std::vector<uint8_t>>& expected_key,
    const std::string& label,
    bool inner_left_to_right = true) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery_path = std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  branch.subquery = std::make_unique<grovedb::Query>(grovedb::Query::NewSingleQueryItem(inner_item));
  branch.subquery->left_to_right = inner_left_to_right;
  query.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'k', 'e', 'y'}), std::move(branch)});
  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail(label + " verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail(label + " root hash mismatch");
  }
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_z != nullptr) {
    Fail(label + " unexpectedly included key_b branch result: " + ElementsDebugString(elements));
  }
  if (expected_key.has_value()) {
    const auto* expected_child = FindElementByPathAndKey(
        elements,
        {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
        *expected_key);
    if (expected_child == nullptr || !expected_child->has_element) {
      Fail(label + " missing expected key result: " + ElementsDebugString(elements));
    }
  } else {
    const std::vector<std::vector<uint8_t>> key_branch_path = {
        {'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}};
    for (const auto& element : elements) {
      if (element.path == key_branch_path) {
        Fail(label + " expected empty key branch result: " + ElementsDebugString(elements));
      }
    }
  }
}

void AssertConditionalSubqueryPathRangeLimitTraversal(const std::vector<uint8_t>& proof,
                                                      const std::vector<uint8_t>& expected_root_hash) {
  AssertConditionalSubqueryPathLimitWithInner(
      proof,
      expected_root_hash,
      grovedb::QueryItem::Range({'x'}, {'y'}),
      std::vector<uint8_t>({'x'}),
      "conditional subquery_path range limit");
}

void AssertConditionalSubqueryPathRangeInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  AssertConditionalSubqueryPathLimitWithInner(
      proof,
      expected_root_hash,
      grovedb::QueryItem::RangeInclusive({'x'}, {'y'}),
      std::vector<uint8_t>({'x'}),
      "conditional subquery_path range-inclusive limit");
}

void AssertConditionalSubqueryPathRangeAfterToLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  AssertConditionalSubqueryPathLimitWithInner(
      proof,
      expected_root_hash,
      grovedb::QueryItem::RangeAfterTo({'x'}, {'y'}),
      std::nullopt,
      "conditional subquery_path range-after-to limit");
}

void AssertConditionalSubqueryPathRangeAfterToInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  AssertConditionalSubqueryPathLimitWithInner(
      proof,
      expected_root_hash,
      grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'}),
      std::vector<uint8_t>({'y'}),
      "conditional subquery_path range-after-to-inclusive limit");
}

void AssertConditionalSubqueryPathRangeToLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  AssertConditionalSubqueryPathLimitWithInner(
      proof,
      expected_root_hash,
      grovedb::QueryItem::RangeTo({'y'}),
      std::vector<uint8_t>({'x'}),
      "conditional subquery_path range-to limit");
}

void AssertNestedConditionalSubqueryTraversal(const std::vector<uint8_t>& proof,
                                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query root_query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  root_query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'n', 'e', 's', 't'}};
  auto nested_query =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  nested_query->conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  nested_query->conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'m'}), std::move(branch)});
  root_query.default_subquery_branch.subquery = std::move(nested_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(root_query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("nested conditional subquery verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("nested conditional subquery root hash mismatch");
  }
  const auto* u = FindElementByPathAndKey(elements,
                                          {{'r', 'o', 'o', 't'},
                                           {'k', 'e', 'y'},
                                           {'n', 'e', 's', 't'},
                                           {'m'}},
                                          {'u'});
  const auto* n = FindElementByPathAndKey(elements,
                                          {{'r', 'o', 'o', 't'},
                                           {'k', 'e', 'y'},
                                           {'n', 'e', 's', 't'}},
                                          {'n'});
  if (u == nullptr || n == nullptr || !u->has_element || !n->has_element) {
    Fail("nested conditional subquery result shape mismatch: " + ElementsDebugString(elements));
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(u->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'7'})) {
    Fail("nested conditional subquery value mismatch for u");
  }
  if (!grovedb::DecodeItemFromElementBytes(n->element_bytes, &item, &error) ||
      item.value != std::vector<uint8_t>({'6'})) {
    Fail("nested conditional subquery value mismatch for n");
  }
  if (elements.size() != 2) {
    Fail("nested conditional subquery expected exactly 2 elements");
  }
}

void AssertReferenceNestedConditionalSubqueryTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query root_query = grovedb::Query::NewSingleKey({'r', 'e', 'f', 'a'});
  root_query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'n', 'e', 's', 't'}};
  auto nested_query =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  nested_query->conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  grovedb::SubqueryBranch branch;
  branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  nested_query->conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key({'m'}), std::move(branch)});
  root_query.default_subquery_branch.subquery = std::move(nested_query);

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(root_query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference nested conditional subquery verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference nested conditional subquery root hash mismatch");
  }
  if (!elements.empty()) {
    Fail("reference nested conditional subquery expected no verified elements");
  }
}

void AssertChainedPathQueryVerification(const std::vector<uint8_t>& proof,
                                        const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query root_query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  root_query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  root_query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
  grovedb::PathQuery first_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(root_query), std::nullopt, std::nullopt));

  grovedb::PathQuery chained_query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                                       {'x'});
  std::vector<grovedb::PathQuery> chained_queries;
  chained_queries.push_back(std::move(chained_query));

  std::vector<uint8_t> root_hash;
  std::vector<std::vector<grovedb::VerifiedPathKeyElement>> results;
  std::string error;
  if (!grovedb::VerifyPathQueryProofWithChainedQueries(
          proof, first_query, chained_queries, &root_hash, &results, &error)) {
    Fail("chained path query verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("chained path query root hash mismatch");
  }
  if (results.size() != 2) {
    Fail("chained path query expected 2 result sets");
  }
  if (results[0].size() != 2) {
    Fail("chained path query first result set expected 2 elements");
  }
  const auto* x_from_first = FindElementByPathAndKey(
      results[0], {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  const auto* y_from_first = FindElementByPathAndKey(
      results[0], {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  if (x_from_first == nullptr || y_from_first == nullptr || !x_from_first->has_element ||
      !y_from_first->has_element) {
    Fail("chained path query first result set shape mismatch");
  }
  if (results[1].size() != 1) {
    Fail("chained path query second result set expected 1 element");
  }
  const auto* x_from_second = FindElementByPathAndKey(
      results[1], {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  if (x_from_second == nullptr || !x_from_second->has_element) {
    Fail("chained path query second result set shape mismatch");
  }
}

void AssertZeroOffsetAccepted(const std::vector<uint8_t>& proof,
                              const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.offset = 0;
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("zero-offset path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("zero-offset path query root hash mismatch");
  }
  if (elements.size() != 1) {
    Fail("zero-offset path query expected one element");
  }
}

void AssertNonZeroOffsetRejected(const std::vector<uint8_t>& proof) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.offset = 1;
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (grovedb::VerifyPathQueryProof(proof, query, &root_hash, &elements, &error)) {
    Fail("non-zero offset path query should fail");
  }
  if (error != "offsets in path queries are not supported for proofs") {
    Fail("non-zero offset path query returned unexpected error: " + error);
  }
}

void AssertProveQueryGenerationOffsetAndLimitContracts(const std::filesystem::path& fixture_dir) {
  const std::filesystem::path cpp_fixture_path =
      fixture_dir / "cpp_prove_query_offset_limit_contract_fixture_db";
  std::filesystem::remove_all(cpp_fixture_path);
  std::filesystem::create_directories(cpp_fixture_path);

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_fixture_path.string(), &error)) {
    Fail("failed to open C++ prove-query offset/limit contract fixture db: " + error);
  }
  std::vector<uint8_t> item;
  if (!grovedb::EncodeItemToElementBytes({'v'}, &item, &error)) {
    Fail("failed to encode prove-query offset/limit contract fixture item: " + error);
  }
  if (!db.Insert({}, {'a'}, item, &error)) {
    Fail("failed to insert prove-query offset/limit contract fixture item: " + error);
  }

  grovedb::PathQuery query = grovedb::PathQuery::NewSingleKey({}, {'a'});
  std::vector<uint8_t> proof;

  query.query.offset = 0;
  if (!db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery should accept zero offset: " + error);
  }
  if (proof.empty()) {
    Fail("ProveQuery zero-offset should return a proof");
  }
  if (!db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion should accept zero offset: " + error);
  }
  if (proof.empty()) {
    Fail("ProveQueryForVersion zero-offset should return a proof");
  }

  query.query.offset = 1;
  error.clear();
  if (db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery with non-zero offset should fail");
  }
  if (error != "proved path queries can not have offsets") {
    Fail("ProveQuery non-zero offset returned unexpected error: " + error);
  }
  error.clear();
  if (db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion with non-zero offset should fail");
  }
  if (error != "proved path queries can not have offsets") {
    Fail("ProveQueryForVersion non-zero offset returned unexpected error: " + error);
  }

  query.query.offset.reset();
  query.query.limit = 0;
  error.clear();
  if (db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery with limit=0 should fail");
  }
  if (error != "proved path queries can not be for limit 0") {
    Fail("ProveQuery limit=0 returned unexpected error: " + error);
  }
  error.clear();
  if (db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion with limit=0 should fail");
  }
  if (error != "proved path queries can not be for limit 0") {
    Fail("ProveQueryForVersion limit=0 returned unexpected error: " + error);
  }

  query.query.limit.reset();
  query.query.offset.reset();
  query.query.query.left_to_right = false;
  error.clear();
  if (db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery with right-to-left order should fail");
  }
  if (error != "proof generation currently supports only left-to-right query order") {
    Fail("ProveQuery right-to-left returned unexpected error: " + error);
  }
  error.clear();
  if (db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion with right-to-left order should fail");
  }
  if (error != "proof generation currently supports only left-to-right query order") {
    Fail("ProveQueryForVersion right-to-left returned unexpected error: " + error);
  }

  query.query.query = grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'a'}));
  query.query.query.items.push_back(grovedb::QueryItem::Key({'b'}));
  error.clear();
  if (db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery with multiple query items should fail");
  }
  if (error != "proof generation currently supports exactly one query item") {
    Fail("ProveQuery multi-item returned unexpected error: " + error);
  }
  error.clear();
  if (db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion with multiple query items should fail");
  }
  if (error != "proof generation currently supports exactly one query item") {
    Fail("ProveQueryForVersion multi-item returned unexpected error: " + error);
  }

  query = grovedb::PathQuery::NewSingleKey({}, {'a'});
  query.query.query.items.clear();
  error.clear();
  if (db.ProveQuery(query, &proof, &error)) {
    Fail("ProveQuery with empty query items should fail");
  }
  if (error != "query has no items") {
    Fail("ProveQuery empty query items returned unexpected error: " + error);
  }
  error.clear();
  if (db.ProveQueryForVersion(query, grovedb::GroveVersion::V4_0_0(), &proof, &error)) {
    Fail("ProveQueryForVersion with empty query items should fail");
  }
  if (error != "query has no items") {
    Fail("ProveQueryForVersion empty query items returned unexpected error: " + error);
  }
}

void AssertSubsetQuery(const std::vector<uint8_t>& proof,
                       const std::vector<uint8_t>& expected_root_hash) {
  // VerifySubsetQuery mirrors Rust's verify_subset_query which allows
  // non-minimal proofs (verify_proof_succinctness: false).
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifySubsetQuery(proof, query, &root_hash, &elements, &error)) {
    Fail("subset path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subset path query root hash mismatch");
  }
  if (elements.size() != 1) {
    Fail("subset path query expected one element");
  }
  const auto& first = elements.front();
  if (!first.has_element || first.key != std::vector<uint8_t>({'k', '2'})) {
    Fail("subset path query returned unexpected key element");
  }
}

void AssertSubsetQueryForVersion(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash,
                                 const grovedb::GroveVersion& version) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifySubsetQueryForVersion(proof, query, version, &root_hash, &elements, &error)) {
    Fail("subset path query proof verification failed for version " + version.ToString() + ": " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subset path query root hash mismatch for version " + version.ToString());
  }
  if (elements.size() != 1) {
    Fail("subset path query expected one element for version " + version.ToString());
  }
}

void AssertQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash) {
  // VerifyQueryWithAbsenceProof mirrors Rust's verify_query_with_absence_proof
  // which includes absence entries for non-existing searched keys.
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.limit = 10;  // Required for absence proof
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("absence path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("absence path query root hash mismatch");
  }
  // For presence proof, we should get the element
  if (elements.size() < 1) {
    Fail("absence path query expected at least one element");
  }
}

void AssertQueryWithAbsenceProofMissingKey(const std::vector<uint8_t>& proof,
                                          const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '0'});
  query.query.limit = 10;
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("absence missing-key proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("absence missing-key proof root hash mismatch");
  }
  const auto* absent = FindElementByPathAndKey(elements, {{'r', 'o', 'o', 't'}}, {'k', '0'});
  if (absent == nullptr) {
    Fail("absence missing-key proof should include terminal absent row: " +
         ElementsDebugString(elements));
  }
  if (absent->has_element) {
    Fail("absence missing-key proof row should be absent");
  }
}

void AssertQueryWithAbsenceProofRejectsUnsupportedRangeTerminalKeys(
    const std::vector<uint8_t>& unbounded_range_proof,
    const std::vector<uint8_t>& bounded_multibyte_range_proof) {
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;

  grovedb::PathQuery unbounded_query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::RangeTo({'k', '0'}));
  unbounded_query.query.limit = 10;
  if (grovedb::VerifyQueryWithAbsenceProof(
          unbounded_range_proof, unbounded_query, &root_hash, &elements, &error)) {
    Fail("absence proof with unbounded range terminal keys should fail");
  }
  if (error != "terminal keys are not supported with unbounded ranges") {
    Fail("unexpected unbounded-range absence proof error: " + error);
  }

  error.clear();
  grovedb::PathQuery multibyte_query = grovedb::PathQuery::NewSingleQueryItem(
      {{'r', 'o', 'o', 't'}}, grovedb::QueryItem::Range({'k', '0'}, {'k', '1'}));
  multibyte_query.query.limit = 10;
  if (grovedb::VerifyQueryWithAbsenceProof(
          bounded_multibyte_range_proof, multibyte_query, &root_hash, &elements, &error)) {
    Fail("absence proof with multibyte bounded range terminal keys should fail");
  }
  if (error != "distinct keys are not available for ranges using more or less than 1 byte") {
    Fail("unexpected multibyte-range absence proof error: " + error);
  }
}

void AssertQueryWithAbsenceProofBoundedRangeTerminalExpansion(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query = grovedb::PathQuery::NewSingleQueryItem(
      {}, grovedb::QueryItem::RangeInclusive({'a'}, {'c'}));
  query.query.limit = 10;
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("bounded-range absence proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("bounded-range absence proof root hash mismatch");
  }
  const auto* a = FindElementByPathAndKey(elements, {}, {'a'});
  const auto* b = FindElementByPathAndKey(elements, {}, {'b'});
  const auto* c = FindElementByPathAndKey(elements, {}, {'c'});
  if (a == nullptr || b == nullptr || c == nullptr) {
    Fail("bounded-range absence proof missing terminal rows: " + ElementsDebugString(elements));
  }
  if (!a->has_element || b->has_element || !c->has_element) {
    Fail("bounded-range absence proof presence/absence shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertQueryWithAbsenceProofForVersion(const std::vector<uint8_t>& proof,
                                           const std::vector<uint8_t>& expected_root_hash,
                                           const grovedb::GroveVersion& version) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.limit = 10;  // Required for absence proof
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyQueryWithAbsenceProofForVersion(proof, query, version, &root_hash, &elements, &error)) {
    Fail("absence path query proof verification failed for version " + version.ToString() + ": " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("absence path query root hash mismatch for version " + version.ToString());
  }
}

void AssertSubsetQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                       const std::vector<uint8_t>& expected_root_hash) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.limit = 10;  // Required for absence proof
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifySubsetQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("subset+absence path query proof verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subset+absence path query root hash mismatch");
  }
  if (elements.size() < 1) {
    Fail("subset+absence path query expected at least one element");
  }
}

void AssertSubsetQueryWithAbsenceProofForVersion(const std::vector<uint8_t>& proof,
                                                 const std::vector<uint8_t>& expected_root_hash,
                                                 const grovedb::GroveVersion& version) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  query.query.limit = 10;  // Required for absence proof
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifySubsetQueryWithAbsenceProofForVersion(
          proof, query, version, &root_hash, &elements, &error)) {
    Fail("subset+absence path query proof verification failed for version " +
         version.ToString() + ": " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subset+absence path query root hash mismatch for version " + version.ToString());
  }
}

void AssertReferenceChainSubsetVerification(const std::vector<uint8_t>& proof,
                                            const std::vector<uint8_t>& expected_root_hash) {
  // VerifySubsetQuery across reference chain with multiple hops
  // Tests reference resolution: ref_hop_a -> ref_hop_b -> target subtree
  grovedb::Query query;
  query.items.push_back(
      grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'}));
  query.items.push_back(
      grovedb::QueryItem::Key({'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'b'}));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}},
      grovedb::SizedQuery::New(std::move(query), std::nullopt, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifySubsetQuery(proof, path_query, &root_hash, &elements, &error)) {
    Fail("reference-chain subset verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("reference-chain subset root hash mismatch");
  }
  // Should have found both reference nodes
  if (elements.size() < 2) {
    Fail("reference-chain subset expected at least two elements (ref_hop_a and ref_hop_b): " +
         ElementsDebugString(elements));
  }
  const auto* hop_a = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}}, {'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'a'});
  const auto* hop_b = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'r', 'e', 'f', 's'}}, {'r', 'e', 'f', '_', 'h', 'o', 'p', '_', 'b'});
  if (hop_a == nullptr || !hop_a->has_element) {
    Fail("reference-chain subset expected ref_hop_a element: " + ElementsDebugString(elements));
  }
  if (hop_b == nullptr || !hop_b->has_element) {
    Fail("reference-chain subset expected ref_hop_b element: " + ElementsDebugString(elements));
  }
}

void AssertVerifyQueryGetParentTreeInfo(const std::vector<uint8_t>& proof,
                                        const std::vector<uint8_t>& expected_root_hash) {
  // Verify subtree query with parent tree info extraction
  // Query the "key" subtree under "root"
  grovedb::PathQuery query;
  query.path = {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}};
  query.query.query = grovedb::Query();
  
  grovedb::VerifiedQueryResult result;
  std::string error;
  if (!grovedb::VerifyQueryGetParentTreeInfo(proof, query, &result, &error)) {
    Fail("verify_query_get_parent_tree_info failed: " + error);
  }
  if (result.root_hash != expected_root_hash) {
    Fail("verify_query_get_parent_tree_info root hash mismatch");
  }
  // The tree type should be set for a subtree query (Tree = 2)
  if (!result.has_tree_type) {
    Fail("verify_query_get_parent_tree_info expected tree type to be set");
  }
  if (result.tree_type != 2) {
    Fail("verify_query_get_parent_tree_info expected tree type 2 (Tree), got " +
         std::to_string(result.tree_type));
  }
  // The fixture is produced from an empty subtree query and is used here only to
  // validate parent tree info extraction + root hash reconstruction.
  if (!result.elements.empty()) {
    Fail("verify_query_get_parent_tree_info expected no elements for empty subtree query");
  }
}

void AssertVerifyQueryGetParentTreeInfoForVersion(const std::vector<uint8_t>& proof,
                                                  const std::vector<uint8_t>& expected_root_hash,
                                                  const grovedb::GroveVersion& version) {
  grovedb::PathQuery query;
  query.path = {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}};
  query.query.query = grovedb::Query();
  
  grovedb::VerifiedQueryResult result;
  std::string error;
  if (!grovedb::VerifyQueryGetParentTreeInfoForVersion(proof, query, version, &result, &error)) {
    Fail("verify_query_get_parent_tree_info failed for version " + version.ToString() + ": " +
         error);
  }
  if (result.root_hash != expected_root_hash) {
    Fail("verify_query_get_parent_tree_info root hash mismatch for version " +
         version.ToString());
  }
  if (!result.has_tree_type) {
    Fail("verify_query_get_parent_tree_info expected tree type to be set for version " +
         version.ToString());
  }
  if (result.tree_type != 2) {
    Fail("verify_query_get_parent_tree_info expected tree type 2 (Tree) for version " +
         version.ToString() + ", got " + std::to_string(result.tree_type));
  }
  if (!result.elements.empty()) {
    Fail("verify_query_get_parent_tree_info expected no elements for empty subtree query for "
         "version " + version.ToString());
  }
}

void AssertAbsenceProofRequiresLimit(const std::vector<uint8_t>& proof) {
  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '0'});
  // No limit set - should fail
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (grovedb::VerifyQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("absence path query without limit should fail");
  }
  if (error != "limits must be set in verify_query_with_absence_proof") {
    Fail("absence path query without limit returned unexpected error: " + error);
  }

  error.clear();
  if (grovedb::VerifySubsetQueryWithAbsenceProof(proof, query, &root_hash, &elements, &error)) {
    Fail("subset+absence path query without limit should fail");
  }
  if (error != "limits must be set in verify_query_with_absence_proof") {
    Fail("subset+absence path query without limit returned unexpected error: " + error);
  }
}

void AssertDecodeAndVerifyRejectMalformed(const std::vector<uint8_t>& proof) {
  std::string error;
  grovedb::GroveLayerProof layer;
  if (proof.size() > 1) {
    std::vector<uint8_t> truncated(proof.begin(), proof.end() - 1);
    if (grovedb::DecodeGroveDbProof(truncated, &layer, &error)) {
      Fail("truncated grove proof decode should fail");
    }
  }

  const std::vector<uint8_t> malformed = {0xff};
  if (grovedb::DecodeGroveDbProof(malformed, &layer, &error)) {
    Fail("malformed grove proof decode should fail");
  }

  grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  if (grovedb::VerifyPathQueryProof(malformed, query, &root_hash, &elements, &error)) {
    Fail("malformed grove proof verify should fail");
  }
}

void AssertSubqueryPathRangeToInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeToInclusive({'y'})));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery_path range-to-inclusive limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery_path range-to-inclusive limit root hash mismatch");
  }
  // range_to_inclusive(..="y") with limit=1: first outer key is "key",
  // descending into root/key/branch gives items x, y (both <= "y").
  // With limit=1, we expect exactly one element (the first match, "x").
  const auto* child_x = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'x'});
  if (child_x == nullptr || !child_x->has_element) {
    Fail("subquery_path range-to-inclusive limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

void AssertSubqueryPathRangeAfterToInclusiveLimitTraversal(
    const std::vector<uint8_t>& proof,
    const std::vector<uint8_t>& expected_root_hash) {
  grovedb::Query query;
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y'}));
  query.items.push_back(grovedb::QueryItem::Key({'k', 'e', 'y', '_', 'b'}));
  query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  query.default_subquery_branch.subquery =
      std::make_unique<grovedb::Query>(
          grovedb::Query::NewSingleQueryItem(
              grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'})));

  grovedb::PathQuery path_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(std::move(query), 1, std::nullopt));

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  if (!grovedb::VerifyPathQueryProof(proof, path_query, &root_hash, &elements, &error)) {
    Fail("subquery_path range-after-to-inclusive limit verification failed: " + error);
  }
  if (root_hash != expected_root_hash) {
    Fail("subquery_path range-after-to-inclusive limit root hash mismatch");
  }
  const auto* child_y = FindElementByPathAndKey(
      elements, {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}, {'y'});
  const auto* child_z = FindElementByPathAndKey(
      elements,
      {{'r', 'o', 'o', 't'}, {'k', 'e', 'y', '_', 'b'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
      {'z'});
  if (child_y == nullptr || child_z != nullptr || !child_y->has_element || elements.size() != 1) {
    Fail("subquery_path range-after-to-inclusive limit result shape mismatch: " +
         ElementsDebugString(elements));
  }
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  std::string dir = MakeTempDir("rust_grovedb_proof_parity");

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_grovedb_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb proof writer");
  }

  const std::filesystem::path fixture_dir(dir);
  if (std::filesystem::exists(fixture_dir / "grove_proof_reference_cycle.bin")) {
    Fail("reference-cycle proof fixture should not exist (Rust proving must reject cycle)");
  }
  if (!std::filesystem::exists(fixture_dir / "grove_proof_reference_hop_limit.bin")) {
    Fail("reference-hop-limit proof fixture missing");
  }

  const std::vector<uint8_t> present_proof = ReadFile(fixture_dir / "grove_proof_present.bin");
  const std::vector<uint8_t> absent_proof = ReadFile(fixture_dir / "grove_proof_absent.bin");
  const std::vector<uint8_t> prove_query_for_version_rust_proof =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_rust.bin");
  const std::vector<uint8_t> prove_query_for_version_root_hash =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_root_hash.bin");
  const std::vector<uint8_t> prove_query_for_version_layered_rust_proof =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_layered_rust.bin");
  const std::vector<uint8_t> prove_query_for_version_layered_root_hash =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_layered_root_hash.bin");
  const std::vector<uint8_t> prove_query_for_version_limit_rust_proof =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_limit_rust.bin");
  const std::vector<uint8_t> prove_query_for_version_limit_root_hash =
      ReadFile(fixture_dir / "grove_proof_prove_query_for_version_limit_root_hash.bin");
  const std::vector<uint8_t> aggregate_tree_proof =
      ReadFile(fixture_dir / "grove_proof_aggregate_trees.bin");
  const std::vector<uint8_t> aggregate_tree_root_hash =
      ReadFile(fixture_dir / "grove_proof_aggregate_trees_root_hash.bin");
  const std::vector<uint8_t> absence_terminal_range_proof =
      ReadFile(fixture_dir / "grove_proof_absence_terminal_range.bin");
  const std::vector<uint8_t> absence_terminal_range_root_hash =
      ReadFile(fixture_dir / "grove_proof_absence_terminal_range_root_hash.bin");
  const std::vector<uint8_t> root_range_proof =
      ReadFile(fixture_dir / "grove_proof_root_range.bin");
  const std::vector<uint8_t> root_range_desc_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_desc.bin");
  const std::vector<uint8_t> root_range_exclusive_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_exclusive.bin");
  const std::vector<uint8_t> root_range_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_to_inclusive.bin");
  const std::vector<uint8_t> root_range_after_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after.bin");
  const std::vector<uint8_t> root_range_to_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_to.bin");
  const std::vector<uint8_t> root_range_from_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_from.bin");
  const std::vector<uint8_t> root_range_after_to_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after_to.bin");
  const std::vector<uint8_t> root_range_after_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after_to_inclusive.bin");
  const std::vector<uint8_t> root_range_full_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_full.bin");
  const std::vector<uint8_t> nested_path_range_full_proof =
      ReadFile(fixture_dir / "grove_proof_nested_path_range_full.bin");
  const std::vector<uint8_t> root_multi_key_proof =
      ReadFile(fixture_dir / "grove_proof_root_multi_key.bin");
  const std::vector<uint8_t> root_range_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_to_absent.bin");
  const std::vector<uint8_t> root_range_to_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_to_inclusive_absent.bin");
  const std::vector<uint8_t> root_range_from_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_from_absent.bin");
  const std::vector<uint8_t> root_range_after_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after_absent.bin");
  const std::vector<uint8_t> root_range_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_absent.bin");
  const std::vector<uint8_t> root_range_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_inclusive_absent.bin");
  const std::vector<uint8_t> root_range_after_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after_to_absent.bin");
  const std::vector<uint8_t> root_range_after_to_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_root_range_after_to_inclusive_absent.bin");
  const std::vector<uint8_t> subquery_without_parent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_without_parent.bin");
  const std::vector<uint8_t> subquery_with_parent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_with_parent.bin");
  const std::vector<uint8_t> conditional_subquery_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery.bin");
  const std::vector<uint8_t> subquery_path_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path.bin");
  const std::vector<uint8_t> subquery_path_range_after_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after.bin");
  const std::vector<uint8_t> subquery_path_range_after_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_absent.bin");
  const std::vector<uint8_t> subquery_path_range_after_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_limit.bin");
  const std::vector<uint8_t> subquery_path_range_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to_inclusive.bin");
  const std::vector<uint8_t> subquery_path_range_to_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to_inclusive_absent.bin");
  const std::vector<uint8_t> subquery_path_range_to_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to.bin");
  const std::vector<uint8_t> subquery_path_range_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to_absent.bin");
  const std::vector<uint8_t> subquery_path_range_to_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to_limit.bin");
  const std::vector<uint8_t> subquery_path_range_from_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_from.bin");
  const std::vector<uint8_t> subquery_path_range_from_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_from_limit.bin");
  const std::vector<uint8_t> subquery_path_range_from_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_from_absent.bin");
  const std::vector<uint8_t> subquery_path_range_from_limit_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_from_limit_absent.bin");
  const std::vector<uint8_t> subquery_path_range_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range.bin");
  const std::vector<uint8_t> subquery_path_range_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_absent.bin");
  const std::vector<uint8_t> subquery_path_range_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_limit.bin");
  const std::vector<uint8_t> subquery_path_range_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_inclusive.bin");
  const std::vector<uint8_t> subquery_path_range_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_inclusive_absent.bin");
  const std::vector<uint8_t> subquery_path_range_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_inclusive_limit.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to_absent.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to_limit.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to_inclusive.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to_inclusive_absent.bin");
  const std::vector<uint8_t> subquery_path_with_parent_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_with_parent.bin");
  const std::vector<uint8_t> subquery_path_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_limit.bin");
  const std::vector<uint8_t> subquery_path_limit_desc_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_limit_desc.bin");
  const std::vector<uint8_t> reference_chain_wrapped_limit_proof =
      ReadFile(fixture_dir / "grove_proof_reference_chain_wrapped_limit.bin");
  const std::vector<uint8_t> reference_hop_limit_proof =
      ReadFile(fixture_dir / "grove_proof_reference_hop_limit.bin");
  const std::vector<uint8_t> reference_hop_limit_materialized_chain_proof =
      ReadFile(fixture_dir / "grove_proof_reference_hop_limit_materialized_chain.bin");
  const std::vector<uint8_t> reference_conditional_subquery_limit_proof =
      ReadFile(fixture_dir / "grove_proof_reference_conditional_subquery_limit.bin");
  const std::vector<uint8_t> reference_chain_subset_proof =
      ReadFile(fixture_dir / "grove_proof_reference_chain_subset.bin");
  const std::vector<uint8_t> reference_empty_key_sibling_proof =
      ReadFile(fixture_dir / "grove_proof_reference_empty_key_sibling.bin");
  const std::vector<uint8_t> reference_empty_key_sibling_root_hash =
      ReadFile(fixture_dir / "grove_proof_reference_empty_key_sibling_root_hash.bin");
  const std::vector<uint8_t> conditional_subquery_path_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_limit_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_limit_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_to_inclusive.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_inclusive_absent_proof = ReadFile(
      fixture_dir / "grove_proof_conditional_subquery_path_range_to_inclusive_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_from_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_from.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_from_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_from_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_to.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_to_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_inclusive.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_inclusive_absent_proof = ReadFile(
      fixture_dir / "grove_proof_conditional_subquery_path_range_inclusive_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_inclusive_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to_inclusive.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_inclusive_absent_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to_inclusive_absent.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_to_inclusive_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_from_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_from_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_to_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_to_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_inclusive_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to_limit.bin");
  const std::vector<uint8_t> conditional_subquery_path_range_after_to_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_conditional_subquery_path_range_after_to_inclusive_limit.bin");
  const std::vector<uint8_t> nested_conditional_subquery_proof =
      ReadFile(fixture_dir / "grove_proof_nested_conditional_subquery.bin");
  const std::vector<uint8_t> reference_nested_conditional_subquery_proof =
      ReadFile(fixture_dir / "grove_proof_reference_nested_conditional_subquery.bin");
  const std::vector<uint8_t> subquery_path_range_to_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_to_inclusive_limit.bin");
  const std::vector<uint8_t> subquery_path_range_after_to_inclusive_limit_proof =
      ReadFile(fixture_dir / "grove_proof_subquery_path_range_after_to_inclusive_limit.bin");
  const std::vector<uint8_t> root_hash = ReadFile(fixture_dir / "grove_root_hash.bin");

  AssertPresentProof(present_proof, root_hash);
  AssertAbsenceProof(absent_proof, root_hash);
  AssertProveQueryForVersionParity(
      fixture_dir, prove_query_for_version_rust_proof, prove_query_for_version_root_hash);
  AssertProveQueryForVersionLayeredParity(fixture_dir,
                                          prove_query_for_version_layered_rust_proof,
                                          prove_query_for_version_layered_root_hash);
  AssertProveQueryForVersionLimitedRangeParity(
      fixture_dir, prove_query_for_version_limit_rust_proof, prove_query_for_version_limit_root_hash);
  AssertProveQueryAbsenceTerminalRangeParity(
      fixture_dir, absence_terminal_range_proof, absence_terminal_range_root_hash);
  AssertAggregateTreeProof(aggregate_tree_proof, aggregate_tree_root_hash);
  AssertRootRangeProof(root_range_proof, root_hash);
  AssertRootRangeDescProof(root_range_desc_proof, root_hash);
  AssertRootRangeExclusiveProof(root_range_exclusive_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_exclusive_proof,
      grovedb::QueryItem::Range({'k', '1'}, {'k', '3'}),
      "root range-exclusive");
  AssertRootRangeToInclusiveProof(root_range_to_inclusive_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_to_inclusive_proof,
      grovedb::QueryItem::RangeToInclusive({'k', '2'}),
      "root range-to-inclusive");
  AssertRootRangeAfterProof(root_range_after_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_proof,
      grovedb::QueryItem::RangeAfter({'k', '1'}),
      "root range-after");
  AssertRootRangeToProof(root_range_to_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir, root_range_to_proof, grovedb::QueryItem::RangeTo({'k', '2'}), "root range-to");
  AssertRootRangeFromProof(root_range_from_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir, root_range_from_proof, grovedb::QueryItem::RangeFrom({'k', '2'}), "root range-from");
  AssertRootRangeAfterToProof(root_range_after_to_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_to_proof,
      grovedb::QueryItem::RangeAfterTo({'k', '1'}, {'k', '3'}),
      "root range-after-to");
  AssertRootRangeAfterToInclusiveProof(root_range_after_to_inclusive_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_to_inclusive_proof,
      grovedb::QueryItem::RangeAfterToInclusive({'k', '1'}, {'k', '3'}),
      "root range-after-to-inclusive");
  AssertRootRangeProveQueryForVersionParityOnRustFixture(
      fixture_dir,
      root_range_after_to_inclusive_proof,
      grovedb::QueryItem::RangeAfterToInclusive({'k', '1'}, {'k', '3'}),
      "root range-after-to-inclusive");
  AssertRootRangeFullProof(root_range_full_proof, root_hash);
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir, root_range_full_proof, grovedb::QueryItem::RangeFull(), "root range-full");
  AssertRootRangeProveQueryForVersionParityOnRustFixture(
      fixture_dir, root_range_full_proof, grovedb::QueryItem::RangeFull(), "root range-full");
  AssertNestedPathRangeFullProof(nested_path_range_full_proof, root_hash);
  AssertRootMultiKeyProof(root_multi_key_proof, root_hash);
  AssertRootRangeAbsenceProof(
      root_range_to_absent_proof, root_hash, grovedb::QueryItem::RangeTo({'k', '0'}),
      "root range-to absence");
  AssertRootRangeAbsenceProof(
      root_range_to_inclusive_absent_proof, root_hash,
      grovedb::QueryItem::RangeToInclusive({'k', '0'}), "root range-to-inclusive absence");
  AssertRootRangeAbsenceProof(
      root_range_from_absent_proof, root_hash, grovedb::QueryItem::RangeFrom({'k', 'z'}),
      "root range-from absence");
  AssertRootRangeAbsenceProof(
      root_range_after_absent_proof, root_hash, grovedb::QueryItem::RangeAfter({'k', 'e', 'y', '_', 'b'}),
      "root range-after absence");
  AssertRootRangeAbsenceProof(
      root_range_absent_proof, root_hash, grovedb::QueryItem::Range({'k', '0'}, {'k', '1'}),
      "root range absence");
  AssertRootRangeAbsenceProof(
      root_range_inclusive_absent_proof, root_hash,
      grovedb::QueryItem::RangeInclusive({'k', '0'}, {'k', '0'}),
      "root range-inclusive absence");
  AssertRootRangeAbsenceProof(
      root_range_after_to_absent_proof, root_hash,
      grovedb::QueryItem::RangeAfterTo({'k', 'e', 'y', '_', 'b'}, {'k', 'z'}),
      "root range-after-to absence");
  AssertRootRangeAbsenceProof(
      root_range_after_to_inclusive_absent_proof, root_hash,
      grovedb::QueryItem::RangeAfterToInclusive({'k', 'e', 'y', '_', 'b'}, {'k', 'z'}),
      "root range-after-to-inclusive absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir, root_range_from_absent_proof, grovedb::QueryItem::RangeFrom({'k', 'z'}),
      "root range-from absence");
  AssertRootRangeProveQueryForVersionParityOnRustFixture(
      fixture_dir,
      root_range_from_absent_proof,
      grovedb::QueryItem::RangeFrom({'k', 'z'}),
      "root range-from absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_to_absent_proof,
      grovedb::QueryItem::RangeTo({'k', '0'}),
      "root range-to absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_to_inclusive_absent_proof,
      grovedb::QueryItem::RangeToInclusive({'k', '0'}),
      "root range-to-inclusive absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_absent_proof,
      grovedb::QueryItem::RangeAfter({'k', 'e', 'y', '_', 'b'}),
      "root range-after absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_to_absent_proof,
      grovedb::QueryItem::RangeAfterTo({'k', 'e', 'y', '_', 'b'}, {'k', 'z'}),
      "root range-after-to absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_after_to_inclusive_absent_proof,
      grovedb::QueryItem::RangeAfterToInclusive({'k', 'e', 'y', '_', 'b'}, {'k', 'z'}),
      "root range-after-to-inclusive absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_absent_proof,
      grovedb::QueryItem::Range({'k', '0'}, {'k', '1'}),
      "root range absence");
  AssertRootRangeAbsenceProveQueryParityOnRustFixture(
      fixture_dir,
      root_range_inclusive_absent_proof,
      grovedb::QueryItem::RangeInclusive({'k', '0'}, {'k', '0'}),
      "root range-inclusive absence");
  AssertSubqueryParentBehavior(subquery_without_parent_proof, false, root_hash);
  AssertSubqueryParentBehavior(subquery_with_parent_proof, true, root_hash);
  AssertConditionalSubqueryOverride(conditional_subquery_proof, root_hash);
  AssertSubqueryPathTraversal(subquery_path_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeFull()),
      "subquery path range-full");
  AssertSubqueryPathRangeAfterTraversal(subquery_path_range_after_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfter({'x'})),
      "subquery path range-after");
  AssertSubqueryPathRangeAfterAbsenceTraversal(subquery_path_range_after_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfter({'y'})),
      "subquery path range-after absence");
  AssertSubqueryPathRangeAfterLimitTraversal(subquery_path_range_after_limit_proof, root_hash);
  AssertSubqueryPathRangeToInclusiveTraversal(subquery_path_range_to_inclusive_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_to_inclusive_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeToInclusive({'x'})),
      "subquery path range-to-inclusive");
  AssertSubqueryPathRangeToInclusiveAbsenceTraversal(
      subquery_path_range_to_inclusive_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_to_inclusive_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeToInclusive({'w'})),
      "subquery path range-to-inclusive absence");
  AssertSubqueryPathRangeToTraversal(subquery_path_range_to_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_to_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeTo({'y'})),
      "subquery path range-to");
  AssertSubqueryPathRangeToAbsenceTraversal(subquery_path_range_to_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_to_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeTo({'x'})),
      "subquery path range-to absence");
  AssertSubqueryPathRangeToLimitTraversal(subquery_path_range_to_limit_proof, root_hash);
  AssertSubqueryPathRangeFromTraversal(subquery_path_range_from_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_from_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeFrom({'y'})),
      "subquery path range-from");
  AssertSubqueryPathRangeFromLimitTraversal(subquery_path_range_from_limit_proof, root_hash);
  AssertSubqueryPathRangeFromAbsenceTraversal(subquery_path_range_from_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_from_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeFrom({'z'})),
      "subquery path range-from absence");
  AssertSubqueryPathRangeFromLimitAbsenceTraversal(subquery_path_range_from_limit_absent_proof, root_hash);
  AssertSubqueryPathRangeTraversal(subquery_path_range_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::Range({'x'}, {'y'})),
      "subquery path range");
  AssertSubqueryPathRangeLimitTraversal(subquery_path_range_limit_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_limit_proof,
      MakeRootKeyBranchSubqueryPathQueryWithLimit(grovedb::QueryItem::Range({'a'}, {'z'}), 1),
      "subquery path range limit");
  AssertPathQueryProveQueryForVersionParityOnRustFixture(
      fixture_dir,
      subquery_path_range_limit_proof,
      MakeRootKeyBranchSubqueryPathQueryWithLimit(grovedb::QueryItem::Range({'a'}, {'z'}), 1),
      "subquery path range limit");
  AssertSubqueryPathRangeAbsenceTraversal(subquery_path_range_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::Range({'w'}, {'x'})),
      "subquery path range absence");
  AssertSubqueryPathRangeInclusiveTraversal(subquery_path_range_inclusive_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_inclusive_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeInclusive({'x'}, {'y'})),
      "subquery path range-inclusive");
  AssertSubqueryPathRangeInclusiveAbsenceTraversal(
      subquery_path_range_inclusive_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_inclusive_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeInclusive({'w'}, {'w'})),
      "subquery path range-inclusive absence");
  AssertSubqueryPathRangeInclusiveLimitTraversal(
      subquery_path_range_inclusive_limit_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_inclusive_limit_proof,
      MakeRootKeyBranchSubqueryPathQueryWithLimit(
          grovedb::QueryItem::RangeInclusive({'x'}, {'y'}),
          1),
      "subquery path range-inclusive limit");
  AssertPathQueryProveQueryForVersionParityOnRustFixture(
      fixture_dir,
      subquery_path_range_inclusive_limit_proof,
      MakeRootKeyBranchSubqueryPathQueryWithLimit(
          grovedb::QueryItem::RangeInclusive({'x'}, {'y'}),
          1),
      "subquery path range-inclusive limit");
  AssertSubqueryPathRangeAfterToTraversal(subquery_path_range_after_to_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_to_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfterTo({'x'}, {'y'})),
      "subquery path range-after-to");
  AssertSubqueryPathRangeAfterToAbsenceTraversal(
      subquery_path_range_after_to_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_to_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfterTo({'y'}, {'z'})),
      "subquery path range-after-to absence");
  AssertSubqueryPathRangeAfterToLimitTraversal(
      subquery_path_range_after_to_limit_proof, root_hash);
  AssertSubqueryPathRangeAfterToInclusiveTraversal(subquery_path_range_after_to_inclusive_proof,
                                                   root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_to_inclusive_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'})),
      "subquery path range-after-to-inclusive");
  AssertSubqueryPathRangeAfterToInclusiveAbsenceTraversal(
      subquery_path_range_after_to_inclusive_absent_proof, root_hash);
  AssertPathQueryProveQueryParityOnRustFixture(
      fixture_dir,
      subquery_path_range_after_to_inclusive_absent_proof,
      MakeRootKeyBranchSubqueryPathQuery(grovedb::QueryItem::RangeAfterToInclusive({'y'}, {'y'})),
      "subquery path range-after-to-inclusive absence");
  AssertSubqueryPathWithParentTraversal(subquery_path_with_parent_proof, root_hash);
  AssertSubqueryPathLimitTraversal(subquery_path_limit_proof, root_hash);
  AssertSubqueryPathRangeToInclusiveLimitTraversal(
      subquery_path_range_to_inclusive_limit_proof, root_hash);
  AssertSubqueryPathRangeAfterToInclusiveLimitTraversal(
      subquery_path_range_after_to_inclusive_limit_proof, root_hash);
  AssertSubqueryPathLimitDescTraversal(subquery_path_limit_desc_proof, root_hash);
  AssertReferenceChainWrappedLimitTraversal(reference_chain_wrapped_limit_proof, root_hash);
  AssertReferenceHopLimitRejected(reference_hop_limit_proof, root_hash);
  AssertReferenceHopLimitMaterializedChain(reference_hop_limit_materialized_chain_proof, root_hash);
  AssertReferenceConditionalSubqueryLimitTraversal(
      reference_conditional_subquery_limit_proof, root_hash);
  AssertReferenceChainSubsetVerification(reference_chain_subset_proof, root_hash);
  AssertReferenceEmptyKeySiblingTraversal(reference_empty_key_sibling_proof,
                                          reference_empty_key_sibling_root_hash);
  AssertConditionalSubqueryPathTraversal(conditional_subquery_path_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterTraversal(conditional_subquery_path_range_after_proof,
                                                   root_hash);
  AssertConditionalSubqueryPathRangeAfterAbsenceTraversal(
      conditional_subquery_path_range_after_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterLimitTraversal(
      conditional_subquery_path_range_after_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterLimitAbsenceTraversal(
      conditional_subquery_path_range_after_limit_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeToInclusiveTraversal(
      conditional_subquery_path_range_to_inclusive_proof, root_hash);
  AssertConditionalSubqueryPathRangeToInclusiveAbsenceTraversal(
      conditional_subquery_path_range_to_inclusive_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeFromTraversal(conditional_subquery_path_range_from_proof,
                                                  root_hash);
  AssertConditionalSubqueryPathRangeFromAbsenceTraversal(
      conditional_subquery_path_range_from_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeToTraversal(conditional_subquery_path_range_to_proof,
                                                root_hash);
  AssertConditionalSubqueryPathRangeToAbsenceTraversal(
      conditional_subquery_path_range_to_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeTraversal(conditional_subquery_path_range_proof, root_hash);
  AssertConditionalSubqueryPathRangeAbsenceTraversal(
      conditional_subquery_path_range_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeInclusiveTraversal(
      conditional_subquery_path_range_inclusive_proof, root_hash);
  AssertConditionalSubqueryPathRangeInclusiveAbsenceTraversal(
      conditional_subquery_path_range_inclusive_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToTraversal(
      conditional_subquery_path_range_after_to_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToAbsenceTraversal(
      conditional_subquery_path_range_after_to_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToInclusiveTraversal(
      conditional_subquery_path_range_after_to_inclusive_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToInclusiveAbsenceTraversal(
      conditional_subquery_path_range_after_to_inclusive_absent_proof, root_hash);
  AssertConditionalSubqueryPathRangeToInclusiveLimitTraversal(
      conditional_subquery_path_range_to_inclusive_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeFromLimitTraversal(
      conditional_subquery_path_range_from_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeToLimitTraversal(
      conditional_subquery_path_range_to_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeLimitTraversal(
      conditional_subquery_path_range_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeInclusiveLimitTraversal(
      conditional_subquery_path_range_inclusive_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToLimitTraversal(
      conditional_subquery_path_range_after_to_limit_proof, root_hash);
  AssertConditionalSubqueryPathRangeAfterToInclusiveLimitTraversal(
      conditional_subquery_path_range_after_to_inclusive_limit_proof, root_hash);
  AssertNestedConditionalSubqueryTraversal(nested_conditional_subquery_proof, root_hash);
  AssertReferenceNestedConditionalSubqueryTraversal(
      reference_nested_conditional_subquery_proof, root_hash);
  AssertChainedPathQueryVerification(subquery_path_proof, root_hash);
  AssertDecodedLayerStructure(subquery_path_proof, true);
  AssertDecodedLayerStructure(conditional_subquery_path_proof, true);
  AssertZeroOffsetAccepted(present_proof, root_hash);
  AssertNonZeroOffsetRejected(present_proof);
  AssertProveQueryGenerationOffsetAndLimitContracts(fixture_dir);
  AssertDecodeAndVerifyRejectMalformed(present_proof);

  // VerifySubsetQuery parity tests
  AssertSubsetQuery(present_proof, root_hash);
  AssertSubsetQueryForVersion(present_proof, root_hash, grovedb::GroveVersion::V4_0_0());
  AssertSubsetQueryForVersion(present_proof, root_hash, grovedb::GroveVersion{4, 1, 0});
  AssertSubsetQueryForVersion(present_proof, root_hash, grovedb::GroveVersion{5, 0, 0});

  // VerifyQueryWithAbsenceProof parity tests
  AssertQueryWithAbsenceProof(present_proof, root_hash);
  AssertQueryWithAbsenceProofMissingKey(absent_proof, root_hash);
  AssertQueryWithAbsenceProofBoundedRangeTerminalExpansion(
      absence_terminal_range_proof, absence_terminal_range_root_hash);
  AssertQueryWithAbsenceProofRejectsUnsupportedRangeTerminalKeys(
      root_range_to_absent_proof, root_range_absent_proof);
  AssertQueryWithAbsenceProofForVersion(present_proof, root_hash, grovedb::GroveVersion::V4_0_0());
  AssertQueryWithAbsenceProofForVersion(present_proof, root_hash, grovedb::GroveVersion{4, 1, 0});
  AssertQueryWithAbsenceProofForVersion(present_proof, root_hash, grovedb::GroveVersion{5, 0, 0});
  AssertSubsetQueryWithAbsenceProof(present_proof, root_hash);
  AssertSubsetQueryWithAbsenceProofForVersion(
      present_proof, root_hash, grovedb::GroveVersion::V4_0_0());
  AssertSubsetQueryWithAbsenceProofForVersion(
      present_proof, root_hash, grovedb::GroveVersion{4, 1, 0});
  AssertSubsetQueryWithAbsenceProofForVersion(
      present_proof, root_hash, grovedb::GroveVersion{5, 0, 0});
  AssertAbsenceProofRequiresLimit(absent_proof);

  // VerifyQueryGetParentTreeInfo parity tests
  const std::vector<uint8_t> subtree_parent_tree_info_proof =
      ReadFile(fixture_dir / "grove_proof_subtree_parent_tree_info.bin");
  AssertVerifyQueryGetParentTreeInfo(subtree_parent_tree_info_proof, root_hash);
  AssertVerifyQueryGetParentTreeInfoForVersion(
      subtree_parent_tree_info_proof, root_hash, grovedb::GroveVersion::V4_0_0());
  AssertVerifyQueryGetParentTreeInfoForVersion(
      subtree_parent_tree_info_proof, root_hash, grovedb::GroveVersion{4, 1, 0});
  AssertVerifyQueryGetParentTreeInfoForVersion(
      subtree_parent_tree_info_proof, root_hash, grovedb::GroveVersion{5, 0, 0});

  return 0;
}
