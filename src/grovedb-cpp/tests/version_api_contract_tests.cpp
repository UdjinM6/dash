#include "grove_version.h"
#include "grovedb.h"
#include "proof.h"
#include "test_utils.h"

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

void AssertBasicContract() {
  const grovedb::GroveVersion current = grovedb::GroveVersion::Current();
  const grovedb::GroveVersion minimum = grovedb::GroveVersion::MinimumSupported();
  if (current != grovedb::GroveVersion::V4_0_0()) {
    Fail("Current() should be v4.0.0");
  }
  if (minimum != grovedb::GroveVersion::V4_0_0()) {
    Fail("MinimumSupported() should be v4.0.0");
  }
  if (current.ToString() != "4.0.0") {
    Fail("ToString() mismatch");
  }
}

void AssertOrderingAndSupport() {
  const grovedb::GroveVersion legacy{3, 9, 9};
  const grovedb::GroveVersion supported{4, 0, 0};
  const grovedb::GroveVersion newer{4, 1, 0};

  if (!(legacy < supported) || !(newer >= supported) || !(legacy != supported)) {
    Fail("version ordering operators mismatch");
  }
  if (legacy.IsSupported()) {
    Fail("legacy version should be unsupported");
  }
  if (!supported.IsSupported() || !newer.IsSupported()) {
    Fail("supported versions should be accepted");
  }

  for (auto feature : {grovedb::GroveFeature::kPathQueryV2,
                       grovedb::GroveFeature::kSumTrees,
                       grovedb::GroveFeature::kBigSumTrees,
                       grovedb::GroveFeature::kCountTrees,
                       grovedb::GroveFeature::kChunkProofs,
                       grovedb::GroveFeature::kReferencePaths,
                       grovedb::GroveFeature::kProvableCountTrees}) {
    if (legacy.Supports(feature)) {
      Fail("legacy version should not support features");
    }
    if (!supported.Supports(feature)) {
      Fail("v4.0.0 should support all declared v4 features");
    }
  }
}

void AssertParseContract() {
  grovedb::GroveVersion parsed;
  std::string error;
  if (!grovedb::GroveVersion::Parse("4.0.0", &parsed, &error)) {
    Fail("Parse(4.0.0) should succeed");
  }
  if (parsed != grovedb::GroveVersion::V4_0_0()) {
    Fail("Parse(4.0.0) value mismatch");
  }

  error.clear();
  if (!grovedb::GroveVersion::Parse("12.34.56", &parsed, &error)) {
    Fail("Parse(valid version) should succeed");
  }
  if (parsed.major != 12 || parsed.minor != 34 || parsed.patch != 56) {
    Fail("Parse(valid version) component mismatch");
  }

  if (grovedb::GroveVersion::Parse("4", &parsed, &error)) {
    Fail("Parse(malformed) should fail");
  }
  if (error != "invalid version format") {
    Fail("Parse(malformed) error mismatch: " + error);
  }

  error.clear();
  if (grovedb::GroveVersion::Parse("4.a.0", &parsed, &error)) {
    Fail("Parse(non-numeric) should fail");
  }
  if (error != "invalid version component") {
    Fail("Parse(non-numeric) error mismatch: " + error);
  }

  error.clear();
  if (grovedb::GroveVersion::Parse("70000.0.0", &parsed, &error)) {
    Fail("Parse(out-of-range) should fail");
  }
  if (error != "invalid version component") {
    Fail("Parse(out-of-range) error mismatch: " + error);
  }

  error.clear();
  if (grovedb::GroveVersion::Parse("4.0.0", nullptr, &error)) {
    Fail("Parse(null output) should fail");
  }
  if (error != "version output is null") {
    Fail("Parse(null output) error mismatch: " + error);
  }
}

