#ifndef GROVEDB_CPP_GROVE_VERSION_H
#define GROVEDB_CPP_GROVE_VERSION_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace grovedb {

enum class GroveFeature {
  kPathQueryV2,
  kSumTrees,
  kBigSumTrees,
  kCountTrees,
  kChunkProofs,
  kReferencePaths,
  kProvableCountTrees,
};

struct GroveVersion {
  uint16_t major = 4;
  uint16_t minor = 0;
  uint16_t patch = 0;

  static GroveVersion V4_0_0() { return {4, 0, 0}; }
  static GroveVersion MinimumSupported() { return V4_0_0(); }
  static GroveVersion Current() { return V4_0_0(); }

  bool IsSupported() const { return *this >= MinimumSupported(); }

  bool Supports(GroveFeature feature) const {
    (void)feature;
    return IsSupported();
  }

  std::string ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch);
  }

  static bool Parse(std::string_view input, GroveVersion* out, std::string* error) {
    if (out == nullptr) {
      if (error) {
        *error = "version output is null";
      }
      return false;
    }
    size_t first_dot = input.find('.');
    size_t second_dot = (first_dot == std::string_view::npos)
                            ? std::string_view::npos
                            : input.find('.', first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
        input.find('.', second_dot + 1) != std::string_view::npos) {
      if (error) {
        *error = "invalid version format";
      }
      return false;
    }
    auto parse_component = [&](std::string_view part, uint16_t* value) -> bool {
      if (part.empty() || value == nullptr) {
        return false;
      }
      uint32_t parsed = 0;
      for (char c : part) {
        if (c < '0' || c > '9') {
          return false;
        }
        parsed = parsed * 10u + static_cast<uint32_t>(c - '0');
        if (parsed > 65535u) {
          return false;
        }
      }
      *value = static_cast<uint16_t>(parsed);
      return true;
    };

    GroveVersion parsed;
    if (!parse_component(input.substr(0, first_dot), &parsed.major) ||
        !parse_component(input.substr(first_dot + 1, second_dot - first_dot - 1),
                         &parsed.minor) ||
        !parse_component(input.substr(second_dot + 1), &parsed.patch)) {
      if (error) {
        *error = "invalid version component";
      }
      return false;
    }
    *out = parsed;
    return true;
  }

  friend bool operator==(const GroveVersion& lhs, const GroveVersion& rhs) {
    return lhs.major == rhs.major && lhs.minor == rhs.minor && lhs.patch == rhs.patch;
  }
  friend bool operator!=(const GroveVersion& lhs, const GroveVersion& rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const GroveVersion& lhs, const GroveVersion& rhs) {
    if (lhs.major != rhs.major) {
      return lhs.major < rhs.major;
    }
    if (lhs.minor != rhs.minor) {
      return lhs.minor < rhs.minor;
    }
    return lhs.patch < rhs.patch;
  }
  friend bool operator>=(const GroveVersion& lhs, const GroveVersion& rhs) {
    return !(lhs < rhs);
  }
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_GROVE_VERSION_H
