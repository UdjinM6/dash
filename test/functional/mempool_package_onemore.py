#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descendant package tracking carve-out allowing one final transaction in
   an otherwise-full package as long as it has only one parent and is <= 10k in
   size.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet


MAX_ANCESTORS = 25
MAX_DESCENDANTS = 25


class MempoolPackagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-maxorphantxsize=100000"]]

    def chain_tx(self, utxos_to_spend, *, num_outputs=1):
        return self.wallet.send_self_transfer_multi(
            from_node=self.nodes[0],
            utxos_to_spend=utxos_to_spend,
            num_outputs=num_outputs)['new_utxos']

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.wallet.rescan_utxos()

        # MAX_ANCESTORS transactions off a confirmed tx should be fine
        chain = []
        utxo = self.wallet.get_utxo()
        for _ in range(4):
            utxo, utxo2 = self.chain_tx([utxo], num_outputs=2)
            chain.append(utxo2)
        for _ in range(MAX_ANCESTORS - 4):
            utxo, = self.chain_tx([utxo])
            chain.append(utxo)
        second_chain, = self.chain_tx([self.wallet.get_utxo()])

        # Check mempool has MAX_ANCESTORS + 1 transactions in it
        assert_equal(len(self.nodes[0].getrawmempool()), MAX_ANCESTORS + 1)

        # Adding one more transaction on to the chain should fail.
        assert_raises_rpc_error(-26, "too-long-mempool-chain, too many unconfirmed ancestors [limit: 25]", self.chain_tx, [utxo])
        # ...even if it chains on from some point in the middle of the chain.
        assert_raises_rpc_error(-26, "too-long-mempool-chain, too many descendants", self.chain_tx, [chain[2]])
        assert_raises_rpc_error(-26, "too-long-mempool-chain, too many descendants", self.chain_tx, [chain[1]])
        # ...even if it chains on to two parent transactions with one in the chain.
        assert_raises_rpc_error(-26, "too-long-mempool-chain, too many descendants", self.chain_tx, [chain[0], second_chain])
        # ...especially if its > 40k weight
        assert_raises_rpc_error(-26, "too-long-mempool-chain, too many descendants", self.chain_tx, [chain[0]], num_outputs=350)
        # But not if it chains directly off the first transaction
        self.wallet.send_self_transfer_multi(from_node=self.nodes[0], utxos_to_spend=[chain[0]])['tx']
        # and the second chain should work just fine
        self.chain_tx([second_chain])

        # Finally, check that we added two transactions
        assert_equal(len(self.nodes[0].getrawmempool()), MAX_ANCESTORS + 3)


if __name__ == '__main__':
    MempoolPackagesTest().main()
