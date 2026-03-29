#include "element.h"
#include "grovedb.h"
#include "proof.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

void ExpectErrorContains(const std::string& name, const std::string& error, const std::string& needle) {
  if (error.find(needle) == std::string::npos) {
    Fail(name + ": expected error containing '" + needle + "', got '" + error + "'");
  }
}

void ExpectErrorContainsAny(const std::string& name,
                            const std::string& error,
                            const std::vector<std::string>& needles) {
  for (const auto& needle : needles) {
    if (error.find(needle) != std::string::npos) {
      return;
    }
  }
  std::string joined;
  for (size_t i = 0; i < needles.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += "'" + needles[i] + "'";
  }
  Fail(name + ": expected error containing one of [" + joined + "], got '" + error + "'");
}

bool DecodeVarint(const std::vector<uint8_t>& bytes,
                  size_t* cursor,
                  uint64_t* out,
                  std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "decode cursor or output is null";
    }
    return false;
  }
  uint64_t result = 0;
  int shift = 0;
  while (*cursor < bytes.size()) {
    uint8_t byte = bytes[*cursor];
    (*cursor)++;
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 64) {
      if (error) {
        *error = "varint is too large";
      }
      return false;
    }
  }
  if (error) {
    *error = "unexpected end of input while decoding varint";
  }
  return false;
}

bool DecodeVarintU128(const std::vector<uint8_t>& bytes,
                      size_t* cursor,
                      unsigned __int128* out,
                      std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "decode cursor or output is null";
    }
    return false;
  }
  unsigned __int128 result = 0;
  int shift = 0;
  while (*cursor < bytes.size()) {
    uint8_t byte = bytes[*cursor];
    (*cursor)++;
    result |= static_cast<unsigned __int128>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 128) {
      if (error) {
        *error = "varint is too large";
      }
      return false;
    }
  }
  if (error) {
    *error = "unexpected end of input while decoding varint";
  }
  return false;
}

