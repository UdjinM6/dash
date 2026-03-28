#include "corpus.h"
#include "element.h"
#include "hex.h"
#include "proof.h"

#include <iostream>
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

bool DecodeVarintU64(const std::vector<uint8_t>& bytes, uint64_t* out) {
  if (out == nullptr) {
    return false;
  }
  uint64_t result = 0;
  int shift = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    uint8_t byte = bytes[i];
    result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 64) {
      return false;
    }
  }
  return false;
}

int64_t ZigZagDecodeI64(uint64_t value) {
  return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

bool DecodeVarintI64(const std::vector<uint8_t>& bytes, int64_t* out) {
  uint64_t raw = 0;
  if (!DecodeVarintU64(bytes, &raw)) {
    return false;
  }
  if (out) {
    *out = ZigZagDecodeI64(raw);
  }
  return true;
}

bool IsSingleKeyLeafSemanticsCase(const std::string& name) {
  return name == "single_key_item" ||
         name == "batch_single_key_item" ||
         name == "reference_single_key" ||
         name == "reference_chain_max_hop" ||
         name == "batch_reference_chain_max_hop";
}

bool IsExplicitSimpleLeafFastPathContractCase(const std::string& name) {
  return name == "single_key_item" ||
         name == "range_query";
}

}  // namespace

