#include "query.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;

namespace {

std::vector<uint8_t> Key(const char* s) {
  return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}

void AssertContainsBehavior() {
  const auto key_a = Key("a");
  const auto key_b = Key("b");
  const auto key_c = Key("c");

  if (!grovedb::QueryItem::Key(key_b).Contains(key_b)) {
    Fail("key query item should contain exact key");
  }
  if (grovedb::QueryItem::Key(key_b).Contains(key_a)) {
    Fail("key query item should not contain other keys");
  }

  if (!grovedb::QueryItem::Range(key_a, key_c).Contains(key_b)) {
    Fail("exclusive range should contain interior key");
  }
  if (grovedb::QueryItem::Range(key_a, key_c).Contains(key_c)) {
    Fail("exclusive range should exclude upper bound");
  }
  if (!grovedb::QueryItem::RangeInclusive(key_a, key_c).Contains(key_c)) {
    Fail("inclusive range should include upper bound");
  }
  if (grovedb::QueryItem::RangeAfter(key_a).Contains(key_a)) {
    Fail("range-after should exclude lower bound");
  }
  if (!grovedb::QueryItem::RangeAfterTo(key_a, key_c).Contains(key_b)) {
    Fail("range-after-to should contain interior key");
  }
  if (grovedb::QueryItem::RangeAfterTo(key_a, key_c).Contains(key_a)) {
    Fail("range-after-to should exclude lower bound");
  }
  if (!grovedb::QueryItem::RangeAfterToInclusive(key_a, key_c).Contains(key_c)) {
    Fail("range-after-to-inclusive should include upper bound");
  }
}

void AssertCopyContract() {
  grovedb::Query source = grovedb::Query::NewSingleKey(Key("k1"));
  source.default_subquery_branch.subquery_path =
      std::vector<std::vector<uint8_t>>{{'b', 'r', 'a', 'n', 'c', 'h'}};
  source.default_subquery_branch.subquery = std::make_unique<grovedb::Query>(
      grovedb::Query::NewSingleKey(Key("child")));
  source.conditional_subquery_branches = grovedb::ConditionalSubqueryBranches();
  source.conditional_subquery_branches->push_back(
      {grovedb::QueryItem::Key(Key("k1")), grovedb::SubqueryBranch()});

  grovedb::Query copied = source;
  if (copied.default_subquery_branch.subquery == nullptr) {
    Fail("copied query should preserve default subquery");
  }
  if (copied.default_subquery_branch.subquery.get() ==
      source.default_subquery_branch.subquery.get()) {
    Fail("copied query should deep-copy default subquery");
  }
  if (!copied.conditional_subquery_branches.has_value() ||
      copied.conditional_subquery_branches->size() != 1) {
    Fail("copied query should preserve conditional subquery branches");
  }

  source.default_subquery_branch.subquery->items.clear();
  if (copied.default_subquery_branch.subquery->items.empty()) {
    Fail("copied query should be independent from source mutation");
  }
}

void AssertBuilderContract() {
  const auto key = Key("k");
  grovedb::SizedQuery sized =
      grovedb::SizedQuery::New(grovedb::Query::NewSingleKey(key), 7, 2);
  if (!sized.limit.has_value() || *sized.limit != 7 || !sized.offset.has_value() ||
      *sized.offset != 2) {
    Fail("SizedQuery::New should preserve limit/offset");
  }

  grovedb::PathQuery path_query =
      grovedb::PathQuery::NewSingleKey({Key("root"), Key("child")}, key);
  if (path_query.path.size() != 2 || path_query.path[0] != Key("root") ||
      path_query.path[1] != Key("child")) {
    Fail("PathQuery::NewSingleKey should preserve path");
  }
  if (path_query.query.query.items.size() != 1 ||
      !path_query.query.query.items.front().IsKey()) {
    Fail("PathQuery::NewSingleKey should create a single key query item");
  }

  const auto trunk = grovedb::PathTrunkChunkQuery::New({Key("a")}, 4, 2);
  if (trunk.path.size() != 1 || trunk.max_depth != 4 || !trunk.min_depth.has_value() ||
      *trunk.min_depth != 2) {
    Fail("PathTrunkChunkQuery::New should preserve fields");
  }

  const auto branch = grovedb::PathBranchChunkQuery::New({Key("a")}, Key("b"), 3);
  if (branch.path.size() != 1 || branch.key != Key("b") || branch.depth != 3) {
    Fail("PathBranchChunkQuery::New should preserve fields");
  }
}

}  // namespace

int main() {
  AssertContainsBehavior();
  AssertCopyContract();
  AssertBuilderContract();
  return 0;
}