void AssertVersionGatedProofContract() {
  std::vector<uint8_t> root_hash;
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  std::string error;
  const grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k'});

  if (grovedb::VerifyPathQueryProofForVersion({0x00},
                                              query,
                                              grovedb::GroveVersion{3, 9, 9},
                                              &root_hash,
                                              &elements,
                                              &error)) {
    Fail("VerifyPathQueryProofForVersion should reject unsupported versions");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (grovedb::VerifyPathQueryProofForVersion({},
                                              query,
                                              grovedb::GroveVersion::Current(),
                                              &root_hash,
                                              &elements,
                                              &error)) {
    Fail("VerifyPathQueryProofForVersion should fail on empty proof");
  }
  if (error != "missing grove proof bytes") {
    Fail("supported-version empty-proof error mismatch: " + error);
  }

  error.clear();
  std::vector<uint8_t> newer_root;
  std::vector<grovedb::VerifiedPathKeyElement> newer_elements;
  if (grovedb::VerifyPathQueryProofForVersion({},
                                              query,
                                              grovedb::GroveVersion{4, 1, 0},
                                              &newer_root,
                                              &newer_elements,
                                              &error)) {
    Fail("newer supported version should fail on empty proof");
  }
  if (error != "missing grove proof bytes") {
    Fail("newer supported-version empty-proof error mismatch: " + error);
  }

  error.clear();
  if (grovedb::VerifySubsetQueryWithAbsenceProofForVersion(
          {0x00},
          query,
          grovedb::GroveVersion{3, 9, 9},
          &root_hash,
          &elements,
          &error)) {
    Fail("VerifySubsetQueryWithAbsenceProofForVersion should reject unsupported versions");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("subset+absence unsupported-version error mismatch: " + error);
  }
}

void AssertLegacyProofApiVersionContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;

  grovedb::SingleKeyProofInput single{};
  if (grovedb::VerifySingleKeyProofForVersion(single, unsupported, &error)) {
    Fail("VerifySingleKeyProofForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("single-key unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (grovedb::VerifySingleKeyAbsenceProofForVersion(single, unsupported, &error)) {
    Fail("VerifySingleKeyAbsenceProofForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("single-key-absence unsupported-version error mismatch: " + error);
  }

  error.clear();
  grovedb::RangeProofInput range{};
  if (grovedb::VerifyRangeProofForVersion(range, unsupported, &error)) {
    Fail("VerifyRangeProofForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("range unsupported-version error mismatch: " + error);
  }
}

void AssertVersionGatedIsEmptyTreeContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  bool is_empty = true;
  grovedb::GroveDb unopened;

  if (unopened.IsEmptyTreeForVersion({}, unsupported, &is_empty, &error)) {
    Fail("IsEmptyTreeForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("IsEmptyTreeForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.IsEmptyTreeForVersion({}, grovedb::GroveVersion::Current(), &is_empty, &error)) {
    Fail("IsEmptyTreeForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("IsEmptyTreeForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.IsEmptyTreeForVersion({}, unsupported, &is_empty, &unopened_tx, &error)) {
    Fail("IsEmptyTreeForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("IsEmptyTreeForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for IsEmptyTreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for IsEmptyTreeForVersion contract: " + error);
  }

  error.clear();
  is_empty = false;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}},
                                grovedb::GroveVersion::Current(),
                                &is_empty,
                                &error)) {
    Fail("IsEmptyTreeForVersion should succeed for supported version: " + error);
  }
  if (!is_empty) {
    Fail("IsEmptyTreeForVersion should report inserted empty subtree as empty");
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for IsEmptyTreeForVersion contract: " + error);
  }
  error.clear();
  is_empty = false;
  if (!db.IsEmptyTreeForVersion({{'r', 'o', 'o', 't'}},
                                grovedb::GroveVersion::Current(),
                                &is_empty,
                                &tx,
                                &error)) {
    Fail("IsEmptyTreeForVersion(tx) should succeed for supported version: " + error);
  }
  if (!is_empty) {
    Fail("IsEmptyTreeForVersion(tx) should report inserted empty subtree as empty");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for IsEmptyTreeForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedDeleteIfEmptyTreeContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  bool deleted = true;
  grovedb::GroveDb unopened;

  if (unopened.DeleteIfEmptyTreeForVersion({}, {'k'}, unsupported, &deleted, &error)) {
    Fail("DeleteIfEmptyTreeForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("DeleteIfEmptyTreeForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.DeleteIfEmptyTreeForVersion(
          {}, {'k'}, grovedb::GroveVersion::Current(), &deleted, &error)) {
    Fail("DeleteIfEmptyTreeForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("DeleteIfEmptyTreeForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.DeleteIfEmptyTreeForVersion({}, {'k'}, unsupported, &deleted, &unopened_tx, &error)) {
    Fail("DeleteIfEmptyTreeForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("DeleteIfEmptyTreeForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for DeleteIfEmptyTreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r'}, &error)) {
    Fail("InsertEmptyTree root failed for DeleteIfEmptyTreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r'}}, {'e'}, &error)) {
    Fail("InsertEmptyTree leaf failed for DeleteIfEmptyTreeForVersion contract: " + error);
  }

  error.clear();
  deleted = false;
  if (!db.DeleteIfEmptyTreeForVersion(
          {{'r'}}, {'e'}, grovedb::GroveVersion::Current(), &deleted, &error)) {
    Fail("DeleteIfEmptyTreeForVersion should succeed for supported version: " + error);
  }
  if (!deleted) {
    Fail("DeleteIfEmptyTreeForVersion should report deleted=true for empty subtree");
  }

  if (!db.InsertEmptyTree({{'r'}}, {'t'}, &error)) {
    Fail("InsertEmptyTree tx leaf failed for DeleteIfEmptyTreeForVersion contract: " + error);
  }
  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for DeleteIfEmptyTreeForVersion contract: " + error);
  }
  error.clear();
  deleted = false;
  if (!db.DeleteIfEmptyTreeForVersion(
          {{'r'}}, {'t'}, grovedb::GroveVersion::Current(), &deleted, &tx, &error)) {
    Fail("DeleteIfEmptyTreeForVersion(tx) should succeed for supported version: " + error);
  }
  if (!deleted) {
    Fail("DeleteIfEmptyTreeForVersion(tx) should report deleted=true for empty subtree");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for DeleteIfEmptyTreeForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedRootHashContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<uint8_t> hash;
  grovedb::GroveDb unopened;

  // Unsupported version -> rejection (non-tx).
  if (unopened.RootHashForVersion(unsupported, &hash, &error)) {
    Fail("RootHashForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("RootHashForVersion unsupported-version error mismatch: " + error);
  }

  // Supported version on unopened DB -> delegation to RootHash (unopened error).
  error.clear();
  if (unopened.RootHashForVersion(grovedb::GroveVersion::Current(), &hash, &error)) {
    Fail("RootHashForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("RootHashForVersion supported-version unopened error mismatch: " + error);
  }

  // Unsupported version -> rejection (tx overload).
  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.RootHashForVersion(unsupported, &hash, &unopened_tx, &error)) {
    Fail("RootHashForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("RootHashForVersion(tx) unsupported-version error mismatch: " + error);
  }

  // Positive path: supported version on open DB returns 32-byte hash.
  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for RootHashForVersion contract: " + error);
  }

  error.clear();
  hash.clear();
  if (!db.RootHashForVersion(grovedb::GroveVersion::Current(), &hash, &error)) {
    Fail("RootHashForVersion should succeed for supported version: " + error);
  }
  if (hash.size() != 32) {
    Fail("RootHashForVersion should return 32-byte hash");
  }

  // Positive path with tx.
  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for RootHashForVersion contract: " + error);
  }
  error.clear();
  hash.clear();
  if (!db.RootHashForVersion(grovedb::GroveVersion::Current(), &hash, &tx, &error)) {
    Fail("RootHashForVersion(tx) should succeed for supported version: " + error);
  }
  if (hash.size() != 32) {
    Fail("RootHashForVersion(tx) should return 32-byte hash");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for RootHashForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedVerifyGroveDbContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<grovedb::VerificationIssue> issues;
  grovedb::GroveDb unopened;

  // Unsupported version -> rejection (non-tx).
  if (unopened.VerifyGroveDbForVersion(unsupported, true, true, &issues, &error)) {
    Fail("VerifyGroveDbForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("VerifyGroveDbForVersion unsupported-version error mismatch: " + error);
  }

  // Supported version on unopened DB -> delegation to VerifyGroveDb (unopened error).
  error.clear();
  issues.clear();
  if (unopened.VerifyGroveDbForVersion(grovedb::GroveVersion::Current(), true, true, &issues, &error)) {
    Fail("VerifyGroveDbForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("VerifyGroveDbForVersion supported-version unopened error mismatch: " + error);
  }

  // Unsupported version -> rejection (tx overload).
  error.clear();
  issues.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.VerifyGroveDbForVersion(unsupported, true, true, &issues, &unopened_tx, &error)) {
    Fail("VerifyGroveDbForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("VerifyGroveDbForVersion(tx) unsupported-version error mismatch: " + error);
  }

  // Positive path: supported version on open DB returns verification result.
  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for VerifyGroveDbForVersion contract: " + error);
  }

  error.clear();
  issues.clear();
  if (!db.VerifyGroveDbForVersion(grovedb::GroveVersion::Current(), true, true, &issues, &error)) {
    Fail("VerifyGroveDbForVersion should succeed for supported version: " + error);
  }
  // For empty DB, should have no issues.
  if (!issues.empty()) {
    Fail("VerifyGroveDbForVersion should have no issues for empty DB");
  }

  // Positive path with tx.
  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for VerifyGroveDbForVersion contract: " + error);
  }
  error.clear();
  issues.clear();
  if (!db.VerifyGroveDbForVersion(grovedb::GroveVersion::Current(), true, true, &issues, &tx, &error)) {
    Fail("VerifyGroveDbForVersion(tx) should succeed for supported version: " + error);
  }
  if (!issues.empty()) {
    Fail("VerifyGroveDbForVersion(tx) should have no issues for empty DB");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for VerifyGroveDbForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedCheckSubtreeExistsInvalidPathContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  grovedb::GroveDb unopened;

  if (unopened.CheckSubtreeExistsInvalidPathForVersion({}, unsupported, &error)) {
    Fail("CheckSubtreeExistsInvalidPathForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("CheckSubtreeExistsInvalidPathForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.CheckSubtreeExistsInvalidPathForVersion(
          {}, grovedb::GroveVersion::Current(), &error)) {
    Fail("CheckSubtreeExistsInvalidPathForVersion should delegate supported version to unopened contract");
  }
  if (error != "database not opened") {
    Fail("CheckSubtreeExistsInvalidPathForVersion supported-version unopened error mismatch: " +
         error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.CheckSubtreeExistsInvalidPathForVersion({}, unsupported, &unopened_tx, &error)) {
    Fail("CheckSubtreeExistsInvalidPathForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("CheckSubtreeExistsInvalidPathForVersion(tx) unsupported-version error mismatch: " +
         error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for CheckSubtreeExistsInvalidPathForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r'}, &error)) {
    Fail("InsertEmptyTree root failed for CheckSubtreeExistsInvalidPathForVersion contract: " +
         error);
  }
  if (!db.InsertEmptyTree({{'r'}}, {'t'}, &error)) {
    Fail("InsertEmptyTree leaf failed for CheckSubtreeExistsInvalidPathForVersion contract: " +
         error);
  }

  error.clear();
  if (!db.CheckSubtreeExistsInvalidPathForVersion(
          {{'r'}, {'t'}}, grovedb::GroveVersion::Current(), &error)) {
    Fail("CheckSubtreeExistsInvalidPathForVersion should succeed for existing subtree: " + error);
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for CheckSubtreeExistsInvalidPathForVersion contract: " +
         error);
  }
  if (!db.InsertEmptyTree({{'r'}}, {'x'}, &tx, &error)) {
    Fail("InsertEmptyTree(tx) failed for CheckSubtreeExistsInvalidPathForVersion contract: " +
         error);
  }

  error.clear();
  if (!db.CheckSubtreeExistsInvalidPathForVersion(
          {{'r'}, {'x'}}, grovedb::GroveVersion::Current(), &tx, &error)) {
    Fail("CheckSubtreeExistsInvalidPathForVersion(tx) should succeed for tx-visible subtree: " +
         error);
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for CheckSubtreeExistsInvalidPathForVersion contract: " +
         rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedQueryRawContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> out;
  const grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k'});
  grovedb::GroveDb unopened;

  if (unopened.QueryRawForVersion(query, unsupported, &out, &error)) {
    Fail("QueryRawForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryRawForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.QueryRawForVersion(query, grovedb::GroveVersion::Current(), &out, &error)) {
    Fail("QueryRawForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("QueryRawForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.QueryRawForVersion(query, unsupported, &out, &unopened_tx, &error)) {
    Fail("QueryRawForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryRawForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for QueryRawForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for QueryRawForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k'}, {'v'}, &error)) {
    Fail("InsertItem failed for QueryRawForVersion contract: " + error);
  }

  error.clear();
  out.clear();
  if (!db.QueryRawForVersion(query, grovedb::GroveVersion::Current(), &out, &error)) {
    Fail("QueryRawForVersion should succeed for supported version: " + error);
  }
  if (out.size() != 1 || out[0].first != std::vector<uint8_t>{'k'}) {
    Fail("QueryRawForVersion supported-version key mismatch");
  }
  grovedb::ElementItem item;
  if (!grovedb::DecodeItemFromElementBytes(out[0].second, &item, &error)) {
    Fail("QueryRawForVersion supported-version decode mismatch: " + error);
  }
  if (item.value != std::vector<uint8_t>{'v'}) {
    Fail("QueryRawForVersion supported-version value mismatch");
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for QueryRawForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t'}, {'x'}, &tx, &error)) {
    Fail("InsertItem(tx) failed for QueryRawForVersion contract: " + error);
  }
  const grovedb::PathQuery tx_query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'t'});

  error.clear();
  out.clear();
  if (!db.QueryRawForVersion(tx_query, grovedb::GroveVersion::Current(), &out, &tx, &error)) {
    Fail("QueryRawForVersion(tx) should succeed for supported version: " + error);
  }
  if (out.size() != 1 || out[0].first != std::vector<uint8_t>{'t'}) {
    Fail("QueryRawForVersion(tx) supported-version key mismatch");
  }
  item = {};
  if (!grovedb::DecodeItemFromElementBytes(out[0].second, &item, &error)) {
    Fail("QueryRawForVersion(tx) supported-version decode mismatch: " + error);
  }
  if (item.value != std::vector<uint8_t>{'x'}) {
    Fail("QueryRawForVersion(tx) supported-version value mismatch");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for QueryRawForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedQuerySumsContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<int64_t> out_sums;
  const grovedb::PathQuery query = grovedb::PathQuery::New(
      {},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 10, std::nullopt));
  grovedb::GroveDb unopened;

  if (unopened.QuerySumsForVersion(query, unsupported, &out_sums, &error)) {
    Fail("QuerySumsForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QuerySumsForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  out_sums.clear();
  if (unopened.QuerySumsForVersion(query, grovedb::GroveVersion::Current(), &out_sums, &error)) {
    Fail("QuerySumsForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("QuerySumsForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.QuerySumsForVersion(query, unsupported, &out_sums, &unopened_tx, &error)) {
    Fail("QuerySumsForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QuerySumsForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for QuerySumsForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for QuerySumsForVersion contract: " + error);
  }
  if (!db.InsertSumItem({}, {'s', '1'}, 42, &error)) {
    Fail("InsertSumItem failed for QuerySumsForVersion contract: " + error);
  }
  if (!db.InsertSumItem({}, {'s', '2'}, 100, &error)) {
    Fail("InsertSumItem failed for QuerySumsForVersion contract: " + error);
  }

  // Test that QuerySumsForVersion delegates to QuerySums correctly
  // by comparing outputs
  std::vector<int64_t> sums_direct, sums_versioned;
  const grovedb::PathQuery sum_query = grovedb::PathQuery::New(
      {},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::RangeFrom({'s'})), 10, std::nullopt));
  
  if (!db.QuerySums(sum_query, &sums_direct, &error)) {
    Fail("QuerySums direct failed: " + error);
  }
  
  error.clear();
  if (!db.QuerySumsForVersion(sum_query, grovedb::GroveVersion::Current(), &sums_versioned, &error)) {
    Fail("QuerySumsForVersion should succeed for supported version: " + error);
  }
  
  if (sums_direct.size() != sums_versioned.size()) {
    Fail("QuerySumsForVersion count should match QuerySums: direct=" + 
         std::to_string(sums_direct.size()) + " versioned=" + std::to_string(sums_versioned.size()));
  }
  
  for (size_t i = 0; i < sums_direct.size(); ++i) {
    if (sums_direct[i] != sums_versioned[i]) {
      Fail("QuerySumsForVersion value mismatch at index " + std::to_string(i));
    }
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for QuerySumsForVersion contract: " + error);
  }
  if (!db.InsertSumItem({}, {'s', '3'}, 200, &tx, &error)) {
    Fail("InsertSumItem(tx) failed for QuerySumsForVersion contract: " + error);
  }

  sums_direct.clear();
  sums_versioned.clear();
  error.clear();
  if (!db.QuerySums(sum_query, &sums_direct, &tx, &error)) {
    Fail("QuerySums(tx) direct failed: " + error);
  }
  
  if (!db.QuerySumsForVersion(sum_query, grovedb::GroveVersion::Current(), &sums_versioned, &tx, &error)) {
    Fail("QuerySumsForVersion(tx) should succeed for supported version: " + error);
  }
  
  if (sums_direct.size() != sums_versioned.size()) {
    Fail("QuerySumsForVersion(tx) count should match QuerySums(tx): direct=" + 
         std::to_string(sums_direct.size()) + " versioned=" + std::to_string(sums_versioned.size()));
  }
  
  for (size_t i = 0; i < sums_direct.size(); ++i) {
    if (sums_direct[i] != sums_versioned[i]) {
      Fail("QuerySumsForVersion(tx) value mismatch at index " + std::to_string(i));
    }
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for QuerySumsForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedQueryItemValueContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<std::vector<uint8_t>> out_values;
  const grovedb::PathQuery query = grovedb::PathQuery::New(
      {},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 10, std::nullopt));
  grovedb::GroveDb unopened;

  if (unopened.QueryItemValueForVersion(query, unsupported, &out_values, &error)) {
    Fail("QueryItemValueForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryItemValueForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  out_values.clear();
  if (unopened.QueryItemValueForVersion(query, grovedb::GroveVersion::Current(), &out_values, &error)) {
    Fail("QueryItemValueForVersion should delegate to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("QueryItemValueForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.QueryItemValueForVersion(query, unsupported, &out_values, &unopened_tx, &error)) {
    Fail("QueryItemValueForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryItemValueForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for QueryItemValueForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for QueryItemValueForVersion contract: " + error);
  }
  if (!db.InsertItem({}, {'k', '1'}, {'v', '1'}, &error)) {
    Fail("InsertItem failed for QueryItemValueForVersion contract: " + error);
  }
  if (!db.InsertItem({}, {'k', '2'}, {'v', '2'}, &error)) {
    Fail("InsertItem failed for QueryItemValueForVersion contract: " + error);
  }

  // Test that QueryItemValueForVersion delegates to QueryItemValue correctly
  std::vector<std::vector<uint8_t>> values_direct, values_versioned;
  const grovedb::PathQuery item_query = grovedb::PathQuery::New(
      {},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k', '1'})), 10, std::nullopt));

  if (!db.QueryItemValue(item_query, &values_direct, &error)) {
    Fail("QueryItemValue direct failed: " + error);
  }

  error.clear();
  if (!db.QueryItemValueForVersion(item_query, grovedb::GroveVersion::Current(), &values_versioned, &error)) {
    Fail("QueryItemValueForVersion should succeed for supported version: " + error);
  }

  if (values_direct.size() != values_versioned.size()) {
    Fail("QueryItemValueForVersion count mismatch: direct=" +
         std::to_string(values_direct.size()) + " versioned=" + std::to_string(values_versioned.size()));
  }

  for (size_t i = 0; i < values_direct.size(); ++i) {
    if (values_direct[i] != values_versioned[i]) {
      Fail("QueryItemValueForVersion value mismatch at index " + std::to_string(i));
    }
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for QueryItemValueForVersion contract: " + error);
  }
  if (!db.InsertItem({}, {'k', '3'}, {'v', '3'}, &tx, &error)) {
    Fail("InsertItem(tx) failed for QueryItemValueForVersion contract: " + error);
  }

  values_direct.clear();
  values_versioned.clear();
  error.clear();
  const grovedb::PathQuery item_query_tx = grovedb::PathQuery::New(
      {},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k', '3'})), 10, std::nullopt));
  if (!db.QueryItemValue(item_query_tx, &values_direct, &tx, &error)) {
    Fail("QueryItemValue(tx) direct failed: " + error);
  }

  if (!db.QueryItemValueForVersion(item_query_tx, grovedb::GroveVersion::Current(), &values_versioned, &tx, &error)) {
    Fail("QueryItemValueForVersion(tx) should succeed for supported version: " + error);
  }

  if (values_direct.size() != values_versioned.size()) {
    Fail("QueryItemValueForVersion(tx) count mismatch: direct=" +
         std::to_string(values_direct.size()) + " versioned=" + std::to_string(values_versioned.size()));
  }

  for (size_t i = 0; i < values_direct.size(); ++i) {
    if (values_direct[i] != values_versioned[i]) {
      Fail("QueryItemValueForVersion(tx) value mismatch at index " + std::to_string(i));
    }
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for QueryItemValueForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedCountSubtreesContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  uint64_t out_count = 0;
  grovedb::GroveDb unopened;

  if (unopened.CountSubtreesForVersion({}, unsupported, &out_count, &error)) {
    Fail("CountSubtreesForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("CountSubtreesForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  out_count = 0;
  if (unopened.CountSubtreesForVersion({}, grovedb::GroveVersion::Current(), &out_count, &error)) {
    Fail("CountSubtreesForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("CountSubtreesForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.CountSubtreesForVersion({}, unsupported, &out_count, &unopened_tx, &error)) {
    Fail("CountSubtreesForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("CountSubtreesForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for CountSubtreesForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for CountSubtreesForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', '1'}, &error)) {
    Fail("InsertEmptyTree(child1) failed for CountSubtreesForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', '2'}, &error)) {
    Fail("InsertEmptyTree(child2) failed for CountSubtreesForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}, {'c', '1'}}, {'g', '1'}, &error)) {
    Fail("InsertEmptyTree(grandchild1) failed for CountSubtreesForVersion contract: " + error);
  }

  uint64_t count_direct = 0;
  uint64_t count_versioned = 0;

  if (!db.CountSubtrees({}, &count_direct, &error)) {
    Fail("CountSubtrees direct failed: " + error);
  }

  error.clear();
  if (!db.CountSubtreesForVersion({}, grovedb::GroveVersion::Current(), &count_versioned, &error)) {
    Fail("CountSubtreesForVersion should succeed for supported version: " + error);
  }

  if (count_direct != count_versioned) {
    Fail("CountSubtreesForVersion count mismatch: direct=" + std::to_string(count_direct) +
         " versioned=" + std::to_string(count_versioned));
  }
  if (count_direct != 4) {
    Fail("CountSubtrees should return 4 (root + c1 + c2 + g1): got " + std::to_string(count_direct));
  }

  uint64_t child_count_direct = 0;
  uint64_t child_count_versioned = 0;
  if (!db.CountSubtrees({{'r', 'o', 'o', 't'}}, &child_count_direct, &error)) {
    Fail("CountSubtrees(child) direct failed: " + error);
  }

  error.clear();
  if (!db.CountSubtreesForVersion({{'r', 'o', 'o', 't'}}, grovedb::GroveVersion::Current(), &child_count_versioned, &error)) {
    Fail("CountSubtreesForVersion(child) should succeed for supported version: " + error);
  }

  if (child_count_direct != child_count_versioned) {
    Fail("CountSubtreesForVersion(child) count mismatch: direct=" + std::to_string(child_count_direct) +
         " versioned=" + std::to_string(child_count_versioned));
  }
  if (child_count_direct != 3) {
    Fail("CountSubtrees(child) should return 3 (c1 + c2 + g1): got " + std::to_string(child_count_direct));
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for CountSubtreesForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'c', '3'}, &tx, &error)) {
    Fail("InsertEmptyTree(c3, tx) failed for CountSubtreesForVersion contract: " + error);
  }

  count_direct = 0;
  count_versioned = 0;
  error.clear();
  if (!db.CountSubtrees({}, &count_direct, &tx, &error)) {
    Fail("CountSubtrees(tx) direct failed: " + error);
  }

  if (!db.CountSubtreesForVersion({}, grovedb::GroveVersion::Current(), &count_versioned, &tx, &error)) {
    Fail("CountSubtreesForVersion(tx) should succeed for supported version: " + error);
  }

  if (count_direct != count_versioned) {
    Fail("CountSubtreesForVersion(tx) count mismatch: direct=" + std::to_string(count_direct) +
         " versioned=" + std::to_string(count_versioned));
  }
  if (count_direct != 5) {
    Fail("CountSubtrees(tx) should return 5 (root + c1 + c2 + g1 + c3): got " + std::to_string(count_direct));
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for CountSubtreesForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedDeleteSubtreeContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  const std::vector<std::vector<uint8_t>> path = {{'r', 'o', 'o', 't'}};
  const std::vector<uint8_t> key = {'s', 'u', 'b'};
  grovedb::GroveDb unopened;

  if (unopened.DeleteSubtreeForVersion(path, key, unsupported, &error)) {
    Fail("DeleteSubtreeForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("DeleteSubtreeForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.DeleteSubtreeForVersion(path, key, grovedb::GroveVersion::Current(), &error)) {
    Fail("DeleteSubtreeForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("DeleteSubtreeForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.DeleteSubtreeForVersion(path, key, unsupported, &unopened_tx, &error)) {
    Fail("DeleteSubtreeForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("DeleteSubtreeForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, &error)) {
    Fail("InsertEmptyTree(sub) failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'s', 'u', 'b'}}, {'k'}, {'v'}, &error)) {
    Fail("InsertItem failed for DeleteSubtreeForVersion contract: " + error);
  }

  grovedb::OperationCost cost_direct;
  if (!db.DeleteSubtree({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, &cost_direct, &error)) {
    Fail("DeleteSubtree direct should succeed: " + error);
  }

  error.clear();
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, &error)) {
    Fail("InsertEmptyTree(sub) re-insert failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'s', 'u', 'b'}}, {'k'}, {'v'}, &error)) {
    Fail("InsertItem re-insert failed for DeleteSubtreeForVersion contract: " + error);
  }

  grovedb::OperationCost cost_versioned;
  if (!db.DeleteSubtreeForVersion({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, grovedb::GroveVersion::Current(), &cost_versioned, &error)) {
    Fail("DeleteSubtreeForVersion should succeed for supported version: " + error);
  }

  // After DeleteSubtree, the subtree element should be deleted from the parent
  bool is_empty_check = false;
  std::string check_error;
  if (db.IsEmptyTree({{'r', 'o', 'o', 't'}, {'s', 'u', 'b'}}, &is_empty_check, &check_error)) {
    Fail("IsEmptyTree should fail after DeleteSubtree (path should not exist): " + check_error);
  }
  // Expected error: path not found
  if (check_error.find("path not found") == std::string::npos) {
    Fail("IsEmptyTree should fail with 'path not found' after DeleteSubtree: " + check_error);
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, &tx, &error)) {
    Fail("InsertEmptyTree(sub, tx) failed for DeleteSubtreeForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}, {'s', 'u', 'b'}}, {'k'}, {'v'}, &tx, &error)) {
    Fail("InsertItem(tx) failed for DeleteSubtreeForVersion contract: " + error);
  }

  if (!db.DeleteSubtreeForVersion({{'r', 'o', 'o', 't'}}, {'s', 'u', 'b'}, grovedb::GroveVersion::Current(), &tx, &error)) {
    Fail("DeleteSubtreeForVersion(tx) should succeed for supported version: " + error);
  }

  // After DeleteSubtree in tx, the subtree element should be deleted from the parent (in tx context)
  bool is_empty = false;
  std::string tx_check_error;
  if (db.IsEmptyTree({{'r', 'o', 'o', 't'}, {'s', 'u', 'b'}}, &is_empty, &tx, &tx_check_error)) {
    Fail("IsEmptyTree(tx) should fail after DeleteSubtree(tx) (path should not exist): " + tx_check_error);
  }
  // Expected error: path not found
  if (tx_check_error.find("path not found") == std::string::npos) {
    Fail("IsEmptyTree(tx) should fail with 'path not found' after DeleteSubtree(tx): " + tx_check_error);
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for DeleteSubtreeForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertVersionGatedQueryKeysOptionalContract() {
  const grovedb::GroveVersion unsupported{3, 9, 9};
  std::string error;
  std::vector<grovedb::GroveDb::PathKeyOptionalElement> out;
  const grovedb::PathQuery query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'k'})), 10, std::nullopt));
  grovedb::GroveDb unopened;

  if (unopened.QueryKeysOptionalForVersion(query, unsupported, &out, &error)) {
    Fail("QueryKeysOptionalForVersion should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryKeysOptionalForVersion unsupported-version error mismatch: " + error);
  }

  error.clear();
  if (unopened.QueryKeysOptionalForVersion(query, grovedb::GroveVersion::Current(), &out, &error)) {
    Fail("QueryKeysOptionalForVersion should delegate supported version to regular unopened contract");
  }
  if (error != "database not opened") {
    Fail("QueryKeysOptionalForVersion supported-version unopened error mismatch: " + error);
  }

  error.clear();
  grovedb::GroveDb::Transaction unopened_tx;
  if (unopened.QueryKeysOptionalForVersion(query, unsupported, &out, &unopened_tx, &error)) {
    Fail("QueryKeysOptionalForVersion(tx) should reject unsupported version");
  }
  if (error != "unsupported grove version: 3.9.9") {
    Fail("QueryKeysOptionalForVersion(tx) unsupported-version error mismatch: " + error);
  }

  const std::string dir = MakeTempDir("version_contract");
  grovedb::GroveDb db;
  if (!db.Open(dir, &error)) {
    Fail("open failed for QueryKeysOptionalForVersion contract: " + error);
  }
  if (!db.InsertEmptyTree({}, {'r', 'o', 'o', 't'}, &error)) {
    Fail("InsertEmptyTree failed for QueryKeysOptionalForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'k'}, {'v'}, &error)) {
    Fail("InsertItem failed for QueryKeysOptionalForVersion contract: " + error);
  }

  error.clear();
  out.clear();
  if (!db.QueryKeysOptionalForVersion(query, grovedb::GroveVersion::Current(), &out, &error)) {
    Fail("QueryKeysOptionalForVersion should succeed for supported version: " + error);
  }
  if (out.size() != 1 || out[0].key != std::vector<uint8_t>{'k'}) {
    Fail("QueryKeysOptionalForVersion supported-version key mismatch");
  }
  grovedb::ElementItem item;
  if (!out[0].element_found ||
      !grovedb::DecodeItemFromElementBytes(out[0].element_bytes, &item, &error)) {
    Fail("QueryKeysOptionalForVersion supported-version element decode mismatch: " + error);
  }
  if (item.value != std::vector<uint8_t>{'v'}) {
    Fail("QueryKeysOptionalForVersion supported-version value mismatch");
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed for QueryKeysOptionalForVersion contract: " + error);
  }
  if (!db.InsertItem({{'r', 'o', 'o', 't'}}, {'t'}, {'x'}, &tx, &error)) {
    Fail("InsertItem(tx) failed for QueryKeysOptionalForVersion contract: " + error);
  }
  const grovedb::PathQuery tx_query = grovedb::PathQuery::New(
      {{'r', 'o', 'o', 't'}},
      grovedb::SizedQuery::New(
          grovedb::Query::NewSingleQueryItem(grovedb::QueryItem::Key({'t'})), 10, std::nullopt));

  error.clear();
  out.clear();
  if (!db.QueryKeysOptionalForVersion(tx_query, grovedb::GroveVersion::Current(), &out, &tx, &error)) {
    Fail("QueryKeysOptionalForVersion(tx) should succeed for supported version: " + error);
  }
  if (out.size() != 1 || out[0].key != std::vector<uint8_t>{'t'}) {
    Fail("QueryKeysOptionalForVersion(tx) supported-version key mismatch");
  }
  item = {};
  if (!out[0].element_found ||
      !grovedb::DecodeItemFromElementBytes(out[0].element_bytes, &item, &error)) {
    Fail("QueryKeysOptionalForVersion(tx) supported-version element decode mismatch: " + error);
  }
  if (item.value != std::vector<uint8_t>{'x'}) {
    Fail("QueryKeysOptionalForVersion(tx) supported-version value mismatch");
  }

  std::string rollback_error;
  if (!db.RollbackTransaction(&tx, &rollback_error)) {
    Fail("RollbackTransaction failed for QueryKeysOptionalForVersion contract: " + rollback_error);
  }
  std::filesystem::remove_all(dir);
}

void AssertNonLatestVersionFixtureParity() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return;
  }

  const std::string dir = MakeTempDir("version_contract");
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_grovedb_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb proof writer");
  }

  const std::vector<uint8_t> proof =
      ReadFile(std::filesystem::path(dir) / "grove_proof_present.bin");
  const grovedb::PathQuery query =
      grovedb::PathQuery::NewSingleKey({{'r', 'o', 'o', 't'}}, {'k', '2'});

  std::vector<uint8_t> current_root;
  std::vector<grovedb::VerifiedPathKeyElement> current_elements;
  std::vector<uint8_t> non_latest_root;
  std::vector<grovedb::VerifiedPathKeyElement> non_latest_elements;
  std::vector<uint8_t> direct_root;
  std::vector<grovedb::VerifiedPathKeyElement> direct_elements;
  std::string error;

  if (!grovedb::VerifyPathQueryProofForVersion(proof,
                                               query,
                                               grovedb::GroveVersion::Current(),
                                               &current_root,
                                               &current_elements,
                                               &error)) {
    Fail("current version proof verify failed: " + error);
  }

  error.clear();
  if (!grovedb::VerifyPathQueryProofForVersion(proof,
                                               query,
                                               grovedb::GroveVersion{4, 1, 0},
                                               &non_latest_root,
                                               &non_latest_elements,
                                               &error)) {
    Fail("non-latest supported version proof verify failed: " + error);
  }

  error.clear();
  if (!grovedb::VerifyPathQueryProof(proof, query, &direct_root, &direct_elements, &error)) {
    Fail("direct proof verify failed: " + error);
  }

  if (current_root != non_latest_root || current_root != direct_root) {
    Fail("root hash mismatch across supported versions");
  }
  if (!SameVerifiedElements(current_elements, non_latest_elements) ||
      !SameVerifiedElements(current_elements, direct_elements)) {
    Fail("verified element mismatch across supported versions");
  }

  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  AssertBasicContract();
  AssertOrderingAndSupport();
  AssertParseContract();
  AssertVersionGatedProofContract();
  AssertLegacyProofApiVersionContract();
  AssertVersionGatedIsEmptyTreeContract();
  AssertVersionGatedDeleteIfEmptyTreeContract();
  AssertVersionGatedRootHashContract();
  AssertVersionGatedVerifyGroveDbContract();
  AssertVersionGatedCheckSubtreeExistsInvalidPathContract();
  AssertVersionGatedQueryRawContract();
  AssertVersionGatedQuerySumsContract();
  AssertVersionGatedQueryItemValueContract();
  AssertVersionGatedCountSubtreesContract();
  AssertVersionGatedDeleteSubtreeContract();
  AssertVersionGatedQueryKeysOptionalContract();
  AssertNonLatestVersionFixtureParity();
  return 0;
}
