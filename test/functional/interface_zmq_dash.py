#!/usr/bin/env python3
# Copyright (c) 2018-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the dash specific ZMQ notification interfaces."""

import configparser
from enum import Enum
import io
import json
import random
import struct
import time

from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.p2p import P2PInterface
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    p2p_port,
)
from test_framework.messages import (
    CBlock,
    CGovernanceObject,
    CGovernanceVote,
    CInv,
    COutPoint,
    CRecoveredSig,
    CTransaction,
    from_hex,
    hash256,
    msg_clsig,
    msg_inv,
    msg_isdlock,
    msg_tx,
    MSG_TX,
    MSG_TYPE_MASK,
    ser_string,
    uint256_from_str,
    uint256_to_string
)


class ZMQPublisher(Enum):
    hash_chain_lock = "hashchainlock"
    hash_tx_lock = "hashtxlock"
    hash_governance_vote = "hashgovernancevote"
    hash_governance_object = "hashgovernanceobject"
    hash_instantsend_doublespend = "hashinstantsenddoublespend"
    hash_recovered_sig = "hashrecoveredsig"
    raw_chain_lock = "rawchainlock"
    raw_chain_lock_sig = "rawchainlocksig"
    raw_tx_lock = "rawtxlock"
    raw_tx_lock_sig = "rawtxlocksig"
    raw_governance_vote = "rawgovernancevote"
    raw_governance_object = "rawgovernanceobject"
    raw_instantsend_doublespend = "rawinstantsenddoublespend"
    raw_recovered_sig = "rawrecoveredsig"


class ZMQSubscriber:
    def __init__(self, socket, topic):
        self.socket = socket
        self.topic = topic

        import zmq
        self.socket.setsockopt(zmq.SUBSCRIBE, self.topic)

    def receive(self, flags=0):
        topic, body, seq = self.socket.recv_multipart(flags)
        # Topic should match the subscriber topic.
        assert_equal(topic, self.topic)
        return io.BytesIO(body)


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.islocks = {}
        self.txes = {}

    def send_islock(self, islock):
        hash = uint256_from_str(hash256(islock.serialize()))
        self.islocks[hash] = islock

        inv = msg_inv([CInv(31, hash)])
        self.send_message(inv)

    def send_tx(self, tx):
        hash = uint256_from_str(hash256(tx.serialize()))
        self.txes[hash] = tx

        inv = msg_inv([CInv(MSG_TX, hash)])
        self.send_message(inv)

    def on_getdata(self, message):
        for inv in message.inv:
            if ((inv.type & MSG_TYPE_MASK) == 30 or (inv.type & MSG_TYPE_MASK) == 31) and inv.hash in self.islocks:
                self.send_message(self.islocks[inv.hash])
            if (inv.type & MSG_TYPE_MASK) == MSG_TX and inv.hash in self.txes:
                self.send_message(self.txes[inv.hash])


