#include "binary.h"
#include "chunk.h"
#include "chunk_producer.h"
#include "test_utils.h"

#include <optional>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {

bool ReadVarint(const std::vector<uint8_t>& data, size_t* cursor, uint64_t* out) {
  std::string error;
  if (!grovedb::ReadVarintU64(data, cursor, out, &error)) {
    Fail("varint decode failed: " + error);
  }
  return true;
}

std::vector<uint8_t> ExtractChunkId(const std::vector<uint8_t>& chunk_bytes) {
  if (chunk_bytes.empty() || chunk_bytes[0] != 0x00) {
    Fail("missing chunk id marker");
  }
  size_t cursor = 1;
  uint64_t len = 0;
  ReadVarint(chunk_bytes, &cursor, &len);
  if (cursor + len > chunk_bytes.size()) {
    Fail("chunk id truncated");
  }
  return std::vector<uint8_t>(chunk_bytes.begin() + static_cast<long>(cursor),
                              chunk_bytes.begin() + static_cast<long>(cursor + len));
}

size_t CountChunks(const std::vector<uint8_t>& multi_chunk) {
  size_t cursor = 0;
  size_t count = 0;
  while (cursor < multi_chunk.size()) {
    uint8_t marker = multi_chunk[cursor++];
    uint64_t len = 0;
    ReadVarint(multi_chunk, &cursor, &len);
    if (marker == 0) {
      cursor += static_cast<size_t>(len);
      count += 1;
      continue;
    }
    if (marker == 1) {
      // decode ops payload to advance cursor
      for (uint64_t i = 0; i < len; ++i) {
        if (cursor >= multi_chunk.size()) {
          Fail("proof truncated while skipping ops");
        }
        uint8_t opcode = multi_chunk[cursor++];
        auto read_bytes = [&](size_t amount) {
          if (cursor + amount > multi_chunk.size()) {
            Fail("proof truncated while skipping ops");
          }
          cursor += amount;
        };
        auto read_u16 = [&]() -> uint16_t {
          if (cursor + 2 > multi_chunk.size()) {
            Fail("proof truncated while skipping u16");
          }
          uint16_t value =
              static_cast<uint16_t>(multi_chunk[cursor] << 8 | multi_chunk[cursor + 1]);
          cursor += 2;
          return value;
        };
        auto read_u32 = [&]() -> uint32_t {
          if (cursor + 4 > multi_chunk.size()) {
            Fail("proof truncated while skipping u32");
          }
          uint32_t value = (static_cast<uint32_t>(multi_chunk[cursor]) << 24) |
                           (static_cast<uint32_t>(multi_chunk[cursor + 1]) << 16) |
                           (static_cast<uint32_t>(multi_chunk[cursor + 2]) << 8) |
                           static_cast<uint32_t>(multi_chunk[cursor + 3]);
          cursor += 4;
          return value;
        };
        auto read_u64 = [&]() { read_bytes(8); };
        auto skip_varint = [&]() {
          while (cursor < multi_chunk.size()) {
            uint8_t byte = multi_chunk[cursor++];
            if ((byte & 0x80) == 0) {
              return;
            }
          }
          Fail("proof truncated while skipping varint");
        };
        auto read_kv = [&](bool large_value) {
          if (cursor >= multi_chunk.size()) {
            Fail("proof truncated while skipping kv");
          }
          uint8_t key_len = multi_chunk[cursor++];
          read_bytes(key_len);
          size_t value_len = 0;
          if (large_value) {
            value_len = read_u32();
          } else {
            value_len = read_u16();
          }
          read_bytes(value_len);
        };
        switch (opcode) {
          case 0x10:
          case 0x11:
          case 0x12:
          case 0x13:
            break;
          case 0x01:
          case 0x02:
          case 0x08:
          case 0x09:
            read_bytes(32);
            break;
          case 0x03:
          case 0x20:
            read_kv(opcode == 0x20);
            break;
          case 0x04:
          case 0x21:
            read_kv(opcode == 0x21);
            read_bytes(32);
            break;
          case 0x06:
          case 0x22:
            read_kv(opcode == 0x22);
            read_bytes(32);
            break;
          case 0x07:
          case 0x23:
            read_kv(opcode == 0x23);
            read_bytes(32);
            if (cursor >= multi_chunk.size()) {
              Fail("proof truncated while skipping feature type");
            }
            // feature type tag
            {
              uint8_t tag = multi_chunk[cursor++];
              if (tag == 1) {
                skip_varint();
              } else if (tag == 2) {
                read_bytes(16);
              } else if (tag == 3 || tag == 5) {
                skip_varint();
              } else if (tag == 4 || tag == 6) {
                skip_varint();
                skip_varint();
              } else if (tag != 0) {
                Fail("unknown feature type tag");
              }
            }
            break;
          case 0x05:
          case 0x0c:
          case 0x1a: {
            if (cursor >= multi_chunk.size()) {
              Fail("proof truncated while skipping digest");
            }
            uint8_t key_len = multi_chunk[cursor++];
            read_bytes(key_len);
            read_bytes(32);
            if (opcode == 0x1a) {
              read_u64();
            }
            break;
          }
          case 0x14:
          case 0x24:
            read_kv(opcode == 0x24);
            read_u64();
            break;
          case 0x15:
          case 0x17:
            read_bytes(32);
            read_u64();
            break;
          case 0x18:
          case 0x25:
            read_kv(opcode == 0x25);
            read_bytes(32);
            read_u64();
            break;
          case 0x0a:
          case 0x28:
            read_kv(opcode == 0x28);
            break;
          case 0x0b:
          case 0x29:
          case 0x0d:
          case 0x2a:
          case 0x0e:
          case 0x2b:
          case 0x16:
          case 0x2c:
          case 0x19:
          case 0x2d: {
            bool large_value = (opcode == 0x28 || opcode == 0x29 || opcode == 0x2a ||
                                opcode == 0x2b || opcode == 0x2c || opcode == 0x2d);
            read_kv(large_value);
            if (opcode == 0x0b || opcode == 0x29 || opcode == 0x0d || opcode == 0x2a ||
                opcode == 0x0e || opcode == 0x2b || opcode == 0x19 || opcode == 0x2d) {
              read_bytes(32);
            }
            if (opcode == 0x0e || opcode == 0x2b) {
              uint8_t tag = multi_chunk[cursor++];
              if (tag == 1) {
                skip_varint();
              } else if (tag == 2) {
                read_bytes(16);
              } else if (tag == 3 || tag == 5) {
                skip_varint();
              } else if (tag == 4 || tag == 6) {
                skip_varint();
                skip_varint();
              } else if (tag != 0) {
                Fail("unknown feature type tag");
              }
            }
            if (opcode == 0x16 || opcode == 0x2c || opcode == 0x19 || opcode == 0x2d) {
              read_u64();
            }
            break;
          }
          case 0x1b: {
            if (cursor >= multi_chunk.size()) {
              Fail("proof truncated while skipping digest count");
            }
            uint8_t key_len = multi_chunk[cursor++];
            read_bytes(key_len);
            read_bytes(32);
            read_u64();
            break;
          }
          default:
            Fail("unexpected opcode while skipping");
        }
      }
      continue;
    }
    Fail("unexpected chunk marker");
  }
  return count;
}
}  // namespace

