#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "element.h"
#include "grovedb.h"
#include "hex.h"
#include "proof.h"
#include "query.h"
#include "test_utils.h"

namespace {

using test_utils::Fail;

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  return out;
}

std::vector<std::vector<uint8_t>> ParsePath(const std::string& token) {
  std::vector<std::vector<uint8_t>> out;
  if (token == "-") {
    return out;
  }
  for (const auto& part : Split(token, '/')) {
    out.push_back(std::vector<uint8_t>(part.begin(), part.end()));
  }
  return out;
}

std::vector<uint8_t> ToBytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> ReadHex(const std::string& hex) {
  std::vector<uint8_t> out;
  std::string error;
  if (!grovedb::DecodeHex(hex, &out, &error)) {
    Fail("failed to decode hex: " + error);
  }
  return out;
}

std::string BytesToString(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

std::string PathToString(const std::vector<std::vector<uint8_t>>& path) {
  if (path.empty()) return "/";
  std::string out;
  for (size_t i = 0; i < path.size(); ++i) {
    if (i > 0) out += "/";
    out += BytesToString(path[i]);
  }
  return out;
}

void RunOneScenario(uint64_t seed, int batches, int queries) {
  const std::string rust_dir =
      test_utils::MakeTempDir("rust_grovedb_diff_fuzz_rust_" + std::to_string(seed));
  const std::string cpp_dir =
      test_utils::MakeTempDir("rust_grovedb_diff_fuzz_cpp_" + std::to_string(seed));

  std::string cmd = test_utils::RustToolsCargoRunPrefix() +
                    "rust_grovedb_differential_fuzz_writer \"" + rust_dir + "\" " +
                    std::to_string(seed) + " " + std::to_string(batches) + " " +
                    std::to_string(queries);
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust differential fuzz writer");
  }

  grovedb::GroveDb db;
  std::string error;
  if (!db.Open(cpp_dir, &error)) {
    Fail("Open failed: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree(root) failed: " + error);
  }

  std::ifstream ops_in(std::filesystem::path(rust_dir) / "ops.txt");
  std::ifstream states_in(std::filesystem::path(rust_dir) / "states.txt");
  if (!ops_in) {
    Fail("missing ops transcript file");
  }
  if (!states_in) {
    Fail("missing states transcript file");
  }

  std::string line;
  std::getline(ops_in, line);  // SEED ...
  int batch_index = 0;
  while (std::getline(ops_in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream batch_header(line);
    std::string marker;
    int disable = 0;
    int use_tx = 0;
    batch_header >> marker >> disable;
    if (!(batch_header >> use_tx)) {
      use_tx = 0;
    }
    if (marker != "BATCH") {
      Fail("expected BATCH marker, got: " + line);
    }

    std::vector<grovedb::GroveDb::BatchOp> ops;
    while (std::getline(ops_in, line)) {
      if (line == "END") {
        break;
      }
      std::istringstream op_ss(line);
      std::string op_marker;
      std::string op_kind;
      std::string path_token;
      std::string key;
      op_ss >> op_marker >> op_kind >> path_token >> key;
      if (op_marker != "OP") {
        Fail("expected OP marker, got: " + line);
      }

      grovedb::GroveDb::BatchOp op;
      op.path = ParsePath(path_token);
      op.key = ToBytes(key);
      if (op_kind == "PUT_ITEM") {
        std::string value;
        op_ss >> value;
        if (value.empty()) {
          Fail("PUT_ITEM missing value");
        }
        op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
        if (!grovedb::EncodeItemToElementBytes(ToBytes(value), &op.element_bytes, &error)) {
          Fail("EncodeItemToElementBytes failed: " + error);
        }
      } else if (op_kind == "PUT_TREE") {
        op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
        if (!grovedb::EncodeTreeToElementBytes(&op.element_bytes, &error)) {
          Fail("EncodeTreeToElementBytes failed: " + error);
        }
      } else if (op_kind == "DELETE") {
        op.kind = grovedb::GroveDb::BatchOp::Kind::kDelete;
      } else {
        Fail("unknown op kind: " + op_kind);
      }
      ops.push_back(std::move(op));
    }

    grovedb::GroveDb::BatchApplyOptions options;
    options.disable_operation_consistency_check = (disable == 1);
    grovedb::GroveDb::Transaction batch_tx;
    if (use_tx == 1) {
      if (!db.StartTransaction(&batch_tx, &error)) {
        Fail("StartTransaction failed at batch " + std::to_string(batch_index) + ": " + error);
      }
      if (!db.ApplyBatch(ops, options, &batch_tx, &error)) {
        Fail("ApplyBatch(tx) failed at batch " + std::to_string(batch_index) + ": " + error);
      }
    } else {
      if (!db.ApplyBatch(ops, options, &error)) {
        Fail("ApplyBatch failed at batch " + std::to_string(batch_index) + ": " + error);
      }
    }

    std::string state_line;
    if (!std::getline(states_in, state_line)) {
      Fail("missing state line for batch " + std::to_string(batch_index));
    }
    std::istringstream state_ss(state_line);
    std::string state_marker;
    state_ss >> state_marker;
    if (state_marker != "STATE") {
      Fail("expected STATE marker, got: " + state_line);
    }
    std::string state_token;
    while (state_ss >> state_token) {
      const auto eq = state_token.find('=');
      if (eq == std::string::npos) {
        Fail("invalid state token: " + state_token);
      }
      const std::string key = state_token.substr(0, eq);
      const std::string expected_state = state_token.substr(eq + 1);

      std::vector<uint8_t> actual_element;
      bool found = false;
      if (use_tx == 1) {
        if (!db.GetRawOptional({{'r', 'o', 'o', 't'}},
                               ToBytes(key),
                               &actual_element,
                               &found,
                               &batch_tx,
                               &error)) {
          Fail("GetRawOptional(state,tx) failed: " + error);
        }
      } else {
        if (!db.GetRawOptional({{'r', 'o', 'o', 't'}}, ToBytes(key), &actual_element, &found, &error)) {
          Fail("GetRawOptional(state) failed: " + error);
        }
      }
      if (expected_state == "-") {
        if (found) {
          Fail("state mismatch: expected absent for key " + key +
               " at batch " + std::to_string(batch_index));
        }
        continue;
      }
      if (!found) {
        Fail("state mismatch: expected present for key " + key +
             " at batch " + std::to_string(batch_index));
      }
      grovedb::ElementItem item;
      if (!grovedb::DecodeItemFromElementBytes(actual_element, &item, &error)) {
        Fail("state mismatch: expected item for key " + key +
             " at batch " + std::to_string(batch_index) + ": " + error);
      }
      if (item.value != ReadHex(expected_state)) {
        Fail("state mismatch: item value differs for key " + key +
             " at batch " + std::to_string(batch_index));
      }
    }
    if (use_tx == 1) {
      if (!db.CommitTransaction(&batch_tx, &error)) {
        Fail("CommitTransaction failed at batch " + std::to_string(batch_index) + ": " + error);
      }
    }

    ++batch_index;
  }

  std::ifstream queries_in(std::filesystem::path(rust_dir) / "queries.txt");
  if (!queries_in) {
    Fail("missing queries transcript file");
  }
  while (std::getline(queries_in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream qss(line);
    std::string q_marker;
    std::string q_kind;
    std::string path_token;
    qss >> q_marker >> q_kind >> path_token;
    if (q_marker != "Q") {
      Fail("expected Q marker, got: " + line);
    }
    if (q_kind == "KEY") {
      std::string key;
      std::string proof_hex;
      qss >> key >> proof_hex;
      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(KEY) failed: " + error);
      }

      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(KEY) failed: " + error);
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (found != proof_found) {
        Fail("proof semantic mismatch for KEY query (presence differs)");
      }
      if (found && proof_element != actual_element) {
        Fail("proof semantic mismatch for KEY query (element bytes differ)");
      }
    } else if (q_kind == "KEY_TX") {
      std::string key;
      std::string stage_kind;
      std::string stage_value_hex;
      std::string expected_state;
      qss >> key >> stage_kind >> stage_value_hex >> expected_state;
      if (stage_kind.empty()) {
        Fail("KEY_TX missing stage kind");
      }
      if (expected_state.empty()) {
        Fail("KEY_TX missing expected state");
      }

      grovedb::GroveDb::Transaction tx;
      if (!db.StartTransaction(&tx, &error)) {
        Fail("StartTransaction(KEY_TX) failed: " + error);
      }
      if (stage_kind == "PUT_ITEM") {
        if (stage_value_hex.empty() || stage_value_hex == "-") {
          Fail("KEY_TX PUT_ITEM missing value");
        }
        grovedb::GroveDb::BatchOp op;
        op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
        op.path = ParsePath(path_token);
        op.key = ToBytes(key);
        if (!grovedb::EncodeItemToElementBytes(ReadHex(stage_value_hex), &op.element_bytes, &error)) {
          Fail("EncodeItemToElementBytes(KEY_TX) failed: " + error);
        }
        if (!db.ApplyBatch({op}, &tx, &error)) {
          Fail("ApplyBatch(KEY_TX PUT_ITEM) failed: " + error);
        }
      } else if (stage_kind == "DELETE") {
        grovedb::GroveDb::BatchOp op;
        op.kind = grovedb::GroveDb::BatchOp::Kind::kDelete;
        op.path = ParsePath(path_token);
        op.key = ToBytes(key);
        if (!db.ApplyBatch({op}, &tx, &error)) {
          Fail("ApplyBatch(KEY_TX DELETE) failed: " + error);
        }
      } else {
        Fail("unknown KEY_TX stage kind: " + stage_kind);
      }

      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(
              ParsePath(path_token), ToBytes(key), &actual_element, &found, &tx, &error)) {
        Fail("GetRawOptional(KEY_TX) failed: " + error);
      }
      if (expected_state == "-") {
        if (found) {
          Fail("tx semantic mismatch for KEY_TX query (expected absent)");
        }
      } else {
        if (!found) {
          Fail("tx semantic mismatch for KEY_TX query (expected present)");
        }
        grovedb::ElementItem item;
        if (!grovedb::DecodeItemFromElementBytes(actual_element, &item, &error)) {
          Fail("tx semantic mismatch for KEY_TX query: expected item decode to succeed: " + error);
        }
        if (item.value != ReadHex(expected_state)) {
          Fail("tx semantic mismatch for KEY_TX query (value differs)");
        }
      }
      if (!db.RollbackTransaction(&tx, &error)) {
        Fail("RollbackTransaction(KEY_TX) failed: " + error);
      }
    } else if (q_kind == "SUBQ_PATH") {
      std::string proof_hex;
      qss >> proof_hex;
      if (proof_hex.empty()) {
        Fail("SUBQ_PATH missing proof");
      }

      if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'s', 'k', 'e', 'y'}, &error)) {
        Fail("InsertEmptyTree(SUBQ_PATH skey) failed: " + error);
      }
      if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'s', 'k', 'e', 'y'}},
                              {'b', 'r', 'a', 'n', 'c', 'h'},
                              &error)) {
        Fail("InsertEmptyTree(SUBQ_PATH branch) failed: " + error);
      }
      std::vector<uint8_t> sub_a;
      if (!grovedb::EncodeItemToElementBytes({'s', 'u', 'b', '_', 'a'}, &sub_a, &error)) {
        Fail("EncodeItemToElementBytes(SUBQ_PATH sub_a) failed: " + error);
      }
      if (!db.Insert(
              {{'r', 'o', 'o', 't'}, {'s', 'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
              {'x', 'a'},
              sub_a,
              &error)) {
        Fail("Insert(SUBQ_PATH xa) failed: " + error);
      }
      std::vector<uint8_t> sub_b;
      if (!grovedb::EncodeItemToElementBytes({'s', 'u', 'b', '_', 'b'}, &sub_b, &error)) {
        Fail("EncodeItemToElementBytes(SUBQ_PATH sub_b) failed: " + error);
      }
      if (!db.Insert(
              {{'r', 'o', 'o', 't'}, {'s', 'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
              {'x', 'b'},
              sub_b,
              &error)) {
        Fail("Insert(SUBQ_PATH xb) failed: " + error);
      }

      grovedb::Query layered_query = grovedb::Query::NewSingleKey({'s', 'k', 'e', 'y'});
      layered_query.default_subquery_branch.subquery_path =
          std::vector<std::vector<uint8_t>>({{'b', 'r', 'a', 'n', 'c', 'h'}});
      layered_query.default_subquery_branch.subquery =
          std::make_unique<grovedb::Query>(
              grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
      auto path_query = grovedb::PathQuery::New(
          ParsePath(path_token),
          grovedb::SizedQuery::New(layered_query, std::nullopt, std::nullopt));

      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, path_query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(SUBQ_PATH) failed: " + error);
      }

      const std::vector<std::vector<uint8_t>> expected_path = {
          {'r', 'o', 'o', 't'},
          {'s', 'k', 'e', 'y'},
          {'b', 'r', 'a', 'n', 'c', 'h'},
      };
      bool found_xa = false;
      bool found_xb = false;
      for (const auto& e : verified_elements) {
        if (e.path == expected_path && e.key == std::vector<uint8_t>({'x', 'a'}) &&
            e.has_element) {
          found_xa = true;
        }
        if (e.path == expected_path && e.key == std::vector<uint8_t>({'x', 'b'}) &&
            e.has_element) {
          found_xb = true;
        }
        if (!e.has_element) {
          continue;
        }
        std::vector<uint8_t> actual_element;
        bool found = false;
        if (!db.GetRawOptional(e.path, e.key, &actual_element, &found, &error)) {
          Fail("GetRawOptional(SUBQ_PATH) failed: " + error);
        }
        if (!found) {
          Fail("SUBQ_PATH semantic mismatch: proof has present element missing in C++ db at " +
               PathToString(e.path) + " key=" + BytesToString(e.key));
        }
        if (actual_element != e.element_bytes) {
          Fail("SUBQ_PATH semantic mismatch: element bytes differ");
        }
      }
      if (!found_xa || !found_xb) {
        Fail("SUBQ_PATH semantic mismatch: expected xa/xb entries not found in proof");
      }
    } else if (q_kind == "COND_SUBQ") {
      std::string proof_hex;
      qss >> proof_hex;
      if (proof_hex.empty()) {
        Fail("COND_SUBQ missing proof");
      }

      if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', 'k', 'e', 'y'}, &error)) {
        Fail("InsertEmptyTree(COND_SUBQ ckey) failed: " + error);
      }
      std::vector<uint8_t> cond_a;
      if (!grovedb::EncodeItemToElementBytes({'c', 'o', 'n', 'd', '_', 'a'}, &cond_a, &error)) {
        Fail("EncodeItemToElementBytes(COND_SUBQ cond_a) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'k', 'e', 'y'}}, {'c', 'a'}, cond_a, &error)) {
        Fail("Insert(COND_SUBQ ckey/ca) failed: " + error);
      }
      std::vector<uint8_t> cond_b;
      if (!grovedb::EncodeItemToElementBytes({'c', 'o', 'n', 'd', '_', 'b'}, &cond_b, &error)) {
        Fail("EncodeItemToElementBytes(COND_SUBQ cond_b) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'k', 'e', 'y'}}, {'c', 'b'}, cond_b, &error)) {
        Fail("Insert(COND_SUBQ ckey/cb) failed: " + error);
      }
      std::vector<uint8_t> cond_leaf;
      if (!grovedb::EncodeItemToElementBytes(
              {'c', 'o', 'n', 'd', '_', 'l', 'e', 'a', 'f'}, &cond_leaf, &error)) {
        Fail("EncodeItemToElementBytes(COND_SUBQ cond_leaf) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'k', '2'}, cond_leaf, &error)) {
        Fail("Insert(COND_SUBQ ck2 item) failed: " + error);
      }

      grovedb::Query conditional_query;
      conditional_query.items.push_back(grovedb::QueryItem::Key({'c', 'k', 'e', 'y'}));
      conditional_query.items.push_back(grovedb::QueryItem::Key({'c', 'k', '2'}));
      conditional_query.default_subquery_branch.subquery =
          std::make_unique<grovedb::Query>(
              grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull()));
      conditional_query.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
      conditional_query.conditional_subquery_branches->push_back(
          {grovedb::QueryItem::Key({'c', 'k', '2'}), grovedb::SubqueryBranch()});
      auto path_query = grovedb::PathQuery::New(
          ParsePath(path_token),
          grovedb::SizedQuery::New(conditional_query, std::nullopt, std::nullopt));

      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, path_query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(COND_SUBQ) failed: " + error);
      }

      bool found_cond_a = false;
      for (const auto& e : verified_elements) {
        if (e.path == std::vector<std::vector<uint8_t>>(
                          {{'r', 'o', 'o', 't'}, {'c', 'k', 'e', 'y'}}) &&
            e.key == std::vector<uint8_t>({'c', 'a'}) && e.has_element) {
          found_cond_a = true;
        }
        if (!e.has_element) {
          continue;
        }
        std::vector<uint8_t> actual_element;
        bool found = false;
        if (!db.GetRawOptional(e.path, e.key, &actual_element, &found, &error)) {
          Fail("GetRawOptional(COND_SUBQ) failed: " + error);
        }
        if (!found) {
          Fail("COND_SUBQ semantic mismatch: proof has present element missing in C++ db at " +
               PathToString(e.path) + " key=" + BytesToString(e.key));
        }
        if (actual_element != e.element_bytes) {
          Fail("COND_SUBQ semantic mismatch: element bytes differ");
        }
      }
      if (!found_cond_a) {
        Fail("COND_SUBQ semantic mismatch: expected ckey/ca entry not found in proof");
      }
    } else if (q_kind == "BAD_PROOF") {
      std::string proof_hex;
      qss >> proof_hex;
      if (proof_hex.empty()) {
        Fail("BAD_PROOF missing payload");
      }
      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), {'k', '0', '0'});
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> malformed = ReadHex(proof_hex);
      if (grovedb::VerifyPathQueryProof(
              malformed, query, &root_hash, &verified_elements, &error)) {
        Fail("BAD_PROOF expected VerifyPathQueryProof to fail");
      }
    } else if (q_kind == "AGG_KEY") {
      std::string key;
      std::string proof_hex;
      qss >> key >> proof_hex;
      if (key.empty() || proof_hex.empty()) {
        Fail("AGG_KEY missing key/proof");
      }

      std::vector<uint8_t> sum_tree_raw;
      if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &sum_tree_raw, &error)) {
        Fail("EncodeSumTreeToElementBytesWithRootKey(AGG_KEY) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'a', 'g', 'g', 't'}, sum_tree_raw, &error)) {
        Fail("Insert(AGG_KEY aggt tree) failed: " + error);
      }
      std::vector<uint8_t> s1_raw;
      if (!grovedb::EncodeSumItemToElementBytes(3, &s1_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_KEY s1) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 't'}}, {'s', '1'}, s1_raw, &error)) {
        Fail("Insert(AGG_KEY s1) failed: " + error);
      }
      std::vector<uint8_t> s2_raw;
      if (!grovedb::EncodeSumItemToElementBytes(4, &s2_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_KEY s2) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 't'}}, {'s', '2'}, s2_raw, &error)) {
        Fail("Insert(AGG_KEY s2) failed: " + error);
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_KEY) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_KEY) failed: " + error);
      }
      if (!found) {
        Fail("AGG_KEY semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_KEY semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_KEY semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_KEY_GEN") {
      std::string key;
      std::string proof_hex;
      std::string spec;
      qss >> key >> proof_hex >> spec;
      if (key.empty() || proof_hex.empty() || spec.empty()) {
        Fail("AGG_KEY_GEN missing key/proof/spec");
      }

      std::vector<uint8_t> sum_tree_raw;
      if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &sum_tree_raw, &error)) {
        Fail("EncodeSumTreeToElementBytesWithRootKey(AGG_KEY_GEN) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'a', 'g', 'g', 'r'}, sum_tree_raw, &error)) {
        Fail("Insert(AGG_KEY_GEN aggr tree) failed: " + error);
      }
      for (const auto& part : Split(spec, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) {
          Fail("AGG_KEY_GEN invalid spec part: " + part);
        }
        const std::string agg_key = part.substr(0, eq);
        const int64_t agg_sum = std::stoll(part.substr(eq + 1));
        std::vector<uint8_t> agg_raw;
        if (!grovedb::EncodeSumItemToElementBytes(agg_sum, &agg_raw, &error)) {
          Fail("EncodeSumItemToElementBytes(AGG_KEY_GEN item) failed: " + error);
        }
        if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 'r'}},
                       ToBytes(agg_key),
                       agg_raw,
                       &error)) {
          Fail("Insert(AGG_KEY_GEN item) failed: " + error);
        }
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_KEY_GEN) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_KEY_GEN) failed: " + error);
      }
      if (!found) {
        Fail("AGG_KEY_GEN semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_KEY_GEN semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_KEY_GEN semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_SUM_RANGE") {
      std::string proof_hex;
      qss >> proof_hex;
      if (proof_hex.empty()) {
        Fail("AGG_SUM_RANGE missing proof");
      }

      std::vector<uint8_t> sum_tree_raw;
      if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &sum_tree_raw, &error)) {
        Fail("EncodeSumTreeToElementBytesWithRootKey(AGG_SUM_RANGE) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'a', 'g', 'g', 't'}, sum_tree_raw, &error)) {
        Fail("Insert(AGG_SUM_RANGE aggt tree) failed: " + error);
      }
      std::vector<uint8_t> s1_raw;
      if (!grovedb::EncodeSumItemToElementBytes(3, &s1_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_SUM_RANGE s1) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 't'}}, {'s', '1'}, s1_raw, &error)) {
        Fail("Insert(AGG_SUM_RANGE s1) failed: " + error);
      }
      std::vector<uint8_t> s2_raw;
      if (!grovedb::EncodeSumItemToElementBytes(4, &s2_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_SUM_RANGE s2) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 't'}}, {'s', '2'}, s2_raw, &error)) {
        Fail("Insert(AGG_SUM_RANGE s2) failed: " + error);
      }

      grovedb::Query query_obj =
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFull());
      auto query = grovedb::PathQuery::New(
          ParsePath(path_token),
          grovedb::SizedQuery::New(query_obj, std::nullopt, std::nullopt));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_SUM_RANGE) failed: " + error);
      }
      bool found_s1 = false;
      bool found_s2 = false;
      for (const auto& e : verified_elements) {
        if (!e.has_element) {
          continue;
        }
        if (e.path == ParsePath(path_token) && e.key == std::vector<uint8_t>({'s', '1'})) {
          found_s1 = true;
        }
        if (e.path == ParsePath(path_token) && e.key == std::vector<uint8_t>({'s', '2'})) {
          found_s2 = true;
        }
        std::vector<uint8_t> actual_element;
        bool found = false;
        if (!db.GetRawOptional(e.path, e.key, &actual_element, &found, &error)) {
          Fail("GetRawOptional(AGG_SUM_RANGE) failed: " + error);
        }
        if (!found) {
          Fail("AGG_SUM_RANGE semantic mismatch: proof element missing in C++ db");
        }
        if (actual_element != e.element_bytes) {
          Fail("AGG_SUM_RANGE semantic mismatch: element bytes differ");
        }
      }
      if (!found_s1 || !found_s2) {
        Fail("AGG_SUM_RANGE semantic mismatch: expected s1/s2 entries not found in proof");
      }
    } else if (q_kind == "AGG_NESTED_KEY") {
      std::string key;
      std::string proof_hex;
      qss >> key >> proof_hex;
      if (key.empty() || proof_hex.empty()) {
        Fail("AGG_NESTED_KEY missing key/proof");
      }

      if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'a', 'g', 'g', 'p'}, &error)) {
        Fail("InsertEmptyTree(AGG_NESTED_KEY aggp) failed: " + error);
      }
      std::vector<uint8_t> inner_tree_raw;
      if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &inner_tree_raw, &error)) {
        Fail("EncodeSumTreeToElementBytesWithRootKey(AGG_NESTED_KEY inner) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 'p'}},
                     {'i', 'n', 'n', 'e', 'r'},
                     inner_tree_raw,
                     &error)) {
        Fail("Insert(AGG_NESTED_KEY inner tree) failed: " + error);
      }
      std::vector<uint8_t> n1_raw;
      if (!grovedb::EncodeSumItemToElementBytes(5, &n1_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_NESTED_KEY n1) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'a', 'g', 'g', 'p'}, {'i', 'n', 'n', 'e', 'r'}},
                     {'n', '1'},
                     n1_raw,
                     &error)) {
        Fail("Insert(AGG_NESTED_KEY n1) failed: " + error);
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_NESTED_KEY) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_NESTED_KEY) failed: " + error);
      }
      if (!found) {
        Fail("AGG_NESTED_KEY semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_NESTED_KEY semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_NESTED_KEY semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_COUNT_KEY") {
      std::string key;
      std::string proof_hex;
      qss >> key >> proof_hex;
      if (key.empty() || proof_hex.empty()) {
        Fail("AGG_COUNT_KEY missing key/proof");
      }

      std::vector<uint8_t> count_tree_raw;
      if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &count_tree_raw, &error)) {
        Fail("EncodeCountTreeToElementBytesWithRootKey(AGG_COUNT_KEY) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'c', 'o', 'u', 'n', 't', 't'}, count_tree_raw, &error)) {
        Fail("Insert(AGG_COUNT_KEY countt tree) failed: " + error);
      }
      std::vector<uint8_t> c1_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'v', '1'}, &c1_raw, &error)) {
        Fail("EncodeItemToElementBytes(AGG_COUNT_KEY c1) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'o', 'u', 'n', 't', 't'}},
                     {'c', '1'},
                     c1_raw,
                     &error)) {
        Fail("Insert(AGG_COUNT_KEY c1) failed: " + error);
      }
      std::vector<uint8_t> c2_raw;
      if (!grovedb::EncodeItemToElementBytes({'c', 'v', '2'}, &c2_raw, &error)) {
        Fail("EncodeItemToElementBytes(AGG_COUNT_KEY c2) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'o', 'u', 'n', 't', 't'}},
                     {'c', '2'},
                     c2_raw,
                     &error)) {
        Fail("Insert(AGG_COUNT_KEY c2) failed: " + error);
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_COUNT_KEY) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_COUNT_KEY) failed: " + error);
      }
      if (!found) {
        Fail("AGG_COUNT_KEY semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_COUNT_KEY semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_COUNT_KEY semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_COUNT_GEN") {
      std::string key;
      std::string proof_hex;
      std::string spec;
      qss >> key >> proof_hex >> spec;
      if (key.empty() || proof_hex.empty() || spec.empty()) {
        Fail("AGG_COUNT_GEN missing key/proof/spec");
      }

      std::vector<uint8_t> count_tree_raw;
      if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &count_tree_raw, &error)) {
        Fail("EncodeCountTreeToElementBytesWithRootKey(AGG_COUNT_GEN) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}},
                     {'c', 'o', 'u', 'n', 't', 'g', 'r'},
                     count_tree_raw,
                     &error)) {
        Fail("Insert(AGG_COUNT_GEN countgr tree) failed: " + error);
      }
      for (const auto& part : Split(spec, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) {
          Fail("AGG_COUNT_GEN invalid spec part: " + part);
        }
        const std::string agg_key = part.substr(0, eq);
        const std::string agg_value_hex = part.substr(eq + 1);
        std::vector<uint8_t> agg_item_raw;
        if (!grovedb::EncodeItemToElementBytes(ReadHex(agg_value_hex), &agg_item_raw, &error)) {
          Fail("EncodeItemToElementBytes(AGG_COUNT_GEN item) failed: " + error);
        }
        if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'o', 'u', 'n', 't', 'g', 'r'}},
                       ToBytes(agg_key),
                       agg_item_raw,
                       &error)) {
          Fail("Insert(AGG_COUNT_GEN item) failed: " + error);
        }
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_COUNT_GEN) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_COUNT_GEN) failed: " + error);
      }
      if (!found) {
        Fail("AGG_COUNT_GEN semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_COUNT_GEN semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_COUNT_GEN semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_COUNT_RANGE_GEN") {
      std::string proof_hex;
      std::string spec;
      qss >> proof_hex >> spec;
      if (proof_hex.empty() || spec.empty()) {
        Fail("AGG_COUNT_RANGE_GEN missing proof/spec");
      }

      std::vector<uint8_t> count_tree_raw;
      if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &count_tree_raw, &error)) {
        Fail("EncodeCountTreeToElementBytesWithRootKey(AGG_COUNT_RANGE_GEN) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}},
                     {'c', 'o', 'u', 'n', 't', 'g', 'r'},
                     count_tree_raw,
                     &error)) {
        Fail("Insert(AGG_COUNT_RANGE_GEN countgr tree) failed: " + error);
      }
      for (const auto& part : Split(spec, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) {
          Fail("AGG_COUNT_RANGE_GEN invalid spec part: " + part);
        }
        const std::string agg_key = part.substr(0, eq);
        const std::string agg_value_hex = part.substr(eq + 1);
        std::vector<uint8_t> agg_item_raw;
        if (!grovedb::EncodeItemToElementBytes(ReadHex(agg_value_hex), &agg_item_raw, &error)) {
          Fail("EncodeItemToElementBytes(AGG_COUNT_RANGE_GEN item) failed: " + error);
        }
        if (!db.Insert({{'r', 'o', 'o', 't'}, {'c', 'o', 'u', 'n', 't', 'g', 'r'}},
                       ToBytes(agg_key),
                       agg_item_raw,
                       &error)) {
          Fail("Insert(AGG_COUNT_RANGE_GEN item) failed: " + error);
        }
      }

      grovedb::Query range_query =
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Range({'r', '0'}, {'r', '9'}));
      auto query = grovedb::PathQuery::New(
          ParsePath(path_token),
          grovedb::SizedQuery::New(range_query, std::nullopt, std::nullopt));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_COUNT_RANGE_GEN) failed: " + error);
      }
      int present_count = 0;
      for (const auto& e : verified_elements) {
        if (!e.has_element) {
          continue;
        }
        ++present_count;
        std::vector<uint8_t> actual_element;
        bool found = false;
        if (!db.GetRawOptional(e.path, e.key, &actual_element, &found, &error)) {
          Fail("GetRawOptional(AGG_COUNT_RANGE_GEN) failed: " + error);
        }
        if (!found || actual_element != e.element_bytes) {
          Fail("AGG_COUNT_RANGE_GEN semantic mismatch: proof/db element mismatch");
        }
      }
      if (present_count < 3) {
        Fail("AGG_COUNT_RANGE_GEN semantic mismatch: expected at least 3 present entries");
      }
    } else if (q_kind == "AGG_BIG_KEY") {
      std::string key;
      std::string proof_hex;
      qss >> key >> proof_hex;
      if (key.empty() || proof_hex.empty()) {
        Fail("AGG_BIG_KEY missing key/proof");
      }

      std::vector<uint8_t> big_tree_raw;
      if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(nullptr, 0, &big_tree_raw, &error)) {
        Fail("EncodeBigSumTreeToElementBytesWithRootKey(AGG_BIG_KEY) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'b', 'i', 'g', 't'}, big_tree_raw, &error)) {
        Fail("Insert(AGG_BIG_KEY bigt tree) failed: " + error);
      }
      std::vector<uint8_t> b1_raw;
      if (!grovedb::EncodeSumItemToElementBytes(9, &b1_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_BIG_KEY b1) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'b', 'i', 'g', 't'}}, {'b', '1'}, b1_raw, &error)) {
        Fail("Insert(AGG_BIG_KEY b1) failed: " + error);
      }
      std::vector<uint8_t> b2_raw;
      if (!grovedb::EncodeSumItemToElementBytes(12, &b2_raw, &error)) {
        Fail("EncodeSumItemToElementBytes(AGG_BIG_KEY b2) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}, {'b', 'i', 'g', 't'}}, {'b', '2'}, b2_raw, &error)) {
        Fail("Insert(AGG_BIG_KEY b2) failed: " + error);
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_BIG_KEY) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_BIG_KEY) failed: " + error);
      }
      if (!found) {
        Fail("AGG_BIG_KEY semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_BIG_KEY semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_BIG_KEY semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_BIG_GEN") {
      std::string key;
      std::string proof_hex;
      std::string spec;
      qss >> key >> proof_hex >> spec;
      if (key.empty() || proof_hex.empty() || spec.empty()) {
        Fail("AGG_BIG_GEN missing key/proof/spec");
      }

      std::vector<uint8_t> big_tree_raw;
      if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(nullptr, 0, &big_tree_raw, &error)) {
        Fail("EncodeBigSumTreeToElementBytesWithRootKey(AGG_BIG_GEN) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'b', 'i', 'g', 'g', 'r'}, big_tree_raw, &error)) {
        Fail("Insert(AGG_BIG_GEN biggr tree) failed: " + error);
      }
      for (const auto& part : Split(spec, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) {
          Fail("AGG_BIG_GEN invalid spec part: " + part);
        }
        const std::string agg_key = part.substr(0, eq);
        const int64_t agg_sum = std::stoll(part.substr(eq + 1));
        std::vector<uint8_t> agg_raw;
        if (!grovedb::EncodeSumItemToElementBytes(agg_sum, &agg_raw, &error)) {
          Fail("EncodeSumItemToElementBytes(AGG_BIG_GEN item) failed: " + error);
        }
        if (!db.Insert({{'r', 'o', 'o', 't'}, {'b', 'i', 'g', 'g', 'r'}},
                       ToBytes(agg_key),
                       agg_raw,
                       &error)) {
          Fail("Insert(AGG_BIG_GEN item) failed: " + error);
        }
      }

      auto query = grovedb::PathQuery::NewSingleKey(ParsePath(path_token), ToBytes(key));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_BIG_GEN) failed: " + error);
      }
      std::vector<uint8_t> actual_element;
      bool found = false;
      if (!db.GetRawOptional(ParsePath(path_token), ToBytes(key), &actual_element, &found, &error)) {
        Fail("GetRawOptional(AGG_BIG_GEN) failed: " + error);
      }
      if (!found) {
        Fail("AGG_BIG_GEN semantic mismatch: expected key missing in C++ db");
      }
      bool proof_found = false;
      std::vector<uint8_t> proof_element;
      for (const auto& e : verified_elements) {
        if (e.path == ParsePath(path_token) && e.key == ToBytes(key) && e.has_element) {
          proof_found = true;
          proof_element = e.element_bytes;
          break;
        }
      }
      if (!proof_found) {
        Fail("AGG_BIG_GEN semantic mismatch: proof missing expected key");
      }
      if (proof_element != actual_element) {
        Fail("AGG_BIG_GEN semantic mismatch: element bytes differ");
      }
    } else if (q_kind == "AGG_BIG_RANGE_GEN") {
      std::string proof_hex;
      std::string spec;
      qss >> proof_hex >> spec;
      if (proof_hex.empty() || spec.empty()) {
        Fail("AGG_BIG_RANGE_GEN missing proof/spec");
      }

      std::vector<uint8_t> big_tree_raw;
      if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(nullptr, 0, &big_tree_raw, &error)) {
        Fail("EncodeBigSumTreeToElementBytesWithRootKey(AGG_BIG_RANGE_GEN) failed: " + error);
      }
      if (!db.Insert({{'r', 'o', 'o', 't'}}, {'b', 'i', 'g', 'g', 'r'}, big_tree_raw, &error)) {
        Fail("Insert(AGG_BIG_RANGE_GEN biggr tree) failed: " + error);
      }
      for (const auto& part : Split(spec, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) {
          Fail("AGG_BIG_RANGE_GEN invalid spec part: " + part);
        }
        const std::string agg_key = part.substr(0, eq);
        const int64_t agg_sum = std::stoll(part.substr(eq + 1));
        std::vector<uint8_t> agg_raw;
        if (!grovedb::EncodeSumItemToElementBytes(agg_sum, &agg_raw, &error)) {
          Fail("EncodeSumItemToElementBytes(AGG_BIG_RANGE_GEN item) failed: " + error);
        }
        if (!db.Insert({{'r', 'o', 'o', 't'}, {'b', 'i', 'g', 'g', 'r'}},
                       ToBytes(agg_key),
                       agg_raw,
                       &error)) {
          Fail("Insert(AGG_BIG_RANGE_GEN item) failed: " + error);
        }
      }

      grovedb::Query range_query =
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Range({'q', '0'}, {'q', '9'}));
      auto query = grovedb::PathQuery::New(
          ParsePath(path_token),
          grovedb::SizedQuery::New(range_query, std::nullopt, std::nullopt));
      std::vector<uint8_t> root_hash;
      std::vector<grovedb::VerifiedPathKeyElement> verified_elements;
      const std::vector<uint8_t> rust_proof = ReadHex(proof_hex);
      if (!grovedb::VerifyPathQueryProof(
              rust_proof, query, &root_hash, &verified_elements, &error)) {
        Fail("VerifyPathQueryProof(AGG_BIG_RANGE_GEN) failed: " + error);
      }
      int present_count = 0;
      for (const auto& e : verified_elements) {
        if (!e.has_element) {
          continue;
        }
        ++present_count;
        std::vector<uint8_t> actual_element;
        bool found = false;
        if (!db.GetRawOptional(e.path, e.key, &actual_element, &found, &error)) {
          Fail("GetRawOptional(AGG_BIG_RANGE_GEN) failed: " + error);
        }
        if (!found || actual_element != e.element_bytes) {
          Fail("AGG_BIG_RANGE_GEN semantic mismatch: proof/db element mismatch");
        }
      }
      if (present_count < 3) {
        Fail("AGG_BIG_RANGE_GEN semantic mismatch: expected at least 3 present entries");
      }
    } else {
      Fail("unknown query kind: " + q_kind);
    }
  }
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  RunOneScenario(/*seed=*/1337, /*batches=*/32, /*queries=*/16);
  RunOneScenario(/*seed=*/20260227, /*batches=*/32, /*queries=*/16);
  return 0;
}
