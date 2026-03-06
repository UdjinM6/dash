#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests serving merk range proofs over P2P."""

from test_framework.messages import (
    FILTER_TYPE_BASIC,
    NODE_MERKRANGE,
    msg_getmerkrange,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)


class MerkRangeClient(P2PInterface):
    def __init__(self):
        super().__init__()
        self.merkranges = []

    def on_merkrange(self, message):
        self.merkranges.append(message)

    def pop_merkranges(self):
        merkranges = self.merkranges
        self.merkranges = []
        return merkranges


class MerkRangeServingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-blockfilterindex", "-merkrangeindex=basic"]]

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(MerkRangeClient())

        self.log.info("Check merk range service bit is signaled")
        assert peer.nServices & NODE_MERKRANGE != 0
        assert int(node.getnetworkinfo()["localservices"], 16) & NODE_MERKRANGE != 0

        def wait_merk_index_synced():
            self.wait_until(
                lambda: node.getindexinfo().get("basic merk range index", {}).get("synced", False)
            )

        def request_merkrange(stop_hash, start_height):
            peer.send_message(
                msg_getmerkrange(
                    filter_type=FILTER_TYPE_BASIC,
                    start_height=start_height,
                    stop_hash=stop_hash,
                )
            )
            peer.wait_until(lambda: len(peer.merkranges) > 0)
            responses = peer.pop_merkranges()
            assert_equal(len(responses), 1)
            response = responses[0]
            assert_equal(response.target_stop_hash, stop_hash)
            assert_equal(response.start_height, start_height)
            assert response.snapshot_tip_hash != 0
            assert response.end_height >= start_height
            assert_greater_than(len(response.proof), 0)
            return response

        def request_full_range(stop_hash, start_height):
            chunks = []
            next_start = start_height
            while True:
                chunk = request_merkrange(stop_hash=stop_hash, start_height=next_start)
                chunks.append(chunk)
                if len(chunks) > 1:
                    assert_equal(chunk.snapshot_tip_hash, chunks[0].snapshot_tip_hash)
                if chunk.served_stop_hash == stop_hash:
                    return chunks
                next_start = chunk.end_height + 1

        self.generate(node, 220)
        wait_merk_index_synced()

        checkpoint_hash = int(node.getblockhash(150), 16)
        tip_hash = int(node.getbestblockhash(), 16)

        self.log.info("Basic merkrange query [50, 150] with RPC parity")
        chunks = request_full_range(stop_hash=checkpoint_hash, start_height=50)
        response = chunks[0]
        assert_equal(response.filter_type, FILTER_TYPE_BASIC)
        assert_equal(response.target_stop_hash, checkpoint_hash)
        assert_equal(response.start_height, 50)
        assert response.end_height >= 50
        rpc_range = node.getmerkrange(response.start_height, response.end_height, "basic")
        assert_equal(response.snapshot_tip_hash, int(rpc_range["snapshot_tip_hash"], 16))
        assert_equal(response.root_hash, int(rpc_range["root_hash"], 16))
        assert_equal(response.proof.hex(), rpc_range["proof"])
        assert_equal(len(response.proof), rpc_range["proof_size"])

        self.log.info("Single-block merkrange [100, 100]")
        single_hash = int(node.getblockhash(100), 16)
        single_chunks = request_full_range(stop_hash=single_hash, start_height=100)
        assert_equal(len(single_chunks), 1)
        single = single_chunks[0]
        assert_equal(single.start_height, 100)
        assert_equal(single.end_height, 100)
        assert_equal(single.target_stop_hash, single_hash)
        assert_equal(single.served_stop_hash, single_hash)
        assert_greater_than(len(single.proof), 0)

        self.log.info("getmerkroot at tip matches merkrange root from all ranges")
        tip_height = node.getblockcount()
        merkroot = node.getmerkroot(node.getbestblockhash(), "basic")
        full_range = node.getmerkrange(0, tip_height, "basic")
        assert_equal(merkroot, full_range["root_hash"])
        assert_equal(merkroot, rpc_range["root_hash"])

        self.log.info("Genesis-spanning merkrange [0, 150]")
        genesis_chunks = request_full_range(stop_hash=checkpoint_hash, start_height=0)
        assert_equal(genesis_chunks[0].start_height, 0)
        assert_equal(genesis_chunks[-1].end_height, 150)
        assert_greater_than(len(genesis_chunks[0].proof), 0)

        self.log.info("Non-tip stop_hash on active chain should still be served")
        older_active_chunks = request_full_range(stop_hash=checkpoint_hash, start_height=120)
        assert_equal(older_active_chunks[0].target_stop_hash, checkpoint_hash)
        assert_equal(older_active_chunks[-1].served_stop_hash, checkpoint_hash)
        assert_equal(older_active_chunks[0].start_height, 120)
        assert_equal(older_active_chunks[-1].end_height, 150)
        assert_greater_than(len(older_active_chunks[0].proof), 0)

        self.log.info("Stale fork stop_hash should be ignored (no response)")
        reorg_base_height = node.getblockcount() - 20
        reorg_base_hash = node.getblockhash(reorg_base_height)
        old_tip_hash = tip_hash
        node.invalidateblock(reorg_base_hash)
        self.wait_until(lambda: node.getblockcount() == reorg_base_height - 1)
        wait_merk_index_synced()

        peer.pop_merkranges()
        peer.send_message(
            msg_getmerkrange(
                filter_type=FILTER_TYPE_BASIC,
                start_height=50,
                stop_hash=old_tip_hash,
            )
        )
        peer.sync_with_ping()
        assert_equal(len(peer.pop_merkranges()), 0)

        self.log.info("Invalid merkrange requests disconnect peers")
        for bad_start_height, bad_stop_hash in [
            (151, checkpoint_hash),
            (2**31, checkpoint_hash),
        ]:
            bad_peer = node.add_p2p_connection(P2PInterface())
            bad_peer.send_message(
                msg_getmerkrange(
                    filter_type=FILTER_TYPE_BASIC,
                    start_height=bad_start_height,
                    stop_hash=bad_stop_hash,
                )
            )
            bad_peer.wait_for_disconnect()


if __name__ == '__main__':
    MerkRangeServingTest().main()
