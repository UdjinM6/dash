#ifndef GROVEDB_CPP_QUERY_H
#define GROVEDB_CPP_QUERY_H

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace grovedb {

struct Query;

enum class QueryItemType {
  kKey,
  kRange,
  kRangeInclusive,
  kRangeFull,
  kRangeFrom,
  kRangeTo,
  kRangeToInclusive,
  kRangeAfter,
  kRangeAfterTo,
  kRangeAfterToInclusive,
};

struct QueryItem {
  QueryItemType type = QueryItemType::kKey;
  std::vector<uint8_t> start;
  std::vector<uint8_t> end;

  static QueryItem Key(std::vector<uint8_t> key);
  static QueryItem Range(std::vector<uint8_t> start, std::vector<uint8_t> end);
  static QueryItem RangeInclusive(std::vector<uint8_t> start, std::vector<uint8_t> end);
  static QueryItem RangeFull();
  static QueryItem RangeFrom(std::vector<uint8_t> start);
  static QueryItem RangeTo(std::vector<uint8_t> end);
  static QueryItem RangeToInclusive(std::vector<uint8_t> end);
  static QueryItem RangeAfter(std::vector<uint8_t> start);
  static QueryItem RangeAfterTo(std::vector<uint8_t> start, std::vector<uint8_t> end);
  static QueryItem RangeAfterToInclusive(std::vector<uint8_t> start, std::vector<uint8_t> end);

  bool Contains(const std::vector<uint8_t>& key) const;
  bool IsKey() const;
  bool IsRange() const;
  bool IsUnboundedRange() const;
  bool UpperUnbounded() const;
  bool LowerUnbounded() const;
  uint32_t EnumValue() const;
  std::optional<std::vector<uint8_t>> LowerBound() const;
  std::pair<std::optional<std::vector<uint8_t>>, bool> UpperBound() const;
};

struct SubqueryBranch {
  std::optional<std::vector<std::vector<uint8_t>>> subquery_path;
  std::unique_ptr<Query> subquery;

  SubqueryBranch() = default;
  SubqueryBranch(const SubqueryBranch& other);
  SubqueryBranch& operator=(const SubqueryBranch& other);
  SubqueryBranch(SubqueryBranch&&) noexcept = default;
  SubqueryBranch& operator=(SubqueryBranch&&) noexcept = default;
};

using ConditionalSubqueryBranches = std::vector<std::pair<QueryItem, SubqueryBranch>>;

struct Query {
  std::vector<QueryItem> items;
  SubqueryBranch default_subquery_branch;
  std::optional<ConditionalSubqueryBranches> conditional_subquery_branches;
  bool left_to_right = true;
  bool add_parent_tree_on_subquery = false;

  Query() = default;
  Query(const Query& other);
  Query& operator=(const Query& other);
  Query(Query&&) noexcept = default;
  Query& operator=(Query&&) noexcept = default;

  static Query NewSingleKey(std::vector<uint8_t> key);
  static Query NewSingleQueryItem(const QueryItem& item);
};

struct SizedQuery {
  Query query;
  std::optional<uint16_t> limit;
  std::optional<uint16_t> offset;

  static SizedQuery New(Query query,
                        std::optional<uint16_t> limit,
                        std::optional<uint16_t> offset);
  static SizedQuery NewSingleKey(std::vector<uint8_t> key);
  static SizedQuery NewSingleQueryItem(const QueryItem& item);
};

struct PathQuery {
  std::vector<std::vector<uint8_t>> path;
  SizedQuery query;

  static PathQuery New(std::vector<std::vector<uint8_t>> path, SizedQuery query);
  static PathQuery NewSingleKey(std::vector<std::vector<uint8_t>> path,
                                std::vector<uint8_t> key);
  static PathQuery NewSingleQueryItem(std::vector<std::vector<uint8_t>> path,
                                      const QueryItem& item);
};

struct PathTrunkChunkQuery {
  std::vector<std::vector<uint8_t>> path;
  uint8_t max_depth = 0;
  std::optional<uint8_t> min_depth;

  static PathTrunkChunkQuery New(std::vector<std::vector<uint8_t>> path,
                                 uint8_t max_depth,
                                 std::optional<uint8_t> min_depth);
};

struct PathBranchChunkQuery {
  std::vector<std::vector<uint8_t>> path;
  std::vector<uint8_t> key;
  uint8_t depth = 0;

  static PathBranchChunkQuery New(std::vector<std::vector<uint8_t>> path,
                                  std::vector<uint8_t> key,
                                  uint8_t depth);
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_QUERY_H
