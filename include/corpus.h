#ifndef GROVEDB_CPP_CORPUS_H
#define GROVEDB_CPP_CORPUS_H

#include <cstdint>
#include <string>
#include <vector>

#include "query.h"

namespace grovedb {

struct CostSummary {
  uint32_t seek_count = 0;
  uint64_t storage_loaded_bytes = 0;
  uint32_t hash_node_calls = 0;
  uint32_t storage_added_bytes = 0;
  uint32_t storage_replaced_bytes = 0;
  std::string storage_removed;
};

struct QueryDescriptor {
  std::string kind;
  std::string key_hex;
  std::vector<std::string> keys_hex;
  std::string start_key_hex;
  std::string end_key_hex;
  bool has_start_key = false;
  bool has_end_key = false;
  bool has_limit = false;
  bool has_offset = false;
  uint16_t limit = 0;
  uint16_t offset = 0;
};

struct CorpusCase {
  std::string name;
  std::vector<std::string> path_hex;
  std::vector<std::string> keys_hex;
  std::vector<std::string> element_bytes_hex;
  std::vector<std::string> item_bytes_hex;
  QueryDescriptor query_descriptor;
  bool expect_present = true;
  std::string proof_hex;
  std::string subtree_key_hex;
  std::string root_hash_hex;
  CostSummary cost;
};

struct Corpus {
  std::string version;
  std::vector<CorpusCase> cases;
};

// Loads the corpus JSON from disk.
bool LoadCorpus(const std::string& path, Corpus* out, std::string* error);

// Builds a PathQuery from a corpus case query descriptor and decoded path.
bool BuildPathQueryFromDescriptor(const CorpusCase& entry,
                                  const std::vector<std::vector<uint8_t>>& path_bytes,
                                  PathQuery* out,
                                  std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_CORPUS_H
