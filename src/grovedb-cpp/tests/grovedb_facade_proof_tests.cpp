#include "element.h"
#include "grovedb.h"
#include "proof.h"
#include "test_utils.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

int main() {
  std::string error;
  grovedb::GroveDb db;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string dir = MakeTempDir("facade_proof_test_" + std::to_string(now));
  if (!db.Open(dir, &error)) {
    Fail("Open failed: " + error);
  }

  std::vector<uint8_t> item_element;
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_element, &error)) {
    Fail("EncodeItemToElementBytes failed: " + error);
  }
  std::vector<uint8_t> empty_tree;
  if (!grovedb::EncodeTreeToElementBytes(&empty_tree, &error)) {
    Fail("EncodeTreeToElementBytes failed: " + error);
  }
  if (!db.Insert({}, {'k', '1'}, item_element, &error)) {
    Fail("Insert root item failed: " + error);
  }
  if (!db.Insert({}, {'r', 'o', 'o', 't'}, empty_tree, &error)) {
    Fail("Insert root tree failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'e', 'y'}, empty_tree, &error)) {
    Fail("Insert root/key tree failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}}, {'b', 'r', 'a', 'n', 'c', 'h'}, empty_tree,
                 &error)) {
    Fail("Insert root/key/branch tree failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'x'},
                 item_element,
                 &error)) {
    Fail("Insert root/key/branch/x failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'y'},
                 item_element,
                 &error)) {
    Fail("Insert root/key/branch/y failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'z'},
                 item_element,
                 &error)) {
    Fail("Insert root/key/branch/z failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'a'},
                 item_element,
                 &error)) {
    Fail("Insert root/key/branch/a failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}},
                 {'m'},
                 item_element,
                 &error)) {
    Fail("Insert root/key/branch/m failed: " + error);
  }

  grovedb::PathQuery query = grovedb::PathQuery::NewSingleKey({}, {'k', '1'});
  std::vector<uint8_t> proof_bytes;
  if (!db.ProveQueryForVersion(query, grovedb::GroveVersion::Current(), &proof_bytes, &error)) {
    Fail("ProveQueryForVersion failed: " + error);
  }
  if (proof_bytes.empty()) {
    Fail("ProveQueryForVersion returned empty proof");
  }

  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  if (!grovedb::VerifyPathQueryProofForVersion(
          proof_bytes, query, grovedb::GroveVersion::Current(), &root_hash, &elements, &error)) {
    Fail("VerifyPathQueryProofForVersion failed: " + error);
  }
  if (root_hash.size() != 32) {
    Fail("VerifyPathQueryProofForVersion returned invalid root hash size");
  }
  if (elements.size() != 1) {
    Fail("VerifyPathQueryProofForVersion should return exactly one element");
  }
  if (elements[0].path != std::vector<std::vector<uint8_t>>({}) ||
      elements[0].key != std::vector<uint8_t>({'k', '1'}) || !elements[0].has_element ||
      elements[0].element_bytes != item_element) {
    Fail("VerifyPathQueryProofForVersion returned unexpected element payload");
  }

  grovedb::Query layered_query = grovedb::Query::NewSingleKey({'k', 'e', 'y'});
  layered_query.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>({{'b', 'r', 'a', 'n', 'c', 'h'}});
  layered_query.default_subquery_branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleKey({'x'}));
  grovedb::PathQuery layered_path_query =
      grovedb::PathQuery::New({{'r', 'o', 'o', 't'}},
                              grovedb::SizedQuery::New(layered_query, std::nullopt, std::nullopt));
  std::vector<uint8_t> layered_proof;
  if (!db.ProveQueryForVersion(
          layered_path_query, grovedb::GroveVersion::Current(), &layered_proof, &error)) {
    Fail("ProveQueryForVersion layered query failed: " + error);
  }
  if (layered_proof.empty()) {
    Fail("ProveQueryForVersion layered query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> layered_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(layered_proof,
                                               layered_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &layered_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion layered query failed: " + error);
  }
  bool found_x = false;
  for (const auto& entry : layered_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_x = true;
    }
  }
  if (!found_x) {
    Fail("layered subquery proof should include branch/x");
  }

  grovedb::PathQuery range_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeAfter({'x'}));
  std::vector<uint8_t> range_proof;
  if (!db.ProveQueryForVersion(
          range_path_query, grovedb::GroveVersion::Current(), &range_proof, &error)) {
    Fail("ProveQueryForVersion range query failed: " + error);
  }
  if (range_proof.empty()) {
    Fail("ProveQueryForVersion range query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_proof,
                                               range_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range query failed: " + error);
  }
  if (range_elements.size() != 2) {
    Fail("range query proof should return exactly two elements (y and z after x)");
  }
  bool found_range_y = false;
  bool found_range_z = false;
  for (const auto& entry : range_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'y'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_range_y = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'z'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_range_z = true;
    }
  }
  if (!found_range_y || !found_range_z) {
    Fail("range query proof should return both branch/y and branch/z");
  }

  grovedb::PathQuery range_to_inclusive_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeToInclusive({'x'}));
  std::vector<uint8_t> range_to_inclusive_proof;
  if (!db.ProveQueryForVersion(
          range_to_inclusive_path_query,
          grovedb::GroveVersion::Current(),
          &range_to_inclusive_proof,
          &error)) {
    Fail("ProveQueryForVersion range-to-inclusive query failed: " + error);
  }
  if (range_to_inclusive_proof.empty()) {
    Fail("ProveQueryForVersion range-to-inclusive query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_to_inclusive_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_to_inclusive_proof,
                                               range_to_inclusive_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_to_inclusive_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-to-inclusive query failed: " + error);
  }
  // RangeToInclusive({'x'}) should return keys <= 'x': a, m, x (3 elements)
  if (range_to_inclusive_elements.size() != 3) {
    Fail("range-to-inclusive query proof should return exactly three elements (a, m, x)");
  }
  bool found_rti_a = false, found_rti_m = false, found_rti_x = false;
  for (const auto& entry : range_to_inclusive_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'a'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rti_a = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'m'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rti_m = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rti_x = true;
    }
  }
  if (!found_rti_a || !found_rti_m || !found_rti_x) {
    Fail("range-to-inclusive query proof should return branch/a, branch/m, and branch/x");
  }

  grovedb::PathQuery range_to_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeTo({'y'}));
  std::vector<uint8_t> range_to_proof;
  if (!db.ProveQueryForVersion(
          range_to_path_query, grovedb::GroveVersion::Current(), &range_to_proof, &error)) {
    Fail("ProveQueryForVersion range-to query failed: " + error);
  }
  if (range_to_proof.empty()) {
    Fail("ProveQueryForVersion range-to query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_to_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_to_proof,
                                               range_to_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_to_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-to query failed: " + error);
  }
  // RangeTo({'y'}) should return keys < 'y': a, m, x (3 elements)
  if (range_to_elements.size() != 3) {
    Fail("range-to query proof should return exactly three elements (a, m, x)");
  }
  bool found_rt_a = false, found_rt_m = false, found_rt_x = false;
  for (const auto& entry : range_to_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'a'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rt_a = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'m'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rt_m = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rt_x = true;
    }
  }
  if (!found_rt_a || !found_rt_m || !found_rt_x) {
    Fail("range-to query proof should return branch/a, branch/m, and branch/x");
  }

  grovedb::PathQuery range_from_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeFrom({'x'}));
  std::vector<uint8_t> range_from_proof;
  if (!db.ProveQueryForVersion(
          range_from_path_query, grovedb::GroveVersion::Current(), &range_from_proof, &error)) {
    Fail("ProveQueryForVersion range-from query failed: " + error);
  }
  if (range_from_proof.empty()) {
    Fail("ProveQueryForVersion range-from query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_from_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_from_proof,
                                               range_from_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_from_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-from query failed: " + error);
  }
  if (range_from_elements.size() != 3) {
    Fail("range-from query proof should return exactly three elements (x, y, and z)");
  }
  bool found_range_from_x = false;
  bool found_range_from_y = false;
  bool found_range_from_z = false;
  for (const auto& entry : range_from_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_range_from_x = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'y'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_range_from_y = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'z'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_range_from_z = true;
    }
  }
  if (!found_range_from_x || !found_range_from_y || !found_range_from_z) {
    Fail("range-from query proof should return branch/x, branch/y, and branch/z");
  }

  grovedb::PathQuery range_after_to_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeAfterTo({'a'}, {'z'}));
  std::vector<uint8_t> range_after_to_proof;
  if (!db.ProveQueryForVersion(
          range_after_to_path_query, grovedb::GroveVersion::Current(), &range_after_to_proof, &error)) {
    Fail("ProveQueryForVersion range-after-to query failed: " + error);
  }
  if (range_after_to_proof.empty()) {
    Fail("ProveQueryForVersion range-after-to query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_after_to_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_after_to_proof,
                                               range_after_to_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_after_to_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-after-to query failed: " + error);
  }
  // RangeAfterTo({'a'}, {'z'}) should return keys strictly between 'a' and 'z': m, x, y (3 elements)
  if (range_after_to_elements.size() != 3) {
    Fail("range-after-to query proof should return exactly three elements (m, x, y between a and z)");
  }
  bool found_rat_m = false, found_rat_x = false, found_rat_y = false;
  for (const auto& entry : range_after_to_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'m'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rat_m = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rat_x = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'y'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rat_y = true;
    }
  }
  if (!found_rat_m || !found_rat_x || !found_rat_y) {
    Fail("range-after-to query proof should return branch/m, branch/x, and branch/y");
  }

  grovedb::PathQuery range_after_to_with_keys_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeAfterTo({'m'}, {'z'}));
  std::vector<uint8_t> range_after_to_with_keys_proof;
  if (!db.ProveQueryForVersion(range_after_to_with_keys_path_query,
                               grovedb::GroveVersion::Current(),
                               &range_after_to_with_keys_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-after-to with keys query failed: " + error);
  }
  if (range_after_to_with_keys_proof.empty()) {
    Fail("ProveQueryForVersion range-after-to with keys query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_after_to_with_keys_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_after_to_with_keys_proof,
                                               range_after_to_with_keys_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_after_to_with_keys_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-after-to with keys query failed: " + error);
  }
  // RangeAfterTo({'m'}, {'z'}) should return keys strictly between 'm' and 'z': x, y (2 elements)
  if (range_after_to_with_keys_elements.size() != 2) {
    Fail("range-after-to with keys query proof should return exactly two elements (x, y between m and z)");
  }
  bool found_rat2_x = false, found_rat2_y = false;
  for (const auto& entry : range_after_to_with_keys_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rat2_x = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'y'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rat2_y = true;
    }
  }
  if (!found_rat2_x || !found_rat2_y) {
    Fail("range-after-to with keys query proof should return branch/x and branch/y");
  }

  grovedb::PathQuery range_after_to_inclusive_path_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeAfterToInclusive({'x'}, {'y'}));
  std::vector<uint8_t> range_after_to_inclusive_proof;
  if (!db.ProveQueryForVersion(range_after_to_inclusive_path_query,
                               grovedb::GroveVersion::Current(),
                               &range_after_to_inclusive_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-after-to-inclusive query failed: " + error);
  }
  if (range_after_to_inclusive_proof.empty()) {
    Fail("ProveQueryForVersion range-after-to-inclusive query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_after_to_inclusive_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_after_to_inclusive_proof,
                                               range_after_to_inclusive_path_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_after_to_inclusive_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-after-to-inclusive query failed: " + error);
  }
  if (range_after_to_inclusive_elements.size() != 1) {
    Fail("range-after-to-inclusive query proof should return exactly one element");
  }
  if (range_after_to_inclusive_elements[0].path !=
          std::vector<std::vector<uint8_t>>(
              {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) ||
      range_after_to_inclusive_elements[0].key != std::vector<uint8_t>({'y'}) ||
      !range_after_to_inclusive_elements[0].has_element ||
      range_after_to_inclusive_elements[0].element_bytes != item_element) {
    Fail("range-after-to-inclusive query proof should return branch/y");
  }

  // Test RangeInclusive: prove keys in range ['m'..='x'] (inclusive on both ends)
  // Expected: m, x (2 elements - inclusive of both bounds)
  grovedb::PathQuery range_inclusive_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeInclusive({'m'}, {'x'}));
  std::vector<uint8_t> range_inclusive_proof;
  if (!db.ProveQueryForVersion(range_inclusive_query,
                               grovedb::GroveVersion::Current(),
                               &range_inclusive_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-inclusive query failed: " + error);
  }
  if (range_inclusive_proof.empty()) {
    Fail("ProveQueryForVersion range-inclusive query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_inclusive_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_inclusive_proof,
                                               range_inclusive_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_inclusive_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-inclusive query failed: " + error);
  }
  if (range_inclusive_elements.size() != 2) {
    Fail("range-inclusive query proof should return exactly 2 elements (m and x)");
  }
  // First element should be 'm'
  if (range_inclusive_elements[0].path !=
          std::vector<std::vector<uint8_t>>(
              {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) ||
      range_inclusive_elements[0].key != std::vector<uint8_t>({'m'}) ||
      !range_inclusive_elements[0].has_element ||
      range_inclusive_elements[0].element_bytes != item_element) {
    Fail("range-inclusive query proof first element should be branch/m");
  }
  // Second element should be 'x'
  if (range_inclusive_elements[1].path !=
          std::vector<std::vector<uint8_t>>(
              {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) ||
      range_inclusive_elements[1].key != std::vector<uint8_t>({'x'}) ||
      !range_inclusive_elements[1].has_element ||
      range_inclusive_elements[1].element_bytes != item_element) {
    Fail("range-inclusive query proof second element should be branch/x");
  }

  // Test RangeFull: prove all keys in the subtree (no bounds)
  // Expected: a, m, x, y, z (all 5 elements)
  grovedb::PathQuery range_full_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeFull());
  std::vector<uint8_t> range_full_proof;
  if (!db.ProveQueryForVersion(range_full_query,
                               grovedb::GroveVersion::Current(),
                               &range_full_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-full query failed: " + error);
  }
  if (range_full_proof.empty()) {
    Fail("ProveQueryForVersion range-full query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_full_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_full_proof,
                                               range_full_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_full_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-full query failed: " + error);
  }
  if (range_full_elements.size() != 5) {
    Fail("range-full query proof should return exactly 5 elements (a, m, x, y, z)");
  }
  bool found_rf_a = false, found_rf_m = false, found_rf_x = false, found_rf_y = false, found_rf_z = false;
  for (const auto& entry : range_full_elements) {
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'a'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rf_a = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'m'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rf_m = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'x'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rf_x = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'y'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rf_y = true;
    }
    if (entry.path ==
            std::vector<std::vector<uint8_t>>(
                {{'r', 'o', 'o', 't'}, {'k', 'e', 'y'}, {'b', 'r', 'a', 'n', 'c', 'h'}}) &&
        entry.key == std::vector<uint8_t>({'z'}) && entry.has_element &&
        entry.element_bytes == item_element) {
      found_rf_z = true;
    }
  }
  if (!found_rf_a || !found_rf_m || !found_rf_x || !found_rf_y || !found_rf_z) {
    Fail("range-full query proof should return branch/a, branch/m, branch/x, branch/y, and branch/z");
  }

  // Test RangeAfter with absence proof: prove keys after 'z' (non-existent, should return empty)
  // This validates absence proof generation when the query range contains no keys.
  // Use 'z' + 0xff as the lower bound to ensure the key doesn't exist but is > 'z'.
  std::vector<uint8_t> after_z = {'z', static_cast<uint8_t>(0xff)};
  grovedb::PathQuery range_after_absence_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeAfter(after_z));
  std::vector<uint8_t> range_after_absence_proof;
  if (!db.ProveQueryForVersion(range_after_absence_query,
                               grovedb::GroveVersion::Current(),
                               &range_after_absence_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-after-absence query failed: " + error);
  }
  if (range_after_absence_proof.empty()) {
    Fail("ProveQueryForVersion range-after-absence query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_after_absence_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_after_absence_proof,
                                               range_after_absence_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_after_absence_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-after-absence query failed: " + error);
  }
  // RangeAfter({'z', 0xff}) should return empty result (no keys after 'z', 0xff)
  if (!range_after_absence_elements.empty()) {
    Fail("range-after-absence query proof should return empty result (no keys after 'z', 0xff)");
  }

  // Test RangeTo with absence proof: prove keys before 'a' (non-existent, should return empty)
  // This validates absence proof generation when the query upper bound is before all existing keys.
  // Use a key with value 0x00 which is < 'a' (0x61) to ensure no keys exist before it.
  std::vector<uint8_t> before_a = {static_cast<uint8_t>(0x00)};
  grovedb::PathQuery range_to_absence_query =
      grovedb::PathQuery::NewSingleQueryItem({{'r', 'o', 'o', 't'},
                                              {'k', 'e', 'y'},
                                              {'b', 'r', 'a', 'n', 'c', 'h'}},
                                             grovedb::QueryItem::RangeTo(before_a));
  std::vector<uint8_t> range_to_absence_proof;
  if (!db.ProveQueryForVersion(range_to_absence_query,
                               grovedb::GroveVersion::Current(),
                               &range_to_absence_proof,
                               &error)) {
    Fail("ProveQueryForVersion range-to-absence query failed: " + error);
  }
  if (range_to_absence_proof.empty()) {
    Fail("ProveQueryForVersion range-to-absence query returned empty proof");
  }
  std::vector<grovedb::VerifiedPathKeyElement> range_to_absence_elements;
  if (!grovedb::VerifyPathQueryProofForVersion(range_to_absence_proof,
                                               range_to_absence_query,
                                               grovedb::GroveVersion::Current(),
                                               &root_hash,
                                               &range_to_absence_elements,
                                               &error)) {
    Fail("VerifyPathQueryProofForVersion range-to-absence query failed: " + error);
  }
  // RangeTo({0x00}) should return empty result (no keys before 0x00)
  if (!range_to_absence_elements.empty()) {
    Fail("range-to-absence query proof should return empty result (no keys before 0x00)");
  }

  return 0;
}
