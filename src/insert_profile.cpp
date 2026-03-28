#include "insert_profile.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace grovedb::insert_profile {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<const char*, static_cast<size_t>(Stage::kCount)> kStageNames = {
    "ensure_path",
    "load_tree",
    "leaf_insert",
    "leaf_save",
    "export_dirty_nodes",
    "parent_loop_total",
    "parent_load_tree",
    "child_root_hash",
    "aggregate_propagation",
    "parent_insert",
    "parent_save",
    "final_batch_commit",
};

constexpr std::array<const char*, static_cast<size_t>(Counter::kCount)> kCounterNames = {
    "compute_value_hash_calls",
    "update_node_hash_calls",
    "ensure_child_loaded_calls",
    "ensure_child_loaded_misses",
    "recover_meta_child_key_lookups",
    "dirty_key_count",
    "exported_entry_count",
    "tree_element_hash_calls",
    "child_load_tree_hash_calls",
    "child_compute_root_hash_calls",
    "child_root_override_hits",
    "child_root_override_fallback_hits",
    "merk_cache_hits",
    "merk_cache_misses",
};

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Totals {
  std::array<uint64_t, static_cast<size_t>(Stage::kCount)> stage_ns{};
  std::array<uint64_t, static_cast<size_t>(Counter::kCount)> counters{};
  uint64_t operations = 0;
};

struct ThreadState {
  std::string label;
  Totals totals;

  ~ThreadState() { Flush(); }

  void Reset() {
    totals = Totals();
  }

  void Flush() {
    if (label.empty() || totals.operations == 0) {
      return;
    }
    std::cerr << "[insert-profile] label=" << label
              << " ops=" << totals.operations;
    for (size_t i = 0; i < kStageNames.size(); ++i) {
      std::cerr << " " << kStageNames[i] << "_ns=" << totals.stage_ns[i];
    }
    for (size_t i = 0; i < kCounterNames.size(); ++i) {
      std::cerr << " " << kCounterNames[i] << "=" << totals.counters[i];
    }
    std::cerr << "\n";
    Reset();
  }
};

bool EnabledInternal() {
  static const bool enabled = []() {
    const char* env = std::getenv("GROVEDB_PROFILE_INSERT");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

std::string CurrentLabel() {
  const char* env = std::getenv("GROVEDB_PROFILE_INSERT_LABEL");
  if (env == nullptr || env[0] == '\0') {
    return "insert";
  }
  return env;
}

thread_local ThreadState g_state;

}  // namespace

bool Enabled() {
  return EnabledInternal();
}

void SyncLabel() {
  if (!EnabledInternal()) {
    return;
  }
  const std::string next_label = CurrentLabel();
  if (g_state.label.empty()) {
    g_state.label = next_label;
    return;
  }
  if (g_state.label != next_label) {
    g_state.Flush();
    g_state.label = next_label;
  }
}

void AddStageNs(Stage stage, uint64_t ns) {
  if (!EnabledInternal()) {
    return;
  }
  SyncLabel();
  g_state.totals.stage_ns[static_cast<size_t>(stage)] += ns;
}

void AddCounter(Counter counter, uint64_t delta) {
  if (!EnabledInternal()) {
    return;
  }
  SyncLabel();
  g_state.totals.counters[static_cast<size_t>(counter)] += delta;
}

ScopedStage::ScopedStage(Stage stage) : stage_(stage), enabled_(EnabledInternal()) {
  if (!enabled_) {
    return;
  }
  SyncLabel();
  if (stage_ == Stage::kLeafInsert) {
    g_state.totals.operations += 1;
  }
  start_ns_ = NowNs();
}

ScopedStage::~ScopedStage() {
  if (!enabled_) {
    return;
  }
  AddStageNs(stage_, NowNs() - start_ns_);
}

}  // namespace grovedb::insert_profile
