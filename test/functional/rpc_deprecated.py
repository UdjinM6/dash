#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test deprecation of RPC calls."""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error, assert_equal

class DeprecatedRpcTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[], ["-deprecatedrpc=permissive_bool"]]

    def run_test(self):
        self.test_permissive_bool()
        # This test should be used to verify correct behaviour of deprecated
        # RPC methods with and without the -deprecatedrpc flags. For example:
        #
        # In set_test_params:
        # self.extra_args = [[], ["-deprecatedrpc=generate"]]
        #
        # In run_test:
        # self.log.info("Test generate RPC")
        # assert_raises_rpc_error(-32, 'The wallet generate rpc method is deprecated', self.nodes[0].rpc.generate, 1)
        # self.generate(self.nodes[1], 1)
        self.log.info("No tested deprecated RPC methods")


    def test_permissive_bool(self):
        self.generate(self.nodes[0], 1)
        assert_equal([], self.nodes[0].protx("list", "valid", True))
        assert_raises_rpc_error(-8, 'detailed must be a JSON boolean. Pass -deprecatedrpc=permissive_bool to allow legacy boolean parsing.', self.nodes[0].protx, "list", "valid", 1)
        assert_raises_rpc_error(-8, 'detailed must be a JSON boolean. Pass -deprecatedrpc=permissive_bool to allow legacy boolean parsing.', self.nodes[0].protx, "list", "valid", "1")

        assert_equal([], self.nodes[1].protx("list", "valid", True))
        assert_equal([], self.nodes[1].protx("list", "valid", 1))
        assert_equal([], self.nodes[1].protx("list", "valid", "1"))


if __name__ == '__main__':
    DeprecatedRpcTest().main()
