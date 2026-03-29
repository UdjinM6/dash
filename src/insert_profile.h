#ifndef GROVEDB_CPP_INSERT_PROFILE_H
#define GROVEDB_CPP_INSERT_PROFILE_H

#include <cstdint>

namespace grovedb::insert_profile {

enum class Stage : uint8_t {
  kEnsurePath = 0,
  kLoadTree,
  kLeafInsert,
  kLeafSave,
  kExportDirtyNodes,
  kParentLoopTotal,
  kParentLoadTree,
  kChildRootHash,
  kAggregatePropagation,
  kParentInsert,
  kParentSave,
  kFinalBatchCommit,
  kCount,
};

enum class Counter : uint8_t {
  kComputeValueHashCalls = 0,
  kUpdateNodeHashCalls,
  kEnsureChildLoadedCalls,
  kEnsureChildLoadedMisses,
  kRecoverMetaChildKeyLookups,
  kDirtyKeyCount,
  kExportedEntryCount,
  kTreeElementHashCalls,
  kChildLoadTreeHashCalls,
  kChildComputeRootHashCalls,
  kChildRootOverrideHits,
  kChildRootOverrideFallbackHits,
  kMerkCacheHits,
  kMerkCacheMisses,
  kCount,
};

bool Enabled();
void SyncLabel();
void AddStageNs(Stage stage, uint64_t ns);
void AddCounter(Counter counter, uint64_t delta = 1);

class ScopedStage {
 public:
  explicit ScopedStage(Stage stage);
  ~ScopedStage();

 private:
  Stage stage_;
  bool enabled_ = false;
  uint64_t start_ns_ = 0;
};

}  // namespace grovedb::insert_profile

#endif  // GROVEDB_CPP_INSERT_PROFILE_H
