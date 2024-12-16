#!/usr/bin/env python3
# Copyright (c) 2019-2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

'''
rpc_coinjoin.py

Tests CoinJoin basic RPC
'''

class CoinJoinTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes)
        self.start_nodes()

    def run_test(self):
        node = self.nodes[0]

        node.createwallet(wallet_name='w1', blank=True, disable_private_keys=False)
        w1 = node.get_wallet_rpc('w1')
        self.test_coinjoin_start_stop(w1)
        self.test_setcoinjoinamount(w1)
        self.test_setcoinjoinrounds(w1)

    def test_coinjoin_start_stop(self, node):
        # Start Mixing
        node.coinjoin('start')
        # Get CoinJoin info
        cj_info = node.getcoinjoininfo()
        # Ensure that it started properly
        assert_equal(cj_info['enabled'], True)
        assert_equal(cj_info['running'], True)

        # Stop mixing
        node.coinjoin('stop')
        # Get CoinJoin info
        cj_info = node.getcoinjoininfo()
        # Ensure that it stopped properly
        assert_equal(cj_info['enabled'], True)
        assert_equal(cj_info['running'], False)

    def test_setcoinjoinamount(self, node):
        # Test normal and large values
        for value in [50, 1200000]:
            node.setcoinjoinamount(value)
            assert_equal(node.getcoinjoininfo()['max_amount'], value)

    def test_setcoinjoinrounds(self, node):
        # Test normal values
        node.setcoinjoinrounds(5)
        assert_equal(node.getcoinjoininfo()['max_rounds'], 5)


if __name__ == '__main__':
    CoinJoinTest().main()
