#include "merk_cache.h"

#include "merk_storage.h"

namespace grovedb {

namespace {

void AppendUint32BE(uint32_t value, std::string* out) {
  out->push_back(static_cast<char>((value >> 24) & 0xFF));
  out->push_back(static_cast<char>((value >> 16) & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

bool ReadUint32BE(const std::string& in, size_t* cursor, uint32_t* out) {
  if (cursor == nullptr || out == nullptr || *cursor + 4 > in.size()) {
    return false;
  }
  const unsigned char b0 = static_cast<unsigned char>(in[*cursor]);
  const unsigned char b1 = static_cast<unsigned char>(in[*cursor + 1]);
  const unsigned char b2 = static_cast<unsigned char>(in[*cursor + 2]);
  const unsigned char b3 = static_cast<unsigned char>(in[*cursor + 3]);
  *out = (static_cast<uint32_t>(b0) << 24) |
         (static_cast<uint32_t>(b1) << 16) |
         (static_cast<uint32_t>(b2) << 8) |
         static_cast<uint32_t>(b3);
  *cursor += 4;
  return true;
}

bool DecodePathKey(const std::string& encoded,
                   std::vector<std::vector<uint8_t>>* out_path) {
  if (out_path == nullptr) {
    return false;
  }
  out_path->clear();
  size_t cursor = 0;
  while (cursor < encoded.size()) {
    uint32_t len = 0;
    if (!ReadUint32BE(encoded, &cursor, &len)) {
      return false;
    }
    if (cursor + len > encoded.size()) {
      return false;
    }
    out_path->emplace_back(encoded.begin() + static_cast<std::ptrdiff_t>(cursor),
                           encoded.begin() + static_cast<std::ptrdiff_t>(cursor + len));
    cursor += len;
  }
  return true;
}

}  // namespace

MerkCache::MerkCache(RocksDbWrapper* storage) : storage_(storage) {}

std::string MerkCache::pathToString(const std::vector<std::vector<uint8_t>>& path) {
  std::string encoded;
  size_t reserve_size = 0;
  for (const auto& component : path) {
    reserve_size += 4 + component.size();
  }
  encoded.reserve(reserve_size);
  for (const auto& component : path) {
    AppendUint32BE(static_cast<uint32_t>(component.size()), &encoded);
    encoded.append(reinterpret_cast<const char*>(component.data()), component.size());
  }
  return encoded;
}

bool MerkCache::getOrLoad(const std::vector<std::vector<uint8_t>>& path,
                          RocksDbWrapper::Transaction* tx,
                          MerkTree** out,
                          std::string* error) {
  std::string path_str = pathToString(path);

  auto it = merks_.find(path_str);
  if (it != merks_.end()) {
    *out = it->second.get();
    return true;
  }

  auto merk = std::make_unique<MerkTree>();
  if (tx != nullptr) {
    if (!MerkStorage::LoadTree(storage_, tx, path, merk.get(), error)) {
      return false;
    }
  } else {
    if (!MerkStorage::LoadTree(storage_, path, merk.get(), error)) {
      return false;
    }
  }

  *out = merk.get();
  merks_[path_str] = std::move(merk);
  return true;
}

bool MerkCache::getOrLoad(const std::vector<std::vector<uint8_t>>& path,
                          MerkTree** out,
                          std::string* error) {
  return getOrLoad(path, nullptr, out, error);
}

void MerkCache::clear() {
  merks_.clear();
}

size_t MerkCache::size() const {
  return merks_.size();
}

bool MerkCache::contains(const std::vector<std::vector<uint8_t>>& path) const {
  return merks_.find(pathToString(path)) != merks_.end();
}

void MerkCache::erase(const std::vector<std::vector<uint8_t>>& path) {
  merks_.erase(pathToString(path));
}

std::vector<std::vector<std::vector<uint8_t>>> MerkCache::getCachedPaths() const {
  std::vector<std::vector<std::vector<uint8_t>>> result;
  result.reserve(merks_.size());
  for (const auto& entry : merks_) {
    std::vector<std::vector<uint8_t>> path;
    if (!DecodePathKey(entry.first, &path)) {
      continue;
    }
    result.push_back(std::move(path));
  }
  return result;
}

void MerkCache::saveState() {
  saved_paths_.clear();
  for (const auto& entry : merks_) {
    saved_paths_.push_back(entry.first);
  }
  merks_.clear();
}

void MerkCache::restoreState() {
  merks_.clear();
  for (const auto& path_str : saved_paths_) {
    std::vector<std::vector<uint8_t>> path;
    if (!DecodePathKey(path_str, &path)) {
      continue;
    }
    auto merk = std::make_unique<MerkTree>();
    if (!MerkStorage::LoadTree(storage_, path, merk.get(), nullptr)) {
      continue;
    }
    merks_[path_str] = std::move(merk);
  }
  saved_paths_.clear();
}

}  // namespace grovedb
