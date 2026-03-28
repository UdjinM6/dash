#include "proof.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {

bool IsGroveProofFixtureFile(const std::filesystem::path& path) {
  if (path.extension() != ".bin") {
    return false;
  }
  const std::string name = path.filename().string();
  return name.rfind("grove_proof_", 0) == 0 &&
         name.find("_root_hash.bin") == std::string::npos;
}

void AssertRoundTrip(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  const std::vector<uint8_t> original = ReadFile(path);
  grovedb::GroveLayerProof layer;
  std::string error;
  if (!grovedb::DecodeGroveDbProof(original, &layer, &error)) {
    Fail("DecodeGroveDbProof failed for " + name + ": " + error);
  }
  std::vector<uint8_t> encoded;
  if (!grovedb::EncodeGroveDbProof(layer, &encoded, &error)) {
    Fail("EncodeGroveDbProof failed for " + name + ": " + error);
  }
  if (encoded != original) {
    size_t diff_at = 0;
    const size_t common = std::min(encoded.size(), original.size());
    while (diff_at < common && encoded[diff_at] == original[diff_at]) {
      diff_at += 1;
    }
    std::string details = "roundtrip byte mismatch for " + name +
                          " orig_size=" + std::to_string(original.size()) +
                          " enc_size=" + std::to_string(encoded.size()) +
                          " diff_at=" + std::to_string(diff_at);
    if (diff_at < common) {
      details += " orig_byte=" + std::to_string(static_cast<unsigned>(original[diff_at])) +
                 " enc_byte=" + std::to_string(static_cast<unsigned>(encoded[diff_at]));
    }
    Fail(details);
  }
  std::cout << "GROVE_FIXTURE " << name << " PASS bytes=" << original.size() << "\n";
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  std::string dir = MakeTempDir("grove_proof_byte_roundtrip");
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_grovedb_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust grovedb proof writer");
  }

  size_t checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir))) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path path = entry.path();
    if (!IsGroveProofFixtureFile(path)) {
      continue;
    }
    AssertRoundTrip(path);
    checked += 1;
  }
  if (checked == 0) {
    Fail("no grove proof fixtures found");
  }
  std::cout << "GROVE_FIXTURE_SUMMARY checked=" << checked << " status=PASS\n";
  return 0;
}
