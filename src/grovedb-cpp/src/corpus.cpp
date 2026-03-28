#include "corpus.h"

#include <fstream>
#include <optional>
#include <sstream>

#include "hex.h"

namespace grovedb {

namespace {

bool ExtractString(const std::string& input, const std::string& key, std::string* out) {
  std::string token = "\"" + key + "\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find(':', pos);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find('"', pos);
  if (pos == std::string::npos) {
    return false;
  }
  size_t end = input.find('"', pos + 1);
  if (end == std::string::npos) {
    return false;
  }
  *out = input.substr(pos + 1, end - pos - 1);
  return true;
}

bool ExtractArrayOfStrings(const std::string& input,
                           const std::string& key,
                           std::vector<std::string>* out) {
  std::string token = "\"" + key + "\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find('[', pos);
  if (pos == std::string::npos) {
    return false;
  }
  size_t end = std::string::npos;
  int bracket_depth = 0;
  for (size_t i = pos; i < input.size(); ++i) {
    char ch = input[i];
    if (ch == '[') {
      bracket_depth += 1;
    } else if (ch == ']') {
      bracket_depth -= 1;
      if (bracket_depth == 0) {
        end = i;
        break;
      }
      if (bracket_depth < 0) {
        return false;
      }
    }
  }
  if (end == std::string::npos) {
    return false;
  }
  std::string slice = input.substr(pos + 1, end - pos - 1);
  out->clear();
  size_t cursor = 0;
  while (true) {
    size_t start_quote = slice.find('"', cursor);
    if (start_quote == std::string::npos) {
      break;
    }
    size_t end_quote = slice.find('"', start_quote + 1);
    if (end_quote == std::string::npos) {
      return false;
    }
    out->push_back(slice.substr(start_quote + 1, end_quote - start_quote - 1));
    cursor = end_quote + 1;
  }
  return true;
}

bool ExtractUnsigned(const std::string& input, const std::string& key, uint64_t* out) {
  std::string token = "\"" + key + "\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find(':', pos);
  if (pos == std::string::npos) {
    return false;
  }
  size_t start = input.find_first_of("0123456789", pos);
  if (start == std::string::npos) {
    return false;
  }
  size_t end = input.find_first_not_of("0123456789", start);
  std::string number = input.substr(start, end - start);
  *out = static_cast<uint64_t>(std::stoull(number));
  return true;
}

bool ExtractBool(const std::string& input, const std::string& key, bool* out) {
  std::string token = "\"" + key + "\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find(':', pos);
  if (pos == std::string::npos) {
    return false;
  }
  size_t start = input.find_first_not_of(" \t\r\n", pos + 1);
  if (start == std::string::npos) {
    return false;
  }
  if (input.compare(start, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (input.compare(start, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool ExtractCost(const std::string& input, CostSummary* cost) {
  uint64_t value = 0;
  if (!ExtractUnsigned(input, "seek_count", &value)) {
    return false;
  }
  cost->seek_count = static_cast<uint32_t>(value);
  if (!ExtractUnsigned(input, "storage_loaded_bytes", &value)) {
    return false;
  }
  cost->storage_loaded_bytes = value;
  if (!ExtractUnsigned(input, "hash_node_calls", &value)) {
    return false;
  }
  cost->hash_node_calls = static_cast<uint32_t>(value);
  if (!ExtractUnsigned(input, "storage_added_bytes", &value)) {
    return false;
  }
  cost->storage_added_bytes = static_cast<uint32_t>(value);
  if (!ExtractUnsigned(input, "storage_replaced_bytes", &value)) {
    return false;
  }
  cost->storage_replaced_bytes = static_cast<uint32_t>(value);
  if (!ExtractString(input, "storage_removed", &cost->storage_removed)) {
    return false;
  }
  return true;
}

bool ExtractCaseBlocks(const std::string& input, std::vector<std::string>* out) {
  std::string token = "\"cases\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find('[', pos);
  if (pos == std::string::npos) {
    return false;
  }
  out->clear();

  int array_depth = 0;
  int object_depth = 0;
  size_t start = std::string::npos;
  for (size_t i = pos; i < input.size(); ++i) {
    char ch = input[i];
    if (ch == '[') {
      array_depth += 1;
      continue;
    }
    if (ch == ']') {
      array_depth -= 1;
      if (array_depth == 0) {
        break;
      }
      if (array_depth < 0) {
        return false;
      }
      continue;
    }
    if (array_depth != 1) {
      continue;
    }
    if (ch == '{') {
      if (object_depth == 0) {
        start = i;
      }
      object_depth += 1;
    } else if (ch == '}') {
      object_depth -= 1;
      if (object_depth == 0 && start != std::string::npos) {
        out->push_back(input.substr(start, i - start + 1));
        start = std::string::npos;
      }
      if (object_depth < 0) {
        return false;
      }
    }
  }
  return !out->empty();
}

bool ExtractObjectBlock(const std::string& input,
                        const std::string& key,
                        std::string* out) {
  std::string token = "\"" + key + "\"";
  size_t pos = input.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  pos = input.find('{', pos);
  if (pos == std::string::npos) {
    return false;
  }
  int depth = 0;
  size_t end = std::string::npos;
  for (size_t i = pos; i < input.size(); ++i) {
    if (input[i] == '{') {
      depth += 1;
    } else if (input[i] == '}') {
      depth -= 1;
      if (depth == 0) {
        end = i;
        break;
      }
      if (depth < 0) {
        return false;
      }
    }
  }
  if (end == std::string::npos) {
    return false;
  }
  *out = input.substr(pos, end - pos + 1);
  return true;
}

bool ExtractQueryDescriptor(const std::string& input,
                            QueryDescriptor* descriptor) {
  std::string block;
  if (!ExtractObjectBlock(input, "query_descriptor", &block)) {
    return false;
  }
  if (!ExtractString(block, "kind", &descriptor->kind)) {
    return false;
  }
  if (!ExtractString(block, "key", &descriptor->key_hex)) {
    return false;
  }
  if (!ExtractArrayOfStrings(block, "keys", &descriptor->keys_hex)) {
    return false;
  }
  if (!ExtractString(block, "start_key", &descriptor->start_key_hex)) {
    return false;
  }
  if (!ExtractString(block, "end_key", &descriptor->end_key_hex)) {
    return false;
  }
  if (!ExtractBool(block, "has_start_key", &descriptor->has_start_key)) {
    return false;
  }
  if (!ExtractBool(block, "has_end_key", &descriptor->has_end_key)) {
    return false;
  }
  if (!ExtractBool(block, "has_limit", &descriptor->has_limit)) {
    return false;
  }
  if (!ExtractBool(block, "has_offset", &descriptor->has_offset)) {
    return false;
  }
  uint64_t value = 0;
  if (!ExtractUnsigned(block, "limit", &value)) {
    return false;
  }
  descriptor->limit = static_cast<uint16_t>(value);
  if (!ExtractUnsigned(block, "offset", &value)) {
    return false;
  }
  descriptor->offset = static_cast<uint16_t>(value);
  return true;
}

}  // namespace

bool LoadCorpus(const std::string& path, Corpus* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }

  std::ifstream input(path);
  if (!input) {
    if (error) {
      *error = "failed to open corpus file";
    }
    return false;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string contents = buffer.str();

  Corpus corpus;
  if (!ExtractString(contents, "version", &corpus.version)) {
    if (error) {
      *error = "missing version";
    }
    return false;
  }

  std::vector<std::string> case_blocks;
  if (!ExtractCaseBlocks(contents, &case_blocks)) {
    if (error) {
      *error = "missing cases array";
    }
    return false;
  }
  for (const auto& block : case_blocks) {
    CorpusCase entry;
    if (!ExtractString(block, "name", &entry.name)) {
      if (error) {
        *error = "missing case name";
      }
      return false;
    }
    if (!ExtractArrayOfStrings(block, "path", &entry.path_hex)) {
      if (error) {
        *error = "missing path array";
      }
      return false;
    }
    if (!ExtractArrayOfStrings(block, "keys", &entry.keys_hex)) {
      if (error) {
        *error = "missing keys";
      }
      return false;
    }
    if (!ExtractArrayOfStrings(block, "element_bytes_list", &entry.element_bytes_hex)) {
      if (error) {
        *error = "missing element_bytes_list";
      }
      return false;
    }
    if (!ExtractArrayOfStrings(block, "item_bytes_list", &entry.item_bytes_hex)) {
      if (error) {
        *error = "missing item_bytes_list";
      }
      return false;
    }
    if (!ExtractQueryDescriptor(block, &entry.query_descriptor)) {
      if (error) {
        *error = "missing query_descriptor";
      }
      return false;
    }
    if (!ExtractBool(block, "expect_present", &entry.expect_present)) {
      if (error) {
        *error = "missing expect_present";
      }
      return false;
    }
    if (!ExtractString(block, "proof", &entry.proof_hex)) {
      if (error) {
        *error = "missing proof";
      }
      return false;
    }
    if (!ExtractString(block, "subtree_key", &entry.subtree_key_hex)) {
      if (error) {
        *error = "missing subtree_key";
      }
      return false;
    }
    if (!ExtractString(block, "root_hash", &entry.root_hash_hex)) {
      if (error) {
        *error = "missing root_hash";
      }
      return false;
    }
    if (!ExtractCost(block, &entry.cost)) {
      if (error) {
        *error = "missing cost fields";
      }
      return false;
    }
    corpus.cases.push_back(entry);
  }
  *out = corpus;
  return true;
}

bool BuildPathQueryFromDescriptor(const CorpusCase& entry,
                                  const std::vector<std::vector<uint8_t>>& path_bytes,
                                  PathQuery* out,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "query output is null";
    }
    return false;
  }
  const auto& descriptor = entry.query_descriptor;
  if (descriptor.kind == "single_key") {
    std::vector<uint8_t> key;
    if (!DecodeHex(descriptor.key_hex, &key, error)) {
      if (error && error->empty()) {
        *error = "failed to decode query_descriptor.key";
      }
      return false;
    }
    *out = PathQuery::NewSingleKey(path_bytes, std::move(key));
    return true;
  }

  Query query;
  if (descriptor.kind == "key_set") {
    for (const auto& key_hex : descriptor.keys_hex) {
      std::vector<uint8_t> key;
      if (!DecodeHex(key_hex, &key, error)) {
        if (error && error->empty()) {
          *error = "failed to decode query_descriptor.keys";
        }
        return false;
      }
      query.items.push_back(QueryItem::Key(std::move(key)));
    }
  } else if (descriptor.kind == "range" ||
             descriptor.kind == "range_inclusive" ||
             descriptor.kind == "range_from" ||
             descriptor.kind == "range_to" ||
             descriptor.kind == "range_to_inclusive" ||
             descriptor.kind == "range_after" ||
             descriptor.kind == "range_after_to" ||
             descriptor.kind == "range_after_to_inclusive") {
    std::vector<uint8_t> start_key;
    std::vector<uint8_t> end_key;
    if (descriptor.has_start_key && !DecodeHex(descriptor.start_key_hex, &start_key, error)) {
      if (error && error->empty()) {
        *error = "failed to decode query_descriptor.start_key";
      }
      return false;
    }
    if (descriptor.has_end_key && !DecodeHex(descriptor.end_key_hex, &end_key, error)) {
      if (error && error->empty()) {
        *error = "failed to decode query_descriptor.end_key";
      }
      return false;
    }
    if (descriptor.kind == "range") {
      query.items.push_back(QueryItem::Range(std::move(start_key), std::move(end_key)));
    } else if (descriptor.kind == "range_inclusive") {
      query.items.push_back(QueryItem::RangeInclusive(std::move(start_key), std::move(end_key)));
    } else if (descriptor.kind == "range_from") {
      query.items.push_back(QueryItem::RangeFrom(std::move(start_key)));
    } else if (descriptor.kind == "range_to") {
      query.items.push_back(QueryItem::RangeTo(std::move(end_key)));
    } else if (descriptor.kind == "range_to_inclusive") {
      query.items.push_back(QueryItem::RangeToInclusive(std::move(end_key)));
    } else if (descriptor.kind == "range_after") {
      query.items.push_back(QueryItem::RangeAfter(std::move(start_key)));
    } else if (descriptor.kind == "range_after_to") {
      query.items.push_back(QueryItem::RangeAfterTo(std::move(start_key), std::move(end_key)));
    } else {
      query.items.push_back(
          QueryItem::RangeAfterToInclusive(std::move(start_key), std::move(end_key)));
    }
  } else if (descriptor.kind == "range_full") {
    query.items.push_back(QueryItem::RangeFull());
  } else {
    if (error) {
      *error = "unsupported query descriptor kind: " + descriptor.kind;
    }
    return false;
  }

  std::optional<uint16_t> limit = std::nullopt;
  std::optional<uint16_t> offset = std::nullopt;
  if (descriptor.has_limit) {
    limit = descriptor.limit;
  }
  if (descriptor.has_offset) {
    offset = descriptor.offset;
  }
  *out = PathQuery::New(path_bytes, SizedQuery::New(std::move(query), limit, offset));
  return true;
}

}  // namespace grovedb
