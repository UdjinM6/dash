#ifndef GROVEDB_CPP_VALUE_DEFINED_COST_H
#define GROVEDB_CPP_VALUE_DEFINED_COST_H

#include <cstdint>

namespace grovedb {

struct ValueDefinedCostType {
  enum class Kind {
    kLayered,
    kSpecialized,
  };

  Kind kind = Kind::kLayered;
  uint32_t cost = 0;
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_VALUE_DEFINED_COST_H