int64_t ZigZagDecodeI64(uint64_t value) {
  return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

struct DecodedTreeElementState {
  uint64_t variant = 0;
  bool has_root_key = false;
  bool has_count = false;
  uint64_t count = 0;
  bool has_sum = false;
  int64_t sum = 0;
  std::vector<uint8_t> flags;
};

bool DecodeTreeElementState(const std::vector<uint8_t>& element_bytes,
                            DecodedTreeElementState* state,
                            std::string* error) {
  if (state == nullptr) {
    if (error) {
      *error = "decoded state output is null";
    }
    return false;
  }
  size_t cursor = 0;
  if (!DecodeVarint(element_bytes, &cursor, &state->variant, error)) {
    return false;
  }
  uint64_t root_key_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &root_key_option, error)) {
    return false;
  }
  if (root_key_option == 0) {
    state->has_root_key = false;
  } else if (root_key_option == 1) {
    state->has_root_key = true;
    uint64_t root_key_len = 0;
    if (!DecodeVarint(element_bytes, &cursor, &root_key_len, error)) {
      return false;
    }
    if (cursor + root_key_len > element_bytes.size()) {
      if (error) {
        *error = "vector length exceeds input size";
      }
      return false;
    }
    cursor += static_cast<size_t>(root_key_len);
  } else {
    if (error) {
      *error = "invalid option tag";
    }
    return false;
  }

  if (state->variant == 6 || state->variant == 7 || state->variant == 8 ||
      state->variant == 10) {
    state->has_count = true;
    if (!DecodeVarint(element_bytes, &cursor, &state->count, error)) {
      return false;
    }
  }
  if (state->variant == 4 || state->variant == 7 || state->variant == 10) {
    state->has_sum = true;
    uint64_t raw_sum = 0;
    if (!DecodeVarint(element_bytes, &cursor, &raw_sum, error)) {
      return false;
    }
    state->sum = ZigZagDecodeI64(raw_sum);
  } else if (state->variant == 5) {
    state->has_sum = true;
    unsigned __int128 raw_sum = 0;
    if (!DecodeVarintU128(element_bytes, &cursor, &raw_sum, error)) {
      return false;
    }
    state->sum = static_cast<int64_t>((raw_sum >> 1) ^ (~(raw_sum & 1) + 1));
  }

  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, &cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 0) {
    state->flags.clear();
  } else if (flags_option == 1) {
    uint64_t flags_len = 0;
    if (!DecodeVarint(element_bytes, &cursor, &flags_len, error)) {
      return false;
    }
    if (cursor + flags_len > element_bytes.size()) {
      if (error) {
        *error = "vector length exceeds input size";
      }
      return false;
    }
    state->flags.assign(element_bytes.begin() + static_cast<long>(cursor),
                        element_bytes.begin() + static_cast<long>(cursor + flags_len));
    cursor += static_cast<size_t>(flags_len);
  } else {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::string error;
  std::vector<uint8_t> item_element;
  if (!grovedb::EncodeItemToElementBytes({'v'}, &item_element, &error)) {
    Fail("Encode item failed: " + error);
  }
  std::vector<uint8_t> tree_element;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error)) {
    Fail("Encode tree failed: " + error);
  }

  {
    grovedb::GroveDb unopened;
    std::vector<grovedb::GroveDb::BatchOp> empty_batch;
    bool deleted = false;
    uint16_t deleted_count = 0;
    bool found = false;
    std::vector<uint8_t> got;

    if (unopened.Insert({}, {'k'}, item_element, &error)) {
      Fail("Insert should fail on unopened DB");
    }
    ExpectErrorContains("Insert unopened", error, "not opened");
    bool inserted = false;
    if (unopened.InsertIfNotExists({}, {'k'}, item_element, &inserted, &error)) {
      Fail("InsertIfNotExists should fail on unopened DB");
    }
    ExpectErrorContains("InsertIfNotExists unopened", error, "not opened");
    bool changed = false;
    bool had_previous = false;
    std::vector<uint8_t> previous;
    if (unopened.InsertIfChangedValue(
            {}, {'k'}, item_element, &changed, &previous, &had_previous, &error)) {
      Fail("InsertIfChangedValue should fail on unopened DB");
    }
    ExpectErrorContains("InsertIfChangedValue unopened", error, "not opened");
    bool had_existing = false;
    if (unopened.InsertIfNotExistsReturnExisting(
            {}, {'k'}, item_element, &previous, &had_existing, &error)) {
      Fail("InsertIfNotExistsReturnExisting should fail on unopened DB");
    }
    ExpectErrorContains("InsertIfNotExistsReturnExisting unopened", error, "not opened");
    if (unopened.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem should fail on unopened DB");
    }
    ExpectErrorContains("InsertItem unopened", error, "not opened");
    if (unopened.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree should fail on unopened DB");
    }
    ExpectErrorContains("InsertEmptyTree unopened", error, "not opened");
    if (unopened.InsertBigSumTree({}, {'b'}, &error)) {
      Fail("InsertBigSumTree should fail on unopened DB");
    }
    ExpectErrorContains("InsertBigSumTree unopened", error, "not opened");
    if (unopened.InsertCountTree({}, {'c'}, &error)) {
      Fail("InsertCountTree should fail on unopened DB");
    }
    ExpectErrorContains("InsertCountTree unopened", error, "not opened");
    if (unopened.InsertProvableCountTree({}, {'p'}, &error)) {
      Fail("InsertProvableCountTree should fail on unopened DB");
    }
    ExpectErrorContains("InsertProvableCountTree unopened", error, "not opened");
    if (unopened.InsertSumItem({}, {'s'}, 42, &error)) {
      Fail("InsertSumItem should fail on unopened DB");
    }
    ExpectErrorContains("InsertSumItem unopened", error, "not opened");
    if (unopened.InsertItemWithSum({}, {'s'}, {'v'}, 42, &error)) {
      Fail("InsertItemWithSum should fail on unopened DB");
    }
    ExpectErrorContains("InsertItemWithSum unopened", error, "not opened");

    if (unopened.Delete({}, {'k'}, &deleted, &error)) {
      Fail("Delete should fail on unopened DB");
    }
    ExpectErrorContains("Delete unopened", error, "not opened");
    if (unopened.DeleteIfEmptyTree({}, {'k'}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree should fail on unopened DB");
    }
    ExpectErrorContains("DeleteIfEmptyTree unopened", error, "not opened");
    if (unopened.ClearSubtree({}, &error)) {
      Fail("ClearSubtree should fail on unopened DB");
    }
    ExpectErrorContains("ClearSubtree unopened", error, "not opened");
    if (unopened.DeleteUpTreeWhileEmpty({{'p'}}, {'k'}, &deleted_count, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail on unopened DB");
    }
    ExpectErrorContains("DeleteUpTreeWhileEmpty unopened", error, "not opened");

    if (unopened.Get({}, {'k'}, &got, &found, &error)) {
      Fail("Get should fail on unopened DB");
    }
    ExpectErrorContains("Get unopened", error, "not opened");
    if (unopened.GetRaw({}, {'k'}, &got, &found, &error)) {
      Fail("GetRaw should fail on unopened DB");
    }
    ExpectErrorContains("GetRaw unopened", error, "not opened");
    if (unopened.FollowReference({}, {'k'}, &got, &found, &error)) {
      Fail("FollowReference should fail on unopened DB");
    }
    ExpectErrorContains("FollowReference unopened", error, "not opened");
    if (unopened.Has({}, {'k'}, &found, &error)) {
      Fail("Has should fail on unopened DB");
    }
    ExpectErrorContains("Has unopened", error, "not opened");
    if (unopened.HasRaw({}, {'k'}, &found, &error)) {
      Fail("HasRaw should fail on unopened DB");
    }
    ExpectErrorContains("HasRaw unopened", error, "not opened");
    if (unopened.HasCachingOptional({}, {'k'}, &found, false, &error)) {
      Fail("HasCachingOptional should fail on unopened DB");
    }
    ExpectErrorContains("HasCachingOptional unopened", error, "not opened");
    if (unopened.IsEmptyTree({}, &found, &error)) {
      Fail("IsEmptyTree should fail on unopened DB");
    }
    ExpectErrorContains("IsEmptyTree unopened", error, "not opened");
    if (unopened.CheckSubtreeExistsInvalidPath({}, &error)) {
      Fail("CheckSubtreeExistsInvalidPath should fail on unopened DB");
    }
    ExpectErrorContains("CheckSubtreeExistsInvalidPath unopened", error, "not opened");
    std::vector<std::vector<std::vector<uint8_t>>> subtrees;
    if (unopened.FindSubtrees({}, &subtrees, &error)) {
      Fail("FindSubtrees should fail on unopened DB");
    }
    ExpectErrorContains("FindSubtrees unopened", error, "not opened");
    std::vector<uint64_t> filter = {2, 4};
    if (unopened.FindSubtreesByKinds({}, &subtrees, &filter, &error)) {
      Fail("FindSubtreesByKinds should fail on unopened DB");
    }
    ExpectErrorContains("FindSubtreesByKinds unopened", error, "not opened");
    std::vector<uint8_t> root_key;
    std::vector<uint8_t> element_bytes;
    if (unopened.GetSubtreeRoot({}, &root_key, &element_bytes, &error)) {
      Fail("GetSubtreeRoot should fail on unopened DB");
    }
    ExpectErrorContains("GetSubtreeRoot unopened", error, "not opened");

    if (unopened.ApplyBatch(empty_batch, &error)) {
      Fail("ApplyBatch should fail on unopened DB");
    }
    ExpectErrorContains("ApplyBatch unopened", error, "not opened");

    if (unopened.ValidateBatch(empty_batch, &error)) {
      Fail("ValidateBatch should fail on unopened DB");
    }
    ExpectErrorContains("ValidateBatch unopened", error, "not opened");
    grovedb::OperationCost unopened_estimated_batch_cost;
    if (unopened.EstimatedCaseOperationsForBatch(
            empty_batch, &unopened_estimated_batch_cost, &error)) {
      Fail("EstimatedCaseOperationsForBatch should fail on unopened DB");
    }
    ExpectErrorContains("EstimatedCaseOperationsForBatch unopened", error, "not opened");

    if (unopened.Wipe(&error)) {
      Fail("Wipe should fail on unopened DB");
    }
    ExpectErrorContains("Wipe unopened", error, "not opened");
    if (unopened.Flush(&error)) {
      Fail("Flush should fail on unopened DB");
    }
    ExpectErrorContains("Flush unopened", error, "not opened");
    if (unopened.StartVisualizer("127.0.0.1:0", &error)) {
      Fail("StartVisualizer should fail in C++ rewrite scope");
    }
    ExpectErrorContains("StartVisualizer unopened unsupported", error, "out of scope");
    ExpectErrorContains("StartVisualizer unopened expiry", error, "2026-06-30");
    ExpectErrorContains("StartVisualizer unopened gap id", error, "visualizer-hook-parity-decision");
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    std::vector<uint8_t> proof_bytes;
    grovedb::GroveVersion unsupported{3, 9, 9};
    if (unopened.ProveQuery(single_key_query, &proof_bytes, &error)) {
      Fail("ProveQuery should fail on unopened DB");
    }
    ExpectErrorContains("ProveQuery unopened", error, "not opened");
    if (unopened.ProveQueryForVersion(single_key_query, unsupported, &proof_bytes, &error)) {
      Fail("ProveQueryForVersion should reject unsupported version on unopened DB");
    }
    ExpectErrorContains("ProveQueryForVersion unopened unsupported", error, "unsupported grove version");
    bool is_empty = false;
    if (unopened.IsEmptyTreeForVersion({}, unsupported, &is_empty, &error)) {
      Fail("IsEmptyTreeForVersion should reject unsupported version on unopened DB");
    }
    ExpectErrorContains("IsEmptyTreeForVersion unopened unsupported", error, "unsupported grove version");
    grovedb::GroveDb::Transaction unopened_tx;
    if (unopened.IsEmptyTreeForVersion({}, unsupported, &is_empty, &unopened_tx, &error)) {
      Fail("IsEmptyTreeForVersion(tx) should reject unsupported version on unopened DB");
    }
    ExpectErrorContains("IsEmptyTreeForVersion(tx) unopened unsupported", error, "unsupported grove version");
    std::vector<grovedb::VerificationIssue> issues;
    if (unopened.VerifyGroveDbForVersion(unsupported, true, true, &issues, &error)) {
      Fail("VerifyGroveDbForVersion should reject unsupported version on unopened DB");
    }
    ExpectErrorContains("VerifyGroveDbForVersion unopened unsupported", error, "unsupported grove version");

    {
      auto cp_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      if (unopened.CreateCheckpoint(MakeTempDir("facade_contract_" + std::to_string(cp_now)), &error)) {
        Fail("CreateCheckpoint should fail on unopened DB");
      }
      ExpectErrorContains("CreateCheckpoint unopened", error, "not opened");
    }

    {
      auto oc_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      if (unopened.OpenCheckpoint(MakeTempDir("facade_contract_" + std::to_string(oc_now)), &error)) {
        Fail("OpenCheckpoint should fail for non-checkpoint directory");
      }
      ExpectErrorContains("OpenCheckpoint non-checkpoint", error, "not a valid checkpoint");
    }

    if (grovedb::GroveDb::DeleteCheckpoint("/", &error)) {
      Fail("DeleteCheckpoint should fail for unsafe short path");
    }
    ExpectErrorContains("DeleteCheckpoint short path", error, "safety check");
  }

  grovedb::GroveDb db;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string dir = MakeTempDir("facade_contract_" + std::to_string(now));
  if (!db.Open(dir, &error)) {
    Fail("Open failed: " + error);
  }
  if (!db.Flush(&error)) {
    Fail("Flush should succeed on opened DB: " + error);
  }
  if (db.StartVisualizer("127.0.0.1:0", &error)) {
    Fail("StartVisualizer should fail in C++ rewrite scope");
  }
  ExpectErrorContains("StartVisualizer opened unsupported", error, "out of scope");
  ExpectErrorContains("StartVisualizer opened expiry", error, "2026-06-30");
  ExpectErrorContains("StartVisualizer opened gap id", error, "visualizer-hook-parity-decision");

  {
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    if (db.ProveQuery(single_key_query, nullptr, &error)) {
      Fail("ProveQuery should fail for null proof output");
    }
    ExpectErrorContains("ProveQuery null output", error, "output is null");

    std::vector<uint8_t> proof_bytes;
    grovedb::GroveVersion unsupported{3, 9, 9};
    if (db.ProveQueryForVersion(single_key_query, unsupported, &proof_bytes, &error)) {
      Fail("ProveQueryForVersion should fail for unsupported version");
    }
    ExpectErrorContains("ProveQueryForVersion unsupported version", error, "unsupported grove version");

    std::vector<uint8_t> empty_tree;
    if (!grovedb::EncodeTreeToElementBytes(&empty_tree, &error)) {
      Fail("EncodeTreeToElementBytes failed: " + error);
    }
    std::vector<uint8_t> item_element;
    if (!grovedb::EncodeItemToElementBytes({'v'}, &item_element, &error)) {
      Fail("EncodeItemToElementBytes failed: " + error);
    }
    if (!db.Insert({}, {'s', 'u', 'b'}, empty_tree, &error)) {
      Fail("Insert subtree failed: " + error);
    }
    if (!db.Insert({{'s', 'u', 'b'}}, {'k'}, item_element, &error)) {
      Fail("Insert subtree key failed: " + error);
    }
    grovedb::PathQuery non_root_query = grovedb::PathQuery::NewSingleKey({{'s', 'u', 'b'}}, {'k'});
    if (!db.ProveQuery(non_root_query, &proof_bytes, &error)) {
      Fail("ProveQuery should support non-root path query: " + error);
    }
    if (proof_bytes.empty()) {
      Fail("ProveQuery non-root query should return proof bytes");
    }
  }

  {
    std::vector<grovedb::GroveDb::BatchOp> empty_batch;
    if (db.EstimatedCaseOperationsForBatch(empty_batch, nullptr, &error)) {
      Fail("EstimatedCaseOperationsForBatch should fail for null cost output");
    }
    ExpectErrorContains("EstimatedCaseOperationsForBatch null cost", error, "cost output is null");

    if (db.Insert({}, {}, item_element, &error)) {
      Fail("Insert should fail for empty key");
    }
    ExpectErrorContains("Insert empty key", error, "key is empty");
    bool inserted = false;
    if (db.InsertIfNotExists({}, {'k'}, item_element, nullptr, &error)) {
      Fail("InsertIfNotExists should fail for null inserted output");
    }
    ExpectErrorContains("InsertIfNotExists null inserted", error, "output is null");
    if (db.InsertIfNotExists({}, {}, item_element, &inserted, &error)) {
      Fail("InsertIfNotExists should fail for empty key");
    }
    ExpectErrorContains("InsertIfNotExists empty key", error, "key is empty");
    bool changed = false;
    bool had_previous = false;
    std::vector<uint8_t> previous;
    if (db.InsertIfChangedValue({}, {'k'}, item_element, nullptr, &previous, &had_previous, &error)) {
      Fail("InsertIfChangedValue should fail for null inserted output");
    }
    ExpectErrorContains("InsertIfChangedValue null inserted", error, "output is null");
    if (db.InsertIfChangedValue({}, {'k'}, item_element, &changed, &previous, nullptr, &error)) {
      Fail("InsertIfChangedValue should fail for null previous-found output");
    }
    ExpectErrorContains("InsertIfChangedValue null previous-found", error, "output is null");
    if (db.InsertIfChangedValue({}, {}, item_element, &changed, &previous, &had_previous, &error)) {
      Fail("InsertIfChangedValue should fail for empty key");
    }
    ExpectErrorContains("InsertIfChangedValue empty key", error, "key is empty");
    bool had_existing = false;
    if (db.InsertIfNotExistsReturnExisting({}, {'k'}, item_element, &previous, nullptr, &error)) {
      Fail("InsertIfNotExistsReturnExisting should fail for null had-existing output");
    }
    ExpectErrorContains("InsertIfNotExistsReturnExisting null had-existing", error, "output is null");
    if (db.InsertIfNotExistsReturnExisting({}, {}, item_element, &previous, &had_existing, &error)) {
      Fail("InsertIfNotExistsReturnExisting should fail for empty key");
    }
    ExpectErrorContains("InsertIfNotExistsReturnExisting empty key", error, "key is empty");
    if (db.InsertItem({}, {}, {'v'}, &error)) {
      Fail("InsertItem should fail for empty key");
    }
    ExpectErrorContains("InsertItem empty key", error, "key is empty");
    if (db.InsertEmptyTree({}, {}, &error)) {
      Fail("InsertEmptyTree should fail for empty key");
    }
    ExpectErrorContains("InsertEmptyTree empty key", error, "key is empty");
    if (db.InsertBigSumTree({}, {}, &error)) {
      Fail("InsertBigSumTree should fail for empty key");
    }
    ExpectErrorContains("InsertBigSumTree empty key", error, "key is empty");
    if (db.InsertCountTree({}, {}, &error)) {
      Fail("InsertCountTree should fail for empty key");
    }
    ExpectErrorContains("InsertCountTree empty key", error, "key is empty");
    if (db.InsertProvableCountTree({}, {}, &error)) {
      Fail("InsertProvableCountTree should fail for empty key");
    }
    ExpectErrorContains("InsertProvableCountTree empty key", error, "key is empty");
    if (db.InsertSumItem({}, {}, 42, &error)) {
      Fail("InsertSumItem should fail for empty key");
    }
    ExpectErrorContains("InsertSumItem empty key", error, "key is empty");
    if (db.InsertItemWithSum({}, {}, {'v'}, 42, &error)) {
      Fail("InsertItemWithSum should fail for empty key");
    }
    ExpectErrorContains("InsertItemWithSum empty key", error, "key is empty");

    if (db.Insert({}, {'k'}, {}, &error)) {
      Fail("Insert should fail for empty element");
    }
    ExpectErrorContains("Insert empty element", error, "element bytes are empty");

    bool deleted = false;
    if (db.Delete({}, {}, &deleted, &error)) {
      Fail("Delete should fail for empty key");
    }
    ExpectErrorContains("Delete empty key", error, "key is empty");
    if (db.DeleteIfEmptyTree({}, {'k'}, nullptr, &error)) {
      Fail("DeleteIfEmptyTree should fail for null deleted output");
    }
    ExpectErrorContains("DeleteIfEmptyTree null output", error, "output is null");
    if (db.DeleteUpTreeWhileEmpty({{'p'}}, {'k'}, nullptr, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for null output");
    }
    ExpectErrorContains("DeleteUpTreeWhileEmpty null output", error, "output is null");
    if (db.DeleteIfEmptyTree({}, {}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree should fail for empty key");
    }
    ExpectErrorContains("DeleteIfEmptyTree empty key", error, "key is empty");
    uint16_t deleted_count = 0;
    if (db.DeleteUpTreeWhileEmpty({}, {'k'}, &deleted_count, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for empty path");
    }
    ExpectErrorContains("DeleteUpTreeWhileEmpty empty path", error, "root tree leaves currently cannot be deleted");
    if (db.DeleteUpTreeWhileEmpty({{'p'}}, {}, &deleted_count, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for empty key");
    }
    ExpectErrorContains("DeleteUpTreeWhileEmpty empty key", error, "key is empty");

    bool found = false;
    std::vector<uint8_t> got;
    if (db.Get({}, {}, &got, &found, &error)) {
      Fail("Get should fail for empty key");
    }
    ExpectErrorContains("Get empty key", error, "key is empty");
    if (db.GetRaw({}, {}, &got, &found, &error)) {
      Fail("GetRaw should fail for empty key");
    }
    ExpectErrorContains("GetRaw empty key", error, "key is empty");
    if (db.GetRaw({}, {'k'}, nullptr, &found, &error)) {
      Fail("GetRaw should fail for null element output");
    }
    ExpectErrorContains("GetRaw null element output", error, "output is null");
    if (db.GetRaw({}, {'k'}, &got, nullptr, &error)) {
      Fail("GetRaw should fail for null found output");
    }
    ExpectErrorContains("GetRaw null found output", error, "output is null");
    if (db.GetRawOptional({}, {'k'}, &got, nullptr, &error)) {
      Fail("GetRawOptional should fail for null found output");
    }
    ExpectErrorContains("GetRawOptional null found output", error, "output is null");
    if (db.GetRawOptional({}, {}, &got, &found, &error)) {
      Fail("GetRawOptional should fail for empty key");
    }
    ExpectErrorContains("GetRawOptional empty key", error, "key is empty");
    if (db.FollowReference({}, {}, &got, &found, &error)) {
      Fail("FollowReference should fail for empty key");
    }
    ExpectErrorContains("FollowReference empty key", error, "key is empty");
    if (db.FollowReference({}, {'k'}, nullptr, &found, &error)) {
      Fail("FollowReference should fail for null element output");
    }
    ExpectErrorContains("FollowReference null element output", error, "output is null");
    if (db.FollowReference({}, {'k'}, &got, nullptr, &error)) {
      Fail("FollowReference should fail for null found output");
    }
    ExpectErrorContains("FollowReference null found output", error, "output is null");
    if (db.Has({}, {}, &found, &error)) {
      Fail("Has should fail for empty key");
    }
    ExpectErrorContains("Has empty key", error, "key is empty");
    if (db.Has({}, {'k'}, nullptr, &error)) {
      Fail("Has should fail for null output");
    }
    ExpectErrorContains("Has null output", error, "output is null");
    if (db.HasRaw({}, {}, &found, &error)) {
      Fail("HasRaw should fail for empty key");
    }
    ExpectErrorContains("HasRaw empty key", error, "key is empty");
    if (db.HasRaw({}, {'k'}, nullptr, &error)) {
      Fail("HasRaw should fail for null output");
    }
    ExpectErrorContains("HasRaw null output", error, "output is null");
    if (db.HasCachingOptional({}, {}, &found, false, &error)) {
      Fail("HasCachingOptional should fail for empty key");
    }
    ExpectErrorContains("HasCachingOptional empty key", error, "key is empty");
    if (db.HasCachingOptional({}, {'k'}, nullptr, false, &error)) {
      Fail("HasCachingOptional should fail for null output");
    }
    ExpectErrorContains("HasCachingOptional null output", error, "output is null");
    if (db.IsEmptyTree({}, nullptr, &error)) {
      Fail("IsEmptyTree should fail for null output");
    }
    ExpectErrorContains("IsEmptyTree null output", error, "output is null");
    if (db.FindSubtrees({}, nullptr, &error)) {
      Fail("FindSubtrees should fail for null output");
    }
    ExpectErrorContains("FindSubtrees null output", error, "output is null");
    std::vector<uint64_t> filter = {2};
    if (db.FindSubtreesByKinds({}, nullptr, &filter, &error)) {
      Fail("FindSubtreesByKinds should fail for null output");
    }
    ExpectErrorContains("FindSubtreesByKinds null output", error, "output is null");
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    if (db.QueryRaw(single_key_query, nullptr, &error)) {
      Fail("QueryRaw should fail for null output");
    }
    ExpectErrorContains("QueryRaw null output", error, "output is null");
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> raw_keys_out;
    if (db.QueryRawKeysOptional(single_key_query, nullptr, &error)) {
      Fail("QueryRawKeysOptional should fail for null output");
    }
    ExpectErrorContains("QueryRawKeysOptional null output", error, "output is null");
    if (db.QueryKeysOptional(single_key_query, nullptr, &error)) {
      Fail("QueryKeysOptional should fail for null output");
    }
    ExpectErrorContains("QueryKeysOptional null output", error, "output is null");
    std::vector<std::vector<uint8_t>> query_item_values_out;
    if (db.QueryItemValue(single_key_query, nullptr, &error)) {
      Fail("QueryItemValue should fail for null output");
    }
    ExpectErrorContains("QueryItemValue null output", error, "output is null");

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> query_out;
    grovedb::PathQuery empty_items_query;
    if (db.QueryRaw(empty_items_query, &query_out, &error)) {
      Fail("QueryRaw should fail for empty query items");
    }
    ExpectErrorContains("QueryRaw empty query items", error, "query items are empty");

    grovedb::PathQuery unsupported_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 1, std::nullopt));
    unsupported_query.query.query.default_subquery_branch.subquery_path =
        std::vector<std::vector<uint8_t>>{{'x'}};
    if (db.QueryRaw(unsupported_query, &query_out, &error)) {
      Fail("QueryRaw should reject unsupported subquery shape");
    }
    ExpectErrorContains("QueryRaw unsupported shape", error, "unsupported query shape");
    if (db.QueryItemValue(unsupported_query, &query_item_values_out, &error)) {
      Fail("QueryItemValue should reject unsupported subquery shape");
    }
    ExpectErrorContains("QueryItemValue unsupported shape", error, "unsupported query shape");
    if (db.QueryRawKeysOptional(unsupported_query, &raw_keys_out, &error)) {
      Fail("QueryRawKeysOptional should reject unsupported subquery shape");
    }
    ExpectErrorContains(
        "QueryRawKeysOptional unsupported shape", error, "unsupported query shape");
    if (db.QueryKeysOptional(unsupported_query, &raw_keys_out, &error)) {
      Fail("QueryKeysOptional should reject unsupported subquery shape");
    }
    ExpectErrorContains("QueryKeysOptional unsupported shape", error, "unsupported query shape");
    if (db.QueryRawKeysOptional(single_key_query, &raw_keys_out, &error)) {
      Fail("QueryRawKeysOptional should fail when limit is not set");
    }
    ExpectErrorContains("QueryRawKeysOptional missing limit",
                        error,
                        "limits must be set in query_raw_keys_optional");
    if (db.QueryKeysOptional(single_key_query, &raw_keys_out, &error)) {
      Fail("QueryKeysOptional should fail when limit is not set");
    }
    ExpectErrorContains("QueryKeysOptional missing limit",
                        error,
                        "limits must be set in query_keys_optional");
    grovedb::PathQuery with_offset = grovedb::PathQuery::NewSingleKey({}, {'k'});
    with_offset.query.limit = 1;
    with_offset.query.offset = 1;
    if (db.QueryRawKeysOptional(with_offset, &raw_keys_out, &error)) {
      Fail("QueryRawKeysOptional should reject offset");
    }
    ExpectErrorContains("QueryRawKeysOptional offset",
                        error,
                        "offsets are not supported in query_raw_keys_optional");
    if (db.QueryKeysOptional(with_offset, &raw_keys_out, &error)) {
      Fail("QueryKeysOptional should reject offset");
    }
    ExpectErrorContains("QueryKeysOptional offset",
                        error,
                        "offsets are not supported in query_raw_keys_optional");
    grovedb::PathQuery range_query =
        grovedb::PathQuery::NewSingleQueryItem({}, grovedb::QueryItem::Range({'a'}, {'z'}));
    range_query.query.limit = 1;
    if (db.QueryRawKeysOptional(range_query, &raw_keys_out, &error)) {
      Fail("QueryRawKeysOptional should reject non-key query item");
    }
    ExpectErrorContains("QueryRawKeysOptional non-key item", error, "unsupported query item");
    if (db.QueryKeysOptional(range_query, &raw_keys_out, &error)) {
      Fail("QueryKeysOptional should reject non-key query item");
    }
    ExpectErrorContains("QueryKeysOptional non-key item", error, "unsupported query item");
    if (db.QueryItemValue(empty_items_query, &query_item_values_out, &error)) {
      Fail("QueryItemValue should fail for empty query items");
    }
    ExpectErrorContains("QueryItemValue empty query items", error, "query items are empty");
  }

  // HasRaw contract tests - positive path
  {
    bool found = false;
    // Insert an item to test HasRaw
    if (!db.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for HasRaw test failed: " + error);
    }
    // HasRaw should find existing key
    if (!db.HasRaw({}, {'k'}, &found, &error)) {
      Fail("HasRaw positive path failed: " + error);
    }
    if (!found) {
      Fail("HasRaw should report found=true for existing key");
    }
    // HasRaw should not find missing key
    if (!db.HasRaw({}, {'m'}, &found, &error)) {
      Fail("HasRaw missing key failed: " + error);
    }
    if (found) {
      Fail("HasRaw should report found=false for missing key");
    }
  }

  // HasCachingOptional contract tests - positive path
  {
    bool found = false;
    // Insert an item to test HasCachingOptional
    if (!db.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for HasCachingOptional test failed: " + error);
    }
    // HasCachingOptional with allow_cache=true should find existing key
    if (!db.HasCachingOptional({}, {'k'}, &found, true, &error)) {
      Fail("HasCachingOptional (allow_cache=true) positive path failed: " + error);
    }
    if (!found) {
      Fail("HasCachingOptional (allow_cache=true) should report found=true for existing key");
    }
    // HasCachingOptional with allow_cache=false should find existing key
    if (!db.HasCachingOptional({}, {'k'}, &found, false, &error)) {
      Fail("HasCachingOptional (allow_cache=false) positive path failed: " + error);
    }
    if (!found) {
      Fail("HasCachingOptional (allow_cache=false) should report found=true for existing key");
    }
    // HasCachingOptional should not find missing key
    if (!db.HasCachingOptional({}, {'m'}, &found, true, &error)) {
      Fail("HasCachingOptional missing key failed: " + error);
    }
    if (found) {
      Fail("HasCachingOptional should report found=false for missing key");
    }
  }

  // QuerySums contract tests
  {
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> query_out;
    std::vector<int64_t> sums_out;
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    
    // QuerySums requires limit to be set
    if (db.QuerySums(single_key_query, &sums_out, &error)) {
      Fail("QuerySums should fail when limit is not set");
    }
    ExpectErrorContains("QuerySums missing limit",
                        error,
                        "limits must be set in query_sums");
    
    // QuerySums rejects offset (not supported)
    grovedb::PathQuery with_offset = grovedb::PathQuery::NewSingleKey({}, {'k'});
    with_offset.query.limit = 1;
    with_offset.query.offset = 1;
    if (db.QuerySums(with_offset, &sums_out, &error)) {
      Fail("QuerySums should reject offset");
    }
    ExpectErrorContains("QuerySums offset",
                        error,
                        "offsets are not supported in query_sums");
    
    // QuerySums rejects unsupported subquery shapes
    grovedb::PathQuery unsupported_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 1, std::nullopt));
    unsupported_query.query.query.default_subquery_branch.subquery_path =
        std::vector<std::vector<uint8_t>>{{'x'}};
    if (db.QuerySums(unsupported_query, &sums_out, &error)) {
      Fail("QuerySums should reject unsupported subquery shape");
    }
    ExpectErrorContains("QuerySums unsupported shape", error, "unsupported query shape for query_sums");
    
    // QuerySums with empty query items
    grovedb::PathQuery empty_query;
    empty_query.path = {};
    empty_query.query.limit = 1;
    empty_query.query.offset = std::nullopt;
    empty_query.query.query.left_to_right = true;
    if (db.QuerySums(empty_query, &sums_out, &error)) {
      Fail("QuerySums should reject empty query items");
    }
    ExpectErrorContains("QuerySums empty items", error, "query items are empty");
  }

  // QueryItemValueOrSum contract tests
  {
    std::vector<grovedb::GroveDb::QueryItemOrSumValue> out_values;
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});

    // QueryItemValueOrSum requires limit to be set
    if (db.QueryItemValueOrSum(single_key_query, &out_values, &error)) {
      Fail("QueryItemValueOrSum should fail when limit is not set");
    }
    ExpectErrorContains("QueryItemValueOrSum missing limit",
                        error,
                        "limits must be set");

    // QueryItemValueOrSum rejects offset (not supported)
    grovedb::PathQuery with_offset = grovedb::PathQuery::NewSingleKey({}, {'k'});
    with_offset.query.limit = 1;
    with_offset.query.offset = 1;
    if (db.QueryItemValueOrSum(with_offset, &out_values, &error)) {
      Fail("QueryItemValueOrSum should reject offset");
    }
    ExpectErrorContains("QueryItemValueOrSum offset",
                        error,
                        "offsets are not supported");

    // QueryItemValueOrSum rejects unsupported subquery shapes
    grovedb::PathQuery unsupported_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 1, std::nullopt));
    unsupported_query.query.query.default_subquery_branch.subquery_path =
        std::vector<std::vector<uint8_t>>{{'x'}};
    if (db.QueryItemValueOrSum(unsupported_query, &out_values, &error)) {
      Fail("QueryItemValueOrSum should reject unsupported subquery shape");
    }
    ExpectErrorContains("QueryItemValueOrSum unsupported shape", error, "unsupported query shape");

    // QueryItemValueOrSum with empty query items
    grovedb::PathQuery empty_query;
    empty_query.path = {};
    empty_query.query.limit = 1;
    empty_query.query.offset = std::nullopt;
    empty_query.query.query.left_to_right = true;
    if (db.QueryItemValueOrSum(empty_query, &out_values, &error)) {
      Fail("QueryItemValueOrSum should reject empty query items");
    }
    ExpectErrorContains("QueryItemValueOrSum empty items", error, "query items are empty");

    // QueryItemValueOrSum null output
    if (db.QueryItemValueOrSum(single_key_query, nullptr, &error)) {
      Fail("QueryItemValueOrSum should fail for null output");
    }
    ExpectErrorContains("QueryItemValueOrSum null output", error, "output is null");
  }

  // QuerySums transaction lifecycle tests
  {
    grovedb::GroveDb::Transaction tx;
    std::vector<int64_t> sums_out;
    grovedb::PathQuery single_key_query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    single_key_query.query.limit = 1;
    
    if (db.QuerySums(single_key_query, &sums_out, &tx, &error)) {
      Fail("QuerySums should fail for inactive tx");
    }
    ExpectErrorContains("QuerySums inactive tx", error, "not active");
  }

  {
    grovedb::GroveDb::Transaction tx;
    bool found = false;
    bool is_empty = false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    grovedb::GroveVersion unsupported{3, 9, 9};
    if (db.Insert({}, {'k'}, item_element, &tx, &error)) {
      Fail("Insert should fail for inactive tx");
    }
    ExpectErrorContains("Insert inactive tx", error, "not active");

    if (db.CommitTransaction(&tx, &error)) {
      Fail("Commit should fail for inactive tx");
    }
    ExpectErrorContains("Commit inactive tx", error, "not active");

    if (db.QueryRange({}, {}, {}, true, true, &out, &tx, &error)) {
      Fail("QueryRange should fail for inactive tx");
    }
    ExpectErrorContains("QueryRange inactive tx", error, "not active");
    grovedb::PathQuery inactive_query_raw = grovedb::PathQuery::NewSingleKey({}, {'k'});
    if (db.QueryRaw(inactive_query_raw, &out, &tx, &error)) {
      Fail("QueryRaw should fail for inactive tx");
    }
    ExpectErrorContains("QueryRaw inactive tx", error, "not active");
    std::vector<std::vector<uint8_t>> inactive_item_values_out;
    if (db.QueryItemValue(inactive_query_raw, &inactive_item_values_out, &tx, &error)) {
      Fail("QueryItemValue should fail for inactive tx");
    }
    ExpectErrorContains("QueryItemValue inactive tx", error, "not active");
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> inactive_query_raw_keys_out;
    grovedb::PathQuery inactive_query_raw_keys = grovedb::PathQuery::NewSingleKey({}, {'k'});
    inactive_query_raw_keys.query.limit = 1;
    if (db.QueryRawKeysOptional(inactive_query_raw_keys,
                                &inactive_query_raw_keys_out,
                                &tx,
                                &error)) {
      Fail("QueryRawKeysOptional should fail for inactive tx");
    }
    ExpectErrorContains("QueryRawKeysOptional inactive tx", error, "not active");
    if (db.QueryKeysOptional(inactive_query_raw_keys,
                             &inactive_query_raw_keys_out,
                             &tx,
                             &error)) {
      Fail("QueryKeysOptional should fail for inactive tx");
    }
    ExpectErrorContains("QueryKeysOptional inactive tx", error, "not active");
    if (db.IsEmptyTree({}, &found, &tx, &error)) {
      Fail("IsEmptyTree should fail for inactive tx");
    }
    ExpectErrorContains("IsEmptyTree inactive tx", error, "not active");
    if (db.CheckSubtreeExistsInvalidPath({}, &tx, &error)) {
      Fail("CheckSubtreeExistsInvalidPath should fail for inactive tx");
    }
    ExpectErrorContains("CheckSubtreeExistsInvalidPath inactive tx", error, "not active");
    std::vector<std::vector<std::vector<uint8_t>>> subtrees;
    if (db.FindSubtrees({}, &subtrees, &tx, &error)) {
      Fail("FindSubtrees should fail for inactive tx");
    }
    ExpectErrorContains("FindSubtrees inactive tx", error, "not active");
    std::vector<uint8_t> inactive_root_key;
    std::vector<uint8_t> inactive_element_bytes;
    if (db.GetSubtreeRoot({}, &inactive_root_key, &inactive_element_bytes, &tx, &error)) {
      Fail("GetSubtreeRoot should fail for inactive tx");
    }
    ExpectErrorContains("GetSubtreeRoot inactive tx", error, "not active");
    if (db.DeleteIfEmptyTree({}, {'d', 't'}, &found, &tx, &error)) {
      Fail("DeleteIfEmptyTree should fail for inactive tx");
    }
    ExpectErrorContains("DeleteIfEmptyTree inactive tx", error, "not active");
    if (db.ClearSubtree({}, &tx, &error)) {
      Fail("ClearSubtree should fail for inactive tx");
    }
    ExpectErrorContains("ClearSubtree inactive tx", error, "not active");
    uint16_t deleted_count = 0;
    if (db.DeleteUpTreeWhileEmpty({{'t'}}, {'d'}, &deleted_count, &tx, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for inactive tx");
    }
    ExpectErrorContains("DeleteUpTreeWhileEmpty inactive tx", error, "not active");
    if (db.IsEmptyTreeForVersion({}, unsupported, &is_empty, &tx, &error)) {
      Fail("IsEmptyTreeForVersion(tx) should reject unsupported version before tx-state checks");
    }
    ExpectErrorContains("IsEmptyTreeForVersion(tx) inactive unsupported", error, "unsupported grove version");

    std::vector<grovedb::GroveDb::BatchOp> batch_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'k'}, item_element}};
    if (db.ApplyBatch(batch_ops, &tx, &error)) {
      Fail("ApplyBatch should fail for inactive tx");
    }
    ExpectErrorContains("ApplyBatch inactive tx", error, "not active");
  }

  {
    grovedb::GroveDb::Transaction tx_reuse;
    if (!db.StartTransaction(&tx_reuse, &error)) {
      Fail("StartTransaction(tx_reuse first) failed: " + error);
    }
    if (db.StartTransaction(&tx_reuse, &error)) {
      Fail("StartTransaction should fail for already-active output handle");
    }
    ExpectErrorContains("StartTransaction active output", error, "already active");
    if (!db.RollbackTransaction(&tx_reuse, &error)) {
      Fail("RollbackTransaction(tx_reuse first) failed: " + error);
    }
    if (!db.StartTransaction(&tx_reuse, &error)) {
      Fail("StartTransaction(tx_reuse second) failed: " + error);
    }
    if (!db.CommitTransaction(&tx_reuse, &error)) {
      Fail("CommitTransaction(tx_reuse second) failed: " + error);
    }
  }

  {
    grovedb::GroveDb::Transaction tx;
    bool found = false;
    bool is_empty = false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    grovedb::GroveVersion unsupported{3, 9, 9};
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(contract lifecycle) failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction(contract lifecycle) failed: " + error);
    }
    if (db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction should fail when called after commit");
    }
    ExpectErrorContains("Commit after commit", error, "not active");

    if (db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction should fail when called after commit");
    }
    ExpectErrorContains("Rollback after commit", error, "not active");

    std::vector<grovedb::GroveDb::BatchOp> batch_ops = {
        {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'q'}, item_element}};
    if (db.ApplyBatch(batch_ops, &tx, &error)) {
      Fail("ApplyBatch should fail for committed tx");
    }
    ExpectErrorContains("ApplyBatch committed tx", error, "not active");

    bool inserted = false;
    if (db.InsertIfNotExists({}, {'n', 'i', 't', 'x'}, item_element, &inserted, &tx, &error)) {
      Fail("InsertIfNotExists should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertIfNotExists committed tx", error, {"committed", "not active"});

    bool changed = false;
    bool had_previous = false;
    std::vector<uint8_t> previous;
    if (db.InsertIfChangedValue(
            {}, {'c', 'h', 't', 'x'}, item_element, &changed, &previous, &had_previous, &tx, &error)) {
      Fail("InsertIfChangedValue should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertIfChangedValue committed tx", error, {"committed", "not active"});

    bool had_existing = false;
    if (db.InsertIfNotExistsReturnExisting(
            {}, {'r', 'e', 't', 'x'}, item_element, &previous, &had_existing, &tx, &error)) {
      Fail("InsertIfNotExistsReturnExisting should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertIfNotExistsReturnExisting committed tx",
                           error,
                           {"committed", "not active"});

    if (db.InsertItem({}, {'i', 't', 'x'}, {'v'}, &tx, &error)) {
      Fail("InsertItem should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertItem committed tx", error, {"committed", "not active"});

    if (db.InsertEmptyTree({}, {'e', 't', 'x'}, &tx, &error)) {
      Fail("InsertEmptyTree should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertEmptyTree committed tx", error, {"committed", "not active"});
    if (db.InsertBigSumTree({}, {'b', 't', 'x'}, &tx, &error)) {
      Fail("InsertBigSumTree should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertBigSumTree committed tx", error, {"committed", "not active"});
    if (db.InsertCountTree({}, {'c', 't', 'x'}, &tx, &error)) {
      Fail("InsertCountTree should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertCountTree committed tx", error, {"committed", "not active"});
    if (db.InsertProvableCountTree({}, {'p', 't', 'x'}, &tx, &error)) {
      Fail("InsertProvableCountTree should fail for committed tx");
    }
    ExpectErrorContainsAny(
        "InsertProvableCountTree committed tx", error, {"committed", "not active"});
    if (db.InsertSumItem({}, {'s', 't', 'x'}, 42, &tx, &error)) {
      Fail("InsertSumItem should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertSumItem committed tx", error, {"committed", "not active"});
    if (db.InsertItemWithSum({}, {'i', 'w', 's'}, {'v'}, 42, &tx, &error)) {
      Fail("InsertItemWithSum should fail for committed tx");
    }
    ExpectErrorContainsAny("InsertItemWithSum committed tx", error, {"committed", "not active"});
    if (db.DeleteIfEmptyTree({}, {'d', 'e', 't', 'x'}, &found, &tx, &error)) {
      Fail("DeleteIfEmptyTree should fail for committed tx");
    }
    ExpectErrorContainsAny("DeleteIfEmptyTree committed tx", error, {"committed", "not active"});
    if (db.ClearSubtree({}, &tx, &error)) {
      Fail("ClearSubtree should fail for committed tx");
    }
    ExpectErrorContainsAny("ClearSubtree committed tx", error, {"committed", "not active"});
    uint16_t deleted_count = 0;
    if (db.DeleteUpTreeWhileEmpty({{'t'}}, {'d'}, &deleted_count, &tx, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for committed tx");
    }
    ExpectErrorContainsAny("DeleteUpTreeWhileEmpty committed tx", error, {"committed", "not active"});
    if (db.IsEmptyTree({}, &found, &tx, &error)) {
      Fail("IsEmptyTree should fail for committed tx");
    }
    ExpectErrorContainsAny("IsEmptyTree committed tx", error, {"committed", "not active"});
    if (db.CheckSubtreeExistsInvalidPath({}, &tx, &error)) {
      Fail("CheckSubtreeExistsInvalidPath should fail for committed tx");
    }
    ExpectErrorContainsAny(
        "CheckSubtreeExistsInvalidPath committed tx", error, {"committed", "not active"});
    std::vector<std::vector<std::vector<uint8_t>>> committed_subtrees;
    if (db.FindSubtrees({}, &committed_subtrees, &tx, &error)) {
      Fail("FindSubtrees should fail for committed tx");
    }
    ExpectErrorContainsAny("FindSubtrees committed tx", error, {"committed", "not active"});
    std::vector<uint8_t> committed_root_key;
    std::vector<uint8_t> committed_element_bytes;
    if (db.GetSubtreeRoot({}, &committed_root_key, &committed_element_bytes, &tx, &error)) {
      Fail("GetSubtreeRoot should fail for committed tx");
    }
    ExpectErrorContainsAny("GetSubtreeRoot committed tx", error, {"committed", "not active"});
    grovedb::PathQuery committed_query_raw = grovedb::PathQuery::NewSingleKey({}, {'k'});
    if (db.QueryRaw(committed_query_raw, &out, &tx, &error)) {
      Fail("QueryRaw should fail for committed tx");
    }
    ExpectErrorContainsAny("QueryRaw committed tx", error, {"committed", "not active"});
    std::vector<std::vector<uint8_t>> committed_item_values_out;
    if (db.QueryItemValue(committed_query_raw, &committed_item_values_out, &tx, &error)) {
      Fail("QueryItemValue should fail for committed tx");
    }
    ExpectErrorContainsAny(
        "QueryItemValue committed tx", error, {"committed", "not active"});
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> committed_query_raw_keys_out;
    grovedb::PathQuery committed_query_raw_keys = grovedb::PathQuery::NewSingleKey({}, {'k'});
    committed_query_raw_keys.query.limit = 1;
    if (db.QueryRawKeysOptional(committed_query_raw_keys,
                                &committed_query_raw_keys_out,
                                &tx,
                                &error)) {
      Fail("QueryRawKeysOptional should fail for committed tx");
    }
    ExpectErrorContainsAny(
        "QueryRawKeysOptional committed tx", error, {"committed", "not active"});
    if (db.QueryKeysOptional(committed_query_raw_keys,
                             &committed_query_raw_keys_out,
                             &tx,
                             &error)) {
      Fail("QueryKeysOptional should fail for committed tx");
    }
    ExpectErrorContainsAny(
        "QueryKeysOptional committed tx", error, {"committed", "not active"});
    if (db.IsEmptyTreeForVersion({}, unsupported, &is_empty, &tx, &error)) {
      Fail("IsEmptyTreeForVersion(tx) should reject unsupported version before committed-state checks");
    }
    ExpectErrorContains("IsEmptyTreeForVersion(tx) committed unsupported", error, "unsupported grove version");
  }

  {
    grovedb::GroveDb::Transaction tx_poisoned;
    if (!db.StartTransaction(&tx_poisoned, &error)) {
      Fail("StartTransaction(tx_poisoned) failed: " + error);
    }
    tx_poisoned.inner.Poison();

    if (db.Insert({}, {'p', 'o', 'i', 's', 'o', 'n'}, item_element, &tx_poisoned, &error)) {
      Fail("Insert should fail on poisoned tx handle");
    }
    ExpectErrorContains("Insert poisoned tx", error, "poisoned");

    bool found = false;
    std::vector<uint8_t> got;
    if (db.Get({}, {'p', 'o', 'i', 's', 'o', 'n'}, &got, &found, &tx_poisoned, &error)) {
      Fail("Get should fail on poisoned tx handle");
    }
    ExpectErrorContains("Get poisoned tx", error, "poisoned");

    if (db.CommitTransaction(&tx_poisoned, &error)) {
      Fail("CommitTransaction should fail on poisoned tx handle");
    }
    ExpectErrorContains("Commit poisoned tx", error, "poisoned");

    if (!db.RollbackTransaction(&tx_poisoned, &error)) {
      Fail("RollbackTransaction(tx_poisoned) failed: " + error);
    }

    if (!db.Insert({}, {'p', 'o', 's', 't'}, item_element, &tx_poisoned, &error)) {
      Fail("Insert should work after poisoned tx rollback reuse: " + error);
    }
    if (!db.CommitTransaction(&tx_poisoned, &error)) {
      Fail("Commit after poisoned tx rollback reuse failed: " + error);
    }
  }

  {
    // Rolled-back handles are reusable in Rust and in this facade; ensure new APIs
    // follow the same lifecycle contract.
    if (!db.Insert({}, {'n', 'e', 'w', 'a', 'p', 'i'}, tree_element, &error)) {
      Fail("Insert new-api lifecycle root tree failed: " + error);
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(new-api lifecycle) failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(new-api lifecycle) failed: " + error);
    }

    bool inserted = false;
    if (!db.InsertIfNotExists(
            {{'n', 'e', 'w', 'a', 'p', 'i'}}, {'k'}, item_element, &inserted, &tx, &error)) {
      Fail("InsertIfNotExists should work on rolled-back tx handle: " + error);
    }
    if (!inserted) {
      Fail("InsertIfNotExists on rolled-back tx handle should insert");
    }

    bool changed = false;
    bool had_previous = false;
    std::vector<uint8_t> previous;
    if (!db.InsertIfChangedValue({{'n', 'e', 'w', 'a', 'p', 'i'}},
                                 {'k'},
                                 item_element,
                                 &changed,
                                 &previous,
                                 &had_previous,
                                 &tx,
                                 &error)) {
      Fail("InsertIfChangedValue should work on rolled-back tx handle: " + error);
    }

    bool had_existing = false;
    if (!db.InsertIfNotExistsReturnExisting({{'n', 'e', 'w', 'a', 'p', 'i'}},
                                            {'k'},
                                            item_element,
                                            &previous,
                                            &had_existing,
                                            &tx,
                                            &error)) {
      Fail("InsertIfNotExistsReturnExisting should work on rolled-back tx handle: " + error);
    }

    if (!db.InsertItem({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'k', '2'}, {'v', '2'}, &tx, &error)) {
      Fail("InsertItem should work on rolled-back tx handle: " + error);
    }
    if (!db.InsertEmptyTree({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'t'}, &tx, &error)) {
      Fail("InsertEmptyTree should work on rolled-back tx handle: " + error);
    }
    if (!db.InsertBigSumTree({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'b'}, &tx, &error)) {
      Fail("InsertBigSumTree should work on rolled-back tx handle: " + error);
    }
    if (!db.InsertCountTree({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'c'}, &tx, &error)) {
      Fail("InsertCountTree should work on rolled-back tx handle: " + error);
    }
    if (!db.InsertProvableCountTree({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'p'}, &tx, &error)) {
      Fail("InsertProvableCountTree should work on rolled-back tx handle: " + error);
    }
    if (!db.InsertSumItem({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'s'}, 99, &tx, &error)) {
      Fail("InsertSumItem should work on rolled-back tx handle: " + error);
    }
    bool deleted = false;
    if (!db.DeleteIfEmptyTree({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'t'}, &deleted, &tx, &error)) {
      Fail("DeleteIfEmptyTree should work on rolled-back tx handle: " + error);
    }
    if (!deleted) {
      Fail("DeleteIfEmptyTree on rolled-back tx handle should delete empty subtree");
    }

    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction(new-api lifecycle) failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> got;
    if (!db.Get({{'n', 'e', 'w', 'a', 'p', 'i'}}, {'k', '2'}, &got, &found, &error)) {
      Fail("Get committed InsertItem result failed: " + error);
    }
    if (!found) {
      Fail("InsertItem result should be present after commit");
    }
  }

  {
    // Non-tree root key.
    if (!db.Insert({}, {'n', 'o', 'n', 't', 'r', 'e', 'e'}, item_element, &error)) {
      Fail("Insert root non-tree failed: " + error);
    }
    if (db.Insert({{'n', 'o', 'n', 't', 'r', 'e', 'e'}}, {'k'}, item_element, &error)) {
      Fail("Insert should fail when path points to non-tree element");
    }
    ExpectErrorContains("Insert path non-tree", error, "not a tree");

    if (db.Insert({{'m', 'i', 's', 's', 'i', 'n', 'g'}}, {'k'}, item_element, &error)) {
      Fail("Insert should fail when path is missing");
    }
    ExpectErrorContains("Insert path missing", error, "path not found");

    bool is_empty = false;
    if (db.IsEmptyTree({{'n', 'o', 'n', 't', 'r', 'e', 'e'}}, &is_empty, &error)) {
      Fail("IsEmptyTree should fail when path points to non-tree element");
    }
    ExpectErrorContains("IsEmptyTree path non-tree", error, "not a tree");
    if (db.IsEmptyTree({{'m', 'i', 's', 's', 'i', 'n', 'g'}}, &is_empty, &error)) {
      Fail("IsEmptyTree should fail when path is missing");
    }
    ExpectErrorContains("IsEmptyTree path missing", error, "path not found");
    std::vector<std::vector<std::vector<uint8_t>>> subtrees;
    if (db.FindSubtrees({{'n', 'o', 'n', 't', 'r', 'e', 'e'}}, &subtrees, &error)) {
      Fail("FindSubtrees should fail when path points to non-tree element");
    }
    ExpectErrorContains("FindSubtrees path non-tree", error, "not a tree");
    if (db.FindSubtrees({{'m', 'i', 's', 's', 'i', 'n', 'g'}}, &subtrees, &error)) {
      Fail("FindSubtrees should fail when path is missing");
    }
    ExpectErrorContains("FindSubtrees path missing", error, "path not found");
    bool deleted = false;
    if (db.DeleteIfEmptyTree({}, {'n', 'o', 'n', 't', 'r', 'e', 'e'}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree should fail for non-tree target");
    }
    ExpectErrorContains("DeleteIfEmptyTree non-tree target", error, "tree element");
    if (!db.Insert({}, {'n', 't', 'p'}, tree_element, &error)) {
      Fail("Insert non-tree parent failed: " + error);
    }
    if (!db.Insert({{'n', 't', 'p'}}, {'n', 't', 'v'}, item_element, &error)) {
      Fail("Insert non-tree value under parent failed: " + error);
    }
    uint16_t deleted_count = 0;
    if (!db.DeleteUpTreeWhileEmpty({{'n', 't', 'p'}}, {'n', 't', 'v'}, &deleted_count, &error)) {
      Fail("DeleteUpTreeWhileEmpty should delete non-tree target at first level: " + error);
    }
    if (deleted_count != 1) {
      Fail("DeleteUpTreeWhileEmpty should report one deletion for non-tree target");
    }
    if (db.DeleteIfEmptyTree({}, {'m', 'i', 's', 's', 'i', 'n', 'g'}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree should fail for missing target key");
    }
    ExpectErrorContainsAny("DeleteIfEmptyTree missing target key", error, {"not found", "missing"});
    if (db.DeleteUpTreeWhileEmpty({{'m', 'i', 's', 's', 'i', 'n', 'g'}}, {'k'}, &deleted_count, &error)) {
      Fail("DeleteUpTreeWhileEmpty should fail for missing path");
    }
    ExpectErrorContainsAny("DeleteUpTreeWhileEmpty missing path", error, {"path not found", "not found"});
  }

  {
    // Valid tree path still works.
    if (!db.Insert({}, {'t', 'r', 'e', 'e'}, tree_element, &error)) {
      Fail("Insert root tree failed: " + error);
    }
    if (!db.Insert({{'t', 'r', 'e', 'e'}}, {'k'}, item_element, &error)) {
      Fail("Insert under tree path failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> got;
    if (!db.Get({{'t', 'r', 'e', 'e'}}, {'k'}, &got, &found, &error)) {
      Fail("Get under tree path failed: " + error);
    }
    if (!found || got != item_element) {
      Fail("Get under tree path mismatch");
    }

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (!db.QueryRange({{'t', 'r', 'e', 'e'}}, {'k'}, {'z'}, true, true, &out, &error)) {
      Fail("QueryRange under tree path failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'k'})) {
      Fail("QueryRange under tree path mismatch");
    }
    grovedb::PathQuery query_raw_single =
        grovedb::PathQuery::NewSingleKey({{'t', 'r', 'e', 'e'}}, {'k'});
    if (!db.QueryRaw(query_raw_single, &out, &error)) {
      Fail("QueryRaw under tree path failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'k'}) ||
        out[0].second != item_element) {
      Fail("QueryRaw under tree path mismatch");
    }
    grovedb::PathQuery query_raw_keys = grovedb::PathQuery::New(
        {{'t', 'r', 'e', 'e'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 2, std::nullopt));
    query_raw_keys.query.query.items.push_back(grovedb::QueryItem::Key({'m'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> key_rows;
    if (!db.QueryRawKeysOptional(query_raw_keys, &key_rows, &error)) {
      Fail("QueryRawKeysOptional under tree path failed: " + error);
    }
    if (key_rows.size() != 2) {
      Fail("QueryRawKeysOptional should return 2 rows");
    }
    if (key_rows[0].path != std::vector<std::vector<uint8_t>>({{'t', 'r', 'e', 'e'}}) ||
        key_rows[0].key != std::vector<uint8_t>({'k'}) || !key_rows[0].element_found ||
        key_rows[0].element_bytes != item_element) {
      Fail("QueryRawKeysOptional first row mismatch");
    }
    if (key_rows[1].path != std::vector<std::vector<uint8_t>>({{'t', 'r', 'e', 'e'}}) ||
        key_rows[1].key != std::vector<uint8_t>({'m'}) || key_rows[1].element_found ||
        !key_rows[1].element_bytes.empty()) {
      Fail("QueryRawKeysOptional second row mismatch");
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}}, {'q', 't'}, {'q', 'v'}, &error)) {
      Fail("InsertItem query keys optional target failed: " + error);
    }
    grovedb::ReferencePathType query_keys_ref;
    query_keys_ref.kind = grovedb::ReferencePathKind::kSibling;
    query_keys_ref.key = {'q', 't'};
    if (!db.InsertReference({{'t', 'r', 'e', 'e'}}, {'q', 'r'}, query_keys_ref, &error)) {
      Fail("InsertReference query keys optional ref failed: " + error);
    }
    grovedb::PathQuery query_keys_optional = grovedb::PathQuery::New(
        {{'t', 'r', 'e', 'e'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'q', 'r'})),
            2,
            std::nullopt));
    query_keys_optional.query.query.items.push_back(grovedb::QueryItem::Key({'m'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> query_keys_rows;
    if (!db.QueryKeysOptional(query_keys_optional, &query_keys_rows, &error)) {
      Fail("QueryKeysOptional under tree path failed: " + error);
    }
    if (query_keys_rows.size() != 2) {
      Fail("QueryKeysOptional should return 2 rows");
    }
    const grovedb::GroveDb::PathKeyOptionalElement* query_keys_ref_row = nullptr;
    const grovedb::GroveDb::PathKeyOptionalElement* query_keys_missing_row = nullptr;
    for (const auto& row : query_keys_rows) {
      if (row.key == std::vector<uint8_t>({'q', 'r'})) {
        query_keys_ref_row = &row;
      } else if (row.key == std::vector<uint8_t>({'m'})) {
        query_keys_missing_row = &row;
      }
    }
    if (query_keys_ref_row == nullptr || !query_keys_ref_row->element_found ||
        query_keys_missing_row == nullptr || query_keys_missing_row->element_found) {
      Fail("QueryKeysOptional row presence mismatch");
    }
    grovedb::ElementItem query_keys_decoded_item;
    if (!grovedb::DecodeItemFromElementBytes(
            query_keys_ref_row->element_bytes, &query_keys_decoded_item, &error)) {
      Fail("QueryKeysOptional should resolve reference to item bytes: " + error);
    }
    if (query_keys_decoded_item.value != std::vector<uint8_t>({'q', 'v'})) {
      Fail("QueryKeysOptional resolved item value mismatch");
    }
    grovedb::GroveDb::Transaction tx_query_raw;
    if (!db.StartTransaction(&tx_query_raw, &error)) {
      Fail("StartTransaction query_raw tx failed: " + error);
    }
    if (!db.Insert({{'t', 'r', 'e', 'e'}}, {'u'}, item_element, &tx_query_raw, &error)) {
      Fail("Insert query_raw tx key failed: " + error);
    }
    grovedb::PathQuery query_raw_tx_key =
        grovedb::PathQuery::NewSingleKey({{'t', 'r', 'e', 'e'}}, {'u'});
    if (!db.QueryRaw(query_raw_tx_key, &out, &tx_query_raw, &error)) {
      Fail("QueryRaw in tx failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'u'}) ||
        out[0].second != item_element) {
      Fail("QueryRaw in tx should include uncommitted key");
    }
    if (!db.QueryRaw(query_raw_tx_key, &out, &error)) {
      Fail("QueryRaw outside tx before commit failed: " + error);
    }
    if (!out.empty()) {
      Fail("QueryRaw outside tx should not see uncommitted key");
    }
    if (!db.CommitTransaction(&tx_query_raw, &error)) {
      Fail("CommitTransaction query_raw tx failed: " + error);
    }
    if (!db.QueryRaw(query_raw_tx_key, &out, &error)) {
      Fail("QueryRaw outside tx after commit failed: " + error);
    }
    if (out.size() != 1 || out[0].first != std::vector<uint8_t>({'u'}) ||
        out[0].second != item_element) {
      Fail("QueryRaw outside tx after commit should include committed key");
    }
    grovedb::GroveDb::Transaction tx_query_raw_keys;
    if (!db.StartTransaction(&tx_query_raw_keys, &error)) {
      Fail("StartTransaction query_raw_keys tx failed: " + error);
    }
    if (!db.Insert({{'t', 'r', 'e', 'e'}}, {'w'}, item_element, &tx_query_raw_keys, &error)) {
      Fail("Insert query_raw_keys tx key failed: " + error);
    }
    grovedb::PathQuery query_raw_keys_tx = grovedb::PathQuery::New(
        {{'t', 'r', 'e', 'e'}},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'w'})),
            2,
            std::nullopt));
    query_raw_keys_tx.query.query.items.push_back(grovedb::QueryItem::Key({'m'}));
    std::vector<grovedb::GroveDb::PathKeyOptionalElement> key_rows_tx;
    auto find_row_by_key =
        [](const std::vector<grovedb::GroveDb::PathKeyOptionalElement>& rows,
           const std::vector<uint8_t>& key)
        -> const grovedb::GroveDb::PathKeyOptionalElement* {
      for (const auto& row : rows) {
        if (row.key == key) {
          return &row;
        }
      }
      return nullptr;
    };
    if (!db.QueryRawKeysOptional(query_raw_keys_tx, &key_rows_tx, &tx_query_raw_keys, &error)) {
      Fail("QueryRawKeysOptional in tx failed: " + error);
    }
    const auto* tx_w_row = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || tx_w_row == nullptr || !tx_w_row->element_found) {
      Fail("QueryRawKeysOptional in tx should include uncommitted key");
    }
    if (!db.QueryRawKeysOptional(query_raw_keys_tx, &key_rows_tx, &error)) {
      Fail("QueryRawKeysOptional outside tx before commit failed: " + error);
    }
    const auto* precommit_w_row = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || precommit_w_row == nullptr || precommit_w_row->element_found) {
      Fail("QueryRawKeysOptional outside tx should not see uncommitted key");
    }
    if (!db.QueryKeysOptional(query_raw_keys_tx, &key_rows_tx, &tx_query_raw_keys, &error)) {
      Fail("QueryKeysOptional in tx failed: " + error);
    }
    const auto* tx_w_row_resolved = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || tx_w_row_resolved == nullptr || !tx_w_row_resolved->element_found) {
      Fail("QueryKeysOptional in tx should include uncommitted key");
    }
    if (tx_w_row_resolved->element_bytes != item_element) {
      Fail("QueryKeysOptional in tx should resolve to inserted value");
    }
    if (!db.QueryKeysOptional(query_raw_keys_tx, &key_rows_tx, &error)) {
      Fail("QueryKeysOptional outside tx before commit failed: " + error);
    }
    const auto* precommit_w_row_resolved = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || precommit_w_row_resolved == nullptr ||
        precommit_w_row_resolved->element_found) {
      Fail("QueryKeysOptional outside tx should not see uncommitted key");
    }
    if (!db.CommitTransaction(&tx_query_raw_keys, &error)) {
      Fail("CommitTransaction query_raw_keys tx failed: " + error);
    }
    if (!db.QueryRawKeysOptional(query_raw_keys_tx, &key_rows_tx, &error)) {
      Fail("QueryRawKeysOptional outside tx after commit failed: " + error);
    }
    const auto* committed_w_row = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || committed_w_row == nullptr || !committed_w_row->element_found) {
      Fail("QueryRawKeysOptional outside tx after commit should include committed key");
    }
    if (!db.QueryKeysOptional(query_raw_keys_tx, &key_rows_tx, &error)) {
      Fail("QueryKeysOptional outside tx after commit failed: " + error);
    }
    const auto* committed_w_row_resolved = find_row_by_key(key_rows_tx, {'w'});
    if (key_rows_tx.size() != 2 || committed_w_row_resolved == nullptr ||
        !committed_w_row_resolved->element_found) {
      Fail("QueryKeysOptional outside tx after commit should include committed key");
    }
    if (committed_w_row_resolved->element_bytes != item_element) {
      Fail("QueryKeysOptional outside tx after commit should resolve committed value");
    }

    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'e', 'm', 'p'}, &error)) {
      Fail("InsertEmptyTree for IsEmptyTree positive path failed: " + error);
    }
    bool is_empty = false;
    if (!db.IsEmptyTree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, &is_empty, &error)) {
      Fail("IsEmptyTree empty subtree check failed: " + error);
    }
    if (!is_empty) {
      Fail("IsEmptyTree should report true for empty subtree");
    }
    if (!db.CheckSubtreeExistsInvalidPath({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, &error)) {
      Fail("CheckSubtreeExistsInvalidPath non-tx positive path failed: " + error);
    }
    if (db.CheckSubtreeExistsInvalidPath({{'t', 'r', 'e', 'e'}, {'m', 'i', 's', 's'}}, &error)) {
      Fail("CheckSubtreeExistsInvalidPath should fail for missing subtree");
    }
    ExpectErrorContains(
        "CheckSubtreeExistsInvalidPath missing subtree", error, "subtree doesn't exist");
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}}, {'n', 'o', 'n', 't', 'r', 'e', 'e'}, {'v'}, &error)) {
      Fail("InsertItem for CheckSubtreeExistsInvalidPath non-tree path failed: " + error);
    }
    if (db.CheckSubtreeExistsInvalidPath(
            {{'t', 'r', 'e', 'e'}, {'n', 'o', 'n', 't', 'r', 'e', 'e'}}, &error)) {
      Fail("CheckSubtreeExistsInvalidPath should fail when target is not a tree");
    }
    ExpectErrorContains("CheckSubtreeExistsInvalidPath non-tree subtree",
                        error,
                        "subtree doesn't exist");
    std::vector<std::vector<std::vector<uint8_t>>> subtrees;
    if (!db.FindSubtrees({{'t', 'r', 'e', 'e'}}, &subtrees, &error)) {
      Fail("FindSubtrees non-tx positive path failed: " + error);
    }
    grovedb::OperationCost find_subtrees_cost;
    std::vector<std::vector<std::vector<uint8_t>>> cost_subtrees;
    if (!db.FindSubtrees({{'t', 'r', 'e', 'e'}}, &cost_subtrees, &find_subtrees_cost, &error)) {
      Fail("FindSubtrees non-tx cost overload failed: " + error);
    }
    if (find_subtrees_cost.IsNothing()) {
      Fail("FindSubtrees non-tx cost overload should report non-empty cost");
    }
    const std::vector<std::vector<std::vector<uint8_t>>> expected_subtrees = {
        {{'t', 'r', 'e', 'e'}},
        {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}};
    if (subtrees != expected_subtrees) {
      Fail("FindSubtrees non-tx subtree set/order mismatch");
    }
    if (cost_subtrees != expected_subtrees) {
      Fail("FindSubtrees non-tx cost overload subtree set/order mismatch");
    }

    // Test FindSubtreesByKinds with filter for Tree (variant=2) only
    std::vector<std::vector<std::vector<uint8_t>>> filtered_subtrees;
    std::vector<uint64_t> tree_filter = {2};  // Tree only
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &filtered_subtrees, &tree_filter, &error)) {
      Fail("FindSubtreesByKinds non-tx positive path failed: " + error);
    }
    const std::vector<std::vector<std::vector<uint8_t>>> expected_tree_only = {
        {{'t', 'r', 'e', 'e'}},
        {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}};
    if (filtered_subtrees != expected_tree_only) {
      Fail("FindSubtreesByKinds Tree filter subtree set/order mismatch");
    }

    // Test FindSubtreesByKinds with filter for SumTree (variant=4) - should return empty
    std::vector<std::vector<std::vector<uint8_t>>> sumtree_filtered;
    std::vector<uint64_t> sumtree_filter = {4};  // SumTree only
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &sumtree_filtered, &sumtree_filter, &error)) {
      Fail("FindSubtreesByKinds SumTree filter positive path failed: " + error);
    }
    const std::vector<std::vector<std::vector<uint8_t>>> expected_sumtree_only = {
        {{'t', 'r', 'e', 'e'}}};  // Root is Tree (2), not SumTree (4), so only root path if it matches
    // Actually root is Tree (variant 2), so with SumTree filter it should be empty
    if (!sumtree_filtered.empty()) {
      Fail("FindSubtreesByKinds SumTree filter should return empty for Tree-only subtrees");
    }

    // Regression test: filter must traverse non-matching intermediate nodes
    // Structure: tree/emp (Tree, variant 2) -> tree/emp/bst (BigSumTree, variant 5)
    // tree/emp already exists as an empty Tree from earlier in the test
    if (!db.InsertBigSumTree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, {'b', 's', 't'}, &error)) {
      Fail("InsertBigSumTree for filter traversal test failed: " + error);
    }
    std::vector<std::vector<std::vector<uint8_t>>> deep_filtered;
    std::vector<uint64_t> bigsumtree_filter = {5};  // BigSumTree only
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &deep_filtered, &bigsumtree_filter, &error)) {
      Fail("FindSubtreesByKinds deep BigSumTree filter failed: " + error);
    }
    // Root is Tree (2), emp is Tree (2) — neither matches. Only tree/emp/bst (BigSumTree=5) should appear.
    const std::vector<std::vector<std::vector<uint8_t>>> expected_deep = {
        {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}, {'b', 's', 't'}}};
    if (deep_filtered != expected_deep) {
      Fail("FindSubtreesByKinds should find BigSumTree through non-matching intermediate Tree");
    }
    // Clean up: remove bst so subsequent tests that depend on emp being empty still work
    bool bst_deleted = false;
    if (!db.Delete({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, {'b', 's', 't'}, &bst_deleted, &error)) {
      Fail("Delete bst cleanup failed: " + error);
    }
    if (!bst_deleted) {
      Fail("Delete bst cleanup should report deleted=true");
    }

    // Test FindSubtreesByKinds with empty filter (should return all, same as FindSubtrees)
    std::vector<std::vector<std::vector<uint8_t>>> empty_filter_subtrees;
    std::vector<uint64_t> empty_filter;
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &empty_filter_subtrees, &empty_filter, &error)) {
      Fail("FindSubtreesByKinds empty filter positive path failed: " + error);
    }
    if (empty_filter_subtrees != expected_subtrees) {
      Fail("FindSubtreesByKinds empty filter should return all subtrees");
    }

    // Test FindSubtreesByKinds with cost
    grovedb::OperationCost find_subtrees_by_kinds_cost;
    std::vector<std::vector<std::vector<uint8_t>>> cost_filtered_subtrees;
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &cost_filtered_subtrees, &tree_filter, &find_subtrees_by_kinds_cost, &error)) {
      Fail("FindSubtreesByKinds non-tx cost overload failed: " + error);
    }
    if (find_subtrees_by_kinds_cost.IsNothing()) {
      Fail("FindSubtreesByKinds non-tx cost overload should report non-empty cost");
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, {'v'}, {'1'}, &error)) {
      Fail("InsertItem for IsEmptyTree non-empty path failed: " + error);
    }
    if (!db.IsEmptyTree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, &is_empty, &error)) {
      Fail("IsEmptyTree non-empty subtree check failed: " + error);
    }
    if (is_empty) {
      Fail("IsEmptyTree should report false for non-empty subtree");
    }
    bool deleted = false;
    if (!db.DeleteIfEmptyTree({{'t', 'r', 'e', 'e'}}, {'e', 'm', 'p'}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree non-empty subtree check failed: " + error);
    }
    if (deleted) {
      Fail("DeleteIfEmptyTree should report deleted=false for non-empty subtree");
    }
    if (!db.Delete({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, {'v'}, &deleted, &error)) {
      Fail("Delete subtree value for DeleteIfEmptyTree positive path failed: " + error);
    }
    if (!deleted) {
      Fail("Delete subtree value should report deleted=true");
    }
    if (!db.DeleteIfEmptyTree({{'t', 'r', 'e', 'e'}}, {'e', 'm', 'p'}, &deleted, &error)) {
      Fail("DeleteIfEmptyTree empty subtree check failed: " + error);
    }
    if (!deleted) {
      Fail("DeleteIfEmptyTree should report deleted=true for empty subtree");
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'u', 'p', 'a'}, &error)) {
      Fail("InsertEmptyTree up-tree parent A failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}}, {'u', 'p', 'b'}, &error)) {
      Fail("InsertEmptyTree up-tree parent B failed: " + error);
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}, {'u', 'p', 'b'}},
                       {'v'},
                       {'1'},
                       &error)) {
      Fail("InsertItem up-tree leaf failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}}, {'s', 'i', 'b'}, &error)) {
      Fail("InsertEmptyTree up-tree sibling failed: " + error);
    }
    uint16_t deleted_count = 0;
    if (!db.DeleteUpTreeWhileEmpty({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}, {'u', 'p', 'b'}},
                                   {'v'},
                                   &deleted_count,
                                   &error)) {
      Fail("DeleteUpTreeWhileEmpty positive path failed: " + error);
    }
    if (deleted_count != 2) {
      Fail("DeleteUpTreeWhileEmpty should delete leaf + now-empty parent");
    }
    bool found_up = false;
    std::vector<uint8_t> got_up;
    if (!db.Get({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}}, {'u', 'p', 'b'}, &got_up, &found_up, &error)) {
      Fail("Get after DeleteUpTreeWhileEmpty parent deletion failed: " + error);
    }
    if (found_up) {
      Fail("DeleteUpTreeWhileEmpty should remove empty intermediate tree");
    }
    if (!db.Get({{'t', 'r', 'e', 'e'}, {'u', 'p', 'a'}}, {'s', 'i', 'b'}, &got_up, &found_up, &error)) {
      Fail("Get after DeleteUpTreeWhileEmpty sibling check failed: " + error);
    }
    if (!found_up) {
      Fail("DeleteUpTreeWhileEmpty should keep non-empty ancestor subtree");
    }

    // Test DeleteUpTreeWhileEmpty with stop_path_height
    {
      // Create root tree for stop_test
      if (!db.InsertEmptyTree({}, {'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}, &error)) {
        Fail("InsertEmptyTree stop_test root failed: " + error);
      }
      // Create a 3-level nested empty tree structure: /stop_test/l1/l2/l3
      if (!db.InsertEmptyTree({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}}, {'l', '1'}, &error)) {
        Fail("InsertEmptyTree stop_test/l1 failed: " + error);
      }
      if (!db.InsertEmptyTree({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}, {'l', '1'}}, {'l', '2'}, &error)) {
        Fail("InsertEmptyTree stop_test/l1/l2 failed: " + error);
      }
      if (!db.InsertEmptyTree({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}, {'l', '1'}, {'l', '2'}}, {'l', '3'}, &error)) {
        Fail("InsertEmptyTree stop_test/l1/l2/l3 failed: " + error);
      }
      
      // Delete with stop_path_height=1 should stop at path height 1 (after deleting l3 and l2)
      uint16_t stop_deleted_count = 0;
      if (!db.DeleteUpTreeWhileEmpty(
              {{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}, {'l', '1'}, {'l', '2'}},
              {'l', '3'},
              &stop_deleted_count,
              static_cast<uint16_t>(1),  // stop_path_height
              static_cast<grovedb::OperationCost*>(nullptr),
              static_cast<grovedb::GroveDb::Transaction*>(nullptr),
              &error)) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height failed: " + error);
      }
      // Should delete l3 and l2 (2 levels), but stop before l1 because path height 1 is the limit
      if (stop_deleted_count != 2) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height=1 should delete 2 levels, got " + std::to_string(stop_deleted_count));
      }
      
      // l1 should still exist (we stopped at height 1)
      bool l1_exists = false;
      std::vector<uint8_t> l1_element;
      if (!db.Get({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}}, {'l', '1'}, &l1_element, &l1_exists, &error)) {
        Fail("Get l1 after stop_path_height delete failed: " + error);
      }
      if (!l1_exists) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height=1 should keep l1");
      }
      
      // Clean up for next test
      if (!db.Delete({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}}, {'l', '1'}, &l1_exists, &error)) {
        Fail("Cleanup delete l1 failed: " + error);
      }
      if (!db.Delete({{'s', 't', 'o', 'p', '_', 't', 'e', 's', 't'}}, {'l', '1'}, &l1_exists, &error)) {
        // Delete the now-empty l1 tree
      }
    }
    
    // Test DeleteUpTreeWhileEmpty with stop_path_height=0 (delete everything up to root level)
    {
      // Create root tree for stop_zero
      if (!db.InsertEmptyTree({}, {'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}, &error)) {
        Fail("InsertEmptyTree stop_zero root failed: " + error);
      }
      // Create a 2-level nested empty tree structure: /stop_zero/l1/l2
      if (!db.InsertEmptyTree({{'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}}, {'l', '1'}, &error)) {
        Fail("InsertEmptyTree stop_zero/l1 failed: " + error);
      }
      if (!db.InsertEmptyTree({{'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}, {'l', '1'}}, {'l', '2'}, &error)) {
        Fail("InsertEmptyTree stop_zero/l1/l2 failed: " + error);
      }
      
      // Delete with stop_path_height=0 should delete l2 and l1, but stop before the root level
      uint16_t zero_deleted_count = 0;
      if (!db.DeleteUpTreeWhileEmpty(
              {{'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}, {'l', '1'}},
              {'l', '2'},
              &zero_deleted_count,
              static_cast<uint16_t>(0),  // stop_path_height=0 means stop at root level
              static_cast<grovedb::OperationCost*>(nullptr),
              static_cast<grovedb::GroveDb::Transaction*>(nullptr),
              &error)) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height=0 failed: " + error);
      }
      // Should delete l2 and l1 (2 levels)
      if (zero_deleted_count != 2) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height=0 should delete 2 levels, got " + std::to_string(zero_deleted_count));
      }
      
      // The root level key 'stop_zero' should still exist as an empty tree
      bool root_exists = false;
      std::vector<uint8_t> root_element;
      if (!db.Get({{'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}}, {'l', '1'}, &root_element, &root_exists, &error)) {
        Fail("Get l1 after stop_path_height=0 delete failed: " + error);
      }
      if (root_exists) {
        Fail("DeleteUpTreeWhileEmpty with stop_path_height=0 should delete l1");
      }
      
      // Clean up
      bool deleted = false;
      if (!db.Delete({{'s', 't', 'o', 'p', '_', 'z', 'e', 'r', 'o'}}, {'l', '1'}, &deleted, &error)) {
        // May already be deleted
      }
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(IsEmptyTree tx positive path) failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'e', 'm', 'p', 't', 'x'}, &tx, &error)) {
      Fail("InsertEmptyTree in tx for IsEmptyTree positive path failed: " + error);
    }
    if (!db.IsEmptyTree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}, &is_empty, &tx, &error)) {
      Fail("IsEmptyTree empty subtree in tx check failed: " + error);
    }
    if (!is_empty) {
      Fail("IsEmptyTree should report true for tx-local empty subtree");
    }
    if (!db.CheckSubtreeExistsInvalidPath(
            {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}, &tx, &error)) {
      Fail("CheckSubtreeExistsInvalidPath in tx should see uncommitted subtree: " + error);
    }
    if (db.CheckSubtreeExistsInvalidPath(
            {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}, &error)) {
      Fail("CheckSubtreeExistsInvalidPath outside tx should not see uncommitted subtree");
    }
    ExpectErrorContains("CheckSubtreeExistsInvalidPath outside tx before commit",
                        error,
                        "subtree doesn't exist");
    std::vector<std::vector<std::vector<uint8_t>>> tx_subtrees;
    if (!db.FindSubtrees({{'t', 'r', 'e', 'e'}}, &tx_subtrees, &tx, &error)) {
      Fail("FindSubtrees in tx failed: " + error);
    }
    grovedb::OperationCost tx_find_subtrees_cost;
    std::vector<std::vector<std::vector<uint8_t>>> tx_cost_subtrees;
    if (!db.FindSubtrees(
            {{'t', 'r', 'e', 'e'}}, &tx_cost_subtrees, &tx_find_subtrees_cost, &tx, &error)) {
      Fail("FindSubtrees in tx cost overload failed: " + error);
    }
    if (tx_find_subtrees_cost.IsNothing()) {
      Fail("FindSubtrees in tx cost overload should report non-empty cost");
    }
    bool saw_tx_local_subtree = false;
    for (const auto& subtree : tx_subtrees) {
      if (subtree == std::vector<std::vector<uint8_t>>{{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}) {
        saw_tx_local_subtree = true;
      }
    }
    if (!saw_tx_local_subtree) {
      Fail("FindSubtrees in tx should include uncommitted subtree");
    }
    bool saw_tx_local_subtree_in_cost = false;
    for (const auto& subtree : tx_cost_subtrees) {
      if (subtree == std::vector<std::vector<uint8_t>>{{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}) {
        saw_tx_local_subtree_in_cost = true;
      }
    }
    if (!saw_tx_local_subtree_in_cost) {
      Fail("FindSubtrees in tx cost overload should include uncommitted subtree");
    }

    // Test FindSubtreesByKinds in tx
    std::vector<std::vector<std::vector<uint8_t>>> tx_filtered_subtrees;
    std::vector<uint64_t> tx_tree_filter = {2};  // Tree only
    if (!db.FindSubtreesByKinds({{'t', 'r', 'e', 'e'}}, &tx_filtered_subtrees, &tx_tree_filter, &tx, &error)) {
      Fail("FindSubtreesByKinds in tx failed: " + error);
    }
    bool saw_tx_local_subtree_filtered = false;
    for (const auto& subtree : tx_filtered_subtrees) {
      if (subtree == std::vector<std::vector<uint8_t>>{{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}) {
        saw_tx_local_subtree_filtered = true;
      }
    }
    if (!saw_tx_local_subtree_filtered) {
      Fail("FindSubtreesByKinds in tx should include uncommitted subtree");
    }

    // Test FindSubtreesByKinds in tx with cost
    grovedb::OperationCost tx_find_subtrees_by_kinds_cost;
    std::vector<std::vector<std::vector<uint8_t>>> tx_cost_filtered_subtrees;
    if (!db.FindSubtreesByKinds(
            {{'t', 'r', 'e', 'e'}}, &tx_cost_filtered_subtrees, &tx_tree_filter, &tx_find_subtrees_by_kinds_cost, &tx, &error)) {
      Fail("FindSubtreesByKinds in tx cost overload failed: " + error);
    }
    if (tx_find_subtrees_by_kinds_cost.IsNothing()) {
      Fail("FindSubtreesByKinds in tx cost overload should report non-empty cost");
    }
    bool saw_tx_local_subtree_cost_filtered = false;
    for (const auto& subtree : tx_cost_filtered_subtrees) {
      if (subtree == std::vector<std::vector<uint8_t>>{{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}) {
        saw_tx_local_subtree_cost_filtered = true;
      }
    }
    if (!saw_tx_local_subtree_cost_filtered) {
      Fail("FindSubtreesByKinds in tx cost overload should include uncommitted subtree");
    }

    // Test CountSubtrees non-tx
    uint64_t subtree_count = 0;
    if (!db.CountSubtrees({{'t', 'r', 'e', 'e'}}, &subtree_count, &error)) {
      Fail("CountSubtrees non-tx positive path failed: " + error);
    }
    // Outside tx: upa + sib = 2 child subtrees (emptx is tx-local, not visible)
    if (subtree_count != 2) {
      Fail("CountSubtrees non-tx should count 2 (upa + sib): got " + std::to_string(subtree_count));
    }

    // Test CountSubtrees with cost
    grovedb::OperationCost count_cost;
    uint64_t count_with_cost = 0;
    if (!db.CountSubtrees({{'t', 'r', 'e', 'e'}}, &count_with_cost, &count_cost, &error)) {
      Fail("CountSubtrees non-tx cost overload failed: " + error);
    }
    if (count_cost.IsNothing()) {
      Fail("CountSubtrees non-tx cost overload should report non-empty cost");
    }
    if (count_with_cost != 2) {
      Fail("CountSubtrees non-tx cost overload should count 2: got " + std::to_string(count_with_cost));
    }

    // Test CountSubtrees in tx (should see tx-local subtree)
    uint64_t tx_count = 0;
    if (!db.CountSubtrees({{'t', 'r', 'e', 'e'}}, &tx_count, &tx, &error)) {
      Fail("CountSubtrees in tx failed: " + error);
    }
    // In tx, we should see upa + sib + emptx = 3
    if (tx_count != 3) {
      Fail("CountSubtrees in tx should count 3 (upa + sib + emptx): got " + std::to_string(tx_count));
    }

    // Test CountSubtrees in tx with cost
    grovedb::OperationCost tx_count_cost;
    uint64_t tx_count_with_cost = 0;
    if (!db.CountSubtrees({{'t', 'r', 'e', 'e'}}, &tx_count_with_cost, &tx_count_cost, &tx, &error)) {
      Fail("CountSubtrees in tx cost overload failed: " + error);
    }
    if (tx_count_cost.IsNothing()) {
      Fail("CountSubtrees in tx cost overload should report non-empty cost");
    }
    if (tx_count_with_cost != 3) {
      Fail("CountSubtrees in tx cost overload should count 3: got " + std::to_string(tx_count_with_cost));
    }

    // Test GetSubtreeRoot non-tx
    std::vector<uint8_t> root_key;
    std::vector<uint8_t> element_bytes;
    if (!db.GetSubtreeRoot({{'t', 'r', 'e', 'e'}}, &root_key, &element_bytes, &error)) {
      Fail("GetSubtreeRoot non-tx failed: " + error);
    }
    if (root_key != std::vector<uint8_t>{'t', 'r', 'e', 'e'}) {
      Fail("GetSubtreeRoot returned wrong root key");
    }
    uint64_t variant_check = 0;
    if (!grovedb::DecodeElementVariant(element_bytes, &variant_check, &error)) {
      Fail("GetSubtreeRoot returned invalid element bytes: " + error);
    }
    if (variant_check != 2) {
      Fail("GetSubtreeRoot should return Tree variant (2), got: " + std::to_string(variant_check));
    }

    // Test GetSubtreeRoot with cost
    grovedb::OperationCost subtree_cost;
    std::vector<uint8_t> subtree_root_key;
    std::vector<uint8_t> subtree_element_bytes;
    if (!db.GetSubtreeRoot({{'t', 'r', 'e', 'e'}}, &subtree_root_key, &subtree_element_bytes, &subtree_cost, &error)) {
      Fail("GetSubtreeRoot cost overload failed: " + error);
    }
    if (subtree_cost.IsNothing()) {
      Fail("GetSubtreeRoot cost overload should report non-empty cost");
    }

    // Test GetSubtreeRoot in tx (should see tx-local subtree)
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("StartTransaction for GetSubtreeRoot tx test failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'t', 'x', 's'}, &tx2, &error)) {
      Fail("InsertEmptyTree in tx for GetSubtreeRoot failed: " + error);
    }
    std::vector<uint8_t> tx_root_key;
    std::vector<uint8_t> tx_element_bytes;
    if (!db.GetSubtreeRoot({{'t', 'r', 'e', 'e'}}, &tx_root_key, &tx_element_bytes, &tx2, &error)) {
      Fail("GetSubtreeRoot in tx failed: " + error);
    }
    // Root key should be the last path component
    if (tx_root_key != std::vector<uint8_t>{'t', 'r', 'e', 'e'}) {
      Fail("GetSubtreeRoot in tx returned wrong root key");
    }
    if (!db.RollbackTransaction(&tx2, &error)) {
      Fail("RollbackTransaction(GetSubtreeRoot tx test) failed: " + error);
    }

    // Test GetSubtreeRoot with cost in tx
    grovedb::GroveDb::Transaction tx3;
    if (!db.StartTransaction(&tx3, &error)) {
      Fail("StartTransaction for GetSubtreeRoot tx cost test failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'t', 'x', 'c'}, &tx3, &error)) {
      Fail("InsertEmptyTree in tx for GetSubtreeRoot cost failed: " + error);
    }
    grovedb::OperationCost tx_subtree_cost;
    std::vector<uint8_t> tx_cost_root_key;
    std::vector<uint8_t> tx_cost_element_bytes;
    if (!db.GetSubtreeRoot({{'t', 'r', 'e', 'e'}}, &tx_cost_root_key, &tx_cost_element_bytes, &tx_subtree_cost, &tx3, &error)) {
      Fail("GetSubtreeRoot in tx cost overload failed: " + error);
    }
    if (tx_subtree_cost.IsNothing()) {
      Fail("GetSubtreeRoot in tx cost overload should report non-empty cost");
    }
    if (!db.RollbackTransaction(&tx3, &error)) {
      Fail("RollbackTransaction(GetSubtreeRoot tx cost test) failed: " + error);
    }

    std::vector<std::vector<std::vector<uint8_t>>> outside_tx_subtrees;
    if (!db.FindSubtrees({{'t', 'r', 'e', 'e'}}, &outside_tx_subtrees, &error)) {
      Fail("FindSubtrees outside tx failed: " + error);
    }
    for (const auto& subtree : outside_tx_subtrees) {
      if (subtree == std::vector<std::vector<uint8_t>>{{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}) {
        Fail("FindSubtrees outside tx should not include uncommitted subtree");
      }
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}, {'v'}, {'2'}, &tx, &error)) {
      Fail("InsertItem in tx for IsEmptyTree positive path failed: " + error);
    }
    if (!db.IsEmptyTree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p', 't', 'x'}}, &is_empty, &tx, &error)) {
      Fail("IsEmptyTree non-empty subtree in tx check failed: " + error);
    }
    if (is_empty) {
      Fail("IsEmptyTree should report false for tx-local non-empty subtree");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(IsEmptyTree tx positive path) failed: " + error);
    }

    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'c', 'l', 'r'}, &error)) {
      Fail("InsertEmptyTree for ClearSubtree positive path failed: " + error);
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'a'}, {'1'}, &error)) {
      Fail("InsertItem ClearSubtree key a failed: " + error);
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'b'}, {'2'}, &error)) {
      Fail("InsertItem ClearSubtree key b failed: " + error);
    }
    if (!db.InsertItem({{'t', 'r', 'e', 'e'}}, {'r', 'g', 't'}, {'v'}, &error)) {
      Fail("InsertItem GetRaw target failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'r', 'g', 't'};
    if (!db.InsertReference({{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, ref_to_target, &error)) {
      Fail("InsertReference GetRaw source failed: " + error);
    }
    std::vector<uint8_t> get_raw;
    bool get_raw_found = false;
    if (!db.GetRaw({{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_raw, &get_raw_found, &error)) {
      Fail("GetRaw positive path failed: " + error);
    }
    if (!get_raw_found) {
      Fail("GetRaw should report found=true for existing reference");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(get_raw, &variant, &error)) {
      Fail("GetRaw decode variant failed: " + error);
    }
    if (variant != 1) {
      Fail("GetRaw should return unresolved reference bytes");
    }
    std::vector<uint8_t> get_resolved;
    bool get_found = false;
    if (!db.Get({{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_resolved, &get_found, &error)) {
      Fail("Get should resolve reference positive path failed: " + error);
    }
    if (!get_found) {
      Fail("Get should report found=true for existing reference");
    }
    grovedb::ElementItem get_item;
    if (!grovedb::DecodeItemFromElementBytes(get_resolved, &get_item, &error)) {
      Fail("Get decode resolved item failed: " + error);
    }
    if (get_item.value != std::vector<uint8_t>({'v'})) {
      Fail("Get should resolve reference to target item");
    }
    std::vector<uint8_t> followed;
    bool followed_found = false;
    if (!db.FollowReference(
            {{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &followed, &followed_found, &error)) {
      Fail("FollowReference after GetRaw failed: " + error);
    }
    if (!followed_found) {
      Fail("FollowReference should resolve GetRaw reference");
    }
    grovedb::ElementItem followed_item;
    if (!grovedb::DecodeItemFromElementBytes(followed, &followed_item, &error)) {
      Fail("FollowReference decode after GetRaw failed: " + error);
    }
    if (followed_item.value != std::vector<uint8_t>({'v'})) {
      Fail("FollowReference after GetRaw should resolve to target item");
    }
    grovedb::OperationCost get_raw_cost;
    if (!db.GetRaw(
            {{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_raw, &get_raw_found, &get_raw_cost, &error)) {
      Fail("GetRaw with cost should succeed: " + error);
    }
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(GetRaw tx path) failed: " + error);
    }
    if (!db.GetRaw(
            {{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_raw, &get_raw_found, &tx, &error)) {
      Fail("GetRaw with tx should succeed: " + error);
    }
    if (!db.GetRaw({{'t', 'r', 'e', 'e'}},
                   {'r', 'e', 'f'},
                   &get_raw,
                   &get_raw_found,
                   &get_raw_cost,
                   &tx,
                   &error)) {
      Fail("GetRaw with cost+tx should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(GetRaw tx path) failed: " + error);
    }

    std::vector<uint8_t> get_raw_optional;
    bool get_raw_optional_found = true;
    if (!db.GetRawOptional(
            {{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_raw_optional, &get_raw_optional_found, &error)) {
      Fail("GetRawOptional existing key should succeed: " + error);
    }
    if (!get_raw_optional_found) {
      Fail("GetRawOptional should report found=true for existing key");
    }
    if (!grovedb::DecodeElementVariant(get_raw_optional, &variant, &error)) {
      Fail("GetRawOptional decode variant failed: " + error);
    }
    if (variant != 1) {
      Fail("GetRawOptional should return unresolved reference bytes");
    }
    grovedb::OperationCost get_raw_optional_cost;
    if (!db.GetRawOptional({{'t', 'r', 'e', 'e'}},
                           {'r', 'e', 'f'},
                           &get_raw_optional,
                           &get_raw_optional_found,
                           &get_raw_optional_cost,
                           &error)) {
      Fail("GetRawOptional with cost should succeed: " + error);
    }
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(GetRawOptional tx path) failed: " + error);
    }
    if (!db.GetRawOptional(
            {{'t', 'r', 'e', 'e'}}, {'r', 'e', 'f'}, &get_raw_optional, &get_raw_optional_found, &tx, &error)) {
      Fail("GetRawOptional with tx should succeed: " + error);
    }
    if (!db.GetRawOptional({{'t', 'r', 'e', 'e'}},
                           {'r', 'e', 'f'},
                           &get_raw_optional,
                           &get_raw_optional_found,
                           &get_raw_optional_cost,
                           &tx,
                           &error)) {
      Fail("GetRawOptional with cost+tx should succeed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(GetRawOptional tx path) failed: " + error);
    }
    if (!db.GetRawOptional({{'t', 'r', 'e', 'e'}, {'m', 'i', 's', 's'}},
                           {'k'},
                           &get_raw_optional,
                           &get_raw_optional_found,
                           &error)) {
      Fail("GetRawOptional missing path should succeed: " + error);
    }
    if (get_raw_optional_found || !get_raw_optional.empty()) {
      Fail("GetRawOptional missing path should return found=false with empty output");
    }

    if (!db.ClearSubtree({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, &error)) {
      Fail("ClearSubtree positive path failed: " + error);
    }
    std::vector<uint8_t> clr_raw;
    bool clr_found = false;
    if (!db.Get({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'a'}, &clr_raw, &clr_found, &error)) {
      Fail("Get after ClearSubtree key a failed: " + error);
    }
    if (clr_found) {
      Fail("ClearSubtree should remove key a");
    }
    if (!db.Get({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'b'}, &clr_raw, &clr_found, &error)) {
      Fail("Get after ClearSubtree key b failed: " + error);
    }
    if (clr_found) {
      Fail("ClearSubtree should remove key b");
    }
    if (!db.Get({{'t', 'r', 'e', 'e'}}, {'c', 'l', 'r'}, &clr_raw, &clr_found, &error)) {
      Fail("Get after ClearSubtree subtree root failed: " + error);
    }
    if (!clr_found) {
      Fail("ClearSubtree should keep the subtree element itself");
    }

    if (!db.InsertEmptyTree(
            {{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'n', 'e', 's', 't', 'e', 'd'}, &error)) {
      Fail("InsertEmptyTree nested subtree for ClearSubtree failed: " + error);
    }
    if (db.ClearSubtree({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, &error)) {
      Fail("ClearSubtree should reject clearing subtree containing nested subtrees");
    }
    ExpectErrorContains("ClearSubtree nested subtree rejection",
                        error,
                        "options do not allow to clear this merk tree as it contains subtrees");

    bool deleted_nested = false;
    if (!db.DeleteIfEmptyTree(
            {{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}},
            {'n', 'e', 's', 't', 'e', 'd'},
            &deleted_nested,
            &error)) {
      Fail("DeleteIfEmptyTree nested subtree cleanup failed: " + error);
    }
    if (!deleted_nested) {
      Fail("DeleteIfEmptyTree nested subtree cleanup should delete empty subtree");
    }

    if (!db.InsertItem({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, {'c'}, {'3'}, &error)) {
      Fail("InsertItem ClearSubtree cost key failed: " + error);
    }
    grovedb::OperationCost clear_cost;
    if (!db.ClearSubtree({{'t', 'r', 'e', 'e'}, {'c', 'l', 'r'}}, &clear_cost, &error)) {
      Fail("ClearSubtree with cost should succeed: " + error);
    }
    if (clear_cost.seek_count == 0 && clear_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
      Fail("ClearSubtree cost should include seeks or removed bytes");
    }

    if (!db.InsertEmptyTree({{'t', 'r', 'e', 'e'}}, {'e', 'm', 'p'}, &error)) {
      Fail("InsertEmptyTree for ClearSubtree tx path failed: " + error);
    }
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction(ClearSubtree tx path) failed: " + error);
    }
    if (!db.InsertItem(
            {{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, {'t', 'x', 'k'}, {'3'}, &tx, &error)) {
      Fail("InsertItem for ClearSubtree tx path failed: " + error);
    }
    grovedb::OperationCost clear_tx_cost;
    if (!db.ClearSubtree({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}}, &clear_tx_cost, &tx, &error)) {
      Fail("ClearSubtree with cost in tx should succeed: " + error);
    }
    if (clear_tx_cost.seek_count == 0 &&
        clear_tx_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
      Fail("ClearSubtree with cost in tx should include seeks or removed bytes");
    }
    if (!db.Get({{'t', 'r', 'e', 'e'}, {'e', 'm', 'p'}},
                {'t', 'x', 'k'},
                &clr_raw,
                &clr_found,
                &tx,
                &error)) {
      Fail("Get after ClearSubtree in tx failed: " + error);
    }
    if (clr_found) {
      Fail("ClearSubtree in tx should remove tx-local key");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(ClearSubtree tx path) failed: " + error);
    }
  }

  // ==================== DeleteSubtree API Contract Tests ====================
  {
    // Test DeleteSubtree lifecycle: unopened DB rejection
    grovedb::GroveDb unopened;
    std::string error;
    if (unopened.DeleteSubtree({}, {}, &error)) {
      Fail("DeleteSubtree should fail on unopened DB");
    }
    ExpectErrorContains("DeleteSubtree unopened", error, "not opened");
  }

  {
    // Test DeleteSubtree lifecycle: inactive tx rejection
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction failed: " + error);
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction failed: " + error);
    }
    // After rollback, the tx is transparently reactivated, so the error
    // is about the path not existing rather than tx state.
    if (db.DeleteSubtree({{'t'}}, {'k'}, &tx, &error)) {
      Fail("DeleteSubtree should fail for non-existent path");
    }
    ExpectErrorContains("DeleteSubtree rolled-back tx reactivated", error, "path not found");
  }

  {
    // Test DeleteSubtree lifecycle: committed tx rejection
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &tx, &error)) {
      Fail("InsertEmptyTree root t in tx failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'k'}, &tx, &error)) {
      Fail("InsertEmptyTree in tx failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction failed: " + error);
    }
    if (db.DeleteSubtree({{'t'}}, {'k'}, &tx, &error)) {
      Fail("DeleteSubtree should fail for committed tx");
    }
    ExpectErrorContainsAny("DeleteSubtree committed tx", error, {"committed", "not active"});
  }

  {
    // Test DeleteSubtree: empty path rejection (cannot delete at root)
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'r'}, &error)) {
      Fail("InsertEmptyTree at root failed: " + error);
    }
    if (db.DeleteSubtree({}, {'r'}, &error)) {
      Fail("DeleteSubtree should reject empty path");
    }
    ExpectErrorContains("DeleteSubtree empty path", error, "root path");
  }

  {
    // Test DeleteSubtree: non-tree element rejection
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertItem({{'t'}}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem failed: " + error);
    }
    if (db.DeleteSubtree({{'t'}}, {'k'}, &error)) {
      Fail("DeleteSubtree should reject non-tree element");
    }
    ExpectErrorContains("DeleteSubtree non-tree", error, "not a tree");
  }

  {
    // Test DeleteSubtree: missing parent path
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (db.DeleteSubtree({{'m'}}, {'k'}, &error)) {
      Fail("DeleteSubtree should fail for missing parent path");
    }
  }

  {
    // Test DeleteSubtree: positive path - delete empty subtree
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'e'}, &error)) {
      Fail("InsertEmptyTree for DeleteSubtree test failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> element;
    if (!db.Get({{'t'}}, {'e'}, &element, &found, &error)) {
      Fail("Get before DeleteSubtree failed: " + error);
    }
    if (!found) {
      Fail("subtree should exist before DeleteSubtree");
    }
    if (!db.DeleteSubtree({{'t'}}, {'e'}, &error)) {
      Fail("DeleteSubtree positive path failed: " + error);
    }
    if (!db.Get({{'t'}}, {'e'}, &element, &found, &error)) {
      Fail("Get after DeleteSubtree failed: " + error);
    }
    if (found) {
      Fail("subtree should be deleted");
    }
  }

  {
    // Test DeleteSubtree: delete subtree with elements
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'d'}, &error)) {
      Fail("InsertEmptyTree for DeleteSubtree with elements failed: " + error);
    }
    if (!db.InsertItem({{'t'}, {'d'}}, {'1'}, {'v'}, &error)) {
      Fail("InsertItem 1 failed: " + error);
    }
    if (!db.InsertItem({{'t'}, {'d'}}, {'2'}, {'v'}, &error)) {
      Fail("InsertItem 2 failed: " + error);
    }
    if (!db.DeleteSubtree({{'t'}}, {'d'}, &error)) {
      Fail("DeleteSubtree with elements failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> element;
    if (!db.Get({{'t'}}, {'d'}, &element, &found, &error)) {
      Fail("Get after DeleteSubtree failed: " + error);
    }
    if (found) {
      Fail("subtree element should be deleted");
    }
  }

  {
    // Test DeleteSubtree: nested subtrees
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'p'}, &error)) {
      Fail("InsertEmptyTree parent failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}, {'p'}}, {'c'}, &error)) {
      Fail("InsertEmptyTree child failed: " + error);
    }
    if (!db.InsertItem({{'t'}, {'p'}, {'c'}}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem under child failed: " + error);
    }
    if (!db.DeleteSubtree({{'t'}}, {'p'}, &error)) {
      Fail("DeleteSubtree with nested subtrees failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> element;
    if (!db.Get({{'t'}}, {'p'}, &element, &found, &error)) {
      Fail("Get parent after DeleteSubtree failed: " + error);
    }
    if (found) {
      Fail("parent subtree should be deleted");
    }
  }

  {
    // Test DeleteSubtree: with cost
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'c'}, &error)) {
      Fail("InsertEmptyTree for DeleteSubtree cost test failed: " + error);
    }
    if (!db.InsertItem({{'t'}, {'c'}}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for DeleteSubtree cost test failed: " + error);
    }
    grovedb::OperationCost cost;
    if (!db.DeleteSubtree({{'t'}}, {'c'}, &cost, &error)) {
      Fail("DeleteSubtree with cost failed: " + error);
    }
    if (cost.IsNothing()) {
      Fail("DeleteSubtree should report non-empty cost");
    }
  }

  {
    // Test DeleteSubtree: tx-local visibility
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'e'}, &error)) {
      Fail("InsertEmptyTree for DeleteSubtree tx test failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction failed: " + error);
    }
    if (!db.DeleteSubtree({{'t'}}, {'e'}, &tx, &error)) {
      Fail("DeleteSubtree in tx failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> element;
    if (!db.Get({{'t'}}, {'e'}, &element, &found, &tx, &error)) {
      Fail("Get in tx after DeleteSubtree failed: " + error);
    }
    if (found) {
      Fail("subtree should be deleted in tx");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction failed: " + error);
    }
    if (!db.Get({{'t'}}, {'e'}, &element, &found, &error)) {
      Fail("Get after rollback failed: " + error);
    }
    if (!found) {
      Fail("subtree should exist after rollback");
    }
  }

  {
    // Test DeleteSubtree: tx with cost
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(ds_dir, &error)) {
      Fail("Open failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree root t failed: " + error);
    }
    if (!db.InsertEmptyTree({{'t'}}, {'e'}, &error)) {
      Fail("InsertEmptyTree for DeleteSubtree tx cost test failed: " + error);
    }
    if (!db.InsertItem({{'t'}, {'e'}}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for DeleteSubtree tx cost test failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction failed: " + error);
    }
    grovedb::OperationCost tx_cost;
    if (!db.DeleteSubtree({{'t'}}, {'e'}, &tx_cost, &tx, &error)) {
      Fail("DeleteSubtree with cost in tx failed: " + error);
    }
    if (tx_cost.IsNothing()) {
      Fail("DeleteSubtree with cost in tx should include seeks or removed bytes");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction(DeleteSubtree tx cost path) failed: " + error);
    }
  }

  {
    // Regression test: nested DeleteSubtree should not resurrect stale data
    auto ds_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ds_dir = MakeTempDir("facade_contract_" + std::to_string(ds_now));
    grovedb::GroveDb db2;
    std::string error;
    if (!db2.Open(ds_dir, &error)) {
      Fail("Open for nested DeleteSubtree regression failed: " + error);
    }
    // Create root/parent/child with an item inside child
    if (!db2.InsertEmptyTree({}, {'r'}, &error)) {
      Fail("InsertEmptyTree root r failed: " + error);
    }
    if (!db2.InsertEmptyTree({{'r'}}, {'p'}, &error)) {
      Fail("InsertEmptyTree parent failed: " + error);
    }
    if (!db2.InsertEmptyTree({{'r'}, {'p'}}, {'c'}, &error)) {
      Fail("InsertEmptyTree child failed: " + error);
    }
    if (!db2.InsertItem({{'r'}, {'p'}, {'c'}}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem under child failed: " + error);
    }
    // Delete parent (recursively deletes child)
    if (!db2.DeleteSubtree({{'r'}}, {'p'}, &error)) {
      Fail("DeleteSubtree parent failed: " + error);
    }
    // Reinsert a tree at the same path
    if (!db2.InsertEmptyTree({{'r'}}, {'p'}, &error)) {
      Fail("Re-InsertEmptyTree parent failed: " + error);
    }
    // Verify no stale nested data is visible
    bool found = false;
    std::vector<uint8_t> element;
    if (!db2.Get({{'r'}, {'p'}}, {'c'}, &element, &found, &error)) {
      Fail("Get child after reinsert failed: " + error);
    }
    if (found) {
      Fail("Stale child data resurrected after DeleteSubtree + reinsert");
    }
  }

  // ==================== End DeleteSubtree API Contract Tests ====================

  {
    // CountTree parent should support root-key propagation on nested writes.
    std::vector<uint8_t> flagged_count_tree = {0x06, 0x00, 0x00, 0x01, 0x02, 'c', 'f'};
    if (!db.Insert({}, {'c', 'n', 't', 'p'}, flagged_count_tree, &error)) {
      Fail("Insert count tree parent failed: " + error);
    }
    if (!db.Insert({{'c', 'n', 't', 'p'}}, {'c', '1'}, item_element, &error)) {
      Fail("Insert under count tree parent failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> got;
    if (!db.Get({}, {'c', 'n', 't', 'p'}, &got, &found, &error)) {
      Fail("Get count tree parent failed: " + error);
    }
    if (!found) {
      Fail("count tree parent should exist");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant(count tree parent) failed: " + error);
    }
    if (variant != 6) {
      Fail("count tree parent variant mismatch");
    }
    std::vector<uint8_t> flags;
    if (!grovedb::ExtractFlagsFromElementBytes(got, &flags, &error)) {
      Fail("ExtractFlagsFromElementBytes(count tree parent) failed: " + error);
    }
    if (flags != std::vector<uint8_t>({'c', 'f'})) {
      Fail("count tree parent flags should be preserved");
    }
  }

  {
    struct TreeRewriteCase {
      const char* name;
      std::vector<uint8_t> parent_key;
      std::vector<uint8_t> element_bytes;
      uint64_t expected_variant;
      bool has_count;
      uint64_t expected_count;
      bool has_sum;
      int64_t expected_sum;
      std::vector<uint8_t> expected_flags;
    };
    const std::vector<TreeRewriteCase> cases = {
        {"Tree",
         {'t', 'r', 'e', 'e'},
         {0x02, 0x00, 0x01, 0x02, 't', 'f'},
         2,
         false,
         0,
         false,
         0,
         {'t', 'f'}},
        {"BigSumTree",
         {'b', 's', 'u', 'm'},
         {0x05, 0x00, 0x06, 0x01, 0x02, 'b', 'f'},
         5,
         false,
         0,
         true,
         0,  // recomputed from child Item (contributes 0 to sum)
         {'b', 'f'}},
        {"CountSumTree",
         {'c', 's', 'u', 'm'},
         {0x07, 0x00, 0x09, 0x07, 0x01, 0x02, 'c', 's'},
         7,
         true,
         1,  // recomputed: 1 child node in merk
         true,
         0,  // recomputed from child Item (contributes 0 to sum)
         {'c', 's'}},
        {"ProvableCountTree",
         {'p', 'c', 'n', 't'},
         {0x08, 0x00, 0x0b, 0x01, 0x02, 'p', 'c'},
         8,
         true,
         1,  // recomputed: 1 child node in merk
         false,
         0,
         {'p', 'c'}},
        {"ProvableCountSumTree",
         {'p', 'c', 's', 'm'},
         {0x0a, 0x00, 0x0d, 0x08, 0x01, 0x02, 'p', 's'},
         10,
         true,
         1,  // recomputed: 1 child node in merk
         true,
         0,  // recomputed from child Item (contributes 0 to sum)
         {'p', 's'}},
    };

    for (size_t i = 0; i < cases.size(); ++i) {
      const auto& test_case = cases[i];
      if (!db.Insert({}, test_case.parent_key, test_case.element_bytes, &error)) {
        Fail(std::string("Insert ") + test_case.name + " parent failed: " + error);
      }
      std::vector<uint8_t> child_key = {'c', static_cast<uint8_t>('0' + i)};
      if (!db.Insert({test_case.parent_key}, child_key, item_element, &error)) {
        Fail(std::string("Insert under ") + test_case.name + " parent failed: " + error);
      }

      bool found = false;
      std::vector<uint8_t> got;
      if (!db.Get({}, test_case.parent_key, &got, &found, &error)) {
        Fail(std::string("Get ") + test_case.name + " parent failed: " + error);
      }
      if (!found) {
        Fail(std::string(test_case.name) + " parent should exist");
      }

      uint64_t variant = 0;
      if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
        Fail(std::string("DecodeElementVariant(") + test_case.name + ") failed: " + error);
      }
      if (variant != test_case.expected_variant) {
        Fail(std::string(test_case.name) + " parent variant mismatch");
      }

      DecodedTreeElementState decoded;
      if (!DecodeTreeElementState(got, &decoded, &error)) {
        Fail(std::string("DecodeTreeElementState(") + test_case.name + ") failed: " + error);
      }
      if (!decoded.has_root_key) {
        Fail(std::string(test_case.name) + " parent should include propagated root key");
      }
      if (decoded.flags != test_case.expected_flags) {
        Fail(std::string(test_case.name) + " parent flags should be preserved");
      }
      if (decoded.has_count != test_case.has_count) {
        Fail(std::string(test_case.name) + " count presence mismatch");
      }
      if (decoded.has_count && decoded.count != test_case.expected_count) {
        Fail(std::string(test_case.name) + " parent count should match recomputed value");
      }
      if (decoded.has_sum != test_case.has_sum) {
        Fail(std::string(test_case.name) + " sum presence mismatch");
      }
      if (decoded.has_sum && decoded.sum != test_case.expected_sum) {
        Fail(std::string(test_case.name) + " parent sum should match recomputed value");
      }
    }
  }

  {
    // Post-commit tx handle should reject operations; post-rollback handle should
    // still support Rust-parity read/write lifecycle on reuse.
    if (!db.Insert({}, {'l', 'i', 'f', 'e'}, tree_element, &error)) {
      Fail("Insert lifecycle root tree failed: " + error);
    }
    if (!db.Insert({{'l', 'i', 'f', 'e'}}, {'k'}, item_element, &error)) {
      Fail("Insert lifecycle base key failed: " + error);
    }

    grovedb::GroveDb::Transaction tx_committed;
    if (!db.StartTransaction(&tx_committed, &error)) {
      Fail("StartTransaction(tx_committed) failed: " + error);
    }
    if (!db.CommitTransaction(&tx_committed, &error)) {
      Fail("CommitTransaction(tx_committed) failed: " + error);
    }

    if (db.Insert({{'l', 'i', 'f', 'e'}}, {'c'}, item_element, &tx_committed, &error)) {
      Fail("Insert should fail on committed tx handle");
    }
    ExpectErrorContainsAny("Insert committed tx", error, {"committed", "not active"});

    bool deleted = false;
    if (db.Delete({{'l', 'i', 'f', 'e'}}, {'k'}, &deleted, &tx_committed, &error)) {
      Fail("Delete should fail on committed tx handle");
    }
    ExpectErrorContainsAny("Delete committed tx", error, {"committed", "not active"});

    bool found = false;
    std::vector<uint8_t> got;
    if (db.Get({{'l', 'i', 'f', 'e'}}, {'k'}, &got, &found, &tx_committed, &error)) {
      Fail("Get should fail on committed tx handle");
    }
    ExpectErrorContainsAny("Get committed tx", error, {"committed", "not active"});
    if (db.FollowReference({{'l', 'i', 'f', 'e'}},
                           {'k'},
                           &got,
                           &found,
                           &tx_committed,
                           &error)) {
      Fail("FollowReference should fail on committed tx handle");
    }
    ExpectErrorContainsAny("FollowReference committed tx", error, {"committed", "not active"});

    grovedb::GroveDb::Transaction tx_rolled;
    if (!db.StartTransaction(&tx_rolled, &error)) {
      Fail("StartTransaction(tx_rolled) failed: " + error);
    }
    if (!db.Insert({{'l', 'i', 'f', 'e'}}, {'t', 'm', 'p'}, item_element, &tx_rolled, &error)) {
      Fail("Insert before rollback failed: " + error);
    }
    if (!db.RollbackTransaction(&tx_rolled, &error)) {
      Fail("RollbackTransaction(tx_rolled) failed: " + error);
    }

    if (!db.Get({{'l', 'i', 'f', 'e'}}, {'k'}, &got, &found, &tx_rolled, &error)) {
      Fail("Get should work on rolled-back tx handle");
    }
    if (!found || got != item_element) {
      Fail("Get on rolled-back tx handle returned unexpected value");
    }

    if (!db.Insert({{'l', 'i', 'f', 'e'}}, {'n', 'e', 'w'}, item_element, &tx_rolled, &error)) {
      Fail("Insert should work on rolled-back tx handle");
    }
    if (!db.Delete({{'l', 'i', 'f', 'e'}}, {'k'}, &deleted, &tx_rolled, &error)) {
      Fail("Delete should work on rolled-back tx handle");
    }
    if (!deleted) {
      Fail("Delete on rolled-back tx handle should report deleted=true");
    }
    if (!db.CommitTransaction(&tx_rolled, &error)) {
      Fail("Commit on reused rolled-back tx handle failed: " + error);
    }

    if (!db.Get({{'l', 'i', 'f', 'e'}}, {'n', 'e', 'w'}, &got, &found, &error)) {
      Fail("Get lifecycle new key after commit failed: " + error);
    }
    if (!found || got != item_element) {
      Fail("Lifecycle new key mismatch after commit");
    }
    if (!db.Get({{'l', 'i', 'f', 'e'}}, {'k'}, &got, &found, &error)) {
      Fail("Get lifecycle deleted key after commit failed: " + error);
    }
    if (found) {
      Fail("Lifecycle deleted key should be absent after commit");
    }
  }

  {
    auto cp_now2 = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string checkpoint_dir = MakeTempDir("facade_contract_" + std::to_string(cp_now2));
    std::filesystem::remove_all(checkpoint_dir);
    if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
      Fail("CreateCheckpoint via GroveDb failed: " + error);
    }
    {
      grovedb::GroveDb checkpoint_db;
      if (!checkpoint_db.OpenCheckpoint(checkpoint_dir, &error)) {
        Fail("Open checkpoint DB via GroveDb OpenCheckpoint failed: " + error);
      }
      bool found = false;
      std::vector<uint8_t> got;
      if (!checkpoint_db.Get({{'t', 'r', 'e', 'e'}}, {'k'}, &got, &found, &error)) {
        Fail("Get from checkpoint DB failed: " + error);
      }
      if (!found || got != item_element) {
        Fail("Checkpoint DB content mismatch");
      }
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_dir, &error)) {
      Fail("Delete checkpoint DB via GroveDb failed: " + error);
    }
  }

  {
    auto ncp_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string non_checkpoint_dir = MakeTempDir("facade_contract_" + std::to_string(ncp_now));
    if (grovedb::GroveDb::DeleteCheckpoint(non_checkpoint_dir, &error)) {
      Fail("DeleteCheckpoint should fail for non-checkpoint directory");
    }
    ExpectErrorContains("DeleteCheckpoint non-checkpoint", error, "not a valid checkpoint");
    std::filesystem::remove_all(non_checkpoint_dir);
  }

  {
    // Checkpoint should preserve committed transactional mutations.
    if (!db.Insert({}, {'t', 'x', 'c', 'p'}, tree_element, &error)) {
      Fail("Insert tx checkpoint tree failed: " + error);
    }
    grovedb::GroveDb::Transaction tx_cp;
    if (!db.StartTransaction(&tx_cp, &error)) {
      Fail("StartTransaction(tx_cp) failed: " + error);
    }
    if (!db.Insert({{'t', 'x', 'c', 'p'}}, {'k', 'c', 'p'}, item_element, &tx_cp, &error)) {
      Fail("Insert tx checkpoint key in tx failed: " + error);
    }
    if (!db.CommitTransaction(&tx_cp, &error)) {
      Fail("CommitTransaction(tx_cp) failed: " + error);
    }

    auto ctx_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string checkpoint_tx_dir = MakeTempDir("facade_contract_" + std::to_string(ctx_now));
    std::filesystem::remove_all(checkpoint_tx_dir);
    if (!db.CreateCheckpoint(checkpoint_tx_dir, &error)) {
      Fail("CreateCheckpoint after tx commit failed: " + error);
    }
    {
      grovedb::GroveDb checkpoint_tx_db;
      if (!checkpoint_tx_db.OpenCheckpoint(checkpoint_tx_dir, &error)) {
        Fail("Open tx checkpoint DB via OpenCheckpoint failed: " + error);
      }
      bool found = false;
      std::vector<uint8_t> got;
      if (!checkpoint_tx_db.Get({{'t', 'x', 'c', 'p'}}, {'k', 'c', 'p'}, &got, &found, &error)) {
        Fail("Get from tx checkpoint DB failed: " + error);
      }
      if (!found || got != item_element) {
        Fail("Tx checkpoint DB content mismatch");
      }
    }
    if (!grovedb::GroveDb::DeleteCheckpoint(checkpoint_tx_dir, &error)) {
      Fail("Delete tx checkpoint DB via GroveDb failed: " + error);
    }
  }

  {
    if (!db.Wipe(&error)) {
      Fail("Wipe failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> got;
    if (db.Get({{'t', 'r', 'e', 'e'}}, {'k'}, &got, &found, &error)) {
      Fail("Get after wipe should fail because subtree path is removed");
    }
    ExpectErrorContains("Get after wipe", error, "path not found");
  }

  {
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
    if (db.QueryRange({}, {'z'}, {'a'}, true, true, &out, &error)) {
      Fail("QueryRange should fail when end precedes start");
    }
    ExpectErrorContains("QueryRange end<start", error, "precedes start");
  }

  // ---- PutAux / GetAux / DeleteAux contract tests ----
  {
    // PutAux + GetAux roundtrip
    std::vector<uint8_t> aux_key = {'a', 'k'};
    std::vector<uint8_t> aux_val = {'a', 'v'};
    if (!db.PutAux(aux_key, aux_val, &error)) {
      Fail("PutAux should succeed: " + error);
    }
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.GetAux(aux_key, &got, &found, &error)) {
      Fail("GetAux should succeed: " + error);
    }
    if (!found || got != aux_val) {
      Fail("GetAux should return stored value");
    }

    // GetAux missing key
    std::vector<uint8_t> missing_key = {'n', 'o'};
    found = true;
    if (!db.GetAux(missing_key, &got, &found, &error)) {
      Fail("GetAux missing key should succeed: " + error);
    }
    if (found) {
      Fail("GetAux missing key should not be found");
    }

    // DeleteAux
    bool deleted = false;
    if (!db.DeleteAux(aux_key, &deleted, &error)) {
      Fail("DeleteAux should succeed: " + error);
    }
    if (!deleted) {
      Fail("DeleteAux should report deleted");
    }

    // GetAux after delete
    found = true;
    if (!db.GetAux(aux_key, &got, &found, &error)) {
      Fail("GetAux after delete should succeed: " + error);
    }
    if (found) {
      Fail("GetAux after delete should not be found");
    }

    // PutAux with empty key should fail
    if (db.PutAux({}, aux_val, &error)) {
      Fail("PutAux with empty key should fail");
    }

    // GetAux with empty key should fail
    if (db.GetAux({}, &got, &found, &error)) {
      Fail("GetAux with empty key should fail");
    }

    // DeleteAux with empty key should fail
    if (db.DeleteAux({}, &deleted, &error)) {
      Fail("DeleteAux with empty key should fail");
    }

    // GetAux with null outputs should fail
    if (db.GetAux(aux_key, nullptr, &found, &error)) {
      Fail("GetAux with null value should fail");
    }
    if (db.GetAux(aux_key, &got, nullptr, &error)) {
      Fail("GetAux with null found should fail");
    }

    // DeleteAux with null output should fail
    if (db.DeleteAux(aux_key, nullptr, &error)) {
      Fail("DeleteAux with null deleted should fail");
    }
  }

  // PutAux / GetAux with transaction
  {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for aux: " + error);
    }
    std::vector<uint8_t> tk = {'t', 'k'};
    std::vector<uint8_t> tv = {'t', 'v'};
    if (!db.PutAux(tk, tv, &tx, &error)) {
      Fail("PutAux in tx should succeed: " + error);
    }
    // Visible in tx
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.GetAux(tk, &got, &found, &tx, &error)) {
      Fail("GetAux in tx should succeed: " + error);
    }
    if (!found || got != tv) {
      Fail("GetAux in tx should see uncommitted value");
    }
    // Not visible outside tx
    found = true;
    if (!db.GetAux(tk, &got, &found, &error)) {
      Fail("GetAux outside tx should succeed: " + error);
    }
    if (found) {
      Fail("GetAux outside tx should not see uncommitted value");
    }
    // Commit and verify visibility
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit tx for aux: " + error);
    }
    found = false;
    if (!db.GetAux(tk, &got, &found, &error)) {
      Fail("GetAux after commit should succeed: " + error);
    }
    if (!found || got != tv) {
      Fail("GetAux after commit should see value");
    }
  }

  // HasAux contract tests
  {
    std::vector<uint8_t> aux_key = {'a', 'k'};
    std::vector<uint8_t> aux_val = {'a', 'v'};
    // First put the key so we can test HasAux on existing key
    if (!db.PutAux(aux_key, aux_val, &error)) {
      Fail("PutAux for HasAux test should succeed: " + error);
    }

    // HasAux on existing key
    bool exists = false;
    if (!db.HasAux(aux_key, &exists, &error)) {
      Fail("HasAux existing key should succeed: " + error);
    }
    if (!exists) {
      Fail("HasAux existing key should return exists=true");
    }

    // HasAux on missing key
    std::vector<uint8_t> missing_key = {'m', 'i', 's', 's', 'i', 'n', 'g'};
    exists = true;
    if (!db.HasAux(missing_key, &exists, &error)) {
      Fail("HasAux missing key should succeed: " + error);
    }
    if (exists) {
      Fail("HasAux missing key should return exists=false");
    }

    // HasAux with empty key should fail
    if (db.HasAux({}, &exists, &error)) {
      Fail("HasAux with empty key should fail");
    }

    // HasAux with null output should fail
    if (db.HasAux(aux_key, nullptr, &error)) {
      Fail("HasAux with null exists should fail");
    }
  }

  // HasAux with transaction
  {
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("start tx for has_aux: " + error);
    }
    std::vector<uint8_t> hk = {'h', 'k'};
    std::vector<uint8_t> hv = {'h', 'v'};

    // HasAux before PutAux in tx
    bool exists = true;
    if (!db.HasAux(hk, &exists, &tx, &error)) {
      Fail("HasAux in tx should succeed: " + error);
    }
    if (exists) {
      Fail("HasAux in tx should return exists=false before PutAux");
    }

    // PutAux in tx
    if (!db.PutAux(hk, hv, &tx, &error)) {
      Fail("PutAux in tx should succeed: " + error);
    }

    // HasAux after PutAux in tx (tx-local visibility)
    exists = false;
    if (!db.HasAux(hk, &exists, &tx, &error)) {
      Fail("HasAux in tx after PutAux should succeed: " + error);
    }
    if (!exists) {
      Fail("HasAux in tx should return exists=true after PutAux");
    }

    // HasAux outside tx should not see uncommitted value
    exists = true;
    if (!db.HasAux(hk, &exists, &error)) {
      Fail("HasAux outside tx should succeed: " + error);
    }
    if (exists) {
      Fail("HasAux outside tx should return exists=false for uncommitted PutAux");
    }

    // Commit and verify
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("commit tx for has_aux: " + error);
    }
    exists = false;
    if (!db.HasAux(hk, &exists, &error)) {
      Fail("HasAux after commit should succeed: " + error);
    }
    if (!exists) {
      Fail("HasAux after commit should return exists=true");
    }
  }

  // HasAux lifecycle: unopened, inactive tx, committed tx
  {
    // Unopened DB
    grovedb::GroveDb unopened_db;
    std::vector<uint8_t> key = {'k'};
    bool exists = true;
    if (unopened_db.HasAux(key, &exists, &error)) {
      Fail("HasAux on unopened DB should fail");
    }
    ExpectErrorContains("HasAux unopened", error, "not opened");

    // Inactive tx (uninitialized)
    grovedb::GroveDb::Transaction inactive_tx;
    exists = true;
    if (db.HasAux(key, &exists, &inactive_tx, &error)) {
      Fail("HasAux with inactive tx should fail");
    }
    ExpectErrorContains("HasAux inactive tx", error, "not active");

    // Committed tx
    grovedb::GroveDb::Transaction committed_tx;
    if (!db.StartTransaction(&committed_tx, &error)) {
      Fail("start tx for committed test: " + error);
    }
    if (!db.CommitTransaction(&committed_tx, &error)) {
      Fail("commit tx for committed test: " + error);
    }
    exists = true;
    if (db.HasAux(key, &exists, &committed_tx, &error)) {
      Fail("HasAux with committed tx should fail");
    }
    ExpectErrorContains("HasAux committed tx", error, "not active");
  }

  std::filesystem::remove_all(dir);

  // ---- Batch kRefreshReference contract tests ----
  {
    auto rr_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string rr_dir = MakeTempDir("facade_contract_" + std::to_string(rr_now));
    grovedb::GroveDb db;
    if (!db.Open(rr_dir, &error)) {
      Fail("open for batch refresh ref contract: " + error);
    }
    const std::vector<uint8_t> rk = {'r', 'o', 'o', 't'};
    const std::vector<std::vector<uint8_t>> rp = {rk};
    std::vector<uint8_t> tree_bytes;
    if (!grovedb::EncodeTreeToElementBytes(&tree_bytes, &error)) {
      Fail("encode tree bytes for batch refresh ref contract: " + error);
    }
    if (!db.Insert({}, rk, tree_bytes, &error)) {
      Fail("insert root tree for batch refresh ref contract: " + error);
    }

    // Insert an Item and a Reference.
    std::vector<uint8_t> item_bytes;
    if (!grovedb::EncodeItemToElementBytes({'v'}, &item_bytes, &error)) {
      Fail("encode item bytes for batch refresh ref contract: " + error);
    }
    if (!db.Insert(rp, {'k'}, item_bytes, &error)) {
      Fail("insert item for batch refresh ref contract: " + error);
    }

    grovedb::ElementReference abs_ref;
    abs_ref.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
    abs_ref.reference_path.path = {rk, {'k'}};
    std::vector<uint8_t> ref_bytes;
    if (!grovedb::EncodeReferenceToElementBytes(abs_ref, &ref_bytes, &error)) {
      Fail("encode ref bytes for batch refresh ref contract: " + error);
    }
    if (!db.Insert(rp, {'r'}, ref_bytes, &error)) {
      Fail("insert reference for batch refresh ref contract: " + error);
    }

    // Contract: RefreshReference on non-reference must fail with descriptive
    // error.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'k'};
      op.element_bytes = ref_bytes;
      error.clear();
      if (db.ApplyBatch({op}, &error)) {
        Fail("refresh ref on item should fail");
      }
      ExpectErrorContains("refresh_non_ref", error, "not a reference");
    }

    // Contract: RefreshReference on missing key must fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'x'};
      op.element_bytes = ref_bytes;
      error.clear();
      if (db.ApplyBatch({op}, &error)) {
        Fail("refresh ref on missing key should fail");
      }
      ExpectErrorContains("refresh_missing", error, "does not exist");
    }

    // Contract: trust_refresh_reference allows refresh on missing key.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'x', '2'};
      op.element_bytes = ref_bytes;
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.trust_refresh_reference = true;
      error.clear();
      if (!db.ApplyBatch({op}, opts, &error)) {
        Fail("trusted refresh ref on missing key should succeed: " + error);
      }
      std::vector<uint8_t> got;
      bool found = false;
      if (!db.GetRaw(rp, {'x', '2'}, &got, &found, &error)) {
        Fail("GetRaw trusted refresh missing-key contract failed: " + error);
      }
      if (!found || got != ref_bytes) {
        Fail("trusted refresh on missing key should persist provided reference");
      }
    }

    // Contract: RefreshReference with non-reference element_bytes must fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'r'};
      op.element_bytes = item_bytes;
      error.clear();
      if (db.ApplyBatch({op}, &error)) {
        Fail("refresh ref with item bytes should fail");
      }
      ExpectErrorContainsAny(
          "refresh_bad_bytes", error, {"must encode a reference", "not a reference"});
    }

    // Contract: RefreshReference on existing reference with valid ref bytes
    // succeeds.
    {
      grovedb::ElementReference new_ref;
      new_ref.reference_path.kind = grovedb::ReferencePathKind::kSibling;
      new_ref.reference_path.key = {'k'};
      new_ref.has_max_hop = true;
      new_ref.max_hop = 3;
      std::vector<uint8_t> new_ref_bytes;
      if (!grovedb::EncodeReferenceToElementBytes(new_ref, &new_ref_bytes, &error)) {
        Fail("encode new ref bytes failed: " + error);
      }

      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'r'};
      op.element_bytes = new_ref_bytes;
      error.clear();
      if (!db.ApplyBatch({op}, &error)) {
        Fail("refresh ref should succeed: " + error);
      }

      std::vector<uint8_t> got;
      bool found = false;
      if (!db.GetRaw(rp, {'r'}, &got, &found, &error)) {
        Fail("GetRaw refreshed reference failed: " + error);
      }
      if (!found || got != new_ref_bytes) {
        Fail("refresh ref did not update stored element");
      }
    }

    // Contract: RefreshReference rolls back on tx failure.
    {
      grovedb::GroveDb::Transaction tx;
      db.StartTransaction(&tx, &error);

      grovedb::ElementReference tx_ref;
      tx_ref.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
      tx_ref.reference_path.path = {rk, {'k'}};
      tx_ref.has_max_hop = true;
      tx_ref.max_hop = 10;
      std::vector<uint8_t> tx_ref_bytes;
      grovedb::EncodeReferenceToElementBytes(tx_ref, &tx_ref_bytes, &error);

      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = rp;
      op.key = {'r'};
      op.element_bytes = tx_ref_bytes;
      db.ApplyBatch({op}, &tx, &error);
      db.RollbackTransaction(&tx, &error);

      // After rollback, element should still be the sibling ref from previous
      // test.
      std::vector<uint8_t> got;
      bool found = false;
      db.GetRaw(rp, {'r'}, &got, &found, &error);
      grovedb::ElementReference decoded;
      grovedb::DecodeReferenceFromElementBytes(got, &decoded, &error);
      if (decoded.reference_path.kind != grovedb::ReferencePathKind::kSibling) {
        Fail("refresh ref should have been rolled back");
      }
    }

    std::filesystem::remove_all(rr_dir);
  }

  // ── RootKey contract tests ──
  {
    grovedb::GroveDb unopened_rk;
    std::vector<uint8_t> root_key;
    bool found = false;
    if (unopened_rk.RootKey(&root_key, &found, &error)) {
      Fail("RootKey should fail on unopened DB");
    }
    ExpectErrorContains("RootKey unopened", error, "not opened");

    auto rk_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto rk_dir = MakeTempDir("facade_contract_" + std::to_string(rk_now));
    grovedb::GroveDb rk_db;
    if (!rk_db.Open(rk_dir, &error)) {
      Fail("Open(RootKey) failed: " + error);
    }

    if (rk_db.RootKey(nullptr, &found, &error)) {
      Fail("RootKey should fail for null key output");
    }
    ExpectErrorContains("RootKey null key", error, "output is null");
    if (rk_db.RootKey(&root_key, nullptr, &error)) {
      Fail("RootKey should fail for null found output");
    }
    ExpectErrorContains("RootKey null found", error, "output is null");

    if (!rk_db.RootKey(&root_key, &found, &error)) {
      Fail("RootKey empty DB: " + error);
    }
    if (found) {
      Fail("RootKey should report found=false for empty DB");
    }
    if (!root_key.empty()) {
      Fail("RootKey output should be empty when found=false");
    }

    if (!rk_db.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for RootKey test: " + error);
    }
    std::vector<uint8_t> committed_root_key;
    if (!rk_db.RootKey(&committed_root_key, &found, &error)) {
      Fail("RootKey after insert: " + error);
    }
    if (!found || committed_root_key.empty()) {
      Fail("RootKey should return found=true with non-empty key after insert");
    }

    grovedb::GroveDb::Transaction tx_rk;
    if (!rk_db.StartTransaction(&tx_rk, &error)) {
      Fail("StartTransaction for RootKey: " + error);
    }
    if (!rk_db.InsertItem({}, {'k', '2'}, {'v', '2'}, &tx_rk, &error)) {
      Fail("InsertItem in tx for RootKey: " + error);
    }
    std::vector<uint8_t> in_tx_root_key;
    if (!rk_db.RootKey(&in_tx_root_key, &found, &tx_rk, &error)) {
      Fail("RootKey in tx: " + error);
    }
    if (!found || in_tx_root_key.empty()) {
      Fail("RootKey in tx should return found=true with non-empty key");
    }
    std::vector<uint8_t> no_tx_root_key;
    if (!rk_db.RootKey(&no_tx_root_key, &found, &error)) {
      Fail("RootKey outside tx: " + error);
    }
    if (!found || no_tx_root_key != committed_root_key) {
      Fail("RootKey outside tx should match committed state before tx commit");
    }
    if (!rk_db.CommitTransaction(&tx_rk, &error)) {
      Fail("CommitTransaction RootKey: " + error);
    }
    std::vector<uint8_t> post_commit_root_key;
    if (!rk_db.RootKey(&post_commit_root_key, &found, &error)) {
      Fail("RootKey after commit: " + error);
    }
    if (!found || post_commit_root_key != in_tx_root_key) {
      Fail("RootKey after commit should match tx RootKey");
    }

    if (rk_db.RootKey(&root_key, &found, &tx_rk, &error)) {
      Fail("RootKey should fail for committed tx");
    }
    ExpectErrorContainsAny("RootKey committed tx", error, {"committed", "not active"});

    // RootKey cost overload tests
    grovedb::OperationCost cost_plain;
    std::vector<uint8_t> cost_key;
    bool cost_found = false;
    if (!rk_db.RootKey(&cost_key, &cost_found, &cost_plain, &error)) {
      Fail("RootKey cost overload: " + error);
    }
    if (!cost_found || cost_key.empty()) {
      Fail("RootKey cost overload should return found=true with non-empty key");
    }
    // Cost should include seek for opening root merk
    if (cost_plain.seek_count == 0) {
      Fail("RootKey cost overload should accumulate seek cost");
    }

    grovedb::GroveDb::Transaction tx_cost;
    if (!rk_db.StartTransaction(&tx_cost, &error)) {
      Fail("StartTransaction for RootKey cost: " + error);
    }
    if (!rk_db.InsertItem({}, {'k', '3'}, {'v', '3'}, &tx_cost, &error)) {
      Fail("InsertItem in tx for RootKey cost: " + error);
    }
    grovedb::OperationCost cost_tx;
    std::vector<uint8_t> cost_tx_key;
    bool cost_tx_found = false;
    if (!rk_db.RootKey(&cost_tx_key, &cost_tx_found, &cost_tx, &tx_cost, &error)) {
      Fail("RootKey cost+tx overload: " + error);
    }
    if (!cost_tx_found || cost_tx_key.empty()) {
      Fail("RootKey cost+tx overload should return found=true with non-empty key");
    }
    if (cost_tx.seek_count == 0) {
      Fail("RootKey cost+tx overload should accumulate seek cost");
    }

    std::filesystem::remove_all(rk_dir);
  }

  // ── RootHash contract tests ──
  {
    // Unopened DB
    grovedb::GroveDb unopened_rh;
    std::vector<uint8_t> hash;
    if (unopened_rh.RootHash(&hash, &error)) {
      Fail("RootHash should fail on unopened DB");
    }
    ExpectErrorContains("RootHash unopened", error, "not opened");

    // Null output
    auto rh_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto rh_dir = MakeTempDir("facade_contract_" + std::to_string(rh_now));
    grovedb::GroveDb rh_db;
    if (!rh_db.Open(rh_dir, &error)) {
      Fail("Open(RootHash) failed: " + error);
    }
    if (rh_db.RootHash(nullptr, &error)) {
      Fail("RootHash should fail for null output");
    }
    ExpectErrorContains("RootHash null", error, "output is null");

    // Empty DB root hash should be 32 zero bytes
    if (!rh_db.RootHash(&hash, &error)) {
      Fail("RootHash empty DB: " + error);
    }
    if (hash.size() != 32) {
      Fail("RootHash should return 32 bytes, got " + std::to_string(hash.size()));
    }

    // After insert, root hash should change
    std::vector<uint8_t> empty_hash = hash;
    if (!rh_db.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem for RootHash test: " + error);
    }
    std::vector<uint8_t> hash_after;
    if (!rh_db.RootHash(&hash_after, &error)) {
      Fail("RootHash after insert: " + error);
    }
    if (hash_after == empty_hash) {
      Fail("RootHash should change after insert");
    }

    // RootHash with tx should reflect tx state
    grovedb::GroveDb::Transaction tx_rh;
    if (!rh_db.StartTransaction(&tx_rh, &error)) {
      Fail("StartTransaction for RootHash: " + error);
    }
    if (!rh_db.InsertItem({}, {'k', '2'}, {'v', '2'}, &tx_rh, &error)) {
      Fail("InsertItem in tx for RootHash: " + error);
    }
    std::vector<uint8_t> hash_in_tx;
    if (!rh_db.RootHash(&hash_in_tx, &tx_rh, &error)) {
      Fail("RootHash in tx: " + error);
    }
    // Root hash in tx should differ from committed state
    if (hash_in_tx == hash_after) {
      Fail("RootHash in tx should differ from pre-tx state");
    }
    // Root hash without tx should still be pre-tx
    std::vector<uint8_t> hash_no_tx;
    if (!rh_db.RootHash(&hash_no_tx, &error)) {
      Fail("RootHash outside tx: " + error);
    }
    if (hash_no_tx != hash_after) {
      Fail("RootHash outside tx should match pre-tx committed state");
    }
    if (!rh_db.CommitTransaction(&tx_rh, &error)) {
      Fail("CommitTransaction RootHash: " + error);
    }
    // After commit, hash should match the tx hash
    std::vector<uint8_t> hash_post_commit;
    if (!rh_db.RootHash(&hash_post_commit, &error)) {
      Fail("RootHash after commit: " + error);
    }
    if (hash_post_commit != hash_in_tx) {
      Fail("RootHash after commit should match in-tx hash");
    }

    // Committed tx rejection
    if (rh_db.RootHash(&hash, &tx_rh, &error)) {
      Fail("RootHash should fail for committed tx");
    }
    ExpectErrorContainsAny("RootHash committed tx", error, {"committed", "not active"});

    std::filesystem::remove_all(rh_dir);
  }

  // ── InsertBigSumTree positive path ──
  {
    auto bst_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto bst_dir = MakeTempDir("facade_contract_" + std::to_string(bst_now));
    grovedb::GroveDb db;
    if (!db.Open(bst_dir, &error)) {
      Fail("Open(InsertBigSumTree positive) failed: " + error);
    }
    if (!db.InsertBigSumTree({}, {'b', 's', 't'}, &error)) {
      Fail("InsertBigSumTree root-level: " + error);
    }
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'b', 's', 't'}, &got, &found, &error)) {
      Fail("Get InsertBigSumTree: " + error);
    }
    if (!found) {
      Fail("InsertBigSumTree should be found after insert");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertBigSumTree: " + error);
    }
    if (variant != 5) {
      Fail("Expected BigSumTree variant 5, got " + std::to_string(variant));
    }
    __int128 big_sum = 1;
    bool has_big_sum = false;
    if (!grovedb::ExtractBigSumValueFromElementBytes(got, &big_sum, &has_big_sum, &error)) {
      Fail("ExtractBigSumValueFromElementBytes InsertBigSumTree: " + error);
    }
    if (!has_big_sum || big_sum != 0) {
      Fail("InsertBigSumTree should decode to big sum = 0");
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertBigSumTree tx: " + error);
    }
    if (!db.InsertBigSumTree({}, {'b', '2'}, &tx, &error)) {
      Fail("InsertBigSumTree in tx: " + error);
    }
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'b', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertBigSumTree in tx: " + error);
    }
    if (!found) {
      Fail("InsertBigSumTree should be visible in tx");
    }
    if (!db.Get({}, {'b', '2'}, &got2, &found, &error)) {
      Fail("Get InsertBigSumTree outside tx: " + error);
    }
    if (found) {
      Fail("InsertBigSumTree should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction InsertBigSumTree: " + error);
    }
    if (!db.Get({}, {'b', '2'}, &got2, &found, &error)) {
      Fail("Get InsertBigSumTree after commit: " + error);
    }
    if (!found) {
      Fail("InsertBigSumTree should be visible after commit");
    }
    if (!grovedb::ExtractBigSumValueFromElementBytes(got2, &big_sum, &has_big_sum, &error)) {
      Fail("ExtractBigSumValueFromElementBytes tx InsertBigSumTree: " + error);
    }
    if (!has_big_sum || big_sum != 0) {
      Fail("tx InsertBigSumTree should decode to big sum = 0");
    }

    grovedb::OperationCost cost;
    if (!db.InsertBigSumTree({}, {'b', '3'}, &cost, &error)) {
      Fail("InsertBigSumTree with cost: " + error);
    }
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertBigSumTree cost should have storage writes");
    }

    std::filesystem::remove_all(bst_dir);
  }

  // ── InsertCountTree positive path ──
  {
    auto ct_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto ct_dir = MakeTempDir("facade_contract_" + std::to_string(ct_now));
    grovedb::GroveDb db;
    if (!db.Open(ct_dir, &error)) {
      Fail("Open(InsertCountTree positive) failed: " + error);
    }
    if (!db.InsertCountTree({}, {'c', 't'}, &error)) {
      Fail("InsertCountTree root-level: " + error);
    }
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'c', 't'}, &got, &found, &error)) {
      Fail("Get InsertCountTree: " + error);
    }
    if (!found) {
      Fail("InsertCountTree should be found after insert");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertCountTree: " + error);
    }
    if (variant != 6) {
      Fail("Expected CountTree variant 6, got " + std::to_string(variant));
    }
    uint64_t count = 1;
    bool has_count = false;
    if (!grovedb::ExtractCountValueFromElementBytes(got, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes InsertCountTree: " + error);
    }
    if (!has_count || count != 0) {
      Fail("InsertCountTree should decode to count = 0");
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertCountTree tx: " + error);
    }
    if (!db.InsertCountTree({}, {'c', '2'}, &tx, &error)) {
      Fail("InsertCountTree in tx: " + error);
    }
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'c', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertCountTree in tx: " + error);
    }
    if (!found) {
      Fail("InsertCountTree should be visible in tx");
    }
    if (!db.Get({}, {'c', '2'}, &got2, &found, &error)) {
      Fail("Get InsertCountTree outside tx: " + error);
    }
    if (found) {
      Fail("InsertCountTree should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction InsertCountTree: " + error);
    }
    if (!db.Get({}, {'c', '2'}, &got2, &found, &error)) {
      Fail("Get InsertCountTree after commit: " + error);
    }
    if (!found) {
      Fail("InsertCountTree should be visible after commit");
    }
    if (!grovedb::ExtractCountValueFromElementBytes(got2, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes tx InsertCountTree: " + error);
    }
    if (!has_count || count != 0) {
      Fail("tx InsertCountTree should decode to count = 0");
    }

    grovedb::OperationCost cost;
    if (!db.InsertCountTree({}, {'c', '3'}, &cost, &error)) {
      Fail("InsertCountTree with cost: " + error);
    }
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertCountTree cost should have storage writes");
    }

    std::filesystem::remove_all(ct_dir);
  }

  // ── InsertProvableCountTree positive path ──
  {
    auto pct_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto pct_dir = MakeTempDir("facade_contract_" + std::to_string(pct_now));
    grovedb::GroveDb db;
    if (!db.Open(pct_dir, &error)) {
      Fail("Open(InsertProvableCountTree positive) failed: " + error);
    }
    if (!db.InsertProvableCountTree({}, {'p', 'c', 't'}, &error)) {
      Fail("InsertProvableCountTree root-level: " + error);
    }
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'p', 'c', 't'}, &got, &found, &error)) {
      Fail("Get InsertProvableCountTree: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountTree should be found after insert");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertProvableCountTree: " + error);
    }
    if (variant != 8) {
      Fail("Expected ProvableCountTree variant 8, got " + std::to_string(variant));
    }
    uint64_t count = 1;
    bool has_count = false;
    if (!grovedb::ExtractCountValueFromElementBytes(got, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes InsertProvableCountTree: " + error);
    }
    if (!has_count || count != 0) {
      Fail("InsertProvableCountTree should decode to count = 0");
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertProvableCountTree tx: " + error);
    }
    if (!db.InsertProvableCountTree({}, {'p', '2'}, &tx, &error)) {
      Fail("InsertProvableCountTree in tx: " + error);
    }
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'p', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertProvableCountTree in tx: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountTree should be visible in tx");
    }
    if (!db.Get({}, {'p', '2'}, &got2, &found, &error)) {
      Fail("Get InsertProvableCountTree outside tx: " + error);
    }
    if (found) {
      Fail("InsertProvableCountTree should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction InsertProvableCountTree: " + error);
    }
    if (!db.Get({}, {'p', '2'}, &got2, &found, &error)) {
      Fail("Get InsertProvableCountTree after commit: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountTree should be visible after commit");
    }
    if (!grovedb::ExtractCountValueFromElementBytes(got2, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes tx InsertProvableCountTree: " + error);
    }
    if (!has_count || count != 0) {
      Fail("tx InsertProvableCountTree should decode to count = 0");
    }

    grovedb::OperationCost cost;
    if (!db.InsertProvableCountTree({}, {'p', '3'}, &cost, &error)) {
      Fail("InsertProvableCountTree with cost: " + error);
    }
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertProvableCountTree cost should have storage writes");
    }

    std::filesystem::remove_all(pct_dir);
  }

  // ── InsertProvableCountSumTree positive path ──
  {
    auto pcst_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto pcst_dir = MakeTempDir("facade_contract_" + std::to_string(pcst_now));
    grovedb::GroveDb db;
    if (!db.Open(pcst_dir, &error)) {
      Fail("Open(InsertProvableCountSumTree positive) failed: " + error);
    }
    if (!db.InsertProvableCountSumTree({}, {'p', 'c', 's', 't'}, &error)) {
      Fail("InsertProvableCountSumTree root-level: " + error);
    }
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'p', 'c', 's', 't'}, &got, &found, &error)) {
      Fail("Get InsertProvableCountSumTree: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountSumTree should be found after insert");
    }
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertProvableCountSumTree: " + error);
    }
    if (variant != 10) {
      Fail("Expected ProvableCountSumTree variant 10, got " + std::to_string(variant));
    }
    uint64_t count = 0;
    int64_t sum = 0;
    bool has_count_sum = false;
    if (!grovedb::ExtractProvableCountSumValueFromElementBytes(got, &count, &sum, &has_count_sum,
                                                               &error)) {
      Fail("ExtractProvableCountSumValueFromElementBytes InsertProvableCountSumTree: " + error);
    }
    if (!has_count_sum) {
      Fail("InsertProvableCountSumTree should have count+sum");
    }
    if (count != 0 || sum != 0) {
      Fail("InsertProvableCountSumTree should decode to count=0, sum=0");
    }

    // Test InsertProvableCountSumTree with tx
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertProvableCountSumTree tx: " + error);
    }
    if (!db.InsertProvableCountSumTree({}, {'p', 'c', 's', '2'}, &tx, &error)) {
      Fail("InsertProvableCountSumTree in tx: " + error);
    }
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'p', 'c', 's', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertProvableCountSumTree in tx: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountSumTree should be visible in tx");
    }
    if (!db.Get({}, {'p', 'c', 's', '2'}, &got2, &found, &error)) {
      Fail("Get InsertProvableCountSumTree outside tx: " + error);
    }
    if (found) {
      Fail("InsertProvableCountSumTree should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction InsertProvableCountSumTree: " + error);
    }
    if (!db.Get({}, {'p', 'c', 's', '2'}, &got2, &found, &error)) {
      Fail("Get InsertProvableCountSumTree after commit: " + error);
    }
    if (!found) {
      Fail("InsertProvableCountSumTree should be visible after commit");
    }
    uint64_t count2 = 0;
    int64_t sum2 = 0;
    bool has_count_sum2 = false;
    if (!grovedb::ExtractProvableCountSumValueFromElementBytes(got2, &count2, &sum2, &has_count_sum2,
                                                               &error)) {
      Fail("ExtractProvableCountSumValueFromElementBytes tx InsertProvableCountSumTree: " + error);
    }
    if (!has_count_sum2) {
      Fail("tx InsertProvableCountSumTree should have count+sum");
    }
    if (count2 != 0 || sum2 != 0) {
      Fail("tx InsertProvableCountSumTree should decode to count=0, sum=0");
    }

    // Test InsertProvableCountSumTree with cost
    grovedb::OperationCost cost;
    if (!db.InsertProvableCountSumTree({}, {'p', 'c', 's', '3'}, &cost, &error)) {
      Fail("InsertProvableCountSumTree with cost: " + error);
    }
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertProvableCountSumTree cost should have storage writes");
    }

    std::filesystem::remove_all(pcst_dir);
  }

  // ── InsertSumItem positive path ──
  {
    auto si_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto si_dir = MakeTempDir("facade_contract_" + std::to_string(si_now));
    grovedb::GroveDb db;
    if (!db.Open(si_dir, &error)) {
      Fail("Open(InsertSumItem positive) failed: " + error);
    }
    // Insert a root-level Item under which we can also test SumItem
    if (!db.InsertItem({}, {'k'}, {'v'}, &error)) {
      Fail("InsertItem root-level for SumItem test: " + error);
    }
    // Insert a SumItem at root level
    if (!db.InsertSumItem({}, {'s'}, 42, &error)) {
      Fail("InsertSumItem root-level: " + error);
    }
    // Read it back
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'s'}, &got, &found, &error)) {
      Fail("Get SumItem: " + error);
    }
    if (!found) {
      Fail("SumItem should be found after insert");
    }
    // Decode and verify the variant is SumItem (3) with value 42
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant SumItem: " + error);
    }
    if (variant != 3) {
      Fail("Expected SumItem variant 3, got " + std::to_string(variant));
    }
    grovedb::ElementSumItem si;
    if (!grovedb::DecodeSumItemFromElementBytes(got, &si, &error)) {
      Fail("DecodeSumItemFromElementBytes: " + error);
    }
    if (si.sum != 42) {
      Fail("Expected sum=42, got " + std::to_string(si.sum));
    }

    // Test InsertSumItem with tx
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for SumItem tx: " + error);
    }
    if (!db.InsertSumItem({}, {'s', '2'}, -7, &tx, &error)) {
      Fail("InsertSumItem in tx: " + error);
    }
    // Visible in tx
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'s', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get SumItem in tx: " + error);
    }
    if (!found) {
      Fail("SumItem should be visible in tx");
    }
    // Not yet visible outside tx
    if (!db.Get({}, {'s', '2'}, &got2, &found, &error)) {
      Fail("Get SumItem outside tx: " + error);
    }
    if (found) {
      Fail("SumItem should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction SumItem: " + error);
    }
    // Now visible
    if (!db.Get({}, {'s', '2'}, &got2, &found, &error)) {
      Fail("Get SumItem after commit: " + error);
    }
    if (!found) {
      Fail("SumItem should be visible after commit");
    }
    grovedb::ElementSumItem si2;
    if (!grovedb::DecodeSumItemFromElementBytes(got2, &si2, &error)) {
      Fail("DecodeSumItemFromElementBytes tx: " + error);
    }
    if (si2.sum != -7) {
      Fail("Expected sum=-7, got " + std::to_string(si2.sum));
    }

    // Test InsertSumItem with cost
    grovedb::OperationCost cost;
    if (!db.InsertSumItem({}, {'s', '3'}, 100, &cost, &error)) {
      Fail("InsertSumItem with cost: " + error);
    }
    // Cost should have some non-zero storage writes
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertSumItem cost should have storage writes");
    }

    std::filesystem::remove_all(si_dir);
  }

  // ── QuerySums positive path ──
  {
    auto qs_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto qs_dir = MakeTempDir("facade_contract_" + std::to_string(qs_now));
    grovedb::GroveDb db;
    if (!db.Open(qs_dir, &error)) {
      Fail("Open(QuerySums positive) failed: " + error);
    }
    
    // Insert some SumItems
    if (!db.InsertSumItem({}, {'s', '1'}, 10, &error)) {
      Fail("InsertSumItem s1: " + error);
    }
    if (!db.InsertSumItem({}, {'s', '2'}, 20, &error)) {
      Fail("InsertSumItem s2: " + error);
    }
    if (!db.InsertSumItem({}, {'s', '3'}, 30, &error)) {
      Fail("InsertSumItem s3: " + error);
    }
    // Also insert a regular item (should not be returned by QuerySums)
    if (!db.InsertItem({}, {'k', '1'}, {'v'}, &error)) {
      Fail("InsertItem k1: " + error);
    }
    
    // Query all sums with limit (use RangeFrom to match all keys >= 's')
    grovedb::PathQuery all_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFrom({'s'})), 10, std::nullopt));
    
    std::vector<int64_t> sums;
    if (!db.QuerySums(all_query, &sums, &error)) {
      Fail("QuerySums all: " + error);
    }
    // Should return s1, s2, s3 (sorted by key)
    if (sums.size() != 3) {
      Fail("QuerySums should return 3 sums, got " + std::to_string(sums.size()));
    }
    if (sums[0] != 10 || sums[1] != 20 || sums[2] != 30) {
      Fail("QuerySums returned wrong values: " + 
           std::to_string(sums[0]) + ", " + 
           std::to_string(sums[1]) + ", " + 
           std::to_string(sums[2]));
    }
    
    // Query with limit
    grovedb::PathQuery limited_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFrom({'s'})), 2, std::nullopt));
    
    sums.clear();
    if (!db.QuerySums(limited_query, &sums, &error)) {
      Fail("QuerySums limited: " + error);
    }
    if (sums.size() != 2) {
      Fail("QuerySums with limit=2 should return 2 sums, got " + std::to_string(sums.size()));
    }
    
    // Query with range (only s2, s3 should match)
    grovedb::PathQuery range_query = grovedb::PathQuery::New(
        {},
        grovedb::SizedQuery::New(
            grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Range({'s', '2'}, {'s', '4'})), 10, std::nullopt));
    
    sums.clear();
    if (!db.QuerySums(range_query, &sums, &error)) {
      Fail("QuerySums range: " + error);
    }
    if (sums.size() != 2) {
      Fail("QuerySums range should return 2 sums (s2, s3), got " + std::to_string(sums.size()));
    }
    if (sums[0] != 20 || sums[1] != 30) {
      Fail("QuerySums range returned wrong values");
    }
    
    // Test with transaction
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for QuerySums: " + error);
    }
    if (!db.InsertSumItem({}, {'s', '4'}, 40, &tx, &error)) {
      Fail("InsertSumItem s4 in tx: " + error);
    }
    
    // Query in tx - should see s4
    sums.clear();
    if (!db.QuerySums(all_query, &sums, &tx, &error)) {
      Fail("QuerySums in tx: " + error);
    }
    if (sums.size() != 4) {
      Fail("QuerySums in tx should return 4 sums, got " + std::to_string(sums.size()));
    }
    bool found_s4 = false;
    for (int64_t s : sums) {
      if (s == 40) found_s4 = true;
    }
    if (!found_s4) {
      Fail("QuerySums in tx should see uncommitted s4");
    }
    
    // Query outside tx - should not see s4
    sums.clear();
    if (!db.QuerySums(all_query, &sums, &error)) {
      Fail("QuerySums outside tx: " + error);
    }
    if (sums.size() != 3) {
      Fail("QuerySums outside tx should return 3 sums, got " + std::to_string(sums.size()));
    }
    found_s4 = false;
    for (int64_t s : sums) {
      if (s == 40) found_s4 = true;
    }
    if (found_s4) {
      Fail("QuerySums outside tx should not see uncommitted s4");
    }
    
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction for QuerySums: " + error);
    }
    
    // After commit - should see s4
    sums.clear();
    if (!db.QuerySums(all_query, &sums, &error)) {
      Fail("QuerySums after commit: " + error);
    }
    if (sums.size() != 4) {
      Fail("QuerySums after commit should return 4 sums, got " + std::to_string(sums.size()));
    }
    
    std::filesystem::remove_all(qs_dir);
  }

  // ── InsertItemWithSum positive path ──
  {
    auto qiv_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto qiv_dir = MakeTempDir("facade_contract_" + std::to_string(qiv_now));
    grovedb::GroveDb db;
    if (!db.Open(qiv_dir, &error)) {
      Fail("Open(QueryItemValue positive) failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("InsertEmptyTree root for QueryItemValue failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("InsertItem target for QueryItemValue failed: " + error);
    }
    if (!db.InsertItem(
            {{'r', 'o', 'o', 't'}}, {'i', 't', 'e', 'm', '2'}, {'i', 'w'}, &error)) {
      Fail("InsertItem(item2) for QueryItemValue failed: " + error);
    }
    grovedb::ReferencePathType ref_to_target;
    ref_to_target.kind = grovedb::ReferencePathKind::kSibling;
    ref_to_target.key = {'t', 'a', 'r', 'g', 'e', 't'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f'}, ref_to_target, &error)) {
      Fail("InsertReference for QueryItemValue failed: " + error);
    }

    grovedb::Query query;
    query.items.push_back(grovedb::QueryItem::Key({'i', 't', 'e', 'm', '2'}));
    query.items.push_back(grovedb::QueryItem::Key({'r', 'e', 'f'}));
    query.items.push_back(grovedb::QueryItem::Key({'t', 'a', 'r', 'g', 'e', 't'}));
    grovedb::PathQuery path_query = grovedb::PathQuery::New(
        {{'r', 'o', 'o', 't'}},
        grovedb::SizedQuery::New(query, 3, std::nullopt));

    std::vector<std::vector<uint8_t>> values;
    if (!db.QueryItemValue(path_query, &values, &error)) {
      Fail("QueryItemValue failed: " + error);
    }
    if (values.size() != 3) {
      Fail("QueryItemValue should return three values, got " + std::to_string(values.size()));
    }

    auto contains_bytes = [&](const std::vector<uint8_t>& needle) {
      for (const auto& value : values) {
        if (value == needle) {
          return true;
        }
      }
      return false;
    };
    if (!contains_bytes({'i', 'w'})) {
      Fail("QueryItemValue should include item value");
    }
    if (!contains_bytes({'t', 'v'})) {
      Fail("QueryItemValue should include resolved reference item value");
    }
    size_t tv_count = 0;
    for (const auto& value : values) {
      if (value == std::vector<uint8_t>({'t', 'v'})) {
        ++tv_count;
      }
    }
    if (tv_count != 2) {
      Fail("QueryItemValue should include two tv values (target + resolved ref)");
    }

    std::filesystem::remove_all(qiv_dir);
  }

  // ── InsertItemWithSum positive path ──
  {
    auto iws_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto iws_dir = MakeTempDir("facade_contract_" + std::to_string(iws_now));
    grovedb::GroveDb db;
    if (!db.Open(iws_dir, &error)) {
      Fail("Open(InsertItemWithSum positive) failed: " + error);
    }
    // Insert a root-level ItemWithSum (item value + sum)
    if (!db.InsertItemWithSum({}, {'k', 'w', 's'}, {'v', 'a', 'l', 'u', 'e'}, 42, &error)) {
      Fail("InsertItemWithSum root-level: " + error);
    }
    // Read it back
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.Get({}, {'k', 'w', 's'}, &got, &found, &error)) {
      Fail("Get InsertItemWithSum: " + error);
    }
    if (!found) {
      Fail("InsertItemWithSum should be found after insert");
    }
    // Decode and verify the variant is ItemWithSumItem (9)
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertItemWithSum: " + error);
    }
    if (variant != 9) {
      Fail("Expected ItemWithSumItem variant 9, got " + std::to_string(variant));
    }
    grovedb::ElementItemWithSum iws;
    if (!grovedb::DecodeItemWithSumItemFromElementBytes(got, &iws, &error)) {
      Fail("DecodeItemWithSumItemFromElementBytes: " + error);
    }
    if (iws.sum != 42) {
      Fail("Expected sum=42, got " + std::to_string(iws.sum));
    }
    if (iws.value != std::vector<uint8_t>({'v', 'a', 'l', 'u', 'e'})) {
      Fail("Expected value='value', got " + std::string(iws.value.begin(), iws.value.end()));
    }

    // Test InsertItemWithSum with tx
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertItemWithSum tx: " + error);
    }
    if (!db.InsertItemWithSum({}, {'k', '2'}, {'d', 'a', 't', 'a'}, -7, &tx, &error)) {
      Fail("InsertItemWithSum in tx: " + error);
    }
    // Visible in tx
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'k', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertItemWithSum in tx: " + error);
    }
    if (!found) {
      Fail("InsertItemWithSum should be visible in tx");
    }
    grovedb::ElementItemWithSum iws2;
    if (!grovedb::DecodeItemWithSumItemFromElementBytes(got2, &iws2, &error)) {
      Fail("DecodeItemWithSumItemFromElementBytes in tx: " + error);
    }
    if (iws2.sum != -7) {
      Fail("Expected sum=-7 in tx, got " + std::to_string(iws2.sum));
    }
    // Commit tx
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction for InsertItemWithSum tx: " + error);
    }
    // Visible after commit
    std::vector<uint8_t> got3;
    if (!db.Get({}, {'k', '2'}, &got3, &found, &error)) {
      Fail("Get InsertItemWithSum after commit: " + error);
    }
    if (!found) {
      Fail("InsertItemWithSum should be visible after commit");
    }

    // Test InsertItemWithSum with cost
    grovedb::OperationCost cost;
    if (!db.InsertItemWithSum({}, {'k', '3'}, {'c', 'o', 's', 't'}, 100, &cost, &error)) {
      Fail("InsertItemWithSum with cost: " + error);
    }
    // Cost should have some non-zero storage writes
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertItemWithSum cost should have storage writes");
    }

    std::filesystem::remove_all(iws_dir);
  }

  // ── InsertReference contract tests ──
  {
    // Unopened DB
    grovedb::GroveDb unopened_ir;
    grovedb::ReferencePathType ref_path;
    ref_path.kind = grovedb::ReferencePathKind::kAbsolute;
    ref_path.path = {{'t', 'a', 'r', 'g', 'e', 't'}};
    if (unopened_ir.InsertReference({}, {'r'}, ref_path, &error)) {
      Fail("InsertReference should fail on unopened DB");
    }
    ExpectErrorContains("InsertReference unopened", error, "not opened");

    // Empty key
    auto ir_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto ir_dir = MakeTempDir("facade_contract_" + std::to_string(ir_now));
    grovedb::GroveDb db;
    if (!db.Open(ir_dir, &error)) {
      Fail("Open(InsertReference) failed: " + error);
    }
    if (db.InsertReference({}, {}, ref_path, &error)) {
      Fail("InsertReference should fail for empty key");
    }
    ExpectErrorContains("InsertReference empty key", error, "key is empty");

    // First insert a target tree to reference
    if (!db.InsertEmptyTree({}, {'t', 'a', 'r', 'g', 'e', 't'}, &error)) {
      Fail("InsertEmptyTree target for InsertReference: " + error);
    }

    // Insert a reference at root level
    ref_path.path = {{'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({}, {'r'}, ref_path, &error)) {
      Fail("InsertReference root-level: " + error);
    }
    // Read it back (use GetRaw to get the reference element, not the resolved target)
    std::vector<uint8_t> got;
    bool found = false;
    if (!db.GetRaw({}, {'r'}, &got, &found, &error)) {
      Fail("GetRaw InsertReference: " + error);
    }
    if (!found) {
      Fail("InsertReference should be found after insert");
    }
    // Decode and verify the variant is Reference (1)
    uint64_t variant = 0;
    if (!grovedb::DecodeElementVariant(got, &variant, &error)) {
      Fail("DecodeElementVariant InsertReference: " + error);
    }
    if (variant != 1) {
      Fail("Expected Reference variant 1, got " + std::to_string(variant));
    }
    grovedb::ElementReference ref;
    if (!grovedb::DecodeReferenceFromElementBytes(got, &ref, &error)) {
      Fail("DecodeReferenceFromElementBytes: " + error);
    }
    if (ref.reference_path.path.size() != 1 || ref.reference_path.path[0] != std::vector<uint8_t>({'t', 'a', 'r', 'g', 'e', 't'})) {
      Fail("Expected reference path [target], got something else");
    }

    // Test InsertReference with tx
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for InsertReference tx: " + error);
    }
    ref_path.path = {{'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({}, {'r', '2'}, ref_path, &tx, &error)) {
      Fail("InsertReference in tx: " + error);
    }
    // Visible in tx
    std::vector<uint8_t> got2;
    if (!db.Get({}, {'r', '2'}, &got2, &found, &tx, &error)) {
      Fail("Get InsertReference in tx: " + error);
    }
    if (!found) {
      Fail("InsertReference should be visible in tx");
    }
    // Not yet visible outside tx
    if (!db.Get({}, {'r', '2'}, &got2, &found, &error)) {
      Fail("Get InsertReference outside tx: " + error);
    }
    if (found) {
      Fail("InsertReference should not be visible outside tx before commit");
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction InsertReference: " + error);
    }
    // Now visible (use GetRaw to read the reference element itself)
    if (!db.GetRaw({}, {'r', '2'}, &got2, &found, &error)) {
      Fail("GetRaw InsertReference after commit: " + error);
    }
    if (!found) {
      Fail("InsertReference should be visible after commit");
    }
    grovedb::ElementReference ref2;
    if (!grovedb::DecodeReferenceFromElementBytes(got2, &ref2, &error)) {
      Fail("DecodeReferenceFromElementBytes tx: " + error);
    }
    if (ref2.reference_path.path.size() != 1) {
      Fail("Expected reference path size 1 after commit");
    }

    // Test InsertReference with cost
    grovedb::OperationCost cost;
    ref_path.path = {{'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({}, {'r', '3'}, ref_path, &cost, &error)) {
      Fail("InsertReference with cost: " + error);
    }
    // Cost should have some non-zero storage writes
    if (cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0) {
      Fail("InsertReference cost should have storage writes");
    }

    std::filesystem::remove_all(ir_dir);
  }

  // ── FollowReference contract tests ──
  {
    auto fr_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto fr_dir = MakeTempDir("facade_contract_" + std::to_string(fr_now));
    grovedb::GroveDb db;
    if (!db.Open(fr_dir, &error)) {
      Fail("Open(FollowReference) failed: " + error);
    }
    if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
      Fail("Insert root tree for FollowReference: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'a', 'r', 'g', 'e', 't'}, {'t', 'v'}, &error)) {
      Fail("Insert target item for FollowReference: " + error);
    }

    grovedb::ReferencePathType abs_to_target;
    abs_to_target.kind = grovedb::ReferencePathKind::kAbsolute;
    abs_to_target.path = {{'r', 'o', 'o', 't'}, {'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', '2'}, abs_to_target, &error)) {
      Fail("Insert ref r2 for FollowReference: " + error);
    }

    grovedb::ReferencePathType sib_to_r2;
    sib_to_r2.kind = grovedb::ReferencePathKind::kSibling;
    sib_to_r2.key = {'r', '2'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', '1'}, sib_to_r2, &error)) {
      Fail("Insert ref r1 for FollowReference: " + error);
    }

    std::vector<uint8_t> resolved;
    bool found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'r', '1'}, &resolved, &found, &error)) {
      Fail("FollowReference should resolve chain: " + error);
    }
    if (!found) {
      Fail("FollowReference should report found=true");
    }
    grovedb::ElementItem resolved_item;
    if (!grovedb::DecodeItemFromElementBytes(resolved, &resolved_item, &error)) {
      Fail("FollowReference resolved decode failed: " + error);
    }
    if (resolved_item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("FollowReference resolved wrong value");
    }

    grovedb::OperationCost cost;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'r', '1'}, &resolved, &found, &cost, &error)) {
      Fail("FollowReference with cost should resolve chain: " + error);
    }
    if (cost.seek_count == 0) {
      Fail("FollowReference cost should include seeks");
    }

    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for FollowReference: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t', 'x', 't', 'a', 'r', 'g', 'e', 't'}, {'x', 'v'}, &tx, &error)) {
      Fail("Insert tx target for FollowReference: " + error);
    }
    grovedb::ReferencePathType abs_to_tx_target;
    abs_to_tx_target.kind = grovedb::ReferencePathKind::kAbsolute;
    abs_to_tx_target.path = {{'r', 'o', 'o', 't'}, {'t', 'x', 't', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'t', 'x', 'r'}, abs_to_tx_target, &tx, &error)) {
      Fail("Insert tx ref for FollowReference: " + error);
    }
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'t', 'x', 'r'}, &resolved, &found, &tx, &error)) {
      Fail("FollowReference in tx should resolve tx-visible data: " + error);
    }
    if (!found) {
      Fail("FollowReference in tx should report found=true");
    }
    if (!grovedb::DecodeItemFromElementBytes(resolved, &resolved_item, &error)) {
      Fail("FollowReference in tx decode failed: " + error);
    }
    if (resolved_item.value != std::vector<uint8_t>({'x', 'v'})) {
      Fail("FollowReference in tx resolved wrong value");
    }
    error.clear();
    if (!db.FollowReference({{'r', 'o', 'o', 't'}}, {'t', 'x', 'r'}, &resolved, &found, &error)) {
      Fail("FollowReference outside tx should return missing for uncommitted tx reference: " + error);
    }
    if (found) {
      Fail("FollowReference outside tx should report found=false for uncommitted tx reference");
    }
    if (!db.RollbackTransaction(&tx, &error)) {
      Fail("RollbackTransaction for FollowReference: " + error);
    }

    grovedb::ReferencePathType abs_missing;
    abs_missing.kind = grovedb::ReferencePathKind::kAbsolute;
    abs_missing.path = {{'r', 'o', 'o', 't'}, {'m', 'i', 's', 's', 'i', 'n', 'g'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'm'}, abs_missing, &error)) {
      Fail("Insert missing ref for FollowReference: " + error);
    }
    if (db.FollowReference({{'r', 'o', 'o', 't'}}, {'r', 'm'}, &resolved, &found, &error)) {
      Fail("FollowReference should fail for missing referenced key");
    }
    ExpectErrorContains("FollowReference missing key", error, "corrupted reference path key not found");

    grovedb::ReferencePathType abs_non_tree_parent;
    abs_non_tree_parent.kind = grovedb::ReferencePathKind::kAbsolute;
    abs_non_tree_parent.path = {
        {'r', 'o', 'o', 't'}, {'t', 'a', 'r', 'g', 'e', 't'}, {'c', 'h', 'i', 'l', 'd'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'n', 't'}, abs_non_tree_parent, &error)) {
      Fail("Insert non-tree-parent ref for FollowReference: " + error);
    }
    if (db.FollowReference({{'r', 'o', 'o', 't'}}, {'r', 'n', 't'}, &resolved, &found, &error)) {
      Fail("FollowReference should fail for non-tree parent layer");
    }
    ExpectErrorContains("FollowReference non-tree parent layer",
                        error,
                        "corrupted reference path parent layer not found");

    grovedb::ReferencePathType abs_empty;
    abs_empty.kind = grovedb::ReferencePathKind::kAbsolute;
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e'}, abs_empty, &error)) {
      Fail("Insert empty absolute reference for FollowReference: " + error);
    }
    if (db.FollowReference({{'r', 'o', 'o', 't'}}, {'r', 'e'}, &resolved, &found, &error)) {
      Fail("FollowReference should fail for empty absolute reference");
    }
    ExpectErrorContains("FollowReference empty reference", error, "empty reference");

    grovedb::ReferencePathType sib_to_c2;
    sib_to_c2.kind = grovedb::ReferencePathKind::kSibling;
    sib_to_c2.key = {'c', '2'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'c', '1'}, sib_to_c2, &error)) {
      Fail("Insert cyclic c1 reference: " + error);
    }
    grovedb::ReferencePathType sib_to_c1;
    sib_to_c1.kind = grovedb::ReferencePathKind::kSibling;
    sib_to_c1.key = {'c', '1'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'c', '2'}, sib_to_c1, &error)) {
      Fail("Insert cyclic c2 reference: " + error);
    }
    if (db.FollowReference({{'r', 'o', 'o', 't'}}, {'c', '1'}, &resolved, &found, &error)) {
      Fail("FollowReference should fail for cyclic references");
    }
    ExpectErrorContains("FollowReference cyclic", error, "cyclic reference");

    const std::vector<uint8_t> hop_root_key = {'h', 'o', 'p', '_', 'l', 'i', 'm'};
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, hop_root_key, &error)) {
      Fail("InsertEmptyTree hop-limit root failed: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, hop_root_key}, {'t', 'a', 'r', 'g', 'e', 't'}, {'h', 'v'}, &error)) {
      Fail("Insert target item for hop-limit reference chain failed: " + error);
    }
    const std::vector<std::vector<uint8_t>> chain_keys = {
        {'l', 'a'}, {'l', 'b'}, {'l', 'c'}, {'l', 'd'}, {'l', 'e'}, {'l', 'f'},
        {'l', 'g'}, {'l', 'h'}, {'l', 'i'}, {'l', 'j'}, {'l', 'k'}};
    for (size_t i = 0; i < chain_keys.size(); ++i) {
      grovedb::ReferencePathType long_ref;
      long_ref.kind = grovedb::ReferencePathKind::kSibling;
      if (i + 1 < chain_keys.size()) {
        long_ref.key = chain_keys[i + 1];
      } else {
        long_ref.key = {'t', 'a', 'r', 'g', 'e', 't'};
      }
      if (!db.InsertReference({{'r', 'o', 'o', 't'}, hop_root_key}, chain_keys[i], long_ref, &error)) {
        Fail("Insert long-chain reference failed: " + error);
      }
    }
    if (db.FollowReference({{'r', 'o', 'o', 't'}, hop_root_key}, {'l', 'a'}, &resolved, &found, &error)) {
      Fail("FollowReference should fail on reference hop limit");
    }
    ExpectErrorContainsAny(
        "FollowReference limit",
        error,
        {"reference limit", "corrupted reference path key not found"});

    // Mixed path type chain: UpstreamRootHeight -> Sibling -> Absolute -> target
    grovedb::ReferencePathType mixed_abs_to_target;
    mixed_abs_to_target.kind = grovedb::ReferencePathKind::kAbsolute;
    mixed_abs_to_target.path = {{'r', 'o', 'o', 't'}, {'t', 'a', 'r', 'g', 'e', 't'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '_', 'a'}, mixed_abs_to_target, &error)) {
      Fail("Insert ref_a absolute for mixed path: " + error);
    }
    grovedb::ReferencePathType mixed_sib_to_ref_a;
    mixed_sib_to_ref_a.kind = grovedb::ReferencePathKind::kSibling;
    mixed_sib_to_ref_a.key = {'r', 'e', 'f', '_', 'a'};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}}, {'r', 'e', 'f', '_', 'b'}, mixed_sib_to_ref_a, &error)) {
      Fail("Insert ref_b sibling for mixed path: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'i', 'n', 'n', 'e', 'r'}, &error)) {
      Fail("Insert inner tree for mixed path: " + error);
    }
    grovedb::ReferencePathType mixed_upstream_to_ref_b;
    mixed_upstream_to_ref_b.kind = grovedb::ReferencePathKind::kUpstreamRootHeight;
    mixed_upstream_to_ref_b.height = 1;
    mixed_upstream_to_ref_b.path = {{'r', 'e', 'f', '_', 'b'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'i', 'n', 'n', 'e', 'r'}},
                            {'r', 'e', 'f', '_', 'c'}, mixed_upstream_to_ref_b, &error)) {
      Fail("Insert ref_c upstream for mixed path: " + error);
    }
    std::vector<uint8_t> mixed_resolved;
    bool mixed_found = false;
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'i', 'n', 'n', 'e', 'r'}},
                            {'r', 'e', 'f', '_', 'c'}, &mixed_resolved, &mixed_found, &error)) {
      Fail("FollowReference mixed path chain should resolve: " + error);
    }
    if (!mixed_found) {
      Fail("FollowReference mixed path chain should report found=true");
    }
    grovedb::ElementItem mixed_item;
    if (!grovedb::DecodeItemFromElementBytes(mixed_resolved, &mixed_item, &error)) {
      Fail("FollowReference mixed path decode failed: " + error);
    }
    if (mixed_item.value != std::vector<uint8_t>({'t', 'v'})) {
      Fail("FollowReference mixed path resolved wrong value");
    }

    // UpstreamRootHeightWithParentPathAddition: from root/branch/target/ref_p,
    // resolve to root/alias/target.
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'a', 'l', 'i', 'a', 's'}, &error)) {
      Fail("Insert alias tree for parent-path-addition reference: " + error);
    }
    if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'a', 'l', 'i', 'a', 's'}},
                       {'t', 'a', 'r', 'g', 'e', 't'},
                       {'p', 'v'},
                       &error)) {
      Fail("Insert alias target item for parent-path-addition reference: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, &error)) {
      Fail("Insert branch tree for parent-path-addition reference: " + error);
    }
    if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                            {'t', 'a', 'r', 'g', 'e', 't'},
                            &error)) {
      Fail("Insert nested target tree for parent-path-addition reference: " + error);
    }
    grovedb::ReferencePathType parent_add_ref;
    parent_add_ref.kind = grovedb::ReferencePathKind::kUpstreamRootHeightWithParentPathAddition;
    parent_add_ref.height = 1;
    parent_add_ref.path = {{'a', 'l', 'i', 'a', 's'}};
    if (!db.InsertReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'t', 'a', 'r', 'g', 'e', 't'}},
                            {'r', 'p'},
                            parent_add_ref,
                            &error)) {
      Fail("Insert parent-path-addition reference: " + error);
    }
    if (!db.FollowReference({{'r', 'o', 'o', 't'}, {'b', 'r', 'a', 'n', 'c', 'h'}, {'t', 'a', 'r', 'g', 'e', 't'}},
                            {'r', 'p'},
                            &mixed_resolved,
                            &mixed_found,
                            &error)) {
      Fail("FollowReference parent-path-addition should resolve: " + error);
    }
    if (!mixed_found) {
      Fail("FollowReference parent-path-addition should report found=true");
    }
    if (!grovedb::DecodeItemFromElementBytes(mixed_resolved, &mixed_item, &error)) {
      Fail("FollowReference parent-path-addition decode failed: " + error);
    }
    if (mixed_item.value != std::vector<uint8_t>({'p', 'v'})) {
      Fail("FollowReference parent-path-addition resolved wrong value");
    }

    std::filesystem::remove_all(fr_dir);
  }

  // ── QueryKeyElementPairs contract tests ──
  {
    // Unopened DB
    grovedb::GroveDb unopened_qkep;
    std::vector<grovedb::GroveDb::KeyElementPair> out;
    grovedb::PathQuery pq = grovedb::PathQuery::NewSingleKey({}, {107});
    if (unopened_qkep.QueryKeyElementPairs(pq, &out, &error)) {
      Fail("QueryKeyElementPairs should fail on unopened DB");
    }
    ExpectErrorContains("QueryKeyElementPairs unopened", error, "not opened");

    // Null output
    auto qkep_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto qkep_dir = MakeTempDir("facade_contract_" + std::to_string(qkep_now));
    grovedb::GroveDb qkep_db;
    if (!qkep_db.Open(qkep_dir, &error)) {
      Fail("Open(QueryKeyElementPairs) failed: " + error);
    }
    if (qkep_db.QueryKeyElementPairs(pq, nullptr, &error)) {
      Fail("QueryKeyElementPairs should fail for null output");
    }
    ExpectErrorContains("QueryKeyElementPairs null", error, "output is null");

    // Empty query items
    grovedb::PathQuery empty_pq;
    empty_pq.path = {};
    empty_pq.query.query.items = {};
    empty_pq.query.limit = 10;
    if (qkep_db.QueryKeyElementPairs(empty_pq, &out, &error)) {
      Fail("QueryKeyElementPairs should fail for empty query items");
    }
    ExpectErrorContains("QueryKeyElementPairs empty", error, "query items are empty");

    // Unsupported query shape (subquery)
    grovedb::PathQuery subquery_pq;
    subquery_pq.path = {};
    subquery_pq.query.query.items.push_back(grovedb::QueryItem::Key({107}));
    subquery_pq.query.query.default_subquery_branch.subquery_path =
        std::vector<std::vector<uint8_t>>{{98, 114, 97, 110, 99, 104}};
    subquery_pq.query.query.default_subquery_branch.subquery =
        std::make_unique<grovedb::Query>(grovedb::Query::NewSingleKey({120}));
    subquery_pq.query.limit = 10;
    if (qkep_db.QueryKeyElementPairs(subquery_pq, &out, &error)) {
      Fail("QueryKeyElementPairs should fail for subquery shape");
    }
    ExpectErrorContains("QueryKeyElementPairs subquery", error, "unsupported query shape");

    // Empty DB - should return empty results
    out.clear();
    grovedb::PathQuery single_key_pq = grovedb::PathQuery::NewSingleKey({}, {107});
    if (!qkep_db.QueryKeyElementPairs(single_key_pq, &out, &error)) {
      Fail("QueryKeyElementPairs on empty DB: " + error);
    }
    if (!out.empty()) {
      Fail("QueryKeyElementPairs empty DB should return empty results, got " +
           std::to_string(out.size()));
    }

    // Insert items and query
    if (!qkep_db.InsertItem({}, {107}, {118}, &error)) {
      Fail("InsertItem for QueryKeyElementPairs: " + error);
    }
    out.clear();
    if (!qkep_db.QueryKeyElementPairs(single_key_pq, &out, &error)) {
      Fail("QueryKeyElementPairs after insert: " + error);
    }
    if (out.size() != 1) {
      Fail("QueryKeyElementPairs should return 1 result, got " + std::to_string(out.size()));
    }
    if (out[0].key != std::vector<uint8_t>{107}) {
      Fail("QueryKeyElementPairs returned wrong key");
    }
    std::vector<uint8_t> expected_element;
    if (!grovedb::EncodeItemToElementBytes({118}, &expected_element, &error)) {
      Fail("EncodeItemToElementBytes failed: " + error);
    }
    if (out[0].element_bytes != expected_element) {
      Fail("QueryKeyElementPairs returned wrong element bytes");
    }

    // Insert reference and query (should resolve)
    if (!qkep_db.InsertItem({}, {116, 97, 114, 103, 101, 116}, {116, 97, 114, 103, 101, 116, 95, 118, 97, 108, 117, 101}, &error)) {
      Fail("InsertItem target for QueryKeyElementPairs ref: " + error);
    }
    std::vector<uint8_t> ref_element;
    grovedb::ElementReference ref;
    ref.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
    ref.reference_path.path = {{116, 97, 114, 103, 101, 116}};
    if (!grovedb::EncodeReferenceToElementBytes(ref, &ref_element, &error)) {
      Fail("EncodeReferenceToElementBytes failed: " + error);
    }
    if (!qkep_db.Insert({}, {114, 101, 102}, ref_element, &error)) {
      Fail("Insert ref for QueryKeyElementPairs: " + error);
    }
    out.clear();
    grovedb::PathQuery ref_pq = grovedb::PathQuery::NewSingleKey({}, {114, 101, 102});
    if (!qkep_db.QueryKeyElementPairs(ref_pq, &out, &error)) {
      Fail("QueryKeyElementPairs for ref: " + error);
    }
    if (out.size() != 1) {
      Fail("QueryKeyElementPairs ref should return 1 result, got " + std::to_string(out.size()));
    }
    // The resolved element should be the target item, not the reference
    std::vector<uint8_t> expected_target_element;
    if (!grovedb::EncodeItemToElementBytes({116, 97, 114, 103, 101, 116, 95, 118, 97, 108, 117, 101},
                                           &expected_target_element, &error)) {
      Fail("EncodeItemToElementBytes target failed: " + error);
    }
    if (out[0].element_bytes != expected_target_element) {
      Fail("QueryKeyElementPairs should resolve reference to target element");
    }

    // Query with transaction - should see uncommitted changes
    grovedb::GroveDb::Transaction tx_qkep;
    if (!qkep_db.StartTransaction(&tx_qkep, &error)) {
      Fail("StartTransaction for QueryKeyElementPairs: " + error);
    }
    if (!qkep_db.InsertItem({}, {107, 50}, {118, 50}, &tx_qkep, &error)) {
      Fail("InsertItem in tx for QueryKeyElementPairs: " + error);
    }
    out.clear();
    grovedb::PathQuery multi_pq = grovedb::PathQuery::NewSingleKey({}, std::vector<uint8_t>{});
    multi_pq.query.query.items.push_back(grovedb::QueryItem::RangeFull());
    if (!qkep_db.QueryKeyElementPairs(multi_pq, &out, &tx_qkep, &error)) {
      Fail("QueryKeyElementPairs in tx: " + error);
    }
    // Should see k, k2, ref, target (4 items)
    if (out.size() != 4) {
      Fail("QueryKeyElementPairs in tx should see 4 items, got " + std::to_string(out.size()));
    }

    // Query without tx should not see uncommitted
    out.clear();
    if (!qkep_db.QueryKeyElementPairs(multi_pq, &out, &error)) {
      Fail("QueryKeyElementPairs outside tx: " + error);
    }
    if (out.size() != 3) {
      Fail("QueryKeyElementPairs outside tx should see 3 items, got " + std::to_string(out.size()));
    }

    if (!qkep_db.CommitTransaction(&tx_qkep, &error)) {
      Fail("CommitTransaction QueryKeyElementPairs: " + error);
    }
    // After commit, should see 4 items
    out.clear();
    if (!qkep_db.QueryKeyElementPairs(multi_pq, &out, &error)) {
      Fail("QueryKeyElementPairs after commit: " + error);
    }
    if (out.size() != 4) {
      Fail("QueryKeyElementPairs after commit should see 4 items, got " + std::to_string(out.size()));
    }

    // Test committed transaction rejection
    if (qkep_db.QueryKeyElementPairs(multi_pq, &out, &tx_qkep, &error)) {
      Fail("QueryKeyElementPairs should fail for committed tx");
    }
    ExpectErrorContainsAny("QueryKeyElementPairs committed tx", error, {"committed", "not active"});

    // Test uninitialized transaction rejection
    grovedb::GroveDb::Transaction tx_uninit;
    if (qkep_db.QueryKeyElementPairs(multi_pq, &out, &tx_uninit, &error)) {
      Fail("QueryKeyElementPairs should fail for uninitialized tx");
    }
    ExpectErrorContains("QueryKeyElementPairs uninitialized tx", error, "not active");

    // Test limit enforcement
    out.clear();
    grovedb::PathQuery limited_pq = grovedb::PathQuery::NewSingleKey({}, std::vector<uint8_t>{});
    limited_pq.query.query.items.push_back(grovedb::QueryItem::RangeFull());
    limited_pq.query.limit = 2;
    if (!qkep_db.QueryKeyElementPairs(limited_pq, &out, &error)) {
      Fail("QueryKeyElementPairs with limit: " + error);
    }
    if (out.size() != 2) {
      Fail("QueryKeyElementPairs with limit=2 should return 2 items, got " +
           std::to_string(out.size()));
    }

    // Test offset enforcement
    out.clear();
    grovedb::PathQuery offset_pq = grovedb::PathQuery::NewSingleKey({}, std::vector<uint8_t>{});
    offset_pq.query.query.items.push_back(grovedb::QueryItem::RangeFull());
    offset_pq.query.limit = 10;
    offset_pq.query.offset = 2;
    if (!qkep_db.QueryKeyElementPairs(offset_pq, &out, &error)) {
      Fail("QueryKeyElementPairs with offset: " + error);
    }
    // Should skip first 2 items and return remaining 2
    if (out.size() != 2) {
      Fail("QueryKeyElementPairs with offset=2 should return 2 items, got " +
           std::to_string(out.size()));
    }

    std::filesystem::remove_all(qkep_dir);
  }

  // ── Non-tx insert cache reuse contract tests ──
  {
    auto cache_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto cache_dir = MakeTempDir("facade_contract_" + std::to_string(cache_now));
    grovedb::GroveDb cache_db;
    if (!cache_db.Open(cache_dir, &error)) {
      Fail("Open(non-tx insert cache reuse) failed: " + error);
    }
    if (!cache_db.InsertEmptyTree({}, {'a'}, &error)) {
      Fail("InsertEmptyTree non-tx cache root a failed: " + error);
    }
    if (!cache_db.InsertEmptyTree({}, {'b'}, &error)) {
      Fail("InsertEmptyTree non-tx cache root b failed: " + error);
    }

    // Repeated non-tx inserts on the same path should remain readable.
    if (!cache_db.Insert({{'a'}}, {'k', '1'}, item_element, &error)) {
      Fail("Insert same-path a/k1 failed: " + error);
    }
    if (!cache_db.Insert({{'a'}}, {'k', '2'}, item_element, &error)) {
      Fail("Insert same-path a/k2 failed: " + error);
    }
    if (!cache_db.Insert({{'a'}}, {'k', '3'}, item_element, &error)) {
      Fail("Insert same-path a/k3 failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> got;
    for (const auto& key : {std::vector<uint8_t>{'k', '1'},
                            std::vector<uint8_t>{'k', '2'},
                            std::vector<uint8_t>{'k', '3'}}) {
      if (!cache_db.Get({{'a'}}, key, &got, &found, &error)) {
        Fail("Get same-path non-tx cache key failed: " + error);
      }
      if (!found || got != item_element) {
        Fail("Repeated same-path non-tx inserts should remain readable");
      }
    }

    // Switching paths and then switching back should keep both subtrees healthy.
    if (!cache_db.Insert({{'b'}}, {'b', '1'}, item_element, &error)) {
      Fail("Insert switched-path b/b1 failed: " + error);
    }
    if (!cache_db.Insert({{'a'}}, {'k', '4'}, item_element, &error)) {
      Fail("Insert switched-back a/k4 failed: " + error);
    }
    if (!cache_db.Get({{'b'}}, {'b', '1'}, &got, &found, &error)) {
      Fail("Get switched-path b/b1 failed: " + error);
    }
    if (!found || got != item_element) {
      Fail("Switching to path b should not break reads");
    }
    if (!cache_db.Get({{'a'}}, {'k', '4'}, &got, &found, &error)) {
      Fail("Get switched-back a/k4 failed: " + error);
    }
    if (!found || got != item_element) {
      Fail("Switching back to path a should not break reads");
    }

    // A failing same-path non-tx insert should clear any reused cache state so
    // a retry on the same path still succeeds.
    if (cache_db.Insert({{'a'}}, {}, item_element, &error)) {
      Fail("Insert with empty key should fail on reused non-tx path");
    }
    ExpectErrorContains("Non-tx insert cache empty key", error, "key is empty");
    if (!cache_db.Insert({{'a'}}, {'k', '5'}, item_element, &error)) {
      Fail("Insert retry after non-tx cache failure failed: " + error);
    }
    if (!cache_db.Get({{'a'}}, {'k', '5'}, &got, &found, &error)) {
      Fail("Get retry key after non-tx cache failure failed: " + error);
    }
    if (!found || got != item_element) {
      Fail("Retry after same-path non-tx failure should remain readable");
    }

    std::filesystem::remove_all(cache_dir);
  }

  // ── VerifyGroveDb contract tests ──
  {
    // Unopened DB
    grovedb::GroveDb unopened_vg;
    std::vector<grovedb::VerificationIssue> issues;
    if (unopened_vg.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb should fail on unopened DB");
    }
    ExpectErrorContains("VerifyGroveDb unopened", error, "not opened");

    // Null output
    auto vg_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto vg_dir = MakeTempDir("facade_contract_" + std::to_string(vg_now));
    grovedb::GroveDb vg_db;
    if (!vg_db.Open(vg_dir, &error)) {
      Fail("Open(VerifyGroveDb) failed: " + error);
    }
    if (vg_db.VerifyGroveDb(false, true, nullptr, &error)) {
      Fail("VerifyGroveDb should fail for null issues output");
    }
    ExpectErrorContains("VerifyGroveDb null", error, "issues output is null");

    // Empty DB - should have no issues
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb on empty DB: " + error);
    }
    if (!issues.empty()) {
      Fail("VerifyGroveDb empty DB should have no issues, got " + std::to_string(issues.size()));
    }

    // Insert a tree and verify - should have no issues
    if (!vg_db.InsertEmptyTree({}, {'t'}, &error)) {
      Fail("InsertEmptyTree for VerifyGroveDb: " + error);
    }
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb after insert: " + error);
    }
    if (!issues.empty()) {
      Fail("VerifyGroveDb with valid tree should have no issues, got " + std::to_string(issues.size()));
    }

    // Insert items into tree and verify - should have no issues
    if (!vg_db.InsertItem({}, {'t'}, {'s', 'u', 'b', 'v', 'a', 'l'}, &error)) {
      Fail("InsertItem into tree for VerifyGroveDb: " + error);
    }
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb with nested items: " + error);
    }
    if (!issues.empty()) {
      Fail("VerifyGroveDb with nested items should have no issues, got " + std::to_string(issues.size()));
    }

    // Verify with transaction - should reflect uncommitted changes
    grovedb::GroveDb::Transaction tx_vg;
    if (!vg_db.StartTransaction(&tx_vg, &error)) {
      Fail("StartTransaction for VerifyGroveDb: " + error);
    }
    std::vector<uint8_t> new_val = {'n', 'e', 'w'};
    if (!vg_db.InsertItem({}, {'t'}, new_val, &tx_vg, &error)) {
      Fail("InsertItem in tx for VerifyGroveDb: " + error);
    }
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &tx_vg, &error)) {
      Fail("VerifyGroveDb in tx: " + error);
    }
    // Should have no issues - the uncommitted item is valid
    if (!issues.empty()) {
      Fail("VerifyGroveDb in tx with uncommitted should have no issues, got " + std::to_string(issues.size()));
    }
    // Without tx should see committed state - still valid
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb outside tx: " + error);
    }
    // Should have no issues - committed state is valid (doesn't include uncommitted)
    if (!issues.empty()) {
      Fail("VerifyGroveDb outside tx should have no issues, got " + std::to_string(issues.size()));
    }

    if (!vg_db.CommitTransaction(&tx_vg, &error)) {
      Fail("CommitTransaction VerifyGroveDb: " + error);
    }
    // After commit, both should see same
    issues.clear();
    if (!vg_db.VerifyGroveDb(false, true, &issues, &error)) {
      Fail("VerifyGroveDb after commit: " + error);
    }
    if (!issues.empty()) {
      Fail("VerifyGroveDb after commit should have no issues, got " + std::to_string(issues.size()));
    }

    // Test committed transaction rejection
    if (vg_db.VerifyGroveDb(false, true, &issues, &tx_vg, &error)) {
      Fail("VerifyGroveDb should fail for committed tx");
    }
    ExpectErrorContainsAny("VerifyGroveDb committed tx", error, {"committed", "not active"});

    // Test uninitialized transaction rejection
    grovedb::GroveDb::Transaction tx_uninit;
    if (vg_db.VerifyGroveDb(false, true, &issues, &tx_uninit, &error)) {
      Fail("VerifyGroveDb should fail for uninitialized tx");
    }
    ExpectErrorContains("VerifyGroveDb uninitialized tx", error, "not active");

    std::filesystem::remove_all(vg_dir);
  }

  return 0;
}
