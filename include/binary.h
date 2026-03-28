#ifndef GROVEDB_CPP_BINARY_H
#define GROVEDB_CPP_BINARY_H

#include <cstdint>
#include <string>
#include <vector>

namespace grovedb {

bool ReadBytes(const std::vector<uint8_t>& data,
               size_t* cursor,
               size_t length,
               std::vector<uint8_t>* out,
               std::string* error);

bool ReadU16BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint16_t* out,
               std::string* error);

bool ReadU32BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint32_t* out,
               std::string* error);

bool ReadU64BE(const std::vector<uint8_t>& data,
               size_t* cursor,
               uint64_t* out,
               std::string* error);

bool ReadBincodeVarintU64(const std::vector<uint8_t>& data,
                          size_t* cursor,
                          uint64_t* out,
                          std::string* error);

bool ReadVarintU64(const std::vector<uint8_t>& data,
                   size_t* cursor,
                   uint64_t* out,
                   std::string* error);

bool ReadVarintI64(const std::vector<uint8_t>& data,
                   size_t* cursor,
                   int64_t* out,
                   std::string* error);

bool DecodeBincodeVecU8(const std::vector<uint8_t>& data,
                        size_t* cursor,
                        std::vector<uint8_t>* out,
                        std::string* error);
bool DecodeBincodeVecU8Body(const std::vector<uint8_t>& data,
                            size_t* cursor,
                            uint64_t len,
                            std::vector<uint8_t>* out,
                            std::string* error);

void EncodeU16BE(uint16_t value, std::vector<uint8_t>* out);
void EncodeU32BE(uint32_t value, std::vector<uint8_t>* out);
void EncodeU64BE(uint64_t value, std::vector<uint8_t>* out);
void EncodeVarintU64(uint64_t value, std::vector<uint8_t>* out);
void EncodeBincodeVarintU64(uint64_t value, std::vector<uint8_t>* out);
void EncodeBincodeVecU8(const std::vector<uint8_t>& value, std::vector<uint8_t>* out);

}  // namespace grovedb

#endif  // GROVEDB_CPP_BINARY_H
