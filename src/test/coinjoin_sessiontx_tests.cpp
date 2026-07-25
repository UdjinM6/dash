// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <coinjoin/coinjoin.h>

#include <arith_uint256.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coinjoin_sessiontx_tests, BasicTestingSetup)

static uint256 MakeTxid(uint64_t n)
{
    return ArithToUint256(arith_uint256{n + 1});
}

static COutPoint MakeOutPoint(uint64_t n)
{
    return COutPoint{MakeTxid(n), 0};
}

static CTransaction MakeTxSpending(const std::vector<COutPoint>& outpoints)
{
    CMutableTransaction mtx;
    for (const auto& outpoint : outpoints) {
        mtx.vin.emplace_back(outpoint);
    }
    return CTransaction{mtx};
}

BOOST_AUTO_TEST_CASE(sessiontx_tracker_recognises_the_fully_signed_session_transaction)
{
    const COutPoint ours{MakeOutPoint(1)};
    const COutPoint theirs{MakeOutPoint(2)};

    // What a participant holds after signing: the session transaction carrying its own
    // signature only. The coordinator merges the other participants' in afterwards.
    CMutableTransaction mtx;
    mtx.vin.emplace_back(ours);
    mtx.vin.emplace_back(theirs);
    mtx.vin[0].scriptSig = CScript() << OP_1;
    const CTransaction partially_signed{mtx};

    mtx.vin[1].scriptSig = CScript() << OP_2;
    const CTransaction fully_signed{mtx};

    // Dash has no segwit, so signatures are covered by the txid: the id a participant could
    // compute for itself is not the id of the transaction that reaches the network. Keying on
    // it would leave the tracker matching nothing at all.
    BOOST_CHECK(partially_signed.GetHash() != fully_signed.GetHash());

    CoinJoinSessionTxTracker tracker;
    tracker.NoteSignedOutpoints({ours});

    BOOST_CHECK(!tracker.Contains(fully_signed.GetHash()));

    // Recognising it by the input we signed works regardless of whose signatures were added
    tracker.NoteTransaction(fully_signed);
    BOOST_CHECK(tracker.Contains(fully_signed.GetHash()));
}

BOOST_AUTO_TEST_CASE(sessiontx_tracker_ignores_transactions_we_had_no_part_in)
{
    CoinJoinSessionTxTracker tracker;
    tracker.NoteSignedOutpoints({MakeOutPoint(1)});

    const CTransaction tx{MakeTxSpending({MakeOutPoint(2), MakeOutPoint(3)})};
    tracker.NoteTransaction(tx);

    // Suppressing an unrelated transaction would silently damage its propagation
    BOOST_CHECK(!tracker.Contains(tx.GetHash()));
}

BOOST_AUTO_TEST_CASE(sessiontx_tracker_stops_suppressing_once_the_deadline_passes)
{
    const int64_t start{GetTime()};
    SetMockTime(start);

    CoinJoinSessionTxTracker tracker;
    tracker.NoteSignedOutpoints({MakeOutPoint(1)});

    const CTransaction tx{MakeTxSpending({MakeOutPoint(1)})};
    tracker.NoteTransaction(tx);

    BOOST_CHECK(tracker.Contains(tx.GetHash()));

    // Still suppressed right up to the earliest the deadline could have been set. One second
    // short of it, since the deadline may be exactly MIN_SUPPRESS_SECONDS away.
    SetMockTime(start + CoinJoinSessionTxTracker::MIN_SUPPRESS_SECONDS - 1);
    BOOST_CHECK(tracker.Contains(tx.GetHash()));

    // A coordinator that tells nobody but its participants would otherwise strand this
    // transaction in their mempools, holding their mixing inputs hostage, so suppression has to
    // end by itself -- the wallet's ordinary rebroadcast then announces it.
    SetMockTime(start + CoinJoinSessionTxTracker::MAX_SUPPRESS_SECONDS + 1);
    BOOST_CHECK(!tracker.Contains(tx.GetHash()));

    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(sessiontx_tracker_does_not_extend_the_deadline_of_a_known_transaction)
{
    const int64_t start{GetTime()};
    SetMockTime(start);

    CoinJoinSessionTxTracker tracker;
    tracker.NoteSignedOutpoints({MakeOutPoint(1)});

    const CTransaction tx{MakeTxSpending({MakeOutPoint(1)})};
    tracker.NoteTransaction(tx);

    // Re-noting must not push the deadline out, or a transaction that keeps being offered to us
    // would stay suppressed forever and never reach the network. Re-note at the point where the
    // original deadline has certainly passed: a fresh one would land at least
    // MIN_SUPPRESS_SECONDS later, so the check below would still see it suppressed.
    SetMockTime(start + CoinJoinSessionTxTracker::MAX_SUPPRESS_SECONDS);
    tracker.NoteTransaction(tx);

    SetMockTime(start + CoinJoinSessionTxTracker::MAX_SUPPRESS_SECONDS + 1);
    BOOST_CHECK(!tracker.Contains(tx.GetHash()));

    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(sessiontx_tracker_spreads_deadlines_across_transactions)
{
    const int64_t start{GetTime()};
    SetMockTime(start);

    constexpr size_t COUNT{100};
    CoinJoinSessionTxTracker tracker{/*deterministic_rng=*/true};

    std::vector<CTransaction> txs;
    txs.reserve(COUNT);
    for (size_t i = 0; i < COUNT; ++i) {
        const COutPoint outpoint{MakeOutPoint(i)};
        tracker.NoteSignedOutpoints({outpoint});
        txs.push_back(MakeTxSpending({outpoint}));
        tracker.NoteTransaction(txs.back());
    }

    // Halfway through the range the deadlines fall on both sides. A fixed delay would put every
    // one of them on the same side, and participants of a round coming off suppression together
    // is the simultaneity this is meant to avoid. The tracker's context is seeded deterministically
    // above, so this is an exact figure rather than an approximate one. It moves if the bounds or
    // the RNG change, which is fine -- what must not happen is 0 or COUNT, meaning no spread.
    SetMockTime(start + (CoinJoinSessionTxTracker::MIN_SUPPRESS_SECONDS +
                         CoinJoinSessionTxTracker::MAX_SUPPRESS_SECONDS) / 2);

    size_t still_suppressed{0};
    for (const auto& tx : txs) {
        if (tracker.Contains(tx.GetHash())) ++still_suppressed;
    }

    BOOST_CHECK_EQUAL(still_suppressed, size_t{49});

    SetMockTime(0);
}

BOOST_AUTO_TEST_SUITE_END()
