#include "element.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;

namespace {

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
}  // namespace

int main() {
  {
    std::string encode_error;
    std::vector<uint8_t> root_key = {'r', 'o', 'o', 't'};
    std::vector<uint8_t> bytes;

    if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(&root_key, -3, &bytes,
                                                         &encode_error)) {
      Fail("encode sum tree failed: " + encode_error);
    }
    const std::vector<uint8_t> sum_expected = {
        0x04, 0x01, 0x04, 'r', 'o', 'o', 't', 0x05, 0x00,
    };
    if (bytes != sum_expected) {
      Fail("sum tree bytes mismatch");
    }

    if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(&root_key, 9, &bytes,
                                                            &encode_error)) {
      Fail("encode big sum tree failed: " + encode_error);
    }
    const std::vector<uint8_t> big_sum_expected = {
        0x05, 0x01, 0x04, 'r', 'o', 'o', 't', 0x12, 0x00,
    };
    if (bytes != big_sum_expected) {
      Fail("big sum tree bytes mismatch");
    }

    if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(&root_key, 7, &bytes,
                                                           &encode_error)) {
      Fail("encode count tree failed: " + encode_error);
    }
    const std::vector<uint8_t> count_expected = {
        0x06, 0x01, 0x04, 'r', 'o', 'o', 't', 0x07, 0x00,
    };
    if (bytes != count_expected) {
      Fail("count tree bytes mismatch");
    }

    if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(&root_key, 2, -1, &bytes,
                                                              &encode_error)) {
      Fail("encode count sum tree failed: " + encode_error);
    }
    const std::vector<uint8_t> count_sum_expected = {
        0x07, 0x01, 0x04, 'r', 'o', 'o', 't', 0x02, 0x01, 0x00,
    };
    if (bytes != count_sum_expected) {
      Fail("count sum tree bytes mismatch");
    }

    if (!grovedb::EncodeProvableCountSumTreeToElementBytesWithRootKey(&root_key,
                                                                      3,
                                                                      4,
                                                                      &bytes,
                                                                      &encode_error)) {
      Fail("encode provable count sum tree failed: " + encode_error);
    }
    const std::vector<uint8_t> provable_count_sum_expected = {
        0x0a, 0x01, 0x04, 'r', 'o', 'o', 't', 0x03, 0x08, 0x00,
    };
    if (bytes != provable_count_sum_expected) {
      Fail("provable count sum tree bytes mismatch");
    }
  }

  {
    std::string encode_error;
    std::vector<uint8_t> item_bytes;
    std::vector<uint8_t> sum_bytes;
    if (!grovedb::EncodeItemWithSumItemToElementBytes(std::vector<uint8_t>{'p', 'a', 'y'},
                                                      -9,
                                                      &item_bytes,
                                                      &encode_error)) {
      Fail("encode item-with-sum failed: " + encode_error);
    }
    grovedb::ElementItemWithSum decoded_item_with_sum;
    std::string decode_error;
    if (!grovedb::DecodeItemWithSumItemFromElementBytes(item_bytes, &decoded_item_with_sum,
                                                        &decode_error)) {
      Fail("decode item-with-sum failed: " + decode_error);
    }
    if (decoded_item_with_sum.value != std::vector<uint8_t>{'p', 'a', 'y'} ||
        decoded_item_with_sum.sum != -9) {
      Fail("item-with-sum decode mismatch");
    }

    if (!grovedb::EncodeSumItemToElementBytes(15, &sum_bytes, &encode_error)) {
      Fail("encode sum item failed: " + encode_error);
    }
    grovedb::ElementSumItem decoded_sum_item;
    if (!grovedb::DecodeSumItemFromElementBytes(sum_bytes, &decoded_sum_item, &decode_error)) {
      Fail("decode sum item failed: " + decode_error);
    }
    if (decoded_sum_item.sum != 15) {
      Fail("sum item decode mismatch");
    }

    int64_t extracted_sum = 0;
    bool has_sum = false;
    if (!grovedb::ExtractSumValueFromElementBytes(sum_bytes, &extracted_sum, &has_sum,
                                                  &decode_error)) {
      Fail("extract sum failed: " + decode_error);
    }
    if (!has_sum || extracted_sum != 15) {
      Fail("extract sum mismatch for sum item");
    }
    if (!grovedb::ExtractSumValueFromElementBytes(item_bytes, &extracted_sum, &has_sum,
                                                  &decode_error)) {
      Fail("extract sum failed for item-with-sum: " + decode_error);
    }
    if (!has_sum || extracted_sum != -9) {
      Fail("extract sum mismatch for item-with-sum");
    }
    std::vector<uint8_t> plain_item;
    if (!grovedb::EncodeItemToElementBytes(std::vector<uint8_t>{'x'}, &plain_item,
                                           &encode_error)) {
      Fail("encode item failed: " + encode_error);
    }
    if (!grovedb::ExtractSumValueFromElementBytes(plain_item, &extracted_sum, &has_sum,
                                                  &decode_error)) {
      Fail("extract sum failed for item: " + decode_error);
    }
    if (has_sum || extracted_sum != 0) {
      Fail("extract sum should be empty for item");
    }
    if (grovedb::ExtractSumValueFromElementBytes(std::vector<uint8_t>(), &extracted_sum, &has_sum,
                                                 &decode_error)) {
      Fail("extract sum should fail for empty input");
    }

    grovedb::ElementSumItem decode_sum;
    if (grovedb::DecodeSumItemFromElementBytes(item_bytes, &decode_sum, &decode_error)) {
      Fail("sum decode should fail for item-with-sum bytes");
    }
    grovedb::ElementItem decode_item;
    if (grovedb::DecodeItemFromElementBytes(sum_bytes, &decode_item, &decode_error)) {
      Fail("item decode should fail for sum item bytes");
    }

    std::vector<uint8_t> bad_item = item_bytes;
    bad_item.push_back(0x00);
    if (grovedb::DecodeItemWithSumItemFromElementBytes(bad_item, &decoded_item_with_sum,
                                                       &decode_error)) {
      Fail("item-with-sum decode should fail for trailing bytes");
    }
    std::vector<uint8_t> truncated_sum = sum_bytes;
    if (!truncated_sum.empty()) {
      truncated_sum.pop_back();
    }
    if (grovedb::DecodeSumItemFromElementBytes(truncated_sum, &decode_sum, &decode_error)) {
      Fail("sum decode should fail for truncated bytes");
    }

    std::vector<uint8_t> tree_bytes;
    if (!grovedb::EncodeTreeToElementBytes(&tree_bytes, &encode_error)) {
      Fail("encode tree failed: " + encode_error);
    }
    if (grovedb::DecodeItemFromElementBytes(tree_bytes, &decode_item, &decode_error)) {
      Fail("item decode should fail for tree bytes");
    }
    if (grovedb::DecodeSumItemFromElementBytes(tree_bytes, &decode_sum, &decode_error)) {
      Fail("sum decode should fail for tree bytes");
    }
    if (grovedb::DecodeItemWithSumItemFromElementBytes(tree_bytes,
                                                       &decoded_item_with_sum,
                                                       &decode_error)) {
      Fail("item-with-sum decode should fail for tree bytes");
    }
    int64_t tree_sum = 0;
    bool tree_has_sum = true;
    if (!grovedb::ExtractSumValueFromElementBytes(tree_bytes, &tree_sum, &tree_has_sum,
                                                  &decode_error)) {
      Fail("extract sum failed for tree bytes: " + decode_error);
    }
    if (tree_has_sum || tree_sum != 0) {
      Fail("tree bytes should not include sum");
    }

    std::vector<uint8_t> count_sum_tree;
    if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(
            nullptr, 11, -6, &count_sum_tree, &encode_error)) {
      Fail("encode count sum tree failed: " + encode_error);
    }
    uint64_t extracted_count = 0;
    int64_t extracted_count_sum = 0;
    bool has_count_sum = false;
    if (!grovedb::ExtractCountSumValueFromElementBytes(
            count_sum_tree, &extracted_count, &extracted_count_sum, &has_count_sum, &decode_error)) {
      Fail("extract count/sum failed for count sum tree: " + decode_error);
    }
    if (!has_count_sum || extracted_count != 11 || extracted_count_sum != -6) {
      Fail("extract count/sum mismatch for count sum tree");
    }

    std::vector<uint8_t> bad_variant = {0xff};
    if (grovedb::DecodeItemFromElementBytes(bad_variant, &decode_item, &decode_error)) {
      Fail("item decode should fail for unknown variant");
    }
  }

  return 0;
}
