#include "hash.h"

#include <array>
#include <cstring>

#include "blake3.h"

namespace grovedb {

namespace {

size_t EncodeVarintToArray(uint64_t value, uint8_t out[10]) {
  size_t len = 0;
  while (value >= 0x80) {
    out[len++] = static_cast<uint8_t>(value) | 0x80;
    value >>= 7;
  }
  out[len++] = static_cast<uint8_t>(value);
  return len;
}

bool Blake3HashParts(const uint8_t* part_a,
                     size_t part_a_len,
                     const uint8_t* part_b,
                     size_t part_b_len,
                     const uint8_t* part_c,
                     size_t part_c_len,
                     Hash* out,
                     std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->assign(32, 0);
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  if (part_a_len > 0) {
    blake3_hasher_update(&hasher, part_a, part_a_len);
  }
  if (part_b_len > 0) {
    blake3_hasher_update(&hasher, part_b, part_b_len);
  }
  if (part_c_len > 0) {
    blake3_hasher_update(&hasher, part_c, part_c_len);
  }
  blake3_hasher_finalize(&hasher, out->data(), out->size());
  return true;
}

bool Blake3HashContiguous(const uint8_t* data,
                          size_t len,
                          Hash* out,
                          std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->resize(32);
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  if (len > 0) {
    blake3_hasher_update(&hasher, data, len);
  }
  blake3_hasher_finalize(&hasher, out->data(), out->size());
  return true;
}

}  // namespace

bool ValueHash(const std::vector<uint8_t>& value, Hash* out, std::string* error) {
  uint8_t varint_buf[10];
  const size_t varint_len = EncodeVarintToArray(static_cast<uint64_t>(value.size()),
                                                varint_buf);
  // Common-case small values: build a contiguous stack buffer and hash once.
  if (varint_len + value.size() <= 138) {
    std::array<uint8_t, 138> buffer{};
    std::memcpy(buffer.data(), varint_buf, varint_len);
    if (!value.empty()) {
      std::memcpy(buffer.data() + varint_len, value.data(), value.size());
    }
    return Blake3HashContiguous(buffer.data(), varint_len + value.size(), out, error);
  }
  return Blake3HashParts(varint_buf,
                         varint_len,
                         value.data(),
                         value.size(),
                         nullptr,
                         0,
                         out,
                         error);
}

bool KvHash(const std::vector<uint8_t>& key,
            const std::vector<uint8_t>& value,
            Hash* out,
            std::string* error) {
  Hash value_hash;
  if (!ValueHash(value, &value_hash, error)) {
    return false;
  }
  return KvDigestToKvHash(key, value_hash, out, error);
}

bool KvDigestToKvHash(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value_hash,
                      Hash* out,
                      std::string* error) {
  uint8_t varint_buf[10];
  const size_t varint_len = EncodeVarintToArray(static_cast<uint64_t>(key.size()),
                                                varint_buf);
  // Common-case small keys: hash as one contiguous block.
  if (varint_len + key.size() + value_hash.size() <= 170) {
    std::array<uint8_t, 170> buffer{};
    size_t cursor = 0;
    std::memcpy(buffer.data() + cursor, varint_buf, varint_len);
    cursor += varint_len;
    if (!key.empty()) {
      std::memcpy(buffer.data() + cursor, key.data(), key.size());
      cursor += key.size();
    }
    if (!value_hash.empty()) {
      std::memcpy(buffer.data() + cursor, value_hash.data(), value_hash.size());
      cursor += value_hash.size();
    }
    return Blake3HashContiguous(buffer.data(), cursor, out, error);
  }
  return Blake3HashParts(varint_buf,
                         varint_len,
                         key.data(),
                         key.size(),
                         value_hash.data(),
                         value_hash.size(),
                         out,
                         error);
}

bool CombineHash(const std::vector<uint8_t>& left,
                 const std::vector<uint8_t>& right,
                 Hash* out,
                 std::string* error) {
  if (left.size() != 32 || right.size() != 32) {
    if (error) {
      *error = "hash inputs must be 32 bytes";
    }
    return false;
  }
  std::array<uint8_t, 64> buffer{};
  std::memcpy(buffer.data(), left.data(), left.size());
  std::memcpy(buffer.data() + left.size(), right.data(), right.size());
  return Blake3HashContiguous(buffer.data(), buffer.size(), out, error);
}

bool NodeHash(const std::vector<uint8_t>& kv_hash,
              const std::vector<uint8_t>& left,
              const std::vector<uint8_t>& right,
              Hash* out,
              std::string* error) {
  if (kv_hash.size() != 32 || left.size() != 32 || right.size() != 32) {
    if (error) {
      *error = "hash inputs must be 32 bytes";
    }
    return false;
  }
  std::array<uint8_t, 96> buffer{};
  std::memcpy(buffer.data(), kv_hash.data(), kv_hash.size());
  std::memcpy(buffer.data() + kv_hash.size(), left.data(), left.size());
  std::memcpy(buffer.data() + kv_hash.size() + left.size(), right.data(), right.size());
  return Blake3HashContiguous(buffer.data(), buffer.size(), out, error);
}

bool NodeHashWithCount(const std::vector<uint8_t>& kv_hash,
                       const std::vector<uint8_t>& left,
                       const std::vector<uint8_t>& right,
                       uint64_t count,
                       Hash* out,
                       std::string* error) {
  if (kv_hash.size() != 32 || left.size() != 32 || right.size() != 32) {
    if (error) {
      *error = "hash inputs must be 32 bytes";
    }
    return false;
  }
  std::array<uint8_t, 104> buffer{};
  std::memcpy(buffer.data(), kv_hash.data(), kv_hash.size());
  std::memcpy(buffer.data() + kv_hash.size(), left.data(), left.size());
  std::memcpy(buffer.data() + kv_hash.size() + left.size(), right.data(), right.size());
  uint8_t count_bytes[8];
  for (int i = 7; i >= 0; --i) {
    count_bytes[7 - i] = static_cast<uint8_t>((count >> (i * 8)) & 0xff);
  }
  std::memcpy(buffer.data() + kv_hash.size() + left.size() + right.size(),
              count_bytes,
              sizeof(count_bytes));
  return Blake3HashContiguous(buffer.data(), buffer.size(), out, error);
}

}  // namespace grovedb
