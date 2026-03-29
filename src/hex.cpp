#include "hex.h"

#include <cctype>

namespace grovedb {

namespace {
int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}
}

bool DecodeHex(const std::string& hex, std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output buffer is null";
    }
    return false;
  }
  out->clear();
  if (hex.size() % 2 != 0) {
    if (error) {
      *error = "hex string length must be even";
    }
    return false;
  }
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int high = HexValue(hex[i]);
    int low = HexValue(hex[i + 1]);
    if (high < 0 || low < 0) {
      if (error) {
        *error = "hex string contains non-hex characters";
      }
      return false;
    }
    out->push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return true;
}

}  // namespace grovedb
