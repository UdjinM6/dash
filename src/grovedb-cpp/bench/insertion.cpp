#include "element.h"
#include "grovedb.h"
#include "../tests/test_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using test_utils::MakeTempDir;

namespace {

std::vector<uint8_t> RandomKey(std::mt19937_64* rng) {
  std::vector<uint8_t> key(32);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>((*rng)() & 0xFF);
  }
  return key;
}

struct BenchOptions {
  size_t items = 10000;
  size_t samples = 10;
  size_t warmup_samples = 2;
  bool reset_per_sample = false;
};

double Median(std::vector<double>* values) {
  if (values == nullptr || values->empty()) {
    return 0.0;
  }
  auto mid = values->begin() + static_cast<long>(values->size() / 2);
  std::nth_element(values->begin(), mid, values->end());
  if (values->size() % 2 == 1) {
    return *mid;
  }
  auto mid2 = std::max_element(values->begin(), mid);
  return (*mid + *mid2) / 2.0;
}

bool SetupScenario(grovedb::GroveDb* db,
                   std::mt19937_64* rng,
                   bool root_leaves,
                   bool nested,
                   std::vector<std::vector<uint8_t>>* out_path,
                   std::string* error) {
  if (db == nullptr || rng == nullptr || out_path == nullptr) {
    if (error) {
      *error = "setup inputs are null";
    }
    return false;
  }
  out_path->clear();

  std::vector<uint8_t> tree_element;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, error)) {
    return false;
  }

  if (nested) {
    std::vector<std::vector<uint8_t>> current_path;
    for (size_t i = 0; i < 10; ++i) {
      std::vector<uint8_t> key = RandomKey(rng);
      if (!db->Insert(current_path, key, tree_element, error)) {
        return false;
      }
      current_path.push_back(key);
    }
    *out_path = current_path;
    return true;
  }

  if (!root_leaves) {
    const std::vector<uint8_t> leaf = {'l', 'e', 'a', 'f', '1'};
    if (!db->Insert({}, leaf, tree_element, error)) {
      return false;
    }
    out_path->push_back(leaf);
  }
  return true;
}

double RunOnce(grovedb::GroveDb* db,
               std::mt19937_64* rng,
               const std::vector<std::vector<uint8_t>>& path,
               bool root_leaves,
               size_t items,
               bool use_tx) {
  if (db == nullptr || rng == nullptr) {
    return -1.0;
  }
  std::vector<uint8_t> item_element;
  std::vector<uint8_t> tree_element;
  std::string error;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error)) {
    std::cerr << "encode tree failed: " << error << "\n";
    return -1.0;
  }
  auto start = std::chrono::high_resolution_clock::now();
  if (use_tx) {
    grovedb::GroveDb::Transaction tx;
    if (!db->StartTransaction(&tx, &error)) {
      std::cerr << "begin tx failed: " << error << "\n";
      return -1.0;
    }
    for (size_t i = 0; i < items; ++i) {
      const auto key = RandomKey(rng);
      if (root_leaves) {
        if (!db->Insert(path, key, tree_element, &tx, &error)) {
          std::cerr << "tx insert tree failed: " << error << "\n";
          return -1.0;
        }
        continue;
      }
      if (!grovedb::EncodeItemToElementBytes(key, &item_element, &error)) {
        std::cerr << "encode item failed: " << error << "\n";
        return -1.0;
      }
      if (!db->Insert(path, key, item_element, &tx, &error)) {
        std::cerr << "tx insert failed: " << error << "\n";
        return -1.0;
      }
    }
    if (!db->CommitTransaction(&tx, &error)) {
      std::cerr << "tx commit failed: " << error << "\n";
      return -1.0;
    }
  } else {
    for (size_t i = 0; i < items; ++i) {
      const auto key = RandomKey(rng);
      if (root_leaves) {
        if (!db->Insert(path, key, tree_element, &error)) {
          std::cerr << "insert tree failed: " << error << "\n";
          return -1.0;
        }
        continue;
      }
      if (!grovedb::EncodeItemToElementBytes(key, &item_element, &error)) {
        std::cerr << "encode item failed: " << error << "\n";
        return -1.0;
      }
      if (!db->Insert(path, key, item_element, &error)) {
        std::cerr << "insert failed: " << error << "\n";
        return -1.0;
      }
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  return static_cast<double>(total_ns) / static_cast<double>(items);
}

bool RunInsertBenchmark(const std::string& label,
                        const BenchOptions& options,
                        bool use_tx,
                        bool root_leaves,
                        bool nested,
                        double* out_median) {
  if (out_median == nullptr) {
    return false;
  }
  std::vector<double> samples;
  samples.reserve(options.samples);
  std::mt19937_64 rng(12345);
  setenv("GROVEDB_PROFILE_INSERT_LABEL", label.c_str(), 1);

  std::vector<std::vector<uint8_t>> path;
  const size_t total_samples = options.samples + options.warmup_samples;
  if (options.reset_per_sample) {
    for (size_t s = 0; s < total_samples; ++s) {
      auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      const std::string dir = MakeTempDir("bench_" + label + "_" + std::to_string(now));
      grovedb::GroveDb db;
      std::string error;
      if (!db.Open(dir, &error)) {
        std::cerr << "open rocksdb failed: " << error << "\n";
        return false;
      }
      if (!SetupScenario(&db, &rng, root_leaves, nested, &path, &error)) {
        std::cerr << "setup scenario failed: " << error << "\n";
        std::filesystem::remove_all(dir);
        return false;
      }
      const double ns_per_op = RunOnce(&db, &rng, path, root_leaves, options.items, use_tx);
      std::filesystem::remove_all(dir);
      if (ns_per_op < 0) {
        return false;
      }
      if (s >= options.warmup_samples) {
        samples.push_back(ns_per_op);
      }
    }
  } else {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::string dir = MakeTempDir("bench_" + label + "_" + std::to_string(now));
    grovedb::GroveDb db;
    std::string error;
    if (!db.Open(dir, &error)) {
      std::cerr << "open rocksdb failed: " << error << "\n";
      return false;
    }
    if (!SetupScenario(&db, &rng, root_leaves, nested, &path, &error)) {
      std::cerr << "setup scenario failed: " << error << "\n";
      std::filesystem::remove_all(dir);
      return false;
    }
    for (size_t s = 0; s < total_samples; ++s) {
      const double ns_per_op = RunOnce(&db, &rng, path, root_leaves, options.items, use_tx);
      if (ns_per_op < 0) {
        std::filesystem::remove_all(dir);
        return false;
      }
      if (s >= options.warmup_samples) {
        samples.push_back(ns_per_op);
      }
    }
    std::filesystem::remove_all(dir);
  }

  *out_median = Median(&samples);
  return true;
}

void PrintResult(const std::string& label, double ns_per_op, size_t items, size_t samples) {
  std::cout << label << ": " << std::fixed << std::setprecision(1) << ns_per_op
            << " ns/op (" << items << " items, samples=" << samples << ")\n"
            << std::flush;
}

bool BenchCaseEnabled(const std::string& key) {
  const char* only = std::getenv("GROVEDB_BENCH_INSERT_ONLY");
  if (only == nullptr || only[0] == '\0') {
    return true;
  }
  return key == only;
}

}  // namespace

