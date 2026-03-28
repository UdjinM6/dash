#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;

namespace {

std::string ToHex(const std::array<uint8_t, 32>& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

void ExpectPrefix(const std::string& label,
                  const std::vector<std::vector<uint8_t>>& path,
                  const std::string& expected_hex) {
  std::array<uint8_t, 32> prefix{};
  std::string error;
  if (!grovedb::RocksDbWrapper::BuildPrefix(path, &prefix, &error)) {
    Fail(label + " build prefix failed: " + error);
  }
  std::string actual = ToHex(prefix);
  if (actual != expected_hex) {
    Fail(label + " prefix mismatch");
  }
}
}  // namespace

int main() {
  ExpectPrefix("empty", {}, "0000000000000000000000000000000000000000000000000000000000000000");
  ExpectPrefix("one", {{'a'}}, "35032eb3e6978d224fd984578c6fa8e3c3ab055e8a014edcf8ac39bfa0876498");
  ExpectPrefix("two",
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
               "29070815d8c4af27c1c64670b7688a32105cbd34334a8a5cb8b0b9c2b5358f6d");
  ExpectPrefix("three",
               {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}, {'l', 'e', 'a', 'f'}},
               "4db3b0eb51abf2ef69bdd8697e61d91906697a303c9ff8e62bc394e74099e71b");
  ExpectPrefix("binary", {{0x00, 0xff, 0x10}},
               "d6238f3073904b4312301dea11a179ab65ac0f1fe756bb9c60a8649ce1fc75fa");
  {
    std::array<uint8_t, 32> prefix{};
    std::string error;
    std::vector<std::vector<uint8_t>> path = {std::vector<uint8_t>(256, 0x41)};
    if (grovedb::RocksDbWrapper::BuildPrefix(path, &prefix, &error)) {
      Fail("BuildPrefix should reject path segments larger than 255 bytes");
    }
    if (error != "path segment exceeds 255 bytes") {
      Fail("unexpected oversized segment error: " + error);
    }
  }
  return 0;
}
