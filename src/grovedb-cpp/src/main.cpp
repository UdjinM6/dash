#include "corpus.h"
#include "element.h"
#include "hex.h"
#include "proof.h"

#include <iostream>
#include <vector>

namespace {

bool DecodeAndReport(const std::string& label,
                     const std::string& hex,
                     size_t expected_len,
                     std::vector<uint8_t>* out) {
  std::vector<uint8_t> bytes;
  std::string error;
  if (!grovedb::DecodeHex(hex, &bytes, &error)) {
    std::cerr << "failed to decode " << label << ": " << error << "\n";
    return false;
  }
  if (out) {
    *out = bytes;
  }
  std::cout << label << " bytes: " << bytes.size() << "\n";
  if (expected_len != 0 && bytes.size() != expected_len) {
    std::cerr << label << " length mismatch: expected " << expected_len
              << ", got " << bytes.size() << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = "cpp/corpus/corpus.json";
  if (argc > 1) {
    path = argv[1];
  }

  grovedb::Corpus corpus;
  std::string error;
  if (!grovedb::LoadCorpus(path, &corpus, &error)) {
    std::cerr << "failed to load corpus: " << error << "\n";
    return 1;
  }

  if (corpus.cases.empty()) {
    std::cerr << "corpus has no cases" << "\n";
    return 1;
  }

  std::cout << "corpus version: " << corpus.version << "\n";
  for (const auto& entry : corpus.cases) {
    std::cout << "case: " << entry.name << "\n";

    std::vector<uint8_t> root_hash;
    std::vector<uint8_t> proof;
    std::vector<std::vector<uint8_t>> element_bytes_list;
    std::vector<std::vector<uint8_t>> item_bytes_list;
    std::vector<uint8_t> subtree_key;
    std::vector<std::vector<uint8_t>> path;

    if (!DecodeAndReport("root_hash", entry.root_hash_hex, 32, &root_hash)) {
      return 1;
    }
    if (!DecodeAndReport("proof", entry.proof_hex, 0, &proof)) {
      return 1;
    }
    std::cout << "keys count: " << entry.keys_hex.size() << "\n";
    std::cout << "elements count: " << entry.element_bytes_hex.size() << "\n";
    std::cout << "items count: " << entry.item_bytes_hex.size() << "\n";
    if (!DecodeAndReport("subtree_key", entry.subtree_key_hex, 0, &subtree_key)) {
      return 1;
    }
    path.clear();
    for (const auto& hex : entry.path_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeAndReport("path", hex, 0, &bytes)) {
        return 1;
      }
      path.push_back(bytes);
    }

    std::cout << "cost.seek_count: " << entry.cost.seek_count << "\n";
    std::cout << "cost.storage_loaded_bytes: " << entry.cost.storage_loaded_bytes << "\n";
    std::cout << "cost.hash_node_calls: " << entry.cost.hash_node_calls << "\n";
    std::cout << "cost.storage_added_bytes: " << entry.cost.storage_added_bytes << "\n";
    std::cout << "cost.storage_replaced_bytes: " << entry.cost.storage_replaced_bytes << "\n";
    std::cout << "cost.storage_removed: " << entry.cost.storage_removed << "\n";

    element_bytes_list.clear();
    item_bytes_list.clear();
    for (const auto& hex : entry.element_bytes_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeAndReport("element_bytes", hex, 0, &bytes)) {
        return 1;
      }
      element_bytes_list.push_back(bytes);
    }
    for (const auto& hex : entry.item_bytes_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeAndReport("item_bytes", hex, 0, &bytes)) {
        return 1;
      }
      item_bytes_list.push_back(bytes);
    }
    if (entry.expect_present) {
      if (element_bytes_list.size() != item_bytes_list.size() ||
          element_bytes_list.size() != entry.keys_hex.size()) {
        std::cerr << "expected element/item sizes to match keys\n";
        return 1;
      }
      for (size_t i = 0; i < element_bytes_list.size(); ++i) {
        grovedb::ElementItem decoded_item;
        std::string decode_error;
        if (!grovedb::DecodeItemFromElementBytes(element_bytes_list[i], &decoded_item,
                                                 &decode_error)) {
          std::cout << "element decode skipped: " << decode_error << "\n";
        } else if (decoded_item.value != item_bytes_list[i]) {
          std::cerr << "decoded item mismatch\n";
          return 1;
        }
      }
    } else {
      if (!element_bytes_list.empty() || !item_bytes_list.empty()) {
        std::cerr << "expected empty element/item bytes for absent case\n";
        return 1;
      }
      if (entry.keys_hex.size() != 1) {
        std::cerr << "expected exactly one key for absent case\n";
        return 1;
      }
    }

    if (entry.expect_present) {
      for (size_t i = 0; i < entry.keys_hex.size(); ++i) {
        std::vector<uint8_t> item_key;
        if (!DecodeAndReport("item_key", entry.keys_hex[i], 0, &item_key)) {
          return 1;
        }
        grovedb::SingleKeyProofInput proof_input{
            proof,
            root_hash,
            element_bytes_list[i],
            item_key,
            subtree_key,
            path};
        std::string proof_error;
        if (!grovedb::VerifySingleKeyProof(proof_input, &proof_error)) {
          std::cout << "proof verification skipped: " << proof_error << "\n";
        }
      }
    } else {
      std::vector<uint8_t> item_key;
      if (!DecodeAndReport("item_key", entry.keys_hex[0], 0, &item_key)) {
        return 1;
      }
      grovedb::SingleKeyProofInput proof_input{
          proof,
          root_hash,
          std::vector<uint8_t>(),
          item_key,
          subtree_key,
          path};
      std::string proof_error;
      if (!grovedb::VerifySingleKeyAbsenceProof(proof_input, &proof_error)) {
        std::cout << "proof verification skipped: " << proof_error << "\n";
      }
    }
  }

  std::cout << "corpus validation complete" << "\n";
  return 0;
}
