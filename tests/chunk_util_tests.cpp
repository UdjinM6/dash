#include "chunk.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::Expect;

int main() {
  std::string error;

  // chunk heights
  for (size_t i = 1; i <= 9; ++i) {
    size_t height = 0;
    if (!grovedb::ChunkHeight(6, i, &height, &error)) {
      Fail("chunk height failed: " + error);
    }
    Expect(height == 3, "chunk height mismatch for height=6");
  }

  {
    size_t height = 0;
    if (!grovedb::ChunkHeight(5, 1, &height, &error)) {
      Fail("chunk height failed: " + error);
    }
    Expect(height == 3, "chunk height mismatch for height=5 chunk 1");
    for (size_t i = 2; i <= 9; ++i) {
      if (!grovedb::ChunkHeight(5, i, &height, &error)) {
        Fail("chunk height failed: " + error);
      }
      Expect(height == 2, "chunk height mismatch for height=5 chunk >1");
    }
  }

  {
    size_t height = 0;
    if (!grovedb::ChunkHeight(10, 1, &height, &error)) {
      Fail("chunk height failed: " + error);
    }
    Expect(height == 3, "chunk height mismatch for height=10 chunk 1");
    if (!grovedb::ChunkHeight(10, 2, &height, &error)) {
      Fail("chunk height failed: " + error);
    }
    Expect(height == 3, "chunk height mismatch for height=10 chunk 2");
    for (size_t i = 3; i <= 5; ++i) {
      if (!grovedb::ChunkHeight(10, i, &height, &error)) {
        Fail("chunk height failed: " + error);
      }
      Expect(height == 2, "chunk height mismatch for height=10 chunk 3-5");
    }
  }

  // traversal instruction conversion
  {
    std::vector<uint8_t> bytes = grovedb::TraversalInstructionToBytes({});
    Expect(bytes.empty(), "empty traversal instruction bytes mismatch");
    bytes = grovedb::TraversalInstructionToBytes({grovedb::kChunkLeft});
    Expect(bytes == std::vector<uint8_t>({1}), "left traversal bytes mismatch");
    bytes = grovedb::TraversalInstructionToBytes({grovedb::kChunkRight});
    Expect(bytes == std::vector<uint8_t>({0}), "right traversal bytes mismatch");
    bytes = grovedb::TraversalInstructionToBytes(
        {grovedb::kChunkRight, grovedb::kChunkLeft, grovedb::kChunkLeft, grovedb::kChunkRight});
    Expect(bytes == std::vector<uint8_t>({0, 1, 1, 0}),
           "multi traversal bytes mismatch");

    std::vector<bool> instruction;
    if (!grovedb::BytesToTraversalInstruction({1}, &instruction, &error)) {
      Fail("bytes to traversal failed: " + error);
    }
    Expect(instruction == std::vector<bool>({grovedb::kChunkLeft}),
           "bytes to traversal left mismatch");
    if (!grovedb::BytesToTraversalInstruction({0}, &instruction, &error)) {
      Fail("bytes to traversal failed: " + error);
    }
    Expect(instruction == std::vector<bool>({grovedb::kChunkRight}),
           "bytes to traversal right mismatch");
  }

  // traversal instruction -> chunk id
  for (size_t i = 1; i <= 4; ++i) {
    std::vector<bool> instruction;
    if (!grovedb::GenerateTraversalInstruction(4, i, &instruction, &error)) {
      Fail("generate traversal failed: " + error);
    }
    size_t out = 0;
    if (!grovedb::ChunkIndexFromTraversalInstruction(instruction, 4, &out, &error)) {
      Fail("chunk index from traversal failed: " + error);
    }
    Expect(out == i, "chunk index mismatch for height=4");
  }

  for (size_t i = 1; i <= 9; ++i) {
    std::vector<bool> instruction;
    if (!grovedb::GenerateTraversalInstruction(6, i, &instruction, &error)) {
      Fail("generate traversal failed: " + error);
    }
    size_t out = 0;
    if (!grovedb::ChunkIndexFromTraversalInstruction(instruction, 6, &out, &error)) {
      Fail("chunk index from traversal failed: " + error);
    }
    Expect(out == i, "chunk index mismatch for height=6");
  }

  // traversal recovery
  {
    size_t out = 0;
    std::vector<bool> instr = {grovedb::kChunkLeft};
    Expect(!grovedb::ChunkIndexFromTraversalInstruction(instr, 5, &out, &error),
           "expected invalid traversal without recovery");
    if (!grovedb::ChunkIndexFromTraversalInstructionWithRecovery(instr, 5, &out, &error)) {
      Fail("recovery failed: " + error);
    }
    Expect(out == 1, "recovery mismatch for [left]");
  }

  return 0;
}