class DashZMQTest (DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(5, 4)

        # That's where the zmq publisher will listen for subscriber
        self.zmq_port_base = p2p_port(self.num_nodes + 1)
        self.address = f"tcp://127.0.0.1:{self.zmq_port_base}"

        # node0 creates all available ZMQ publisher
        node0_extra_args = [f"-zmqpub{pub.value}={self.address}" for pub in ZMQPublisher]
        node0_extra_args.append("-whitelist=127.0.0.1")
        node0_extra_args.append("-watchquorums")  # have to watch quorums to receive recsigs and trigger zmq

        #extra_args = [node0_extra_args, [], [], [], []]
        self.extra_args[0] = node0_extra_args

        self.set_dash_llmq_test_params(4, 4)

    def skip_test_if_missing_module(self):
        self.skip_if_no_py3_zmq()
        self.skip_if_no_bitcoind_zmq()
        self.skip_if_no_wallet()

    def run_test(self):
        self.subscribers = {}
        # Check that dashd has been built with ZMQ enabled.
        config = configparser.ConfigParser()
        config.read_file(open(self.options.configfile))
        import zmq

        try:
            # Setup the ZMQ subscriber context
            self.zmq_context = zmq.Context()
            # Initialize the network
            self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
            self.log.info("Test RPC hex getbestchainlock before any CL appeared")
            assert_raises_rpc_error(-32603, "Unable to find any ChainLock", self.nodes[0].getbestchainlock)
            self.wait_for_sporks_same()

            self.mine_cycle_quorum()

            self.sync_blocks()
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())
            # Wait a moment to avoid subscribing to recovered sig in the test before the one from the chainlock
            # has been sent which leads to test failure.
            time.sleep(1)
            # Test all dash related ZMQ publisher
            self.test_recovered_signature_publishers()
            self.test_chainlock_publishers()
            self.test_governance_publishers()
            self.test_getzmqnotifications()
            self.test_instantsend_publishers()
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())
            self.test_instantsend_publishers()
            # At this point, we need to move forward 3 cycles (3 x 24 blocks) so the first 3 quarters can be created (without DKG sessions)
            self.generate(self.nodes[0], 24)
            self.mine_cycle_quorum()
            self.test_instantsend_publishers()
        finally:
            # Destroy the ZMQ context.
            self.log.debug("Destroying ZMQ context")
            self.zmq_context.destroy(linger=None)

    def generate_blocks(self, num_blocks):
        mninfos_online = self.mninfo.copy()
        nodes = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos_online]
        self.generate(self.nodes[0], num_blocks, sync_fun=lambda: self.sync_blocks(nodes))

    def subscribe(self, publishers):
        import zmq
        # Setup the ZMQ subscriber socket
        socket = self.zmq_context.socket(zmq.SUB)
        socket.set(zmq.RCVTIMEO, 60000)
        socket.connect(self.address)
        # Subscribe to a list of ZMQPublishers
        for pub in publishers:
            self.subscribers[pub] = ZMQSubscriber(socket, pub.value.encode())

    def unsubscribe(self, publishers):
        # Unsubscribe from a list of ZMQPublishers
        for pub in publishers:
            del self.subscribers[pub]

    def test_recovered_signature_publishers(self):

        def validate_recovered_sig(request_id, msg_hash):
            # Make sure the recovered sig exists by RPC
            self.wait_for_recovered_sig(request_id, msg_hash)
            rpc_recovered_sig = self.mninfo[0].get_node(self).quorum('getrecsig', 100, request_id, msg_hash)
            # Validate hashrecoveredsig
            zmq_recovered_sig_hash = self.subscribers[ZMQPublisher.hash_recovered_sig].receive().read(32).hex()
            assert_equal(zmq_recovered_sig_hash, msg_hash)
            # Validate rawrecoveredsig
            zmq_recovered_sig_raw = CRecoveredSig()
            zmq_recovered_sig_raw.deserialize(self.subscribers[ZMQPublisher.raw_recovered_sig].receive())
            assert_equal(zmq_recovered_sig_raw.llmqType, rpc_recovered_sig['llmqType'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.quorumHash), rpc_recovered_sig['quorumHash'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.id), rpc_recovered_sig['id'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.msgHash), rpc_recovered_sig['msgHash'])
            assert_equal(zmq_recovered_sig_raw.sig.hex(), rpc_recovered_sig['sig'])

        recovered_sig_publishers = [
            ZMQPublisher.hash_recovered_sig,
            ZMQPublisher.raw_recovered_sig
        ]
        self.log.info("Testing %d recovered signature publishers" % len(recovered_sig_publishers))
        # Subscribe to recovered signature messages
        self.subscribe(recovered_sig_publishers)
        # Generate a ChainLock and make sure this leads to valid recovered sig ZMQ messages
        rpc_last_block_hash = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        self.wait_for_chainlocked_block_all_nodes(rpc_last_block_hash)
        height = self.nodes[0].getblockcount()
        rpc_request_id = hash256(ser_string(b"clsig") + struct.pack("<I", height))[::-1].hex()
        validate_recovered_sig(rpc_request_id, rpc_last_block_hash)
        # Sign an arbitrary and make sure this leads to valid recovered sig ZMQ messages
        sign_id = uint256_to_string(random.getrandbits(256))
        sign_msg_hash = uint256_to_string(random.getrandbits(256))
        quorumHash = self.nodes[0].quorum("selectquorum", 100, sign_id)["quorumHash"]
        for mn in self.get_quorum_masternodes(quorumHash): # type: MasternodeInfo
            mn.get_node(self).quorum("sign", 100, sign_id, sign_msg_hash)
        validate_recovered_sig(sign_id, sign_msg_hash)
        # Unsubscribe from recovered signature messages
        self.unsubscribe(recovered_sig_publishers)

    def test_chainlock_publishers(self):
        chain_lock_publishers = [
            ZMQPublisher.hash_chain_lock,
            ZMQPublisher.raw_chain_lock,
            ZMQPublisher.raw_chain_lock_sig
        ]
        self.log.info("Testing %d ChainLock publishers" % len(chain_lock_publishers))
        # Subscribe to ChainLock messages
        self.subscribe(chain_lock_publishers)
        # Generate ChainLock
        generated_hash = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        self.wait_for_chainlocked_block_all_nodes(generated_hash)
        rpc_best_chain_lock = self.nodes[0].getbestchainlock()
        rpc_best_chain_lock_hash = rpc_best_chain_lock["blockhash"]
        rpc_best_chain_lock_sig = rpc_best_chain_lock["signature"]
        assert_equal(generated_hash, rpc_best_chain_lock_hash)
        rpc_chain_locked_block = self.nodes[0].getblock(rpc_best_chain_lock_hash)
        rpc_chain_lock_height = rpc_chain_locked_block["height"]
        rpc_chain_lock_hash = rpc_chain_locked_block["hash"]
        assert_equal(generated_hash, rpc_chain_lock_hash)
        # Validate hashchainlock
        zmq_chain_lock_hash = self.subscribers[ZMQPublisher.hash_chain_lock].receive().read(32).hex()
        assert_equal(zmq_chain_lock_hash, rpc_best_chain_lock_hash)
        # Validate rawchainlock
        zmq_chain_locked_block = CBlock()
        zmq_chain_locked_block.deserialize(self.subscribers[ZMQPublisher.raw_chain_lock].receive())
        assert zmq_chain_locked_block.is_valid()
        assert_equal(zmq_chain_locked_block.hash, rpc_chain_lock_hash)
        # Validate rawchainlocksig
        zmq_chain_lock_sig_stream = self.subscribers[ZMQPublisher.raw_chain_lock_sig].receive()
        zmq_chain_locked_block = CBlock()
        zmq_chain_locked_block.deserialize(zmq_chain_lock_sig_stream)
        assert zmq_chain_locked_block.is_valid()
        zmq_chain_lock = msg_clsig()
        zmq_chain_lock.deserialize(zmq_chain_lock_sig_stream)
        assert_equal(zmq_chain_lock.height, rpc_chain_lock_height)
        assert_equal(uint256_to_string(zmq_chain_lock.blockHash), rpc_chain_lock_hash)
        assert_equal(zmq_chain_locked_block.hash, rpc_chain_lock_hash)
        assert_equal(zmq_chain_lock.sig.hex(), rpc_best_chain_lock_sig)
        assert_equal(zmq_chain_lock.serialize().hex(), self.nodes[0].getbestchainlock()['hex'])
        # Unsubscribe from ChainLock messages
        self.unsubscribe(chain_lock_publishers)

    def test_instantsend_publishers(self):
        import zmq
        instantsend_publishers = [
            ZMQPublisher.hash_tx_lock,
            ZMQPublisher.raw_tx_lock,
            ZMQPublisher.raw_tx_lock_sig,
            ZMQPublisher.hash_instantsend_doublespend,
            ZMQPublisher.raw_instantsend_doublespend
        ]
        self.log.info("Testing %d InstantSend publishers" % len(instantsend_publishers))
        # Subscribe to InstantSend messages
        self.subscribe(instantsend_publishers)
        # Initialize test node
        self.test_node = self.nodes[0].add_p2p_connection(TestP2PConn())
        # Make sure all nodes agree
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())
        # Create two raw TXs, they will conflict with each other
        rpc_raw_tx_1 = self.create_raw_tx(self.nodes[0], self.nodes[0], 1, 1, 100)
        rpc_raw_tx_2 = self.create_raw_tx(self.nodes[0], self.nodes[0], 1, 1, 100)
        assert_equal(['None'], self.nodes[0].getislocks([rpc_raw_tx_1['txid']]))
        # Send the first transaction and wait for the InstantLock
        rpc_raw_tx_1_hash = self.nodes[0].sendrawtransaction(rpc_raw_tx_1['hex'])
        self.bump_mocktime(30)
        self.wait_for_instantlock(rpc_raw_tx_1_hash, self.nodes[0])
        # Validate hashtxlock
        zmq_tx_lock_hash = self.subscribers[ZMQPublisher.hash_tx_lock].receive().read(32).hex()
        assert_equal(zmq_tx_lock_hash, rpc_raw_tx_1['txid'])
        # Validate rawtxlock
        zmq_tx_lock_raw = CTransaction()
        zmq_tx_lock_raw.deserialize(self.subscribers[ZMQPublisher.raw_tx_lock].receive())
        assert zmq_tx_lock_raw.is_valid()
        assert_equal(zmq_tx_lock_raw.hash, rpc_raw_tx_1['txid'])
        # Validate rawtxlocksig
        zmq_tx_lock_sig_stream = self.subscribers[ZMQPublisher.raw_tx_lock_sig].receive()
        zmq_tx_lock_tx = CTransaction()
        zmq_tx_lock_tx.deserialize(zmq_tx_lock_sig_stream)
        assert zmq_tx_lock_tx.is_valid()
        assert_equal(zmq_tx_lock_tx.hash, rpc_raw_tx_1['txid'])
        zmq_tx_lock = msg_isdlock()
        zmq_tx_lock.deserialize(zmq_tx_lock_sig_stream)
        assert_equal(rpc_raw_tx_1['txid'], self.nodes[0].getislocks([rpc_raw_tx_1['txid']])[0]['txid'])
        assert_equal(zmq_tx_lock.serialize().hex(), self.nodes[0].getislocks([rpc_raw_tx_1['txid']])[0]['hex'])
        assert_equal(uint256_to_string(zmq_tx_lock.txid), rpc_raw_tx_1['txid'])
        # Try to send the second transaction. This must throw an RPC error because it conflicts with rpc_raw_tx_1
        # which already got the InstantSend lock.
        assert_raises_rpc_error(-26, "tx-txlock-conflict", self.nodes[0].sendrawtransaction, rpc_raw_tx_2['hex'])
        # Validate hashinstantsenddoublespend
        zmq_double_spend_hash2 = self.subscribers[ZMQPublisher.hash_instantsend_doublespend].receive().read(32).hex()
        zmq_double_spend_hash1 = self.subscribers[ZMQPublisher.hash_instantsend_doublespend].receive().read(32).hex()
        assert_equal(zmq_double_spend_hash2, rpc_raw_tx_2['txid'])
        assert_equal(zmq_double_spend_hash1, rpc_raw_tx_1['txid'])
        # Validate rawinstantsenddoublespend
        zmq_double_spend_tx_2 = CTransaction()
        zmq_double_spend_tx_2.deserialize(self.subscribers[ZMQPublisher.raw_instantsend_doublespend].receive())
        assert zmq_double_spend_tx_2.is_valid()
        assert_equal(zmq_double_spend_tx_2.hash, rpc_raw_tx_2['txid'])
        zmq_double_spend_tx_1 = CTransaction()
        zmq_double_spend_tx_1.deserialize(self.subscribers[ZMQPublisher.raw_instantsend_doublespend].receive())
        assert zmq_double_spend_tx_1.is_valid()
        assert_equal(zmq_double_spend_tx_1.hash, rpc_raw_tx_1['txid'])
        # No islock notifications when tx is not received yet
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        rpc_raw_tx_3 = self.create_raw_tx(self.nodes[0], self.nodes[0], 1, 1, 100)
        isdlock = self.create_isdlock(rpc_raw_tx_3['hex'])
        self.test_node.send_islock(isdlock)
        # Validate NO hashtxlock
        time.sleep(1)
        try:
            self.subscribers[ZMQPublisher.hash_tx_lock].receive(zmq.NOBLOCK)
            assert False
        except zmq.ZMQError:
            # this is expected
            pass
        # Now send the tx itself
        self.test_node.send_tx(from_hex(msg_tx(),rpc_raw_tx_3['hex']))
        self.bump_mocktime(30)
        self.wait_for_instantlock(rpc_raw_tx_3['txid'], self.nodes[0])
        # Validate hashtxlock
        zmq_tx_lock_hash = self.subscribers[ZMQPublisher.hash_tx_lock].receive().read(32).hex()
        assert_equal(zmq_tx_lock_hash, rpc_raw_tx_3['txid'])
        # Drop test node connection
        self.nodes[0].disconnect_p2ps()
        # Unsubscribe from InstantSend messages
        self.unsubscribe(instantsend_publishers)

    def test_governance_publishers(self):
        governance_publishers = [
            ZMQPublisher.hash_governance_object,
            ZMQPublisher.raw_governance_object,
            ZMQPublisher.hash_governance_vote,
            ZMQPublisher.raw_governance_vote
        ]
        self.log.info("Testing %d governance publishers" % len(governance_publishers))
        # Subscribe to governance messages
        self.subscribe(governance_publishers)
        # Create a proposal and submit it to the network
        proposal_rev = 1
        proposal_time = int(time.time())
        proposal_data = {
            "type": 1,  # GOVERNANCE_OBJECT_PROPOSAL
            "name": "Test",
            "start_epoch": proposal_time,
            "end_epoch": proposal_time + 60,
            "payment_amount": 5,
            "payment_address": self.nodes[0].getnewaddress(),
            "url": "https://dash.org"
        }
        proposal_hex = ''.join(format(x, '02x') for x in json.dumps(proposal_data).encode())
        collateral = self.nodes[0].gobject("prepare", "0", proposal_rev, proposal_time, proposal_hex)
        self.bump_mocktime(30)
        self.wait_for_instantlock(collateral, self.nodes[0])
        self.generate(self.nodes[0], 6, sync_fun=lambda: self.sync_blocks())
        rpc_proposal_hash = self.nodes[0].gobject("submit", "0", proposal_rev, proposal_time, proposal_hex, collateral)
        # Validate hashgovernanceobject
        zmq_governance_object_hash = self.subscribers[ZMQPublisher.hash_governance_object].receive().read(32).hex()
        assert_equal(zmq_governance_object_hash, rpc_proposal_hash)
        # Validate rawgovernanceobject
        zmq_governance_object_raw = CGovernanceObject()
        zmq_governance_object_raw.deserialize(self.subscribers[ZMQPublisher.raw_governance_object].receive())
        assert_equal(zmq_governance_object_raw.nHashParent, 0)
        assert_equal(zmq_governance_object_raw.nRevision, proposal_rev)
        assert_equal(zmq_governance_object_raw.nTime, proposal_time)
        assert_equal(json.loads(zmq_governance_object_raw.vchData.decode()), proposal_data)
        assert_equal(zmq_governance_object_raw.nObjectType, proposal_data["type"])
        assert_equal(zmq_governance_object_raw.masternodeOutpoint.hash, COutPoint().hash)
        assert_equal(zmq_governance_object_raw.masternodeOutpoint.n, COutPoint().n)
        # Vote for the proposal and validate the governance vote message
        map_vote_outcomes = {
            0: "none",
            1: "yes",
            2: "no",
            3: "abstain"
        }
        map_vote_signals = {
            0: "none",
            1: "funding",
            2: "valid",
            3: "delete",
            4: "endorsed"
        }
        self.nodes[0].gobject("vote-many", rpc_proposal_hash, map_vote_signals[1], map_vote_outcomes[1])
        rpc_proposal_votes = self.nodes[0].gobject('getcurrentvotes', rpc_proposal_hash)
        # Validate hashgovernancevote
        zmq_governance_vote_hash = self.subscribers[ZMQPublisher.hash_governance_vote].receive().read(32).hex()
        assert zmq_governance_vote_hash in rpc_proposal_votes
        # Validate rawgovernancevote
        zmq_governance_vote_raw = CGovernanceVote()
        zmq_governance_vote_raw.deserialize(self.subscribers[ZMQPublisher.raw_governance_vote].receive())
        assert_equal(uint256_to_string(zmq_governance_vote_raw.nParentHash), rpc_proposal_hash)
        rpc_vote_parts = rpc_proposal_votes[zmq_governance_vote_hash].split(':')
        rpc_outpoint_parts = rpc_vote_parts[0].split('-')
        assert_equal(uint256_to_string(zmq_governance_vote_raw.masternodeOutpoint.hash), rpc_outpoint_parts[0])
        assert_equal(zmq_governance_vote_raw.masternodeOutpoint.n, int(rpc_outpoint_parts[1]))
        assert_equal(zmq_governance_vote_raw.nTime, int(rpc_vote_parts[1]))
        assert_equal(map_vote_outcomes[zmq_governance_vote_raw.nVoteOutcome], rpc_vote_parts[2])
        assert_equal(map_vote_signals[zmq_governance_vote_raw.nVoteSignal], rpc_vote_parts[3])
        # Unsubscribe from governance messages
        self.unsubscribe(governance_publishers)

    def test_getzmqnotifications(self):
        # Test getzmqnotifications RPC
        assert_equal(self.nodes[0].getzmqnotifications(), [
            {"type": "pubhashchainlock", "address": self.address, "hwm": 1000},
            {"type": "pubhashgovernanceobject", "address": self.address, "hwm": 1000},
            {"type": "pubhashgovernancevote", "address": self.address, "hwm": 1000},
            {"type": "pubhashinstantsenddoublespend", "address": self.address, "hwm": 1000},
            {"type": "pubhashrecoveredsig", "address": self.address, "hwm": 1000},
            {"type": "pubhashtxlock", "address": self.address, "hwm": 1000},
            {"type": "pubrawchainlock", "address": self.address, "hwm": 1000},
            {"type": "pubrawchainlocksig", "address": self.address, "hwm": 1000},
            {"type": "pubrawgovernanceobject", "address": self.address, "hwm": 1000},
            {"type": "pubrawgovernancevote", "address": self.address, "hwm": 1000},
            {"type": "pubrawinstantsenddoublespend", "address": self.address, "hwm": 1000},
            {"type": "pubrawrecoveredsig", "address": self.address, "hwm": 1000},
            {"type": "pubrawtxlock", "address": self.address, "hwm": 1000},
            {"type": "pubrawtxlocksig", "address": self.address, "hwm": 1000},
        ])

if __name__ == '__main__':
    DashZMQTest().main()
