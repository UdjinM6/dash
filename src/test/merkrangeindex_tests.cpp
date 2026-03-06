// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/merkrangeindex.h>
#include <index/blockfilterindex.h>
#include <proof.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

namespace {

std::vector<uint8_t> HeightToTestKey(const int height)
{
    std::vector<uint8_t> out(4);
    out[0] = static_cast<uint8_t>((height >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((height >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((height >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(height & 0xFF);
    return out;
}

int CompareKeys(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

void VerifyRangeProofMatchesData(const std::vector<uint8_t>& proof,
                                 const std::vector<uint8_t>& expected_root_hash,
                                 int start_height,
                                 int stop_height,
                                 const std::vector<std::vector<uint8_t>>& expected_keys,
                                 const std::vector<std::vector<uint8_t>>& expected_values)
{
    std::string verify_error;
    std::vector<uint8_t> computed_root_hash;
    BOOST_REQUIRE_MESSAGE(grovedb::ExecuteMerkProof(proof, &computed_root_hash, &verify_error), verify_error);
    BOOST_CHECK(computed_root_hash == expected_root_hash);

    std::vector<grovedb::ProofNode> nodes;
    verify_error.clear();
    BOOST_REQUIRE_MESSAGE(grovedb::CollectKvNodes(proof, &nodes, &verify_error), verify_error);

    const std::vector<uint8_t> start_key = HeightToTestKey(start_height);
    const std::vector<uint8_t> end_key = HeightToTestKey(stop_height);
    std::vector<std::vector<uint8_t>> actual_keys;
    std::vector<std::vector<uint8_t>> actual_values;
    for (const auto& node : nodes) {
        if (CompareKeys(node.key, start_key) < 0 || CompareKeys(node.key, end_key) > 0) {
            continue;
        }
        actual_keys.push_back(node.key);
        actual_values.push_back(node.value);
    }

    BOOST_CHECK(actual_keys == expected_keys);
    BOOST_CHECK(actual_values == expected_values);
}

void BuildExpectedRangeData(BlockFilterIndex& blockfilter_index,
                            const CChain& active_chain,
                            const int start_height,
                            const int stop_height,
                            std::vector<std::vector<uint8_t>>& out_keys,
                            std::vector<std::vector<uint8_t>>& out_values)
{
    const CBlockIndex* stop_index = active_chain[stop_height];
    BOOST_REQUIRE(stop_index != nullptr);

    std::vector<BlockFilter> filters;
    BOOST_REQUIRE(blockfilter_index.LookupFilterRange(start_height, stop_index, filters));
    BOOST_REQUIRE_EQUAL(filters.size(), static_cast<size_t>(stop_height - start_height + 1));

    out_keys.clear();
    out_values.clear();
    for (size_t i = 0; i < filters.size(); ++i) {
        out_keys.push_back(HeightToTestKey(start_height + static_cast<int>(i)));
        const auto& encoded = filters[i].GetEncodedFilter();
        out_values.emplace_back(encoded.begin(), encoded.end());
    }
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(merkrangeindex_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(merkrangeindex_accessor_lifecycle)
{
    DestroyAllBlockFilterIndexes();
    DestroyAllMerkRangeIndexes();

    BOOST_CHECK(InitMerkRangeIndex(BlockFilterType::BASIC_FILTER, 1 << 20, /*f_memory=*/true, /*f_wipe=*/true));
    BOOST_CHECK(GetMerkRangeIndex(BlockFilterType::BASIC_FILTER) != nullptr);

    BOOST_CHECK(DestroyMerkRangeIndex(BlockFilterType::BASIC_FILTER));
    BOOST_CHECK(GetMerkRangeIndex(BlockFilterType::BASIC_FILTER) == nullptr);
}

BOOST_AUTO_TEST_CASE(merkrangeindex_range_proof)
{
    DestroyAllBlockFilterIndexes();
    DestroyAllMerkRangeIndexes();

    BOOST_REQUIRE(InitBlockFilterIndex(BlockFilterType::BASIC_FILTER, 1 << 20, /*f_memory=*/true, /*f_wipe=*/true));
    BlockFilterIndex* blockfilter_index = GetBlockFilterIndex(BlockFilterType::BASIC_FILTER);
    BOOST_REQUIRE(blockfilter_index != nullptr);
    BOOST_REQUIRE(blockfilter_index->Start(m_node.chainman->ActiveChainstate()));
    IndexWaitSynced(*blockfilter_index);

    BOOST_REQUIRE(InitMerkRangeIndex(BlockFilterType::BASIC_FILTER, 1 << 20, /*f_memory=*/true, /*f_wipe=*/true));
    MerkRangeIndex* merk_index = GetMerkRangeIndex(BlockFilterType::BASIC_FILTER);
    BOOST_REQUIRE(merk_index != nullptr);
    BOOST_REQUIRE(merk_index->Start(m_node.chainman->ActiveChainstate()));
    IndexWaitSynced(*merk_index);

    std::vector<uint8_t> proof;
    std::vector<uint8_t> root_hash;
    uint256 snapshot_tip_hash;
    BOOST_REQUIRE(merk_index->ProduceRangeProof(/*start_height=*/0, /*end_height=*/50, proof, root_hash, &snapshot_tip_hash));
    BOOST_REQUIRE(!proof.empty());
    BOOST_REQUIRE_EQUAL(root_hash.size(), 32U);
    BOOST_CHECK(!snapshot_tip_hash.IsNull());
    BOOST_CHECK_EQUAL(snapshot_tip_hash, WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash();));

    std::vector<uint8_t> current_root_hash;
    BOOST_REQUIRE(merk_index->GetMerkRootHash(current_root_hash));
    BOOST_CHECK(root_hash == current_root_hash);

    std::vector<std::vector<uint8_t>> expected_keys;
    std::vector<std::vector<uint8_t>> expected_element_bytes;
    WITH_LOCK(cs_main, BuildExpectedRangeData(*blockfilter_index, m_node.chainman->ActiveChain(), 0, 50, expected_keys, expected_element_bytes));

    VerifyRangeProofMatchesData(proof, root_hash, 0, 50, expected_keys, expected_element_bytes);

    std::vector<uint8_t> single_proof;
    std::vector<uint8_t> single_root_hash;
    uint256 single_snapshot_tip_hash;
    BOOST_REQUIRE(merk_index->ProduceRangeProof(/*start_height=*/50, /*end_height=*/50, single_proof, single_root_hash, &single_snapshot_tip_hash));
    BOOST_REQUIRE(!single_proof.empty());
    BOOST_CHECK(single_root_hash == root_hash);
    BOOST_CHECK(single_snapshot_tip_hash == snapshot_tip_hash);

    expected_keys.clear();
    expected_element_bytes.clear();
    WITH_LOCK(cs_main, BuildExpectedRangeData(*blockfilter_index, m_node.chainman->ActiveChain(), 50, 50, expected_keys, expected_element_bytes));

    VerifyRangeProofMatchesData(single_proof, single_root_hash, 50, 50, expected_keys, expected_element_bytes);

    std::vector<uint8_t> invalid_proof;
    std::vector<uint8_t> invalid_root_hash;
    BOOST_CHECK(!merk_index->ProduceRangeProof(/*start_height=*/100, /*end_height=*/50, invalid_proof, invalid_root_hash));

    merk_index->Interrupt();
    merk_index->Stop();
    DestroyAllMerkRangeIndexes();

    blockfilter_index->Interrupt();
    blockfilter_index->Stop();
    DestroyAllBlockFilterIndexes();
}

BOOST_AUTO_TEST_CASE(merkrange_estimation_worst_case_dummy_filters)
{
    // Keep in sync with net_processing.cpp.
    static constexpr size_t ESTIMATED_PER_FILTER_OVERHEAD = 48;
    static constexpr size_t TEST_MAX_PROOF_BYTES = 256 * 1024;
    static constexpr size_t MERKRANGE_HEADER_BYTES = 1 + 32 + 32 + 32 + 32 + 4 + 4;

    auto compact_size_len = [](size_t n) -> size_t {
        if (n < 253) return 1;
        if (n <= 0xFFFF) return 3;
        if (n <= 0xFFFFFFFFULL) return 5;
        return 9;
    };

    auto run_case = [&](const size_t filter_size, const int count, const bool expect_fit) {
        grovedb::MerkTree tree;
        std::string err;

        std::vector<size_t> filter_sizes;
        filter_sizes.reserve(count);
        std::vector<uint8_t> value(filter_size, 0xAB);
        for (int h = 0; h < count; ++h) {
            value[0] = static_cast<uint8_t>(h & 0xFF);
            BOOST_REQUIRE_MESSAGE(tree.Insert(HeightToTestKey(h), value, &err), err);
            filter_sizes.push_back(value.size());
        }

        int initial_end_height = 0;
        size_t approx_payload = MERKRANGE_HEADER_BYTES;
        for (int h = 0; h < static_cast<int>(filter_sizes.size()); ++h) {
            const size_t estimated_item_bytes = filter_sizes[h] + ESTIMATED_PER_FILTER_OVERHEAD;
            if (h > 0 && approx_payload + estimated_item_bytes > TEST_MAX_PROOF_BYTES) {
                break;
            }
            approx_payload += estimated_item_bytes;
            initial_end_height = h;
        }
        BOOST_REQUIRE(initial_end_height >= 0);

        auto produce_for_end = [&](int end_height, std::vector<uint8_t>& proof_out, std::vector<uint8_t>& root_hash_out) {
            return tree.GenerateRangeProof(HeightToTestKey(0),
                                           HeightToTestKey(end_height),
                                           /*start_inclusive=*/true,
                                           /*end_inclusive=*/true,
                                           grovedb::MerkTree::ValueHashFn(),
                                           &proof_out,
                                           &root_hash_out,
                                           &err);
        };

        int low = 0;
        int high = initial_end_height;
        int served_end_height = -1;
        std::vector<uint8_t> best_proof;
        while (low <= high) {
            const int mid = low + (high - low) / 2;
            std::vector<uint8_t> proof;
            std::vector<uint8_t> root_hash;
            BOOST_REQUIRE_MESSAGE(produce_for_end(mid, proof, root_hash), err);
            BOOST_REQUIRE_EQUAL(root_hash.size(), 32U);

            const size_t payload_size = MERKRANGE_HEADER_BYTES + compact_size_len(proof.size()) + proof.size();
            if (!proof.empty() && payload_size <= TEST_MAX_PROOF_BYTES) {
                served_end_height = mid;
                best_proof = std::move(proof);
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }

        if (!expect_fit) {
            BOOST_CHECK_EQUAL(served_end_height, -1);
            return;
        }

        BOOST_REQUIRE(served_end_height >= 0);
        BOOST_REQUIRE(!best_proof.empty());
        const size_t best_payload = MERKRANGE_HEADER_BYTES + compact_size_len(best_proof.size()) + best_proof.size();
        BOOST_CHECK(best_payload <= TEST_MAX_PROOF_BYTES);

        // Within the estimated window, binary search should find a fitting cap-safe proof.
        if (served_end_height + 1 < static_cast<int>(filter_sizes.size())) {
            std::vector<uint8_t> next_proof;
            std::vector<uint8_t> next_root_hash;
            BOOST_REQUIRE_MESSAGE(produce_for_end(served_end_height + 1, next_proof, next_root_hash), err);
            const size_t next_payload = MERKRANGE_HEADER_BYTES + compact_size_len(next_proof.size()) + next_proof.size();
            if (served_end_height < initial_end_height) {
                BOOST_CHECK(next_payload > TEST_MAX_PROOF_BYTES);
            } else {
                const size_t estimated_next_item =
                    filter_sizes[served_end_height + 1] + ESTIMATED_PER_FILTER_OVERHEAD;
                BOOST_CHECK(approx_payload + estimated_next_item > TEST_MAX_PROOF_BYTES);
            }
        }
    };

    // Worst overhead ratio (tiny cfilters).
    run_case(/*filter_size=*/1, /*count=*/7000, /*expect_fit=*/true);
    // Common-ish compact filter sizes.
    run_case(/*filter_size=*/256, /*count=*/2000, /*expect_fit=*/true);
    run_case(/*filter_size=*/2048, /*count=*/512, /*expect_fit=*/true);
    // Large filter (triggers 4-byte len encoding in proof ops).
    run_case(/*filter_size=*/70000, /*count=*/24, /*expect_fit=*/true);
    // Realistic high-end cfilter size (<500 KiB), still larger than this test cap.
    run_case(/*filter_size=*/480 * 1024, /*count=*/2, /*expect_fit=*/false);
}

BOOST_AUTO_TEST_SUITE_END()