int main() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'o'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  size_t height = static_cast<size_t>(tree.Height());
  if (height == 0) {
    Fail("expected non-empty tree");
  }

  grovedb::ChunkProducer producer;
  if (!grovedb::ChunkProducer::Create(tree, false, &producer, &error)) {
    Fail("create chunk producer failed: " + error);
  }

  size_t total = producer.Len();
  if (total == 0) {
    Fail("expected non-zero chunk count");
  }

  for (size_t i = 1; i <= total; ++i) {
    grovedb::SubtreeChunk chunk;
    if (!producer.ChunkWithIndex(i, &chunk, &error)) {
      Fail("chunk with index failed: " + error);
    }
    std::vector<bool> instruction;
    if (!grovedb::GenerateTraversalInstruction(height, i, &instruction, &error)) {
      Fail("generate traversal failed: " + error);
    }
    std::vector<uint8_t> expected_id = grovedb::TraversalInstructionToBytes(instruction);
    std::vector<uint8_t> actual_id = ExtractChunkId(chunk.chunk);
    if (expected_id != actual_id) {
      Fail("chunk id mismatch for index " + std::to_string(i));
    }
    if (i < total && (!chunk.next_index || *chunk.next_index != i + 1)) {
      Fail("next_index mismatch for chunk " + std::to_string(i));
    }
    if (i == total && chunk.next_index) {
      Fail("last chunk should not have next_index");
    }

    grovedb::SubtreeChunk by_id;
    if (!producer.Chunk(expected_id, &by_id, &error)) {
      Fail("chunk by id failed: " + error);
    }
    if (by_id.chunk != chunk.chunk) {
      Fail("chunk bytes mismatch for chunk id roundtrip");
    }
  }

  {
    grovedb::MultiChunk multi;
    if (producer.MultiChunkWithLimitAndIndex(1, 1, &multi, &error)) {
      Fail("expected multichunk limit failure");
    }
    if (error.find("limit too small") == std::string::npos) {
      Fail("limit error mismatch");
    }
  }

  {
    grovedb::MultiChunk multi;
    if (!producer.MultiChunkWithLimitAndIndex(1, std::nullopt, &multi, &error)) {
      Fail("multichunk failed: " + error);
    }
    size_t chunk_count = CountChunks(multi.chunk);
    if (chunk_count != total) {
      Fail("multichunk count mismatch");
    }
    if (multi.next_index) {
      Fail("multichunk should not have next index when complete");
    }
  }

  return 0;
}
