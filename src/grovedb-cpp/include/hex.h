#ifndef GROVEDB_CPP_HEX_H
#define GROVEDB_CPP_HEX_H

#include <cstdint>
#include <string>
#include <vector>

namespace grovedb {

bool DecodeHex(const std::string& hex, std::vector<uint8_t>* out, std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_HEX_H
