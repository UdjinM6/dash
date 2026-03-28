#include "query.h"

#include <algorithm>

namespace grovedb {
namespace {

int CompareBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a == b) {
    return 0;
  }
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end()) ? -1 : 1;
}

}  // namespace

SubqueryBranch::SubqueryBranch(const SubqueryBranch& other)
    : subquery_path(other.subquery_path) {
  if (other.subquery) {
    subquery = std::make_unique<Query>(*other.subquery);
  }
}

SubqueryBranch& SubqueryBranch::operator=(const SubqueryBranch& other) {
  if (this == &other) {
    return *this;
  }
  subquery_path = other.subquery_path;
  subquery.reset();
  if (other.subquery) {
    subquery = std::make_unique<Query>(*other.subquery);
  }
  return *this;
}

Query::Query(const Query& other)
    : items(other.items),
      default_subquery_branch(other.default_subquery_branch),
      conditional_subquery_branches(other.conditional_subquery_branches),
      left_to_right(other.left_to_right),
      add_parent_tree_on_subquery(other.add_parent_tree_on_subquery) {}

Query& Query::operator=(const Query& other) {
  if (this == &other) {
    return *this;
  }
  items = other.items;
  default_subquery_branch = other.default_subquery_branch;
  conditional_subquery_branches = other.conditional_subquery_branches;
  left_to_right = other.left_to_right;
  add_parent_tree_on_subquery = other.add_parent_tree_on_subquery;
  return *this;
}

QueryItem QueryItem::Key(std::vector<uint8_t> key) {
  QueryItem item;
  item.type = QueryItemType::kKey;
  item.start = std::move(key);
  return item;
}

QueryItem QueryItem::Range(std::vector<uint8_t> start, std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRange;
  item.start = std::move(start);
  item.end = std::move(end);
  return item;
}

QueryItem QueryItem::RangeInclusive(std::vector<uint8_t> start, std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRangeInclusive;
  item.start = std::move(start);
  item.end = std::move(end);
  return item;
}

QueryItem QueryItem::RangeFull() {
  QueryItem item;
  item.type = QueryItemType::kRangeFull;
  return item;
}

QueryItem QueryItem::RangeFrom(std::vector<uint8_t> start) {
  QueryItem item;
  item.type = QueryItemType::kRangeFrom;
  item.start = std::move(start);
  return item;
}

QueryItem QueryItem::RangeTo(std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRangeTo;
  item.end = std::move(end);
  return item;
}

QueryItem QueryItem::RangeToInclusive(std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRangeToInclusive;
  item.end = std::move(end);
  return item;
}

QueryItem QueryItem::RangeAfter(std::vector<uint8_t> start) {
  QueryItem item;
  item.type = QueryItemType::kRangeAfter;
  item.start = std::move(start);
  return item;
}

QueryItem QueryItem::RangeAfterTo(std::vector<uint8_t> start, std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRangeAfterTo;
  item.start = std::move(start);
  item.end = std::move(end);
  return item;
}

QueryItem QueryItem::RangeAfterToInclusive(std::vector<uint8_t> start,
                                           std::vector<uint8_t> end) {
  QueryItem item;
  item.type = QueryItemType::kRangeAfterToInclusive;
  item.start = std::move(start);
  item.end = std::move(end);
  return item;
}

bool QueryItem::LowerUnbounded() const {
  return type == QueryItemType::kRangeFull ||
         type == QueryItemType::kRangeTo ||
         type == QueryItemType::kRangeToInclusive;
}

bool QueryItem::UpperUnbounded() const {
  return type == QueryItemType::kRangeFull ||
         type == QueryItemType::kRangeFrom ||
         type == QueryItemType::kRangeAfter;
}

uint32_t QueryItem::EnumValue() const {
  switch (type) {
    case QueryItemType::kKey:
      return 0;
    case QueryItemType::kRange:
      return 1;
    case QueryItemType::kRangeInclusive:
      return 2;
    case QueryItemType::kRangeFull:
      return 3;
    case QueryItemType::kRangeFrom:
      return 4;
    case QueryItemType::kRangeTo:
      return 5;
    case QueryItemType::kRangeToInclusive:
      return 6;
    case QueryItemType::kRangeAfter:
      return 7;
    case QueryItemType::kRangeAfterTo:
      return 8;
    case QueryItemType::kRangeAfterToInclusive:
      return 9;
  }
  return 0;
}

