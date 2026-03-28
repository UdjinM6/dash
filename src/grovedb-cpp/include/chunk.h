#ifndef GROVEDB_CPP_CHUNK_H
#define GROVEDB_CPP_CHUNK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace grovedb {

constexpr bool kChunkLeft = true;
constexpr bool kChunkRight = false;

bool GenerateTraversalInstruction(size_t height,
                                  size_t chunk_index,
                                  std::vector<bool>* out,
                                  std::string* error);
bool ChunkHeight(size_t height, size_t chunk_index, size_t* out, std::string* error);
size_t NumberOfChunks(size_t height);
bool NumberOfChunksUnderChunkId(size_t height,
                                size_t chunk_id,
                                size_t* out,
                                std::string* error);
bool ChunkIndexFromTraversalInstruction(const std::vector<bool>& instruction,
                                        size_t height,
                                        size_t* out,
                                        std::string* error);
bool ChunkIndexFromTraversalInstructionWithRecovery(const std::vector<bool>& instruction,
                                                    size_t height,
                                                    size_t* out,
                                                    std::string* error);
std::vector<uint8_t> TraversalInstructionToBytes(const std::vector<bool>& instruction);
bool BytesToTraversalInstruction(const std::vector<uint8_t>& bytes,
                                 std::vector<bool>* out,
                                 std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_CHUNK_H
