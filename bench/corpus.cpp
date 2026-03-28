#include "corpus.h"
#include "element.h"
#include "hex.h"
#include "proof.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

bool DecodeHexField(const char* label,
                    const std::string& hex,
                    std::vector<uint8_t>* out) {
  std::string error;
  if (!grovedb::DecodeHex(hex, out, &error)) {
    std::cerr << "failed to decode " << label << ": " << error << "\n";
    return false;
  }
  return true;
}

int GetIterations(int argc, char** argv) {
  if (argc > 2) {
    int value = std::atoi(argv[2]);
    if (value > 0) {
      return value;
    }
  }
  return 1000;
}

int GetSamples(int argc, char** argv) {
  if (argc > 3) {
    int value = std::atoi(argv[3]);
    if (value > 0) {
      return value;
    }
  }
  return 7;
}

int GetMinSampleMs(int argc, char** argv) {
  if (argc > 4) {
    int value = std::atoi(argv[4]);
    if (value > 0) {
      return value;
    }
  }
  return 100;
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  if ((n % 2) == 1) {
    return values[n / 2];
  }
  return (values[(n / 2) - 1] + values[n / 2]) / 2.0;
}

void BenchmarkCase(const grovedb::CorpusCase& entry,
                   int min_iterations,
                   int samples,
                   int min_sample_ms) {
  setenv("GROVEDB_PROOF_PROFILE_LABEL", entry.name.c_str(), 1);
  std::vector<uint8_t> root_hash;
  std::vector<uint8_t> proof;
  std::vector<std::vector<uint8_t>> path_bytes;

  if (!DecodeHexField("root_hash", entry.root_hash_hex, &root_hash)) {
    std::exit(1);
  }
  if (!DecodeHexField("proof", entry.proof_hex, &proof)) {
    std::exit(1);
  }
  for (const auto& hex : entry.path_hex) {
    std::vector<uint8_t> bytes;
    if (!DecodeHexField("path", hex, &bytes)) {
      std::exit(1);
    }
    path_bytes.push_back(bytes);
  }

  grovedb::PathQuery query;
  std::string query_error;
  if (!grovedb::BuildPathQueryFromDescriptor(entry, path_bytes, &query, &query_error)) {
    std::cerr << "failed to build query for " << entry.name << ": " << query_error << "\n";
    std::exit(1);
  }

  constexpr int kChunkSize = 64;
  const int64_t min_sample_ns = static_cast<int64_t>(min_sample_ms) * 1000000LL;
  std::vector<double> sample_ns_per_op;
  sample_ns_per_op.reserve(static_cast<size_t>(samples));

  for (int sample = 0; sample < samples; ++sample) {
    int64_t iterations = 0;
    int64_t elapsed_ns = 0;
    const auto start = std::chrono::high_resolution_clock::now();

    while (iterations < min_iterations || elapsed_ns < min_sample_ns) {
      for (int i = 0; i < kChunkSize; ++i) {
        std::vector<uint8_t> computed_root_hash;
        std::vector<grovedb::VerifiedPathKeyElement> elements;
        std::string verify_error;
        if (!grovedb::VerifyPathQueryProof(
                proof, query, &computed_root_hash, &elements, &verify_error)) {
          std::cerr << "benchmark failed for " << entry.name << ": " << verify_error << "\n";
          std::exit(1);
        }
        if (computed_root_hash != root_hash) {
          std::cerr << "benchmark failed for " << entry.name
                    << ": root hash mismatch\n";
          std::exit(1);
        }
        iterations += 1;
      }

      const auto now = std::chrono::high_resolution_clock::now();
      elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    }

    sample_ns_per_op.push_back(static_cast<double>(elapsed_ns) /
                               static_cast<double>(iterations));
  }

  const double median_ns_per_op = Median(sample_ns_per_op);
  std::cout << entry.name << ": " << median_ns_per_op
            << " ns/op (median of " << samples
            << " samples, min " << min_iterations
            << " iters/sample, " << min_sample_ms << " ms/sample)\n";
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = "cpp/corpus/corpus.json";
  if (argc > 1) {
    path = argv[1];
  }
  int iterations = GetIterations(argc, argv);
  int samples = GetSamples(argc, argv);
  int min_sample_ms = GetMinSampleMs(argc, argv);

  grovedb::Corpus corpus;
  std::string error;
  if (!grovedb::LoadCorpus(path, &corpus, &error)) {
    std::cerr << "failed to load corpus: " << error << "\n";
    return 1;
  }

  std::unordered_set<std::string> case_filter;
  if (const char* filter_env = std::getenv("GROVEDB_BENCH_CASE_FILTER")) {
    std::string filter = filter_env;
    size_t start = 0;
    while (start <= filter.size()) {
      size_t comma = filter.find(',', start);
      std::string token = (comma == std::string::npos)
                              ? filter.substr(start)
                              : filter.substr(start, comma - start);
      if (!token.empty()) {
        case_filter.insert(token);
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
  }

  for (const auto& entry : corpus.cases) {
    if (!case_filter.empty() && case_filter.find(entry.name) == case_filter.end()) {
      continue;
    }
    BenchmarkCase(entry, iterations, samples, min_sample_ms);
  }
  if (const char* profile_env = std::getenv("GROVEDB_PROOF_PROFILE")) {
    if (profile_env[0] == '1' && profile_env[1] == '\0') {
      grovedb::DumpVerifyPathQueryProfile();
    }
  }
  return 0;
}
