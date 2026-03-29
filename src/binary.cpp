#include "binary.h"

namespace grovedb {

bool ReadBytes(const std::vector<uint8_t>& data,
               size_t* cursor,
               size_t length,
               std::vector<uint8_t>* out,
               std::string* error) {
  if (cursor == nullptr) {
    if (error) {
      *error = "cursor is null";
    }
    return false;
  }
  if (*cursor > data.size() || length > data.size() - *cursor) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  if (out) {
    out->assign(data.begin() + static_cast<long>(*cursor),
                data.begin() + static_cast<long>(*cursor + length));
  }
  *cursor += length;
  return true;
}

bool ReadU16BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint16_t* out,
               std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor > data.size() || 2 > data.size() - *cursor) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  *out = static_cast<uint16_t>(data[*cursor] << 8) |
         static_cast<uint16_t>(data[*cursor + 1]);
  *cursor += 2;
  return true;
}

bool ReadU32BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint32_t* out,
               std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor > data.size() || 4 > data.size() - *cursor) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  *out = (static_cast<uint32_t>(data[*cursor]) << 24) |
         (static_cast<uint32_t>(data[*cursor + 1]) << 16) |
         (static_cast<uint32_t>(data[*cursor + 2]) << 8) |
         static_cast<uint32_t>(data[*cursor + 3]);
  *cursor += 4;
  return true;
}

bool ReadU64BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint64_t* out,
               std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor > data.size() || 8 > data.size() - *cursor) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | data[*cursor + static_cast<size_t>(i)];
  }
  *cursor += 8;
  *out = value;
  return true;
}

bool ReadBincodeVarintU64(const std::vector<uint8_t>& data,
                          size_t* cursor,
                          uint64_t* out,
                          std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor >= data.size()) {
    if (error) {
      *error = "varint truncated";
    }
    return false;
  }
  uint8_t tag = data[*cursor];
  (*cursor)++;
  if (tag <= 250) {
    *out = tag;
    return true;
  }
  if (tag == 251) {
    uint16_t value = 0;
    if (!ReadU16BE(data, cursor, &value, error)) {
      return false;
    }
    *out = value;
    return true;
  }
  if (tag == 252) {
    uint32_t value = 0;
    if (!ReadU32BE(data, cursor, &value, error)) {
      return false;
    }
    *out = value;
    return true;
  }
  if (tag == 253) {
    uint64_t value = 0;
    if (!ReadU64BE(data, cursor, &value, error)) {
      return false;
    }
    *out = value;
    return true;
  }
  if (error) {
    *error = "invalid bincode varint tag";
  }
  return false;
}

bool ReadVarintU64(const std::vector<uint8_t>& data,
                   size_t* cursor,
                   uint64_t* out,
                   std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  uint64_t result = 0;
  int shift = 0;
  while (true) {
    if (*cursor >= data.size()) {
      if (error) {
        *error = "varint truncated";
      }
      return false;
    }
    uint8_t byte = data[*cursor];
    *cursor += 1;
    result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 64) {
      if (error) {
        *error = "varint overflow";
      }
      return false;
    }
  }
}

bool ReadVarintI64(const std::vector<uint8_t>& data,
                   size_t* cursor,
                   int64_t* out,
                   std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  uint64_t raw = 0;
  if (!ReadVarintU64(data, cursor, &raw, error)) {
    return false;
  }
  int64_t value = static_cast<int64_t>((raw >> 1) ^
                                       -static_cast<int64_t>(raw & 1));
  *out = value;
  return true;
}

bool DecodeBincodeVecU8(const std::vector<uint8_t>& data,
                        size_t* cursor,
                        std::vector<uint8_t>* out,
                        std::string* error) {
  uint64_t len = 0;
  if (!ReadBincodeVarintU64(data, cursor, &len, error)) {
    return false;
  }
  return DecodeBincodeVecU8Body(data, cursor, len, out, error);
}

bool DecodeBincodeVecU8Body(const std::vector<uint8_t>& data,
                            size_t* cursor,
                            uint64_t len,
                            std::vector<uint8_t>* out,
                            std::string* error) {
  if (cursor == nullptr) {
    if (error) {
      *error = "cursor is null";
    }
    return false;
  }
  if (*cursor > data.size() || len > static_cast<uint64_t>(data.size() - *cursor)) {
    if (error) {
      *error = "vector length exceeds input size";
    }
    return false;
  }
  return ReadBytes(data, cursor, static_cast<size_t>(len), out, error);
}

void EncodeU16BE(uint16_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(value & 0xff));
}

void EncodeU32BE(uint32_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(value & 0xff));
}

void EncodeU64BE(uint64_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
  }
}

void EncodeVarintU64(uint64_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  while (true) {
    uint8_t byte = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value == 0) {
      out->push_back(byte);
      return;
    }
    out->push_back(static_cast<uint8_t>(byte | 0x80));
  }
}

void EncodeBincodeVarintU64(uint64_t value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  if (value <= 250) {
    out->push_back(static_cast<uint8_t>(value));
    return;
  }
  if (value <= 0xFFFF) {
    out->push_back(251);
    EncodeU16BE(static_cast<uint16_t>(value), out);
    return;
  }
  if (value <= 0xFFFFFFFFu) {
    out->push_back(252);
    EncodeU32BE(static_cast<uint32_t>(value), out);
    return;
  }
  out->push_back(253);
  EncodeU64BE(value, out);
}

void EncodeBincodeVecU8(const std::vector<uint8_t>& value, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return;
  }
  EncodeBincodeVarintU64(static_cast<uint64_t>(value.size()), out);
  out->insert(out->end(), value.begin(), value.end());
}

}  // namespace grovedb
