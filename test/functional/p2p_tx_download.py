#!/usr/bin/env python3
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test transaction download behavior
"""

from test_framework.messages import (
    CInv,
    MSG_TX,
    MSG_TYPE_MASK,
    msg_inv,
    msg_notfound,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.wallet import MiniWallet


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.tx_getdata_count = 0

    def on_getdata(self, message):
        for i in message.inv:
            if i.type & MSG_TYPE_MASK == MSG_TX:
                self.tx_getdata_count += 1


# Constants from net_processing
GETDATA_TX_INTERVAL = 60  # seconds
MAX_GETDATA_RANDOM_DELAY = 2  # seconds
INBOUND_PEER_TX_DELAY = 2  # seconds
MAX_GETDATA_IN_FLIGHT = 100
TX_EXPIRY_INTERVAL = GETDATA_TX_INTERVAL * 10

# Python test constants
NUM_INBOUND = 10
MAX_GETDATA_INBOUND_WAIT = GETDATA_TX_INTERVAL + MAX_GETDATA_RANDOM_DELAY + INBOUND_PEER_TX_DELAY


class TxDownloadTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def test_tx_requests(self):
        self.log.info("Test that we request transactions from all our peers, eventually")

        txid = 0xdeadbeef

        self.log.info("Announce the txid from each incoming peer to node 0")
        msg = msg_inv([CInv(t=1, h=txid)])
        for p in self.nodes[0].p2ps:
            p.send_and_ping(msg)

        outstanding_peer_index = [i for i in range(len(self.nodes[0].p2ps))]

        def getdata_found(peer_index):
            p = self.nodes[0].p2ps[peer_index]
            with p2p_lock:
                return p.last_message.get("getdata") and p.last_message["getdata"].inv[-1].hash == txid

        while outstanding_peer_index:
            self.bump_mocktime(MAX_GETDATA_INBOUND_WAIT)
            self.wait_until(lambda: any(getdata_found(i) for i in outstanding_peer_index))
            for i in outstanding_peer_index:
                if getdata_found(i):
                    outstanding_peer_index.remove(i)

        self.log.info("All outstanding peers received a getdata")

    def test_inv_block(self):
        self.log.info("Generate a transaction on node 0")
        tx = self.wallet.create_self_transfer()
        txid = int(tx['txid'], 16)

        self.log.info(
            "Announce the transaction to all nodes from all {} incoming peers, but never send it".format(NUM_INBOUND))
        msg = msg_inv([CInv(t=1, h=txid)])
        for p in self.peers:
            p.send_and_ping(msg)
            p.sync_with_ping()
            self.bump_mocktime(1)

        self.log.info("Put the tx in node 0's mempool")
        self.nodes[0].sendrawtransaction(tx['hex'])

        # Since node 1 is connected outbound to an honest peer (node 0), it
        # should get the tx within a timeout. (Assuming that node 0
        # announced the tx within the timeout)
        # The timeout is the sum of
        # * the worst case until the tx is first requested from an inbound
        #   peer, plus
        # * the first time it is re-requested from the outbound peer, plus
        # * 2 seconds to avoid races
        timeout = 2 + (MAX_GETDATA_RANDOM_DELAY + INBOUND_PEER_TX_DELAY) + (
            GETDATA_TX_INTERVAL + MAX_GETDATA_RANDOM_DELAY)
        self.log.info("Tx should be received at node 1 after {} seconds".format(timeout))
        self.sync_mempools(timeout=timeout)

    def test_in_flight_max(self):
        self.log.info("Test that we don't request more than {} transactions from any peer, every {} minutes".format(
            MAX_GETDATA_IN_FLIGHT, TX_EXPIRY_INTERVAL / 60))
        txids = [i for i in range(MAX_GETDATA_IN_FLIGHT + 2)]

        p = self.nodes[0].p2ps[0]

        with p2p_lock:
            p.tx_getdata_count = 0

        p.send_message(msg_inv([CInv(t=1, h=i) for i in txids]))

        def wait_for_tx_getdata(target):
            self.bump_mocktime(1)
            return p.tx_getdata_count >= target

        p.wait_until(lambda: wait_for_tx_getdata(MAX_GETDATA_IN_FLIGHT))

        with p2p_lock:
            assert_equal(p.tx_getdata_count, MAX_GETDATA_IN_FLIGHT)

        self.log.info("Now check that if we send a NOTFOUND for a transaction, we'll get one more request")
        p.send_message(msg_notfound(vec=[CInv(t=1, h=txids[0])]))
        p.wait_until(lambda: wait_for_tx_getdata(MAX_GETDATA_IN_FLIGHT + 1), timeout=10)
        with p2p_lock:
            assert_equal(p.tx_getdata_count, MAX_GETDATA_IN_FLIGHT + 1)

        WAIT_TIME = TX_EXPIRY_INTERVAL // 2 + TX_EXPIRY_INTERVAL
        self.log.info("if we wait about {} minutes, we should eventually get more requests".format(WAIT_TIME / 60))
        self.bump_mocktime(WAIT_TIME)
        p.wait_until(lambda: wait_for_tx_getdata(MAX_GETDATA_IN_FLIGHT + 2))

    def test_spurious_notfound(self):
        self.log.info('Check that spurious notfound is ignored')
        self.nodes[0].p2ps[0].send_message(msg_notfound(vec=[CInv(1, 1)]))

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.wallet.rescan_utxos()

        # Run each test against new bitcoind instances, as setting mocktimes has long-term effects on when
        # the next trickle relay event happens.
        for test in [self.test_spurious_notfound, self.test_in_flight_max, self.test_inv_block, self.test_tx_requests]:
            self.stop_nodes()
            self.start_nodes()
            self.connect_nodes(1, 0)
            # Setup the p2p connections
            self.peers = []
            for node in self.nodes:
                for _ in range(NUM_INBOUND):
                    self.peers.append(node.add_p2p_connection(TestP2PConn()))
            self.log.info("Nodes are setup with {} incoming connections each".format(NUM_INBOUND))
            test()


if __name__ == '__main__':
    TxDownloadTest().main()
