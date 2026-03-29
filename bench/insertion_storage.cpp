#include "rocksdb_wrapper.h"
#include "../tests/test_utils.h"

#include <algorithm>
#include <chrono>
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

double RunOnce(grovedb::RocksDbWrapper* db,
               std::mt19937_64* rng,
               const std::vector<std::vector<uint8_t>>& path,
               bool root_leaves,
               size_t items,
               bool use_tx) {
  if (db == nullptr || rng == nullptr) {
    return -1.0;
  }
  std::string error;
  auto start = std::chrono::high_resolution_clock::now();
  if (use_tx) {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db->BeginTransaction(&tx, &error)) {
      std::cerr << "begin tx failed: " << error << "\n";
      return -1.0;
    }
    for (size_t i = 0; i < items; ++i) {
      const auto key = RandomKey(rng);
      const std::vector<uint8_t> value = root_leaves ? std::vector<uint8_t>{0x02} : key;
      if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error)) {
        std::cerr << "tx put failed: " << error << "\n";
        return -1.0;
      }
    }
    if (!tx.Commit(&error)) {
      std::cerr << "tx commit failed: " << error << "\n";
      return -1.0;
    }
  } else {
    for (size_t i = 0; i < items; ++i) {
      const auto key = RandomKey(rng);
      const std::vector<uint8_t> value = root_leaves ? std::vector<uint8_t>{0x02} : key;
      if (!db->Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error)) {
        std::cerr << "put failed: " << error << "\n";
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

  std::vector<std::vector<uint8_t>> path;
  if (nested) {
    for (size_t i = 0; i < 10; ++i) {
      path.push_back(RandomKey(&rng));
    }
  } else if (!root_leaves) {
    path = {{'l', 'e', 'a', 'f', '1'}};
  }

  const size_t total_samples = options.samples + options.warmup_samples;
  if (options.reset_per_sample) {
    for (size_t s = 0; s < total_samples; ++s) {
      auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      const std::string dir = MakeTempDir("bench_storage_" + label + "_" + std::to_string(now));
      grovedb::RocksDbWrapper db;
      std::string error;
      if (!db.Open(dir, &error)) {
        std::cerr << "open rocksdb failed: " << error << "\n";
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
    const std::string dir = MakeTempDir("bench_storage_" + label + "_" + std::to_string(now));
    grovedb::RocksDbWrapper db;
    std::string error;
    if (!db.Open(dir, &error)) {
      std::cerr << "open rocksdb failed: " << error << "\n";
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
            << " ns/op (" << items << " items, samples=" << samples << ")\n";
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

  if (!RunInsertBenchmark("storage_scalars_no_tx", options, false, false, false, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage scalars insertion without transaction",
              ns_per_op,
              options.items,
              options.samples);

  if (!RunInsertBenchmark("storage_scalars_tx", options, true, false, false, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage scalars insertion with transaction",
              ns_per_op,
              options.items,
              options.samples);

  BenchOptions root_options = options;
  root_options.items = 10;
  if (!RunInsertBenchmark("storage_root_no_tx", root_options, false, true, false, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage root leaves insertion without transaction", ns_per_op, 10, root_options.samples);

  if (!RunInsertBenchmark("storage_root_tx", root_options, true, true, false, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage root leaves insertion with transaction", ns_per_op, 10, root_options.samples);

  if (!RunInsertBenchmark("storage_nested_no_tx", options, false, false, true, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage deeply nested scalars insertion without transaction",
              ns_per_op,
              options.items,
              options.samples);

  if (!RunInsertBenchmark("storage_nested_tx", options, true, false, true, &ns_per_op)) {
    return 1;
  }
  PrintResult("storage deeply nested scalars insertion with transaction",
              ns_per_op,
              options.items,
              options.samples);

  return 0;
}
