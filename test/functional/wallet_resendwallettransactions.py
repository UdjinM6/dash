#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the wallet resends transactions periodically."""

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ResendWalletTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]  # alias

        peer_first = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a new transaction and wait until it's broadcast")
        txid = node.sendtoaddress(node.getnewaddress(), 1)

        # Wallet rebroadcast is first scheduled 1 min sec after startup (see
        # nNextResend in ResendWalletTransactions()). Tell scheduler to call
        # MaybeResendWalletTxs now to initialize nNextResend before the first
        # setmocktime call below.
        node.mockscheduler(60)

        # Can take a few seconds due to transaction trickling
        def wait_p2p():
            self.bump_mocktime(1)
            return peer_first.tx_invs_received[int(txid, 16)] >= 1
        self.wait_until(wait_p2p)

        # Add a second peer since txs aren't rebroadcast to the same peer (see m_tx_inventory_known_filter)
        peer_second = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a block")
        # Create and submit a block without the transaction.
        # Transactions are only rebroadcast if there has been a block at least five minutes
        # after the last time we tried to broadcast. Use mocktime and give an extra minute to be sure.
        block_time = self.mocktime + 6 * 60
        node.setmocktime(block_time)
        block = create_block(int(node.getbestblockhash(), 16), create_coinbase(node.getblockcount() + 1), block_time)
        block.solve()
        node.submitblock(block.serialize().hex())

        # Set correct m_best_block_time, which is used in ResendWalletTransactions
        node.syncwithvalidationinterfacequeue()

        # Transaction should not be rebroadcast within first 12 hours
        # Leave 2 mins for buffer
        twelve_hrs = 12 * 60 * 60
        two_min = 2 * 60
        node.setmocktime(self.mocktime + twelve_hrs - two_min)
        node.mockscheduler(60)  # Tell scheduler to call MaybeResendWalletTxs now
        assert_equal(int(txid, 16) in peer_second.get_invs(), False)

        self.log.info("Bump time & check that transaction is rebroadcast")
        # Transaction should be rebroadcast approximately 24 hours in the future,
        # but can range from 12-36. So bump 36 hours to be sure.
        with node.assert_debug_log(['ResendWalletTransactions: resubmit 1 unconfirmed transactions']):
            node.setmocktime(self.mocktime + 36 * 60 * 60)
            # Tell scheduler to call MaybeResendWalletTxs now.
            node.mockscheduler(60)
        # Give some time for trickle to occur
        node.setmocktime(self.mocktime + 36 * 60 * 60 + 600)
        peer_second.wait_for_broadcast([txid])


if __name__ == '__main__':
    ResendWalletTransactionsTest().main()