std::optional<std::vector<uint8_t>> QueryItem::LowerBound() const {
  switch (type) {
    case QueryItemType::kKey:
    case QueryItemType::kRange:
    case QueryItemType::kRangeInclusive:
    case QueryItemType::kRangeFrom:
    case QueryItemType::kRangeAfter:
    case QueryItemType::kRangeAfterTo:
    case QueryItemType::kRangeAfterToInclusive:
      return start;
    case QueryItemType::kRangeFull:
    case QueryItemType::kRangeTo:
    case QueryItemType::kRangeToInclusive:
      return std::nullopt;
  }
  return std::nullopt;
}

std::pair<std::optional<std::vector<uint8_t>>, bool> QueryItem::UpperBound() const {
  switch (type) {
    case QueryItemType::kKey:
      return {start, true};
    case QueryItemType::kRange:
      return {end, false};
    case QueryItemType::kRangeInclusive:
      return {end, true};
    case QueryItemType::kRangeFull:
    case QueryItemType::kRangeFrom:
    case QueryItemType::kRangeAfter:
      return {std::nullopt, false};
    case QueryItemType::kRangeTo:
      return {end, false};
    case QueryItemType::kRangeToInclusive:
      return {end, true};
    case QueryItemType::kRangeAfterTo:
      return {end, false};
    case QueryItemType::kRangeAfterToInclusive:
      return {end, true};
  }
  return {std::nullopt, false};
}

bool QueryItem::Contains(const std::vector<uint8_t>& key) const {
  const auto lower = LowerBound();
  const auto upper = UpperBound();
  bool lower_ok = LowerUnbounded();
  if (!lower_ok && lower.has_value()) {
    int cmp = CompareBytes(key, lower.value());
    if (cmp > 0) {
      lower_ok = true;
    } else if (cmp == 0) {
      lower_ok = (type != QueryItemType::kRangeAfter &&
                  type != QueryItemType::kRangeAfterTo &&
                  type != QueryItemType::kRangeAfterToInclusive);
    }
  }
  bool upper_ok = UpperUnbounded();
  if (!upper_ok && upper.first.has_value()) {
    int cmp = CompareBytes(key, upper.first.value());
    if (cmp < 0) {
      upper_ok = true;
    } else if (cmp == 0) {
      upper_ok = upper.second;
    }
  }
  return lower_ok && upper_ok;
}

bool QueryItem::IsKey() const {
  return type == QueryItemType::kKey;
}

bool QueryItem::IsRange() const {
  return type != QueryItemType::kKey;
}

bool QueryItem::IsUnboundedRange() const {
  return type != QueryItemType::kKey &&
         type != QueryItemType::kRange &&
         type != QueryItemType::kRangeInclusive;
}

Query Query::NewSingleKey(std::vector<uint8_t> key) {
  Query query;
  query.items.emplace_back(QueryItem::Key(std::move(key)));
  return query;
}

Query Query::NewSingleQueryItem(const QueryItem& item) {
  Query query;
  query.items.push_back(item);
  return query;
}

SizedQuery SizedQuery::New(Query query,
                           std::optional<uint16_t> limit,
                           std::optional<uint16_t> offset) {
  SizedQuery sized;
  sized.query = std::move(query);
  sized.limit = limit;
  sized.offset = offset;
  return sized;
}

SizedQuery SizedQuery::NewSingleKey(std::vector<uint8_t> key) {
  return SizedQuery::New(Query::NewSingleKey(std::move(key)), std::nullopt, std::nullopt);
}

SizedQuery SizedQuery::NewSingleQueryItem(const QueryItem& item) {
  return SizedQuery::New(Query::NewSingleQueryItem(item), std::nullopt, std::nullopt);
}

PathQuery PathQuery::New(std::vector<std::vector<uint8_t>> path, SizedQuery query) {
  PathQuery out;
  out.path = std::move(path);
  out.query = std::move(query);
  return out;
}

PathQuery PathQuery::NewSingleKey(std::vector<std::vector<uint8_t>> path,
                                  std::vector<uint8_t> key) {
  return PathQuery::New(std::move(path), SizedQuery::NewSingleKey(std::move(key)));
}

PathQuery PathQuery::NewSingleQueryItem(std::vector<std::vector<uint8_t>> path,
                                        const QueryItem& item) {
  return PathQuery::New(std::move(path), SizedQuery::NewSingleQueryItem(item));
}

PathTrunkChunkQuery PathTrunkChunkQuery::New(std::vector<std::vector<uint8_t>> path,
                                             uint8_t max_depth,
                                             std::optional<uint8_t> min_depth) {
  return PathTrunkChunkQuery{std::move(path), max_depth, min_depth};
}

PathBranchChunkQuery PathBranchChunkQuery::New(std::vector<std::vector<uint8_t>> path,
                                               std::vector<uint8_t> key,
                                               uint8_t depth) {
  return PathBranchChunkQuery{std::move(path), std::move(key), depth};
}

}  // namespace grovedb
