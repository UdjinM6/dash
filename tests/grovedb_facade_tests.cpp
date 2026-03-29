#include "element.h"
#include "grovedb.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

bool DecodeVarintAt(const std::vector<uint8_t>& bytes, size_t* cursor, uint64_t* out) {
  if (cursor == nullptr || out == nullptr) {
    return false;
  }
  uint64_t result = 0;
  int shift = 0;
  while (*cursor < bytes.size()) {
    const uint8_t byte = bytes[*cursor];
    (*cursor)++;
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 64) {
      return false;
    }
  }
  return false;
}

bool SkipOptionBytes(const std::vector<uint8_t>& bytes, size_t* cursor) {
  uint64_t option = 0;
  if (!DecodeVarintAt(bytes, cursor, &option)) {
    return false;
  }
  if (option == 0) {
    return true;
  }
  if (option != 1) {
    return false;
  }
  uint64_t size = 0;
  if (!DecodeVarintAt(bytes, cursor, &size)) {
    return false;
  }
  if (*cursor + size > bytes.size()) {
    return false;
  }
  *cursor += static_cast<size_t>(size);
  return true;
}

bool DecodeCountTreeCount(const std::vector<uint8_t>& element_bytes, uint64_t* out_count) {
  if (out_count == nullptr) {
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarintAt(element_bytes, &cursor, &variant)) {
    return false;
  }
  if (variant != 6) {
    return false;
  }
  if (!SkipOptionBytes(element_bytes, &cursor)) {
    return false;
  }
  return DecodeVarintAt(element_bytes, &cursor, out_count);
}

bool DecodeProvableCountTreeCount(const std::vector<uint8_t>& element_bytes,
                                  uint64_t* out_count) {
  if (out_count == nullptr) {
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarintAt(element_bytes, &cursor, &variant)) {
    return false;
  }
  if (variant != 8) {
    return false;
  }
  if (!SkipOptionBytes(element_bytes, &cursor)) {
    return false;
  }
  return DecodeVarintAt(element_bytes, &cursor, out_count);
}

bool BuildNestedPath(grovedb::GroveDb* db,
                     const std::vector<uint8_t>& tree_element,
                     size_t depth,
                     uint8_t namespace_tag,
                     std::vector<std::vector<uint8_t>>* out_path,
                     std::string* error) {
  if (db == nullptr || out_path == nullptr) {
    if (error) *error = "BuildNestedPath inputs are null";
    return false;
  }
  out_path->clear();
  std::vector<std::vector<uint8_t>> path;
  for (size_t i = 0; i < depth; ++i) {
    std::vector<uint8_t> key = {'d', namespace_tag, static_cast<uint8_t>('0' + i)};
    if (!db->Insert(path, key, tree_element, error)) {
      return false;
    }
    path.push_back(std::move(key));
  }
  *out_path = std::move(path);
  return true;
}

}  // namespace

