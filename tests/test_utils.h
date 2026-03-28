#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace test_utils {

[[noreturn]] inline void Fail(const std::string& message) {
  std::cerr << message << "\n";
  std::exit(1);
}

inline void Expect(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

inline std::string MakeTempDir(const std::string& label) {
  auto path = std::filesystem::temp_directory_path() / "tmp_grovedb_cpp_db" / label;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path.string();
}

inline std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    Fail("failed to open file: " + path.string());
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

inline std::string RustToolsCargoRunPrefix() {
  if (const char* manifest = std::getenv("GROVEDB_CPP_RUST_TOOLS_MANIFEST")) {
    if (std::filesystem::exists(manifest)) {
      return "cargo run --manifest-path \"" + std::string(manifest) +
             "\" --features rocksdb_storage --bin ";
    }
    Fail("GROVEDB_CPP_RUST_TOOLS_MANIFEST does not exist: " + std::string(manifest));
  }

  const std::filesystem::path candidates[] = {
      "tools/rust-storage-tools/Cargo.toml",
      "../tools/rust-storage-tools/Cargo.toml",
  };
  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return "cargo run --manifest-path \"" + path.string() +
             "\" --features rocksdb_storage --bin ";
    }
  }

  Fail("failed to locate tools/rust-storage-tools/Cargo.toml "
       "(tried repo root and build/ working directories)");
}

}  // namespace test_utils