int main(int argc, char** argv) {
  BenchOptions options;
  if (argc > 1) {
    options.items = static_cast<size_t>(std::stoull(argv[1]));
  }
  if (argc > 2) {
    options.samples = static_cast<size_t>(std::stoull(argv[2]));
  }
  if (argc > 3) {
    options.warmup_samples = static_cast<size_t>(std::stoull(argv[3]));
  }
  if (argc > 4) {
    options.reset_per_sample = static_cast<int>(std::stoul(argv[4])) != 0;
  }

  double ns_per_op = 0.0;

  if (BenchCaseEnabled("scalars_no_tx")) {
    if (!RunInsertBenchmark("scalars_no_tx", options, false, false, false, &ns_per_op)) {
      return 1;
    }
    PrintResult("scalars insertion without transaction", ns_per_op, options.items, options.samples);
  }

  if (BenchCaseEnabled("scalars_tx")) {
    if (!RunInsertBenchmark("scalars_tx", options, true, false, false, &ns_per_op)) {
      return 1;
    }
    PrintResult("scalars insertion with transaction", ns_per_op, options.items, options.samples);
  }

  BenchOptions root_options = options;
  root_options.items = 10;
  if (BenchCaseEnabled("root_no_tx")) {
    if (!RunInsertBenchmark("root_no_tx", root_options, false, true, false, &ns_per_op)) {
      return 1;
    }
    PrintResult("root leaves insertion without transaction", ns_per_op, 10, root_options.samples);
  }

  if (BenchCaseEnabled("root_tx")) {
    if (!RunInsertBenchmark("root_tx", root_options, true, true, false, &ns_per_op)) {
      return 1;
    }
    PrintResult("root leaves insertion with transaction", ns_per_op, 10, root_options.samples);
  }

  if (BenchCaseEnabled("nested_no_tx")) {
    if (!RunInsertBenchmark("nested_no_tx", options, false, false, true, &ns_per_op)) {
      return 1;
    }
    PrintResult("deeply nested scalars insertion without transaction",
                ns_per_op,
                options.items,
                options.samples);
  }

  if (BenchCaseEnabled("nested_tx")) {
    if (!RunInsertBenchmark("nested_tx", options, true, false, true, &ns_per_op)) {
      return 1;
    }
    PrintResult("deeply nested scalars insertion with transaction",
                ns_per_op,
                options.items,
                options.samples);
  }

  return 0;
}
