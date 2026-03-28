#include "binary.h"
#include "test_utils.h"

#include <limits>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {

void AssertReadHelpersRejectOverflowedCursor() {
  const std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
  std::string error;

  {
    size_t cursor = std::numeric_limits<size_t>::max();
    std::vector<uint8_t> out;
    if (grovedb::ReadBytes(data, &cursor, 1, &out, &error)) {
      Fail("ReadBytes should reject overflowed cursor");
    }
    if (error != "proof truncated") {
      Fail("unexpected ReadBytes overflow error: " + error);
    }
  }

  {
    size_t cursor = std::numeric_limits<size_t>::max();
    uint16_t out = 0;
    error.clear();
    if (grovedb::ReadU16BE(data, &cursor, &out, &error)) {
      Fail("ReadU16BE should reject overflowed cursor");
    }
    if (error != "proof truncated") {
      Fail("unexpected ReadU16BE overflow error: " + error);
    }
  }

  {
    size_t cursor = std::numeric_limits<size_t>::max();
    uint32_t out = 0;
    error.clear();
    if (grovedb::ReadU32BE(data, &cursor, &out, &error)) {
      Fail("ReadU32BE should reject overflowed cursor");
    }
    if (error != "proof truncated") {
      Fail("unexpected ReadU32BE overflow error: " + error);
    }
  }

  {
    size_t cursor = std::numeric_limits<size_t>::max();
    uint64_t out = 0;
    error.clear();
    if (grovedb::ReadU64BE(data, &cursor, &out, &error)) {
      Fail("ReadU64BE should reject overflowed cursor");
    }
    if (error != "proof truncated") {
      Fail("unexpected ReadU64BE overflow error: " + error);
    }
  }
}

void AssertDecodeBincodeVecRejectsLenExceedingInput() {
  std::string error;
  std::vector<uint8_t> decoded;

  // Exercises the `len > data.size() - *cursor` branch.
  // EncodeBincodeVarintU64(100) produces a single byte (0x64) because
  // 100 <= 250, but the buffer has no payload bytes after it.
  // Varint read succeeds (len=100, cursor=1) then 100 > 0 triggers the guard.
  std::vector<uint8_t> encoded;
  grovedb::EncodeBincodeVarintU64(100, &encoded);  // {0x64}
  size_t cursor = 0;
  error.clear();
  if (grovedb::DecodeBincodeVecU8(encoded, &cursor, &decoded, &error)) {
    Fail("DecodeBincodeVecU8 should reject length exceeding input");
  }
  if (error != "vector length exceeds input size") {
    Fail("unexpected DecodeBincodeVecU8 len error: " + error);
  }
}

void AssertDecodeBincodeVecRejectsCursorPastEnd() {
  std::string error;
  std::vector<uint8_t> decoded;

  // Exercises the `*cursor > data.size()` guard directly, after varint decode.
  std::vector<uint8_t> data;
  size_t cursor = 1;
  error.clear();
  if (grovedb::DecodeBincodeVecU8Body(data, &cursor, 0, &decoded, &error)) {
    Fail("DecodeBincodeVecU8Body should reject cursor past end");
  }
  if (error != "vector length exceeds input size") {
    Fail("unexpected DecodeBincodeVecU8Body cursor error: " + error);
  }
}

}  // namespace

int main() {
  AssertReadHelpersRejectOverflowedCursor();
  AssertDecodeBincodeVecRejectsLenExceedingInput();
  AssertDecodeBincodeVecRejectsCursorPastEnd();
  return 0;
}
