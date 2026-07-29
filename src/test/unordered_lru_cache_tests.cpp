// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <unordered_lru_cache.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using IntCache = unordered_lru_cache<int, int, std::hash<int>>;

BOOST_FIXTURE_TEST_SUITE(unordered_lru_cache_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(no_truncation_below_threshold)
{
    // the default truncate threshold is twice the max size
    IntCache cache(10);
    BOOST_CHECK(cache.max_size() == 10);

    for (int i = 0; i < 20; i++) {
        cache.insert(i, i);
    }

    // reaching the threshold is not enough to trigger truncation
    for (int i = 0; i < 20; i++) {
        BOOST_CHECK(cache.exists(i));
    }
}

BOOST_AUTO_TEST_CASE(truncation_keeps_most_recent)
{
    IntCache cache(10);

    // exceeding the threshold truncates down to the max size
    for (int i = 0; i < 21; i++) {
        cache.insert(i, i);
    }

    for (int i = 0; i < 21; i++) {
        BOOST_CHECK(cache.exists(i) == (i >= 11));
    }

    // the retained values are intact
    for (int i = 11; i < 21; i++) {
        int value{0};
        BOOST_CHECK(cache.get(i, value));
        BOOST_CHECK(value == i);
    }
}

BOOST_AUTO_TEST_CASE(truncation_honors_explicit_threshold)
{
    IntCache cache(5, 6);
    BOOST_CHECK(cache.max_size() == 5);

    for (int i = 0; i < 6; i++) {
        cache.insert(i, i);
    }
    for (int i = 0; i < 6; i++) {
        BOOST_CHECK(cache.exists(i));
    }

    cache.insert(6, 6);
    for (int i = 0; i < 7; i++) {
        BOOST_CHECK(cache.exists(i) == (i >= 2));
    }
}

BOOST_AUTO_TEST_CASE(get_refreshes_recency)
{
    IntCache cache(4);

    // fill up to the threshold without triggering truncation
    for (int i = 0; i < 8; i++) {
        cache.insert(i, i);
    }

    // make the oldest entry the most recently used one
    int value{0};
    BOOST_CHECK(cache.get(0, value));
    BOOST_CHECK(value == 0);

    // this insert exceeds the threshold and truncates
    cache.insert(8, 8);

    // the refreshed entry survives, the entries it outranks do not
    for (int i = 0; i < 9; i++) {
        const bool expected = i == 0 || i >= 6;
        BOOST_CHECK(cache.exists(i) == expected);
    }
}

BOOST_AUTO_TEST_CASE(exists_refreshes_recency)
{
    IntCache cache(4);

    for (int i = 0; i < 8; i++) {
        cache.insert(i, i);
    }

    BOOST_CHECK(cache.exists(1));

    cache.insert(8, 8);

    for (int i = 0; i < 9; i++) {
        const bool expected = i == 1 || i >= 6;
        BOOST_CHECK(cache.exists(i) == expected);
    }
}

BOOST_AUTO_TEST_SUITE_END()