int main() {
  std::string error;
  grovedb::GroveDb db;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string dir = MakeTempDir("facade_test_" + std::to_string(now));
  if (!db.Open(dir, &error)) {
    Fail("Open failed: " + error);
  }

  std::vector<uint8_t> tree_element;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error)) {
    Fail("EncodeTreeToElementBytes failed: " + error);
  }

  const std::vector<uint8_t> leaf_key = {'l', 'e', 'a', 'f', '1'};
  if (!db.Insert({}, leaf_key, tree_element, &error)) {
    Fail("Insert root tree failed: " + error);
  }

  std::vector<uint8_t> item_element;
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_element, &error)) {
    Fail("EncodeItemToElementBytes failed: " + error);
  }

  if (!db.Insert({leaf_key}, {'k', '1'}, item_element, &error)) {
    Fail("Insert child item failed: " + error);
  }
  if (!db.Flush(&error)) {
    Fail("Flush failed: " + error);
  }
  std::vector<uint8_t> got;
  bool found = false;

  // Nested subtree path: root -> leaf1 -> leaf2 -> key.
  const std::vector<uint8_t> leaf2_key = {'l', 'e', 'a', 'f', '2'};
  if (!db.Insert({leaf_key}, leaf2_key, tree_element, &error)) {
    Fail("Insert nested subtree failed: " + error);
  }
  if (!db.Insert({leaf_key, leaf2_key}, {'k', 'n'}, item_element, &error)) {
    Fail("Insert nested item failed: " + error);
  }
  if (!db.Get({leaf_key, leaf2_key}, {'k', 'n'}, &got, &found, &error)) {
    Fail("Get nested item failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Get nested item mismatch");
  }

  // Non-tx insert optimization safety: explicit-tx equivalence on shallow/deep paths.
  {
    auto run_insert_case = [&](size_t depth, bool use_explicit_tx) {
      const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      const std::string case_dir =
          MakeTempDir("facade_insert_depth_" + std::to_string(depth) + "_" +
                      (use_explicit_tx ? "tx_" : "notx_") + std::to_string(ts));
      grovedb::GroveDb case_db;
      std::string case_error;
      if (!case_db.Open(case_dir, &case_error)) {
        Fail("Open insert equivalence DB failed: " + case_error);
      }
      std::vector<std::vector<uint8_t>> case_path;
      if (!BuildNestedPath(&case_db, tree_element, depth, static_cast<uint8_t>('a' + depth),
                           &case_path, &case_error)) {
        Fail("BuildNestedPath failed: " + case_error);
      }
      std::vector<uint8_t> case_item;
      const std::vector<uint8_t> item_value = {'v', static_cast<uint8_t>('0' + depth)};
      if (!grovedb::EncodeItemToElementBytes(item_value, &case_item, &case_error)) {
        Fail("Encode item for insert equivalence failed: " + case_error);
      }
      const std::vector<uint8_t> item_key = {'i', static_cast<uint8_t>('0' + depth)};
      if (use_explicit_tx) {
        grovedb::GroveDb::Transaction tx;
        if (!case_db.StartTransaction(&tx, &case_error)) {
          Fail("StartTransaction for insert equivalence failed: " + case_error);
        }
        if (!case_db.Insert(case_path, item_key, case_item, &tx, &case_error)) {
          Fail("Explicit tx insert for insert equivalence failed: " + case_error);
        }
        if (!case_db.CommitTransaction(&tx, &case_error)) {
          Fail("CommitTransaction for insert equivalence failed: " + case_error);
        }
      } else {
        if (!case_db.Insert(case_path, item_key, case_item, &case_error)) {
          Fail("Non-tx insert for insert equivalence failed: " + case_error);
        }
      }
      std::vector<uint8_t> readback;
      bool case_found = false;
      if (!case_db.Get(case_path, item_key, &readback, &case_found, &case_error)) {
        Fail("Get after insert equivalence op failed: " + case_error);
      }
      if (!case_found || readback != case_item) {
        Fail("Insert equivalence readback mismatch");
      }
      std::vector<uint8_t> root_hash;
      if (!case_db.RootHash(&root_hash, &case_error)) {
        Fail("RootHash after insert equivalence failed: " + case_error);
      }
      if (root_hash.empty()) {
        Fail("RootHash after insert equivalence should not be empty");
      }
      std::filesystem::remove_all(case_dir);
    };

    for (size_t depth : {static_cast<size_t>(1), static_cast<size_t>(4)}) {
      run_insert_case(depth, false);
      run_insert_case(depth, true);
    }
  }

  // Deep non-tx delete should safely propagate ancestor root-key rewrites.
  // This exercises PropagateSubtreeRootKeyUp() without a cache on a depth>=4 path.
  {
    const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string del_dir = MakeTempDir("facade_delete_deep_" + std::to_string(ts));
    grovedb::GroveDb del_db;
    std::string del_error;
    if (!del_db.Open(del_dir, &del_error)) {
      Fail("Open deep delete regression DB failed: " + del_error);
    }

    std::vector<std::vector<uint8_t>> del_path;
    if (!BuildNestedPath(&del_db, tree_element, 4, 'z', &del_path, &del_error)) {
      Fail("BuildNestedPath(deep delete regression) failed: " + del_error);
    }

    std::vector<uint8_t> v1;
    std::vector<uint8_t> v2;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &v1, &del_error) ||
        !grovedb::EncodeItemToElementBytes({'v', '2'}, &v2, &del_error)) {
      Fail("EncodeItemToElementBytes(deep delete regression) failed: " + del_error);
    }
    const std::vector<uint8_t> k1 = {'k', '1'};
    const std::vector<uint8_t> k2 = {'k', '2'};
    if (!del_db.Insert(del_path, k1, v1, &del_error) ||
        !del_db.Insert(del_path, k2, v2, &del_error)) {
      Fail("Insert for deep delete regression failed: " + del_error);
    }

    bool deleted = false;
    if (!del_db.Delete(del_path, k1, &deleted, &del_error)) {
      Fail("Deep non-tx delete regression failed: " + del_error);
    }
    if (!deleted) {
      Fail("Deep non-tx delete regression should report deleted=true");
    }

    std::vector<uint8_t> got2;
    bool found2 = false;
    if (!del_db.Get(del_path, k2, &got2, &found2, &del_error)) {
      Fail("Get surviving sibling after deep delete failed: " + del_error);
    }
    if (!found2 || got2 != v2) {
      Fail("Surviving sibling mismatch after deep delete");
    }

    std::vector<uint8_t> got1;
    bool found1 = false;
    if (!del_db.Get(del_path, k1, &got1, &found1, &del_error)) {
      Fail("Get deleted key after deep delete failed: " + del_error);
    }
    if (found1) {
      Fail("Deleted key should not be found after deep delete");
    }

    std::vector<uint8_t> root_hash;
    if (!del_db.RootHash(&root_hash, &del_error)) {
      Fail("RootHash after deep delete regression failed: " + del_error);
    }
    if (root_hash.empty()) {
      Fail("RootHash after deep delete regression should not be empty");
    }
    std::filesystem::remove_all(del_dir);
  }

  // SumTree parent should support root-key propagation on nested writes.
  const std::vector<uint8_t> sum_parent_key = {'s', 'u', 'm', 'p'};
  const std::vector<uint8_t> flagged_sum_tree = {0x04, 0x00, 0x00, 0x01, 0x02, 'f', 'g'};
  if (!db.Insert({}, sum_parent_key, flagged_sum_tree, &error)) {
    Fail("Insert sum tree parent failed: " + error);
  }
  if (!db.Insert({sum_parent_key}, {'c', '1'}, item_element, &error)) {
    Fail("Insert under sum tree parent failed: " + error);
  }
  if (!db.Get({}, sum_parent_key, &got, &found, &error)) {
    Fail("Get sum tree parent failed: " + error);
  }
  if (!found) {
    Fail("sum tree parent should exist");
  }
  uint64_t sum_tree_variant = 0;
  if (!grovedb::DecodeElementVariant(got, &sum_tree_variant, &error)) {
    Fail("DecodeElementVariant(sum tree parent) failed: " + error);
  }
  if (sum_tree_variant != 4) {
    Fail("sum tree parent variant mismatch");
  }
  std::vector<uint8_t> sum_tree_flags;
  if (!grovedb::ExtractFlagsFromElementBytes(got, &sum_tree_flags, &error)) {
    Fail("ExtractFlagsFromElementBytes(sum tree parent) failed: " + error);
  }
  if (sum_tree_flags != std::vector<uint8_t>({'f', 'g'})) {
    Fail("sum tree parent flags should be preserved");
  }

  // CountTree parent should refresh count from child aggregate on mutation.
  const std::vector<uint8_t> count_parent_key = {'c', 'n', 't', 'p'};
  const std::vector<uint8_t> flagged_count_tree = {0x06, 0x00, 0x00, 0x01, 0x02, 'c', 'f'};
  if (!db.Insert({}, count_parent_key, flagged_count_tree, &error)) {
    Fail("Insert count tree parent failed: " + error);
  }
  if (!db.Insert({count_parent_key}, {'c', '1'}, item_element, &error)) {
    Fail("Insert under count tree parent failed: " + error);
  }
  if (!db.Get({}, count_parent_key, &got, &found, &error)) {
    Fail("Get count tree parent after insert failed: " + error);
  }
  if (!found) {
    Fail("count tree parent should exist after insert");
  }
  uint64_t count_tree_count = 0;
  if (!DecodeCountTreeCount(got, &count_tree_count)) {
    Fail("DecodeCountTreeCount after insert failed");
  }
  if (count_tree_count != 1) {
    Fail("count tree parent count should be 1 after one child insert, got " +
         std::to_string(count_tree_count));
  }
  bool deleted_count_child = false;
  if (!db.Delete({count_parent_key}, {'c', '1'}, &deleted_count_child, &error)) {
    Fail("Delete under count tree parent failed: " + error);
  }
  if (!deleted_count_child) {
    Fail("Delete under count tree parent should report deleted=true");
  }
  if (!db.Get({}, count_parent_key, &got, &found, &error)) {
    Fail("Get count tree parent after delete failed: " + error);
  }
  if (!found) {
    Fail("count tree parent should exist after child delete");
  }
  if (!DecodeCountTreeCount(got, &count_tree_count)) {
    Fail("DecodeCountTreeCount after delete failed");
  }
  if (count_tree_count != 0) {
    Fail("count tree parent count should return to 0 after deleting only child, got " +
         std::to_string(count_tree_count));
  }

  // CountTree parent should also refresh count via tx-batch insert/delete path.
  {
    const std::vector<uint8_t> tx_cnt_key = {'t', 'c', 'n', 't'};
    const std::vector<uint8_t> flagged_ct = {0x06, 0x00, 0x00, 0x01, 0x02, 't', 'f'};
    if (!db.Insert({}, tx_cnt_key, flagged_ct, &error)) {
      Fail("Insert count tree for tx test failed: " + error);
    }
    grovedb::GroveDb::Transaction tx;
    if (!db.StartTransaction(&tx, &error)) {
      Fail("StartTransaction for count tx test failed: " + error);
    }
    if (!db.Insert({tx_cnt_key}, {'t', '1'}, item_element, &tx, &error)) {
      Fail("Insert under count tree in tx failed: " + error);
    }
    if (!db.CommitTransaction(&tx, &error)) {
      Fail("CommitTransaction count tx test failed: " + error);
    }
    std::vector<uint8_t> cnt_got;
    bool cnt_found = false;
    if (!db.Get({}, tx_cnt_key, &cnt_got, &cnt_found, &error)) {
      Fail("Get count tree parent after tx insert: " + error);
    }
    if (!cnt_found) {
      Fail("count tree parent should exist after tx insert");
    }
    uint64_t tx_count = 0;
    if (!DecodeCountTreeCount(cnt_got, &tx_count)) {
      Fail("DecodeCountTreeCount after tx insert failed");
    }
    if (tx_count != 1) {
      Fail("count tree parent count should be 1 after tx insert, got " +
           std::to_string(tx_count));
    }
    // Delete in tx path
    grovedb::GroveDb::Transaction tx2;
    if (!db.StartTransaction(&tx2, &error)) {
      Fail("StartTransaction for count tx delete test failed: " + error);
    }
    bool del_tx = false;
    if (!db.Delete({tx_cnt_key}, {'t', '1'}, &del_tx, &tx2, &error)) {
      Fail("Delete under count tree in tx failed: " + error);
    }
    if (!del_tx) {
      Fail("Delete under count tree in tx should report deleted=true");
    }
    if (!db.CommitTransaction(&tx2, &error)) {
      Fail("CommitTransaction count tx delete test failed: " + error);
    }
    if (!db.Get({}, tx_cnt_key, &cnt_got, &cnt_found, &error)) {
      Fail("Get count tree parent after tx delete: " + error);
    }
    if (!cnt_found) {
      Fail("count tree parent should exist after tx delete");
    }
    if (!DecodeCountTreeCount(cnt_got, &tx_count)) {
      Fail("DecodeCountTreeCount after tx delete failed");
    }
    if (tx_count != 0) {
      Fail("count tree parent count should return to 0 after tx delete, got " +
           std::to_string(tx_count));
    }
  }

  // ProvableCountTree parent should refresh count from child aggregate on mutation.
  {
    const std::vector<uint8_t> prov_count_parent_key = {'p', 'c', 'n', 't'};
    const std::vector<uint8_t> flagged_prov_count_tree = {0x08, 0x00, 0x09, 0x01, 0x02, 'p', 'f'};
    if (!db.Insert({}, prov_count_parent_key, flagged_prov_count_tree, &error)) {
      Fail("Insert provable count tree parent failed: " + error);
    }
    if (!db.Insert({prov_count_parent_key}, {'p', '1'}, item_element, &error)) {
      Fail("Insert under provable count tree parent failed: " + error);
    }
    std::vector<uint8_t> prov_cnt_got;
    bool prov_cnt_found = false;
    if (!db.Get({}, prov_count_parent_key, &prov_cnt_got, &prov_cnt_found, &error)) {
      Fail("Get provable count tree parent after insert failed: " + error);
    }
    if (!prov_cnt_found) {
      Fail("provable count tree parent should exist after insert");
    }
    uint64_t prov_count = 0;
    if (!DecodeProvableCountTreeCount(prov_cnt_got, &prov_count)) {
      Fail("DecodeProvableCountTreeCount after insert failed");
    }
    if (prov_count != 1) {
      Fail("provable count tree parent count should be 1 after one child insert, got " +
           std::to_string(prov_count));
    }

    bool deleted_prov_count_child = false;
    if (!db.Delete({prov_count_parent_key}, {'p', '1'}, &deleted_prov_count_child, &error)) {
      Fail("Delete under provable count tree parent failed: " + error);
    }
    if (!deleted_prov_count_child) {
      Fail("Delete under provable count tree parent should report deleted=true");
    }
    if (!db.Get({}, prov_count_parent_key, &prov_cnt_got, &prov_cnt_found, &error)) {
      Fail("Get provable count tree parent after delete failed: " + error);
    }
    if (!prov_cnt_found) {
      Fail("provable count tree parent should exist after child delete");
    }
    if (!DecodeProvableCountTreeCount(prov_cnt_got, &prov_count)) {
      Fail("DecodeProvableCountTreeCount after delete failed");
    }
    if (prov_count != 0) {
      Fail("provable count tree parent count should return to 0 after deleting only child, got " +
           std::to_string(prov_count));
    }
  }

  // Reference elements should round-trip as element bytes.
  grovedb::ElementReference reference;
  reference.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
  reference.reference_path.path = {{'l', 'e', 'a', 'f', '1'}};
  reference.reference_path.key = {'k', '2'};
  std::vector<uint8_t> reference_element;
  if (!grovedb::EncodeReferenceToElementBytes(reference, &reference_element, &error)) {
    Fail("EncodeReferenceToElementBytes failed: " + error);
  }
  if (!db.Insert({leaf_key}, {'r', '1'}, reference_element, &error)) {
    Fail("Insert reference element failed: " + error);
  }
  if (!db.GetRaw({leaf_key}, {'r', '1'}, &got, &found, &error)) {
    Fail("GetRaw reference element failed: " + error);
  }
  if (!found || got != reference_element) {
    Fail("GetRaw reference element mismatch");
  }

  if (!db.Get({leaf_key}, {'k', '1'}, &got, &found, &error)) {
    Fail("Get child item failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Get child item mismatch");
  }

  const std::vector<uint8_t> helper_tree_key = {'h', 't', 'r', 'e', 'e'};
  if (!db.InsertEmptyTree({}, helper_tree_key, &error)) {
    Fail("InsertEmptyTree helper failed: " + error);
  }
  if (!db.InsertItem({helper_tree_key}, {'h', 'i'}, {'h', 'v'}, &error)) {
    Fail("InsertItem helper failed: " + error);
  }
  if (!db.Get({helper_tree_key}, {'h', 'i'}, &got, &found, &error)) {
    Fail("Get helper-inserted item failed: " + error);
  }
  if (!found) {
    Fail("helper-inserted item should exist");
  }
  grovedb::ElementItem helper_item;
  if (!grovedb::DecodeItemFromElementBytes(got, &helper_item, &error)) {
    Fail("Decode helper item failed: " + error);
  }
  if (helper_item.value != std::vector<uint8_t>({'h', 'v'})) {
    Fail("helper item value mismatch");
  }
  bool inserted = false;
  if (!db.InsertIfNotExists({helper_tree_key}, {'h', 'i'}, item_element, &inserted, &error)) {
    Fail("InsertIfNotExists(existing) failed: " + error);
  }
  if (inserted) {
    Fail("InsertIfNotExists should report inserted=false for existing key");
  }
  std::vector<uint8_t> helper_item2_element;
  if (!grovedb::EncodeItemToElementBytes({'h', '2'}, &helper_item2_element, &error)) {
    Fail("Encode helper item2 failed: " + error);
  }
  if (!db.InsertIfNotExists({helper_tree_key}, {'h', '2'}, helper_item2_element, &inserted, &error)) {
    Fail("InsertIfNotExists(new) failed: " + error);
  }
  if (!inserted) {
    Fail("InsertIfNotExists should report inserted=true for new key");
  }
  if (!db.Get({helper_tree_key}, {'h', '2'}, &got, &found, &error)) {
    Fail("Get helper InsertIfNotExists key failed: " + error);
  }
  if (!found || got != helper_item2_element) {
    Fail("InsertIfNotExists inserted key mismatch");
  }
  bool changed = false;
  bool had_previous = false;
  std::vector<uint8_t> previous_element;
  if (!db.InsertIfChangedValue({helper_tree_key},
                               {'h', '2'},
                               helper_item2_element,
                               &changed,
                               &previous_element,
                               &had_previous,
                               &error)) {
    Fail("InsertIfChangedValue(same value) failed: " + error);
  }
  if (changed || had_previous) {
    Fail("InsertIfChangedValue(same value) should report changed=false and no previous element");
  }
  std::vector<uint8_t> helper_item3_element;
  if (!grovedb::EncodeItemToElementBytes({'h', '3'}, &helper_item3_element, &error)) {
    Fail("Encode helper item3 failed: " + error);
  }
  if (!db.InsertIfChangedValue({helper_tree_key},
                               {'h', '2'},
                               helper_item3_element,
                               &changed,
                               &previous_element,
                               &had_previous,
                               &error)) {
    Fail("InsertIfChangedValue(changed value) failed: " + error);
  }
  if (!changed || !had_previous || previous_element != helper_item2_element) {
    Fail("InsertIfChangedValue(changed value) should return previous element");
  }
  std::vector<uint8_t> helper_item4_element;
  if (!grovedb::EncodeItemToElementBytes({'h', '4'}, &helper_item4_element, &error)) {
    Fail("Encode helper item4 failed: " + error);
  }
  if (!db.InsertIfChangedValue({helper_tree_key},
                               {'h', '4'},
                               helper_item4_element,
                               &changed,
                               &previous_element,
                               &had_previous,
                               &error)) {
    Fail("InsertIfChangedValue(new key) failed: " + error);
  }
  if (!changed || had_previous) {
    Fail("InsertIfChangedValue(new key) should report changed=true and no previous element");
  }
  bool had_existing = false;
  std::vector<uint8_t> existing_element;
  if (!db.InsertIfNotExistsReturnExisting(
          {helper_tree_key}, {'h', '2'}, helper_item4_element, &existing_element, &had_existing, &error)) {
    Fail("InsertIfNotExistsReturnExisting(existing) failed: " + error);
  }
  if (!had_existing || existing_element != helper_item3_element) {
    Fail("InsertIfNotExistsReturnExisting(existing) should return previous value");
  }
  std::vector<uint8_t> helper_item5_element;
  if (!grovedb::EncodeItemToElementBytes({'h', '5'}, &helper_item5_element, &error)) {
    Fail("Encode helper item5 failed: " + error);
  }
  if (!db.InsertIfNotExistsReturnExisting(
          {helper_tree_key}, {'h', '5'}, helper_item5_element, &existing_element, &had_existing, &error)) {
    Fail("InsertIfNotExistsReturnExisting(new) failed: " + error);
  }
  if (had_existing) {
    Fail("InsertIfNotExistsReturnExisting(new) should report had_existing=false");
  }
  if (!db.Get({helper_tree_key}, {'h', '5'}, &got, &found, &error)) {
    Fail("Get helper InsertIfNotExistsReturnExisting key failed: " + error);
  }
  if (!found || got != helper_item5_element) {
    Fail("InsertIfNotExistsReturnExisting inserted key mismatch");
  }

  if (!db.Has({leaf_key}, {'k', '1'}, &found, &error)) {
    Fail("Has child item failed: " + error);
  }
  if (!found) {
    Fail("Has child item should return found=true");
  }

  bool deleted = false;
  if (!db.Delete({leaf_key}, {'k', '1'}, &deleted, &error)) {
    Fail("Delete child item failed: " + error);
  }
  if (!deleted) {
    Fail("Delete should report deleted=true");
  }
  if (!db.Get({leaf_key}, {'k', '1'}, &got, &found, &error)) {
    Fail("Get after delete failed: " + error);
  }
  if (found) {
    Fail("Deleted key should not be found");
  }
  if (!db.Has({leaf_key}, {'k', '1'}, &found, &error)) {
    Fail("Has after delete failed: " + error);
  }
  if (found) {
    Fail("Has after delete should return found=false");
  }

  // delete_if_empty_tree: non-empty tree returns deleted=false; empty tree deletes.
  const std::vector<uint8_t> deletable_tree = {'d', 'e', 'l', 't'};
  if (!db.Insert({}, deletable_tree, tree_element, &error)) {
    Fail("Insert delete-if-empty root tree failed: " + error);
  }
  if (!db.Insert({deletable_tree}, {'c'}, item_element, &error)) {
    Fail("Insert delete-if-empty child item failed: " + error);
  }
  bool deleted_if_empty = false;
  if (!db.DeleteIfEmptyTree({}, deletable_tree, &deleted_if_empty, &error)) {
    Fail("DeleteIfEmptyTree(non-empty) failed: " + error);
  }
  if (deleted_if_empty) {
    Fail("DeleteIfEmptyTree(non-empty) should report deleted=false");
  }
  if (!db.Get({}, deletable_tree, &got, &found, &error)) {
    Fail("Get delete-if-empty tree after non-empty check failed: " + error);
  }
  if (!found) {
    Fail("DeleteIfEmptyTree(non-empty) should keep tree");
  }
  if (!db.Delete({deletable_tree}, {'c'}, &deleted, &error)) {
    Fail("Delete delete-if-empty child item failed: " + error);
  }
  if (!deleted) {
    Fail("Delete delete-if-empty child item should report deleted=true");
  }
  if (!db.DeleteIfEmptyTree({}, deletable_tree, &deleted_if_empty, &error)) {
    Fail("DeleteIfEmptyTree(empty) failed: " + error);
  }
  if (!deleted_if_empty) {
    Fail("DeleteIfEmptyTree(empty) should report deleted=true");
  }
  if (!db.Get({}, deletable_tree, &got, &found, &error)) {
    Fail("Get delete-if-empty tree after deletion failed: " + error);
  }
  if (found) {
    Fail("DeleteIfEmptyTree(empty) should remove tree");
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed: " + error);
  }
  if (!db.Insert({leaf_key}, {'k', '2'}, item_element, &tx, &error)) {
    Fail("Insert in transaction failed: " + error);
  }
  if (!db.CommitTransaction(&tx, &error)) {
    Fail("CommitTransaction failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error)) {
    Fail("Get after commit failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Committed key mismatch");
  }

  // Rust-style tx path: apply multiple mutations under same subtree in one tx.
  const std::vector<uint8_t> txm_key = {'t', 'x', 'm'};
  if (!db.Insert({}, txm_key, tree_element, &error)) {
    Fail("Insert txm subtree failed: " + error);
  }
  if (!db.Insert({txm_key}, {'k', '1'}, item_element, &error)) {
    Fail("Insert txm k1 failed: " + error);
  }
  grovedb::GroveDb::Transaction tx_mixed;
  if (!db.StartTransaction(&tx_mixed, &error)) {
    Fail("StartTransaction(mixed) failed: " + error);
  }
  if (!db.Insert({txm_key}, {'k', 'x'}, item_element, &tx_mixed, &error)) {
    Fail("Mixed tx insert failed: " + error);
  }
  if (!db.Get({txm_key}, {'k', 'x'}, &got, &found, &tx_mixed, &error)) {
    Fail("Mixed tx get after insert failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Mixed tx inserted key mismatch right after insert");
  }
  bool mixed_deleted = false;
  if (!db.Delete({txm_key}, {'k', '1'}, &mixed_deleted, &tx_mixed, &error)) {
    Fail("Mixed tx delete failed: " + error);
  }
  if (!mixed_deleted) {
    Fail("Mixed tx delete should report deleted=true");
  }
  if (!db.Get({txm_key}, {'k', 'x'}, &got, &found, &tx_mixed, &error)) {
    Fail("Mixed tx get inserted key failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Mixed tx inserted key mismatch");
  }
  if (!db.Get({txm_key}, {'k', '1'}, &got, &found, &tx_mixed, &error)) {
    Fail("Mixed tx get deleted key failed: " + error);
  }
  if (found) {
    Fail("Mixed tx deleted key should be absent inside tx");
  }
  if (!db.CommitTransaction(&tx_mixed, &error)) {
    Fail("CommitTransaction(mixed) failed: " + error);
  }
  if (!db.Get({txm_key}, {'k', 'x'}, &got, &found, &error)) {
    Fail("Mixed tx get inserted key after commit failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Mixed tx inserted key missing after commit");
  }
  if (!db.Get({txm_key}, {'k', '1'}, &got, &found, &error)) {
    Fail("Mixed tx get deleted key after commit failed: " + error);
  }
  if (found) {
    Fail("Mixed tx deleted key should stay absent after commit");
  }

  // Range query over subtree keys.
  if (!db.Insert({leaf_key}, {'k', '4'}, item_element, &error)) {
    Fail("Insert k4 failed: " + error);
  }
  if (!db.Insert({leaf_key}, {'k', '5'}, item_element, &error)) {
    Fail("Insert k5 failed: " + error);
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> range_out;
  if (!db.QueryRange({leaf_key},
                     {'k', '2'},
                     {'k', '5'},
                     true,
                     false,
                     &range_out,
                     &error)) {
    Fail("QueryRange failed: " + error);
  }
  if (range_out.size() != 2) {
    Fail("QueryRange expected two keys");
  }
  if (range_out[0].first != std::vector<uint8_t>({'k', '2'}) ||
      range_out[1].first != std::vector<uint8_t>({'k', '4'})) {
    Fail("QueryRange returned unexpected keys");
  }
  const std::vector<uint8_t> query_raw_tree = {'q', 'r'};
  if (!db.Insert({}, query_raw_tree, tree_element, &error)) {
    Fail("Insert query_raw tree failed: " + error);
  }
  if (!db.InsertItem({query_raw_tree}, {'t', '1'}, {'v', '1'}, &error)) {
    Fail("Insert query_raw target item failed: " + error);
  }
  grovedb::ReferencePathType query_raw_reference;
  query_raw_reference.kind = grovedb::ReferencePathKind::kSibling;
  query_raw_reference.key = {'t', '1'};
  if (!db.InsertReference({query_raw_tree}, {'r', '1'}, query_raw_reference, &error)) {
    Fail("InsertReference for QueryRaw test failed: " + error);
  }
  grovedb::PathQuery query_raw_single =
      grovedb::PathQuery::NewSingleKey({query_raw_tree}, {'r', '1'});
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> query_raw_out;
  if (!db.QueryRaw(query_raw_single, &query_raw_out, &error)) {
    Fail("QueryRaw failed: " + error);
  }
  if (query_raw_out.size() != 1 ||
      query_raw_out[0].first != std::vector<uint8_t>({'r', '1'})) {
    Fail("QueryRaw expected one reference key");
  }
  uint64_t query_raw_variant = 0;
  if (!grovedb::DecodeElementVariant(query_raw_out[0].second, &query_raw_variant, &error)) {
    Fail("QueryRaw decode variant failed: " + error);
  }
  if (query_raw_variant != 1) {
    Fail("QueryRaw should return raw reference element bytes");
  }

  // Facade batch API: local atomic batch applies all-or-nothing.
  const std::vector<uint8_t> batch_tree = {'b', 'a', 't', 'c', 'h'};
  std::vector<grovedb::GroveDb::BatchOp> batch_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, batch_tree, tree_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {batch_tree}, {'k', '2'}, item_element},
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {batch_tree}, {'k', 'm', 'i', 's', 's'}, {}}};
  if (!db.ApplyBatch(batch_ops, &error)) {
    Fail("ApplyBatch(local) failed: " + error);
  }
  if (!db.Get({batch_tree}, {'k', 'm', 'i', 's', 's'}, &got, &found, &error)) {
    Fail("Get batch deleted-missing key failed: " + error);
  }
  if (found) {
    Fail("ApplyBatch delete of missing key should remain absent");
  }
  if (!db.Get({batch_tree}, {'k', '2'}, &got, &found, &error)) {
    Fail("Get batch k2 failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("ApplyBatch insert did not persist");
  }

  // Atomicity check: failed local batch must rollback earlier successful ops.
  const std::vector<uint8_t> non_tree_parent = {'n', 'o', 'n', 't', 'r', 'e', 'e'};
  std::vector<grovedb::GroveDb::BatchOp> failing_batch_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, non_tree_parent, item_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {non_tree_parent}, {'k', 'x'}, item_element}};
  if (db.ApplyBatch(failing_batch_ops, &error)) {
    Fail("ApplyBatch(local) should fail on non-tree path component");
  }
  if (!db.Get({}, non_tree_parent, &got, &found, &error)) {
    Fail("Get non_tree_parent after failed batch failed: " + error);
  }
  if (found) {
    Fail("Failed local batch should rollback first operation");
  }

  // External tx batch path: writes are visible in tx before commit and persist after commit.
  grovedb::GroveDb::Transaction tx_batch;
  if (!db.StartTransaction(&tx_batch, &error)) {
    Fail("StartTransaction(batch) failed: " + error);
  }
  const std::vector<uint8_t> tx_batch_tree = {'t', 'x', 'b'};
  std::vector<grovedb::GroveDb::BatchOp> tx_batch_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, tx_batch_tree, tree_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {tx_batch_tree}, {'k', 't'}, item_element}};
  if (!db.ApplyBatch(tx_batch_ops, &tx_batch, &error)) {
    Fail("ApplyBatch(tx) failed: " + error);
  }
  if (!db.Get({tx_batch_tree}, {'k', 't'}, &got, &found, &tx_batch, &error)) {
    Fail("Get batch tx item in tx failed: " + error);
  }
  if (!found) {
    Fail("ApplyBatch(tx) value should be visible in tx");
  }
  if (!db.Get({}, tx_batch_tree, &got, &found, &error)) {
    Fail("Get batch tx subtree out of tx failed: " + error);
  }
  if (found) {
    Fail("ApplyBatch(tx) subtree should not be visible before commit");
  }
  if (!db.CommitTransaction(&tx_batch, &error)) {
    Fail("CommitTransaction(batch) failed: " + error);
  }
  if (!db.Get({tx_batch_tree}, {'k', 't'}, &got, &found, &error)) {
    Fail("Get batch tx item after commit failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("ApplyBatch(tx) value mismatch after commit");
  }

  // InsertOnly/Replace semantics for batch ops.
  std::vector<uint8_t> replace_value;
  if (!grovedb::EncodeItemToElementBytes({'r', '2'}, &replace_value, &error)) {
    Fail("Encode replace value failed: " + error);
  }
  if (!db.Insert({leaf_key}, {'k', 'r'}, item_element, &error)) {
    Fail("Insert replace test baseline key failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> insert_only_success = {
      {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {leaf_key}, {'k', 'i', 'o'}, item_element}};
  if (!db.ApplyBatch(insert_only_success, &error)) {
    Fail("ApplyBatch(insert_only success) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'i', 'o'}, &got, &found, &error) || !found) {
    Fail("insert_only success did not persist");
  }

  std::vector<grovedb::GroveDb::BatchOp> insert_only_fail = {
      {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {leaf_key}, {'k', 'i', 'o'}, replace_value}};
  if (!db.ApplyBatch(insert_only_fail, &error)) {
    Fail("ApplyBatch(insert_only existing-key upsert) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'i', 'o'}, &got, &found, &error) || !found || got != replace_value) {
    Fail("insert_only existing-key upsert mismatch");
  }

  std::vector<grovedb::GroveDb::BatchOp> replace_success = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace, {leaf_key}, {'k', 'r'}, replace_value}};
  if (!db.ApplyBatch(replace_success, &error)) {
    Fail("ApplyBatch(replace success) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'r'}, &got, &found, &error) || !found || got != replace_value) {
    Fail("replace success did not persist updated value");
  }

  std::vector<grovedb::GroveDb::BatchOp> replace_fail = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace, {leaf_key}, {'k', 'm', 'i', 's', 's'}, replace_value}};
  if (!db.ApplyBatch(replace_fail, &error)) {
    Fail("ApplyBatch(replace missing-key upsert) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'm', 'i', 's', 's'}, &got, &found, &error) || !found ||
      got != replace_value) {
    Fail("replace missing-key upsert did not persist");
  }

  // Patch semantics mirror Rust batch patch operation behavior in this facade
  // slice: patch acts like insert-or-replace for item payload updates.
  std::vector<uint8_t> patch_value_existing;
  if (!grovedb::EncodeItemToElementBytes({'p', '1'}, &patch_value_existing, &error)) {
    Fail("Encode patch existing value failed: " + error);
  }
  std::vector<uint8_t> patch_value_missing;
  if (!grovedb::EncodeItemToElementBytes({'p', '2'}, &patch_value_missing, &error)) {
    Fail("Encode patch missing value failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> patch_existing = {
      {grovedb::GroveDb::BatchOp::Kind::kPatch, {leaf_key}, {'k', 'r'}, patch_value_existing}};
  if (!db.ApplyBatch(patch_existing, &error)) {
    Fail("ApplyBatch(patch existing) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'r'}, &got, &found, &error) || !found ||
      got != patch_value_existing) {
    Fail("patch existing-key update did not persist");
  }
  std::vector<grovedb::GroveDb::BatchOp> patch_missing = {
      {grovedb::GroveDb::BatchOp::Kind::kPatch, {leaf_key}, {'k', 'p', 'm'}, patch_value_missing}};
  if (!db.ApplyBatch(patch_missing, &error)) {
    Fail("ApplyBatch(patch missing-key upsert) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'p', 'm'}, &got, &found, &error) || !found ||
      got != patch_value_missing) {
    Fail("patch missing-key upsert did not persist");
  }
  std::vector<uint8_t> patch_value_delta;
  if (!grovedb::EncodeItemToElementBytes({'p', '3'}, &patch_value_delta, &error)) {
    Fail("Encode patch delta value failed: " + error);
  }
  grovedb::GroveDb::BatchOp patch_with_delta;
  patch_with_delta.kind = grovedb::GroveDb::BatchOp::Kind::kPatch;
  patch_with_delta.path = {leaf_key};
  patch_with_delta.key = {'k', 'p', 'd'};
  patch_with_delta.element_bytes = patch_value_delta;
  patch_with_delta.change_in_bytes = -3;
  if (!db.ApplyBatch({patch_with_delta}, &error)) {
    Fail("ApplyBatch(patch with change_in_bytes) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'p', 'd'}, &got, &found, &error) || !found ||
      got != patch_value_delta) {
    Fail("patch with change_in_bytes did not persist");
  }

  // ValidateBatch is a dry-run and must not persist changes.
  std::vector<grovedb::GroveDb::BatchOp> validate_success_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', 'v', 'a', 'l'}, item_element},
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, {'k', '2'}, {}}};
  if (!db.ValidateBatch(validate_success_ops, &error)) {
    Fail("ValidateBatch(success) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'v', 'a', 'l'}, &got, &found, &error)) {
    Fail("Get validate key failed: " + error);
  }
  if (found) {
    Fail("ValidateBatch(success) should not persist inserted key");
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error)) {
    Fail("Get original key after ValidateBatch(success) failed: " + error);
  }
  if (!found) {
    Fail("ValidateBatch(success) should not delete existing key");
  }

  std::vector<grovedb::GroveDb::BatchOp> validate_fail_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'n', 'v', 'n', 't'}, item_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'n', 'v', 'n', 't'}}, {'k', 'x'}, item_element}};
  if (db.ValidateBatch(validate_fail_ops, &error)) {
    Fail("ValidateBatch(fail) should return false");
  }
  if (!db.Get({}, {'n', 'v', 'n', 't'}, &got, &found, &error)) {
    Fail("Get non-tree key after ValidateBatch(fail) failed: " + error);
  }
  if (found) {
    Fail("ValidateBatch(fail) should not persist partial writes");
  }

  // EstimatedCaseOperationsForBatch is a dry-run estimate and must not persist
  // batch writes.
  grovedb::OperationCost estimated_batch_cost;
  std::vector<grovedb::GroveDb::BatchOp> estimate_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert,
       {leaf_key},
       {'k', 'e', 's', 't'},
       item_element},
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, {'k', '2'}, {}}};
  if (!db.EstimatedCaseOperationsForBatch(estimate_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch failed: " + error);
  }
  if (estimated_batch_cost.seek_count == 0 && estimated_batch_cost.storage_loaded_bytes == 0 &&
      estimated_batch_cost.storage_cost.added_bytes == 0 &&
      estimated_batch_cost.storage_cost.replaced_bytes == 0 &&
      estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
    Fail("EstimatedCaseOperationsForBatch should report non-zero cost");
  }
  if (!db.Get({leaf_key}, {'k', 'e', 's', 't'}, &got, &found, &error)) {
    Fail("Get estimated key failed: " + error);
  }
  if (found) {
    Fail("EstimatedCaseOperationsForBatch should not persist inserted key");
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error)) {
    Fail("Get delete target after EstimatedCaseOperationsForBatch failed: " + error);
  }
  if (!found) {
    Fail("EstimatedCaseOperationsForBatch should not delete existing key");
  }

  // Strict validation option: reject overrides for insert-like operations.
  grovedb::GroveDb::BatchApplyOptions strict_options;
  strict_options.validate_insertion_does_not_override = true;
  std::vector<uint8_t> strict_before;
  bool strict_before_found = false;
  if (!db.Get({leaf_key}, {'k', '2'}, &strict_before, &strict_before_found, &error) || !strict_before_found) {
    Fail("Strict options setup should find existing key");
  }
  std::vector<grovedb::GroveDb::BatchOp> strict_insert_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', '2'}, item_element}};
  if (db.ApplyBatch(strict_insert_ops, strict_options, &error)) {
    Fail("ApplyBatch(strict options) should fail on existing key override");
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error) || !found || got != strict_before) {
    Fail("Strict options failure should preserve existing value");
  }
  if (db.EstimatedCaseOperationsForBatch(strict_insert_ops, strict_options, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(strict options) should fail on existing key override");
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error) || !found || got != strict_before) {
    Fail("Strict estimate failure should preserve existing value");
  }
  std::vector<grovedb::GroveDb::BatchOp> strict_patch_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kPatch, {leaf_key}, {'k', '2'}, patch_value_existing}};
  if (db.ApplyBatch(strict_patch_ops, strict_options, &error)) {
    Fail("ApplyBatch(strict options patch) should fail on existing key override");
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &error) || !found || got != strict_before) {
    Fail("Strict options patch failure should preserve existing value");
  }

  std::vector<grovedb::GroveDb::BatchOp> strict_new_insert_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', 's', 't', 'r'}, item_element}};
  if (!db.ApplyBatch(strict_new_insert_ops, strict_options, &error)) {
    Fail("ApplyBatch(strict options new key) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 's', 't', 'r'}, &got, &found, &error) || !found ||
      got != item_element) {
    Fail("Strict options new-key insert mismatch");
  }

  // Strict options on ValidateBatch should mirror apply checks but never persist.
  std::vector<grovedb::GroveDb::BatchOp> strict_validate_fail_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', '2'}, item_element}};
  if (db.ValidateBatch(strict_validate_fail_ops, strict_options, &error)) {
    Fail("ValidateBatch(strict options) should fail on existing key override");
  }
  std::vector<grovedb::GroveDb::BatchOp> strict_validate_success_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', 'v', 's'}, item_element}};
  if (!db.ValidateBatch(strict_validate_success_ops, strict_options, &error)) {
    Fail("ValidateBatch(strict options new key) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'v', 's'}, &got, &found, &error)) {
    Fail("Get strict validate key failed: " + error);
  }
  if (found) {
    Fail("ValidateBatch(strict options) should not persist inserted key");
  }

  // Mid-batch strict validation failure must rollback earlier successful ops.
  std::vector<uint8_t> strict_atomicity_existing = {'k', 's', 'a', 'f', 'e'};
  std::vector<uint8_t> strict_atomicity_first = {'k', 's', 'a', 'f', 'e', '2'};
  if (!db.Insert({leaf_key}, strict_atomicity_existing, item_element, &error)) {
    Fail("Insert strict atomicity existing key failed: " + error);
  }
  if (!db.Get({leaf_key}, strict_atomicity_first, &got, &found, &error)) {
    Fail("Get strict atomicity first key before batch failed: " + error);
  }
  if (found) {
    Fail("Strict atomicity first key should not exist before batch");
  }
  std::vector<grovedb::GroveDb::BatchOp> strict_atomicity_batch = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, strict_atomicity_first, item_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsertOnly, {leaf_key}, strict_atomicity_existing, item_element}};
  error.clear();
  if (db.ApplyBatch(strict_atomicity_batch, strict_options, &error)) {
    Fail("ApplyBatch(strict options mid-batch failure) should fail");
  }
  if (!db.Get({leaf_key}, strict_atomicity_first, &got, &found, &error)) {
    Fail("Get strict atomicity first key after failed batch failed: " + error);
  }
  if (found) {
    Fail("Strict options mid-batch failure should rollback earlier insert");
  }
  if (!db.Get({leaf_key}, strict_atomicity_existing, &got, &found, &error) || !found ||
      got != item_element) {
    Fail("Strict options mid-batch failure should preserve existing key");
  }

  // Rust parity: validate_insertion_does_not_override_tree rejects tree
  // overrides but allows non-tree overrides.
  grovedb::GroveDb::BatchApplyOptions strict_tree_options;
  strict_tree_options.validate_insertion_does_not_override_tree = true;
  // Overriding a Tree element should fail.
  std::vector<grovedb::GroveDb::BatchOp> strict_tree_tree_override_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, leaf_key, item_element}};
  if (db.ApplyBatch(strict_tree_tree_override_ops, strict_tree_options, &error)) {
    Fail("ApplyBatch(strict tree options tree override) should fail");
  }
  // Overriding an Item element should succeed.
  std::vector<uint8_t> strict_item_update;
  if (!grovedb::EncodeItemToElementBytes({'t', 'v'}, &strict_item_update, &error)) {
    Fail("Encode strict tree item update failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> strict_tree_item_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, {'k', 'r'}, strict_item_update}};
  if (!db.ApplyBatch(strict_tree_item_ops, strict_tree_options, &error)) {
    Fail("ApplyBatch(strict tree options item update) failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', 'r'}, &got, &found, &error) || !found || got != strict_item_update) {
    Fail("Strict tree options should allow overriding existing item");
  }

  // Rust batch parity: consistency checks reject duplicate same-path/key ops
  // unless explicitly disabled in BatchApplyOptions.
  std::vector<uint8_t> consistency_key = {'k', 'c'};
  if (!db.Insert({leaf_key}, consistency_key, item_element, &error)) {
    Fail("Insert consistency-check key failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> duplicate_delete_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, consistency_key, {}},
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, consistency_key, {}}};
  if (db.ApplyBatch(duplicate_delete_ops, &error)) {
    Fail("ApplyBatch should fail consistency checks for duplicate same-path/key ops");
  }
  if (!db.Get({leaf_key}, consistency_key, &got, &found, &error) || !found) {
    Fail("Consistency-check failure should preserve original key");
  }
  grovedb::GroveDb::BatchApplyOptions disable_consistency_options;
  disable_consistency_options.disable_operation_consistency_check = true;
  if (!db.ApplyBatch(duplicate_delete_ops, disable_consistency_options, &error)) {
    Fail("ApplyBatch with consistency check disabled should succeed: " + error);
  }
  if (!db.Get({leaf_key}, consistency_key, &got, &found, &error)) {
    Fail("Get consistency-check key after disabled-check batch failed: " + error);
  }
  if (found) {
    Fail("Consistency-check-disabled batch should apply duplicate deletes");
  }

  // Rust batch parity: conflicting same-path/key ops are rejected by
  // consistency checks even when kinds differ (e.g. delete + insert).
  std::vector<uint8_t> conflict_key = {'k', 'x'};
  std::vector<uint8_t> conflict_insert_value;
  if (!grovedb::EncodeItemToElementBytes({'x', '2'}, &conflict_insert_value, &error)) {
    Fail("Encode conflict insert value failed: " + error);
  }
  if (!db.Insert({leaf_key}, conflict_key, item_element, &error)) {
    Fail("Insert conflict baseline key failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> delete_insert_same_key_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, conflict_key, {}},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {leaf_key}, conflict_key, conflict_insert_value}};
  error.clear();
  if (db.ApplyBatch(delete_insert_same_key_ops, &error)) {
    Fail("ApplyBatch should fail consistency checks for delete+insert same key");
  }
  if (error != "batch operations fail consistency checks") {
    Fail("delete+insert same key should fail consistency checks: " + error);
  }
  if (!db.Get({leaf_key}, conflict_key, &got, &found, &error) || !found || got != item_element) {
    Fail("delete+insert consistency failure should preserve original key");
  }

  // With consistency checks disabled, Rust canonicalizes by (path,key) and
  // keeps the last op for duplicates.
  error.clear();
  if (!db.ApplyBatch(delete_insert_same_key_ops, disable_consistency_options, &error)) {
    Fail("ApplyBatch with checks disabled should allow delete+insert same key: " + error);
  }
  if (!db.Get({leaf_key}, conflict_key, &got, &found, &error) || !found ||
      got != conflict_insert_value) {
    Fail("delete+insert with checks disabled should keep the last op for same key");
  }

  // Another same-key mixed-op conflict: patch + replace should also be blocked
  // by consistency checks and preserve state.
  std::vector<uint8_t> patch_replace_key = {'k', 'p', 'r'};
  std::vector<uint8_t> patch_value_conflict;
  std::vector<uint8_t> replace_value_conflict;
  if (!grovedb::EncodeItemToElementBytes({'p', 'v'}, &patch_value_conflict, &error) ||
      !grovedb::EncodeItemToElementBytes({'r', 'v'}, &replace_value_conflict, &error)) {
    Fail("Encode patch/replace conflict values failed: " + error);
  }
  if (!db.Insert({leaf_key}, patch_replace_key, item_element, &error)) {
    Fail("Insert patch/replace conflict baseline key failed: " + error);
  }
  grovedb::GroveDb::BatchOp patch_conflict_op;
  patch_conflict_op.kind = grovedb::GroveDb::BatchOp::Kind::kPatch;
  patch_conflict_op.path = {leaf_key};
  patch_conflict_op.key = patch_replace_key;
  patch_conflict_op.element_bytes = patch_value_conflict;
  patch_conflict_op.change_in_bytes = 0;
  std::vector<grovedb::GroveDb::BatchOp> patch_replace_same_key_ops = {
      patch_conflict_op,
      {grovedb::GroveDb::BatchOp::Kind::kReplace,
       {leaf_key},
       patch_replace_key,
       replace_value_conflict}};
  error.clear();
  if (db.ApplyBatch(patch_replace_same_key_ops, &error)) {
    Fail("ApplyBatch should fail consistency checks for patch+replace same key");
  }
  if (error != "batch operations fail consistency checks") {
    Fail("patch+replace same key should fail consistency checks: " + error);
  }
  if (!db.Get({leaf_key}, patch_replace_key, &got, &found, &error) || !found || got != item_element) {
    Fail("patch+replace consistency failure should preserve original key");
  }
  error.clear();
  if (!db.ApplyBatch(patch_replace_same_key_ops, disable_consistency_options, &error)) {
    Fail("ApplyBatch with checks disabled should allow patch+replace same key: " + error);
  }
  if (!db.Get({leaf_key}, patch_replace_key, &got, &found, &error) || !found ||
      got != replace_value_conflict) {
    Fail("patch+replace with checks disabled should keep the last op for same key");
  }

  // insert_only after delete on the same key is also a same-key conflict under
  // default checks; with checks disabled the final same-key op is kept.
  std::vector<uint8_t> insert_only_after_delete_key = {'k', 'i', 'o', 'd'};
  std::vector<uint8_t> insert_only_after_delete_value;
  if (!grovedb::EncodeItemToElementBytes({'i', 'o', '2'}, &insert_only_after_delete_value, &error)) {
    Fail("Encode insert_only-after-delete value failed: " + error);
  }
  if (!db.Insert({leaf_key}, insert_only_after_delete_key, item_element, &error)) {
    Fail("Insert insert_only-after-delete baseline key failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> insert_only_after_delete_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, insert_only_after_delete_key, {}},
      {grovedb::GroveDb::BatchOp::Kind::kInsertOnly,
       {leaf_key},
       insert_only_after_delete_key,
       insert_only_after_delete_value}};
  error.clear();
  if (db.ApplyBatch(insert_only_after_delete_ops, &error)) {
    Fail("ApplyBatch should fail consistency checks for insert_only-after-delete same key");
  }
  if (error != "batch operations fail consistency checks") {
    Fail("insert_only-after-delete same key should fail consistency checks: " + error);
  }
  if (!db.Get({leaf_key}, insert_only_after_delete_key, &got, &found, &error) || !found ||
      got != item_element) {
    Fail("insert_only-after-delete consistency failure should preserve original key");
  }
  error.clear();
  if (!db.ApplyBatch(insert_only_after_delete_ops, disable_consistency_options, &error)) {
    Fail("ApplyBatch with checks disabled should allow insert_only-after-delete same key: " + error);
  }
  if (!db.Get({leaf_key}, insert_only_after_delete_key, &got, &found, &error) || !found ||
      got != insert_only_after_delete_value) {
    Fail("insert_only-after-delete with checks disabled should keep the last op for same key");
  }

  // Rust parity: insert-tree operations under a to-be-deleted subtree are
  // rejected at runtime as tree-modification-under-delete conflicts.
  std::vector<uint8_t> consistency_tree_key = {'c', 't'};
  if (!db.Insert({leaf_key}, consistency_tree_key, tree_element, &error)) {
    Fail("Insert consistency tree key failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> insert_tree_under_delete_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
       {leaf_key, consistency_tree_key},
       {'g', 'c'},
       tree_element},
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {leaf_key}, consistency_tree_key, {}}};
  error.clear();
  if (db.ApplyBatch(insert_tree_under_delete_ops, &error)) {
    Fail("ApplyBatch should fail for insert_tree under deleted path");
  }
  if (error != "modification of tree when it will be deleted") {
    Fail("insert_tree-under-delete default failure should be runtime conflict: " + error);
  }
  if (!db.Get({leaf_key}, consistency_tree_key, &got, &found, &error) || !found) {
    Fail("insert_tree-under-delete consistency failure should preserve original tree key");
  }
  error.clear();
  if (db.ApplyBatch(insert_tree_under_delete_ops, disable_consistency_options, &error)) {
    Fail("ApplyBatch should reject insert_tree-under-delete even when checks are disabled");
  }
  if (error != "modification of tree when it will be deleted") {
    Fail("insert_tree-under-delete disabled-check failure should be runtime conflict: " + error);
  }
  if (!db.Get({leaf_key}, consistency_tree_key, &got, &found, &error)) {
    Fail("Get consistency tree key after disabled insert_tree-under-delete failure failed: " + error);
  }
  if (!found) {
    Fail("insert_tree-under-delete runtime rejection should keep original tree key");
  }

  // Rust batch execution groups by level/path/key, so parent-tree inserts are
  // applied before child writes even if the input vector is reversed.
  std::vector<uint8_t> ordered_parent_key = {'o', 'r', 'd'};
  std::vector<uint8_t> ordered_child_key = {'c', 'h'};
  std::vector<uint8_t> ordered_child_value;
  if (!grovedb::EncodeItemToElementBytes({'o', 'k'}, &ordered_child_value, &error)) {
    Fail("Encode ordered child value failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> reversed_parent_child_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert,
       {leaf_key, ordered_parent_key},
       ordered_child_key,
       ordered_child_value},
      {grovedb::GroveDb::BatchOp::Kind::kInsertTree, {leaf_key}, ordered_parent_key, tree_element}};
  if (!db.ApplyBatch(reversed_parent_child_ops, &error)) {
    Fail("ApplyBatch should reorder parent/child ops to succeed: " + error);
  }
  if (!db.Get({leaf_key, ordered_parent_key}, ordered_child_key, &got, &found, &error) || !found ||
      got != ordered_child_value) {
    Fail("reordered parent/child batch should persist child item");
  }
  std::vector<uint8_t> ordered_parent_key_disabled = {'o', 'r', 'd', '2'};
  std::vector<uint8_t> ordered_child_key_disabled = {'c', '2'};
  std::vector<uint8_t> ordered_child_value_disabled;
  if (!grovedb::EncodeItemToElementBytes({'o', 'k', '2'}, &ordered_child_value_disabled, &error)) {
    Fail("Encode ordered child value (disabled consistency) failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> reversed_parent_child_ops_disabled = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert,
       {leaf_key, ordered_parent_key_disabled},
       ordered_child_key_disabled,
       ordered_child_value_disabled},
      {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
       {leaf_key},
       ordered_parent_key_disabled,
       tree_element}};
  if (!db.ApplyBatch(reversed_parent_child_ops_disabled, disable_consistency_options, &error)) {
    Fail("ApplyBatch should reorder parent/child ops even when consistency checks are disabled: " +
         error);
  }
  if (!db.Get({leaf_key, ordered_parent_key_disabled},
              ordered_child_key_disabled,
              &got,
              &found,
              &error) ||
      !found || got != ordered_child_value_disabled) {
    Fail("reordered parent/child batch with consistency checks disabled should persist child item");
  }

  // Aggregate-tree batch propagation: creating a SumTree and inserting a
  // SumItem beneath it in the same batch should succeed (even if child op
  // appears first) and refresh the parent SumTree aggregate.
  std::vector<uint8_t> batch_sum_tree_key = {'s', 'b', 'a', 't', 'c', 'h'};
  std::vector<uint8_t> batch_sum_item_key = {'s', '1'};
  std::vector<uint8_t> batch_sum_tree_element;
  std::vector<uint8_t> batch_sum_item_element;
  if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 0, &batch_sum_tree_element, &error) ||
      !grovedb::EncodeSumItemToElementBytes(7, &batch_sum_item_element, &error)) {
    Fail("Encode same-batch SumTree/SumItem elements failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> sumtree_create_and_sumitem_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert,
       {leaf_key, batch_sum_tree_key},
       batch_sum_item_key,
       batch_sum_item_element},
      {grovedb::GroveDb::BatchOp::Kind::kInsertTree,
       {leaf_key},
       batch_sum_tree_key,
       batch_sum_tree_element}};
  if (!db.ApplyBatch(sumtree_create_and_sumitem_ops, &error)) {
    Fail("ApplyBatch same-batch SumTree+SumItem should succeed: " + error);
  }
  if (!db.Get({leaf_key, batch_sum_tree_key}, batch_sum_item_key, &got, &found, &error) || !found ||
      got != batch_sum_item_element) {
    Fail("same-batch SumTree+SumItem should persist nested sum item");
  }
  if (!db.Get({leaf_key}, batch_sum_tree_key, &got, &found, &error) || !found) {
    Fail("same-batch SumTree+SumItem should persist parent sum tree");
  }
  grovedb::ElementSumTree decoded_batch_sum_tree;
  if (!grovedb::DecodeSumTreeFromElementBytes(got, &decoded_batch_sum_tree, &error)) {
    Fail("DecodeSumTreeFromElementBytes(same-batch sum tree) failed: " + error);
  }
  if (decoded_batch_sum_tree.sum != 7) {
    Fail("same-batch SumTree+SumItem should update parent sum aggregate");
  }

  // Rust batch parity: delete_tree is tree-only and blocks non-empty trees by default.
  std::vector<uint8_t> child_tree_key = {'c', 'h', 'i', 'l', 'd'};
  if (!db.Insert({leaf_key}, child_tree_key, tree_element, &error)) {
    Fail("Insert child tree for non-empty delete check failed: " + error);
  }
  if (!db.Insert({leaf_key, child_tree_key}, {'n', 'k'}, item_element, &error)) {
    Fail("Insert nested item for non-empty delete check failed: " + error);
  }
  std::vector<uint8_t> non_tree_key = {'n', 't'};
  if (!db.Insert({leaf_key}, non_tree_key, item_element, &error)) {
    Fail("Insert non-tree key for delete_tree type check failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> non_tree_delete_tree_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {leaf_key}, non_tree_key, {}}};
  if (db.ApplyBatch(non_tree_delete_tree_ops, &error)) {
    Fail("ApplyBatch(delete_tree on non-tree key) should fail");
  }
  grovedb::GroveDb::BatchApplyOptions non_empty_delete_err_options;
  non_empty_delete_err_options.allow_deleting_non_empty_trees = false;
  non_empty_delete_err_options.deleting_non_empty_trees_returns_error = true;
  std::vector<grovedb::GroveDb::BatchOp> non_empty_delete_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDeleteTree, {leaf_key}, child_tree_key, {}}};
  if (!db.ApplyBatch(non_empty_delete_ops, non_empty_delete_err_options, &error)) {
    Fail("ApplyBatch(non-empty delete_tree with error flag) should succeed: " + error);
  }
  if (!db.Get({leaf_key}, child_tree_key, &got, &found, &error)) {
    Fail("Get child tree after non-empty delete error-flag mode failed: " + error);
  }
  if (found) {
    Fail("non-empty delete_tree with error flag should remove tree key");
  }

  if (!db.Insert({leaf_key}, child_tree_key, tree_element, &error)) {
    Fail("Reinsert child tree for no-error option check failed: " + error);
  }
  if (!db.Insert({leaf_key, child_tree_key}, {'n', 'k'}, item_element, &error)) {
    Fail("Reinsert nested item for no-error option check failed: " + error);
  }

  grovedb::GroveDb::BatchApplyOptions non_empty_delete_noerr_options;
  non_empty_delete_noerr_options.allow_deleting_non_empty_trees = false;
  non_empty_delete_noerr_options.deleting_non_empty_trees_returns_error = false;
  if (!db.ApplyBatch(non_empty_delete_ops, non_empty_delete_noerr_options, &error)) {
    Fail("ApplyBatch(non-empty delete_tree no-error) should succeed: " + error);
  }
  if (!db.Get({leaf_key}, child_tree_key, &got, &found, &error)) {
    Fail("Get child tree after non-empty delete_tree no-error mode failed: " + error);
  }
  if (found) {
    Fail("non-empty delete_tree no-error mode should remove tree key");
  }

  if (!db.Insert({leaf_key}, child_tree_key, tree_element, &error)) {
    Fail("Reinsert child tree for allow=true option check failed: " + error);
  }
  if (!db.Insert({leaf_key, child_tree_key}, {'n', 'k'}, item_element, &error)) {
    Fail("Reinsert nested item for allow=true option check failed: " + error);
  }

  grovedb::GroveDb::BatchApplyOptions non_empty_delete_allow_options;
  non_empty_delete_allow_options.allow_deleting_non_empty_trees = true;
  if (!db.ApplyBatch(non_empty_delete_ops, non_empty_delete_allow_options, &error)) {
    Fail("ApplyBatch(non-empty delete_tree allow=true) should succeed: " + error);
  }
  if (!db.Get({leaf_key}, child_tree_key, &got, &found, &error)) {
    Fail("Get child tree after non-empty delete_tree allow=true failed: " + error);
  }
  if (found) {
    Fail("non-empty delete_tree allow=true should remove tree key");
  }

  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction(rollback) failed: " + error);
  }
  if (!db.Insert({leaf_key}, {'k', '3'}, item_element, &tx, &error)) {
    Fail("Insert before rollback failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', '3'}, &got, &found, &tx, &error)) {
    Fail("Get in tx for inserted key failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Tx should see inserted key");
  }
  if (!db.Get({leaf_key}, {'k', '3'}, &got, &found, &error)) {
    Fail("Get outside tx for inserted key failed: " + error);
  }
  if (found) {
    Fail("Outside tx should not see uncommitted insert");
  }
  if (!db.RollbackTransaction(&tx, &error)) {
    Fail("RollbackTransaction failed: " + error);
  }
  if (!db.Get({leaf_key}, {'k', '2'}, &got, &found, &tx, &error)) {
    Fail("Get with rolled-back tx for stable key failed: " + error);
  }
  if (!found || got != item_element) {
    Fail("Rolled-back tx should read stable key");
  }
  if (!db.Get({leaf_key}, {'k', '3'}, &got, &found, &tx, &error)) {
    Fail("Get with rolled-back tx for reverted key failed: " + error);
  }
  if (found) {
    Fail("Rolled-back tx should not see reverted key");
  }
  if (!db.Get({leaf_key}, {'k', '3'}, &got, &found, &error)) {
    Fail("Get after rollback failed: " + error);
  }
  if (found) {
    Fail("Rolled back key should not be found");
  }

  // Rust GroveDb parity: disjoint subtree writes under the same parent still
  // conflict at commit due parent-layer propagation.
  const std::vector<uint8_t> leaf_a = {'l', 'e', 'a', 'f', 'A'};
  const std::vector<uint8_t> leaf_b = {'l', 'e', 'a', 'f', 'B'};
  if (!db.Insert({}, leaf_a, tree_element, &error) || !db.Insert({}, leaf_b, tree_element, &error)) {
    Fail("Insert disjoint root trees failed: " + error);
  }
  grovedb::GroveDb::Transaction tx_a;
  grovedb::GroveDb::Transaction tx_b;
  if (!db.StartTransaction(&tx_a, &error) || !db.StartTransaction(&tx_b, &error)) {
    Fail("Start disjoint transactions failed: " + error);
  }
  if (!db.Insert({leaf_a}, {'a', '1'}, item_element, &tx_a, &error)) {
    Fail("tx_a insert failed: " + error);
  }
  if (!db.Insert({leaf_b}, {'b', '1'}, item_element, &tx_b, &error)) {
    Fail("tx_b insert failed: " + error);
  }
  if (!db.CommitTransaction(&tx_a, &error)) {
    Fail("tx_a commit failed: " + error);
  }
  if (db.CommitTransaction(&tx_b, &error)) {
    Fail("tx_b commit should fail on conflict");
  }

  // Same-path conflicting transactions: second commit should fail.
  grovedb::GroveDb::Transaction tx_c1;
  grovedb::GroveDb::Transaction tx_c2;
  if (!db.StartTransaction(&tx_c1, &error) || !db.StartTransaction(&tx_c2, &error)) {
    Fail("Start conflict transactions failed: " + error);
  }
  std::vector<uint8_t> v_a;
  std::vector<uint8_t> v_b;
  if (!grovedb::EncodeItemToElementBytes({'A'}, &v_a, &error) ||
      !grovedb::EncodeItemToElementBytes({'B'}, &v_b, &error)) {
    Fail("Encode conflict values failed: " + error);
  }
  if (!db.Insert({leaf_a}, {'c', 'f'}, v_a, &tx_c1, &error)) {
    Fail("tx_c1 insert failed: " + error);
  }
  if (!db.Insert({leaf_a}, {'c', 'f'}, v_b, &tx_c2, &error)) {
    Fail("tx_c2 insert failed: " + error);
  }
  if (!db.CommitTransaction(&tx_c1, &error)) {
    Fail("tx_c1 commit failed: " + error);
  }
  if (db.CommitTransaction(&tx_c2, &error)) {
    Fail("tx_c2 commit should fail on conflict");
  }

  std::filesystem::remove_all(dir);

  // ---- kRefreshReference batch op tests ----
  {
    auto rr_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string rr_dir = MakeTempDir("facade_test_" + std::to_string(rr_now));
    grovedb::GroveDb rr_db;
    if (!rr_db.Open(rr_dir, &error)) {
      Fail("Open for kRefreshReference test failed: " + error);
    }

    // Insert root tree.
    std::vector<uint8_t> tree_bytes;
    if (!grovedb::EncodeTreeToElementBytes(&tree_bytes, &error)) {
      Fail("Encode tree failed: " + error);
    }
    if (!rr_db.Insert({}, {'r', 'o', 'o', 't'}, tree_bytes, &error)) {
      Fail("Insert root tree failed: " + error);
    }

    const std::vector<uint8_t> root_key = {'r', 'o', 'o', 't'};
    const std::vector<std::vector<uint8_t>> root_path = {root_key};

    // Insert an Item element at root/k1.
    std::vector<uint8_t> item_bytes;
    if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_bytes, &error)) {
      Fail("Encode item failed: " + error);
    }
    if (!rr_db.Insert(root_path, {'k', '1'}, item_bytes, &error)) {
      Fail("Insert k1 item failed: " + error);
    }

    // Insert a Reference element at root/ref1 pointing to root/k1.
    grovedb::ElementReference ref1;
    ref1.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
    ref1.reference_path.path = {root_key, {'k', '1'}};
    ref1.has_max_hop = false;
    std::vector<uint8_t> ref1_bytes;
    if (!grovedb::EncodeReferenceToElementBytes(ref1, &ref1_bytes, &error)) {
      Fail("Encode ref1 failed: " + error);
    }
    if (!rr_db.Insert(root_path, {'r', 'e', 'f', '1'}, ref1_bytes, &error)) {
      Fail("Insert ref1 failed: " + error);
    }

    // Test 1: kRefreshReference on a non-reference element should fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'k', '1'};
      op.element_bytes = ref1_bytes;
      if (rr_db.ApplyBatch({op}, &error)) {
        Fail("kRefreshReference on Item should fail but succeeded");
      }
    }

    // Test 2: kRefreshReference on a non-existent key should fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'m', 'i', 's', 's', 'i', 'n', 'g'};
      op.element_bytes = ref1_bytes;
      if (rr_db.ApplyBatch({op}, &error)) {
        Fail("kRefreshReference on missing key should fail but succeeded");
      }
    }

    // Test 2b: trust_refresh_reference allows refreshing a missing key by
    // trusting the provided reference payload.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'m', 'i', 's', 's', '2'};
      op.element_bytes = ref1_bytes;
      grovedb::GroveDb::BatchApplyOptions options;
      options.trust_refresh_reference = true;
      if (!rr_db.ApplyBatch({op}, options, &error)) {
        Fail("kRefreshReference with trust_refresh_reference should upsert missing key: " + error);
      }
      std::vector<uint8_t> got_missing_refresh;
      bool missing_refresh_found = false;
      if (!rr_db.GetRaw(root_path,
                        {'m', 'i', 's', 's', '2'},
                        &got_missing_refresh,
                        &missing_refresh_found,
                        &error)) {
        Fail("GetRaw after trusted refresh on missing key failed: " + error);
      }
      if (!missing_refresh_found || got_missing_refresh != ref1_bytes) {
        Fail("trusted refresh on missing key should store provided reference bytes");
      }
    }

    // Test 3: kRefreshReference with non-reference element_bytes should fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'r', 'e', 'f', '1'};
      op.element_bytes = item_bytes;  // This is an Item, not a Reference.
      if (rr_db.ApplyBatch({op}, &error)) {
        Fail("kRefreshReference with Item element_bytes should fail");
      }
    }

    // Test 4: kRefreshReference on existing reference with valid reference
    // element_bytes should succeed and update the element.
    {
      // Create a new reference pointing to a different target (root/k1 but with
      // max_hop set).
      grovedb::ElementReference ref2;
      ref2.reference_path.kind = grovedb::ReferencePathKind::kAbsolute;
      ref2.reference_path.path = {root_key, {'k', '1'}};
      ref2.has_max_hop = true;
      ref2.max_hop = 5;
      std::vector<uint8_t> ref2_bytes;
      if (!grovedb::EncodeReferenceToElementBytes(ref2, &ref2_bytes, &error)) {
        Fail("Encode ref2 failed: " + error);
      }

      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'r', 'e', 'f', '1'};
      op.element_bytes = ref2_bytes;
      if (!rr_db.ApplyBatch({op}, &error)) {
        Fail("kRefreshReference should succeed: " + error);
      }

      // Verify the stored element was updated.
      std::vector<uint8_t> read_back;
      bool found = false;
      // Use GetRaw to get the raw reference bytes (Get would follow the reference).
      if (!rr_db.GetRaw(root_path, {'r', 'e', 'f', '1'}, &read_back, &found, &error)) {
        Fail("Read back after refresh failed: " + error);
      }
      if (!found) {
        Fail("ref1 not found after refresh");
      }
      if (read_back != ref2_bytes) {
        Fail("ref1 element not updated to ref2 after refresh");
      }

      // Decode the refreshed element and verify it's a reference with max_hop=5.
      grovedb::ElementReference decoded_ref;
      if (!grovedb::DecodeReferenceFromElementBytes(read_back, &decoded_ref, &error)) {
        Fail("Decode refreshed reference failed: " + error);
      }
      if (!decoded_ref.has_max_hop || decoded_ref.max_hop != 5) {
        Fail("Decoded reference max_hop mismatch after refresh");
      }
    }

    // Test 5: kRefreshReference works through a transaction.
    {
      grovedb::GroveDb::Transaction rr_tx;
      if (!rr_db.StartTransaction(&rr_tx, &error)) {
        Fail("Start tx for refresh ref test failed: " + error);
      }

      grovedb::ElementReference ref3;
      ref3.reference_path.kind = grovedb::ReferencePathKind::kSibling;
      ref3.reference_path.key = {'k', '1'};
      ref3.has_max_hop = false;
      std::vector<uint8_t> ref3_bytes;
      if (!grovedb::EncodeReferenceToElementBytes(ref3, &ref3_bytes, &error)) {
        Fail("Encode ref3 failed: " + error);
      }

      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kRefreshReference;
      op.path = root_path;
      op.key = {'r', 'e', 'f', '1'};
      op.element_bytes = ref3_bytes;
      if (!rr_db.ApplyBatch({op}, &rr_tx, &error)) {
        Fail("kRefreshReference in tx should succeed: " + error);
      }

      // Visible in tx.
      std::vector<uint8_t> in_tx_read;
      bool in_tx_found = false;
      // Use GetRaw to get the raw reference bytes (Get would follow the reference).
      if (!rr_db.GetRaw(root_path, {'r', 'e', 'f', '1'}, &in_tx_read, &in_tx_found, &rr_tx,
                      &error)) {
        Fail("Get in tx failed: " + error);
      }
      if (in_tx_read != ref3_bytes) {
        Fail("Reference not updated inside tx");
      }

      if (!rr_db.CommitTransaction(&rr_tx, &error)) {
        Fail("Commit refresh ref tx failed: " + error);
      }

      // Visible after commit.
      std::vector<uint8_t> post_commit_read;
      bool post_found = false;
      // Use GetRaw to get the raw reference bytes (Get would follow the reference).
      if (!rr_db.GetRaw(root_path, {'r', 'e', 'f', '1'}, &post_commit_read, &post_found, &error)) {
        Fail("Get after commit failed: " + error);
      }
      if (post_commit_read != ref3_bytes) {
        Fail("Reference not updated after commit");
      }
    }

    std::filesystem::remove_all(rr_dir);
  }

  // ---- kInsertOrReplace batch op tests ----
  {
    auto ior_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string ior_dir = MakeTempDir("facade_test_" + std::to_string(ior_now));
    grovedb::GroveDb ior_db;
    if (!ior_db.Open(ior_dir, &error)) {
      Fail("Open for kInsertOrReplace test failed: " + error);
    }

    const std::vector<std::vector<uint8_t>> root_path = {};
    // Insert a root subtree.
    if (!ior_db.InsertEmptyTree({}, {'r'}, &error)) {
      Fail("InsertEmptyTree for kInsertOrReplace test failed: " + error);
    }
    const std::vector<std::vector<uint8_t>> rp = {{'r'}};

    // Test 1: kInsertOrReplace inserts a new key.
    {
      std::vector<uint8_t> item_bytes;
      grovedb::EncodeItemToElementBytes({'v', '1'}, &item_bytes, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
      op.path = rp;
      op.key = {'k', '1'};
      op.element_bytes = item_bytes;
      if (!ior_db.ApplyBatch({op}, &error)) {
        Fail("kInsertOrReplace insert new key should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!ior_db.Get(rp, {'k', '1'}, &readback, &found, &error)) {
        Fail("Get after kInsertOrReplace insert failed: " + error);
      }
      if (!found || readback != item_bytes) {
        Fail("kInsertOrReplace insert: value mismatch or not found");
      }
    }

    // Test 2: kInsertOrReplace replaces existing key.
    {
      std::vector<uint8_t> item_bytes2;
      grovedb::EncodeItemToElementBytes({'v', '2'}, &item_bytes2, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
      op.path = rp;
      op.key = {'k', '1'};
      op.element_bytes = item_bytes2;
      if (!ior_db.ApplyBatch({op}, &error)) {
        Fail("kInsertOrReplace replace existing key should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!ior_db.Get(rp, {'k', '1'}, &readback, &found, &error)) {
        Fail("Get after kInsertOrReplace replace failed: " + error);
      }
      if (!found || readback != item_bytes2) {
        Fail("kInsertOrReplace replace: value mismatch");
      }
    }

    // Test 3: kInsertOrReplace subject to validate_insertion_does_not_override_tree.
    {
      if (!ior_db.InsertEmptyTree(rp, {'s', 'u', 'b'}, &error)) {
        Fail("InsertEmptyTree for kInsertOrReplace override test failed: " + error);
      }
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'x'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
      op.path = rp;
      op.key = {'s', 'u', 'b'};
      op.element_bytes = new_item;
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.validate_insertion_does_not_override_tree = true;
      if (ior_db.ApplyBatch({op}, opts, &error)) {
        Fail("kInsertOrReplace should be rejected by validate_insertion_does_not_override_tree");
      }
    }

    // Test for kInsertTree with validate_insertion_does_not_override_tree
    {
      // First insert a tree element at a key
      if (!ior_db.InsertEmptyTree(rp, {'i', 't', 't'}, &error)) {
        Fail("InsertEmptyTree for kInsertTree override test failed: " + error);
      }
      // Now try to replace it with kInsertTree - should be rejected
      std::vector<uint8_t> new_tree;
      grovedb::EncodeTreeToElementBytes(&new_tree, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'i', 't', 't'};
      op.element_bytes = new_tree;
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.validate_insertion_does_not_override_tree = true;
      if (ior_db.ApplyBatch({op}, opts, &error)) {
        Fail("kInsertTree should be rejected by validate_insertion_does_not_override_tree when tree exists");
      }
    }

    // Test for kInsertTree with validate_insertion_does_not_override (any override)
    {
      // First insert an item element at a key
      std::vector<uint8_t> item_bytes;
      grovedb::EncodeItemToElementBytes({'i'}, &item_bytes, &error);
      if (!ior_db.Insert(rp, {'i', 't', 'i'}, item_bytes, &error)) {
        Fail("Insert item for kInsertTree strict override test failed: " + error);
      }
      // Now try to replace it with kInsertTree - should be rejected
      std::vector<uint8_t> new_tree;
      grovedb::EncodeTreeToElementBytes(&new_tree, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'i', 't', 'i'};
      op.element_bytes = new_tree;
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.validate_insertion_does_not_override = true;
      if (ior_db.ApplyBatch({op}, opts, &error)) {
        Fail("kInsertTree should be rejected by validate_insertion_does_not_override when element exists");
      }
    }

    // Test 4: kInsertOrReplace works through a transaction (root path).
    {
      grovedb::GroveDb::Transaction ior_tx;
      if (!ior_db.StartTransaction(&ior_tx, &error)) {
        Fail("StartTransaction for kInsertOrReplace tx test failed: " + error);
      }
      std::vector<uint8_t> tree_bytes;
      grovedb::EncodeTreeToElementBytes(&tree_bytes, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertOrReplace;
      op.path = root_path;
      op.key = {'t', 'x'};
      op.element_bytes = tree_bytes;
      if (!ior_db.ApplyBatch({op}, &ior_tx, &error)) {
        Fail("kInsertOrReplace in tx should succeed: " + error);
      }
      // Visible in tx.
      std::vector<uint8_t> in_tx_read;
      bool in_tx_found = false;
      if (!ior_db.Get(root_path, {'t', 'x'}, &in_tx_read, &in_tx_found, &ior_tx, &error)) {
        Fail("Get in tx after kInsertOrReplace failed: " + error);
      }
      if (!in_tx_found) {
        Fail("kInsertOrReplace in tx: element not found");
      }
      if (!ior_db.CommitTransaction(&ior_tx, &error)) {
        Fail("Commit kInsertOrReplace tx failed: " + error);
      }
    }

    std::filesystem::remove_all(ior_dir);
  }

  // ---- validate_insertion_does_not_override_tree option tests ----
  {
    auto vt_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string vt_dir = MakeTempDir("facade_test_" + std::to_string(vt_now));
    grovedb::GroveDb vt_db;
    if (!vt_db.Open(vt_dir, &error)) {
      Fail("Open for override_tree test failed: " + error);
    }

    const std::vector<uint8_t> rk = {'r', 'o', 'o', 't'};
    const std::vector<std::vector<uint8_t>> rp = {rk};

    // Create root tree + subtree + item.
    std::vector<uint8_t> tree_bytes;
    grovedb::EncodeTreeToElementBytes(&tree_bytes, &error);
    vt_db.Insert({}, rk, tree_bytes, &error);

    std::vector<uint8_t> sub_tree_bytes;
    grovedb::EncodeTreeToElementBytes(&sub_tree_bytes, &error);
    vt_db.Insert(rp, {'s', 'u', 'b'}, sub_tree_bytes, &error);

    std::vector<uint8_t> item_bytes;
    grovedb::EncodeItemToElementBytes({'v', '1'}, &item_bytes, &error);
    vt_db.Insert(rp, {'i', 't', 'm'}, item_bytes, &error);

    grovedb::GroveDb::BatchApplyOptions opts;
    opts.validate_insertion_does_not_override_tree = true;

    // Test 1: Inserting over an existing Item should SUCCEED (tree-only check).
    {
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'v', '2'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'i', 't', 'm'};
      op.element_bytes = new_item;
      if (!vt_db.ApplyBatch({op}, opts, &error)) {
        Fail("override_tree: insert over Item should succeed: " + error);
      }
      // Verify it was updated.
      std::vector<uint8_t> got;
      bool found = false;
      vt_db.Get(rp, {'i', 't', 'm'}, &got, &found, &error);
      if (!found || got != new_item) {
        Fail("override_tree: item not updated");
      }
    }

    // Test 2: Inserting over an existing Tree should FAIL.
    {
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'b', 'a', 'd'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'s', 'u', 'b'};
      op.element_bytes = new_item;
      if (vt_db.ApplyBatch({op}, opts, &error)) {
        Fail("override_tree: insert over Tree should fail but succeeded");
      }
    }

    // Test 3: kReplace over a Tree should also fail with this option.
    {
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'r', 'p'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kReplace;
      op.path = rp;
      op.key = {'s', 'u', 'b'};
      op.element_bytes = new_item;
      if (vt_db.ApplyBatch({op}, opts, &error)) {
        Fail("override_tree: replace over Tree should fail");
      }
    }

    // Test 4: kPatch over a Tree should fail with this option.
    {
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'p', 'a'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kPatch;
      op.path = rp;
      op.key = {'s', 'u', 'b'};
      op.element_bytes = new_item;
      if (vt_db.ApplyBatch({op}, opts, &error)) {
        Fail("override_tree: patch over Tree should fail");
      }
    }

    // Test 5: Inserting a new key should SUCCEED (no existing element to
    // override).
    {
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'n', 'e', 'w'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'n', 'k'};
      op.element_bytes = new_item;
      if (!vt_db.ApplyBatch({op}, opts, &error)) {
        Fail("override_tree: insert new key should succeed: " + error);
      }
    }

    // Test 6: Both flags set: validate_insertion_does_not_override takes
    // precedence (reject even non-tree override).
    {
      grovedb::GroveDb::BatchApplyOptions both_opts;
      both_opts.validate_insertion_does_not_override = true;
      both_opts.validate_insertion_does_not_override_tree = true;
      std::vector<uint8_t> new_item;
      grovedb::EncodeItemToElementBytes({'x'}, &new_item, &error);
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'i', 't', 'm'};
      op.element_bytes = new_item;
      if (vt_db.ApplyBatch({op}, both_opts, &error)) {
        Fail("override_tree: both flags should reject non-tree override");
      }
    }

    std::filesystem::remove_all(vt_dir);
  }

  // ---- Batch option contract tests ----
  // base_root_storage_is_free is currently non-enforcing and accepted.
  // ApplyBatch rejects batch_pause_height > 0 and requires ApplyPartialBatch.
  {
    auto pt_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string pt_dir = MakeTempDir("facade_test_" + std::to_string(pt_now));
    grovedb::GroveDb pt_db;
    if (!pt_db.Open(pt_dir, &error)) {
      Fail("Open for pass-through option test failed: " + error);
    }
    if (!pt_db.InsertEmptyTree({}, {'r'}, &error)) {
      Fail("InsertEmptyTree for pass-through test failed: " + error);
    }
    const std::vector<std::vector<uint8_t>> rp = {{'r'}};

    std::vector<uint8_t> item_v1;
    grovedb::EncodeItemToElementBytes({'v', '1'}, &item_v1, &error);

    // Apply baseline without non-enforcing options.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'a'};
      op.element_bytes = item_v1;
      if (!pt_db.ApplyBatch({op}, &error)) {
        Fail("baseline batch insert should succeed: " + error);
      }
    }

    // Apply with base_root_storage_is_free = false (non-default).
    {
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.base_root_storage_is_free = false;
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'b'};
      op.element_bytes = item_v1;
      if (!pt_db.ApplyBatch({op}, opts, &error)) {
        Fail("batch with base_root_storage_is_free=false should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!pt_db.Get(rp, {'b'}, &readback, &found, &error)) {
        Fail("Get after base_root_storage_is_free=false batch failed: " + error);
      }
      if (!found || readback != item_v1) {
        Fail("base_root_storage_is_free=false: value mismatch");
      }
    }

    // ApplyBatch with batch_pause_height > 0 should reject and remain atomic.
    {
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.batch_pause_height = 1;
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'c'};
      op.element_bytes = item_v1;
      std::vector<uint8_t> before_raw;
      bool before_found = false;
      if (!pt_db.Get(rp, {'c'}, &before_raw, &before_found, &error)) {
        Fail("Get before batch_pause_height=1 batch failed: " + error);
      }
      if (pt_db.ApplyBatch({op}, opts, &error)) {
        Fail("batch with batch_pause_height=1 should fail and require ApplyPartialBatch");
      }
      if (error.find("use ApplyPartialBatch for batch_pause_height > 0") == std::string::npos) {
        Fail("unexpected batch_pause_height=1 error: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!pt_db.Get(rp, {'c'}, &readback, &found, &error)) {
        Fail("Get after batch_pause_height=1 batch failed: " + error);
      }
      if (found != before_found) {
        Fail("batch_pause_height=1 should not change key existence");
      }
      if (found && readback != before_raw) {
        Fail("batch_pause_height=1 should not mutate existing value");
      }
      grovedb::OperationCost estimated_cost_pause;
      if (!pt_db.EstimatedCaseOperationsForBatch({op}, opts, &estimated_cost_pause, &error)) {
        Fail("EstimatedCaseOperationsForBatch with batch_pause_height=1 should succeed: " + error);
      }
    }

    // Apply with both non-enforcing options set.
    {
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.base_root_storage_is_free = false;
      opts.batch_pause_height = 0;
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsert;
      op.path = rp;
      op.key = {'d'};
      op.element_bytes = item_v1;
      if (!pt_db.ApplyBatch({op}, opts, &error)) {
        Fail("batch with both non-enforcing options should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!pt_db.Get(rp, {'d'}, &readback, &found, &error)) {
        Fail("Get after combined non-enforcing options batch failed: " + error);
      }
      if (!found || readback != item_v1) {
        Fail("combined non-enforcing options: value mismatch");
      }
    }

    // Partial pause at height 1: level-1 ops execute, level-0 ops become leftovers.
    {
      if (!pt_db.InsertEmptyTree({}, {'p'}, &error)) {
        Fail("InsertEmptyTree p for partial pause test failed: " + error);
      }
      const std::vector<std::vector<uint8_t>> p_path = {{'p'}};
      std::vector<uint8_t> px_raw;
      std::vector<uint8_t> py_raw;
      if (!grovedb::EncodeItemToElementBytes({'x'}, &px_raw, &error) ||
          !grovedb::EncodeItemToElementBytes({'y'}, &py_raw, &error)) {
        Fail("Encode item for partial pause test failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> partial_ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'t', '0'}, px_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsert, p_path, {'t', '1'}, py_raw}};
      grovedb::GroveDb::BatchApplyOptions pause_opts;
      pause_opts.batch_pause_height = 1;
      grovedb::GroveDb::OpsByLevelPath leftovers;
      if (!pt_db.ApplyPartialBatch(partial_ops, pause_opts, nullptr, &leftovers, &error)) {
        Fail("ApplyPartialBatch height=1 failed: " + error);
      }
      if (leftovers.size() != 1 || leftovers[0].size() != 1) {
        Fail("ApplyPartialBatch height=1 leftover shape mismatch");
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!pt_db.Get({}, {'t', '0'}, &readback, &found, &error)) {
        Fail("Get t0 after ApplyPartialBatch failed: " + error);
      }
      if (found) {
        Fail("level-0 op should be leftover after ApplyPartialBatch height=1");
      }
      if (!pt_db.Get(p_path, {'t', '1'}, &readback, &found, &error)) {
        Fail("Get t1 after ApplyPartialBatch failed: " + error);
      }
      if (!found || readback != py_raw) {
        Fail("level-1 op should execute during ApplyPartialBatch height=1");
      }

      grovedb::GroveDb::BatchApplyOptions resume_opts;
      resume_opts.batch_pause_height = 0;
      grovedb::GroveDb::OpsByLevelPath resume_leftovers;
      if (!pt_db.ContinuePartialApplyBatch(
              leftovers, {}, resume_opts, nullptr, &resume_leftovers, &error)) {
        Fail("ContinuePartialApplyBatch from height=1 leftovers failed: " + error);
      }
      if (!resume_leftovers.empty()) {
        Fail("ContinuePartialApplyBatch should not return leftovers at height=0");
      }
      if (!pt_db.Get({}, {'t', '0'}, &readback, &found, &error)) {
        Fail("Get t0 after ContinuePartialApplyBatch failed: " + error);
      }
      if (!found || readback != px_raw) {
        Fail("ContinuePartialApplyBatch should apply level-0 leftover");
      }
    }

    // Pause at height 2 with multi-level ops: only level-2 executes.
    {
      if (!pt_db.InsertEmptyTree({}, {'m'}, &error)) {
        Fail("InsertEmptyTree m failed: " + error);
      }
      if (!pt_db.InsertEmptyTree({{'m'}}, {'n'}, &error)) {
        Fail("InsertEmptyTree m/n failed: " + error);
      }
      std::vector<uint8_t> v0_raw;
      std::vector<uint8_t> v1_raw;
      std::vector<uint8_t> v2_raw;
      if (!grovedb::EncodeItemToElementBytes({'0'}, &v0_raw, &error) ||
          !grovedb::EncodeItemToElementBytes({'1'}, &v1_raw, &error) ||
          !grovedb::EncodeItemToElementBytes({'2'}, &v2_raw, &error)) {
        Fail("Encode level items for height=2 pause failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'h', '0'}, v0_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m'}}, {'h', '1'}, v1_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m'}, {'n'}}, {'h', '2'}, v2_raw}};
      grovedb::GroveDb::BatchApplyOptions pause2_opts;
      pause2_opts.batch_pause_height = 2;
      grovedb::GroveDb::OpsByLevelPath leftovers;
      if (!pt_db.ApplyPartialBatch(ops, pause2_opts, nullptr, &leftovers, &error)) {
        Fail("ApplyPartialBatch height=2 failed: " + error);
      }
      if (leftovers.size() != 2 || leftovers[0].size() != 1 || leftovers[1].size() != 1) {
        Fail("ApplyPartialBatch height=2 leftovers mismatch");
      }
      bool found = false;
      std::vector<uint8_t> readback;
      if (!pt_db.Get({{'m'}, {'n'}}, {'h', '2'}, &readback, &found, &error)) {
        Fail("Get h2 after height=2 pause failed: " + error);
      }
      if (!found || readback != v2_raw) {
        Fail("level-2 op should execute for height=2 pause");
      }
      if (!pt_db.Get({}, {'h', '0'}, &readback, &found, &error)) {
        Fail("Get h0 after height=2 pause failed: " + error);
      }
      if (found) {
        Fail("level-0 op should be leftover for height=2 pause");
      }
      if (!pt_db.Get({{'m'}}, {'h', '1'}, &readback, &found, &error)) {
        Fail("Get h1 after height=2 pause failed: " + error);
      }
      if (found) {
        Fail("level-1 op should be leftover for height=2 pause");
      }

      std::vector<uint8_t> add_raw;
      if (!grovedb::EncodeItemToElementBytes({'a'}, &add_raw, &error)) {
        Fail("Encode additional op for continue failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> additional_ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m'}, {'n'}}, {'a', 'd', 'd'}, add_raw}};
      grovedb::GroveDb::BatchApplyOptions resume_opts;
      resume_opts.batch_pause_height = 0;
      grovedb::GroveDb::OpsByLevelPath resume_leftovers;
      if (!pt_db.ContinuePartialApplyBatch(
              leftovers, additional_ops, resume_opts, nullptr, &resume_leftovers, &error)) {
        Fail("ContinuePartialApplyBatch with additional ops failed: " + error);
      }
      if (!resume_leftovers.empty()) {
        Fail("ContinuePartialApplyBatch height=0 should fully apply leftovers");
      }
      if (!pt_db.Get({}, {'h', '0'}, &readback, &found, &error) || !found || readback != v0_raw) {
        Fail("resume should apply level-0 leftover op");
      }
      if (!pt_db.Get({{'m'}}, {'h', '1'}, &readback, &found, &error) ||
          !found || readback != v1_raw) {
        Fail("resume should apply level-1 leftover op");
      }
      if (!pt_db.Get({{'m'}, {'n'}}, {'a', 'd', 'd'}, &readback, &found, &error) ||
          !found || readback != add_raw) {
        Fail("resume should apply additional ops");
      }
    }

    // ContinuePartialApplyBatch must validate leftover level/path consistency.
    {
      grovedb::GroveDb::OpsByLevelPath invalid_leftover;
      std::vector<uint8_t> iv_raw;
      if (!grovedb::EncodeItemToElementBytes({'z'}, &iv_raw, &error)) {
        Fail("Encode invalid leftover item failed: " + error);
      }
      invalid_leftover[1] = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'b', 'a', 'd'}, iv_raw}};
      grovedb::GroveDb::OpsByLevelPath out_leftovers;
      if (pt_db.ContinuePartialApplyBatch(
              invalid_leftover, {}, grovedb::GroveDb::BatchApplyOptions(), nullptr,
              &out_leftovers, &error)) {
        Fail("ContinuePartialApplyBatch should reject leftover level/path mismatch");
      }
      if (error.find("leftover op level/path mismatch") == std::string::npos) {
        Fail("ContinuePartialApplyBatch mismatch error unexpected: " + error);
      }
    }

    // EstimatedCaseOperationsForBatch with pause height should only estimate executed levels.
    {
      std::vector<uint8_t> e0_raw;
      std::vector<uint8_t> e1_raw;
      if (!grovedb::EncodeItemToElementBytes({'e', '0'}, &e0_raw, &error) ||
          !grovedb::EncodeItemToElementBytes({'e', '1'}, &e1_raw, &error)) {
        Fail("Encode estimate items failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> estimate_ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {}, {'e', '0'}, e0_raw},
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'p'}}, {'e', '1'}, e1_raw}};
      grovedb::OperationCost full_cost;
      if (!pt_db.EstimatedCaseOperationsForBatch(
              estimate_ops, grovedb::GroveDb::BatchApplyOptions(), &full_cost, &error)) {
        Fail("EstimatedCaseOperationsForBatch full estimate failed: " + error);
      }
      grovedb::GroveDb::BatchApplyOptions pause_opts;
      pause_opts.batch_pause_height = 1;
      grovedb::OperationCost pause_cost;
      if (!pt_db.EstimatedCaseOperationsForBatch(estimate_ops, pause_opts, &pause_cost, &error)) {
        Fail("EstimatedCaseOperationsForBatch pause estimate failed: " + error);
      }
      if (pause_cost.seek_count == 0 || pause_cost.seek_count >= full_cost.seek_count) {
        Fail("pause estimate should account for fewer executed operations");
      }
    }

    // If all ops are deeper than pause height, leftovers should be empty.
    {
      std::vector<uint8_t> deep_raw;
      if (!grovedb::EncodeItemToElementBytes({'d', 'e', 'e', 'p'}, &deep_raw, &error)) {
        Fail("Encode deep op item failed: " + error);
      }
      std::vector<grovedb::GroveDb::BatchOp> deep_ops = {
          {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'m'}, {'n'}}, {'d', '2'}, deep_raw}};
      grovedb::GroveDb::BatchApplyOptions opts;
      opts.batch_pause_height = 1;
      grovedb::GroveDb::OpsByLevelPath leftovers;
      if (!pt_db.ApplyPartialBatch(deep_ops, opts, nullptr, &leftovers, &error)) {
        Fail("ApplyPartialBatch deep-only ops failed: " + error);
      }
      if (!leftovers.empty()) {
        Fail("deep-only ops should produce empty leftovers when pause height is lower");
      }
    }

    std::filesystem::remove_all(pt_dir);
  }

  // ---- kInsertTree batch op tests ----
  {
    auto it_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string it_dir = MakeTempDir("facade_test_" + std::to_string(it_now));
    grovedb::GroveDb it_db;
    if (!it_db.Open(it_dir, &error)) {
      Fail("Open for kInsertTree test failed: " + error);
    }

    const std::vector<std::vector<uint8_t>> root_path = {};
    // Insert a root subtree first.
    std::vector<uint8_t> tree_bytes;
    if (!grovedb::EncodeTreeToElementBytes(&tree_bytes, &error)) {
      Fail("Encode tree for kInsertTree test failed: " + error);
    }
    if (!it_db.Insert({}, {'r', 'o', 'o', 't'}, tree_bytes, &error)) {
      Fail("Insert root tree for kInsertTree test failed: " + error);
    }
    const std::vector<std::vector<uint8_t>> rp = {{'r', 'o', 'o', 't'}};

    // Test 1: kInsertTree inserts a new tree.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'s', 'u', 'b', 't', 'r', 'e', 'e'};
      op.element_bytes = tree_bytes;
      if (!it_db.ApplyBatch({op}, &error)) {
        Fail("kInsertTree insert new tree should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!it_db.Get(rp, {'s', 'u', 'b', 't', 'r', 'e', 'e'}, &readback, &found, &error)) {
        Fail("Get after kInsertTree failed: " + error);
      }
      if (!found) {
        Fail("kInsertTree: tree not found");
      }
      // Verify it's a tree element (variants 2,4,5,6,7,8,10 are trees).
      uint64_t variant = 0;
      if (!grovedb::DecodeElementVariant(readback, &variant, &error)) {
        Fail("Decode variant after kInsertTree failed: " + error);
      }
      // Tree variants: 2=Tree, 4=SumTree, 5=BigSumTree, 6=CountTree,
      // 7=CountSumTree, 8=ProvableCountTree, 10=ProvableCountSumTree
      bool is_tree = (variant == 2 || variant == 4 || variant == 5 || variant == 6 ||
                      variant == 7 || variant == 8 || variant == 10);
      if (!is_tree) {
        Fail("kInsertTree: inserted element is not a tree");
      }
    }

    // Test 2: kInsertTree with non-tree element_bytes should fail.
    {
      std::vector<uint8_t> item_bytes;
      if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_bytes, &error)) {
        Fail("Encode item failed: " + error);
      }
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'i', 't', 'e', 'm'};
      op.element_bytes = item_bytes;
      if (it_db.ApplyBatch({op}, &error)) {
        Fail("kInsertTree with Item should fail but succeeded");
      }
    }

    // Test 3: kInsertTree replaces an existing tree.
    {
      // Insert a tree first.
      grovedb::GroveDb::BatchOp insert_op;
      insert_op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      insert_op.path = rp;
      insert_op.key = {'r', 'e', 'p', 'l', 'a', 'c', 'e', 'm', 'e'};
      insert_op.element_bytes = tree_bytes;
      if (!it_db.ApplyBatch({insert_op}, &error)) {
        Fail("Initial tree insert failed: " + error);
      }
      // Now replace it with kInsertTree.
      if (!it_db.ApplyBatch({insert_op}, &error)) {
        Fail("kInsertTree replace existing tree should succeed: " + error);
      }
      std::vector<uint8_t> readback;
      bool found = false;
      if (!it_db.Get(rp, {'r', 'e', 'p', 'l', 'a', 'c', 'e', 'm', 'e'}, &readback, &found, &error)) {
        Fail("Get after kInsertTree replace failed: " + error);
      }
      if (!found) {
        Fail("kInsertTree replace: tree not found");
      }
    }

    // Test 4: kInsertTree works with transaction.
    {
      grovedb::GroveDb::Transaction it_tx;
      if (!it_db.StartTransaction(&it_tx, &error)) {
        Fail("StartTransaction for kInsertTree tx test failed: " + error);
      }
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'t', 'x', 't', 'r', 'e', 'e'};
      op.element_bytes = tree_bytes;
      if (!it_db.ApplyBatch({op}, &it_tx, &error)) {
        Fail("kInsertTree in tx should succeed: " + error);
      }
      // Visible in tx.
      std::vector<uint8_t> in_tx_read;
      bool in_tx_found = false;
      if (!it_db.Get(rp, {'t', 'x', 't', 'r', 'e', 'e'}, &in_tx_read, &in_tx_found, &it_tx, &error)) {
        Fail("Get in tx failed: " + error);
      }
      if (!in_tx_found) {
        Fail("kInsertTree not visible in tx");
      }
      if (!it_db.CommitTransaction(&it_tx, &error)) {
        Fail("Commit tx for kInsertTree failed: " + error);
      }
      // Visible after commit.
      std::vector<uint8_t> post_commit_read;
      bool post_found = false;
      if (!it_db.Get(rp, {'t', 'x', 't', 'r', 'e', 'e'}, &post_commit_read, &post_found, &error)) {
        Fail("Get after commit failed: " + error);
      }
      if (!post_found) {
        Fail("kInsertTree not visible after commit");
      }
    }

    // Test 5: kInsertTree with empty element_bytes should fail.
    {
      grovedb::GroveDb::BatchOp op;
      op.kind = grovedb::GroveDb::BatchOp::Kind::kInsertTree;
      op.path = rp;
      op.key = {'e', 'm', 'p', 't', 'y'};
      op.element_bytes = {};  // Empty bytes.
      if (it_db.ApplyBatch({op}, &error)) {
        Fail("kInsertTree with empty element_bytes should fail but succeeded");
      }
    }

    std::filesystem::remove_all(it_dir);
  }

  return 0;
}
