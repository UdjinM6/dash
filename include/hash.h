#ifndef GROVEDB_CPP_HASH_H
#define GROVEDB_CPP_HASH_H

#include <cstdint>
#include <string>
#include <vector>

namespace grovedb {

using Hash = std::vector<uint8_t>;

// Hashes a value using the same scheme as Merk (len varint + value, Blake3).
bool ValueHash(const std::vector<uint8_t>& value, Hash* out, std::string* error);

// Hashes a key/value pair using Merk's kv_hash (key len varint + key + value_hash).
bool KvHash(const std::vector<uint8_t>& key,
            const std::vector<uint8_t>& value,
            Hash* out,
            std::string* error);

// Hashes a kv digest (key len varint + key + value_hash).
bool KvDigestToKvHash(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value_hash,
                      Hash* out,
                      std::string* error);

// Combines two hashes using Blake3(hash_one || hash_two).
bool CombineHash(const std::vector<uint8_t>& left,
                 const std::vector<uint8_t>& right,
                 Hash* out,
                 std::string* error);

// Hashes a node (kv hash + left hash + right hash) using Blake3.
bool NodeHash(const std::vector<uint8_t>& kv_hash,
              const std::vector<uint8_t>& left,
              const std::vector<uint8_t>& right,
              Hash* out,
              std::string* error);

// Hashes a node with provable count (kv hash + left + right + count BE bytes).
bool NodeHashWithCount(const std::vector<uint8_t>& kv_hash,
                       const std::vector<uint8_t>& left,
                       const std::vector<uint8_t>& right,
                       uint64_t count,
                       Hash* out,
                       std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_HASH_H
