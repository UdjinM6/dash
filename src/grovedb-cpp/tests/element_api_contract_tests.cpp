#include "element.h"
#include "test_utils.h"

#include <optional>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {

void ExpectError(const std::string& label,
                 bool ok,
                 const std::string& actual,
                 const std::string& expected) {
  if (ok) {
    Fail(label + " unexpectedly succeeded");
  }
  if (actual != expected) {
    Fail(label + " expected '" + expected + "', got '" + actual + "'");
  }
}

}  // namespace

int main() {
  std::string error;

  std::vector<uint8_t> item_bytes;
  if (!grovedb::EncodeItemToElementBytes({'v'}, &item_bytes, &error)) {
    Fail("failed to encode item bytes: " + error);
  }
  std::vector<uint8_t> sum_bytes;
  if (!grovedb::EncodeSumItemToElementBytes(7, &sum_bytes, &error)) {
    Fail("failed to encode sum bytes: " + error);
  }
  std::vector<uint8_t> item_sum_bytes;
  if (!grovedb::EncodeItemWithSumItemToElementBytes({'x'}, 9, &item_sum_bytes, &error)) {
    Fail("failed to encode item-with-sum bytes: " + error);
  }
  grovedb::ElementReference reference;
  reference.reference_path.kind = grovedb::ReferencePathKind::kSibling;
  reference.reference_path.key = {'k'};
  std::vector<uint8_t> ref_bytes;
  if (!grovedb::EncodeReferenceToElementBytes(reference, &ref_bytes, &error)) {
    Fail("failed to encode reference bytes: " + error);
  }
  std::vector<uint8_t> tree_bytes;
  if (!grovedb::EncodeTreeToElementBytes(&tree_bytes, &error)) {
    Fail("failed to encode tree bytes: " + error);
  }
  std::vector<uint8_t> tree_with_root_bytes;
  std::vector<uint8_t> root_key_val = {'r', 'o', 'o', 't'};
  if (!grovedb::EncodeTreeToElementBytesWithRootKey(&root_key_val, &tree_with_root_bytes, &error)) {
    Fail("failed to encode tree with root bytes: " + error);
  }
  std::vector<uint8_t> sum_tree_bytes;
  if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, 42, &sum_tree_bytes, &error)) {
    Fail("failed to encode sum tree bytes: " + error);
  }
  std::vector<uint8_t> sum_tree_with_root_bytes;
  if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(
          &root_key_val, -17, &sum_tree_with_root_bytes, &error)) {
    Fail("failed to encode sum tree with root bytes: " + error);
  }
  std::vector<uint8_t> big_sum_tree_bytes;
  if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(nullptr, -9, &big_sum_tree_bytes, &error)) {
    Fail("failed to encode big sum tree bytes: " + error);
  }
  std::vector<uint8_t> big_sum_tree_with_root_bytes;
  if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(
          &root_key_val, static_cast<__int128>(1000) * 1000 * 1000 * 1000,
          &big_sum_tree_with_root_bytes, &error)) {
    Fail("failed to encode big sum tree with root bytes: " + error);
  }
  std::vector<uint8_t> count_tree_bytes;
  if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 42, &count_tree_bytes, &error)) {
    Fail("failed to encode count tree bytes: " + error);
  }
  std::vector<uint8_t> count_sum_tree_bytes;
  if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(
          nullptr, 21, -8, &count_sum_tree_bytes, &error)) {
    Fail("failed to encode count sum tree bytes: " + error);
  }
  std::vector<uint8_t> count_sum_tree_with_root_bytes;
  if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(
          &root_key_val, 100, 500, &count_sum_tree_with_root_bytes, &error)) {
    Fail("failed to encode count sum tree with root bytes: " + error);
  }
  std::vector<uint8_t> prov_count_sum_tree_bytes;
  if (!grovedb::EncodeProvableCountSumTreeToElementBytesWithRootKey(
          nullptr, 100, -50, &prov_count_sum_tree_bytes, &error)) {
    Fail("failed to encode provable count sum tree bytes: " + error);
  }
  std::vector<uint8_t> prov_count_tree_bytes;
  if (!grovedb::EncodeProvableCountTreeToElementBytesWithRootKey(
          nullptr, 42, &prov_count_tree_bytes, &error)) {
    Fail("failed to encode provable count tree bytes: " + error);
  }
  std::vector<uint8_t> prov_count_tree_with_root_bytes;
  if (!grovedb::EncodeProvableCountTreeToElementBytesWithRootKey(
          &root_key_val, 100, &prov_count_tree_with_root_bytes, &error)) {
    Fail("failed to encode provable count tree with root bytes: " + error);
  }

  {
    grovedb::ElementItem item;
    grovedb::ElementItemWithSum item_sum;
    grovedb::ElementSumItem sum_item;
    grovedb::ElementReference decoded_ref;
    grovedb::ElementTree decoded_tree;
    grovedb::ElementSumTree decoded_sum_tree;
    grovedb::ElementCountTree decoded_count_tree;
    grovedb::ElementCountSumTree decoded_count_sum_tree;
    uint64_t variant = 0;
    bool has_root = false;
    uint32_t root_len = 0;
    int64_t sum = 0;
    bool has_sum = false;
    __int128 big_sum = 0;
    bool has_big_sum = false;
    uint64_t count = 0;
    bool has_count = false;
    bool has_count_sum = false;
    std::optional<grovedb::ValueDefinedCostType> cost;
    std::vector<uint8_t> flags;

    error.clear();
    ExpectError("DecodeItem null out",
                grovedb::DecodeItemFromElementBytes(item_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeItemWithSum null out",
                grovedb::DecodeItemWithSumItemFromElementBytes(item_sum_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeSumItem null out",
                grovedb::DecodeSumItemFromElementBytes(sum_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeReference null out",
                grovedb::DecodeReferenceFromElementBytes(ref_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeTree null out",
                grovedb::DecodeTreeFromElementBytes(tree_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeSumTree null out",
                grovedb::DecodeSumTreeFromElementBytes(sum_tree_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeCountTree null out",
                grovedb::DecodeCountTreeFromElementBytes(count_tree_bytes, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("DecodeElementVariant null out",
                grovedb::DecodeElementVariant(item_bytes, nullptr, &error),
                error,
                "variant output is null");
    error.clear();
    ExpectError("TreeElementHasRootKey null out",
                grovedb::TreeElementHasRootKey(tree_bytes, nullptr, &error),
                error,
                "root key output is null");
    error.clear();
    ExpectError("TreeElementRootKeyLen null out",
                grovedb::TreeElementRootKeyLen(tree_bytes, nullptr, &error),
                error,
                "root key length output is null");
    error.clear();
    ExpectError("ExtractSumValue null out",
                grovedb::ExtractSumValueFromElementBytes(sum_bytes, nullptr, &has_sum, &error),
                error,
                "sum output is null");
    error.clear();
    ExpectError("ExtractSumValue null has_sum",
                grovedb::ExtractSumValueFromElementBytes(sum_bytes, &sum, nullptr, &error),
                error,
                "sum output is null");
    error.clear();
    ExpectError("ExtractBigSumValue null out",
                grovedb::ExtractBigSumValueFromElementBytes(big_sum_tree_bytes,
                                                            nullptr,
                                                            &has_big_sum,
                                                            &error),
                error,
                "sum output is null");
    error.clear();
    ExpectError("ExtractBigSumValue null has_sum",
                grovedb::ExtractBigSumValueFromElementBytes(big_sum_tree_bytes,
                                                            &big_sum,
                                                            nullptr,
                                                            &error),
                error,
                "sum output is null");
    error.clear();
    ExpectError("ExtractCountValue null out",
                grovedb::ExtractCountValueFromElementBytes(
                    count_tree_bytes, nullptr, &has_count, &error),
                error,
                "count output is null");
    error.clear();
    ExpectError("ExtractCountValue null has_count",
                grovedb::ExtractCountValueFromElementBytes(
                    count_tree_bytes, &count, nullptr, &error),
                error,
                "count output is null");
    error.clear();
    ExpectError("ExtractCountSumValue null out_count",
                grovedb::ExtractCountSumValueFromElementBytes(
                    count_sum_tree_bytes, nullptr, &sum, &has_count_sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ExtractCountSumValue null out_sum",
                grovedb::ExtractCountSumValueFromElementBytes(
                    count_sum_tree_bytes, &count, nullptr, &has_count_sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ExtractCountSumValue null has_count_sum",
                grovedb::ExtractCountSumValueFromElementBytes(
                    count_sum_tree_bytes, &count, &sum, nullptr, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ExtractProvableCountSumValue null out_count",
                grovedb::ExtractProvableCountSumValueFromElementBytes(
                    prov_count_sum_tree_bytes, nullptr, &sum, &has_count_sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ExtractProvableCountSumValue null out_sum",
                grovedb::ExtractProvableCountSumValueFromElementBytes(
                    prov_count_sum_tree_bytes, &count, nullptr, &has_count_sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ExtractProvableCountSumValue null has_count_sum",
                grovedb::ExtractProvableCountSumValueFromElementBytes(
                    prov_count_sum_tree_bytes, &count, &sum, nullptr, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ValueDefinedCost null out",
                grovedb::ValueDefinedCostForSerializedElement(item_bytes, nullptr, &error),
                error,
                "value defined cost output is null");
    error.clear();
    ExpectError("ExtractFlags null out",
                grovedb::ExtractFlagsFromElementBytes(item_bytes, nullptr, &error),
                error,
                "flags output is null");

    error.clear();
    ExpectError("DecodeItem wrong variant",
                grovedb::DecodeItemFromElementBytes(sum_bytes, &item, &error),
                error,
                "element is not an Item variant");
    error.clear();
    ExpectError("DecodeItemWithSum wrong variant",
                grovedb::DecodeItemWithSumItemFromElementBytes(item_bytes, &item_sum, &error),
                error,
                "element is not an ItemWithSumItem variant");
    error.clear();
    ExpectError("DecodeSumItem wrong variant",
                grovedb::DecodeSumItemFromElementBytes(item_bytes, &sum_item, &error),
                error,
                "element is not a SumItem variant");
    error.clear();
    ExpectError("DecodeReference wrong variant",
                grovedb::DecodeReferenceFromElementBytes(item_bytes, &decoded_ref, &error),
                error,
                "element is not a Reference variant");
    error.clear();
    ExpectError("DecodeTree wrong variant",
                grovedb::DecodeTreeFromElementBytes(item_bytes, &decoded_tree, &error),
                error,
                "element is not a Tree variant");
    error.clear();
    ExpectError("DecodeSumTree wrong variant",
                grovedb::DecodeSumTreeFromElementBytes(item_bytes, &decoded_sum_tree, &error),
                error,
                "element is not a SumTree variant");
    error.clear();
    ExpectError("DecodeCountTree wrong variant",
                grovedb::DecodeCountTreeFromElementBytes(item_bytes, &decoded_count_tree, &error),
                error,
                "element is not a CountTree variant");
    error.clear();
    ExpectError("DecodeCountSumTree wrong variant",
                grovedb::DecodeCountSumTreeFromElementBytes(item_bytes, &decoded_count_sum_tree, &error),
                error,
                "element is not a CountSumTree variant");

    std::vector<uint8_t> unsupported_variant = {0x7f};
    error.clear();
    ExpectError("DecodeElementVariant unsupported",
                grovedb::DecodeElementVariant(unsupported_variant, &variant, &error),
                error,
                "unsupported element variant");

    if (!grovedb::ExtractBigSumValueFromElementBytes(big_sum_tree_bytes,
                                                     &big_sum,
                                                     &has_big_sum,
                                                     &error)) {
      Fail("ExtractBigSumValueFromElementBytes failed: " + error);
    }
    if (!has_big_sum) {
      Fail("ExtractBigSumValueFromElementBytes should set has_big_sum=true");
    }
    if (big_sum != static_cast<__int128>(-9)) {
      Fail("ExtractBigSumValueFromElementBytes decoded wrong big sum");
    }
    if (!grovedb::ExtractBigSumValueFromElementBytes(item_bytes, &big_sum, &has_big_sum, &error)) {
      Fail("ExtractBigSumValueFromElementBytes non-big-sum should succeed: " + error);
    }
    if (has_big_sum) {
      Fail("ExtractBigSumValueFromElementBytes non-big-sum should set has_big_sum=false");
    }
    if (!grovedb::DecodeCountTreeFromElementBytes(
            count_tree_bytes, &decoded_count_tree, &error)) {
      Fail("DecodeCountTreeFromElementBytes failed: " + error);
    }
    if (decoded_count_tree.root_key.has_value()) {
      Fail("DecodeCountTreeFromElementBytes decoded wrong root_key (expected nullopt)");
    }
    if (decoded_count_tree.count != 42) {
      Fail("DecodeCountTreeFromElementBytes decoded wrong count");
    }
    if (grovedb::DecodeCountTreeFromElementBytes(item_bytes, &decoded_count_tree, &error)) {
      Fail("DecodeCountTreeFromElementBytes wrong variant should fail");
    }
    if (!grovedb::ExtractCountValueFromElementBytes(
            count_tree_bytes, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes failed: " + error);
    }
    if (!has_count || count != 42) {
      Fail("ExtractCountValueFromElementBytes decoded wrong count");
    }
    if (!grovedb::ExtractCountValueFromElementBytes(item_bytes, &count, &has_count, &error)) {
      Fail("ExtractCountValueFromElementBytes non-count should succeed: " + error);
    }
    if (has_count) {
      Fail("ExtractCountValueFromElementBytes non-count should set has_count=false");
    }
    if (!grovedb::ExtractCountSumValueFromElementBytes(
            count_sum_tree_bytes, &count, &sum, &has_count_sum, &error)) {
      Fail("ExtractCountSumValueFromElementBytes count-sum failed: " + error);
    }
    if (!has_count_sum || count != 21 || sum != -8) {
      Fail("ExtractCountSumValueFromElementBytes count-sum decoded wrong values");
    }
    if (!grovedb::ExtractCountSumValueFromElementBytes(
            prov_count_sum_tree_bytes, &count, &sum, &has_count_sum, &error)) {
      Fail("ExtractCountSumValueFromElementBytes provable-count-sum failed: " + error);
    }
    if (!has_count_sum || count != 100 || sum != -50) {
      Fail("ExtractCountSumValueFromElementBytes provable-count-sum decoded wrong values");
    }
    if (!grovedb::ExtractCountSumValueFromElementBytes(
            item_bytes, &count, &sum, &has_count_sum, &error)) {
      Fail("ExtractCountSumValueFromElementBytes non-count-sum should succeed: " + error);
    }
    if (has_count_sum) {
      Fail("ExtractCountSumValueFromElementBytes non-count-sum should set has_count_sum=false");
    }
    if (!grovedb::ExtractProvableCountSumValueFromElementBytes(
            prov_count_sum_tree_bytes, &count, &sum, &has_count_sum, &error)) {
      Fail("ExtractProvableCountSumValueFromElementBytes failed: " + error);
    }
    if (!has_count_sum) {
      Fail("ExtractProvableCountSumValueFromElementBytes should set has_count_sum=true");
    }
    if (count != 100 || sum != -50) {
      Fail("ExtractProvableCountSumValueFromElementBytes decoded wrong values");
    }
    if (!grovedb::ExtractProvableCountSumValueFromElementBytes(
            item_bytes, &count, &sum, &has_count_sum, &error)) {
      Fail("ExtractProvableCountSumValueFromElementBytes non-prov-count-sum should succeed: " +
           error);
    }
    if (has_count_sum) {
      Fail("ExtractProvableCountSumValueFromElementBytes non-prov-count-sum should set "
           "has_count_sum=false");
    }

    // Positive DecodeTreeFromElementBytes tests
    if (!grovedb::DecodeTreeFromElementBytes(tree_bytes, &decoded_tree, &error)) {
      Fail("DecodeTreeFromElementBytes empty tree failed: " + error);
    }
    if (decoded_tree.root_key.has_value()) {
      Fail("DecodeTreeFromElementBytes empty tree should have no root key");
    }

    if (!grovedb::DecodeTreeFromElementBytes(tree_with_root_bytes, &decoded_tree, &error)) {
      Fail("DecodeTreeFromElementBytes tree with root failed: " + error);
    }
    if (!decoded_tree.root_key.has_value()) {
      Fail("DecodeTreeFromElementBytes tree with root should have root key");
    }
    if (decoded_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeTreeFromElementBytes tree with root decoded wrong root key");
    }

    // Positive DecodeSumTreeFromElementBytes tests
    if (!grovedb::DecodeSumTreeFromElementBytes(sum_tree_bytes, &decoded_sum_tree, &error)) {
      Fail("DecodeSumTreeFromElementBytes empty sum tree failed: " + error);
    }
    if (decoded_sum_tree.root_key.has_value()) {
      Fail("DecodeSumTreeFromElementBytes empty sum tree should have no root key");
    }
    if (decoded_sum_tree.sum != 42) {
      Fail("DecodeSumTreeFromElementBytes empty sum tree decoded wrong sum");
    }

    if (!grovedb::DecodeSumTreeFromElementBytes(
            sum_tree_with_root_bytes, &decoded_sum_tree, &error)) {
      Fail("DecodeSumTreeFromElementBytes sum tree with root failed: " + error);
    }
    if (!decoded_sum_tree.root_key.has_value()) {
      Fail("DecodeSumTreeFromElementBytes sum tree with root should have root key");
    }
    if (decoded_sum_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeSumTreeFromElementBytes sum tree with root decoded wrong root key");
    }
    if (decoded_sum_tree.sum != -17) {
      Fail("DecodeSumTreeFromElementBytes sum tree with root decoded wrong sum");
    }

    // DecodeSumTreeFromElementBytes should fail on non-SumTree variant
    if (grovedb::DecodeSumTreeFromElementBytes(sum_bytes, &decoded_sum_tree, &error)) {
      Fail("DecodeSumTreeFromElementBytes should fail on SumItem variant");
    }

    // DecodeTreeFromElementBytes should fail on non-Tree variant
    if (grovedb::DecodeTreeFromElementBytes(sum_bytes, &decoded_tree, &error)) {
      Fail("DecodeTreeFromElementBytes should fail on SumItem variant");
    }

    // Positive DecodeCountSumTreeFromElementBytes tests
    if (!grovedb::DecodeCountSumTreeFromElementBytes(count_sum_tree_bytes, &decoded_count_sum_tree, &error)) {
      Fail("DecodeCountSumTreeFromElementBytes empty count sum tree failed: " + error);
    }
    if (decoded_count_sum_tree.root_key.has_value()) {
      Fail("DecodeCountSumTreeFromElementBytes empty count sum tree should have no root key");
    }
    if (decoded_count_sum_tree.count != 21) {
      Fail("DecodeCountSumTreeFromElementBytes empty count sum tree decoded wrong count");
    }
    if (decoded_count_sum_tree.sum != -8) {
      Fail("DecodeCountSumTreeFromElementBytes empty count sum tree decoded wrong sum");
    }

    if (!grovedb::DecodeCountSumTreeFromElementBytes(
            count_sum_tree_with_root_bytes, &decoded_count_sum_tree, &error)) {
      Fail("DecodeCountSumTreeFromElementBytes count sum tree with root failed: " + error);
    }
    if (!decoded_count_sum_tree.root_key.has_value()) {
      Fail("DecodeCountSumTreeFromElementBytes count sum tree with root should have root key");
    }
    if (decoded_count_sum_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeCountSumTreeFromElementBytes count sum tree with root decoded wrong root key");
    }
    if (decoded_count_sum_tree.count != 100) {
      Fail("DecodeCountSumTreeFromElementBytes count sum tree with root decoded wrong count");
    }
    if (decoded_count_sum_tree.sum != 500) {
      Fail("DecodeCountSumTreeFromElementBytes count sum tree with root decoded wrong sum");
    }

    // DecodeCountSumTreeFromElementBytes should fail on non-CountSumTree variant
    if (grovedb::DecodeCountSumTreeFromElementBytes(sum_bytes, &decoded_count_sum_tree, &error)) {
      Fail("DecodeCountSumTreeFromElementBytes should fail on SumItem variant");
    }

    // Positive DecodeBigSumTreeFromElementBytes tests
    grovedb::ElementBigSumTree decoded_big_sum_tree;
    if (!grovedb::DecodeBigSumTreeFromElementBytes(big_sum_tree_bytes, &decoded_big_sum_tree, &error)) {
      Fail("DecodeBigSumTreeFromElementBytes empty big sum tree failed: " + error);
    }
    if (decoded_big_sum_tree.root_key.has_value()) {
      Fail("DecodeBigSumTreeFromElementBytes empty big sum tree should have no root key");
    }
    if (decoded_big_sum_tree.sum != -9) {
      Fail("DecodeBigSumTreeFromElementBytes empty big sum tree decoded wrong sum");
    }

    if (!grovedb::DecodeBigSumTreeFromElementBytes(
            big_sum_tree_with_root_bytes, &decoded_big_sum_tree, &error)) {
      Fail("DecodeBigSumTreeFromElementBytes big sum tree with root failed: " + error);
    }
    if (!decoded_big_sum_tree.root_key.has_value()) {
      Fail("DecodeBigSumTreeFromElementBytes big sum tree with root should have root key");
    }
    if (decoded_big_sum_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeBigSumTreeFromElementBytes big sum tree with root decoded wrong root key");
    }
    __int128 expected_big_sum = static_cast<__int128>(1000) * 1000 * 1000 * 1000;
    if (decoded_big_sum_tree.sum != expected_big_sum) {
      Fail("DecodeBigSumTreeFromElementBytes big sum tree with root decoded wrong sum");
    }

    // DecodeBigSumTreeFromElementBytes should fail on non-BigSumTree variant
    if (grovedb::DecodeBigSumTreeFromElementBytes(sum_bytes, &decoded_big_sum_tree, &error)) {
      Fail("DecodeBigSumTreeFromElementBytes should fail on SumItem variant");
    }

    // Positive DecodeProvableCountTreeFromElementBytes tests
    grovedb::ElementProvableCountTree decoded_prov_count_tree;
    if (!grovedb::DecodeProvableCountTreeFromElementBytes(prov_count_tree_bytes, &decoded_prov_count_tree, &error)) {
      Fail("DecodeProvableCountTreeFromElementBytes empty provable count tree failed: " + error);
    }
    if (decoded_prov_count_tree.root_key.has_value()) {
      Fail("DecodeProvableCountTreeFromElementBytes empty provable count tree should have no root key");
    }
    if (decoded_prov_count_tree.count != 42) {
      Fail("DecodeProvableCountTreeFromElementBytes empty provable count tree decoded wrong count");
    }

    if (!grovedb::DecodeProvableCountTreeFromElementBytes(
            prov_count_tree_with_root_bytes, &decoded_prov_count_tree, &error)) {
      Fail("DecodeProvableCountTreeFromElementBytes provable count tree with root failed: " + error);
    }
    if (!decoded_prov_count_tree.root_key.has_value()) {
      Fail("DecodeProvableCountTreeFromElementBytes provable count tree with root should have root key");
    }
    if (decoded_prov_count_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeProvableCountTreeFromElementBytes provable count tree with root decoded wrong root key");
    }
    if (decoded_prov_count_tree.count != 100) {
      Fail("DecodeProvableCountTreeFromElementBytes provable count tree with root decoded wrong count");
    }

    // DecodeProvableCountTreeFromElementBytes should fail on non-ProvableCountTree variant
    if (grovedb::DecodeProvableCountTreeFromElementBytes(sum_bytes, &decoded_prov_count_tree, &error)) {
      Fail("DecodeProvableCountTreeFromElementBytes should fail on SumItem variant");
    }

    // Positive DecodeProvableCountSumTreeFromElementBytes tests
    grovedb::ElementProvableCountSumTree decoded_prov_count_sum_tree;
    if (!grovedb::DecodeProvableCountSumTreeFromElementBytes(prov_count_sum_tree_bytes, &decoded_prov_count_sum_tree, &error)) {
      Fail("DecodeProvableCountSumTreeFromElementBytes empty provable count sum tree failed: " + error);
    }
    if (decoded_prov_count_sum_tree.root_key.has_value()) {
      Fail("DecodeProvableCountSumTreeFromElementBytes empty provable count sum tree should have no root key");
    }
    if (decoded_prov_count_sum_tree.count != 100) {
      Fail("DecodeProvableCountSumTreeFromElementBytes empty provable count sum tree decoded wrong count");
    }
    if (decoded_prov_count_sum_tree.sum != -50) {
      Fail("DecodeProvableCountSumTreeFromElementBytes empty provable count sum tree decoded wrong sum");
    }

    std::vector<uint8_t> prov_count_sum_tree_with_root_bytes;
    if (!grovedb::EncodeProvableCountSumTreeToElementBytesWithRootKey(
            &root_key_val, 200, 150, &prov_count_sum_tree_with_root_bytes, &error)) {
      Fail("failed to encode provable count sum tree with root bytes: " + error);
    }
    if (!grovedb::DecodeProvableCountSumTreeFromElementBytes(
            prov_count_sum_tree_with_root_bytes, &decoded_prov_count_sum_tree, &error)) {
      Fail("DecodeProvableCountSumTreeFromElementBytes provable count sum tree with root failed: " + error);
    }
    if (!decoded_prov_count_sum_tree.root_key.has_value()) {
      Fail("DecodeProvableCountSumTreeFromElementBytes provable count sum tree with root should have root key");
    }
    if (decoded_prov_count_sum_tree.root_key.value() != std::vector<uint8_t>{'r', 'o', 'o', 't'}) {
      Fail("DecodeProvableCountSumTreeFromElementBytes provable count sum tree with root decoded wrong root key");
    }
    if (decoded_prov_count_sum_tree.count != 200) {
      Fail("DecodeProvableCountSumTreeFromElementBytes provable count sum tree with root decoded wrong count");
    }
    if (decoded_prov_count_sum_tree.sum != 150) {
      Fail("DecodeProvableCountSumTreeFromElementBytes provable count sum tree with root decoded wrong sum");
    }

    // DecodeProvableCountSumTreeFromElementBytes should fail on non-ProvableCountSumTree variant
    if (grovedb::DecodeProvableCountSumTreeFromElementBytes(sum_bytes, &decoded_prov_count_sum_tree, &error)) {
      Fail("DecodeProvableCountSumTreeFromElementBytes should fail on SumItem variant");
    }
  }

  {
    std::vector<uint8_t> malformed_ref = {0x01, 0x7f};
    grovedb::ElementReference decoded_ref;
    error.clear();
    ExpectError("DecodeReference invalid path kind",
                grovedb::DecodeReferenceFromElementBytes(malformed_ref, &decoded_ref, &error),
                error,
                "invalid reference path kind");
  }

  {
    std::vector<uint8_t> malformed_flags = item_bytes;
    malformed_flags.back() = 0x02;
    grovedb::ElementItem item;
    std::vector<uint8_t> flags;
    error.clear();
    ExpectError("DecodeItem invalid flags tag",
                grovedb::DecodeItemFromElementBytes(malformed_flags, &item, &error),
                error,
                "invalid flags option tag");
    error.clear();
    ExpectError("ExtractFlags invalid flags tag",
                grovedb::ExtractFlagsFromElementBytes(malformed_flags, &flags, &error),
                error,
                "invalid flags option tag");
  }

  {
    std::vector<uint8_t> out;
    error.clear();
    ExpectError("EncodeItem null out",
                grovedb::EncodeItemToElementBytes({'a'}, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeItemWithFlags null out",
                grovedb::EncodeItemToElementBytesWithFlags({'a'}, {'f'}, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeSumItem null out",
                grovedb::EncodeSumItemToElementBytes(1, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeItemWithSum null out",
                grovedb::EncodeItemWithSumItemToElementBytes({'a'}, 1, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeReference null out",
                grovedb::EncodeReferenceToElementBytes(reference, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeTree null out",
                grovedb::EncodeTreeToElementBytes(nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("EncodeTreeWithRootKey null out",
                grovedb::EncodeTreeToElementBytesWithRootKey(nullptr, nullptr, &error),
                error,
                "output is null");

    if (!grovedb::EncodeTreeToElementBytes(&out, &error)) {
      Fail("EncodeTreeToElementBytes valid call failed: " + error);
    }
  }

  return 0;
}
