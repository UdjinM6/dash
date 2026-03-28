#include "chunk_depth.h"
#include "test_utils.h"

#include <vector>

using test_utils::Fail;
using test_utils::Expect;

int main() {
  Expect(grovedb::CalculateMaxTreeDepthFromCount(0) == 0, "depth count 0");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(1) == 1, "depth count 1");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(2) == 2, "depth count 2");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(4) == 3, "depth count 4");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(7) == 4, "depth count 7");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(12) == 5, "depth count 12");
  Expect(grovedb::CalculateMaxTreeDepthFromCount(88) == 9, "depth count 88");

  {
    auto chunks = grovedb::CalculateChunkDepths(20, 8);
    Expect(chunks == std::vector<uint8_t>({7, 7, 6}), "chunk depths 20/8");
  }
  {
    auto chunks = grovedb::CalculateChunkDepths(10, 4);
    Expect(chunks == std::vector<uint8_t>({4, 3, 3}), "chunk depths 10/4");
  }
  {
    auto chunks = grovedb::CalculateChunkDepthsWithMinimum(10, 8, 6);
    Expect(chunks == std::vector<uint8_t>({6, 4}), "chunk depths min 10/8/6");
  }
  {
    auto chunks = grovedb::CalculateChunkDepthsWithMinimum(14, 8, 6);
    Expect(chunks == std::vector<uint8_t>({7, 7}), "chunk depths min 14/8/6");
  }

  return 0;
}