int main(int argc, char** argv) {
  grovedb::Corpus corpus;
  std::string error;
  if (argc > 1) {
    if (!grovedb::LoadCorpus(argv[1], &corpus, &error)) {
      std::cerr << "failed to load corpus: " << error << "\n";
      return 1;
    }
  } else {
    const char* fallback_paths[] = {
        "corpus/corpus.json",
        "../corpus/corpus.json",
    };
    bool loaded = false;
    for (const char* candidate : fallback_paths) {
      error.clear();
      if (grovedb::LoadCorpus(candidate, &corpus, &error)) {
        loaded = true;
        break;
      }
    }
    if (!loaded) {
      std::cerr << "failed to load corpus: " << error << "\n";
      return 1;
    }
  }
  if (corpus.cases.empty()) {
    std::cerr << "corpus has no cases\n";
    return 1;
  }

  for (const auto& entry : corpus.cases) {
    std::vector<uint8_t> root_hash;
    std::vector<uint8_t> proof;
    std::vector<uint8_t> subtree_key;
    std::vector<std::vector<uint8_t>> path_bytes;
    std::vector<std::vector<uint8_t>> element_bytes_list;
    std::vector<std::vector<uint8_t>> item_bytes_list;
    std::vector<std::vector<uint8_t>> keys;

    if (!DecodeHexField("root_hash", entry.root_hash_hex, &root_hash)) {
      return 1;
    }
    if (!DecodeHexField("proof", entry.proof_hex, &proof)) {
      return 1;
    }
    if (!DecodeHexField("subtree_key", entry.subtree_key_hex, &subtree_key)) {
      return 1;
    }
    for (const auto& hex : entry.path_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeHexField("path", hex, &bytes)) {
        return 1;
      }
      path_bytes.push_back(bytes);
    }
    for (const auto& hex : entry.keys_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeHexField("key", hex, &bytes)) {
        return 1;
      }
      keys.push_back(bytes);
    }
    for (const auto& hex : entry.element_bytes_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeHexField("element_bytes", hex, &bytes)) {
        return 1;
      }
      element_bytes_list.push_back(bytes);
    }
    for (const auto& hex : entry.item_bytes_hex) {
      std::vector<uint8_t> bytes;
      if (!DecodeHexField("item_bytes", hex, &bytes)) {
        return 1;
      }
      item_bytes_list.push_back(bytes);
    }

    if (entry.expect_present) {
      if (keys.size() != element_bytes_list.size() ||
          keys.size() != item_bytes_list.size()) {
        std::cerr << "case " << entry.name << ": key/element/item size mismatch\n";
        return 1;
      }
      for (size_t i = 0; i < keys.size(); ++i) {
        grovedb::ElementItem decoded_item;
        std::string decode_error;
        bool decoded_item_ok =
            grovedb::DecodeItemFromElementBytes(element_bytes_list[i], &decoded_item,
                                                &decode_error);
        if (decoded_item_ok) {
          if (decoded_item.value != item_bytes_list[i]) {
            std::cerr << "case " << entry.name << ": decoded item mismatch\n";
            return 1;
          }
        } else {
          grovedb::ElementReference decoded_reference;
          std::string reference_error;
          if (grovedb::DecodeReferenceFromElementBytes(element_bytes_list[i],
                                                       &decoded_reference,
                                                       &reference_error)) {
            std::vector<uint8_t> encoded_reference;
            std::string encode_error;
            if (!grovedb::EncodeReferenceToElementBytes(decoded_reference,
                                                        &encoded_reference,
                                                        &encode_error)) {
              std::cerr << "case " << entry.name << ": encode reference failed: "
                        << encode_error << "\n";
              return 1;
            }
            if (encoded_reference != item_bytes_list[i]) {
              std::cerr << "case " << entry.name << ": decoded reference mismatch\n";
              return 1;
            }
            continue;
          }
          grovedb::ElementSumItem decoded_sum;
          std::string sum_error;
          if (!grovedb::DecodeSumItemFromElementBytes(element_bytes_list[i], &decoded_sum,
                                                      &sum_error)) {
            grovedb::ElementItemWithSum decoded_with_sum;
            std::string with_sum_error;
            if (!grovedb::DecodeItemWithSumItemFromElementBytes(element_bytes_list[i],
                                                                &decoded_with_sum,
                                                                &with_sum_error)) {
              std::cerr << "case " << entry.name << ": decode item failed: "
                        << decode_error << "; sum decode failed: " << sum_error
                        << "; item-with-sum decode failed: " << with_sum_error << "\n";
              return 1;
            }
            if (decoded_with_sum.value != item_bytes_list[i]) {
              std::cerr << "case " << entry.name << ": decoded item-with-sum mismatch\n";
              return 1;
            }
            continue;
          }
          int64_t expected_sum = 0;
          if (!DecodeVarintI64(item_bytes_list[i], &expected_sum)) {
            std::cerr << "case " << entry.name << ": decode sum bytes failed\n";
            return 1;
          }
          if (decoded_sum.sum != expected_sum) {
            std::cerr << "case " << entry.name << ": decoded sum mismatch\n";
            return 1;
          }
        }
      }
    } else if (!element_bytes_list.empty() || !item_bytes_list.empty()) {
      std::cerr << "case " << entry.name << ": expected no element/item bytes\n";
      return 1;
    }

    grovedb::PathQuery query;
    std::string query_error;
    if (!grovedb::BuildPathQueryFromDescriptor(entry, path_bytes, &query, &query_error)) {
      std::cerr << "case " << entry.name << ": query build failed: "
                << query_error << "\n";
      return 1;
    }

    std::vector<uint8_t> computed_root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> verified;
    std::string proof_error;
    if (!grovedb::VerifyPathQueryProof(
            proof, query, &computed_root_hash, &verified, &proof_error)) {
      std::cerr << "case " << entry.name << ": proof verification failed: "
                << proof_error << "\n";
      return 1;
    }
    if (computed_root_hash != root_hash) {
      std::cerr << "case " << entry.name << ": root hash mismatch\n";
      return 1;
    }

    std::vector<std::vector<uint8_t>> actual_keys;
    std::vector<std::vector<uint8_t>> actual_elements;
    for (const auto& element : verified) {
      if (element.path == path_bytes && element.has_element) {
        actual_keys.push_back(element.key);
        actual_elements.push_back(element.element_bytes);
      }
    }
    if (entry.expect_present) {
      if (actual_keys.size() != keys.size() ||
          actual_elements.size() != element_bytes_list.size()) {
        std::cerr << "case " << entry.name << ": verified result count mismatch\n";
        return 1;
      }
      for (size_t i = 0; i < keys.size(); ++i) {
        if (actual_keys[i] != keys[i] ||
            actual_elements[i] != element_bytes_list[i]) {
          std::cerr << "case " << entry.name << ": verified result mismatch\n";
          return 1;
        }
      }
    } else if (!actual_keys.empty() || !actual_elements.empty()) {
      std::cerr << "case " << entry.name << ": expected no verified elements\n";
      return 1;
    }

    // Regression guard for the single-key leaf fast path: ensure known
    // single-key corpus cases return exactly the expected element payload.
    if (IsSingleKeyLeafSemanticsCase(entry.name)) {
      if (!entry.expect_present) {
        std::cerr << "case " << entry.name
                  << ": single-key leaf semantics case unexpectedly absent\n";
        return 1;
      }
      if (actual_keys.size() != 1 || actual_elements.size() != 1 ||
          keys.size() != 1 || element_bytes_list.size() != 1) {
        std::cerr << "case " << entry.name
                  << ": single-key leaf semantics count mismatch\n";
        return 1;
      }
      if (actual_keys.front() != keys.front() ||
          actual_elements.front() != element_bytes_list.front()) {
        std::cerr << "case " << entry.name
                  << ": single-key leaf semantics payload mismatch\n";
        return 1;
      }
    }

    // Explicit contract checks for canonical simple-leaf fast-path shapes.
    if (IsExplicitSimpleLeafFastPathContractCase(entry.name)) {
      if (!entry.expect_present) {
        std::cerr << "case " << entry.name
                  << ": simple-leaf contract case unexpectedly absent\n";
        return 1;
      }
      if (actual_keys.size() != keys.size() ||
          actual_elements.size() != element_bytes_list.size()) {
        std::cerr << "case " << entry.name
                  << ": simple-leaf contract result count mismatch\n";
        return 1;
      }
      for (size_t i = 0; i < keys.size(); ++i) {
        if (actual_keys[i] != keys[i] ||
            actual_elements[i] != element_bytes_list[i]) {
          std::cerr << "case " << entry.name
                    << ": simple-leaf contract payload mismatch\n";
          return 1;
        }
      }
    }
  }

  return 0;
}
