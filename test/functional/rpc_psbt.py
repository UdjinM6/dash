#!/usr/bin/env python3
# Copyright (c) 2018-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the Partially Signed Transaction RPCs.
"""

from decimal import Decimal
from itertools import product

from test_framework.descriptors import descsum_create
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_approx,
    assert_equal,
    assert_raises_rpc_error,
    find_output
)
from test_framework.wallet_util import bytes_to_wif

import json
import os

# Create one-input, one-output, no-fee transaction:
class PSBTTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 3
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        # Create and fund a raw tx for sending 10 DASH
        psbtx1 = self.nodes[0].walletcreatefundedpsbt([], {self.nodes[2].getnewaddress():10})['psbt']

        # If inputs are specified, do not automatically add more:
        utxo1 = self.nodes[0].listunspent()[0]
        assert_raises_rpc_error(-4, "Insufficient funds", self.nodes[0].walletcreatefundedpsbt, [{"txid": utxo1['txid'], "vout": utxo1['vout']}], {self.nodes[2].getnewaddress():900})

        psbtx1 = self.nodes[0].walletcreatefundedpsbt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], {self.nodes[2].getnewaddress():900}, 0, {"add_inputs": True})['psbt']
        assert_equal(len(self.nodes[0].decodepsbt(psbtx1)['tx']['vin']), 2)

        # Inputs argument can be null
        self.nodes[0].walletcreatefundedpsbt(None, {self.nodes[2].getnewaddress():10})

        # Node 1 should not be able to add anything to it but still return the psbtx same as before
        psbtx = self.nodes[1].walletprocesspsbt(psbtx1)['psbt']
        assert_equal(psbtx1, psbtx)

        # Node 0 should not be able to sign the transaction with the wallet is locked
        self.nodes[0].encryptwallet("password")
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase with walletpassphrase first", self.nodes[0].walletprocesspsbt, psbtx)

        # Node 0 should be able to process without signing though
        unsigned_tx = self.nodes[0].walletprocesspsbt(psbtx, False)
        assert_equal(unsigned_tx['complete'], False)

        self.nodes[0].walletpassphrase(passphrase="password", timeout=1000000)

        # Sign the transaction and send
        signed_tx = self.nodes[0].walletprocesspsbt(psbt=psbtx, finalize=False)['psbt']
        finalized_tx = self.nodes[0].walletprocesspsbt(psbt=psbtx, finalize=True)['psbt']
        assert signed_tx != finalized_tx
        final_tx = self.nodes[0].finalizepsbt(signed_tx)['hex']
        self.nodes[0].sendrawtransaction(final_tx)

        # Manually selected inputs can be locked:
        assert_equal(len(self.nodes[0].listlockunspent()), 0)
        utxo1 = self.nodes[0].listunspent()[0]
        psbtx1 = self.nodes[0].walletcreatefundedpsbt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], {self.nodes[2].getnewaddress():1}, 0,{"lockUnspents": True})["psbt"]
        assert_equal(len(self.nodes[0].listlockunspent()), 1)

        # Locks are ignored for manually selected inputs
        self.nodes[0].walletcreatefundedpsbt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], {self.nodes[2].getnewaddress():1}, 0)

        # Get pubkeys
        pubkey0 = self.nodes[0].getaddressinfo(self.nodes[0].getnewaddress())['pubkey']
        pubkey1 = self.nodes[1].getaddressinfo(self.nodes[1].getnewaddress())['pubkey']
        pubkey2 = self.nodes[2].getaddressinfo(self.nodes[2].getnewaddress())['pubkey']

        # Setup watchonly wallets
        self.nodes[2].createwallet(wallet_name='wmulti', disable_private_keys=True)
        wmulti = self.nodes[2].get_wallet_rpc('wmulti')

        # Create all the addresses
        p2sh = wmulti.addmultisigaddress(2, [pubkey0, pubkey1, pubkey2])['address']

        if not self.options.descriptors:
            wmulti.importaddress(p2sh)

        p2pkh = self.nodes[1].getnewaddress()

        # fund those addresses
        rawtx = self.nodes[0].createrawtransaction([], {p2sh:10, p2pkh:10})
        rawtx = self.nodes[0].fundrawtransaction(rawtx, {"changePosition":1})
        signed_tx = self.nodes[0].signrawtransactionwithwallet(rawtx['hex'])['hex']
        txid = self.nodes[0].sendrawtransaction(signed_tx)
        self.generate(self.nodes[0], 6)

        # Find the output pos
        p2sh_pos = -1
        p2pkh_pos = -1
        decoded = self.nodes[0].decoderawtransaction(signed_tx)
        for out in decoded['vout']:
            if out['scriptPubKey']['address'] == p2sh:
                p2sh_pos = out['n']
            elif out['scriptPubKey']['address'] == p2pkh:
                p2pkh_pos = out['n']

        inputs = [{"txid":txid,"vout":p2pkh_pos}]
        outputs = [{self.nodes[1].getnewaddress(): 9.99}]

        # spend single key from node 1
        created_psbt = self.nodes[1].walletcreatefundedpsbt(inputs, outputs)
        walletprocesspsbt_out = self.nodes[1].walletprocesspsbt(created_psbt['psbt'])
        # Make sure it has both types of UTXOs
        decoded = self.nodes[1].decodepsbt(walletprocesspsbt_out['psbt'])
        assert 'non_witness_utxo' in decoded['inputs'][0]
        # Check decodepsbt fee calculation (input values shall only be counted once per UTXO)
        assert_equal(decoded['fee'], created_psbt['fee'])
        assert_equal(walletprocesspsbt_out['complete'], True)
        self.nodes[1].sendrawtransaction(self.nodes[1].finalizepsbt(walletprocesspsbt_out['psbt'])['hex'])

        self.log.info("Test walletcreatefundedpsbt fee rate of 10000 sat/vB and 0.1 BTC/kvB produces a total fee at or slightly below -maxtxfee (~0.05290000)")
        res1 = self.nodes[1].walletcreatefundedpsbt(inputs, outputs, 0, {"fee_rate": 10000, "add_inputs": True})
        assert_approx(res1["fee"], 0.04, 0.005)
        res2 = self.nodes[1].walletcreatefundedpsbt(inputs, outputs, 0, {"feeRate": "0.1", "add_inputs": True})
        assert_approx(res2["fee"], 0.04, 0.005)

        self.log.info("Test min fee rate checks with walletcreatefundedpsbt are bypassed, e.g. a fee_rate under 1 sat/vB is allowed")
        res3 = self.nodes[1].walletcreatefundedpsbt(inputs, outputs, 0, {"fee_rate": "0.999", "add_inputs": True})
        assert_approx(res3["fee"], 0.00000224, 0.0000001)
        res4 = self.nodes[1].walletcreatefundedpsbt(inputs, outputs, 0, {"feeRate": 0.00000999, "add_inputs": True})
        assert_approx(res4["fee"], 0.00000224, 0.0000001)

        self.log.info("Test min fee rate checks with walletcreatefundedpsbt are bypassed and that funding non-standard 'zero-fee' transactions is valid")
        for param, zero_value in product(["fee_rate", "feeRate"], [0, 0.000, 0.00000000, "0", "0.000", "0.00000000"]):
            assert_equal(0, self.nodes[1].walletcreatefundedpsbt(inputs, outputs, 0, {param: zero_value, "add_inputs": True})["fee"])

        self.log.info("Test invalid fee rate settings")
        for param, value in {("fee_rate", 100000), ("feeRate", 1)}:
            assert_raises_rpc_error(-4, "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)",
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {param: value, "add_inputs": True})
            assert_raises_rpc_error(-3, "Amount out of range",
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {param: -1, "add_inputs": True})
            assert_raises_rpc_error(-3, "Amount is not a number or string",
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {param: {"foo": "bar"}, "add_inputs": True})
            # Test fee rate values that don't pass fixed-point parsing checks.
            for invalid_value in ["", 0.000000001, 1e-09, 1.111111111, 1111111111111111, "31.999999999999999999999"]:
                assert_raises_rpc_error(-3, "Invalid amount",
                    self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {param: invalid_value, "add_inputs": True})
        # Test fee_rate values that cannot be represented in sat/vB.
        for invalid_value in [0.0001, 0.00000001, 0.00099999, 31.99999999, "0.0001", "0.00000001", "0.00099999", "31.99999999"]:
            assert_raises_rpc_error(-3, "Invalid amount",
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"fee_rate": invalid_value, "add_inputs": True})

        self.log.info("- raises RPC error if both feeRate and fee_rate are passed")
        assert_raises_rpc_error(-8, "Cannot specify both fee_rate (duff/B) and feeRate (DASH/kB)",
            self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"fee_rate": 0.1, "feeRate": 0.1, "add_inputs": True})

        self.log.info("- raises RPC error if both feeRate and estimate_mode passed")
        assert_raises_rpc_error(-8, "Cannot specify both estimate_mode and feeRate",
            self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"estimate_mode": "economical", "feeRate": 0.1, "add_inputs": True})

        for param in ["feeRate", "fee_rate"]:
            self.log.info("- raises RPC error if both {} and conf_target are passed".format(param))
            assert_raises_rpc_error(-8, "Cannot specify both conf_target and {}. Please provide either a confirmation "
                "target in blocks for automatic fee estimation, or an explicit fee rate.".format(param),
                self.nodes[1].walletcreatefundedpsbt ,inputs, outputs, 0, {param: 1, "conf_target": 1, "add_inputs": True})

        self.log.info("- raises RPC error if both fee_rate and estimate_mode are passed")
        assert_raises_rpc_error(-8, "Cannot specify both estimate_mode and fee_rate",
            self.nodes[1].walletcreatefundedpsbt ,inputs, outputs, 0, {"fee_rate": 1, "estimate_mode": "economical", "add_inputs": True})

        self.log.info("- raises RPC error with invalid estimate_mode settings")
        for k, v in {"number": 42, "object": {"foo": "bar"}}.items():
            assert_raises_rpc_error(-3, "Expected type string for estimate_mode, got {}".format(k),
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"estimate_mode": v, "conf_target": 0.1, "add_inputs": True})
        for mode in ["", "foo", Decimal("3.141592")]:
            assert_raises_rpc_error(-8, 'Invalid estimate_mode parameter, must be one of: "unset", "economical", "conservative"',
                self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"estimate_mode": mode, "conf_target": 0.1, "add_inputs": True})

        self.log.info("- raises RPC error with invalid conf_target settings")
        for mode in ["unset", "economical", "conservative"]:
            self.log.debug("{}".format(mode))
            for k, v in {"string": "", "object": {"foo": "bar"}}.items():
                assert_raises_rpc_error(-3, "Expected type number for conf_target, got {}".format(k),
                    self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"estimate_mode": mode, "conf_target": v, "add_inputs": True})
            for n in [-1, 0, 1009]:
                assert_raises_rpc_error(-8, "Invalid conf_target, must be between 1 and 1008",  # max value of 1008 per src/policy/fees.h
                    self.nodes[1].walletcreatefundedpsbt, inputs, outputs, 0, {"estimate_mode": mode, "conf_target": n, "add_inputs": True})

        self.log.info("Test walletcreatefundedpsbt with too-high fee rate produces total fee well above -maxtxfee and raises RPC error")
        # previously this was silently capped at -maxtxfee
        for bool_add, outputs_array in {True: outputs, False: [{self.nodes[1].getnewaddress(): 1}]}.items():
            msg = "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)"
            assert_raises_rpc_error(-4, msg, self.nodes[1].walletcreatefundedpsbt, inputs, outputs_array, 0, {"fee_rate": 1000000, "add_inputs": bool_add})
            assert_raises_rpc_error(-4, msg, self.nodes[1].walletcreatefundedpsbt, inputs, outputs_array, 0, {"feeRate": 1, "add_inputs": bool_add})

        self.log.info("Test various PSBT operations")
        # partially sign multisig things with node 1
        psbtx = wmulti.walletcreatefundedpsbt(inputs=[{"txid":txid,"vout":p2sh_pos}], outputs={self.nodes[1].getnewaddress():9.99}, options={'changeAddress': self.nodes[1].getrawchangeaddress()})['psbt']
        walletprocesspsbt_out = self.nodes[1].walletprocesspsbt(psbtx)
        psbtx = walletprocesspsbt_out['psbt']
        assert_equal(walletprocesspsbt_out['complete'], False)

        # Unload wmulti, we don't need it anymore
        wmulti.unloadwallet()

        # partially sign with node 2. This should be complete and sendable
        walletprocesspsbt_out = self.nodes[2].walletprocesspsbt(psbtx)
        assert_equal(walletprocesspsbt_out['complete'], True)
        self.nodes[2].sendrawtransaction(self.nodes[2].finalizepsbt(walletprocesspsbt_out['psbt'])['hex'])

        # check that walletprocesspsbt fails to decode a non-psbt
        rawtx = self.nodes[1].createrawtransaction([{"txid":txid,"vout":p2pkh_pos}], {self.nodes[1].getnewaddress():9.99})
        assert_raises_rpc_error(-22, "TX decode failed", self.nodes[1].walletprocesspsbt, rawtx)

        # Convert a non-psbt to psbt and make sure we can decode it
        rawtx = self.nodes[0].createrawtransaction([], {self.nodes[1].getnewaddress():10})
        rawtx = self.nodes[0].fundrawtransaction(rawtx)
        new_psbt = self.nodes[0].converttopsbt(rawtx['hex'])
        self.nodes[0].decodepsbt(new_psbt)

        # Make sure that a psbt with signatures cannot be converted
        signedtx = self.nodes[0].signrawtransactionwithwallet(rawtx['hex'])
        assert_raises_rpc_error(-22, "Inputs must not have scriptSigs", self.nodes[0].converttopsbt, hexstring=signedtx['hex'])
        assert_raises_rpc_error(-22, "Inputs must not have scriptSigs", self.nodes[0].converttopsbt, hexstring=signedtx['hex'], permitsigdata=False)
        # Unless we allow it to convert and strip signatures
        self.nodes[0].converttopsbt(signedtx['hex'], True)

        # Explicitly allow converting non-empty txs
        new_psbt = self.nodes[0].converttopsbt(rawtx['hex'])
        self.nodes[0].decodepsbt(new_psbt)

        # Create outputs to nodes 1 and 2
        node1_addr = self.nodes[1].getnewaddress()
        node2_addr = self.nodes[2].getnewaddress()
        txid1 = self.nodes[0].sendtoaddress(node1_addr, 13)
        txid2 = self.nodes[0].sendtoaddress(node2_addr, 13)
        blockhash = self.generate(self.nodes[0], 6)[0]
        vout1 = find_output(self.nodes[1], txid1, 13, blockhash=blockhash)
        vout2 = find_output(self.nodes[2], txid2, 13, blockhash=blockhash)

        # Create a psbt spending outputs from nodes 1 and 2
        psbt_orig = self.nodes[0].createpsbt([{"txid":txid1,  "vout":vout1}, {"txid":txid2, "vout":vout2}], {self.nodes[0].getnewaddress():25.999})

        # Update psbts, should only have data for one input and not the other
        psbt1 = self.nodes[1].walletprocesspsbt(psbt_orig, False, "ALL")['psbt']
        psbt1_decoded = self.nodes[0].decodepsbt(psbt1)
        assert psbt1_decoded['inputs'][0] and not psbt1_decoded['inputs'][1]
        # Check that BIP32 path was added
        assert "bip32_derivs" in psbt1_decoded['inputs'][0]
        psbt2 = self.nodes[2].walletprocesspsbt(psbt_orig, False, "ALL", False)['psbt']
        psbt2_decoded = self.nodes[0].decodepsbt(psbt2)
        assert not psbt2_decoded['inputs'][0] and psbt2_decoded['inputs'][1]
        # Check that BIP32 paths were not added
        assert "bip32_derivs" not in psbt2_decoded['inputs'][1]

        # Sign PSBTs (workaround issue #18039)
        psbt1 = self.nodes[1].walletprocesspsbt(psbt_orig)['psbt']
        psbt2 = self.nodes[2].walletprocesspsbt(psbt_orig)['psbt']

        # Combine, finalize, and send the psbts
        combined = self.nodes[0].combinepsbt([psbt1, psbt2])
        finalized = self.nodes[0].finalizepsbt(combined)['hex']
        self.nodes[0].sendrawtransaction(finalized)
        self.generate(self.nodes[0], 6)

        # Make sure change address wallet does not have P2SH innerscript access to results in success
        # when attempting BnB coin selection
        block_height = self.nodes[0].getblockcount()
        self.nodes[0].walletcreatefundedpsbt([], [{self.nodes[2].getnewaddress():self.nodes[0].listunspent()[0]["amount"]+1}], block_height+2, {"changeAddress":self.nodes[1].getnewaddress()}, False)

        # Regression test for 14473 (mishandling of already-signed witness transaction):
        unspent = self.nodes[0].listunspent()[0]
        psbtx_info = self.nodes[0].walletcreatefundedpsbt([{"txid":unspent["txid"], "vout":unspent["vout"]}], [{self.nodes[2].getnewaddress():unspent["amount"]+1}], 0, {"add_inputs": True})
        complete_psbt = self.nodes[0].walletprocesspsbt(psbtx_info["psbt"])
        double_processed_psbt = self.nodes[0].walletprocesspsbt(complete_psbt["psbt"])
        assert_equal(complete_psbt, double_processed_psbt)
        # We don't care about the decode result, but decoding must succeed.
        self.nodes[0].decodepsbt(double_processed_psbt["psbt"])

        # Make sure unsafe inputs are included if specified
        self.nodes[2].createwallet(wallet_name="unsafe")
        wunsafe = self.nodes[2].get_wallet_rpc("unsafe")
        self.nodes[0].sendtoaddress(wunsafe.getnewaddress(), 2)
        self.sync_mempools()
        assert_raises_rpc_error(-4, "Insufficient funds", wunsafe.walletcreatefundedpsbt, [], [{self.nodes[0].getnewaddress(): 1}])
        wunsafe.walletcreatefundedpsbt([], [{self.nodes[0].getnewaddress(): 1}], 0, {"include_unsafe": True})

        # BIP 174 Test Vectors

        # Check that unknown values are just passed through
        unknown_psbt = "cHNidP8BAD8CAAAAAf//////////////////////////////////////////AAAAAAD/////AQAAAAAAAAAAA2oBAAAAAAAACg8BAgMEBQYHCAkPAQIDBAUGBwgJCgsMDQ4PAAA="
        unknown_out = self.nodes[0].walletprocesspsbt(unknown_psbt)['psbt']
        assert_equal(unknown_psbt, unknown_out)

        # Open the data file
        with open(os.path.join(os.path.dirname(os.path.realpath(__file__)), 'data/rpc_psbt.json'), encoding='utf-8') as f:
            d = json.load(f)
            invalids = d['invalid']
            valids = d['valid']
            creators = d['creator']
            signers = d['signer']
            combiners = d['combiner']
            finalizers = d['finalizer']
            extractors = d['extractor']

        # Invalid PSBTs
        for invalid in invalids:
            assert_raises_rpc_error(-22, "TX decode failed", self.nodes[0].decodepsbt, invalid)

        # Valid PSBTs
        for valid in valids:
            self.nodes[0].decodepsbt(valid)

        # Creator Tests
        for creator in creators:
            created_tx = self.nodes[0].createpsbt(creator['inputs'], creator['outputs'])
            assert_equal(created_tx, creator['result'])

        # Signer tests
        for i, signer in enumerate(signers):
            self.nodes[2].createwallet(wallet_name="wallet{}".format(i))
            wrpc = self.nodes[2].get_wallet_rpc("wallet{}".format(i))
            for key in signer['privkeys']:
                wrpc.importprivkey(key)
            signed_tx = wrpc.walletprocesspsbt(signer['psbt'])['psbt']
            assert_equal(signed_tx, signer['result'])

        # Combiner test
        for combiner in combiners:
            combined = self.nodes[2].combinepsbt(combiner['combine'])
            assert_equal(combined, combiner['result'])

        # Empty combiner test
        assert_raises_rpc_error(-8, "Parameter 'txs' cannot be empty", self.nodes[0].combinepsbt, [])

        # Finalizer test
        for finalizer in finalizers:
            finalized = self.nodes[2].finalizepsbt(finalizer['finalize'], False)['psbt']
            assert_equal(finalized, finalizer['result'])

        # Extractor test
        for extractor in extractors:
            extracted = self.nodes[2].finalizepsbt(extractor['extract'], True)['hex']
            assert_equal(extracted, extractor['result'])

        # Test that psbts with p2pkh outputs are created properly
        p2pkh = self.nodes[0].getnewaddress()
        psbt = self.nodes[1].walletcreatefundedpsbt([], [{p2pkh : 1}], 0, {"includeWatching" : True}, True)
        decoded_psbt = self.nodes[0].decodepsbt(psbtx)
        for psbt_in in decoded_psbt["inputs"]:
            assert "bip32_derivs" in psbt_in

        # Test decoding error: invalid base64
        assert_raises_rpc_error(-22, "TX decode failed invalid base64", self.nodes[0].decodepsbt, ";definitely not base64;")

        # Send to all types of addresses
        addr1 = self.nodes[1].getnewaddress()
        txid1 = self.nodes[0].sendtoaddress(addr1, 11)
        vout1 = find_output(self.nodes[0], txid1, 11)
        addr2 = self.nodes[1].getnewaddress()
        txid2 = self.nodes[0].sendtoaddress(addr2, 11)
        vout2 = find_output(self.nodes[0], txid2, 11)
        addr3 = self.nodes[1].getnewaddress()
        txid3 = self.nodes[0].sendtoaddress(addr3, 11)
        vout3 = find_output(self.nodes[0], txid3, 11)
        self.sync_all()

        def test_psbt_input_keys(psbt_input, keys):
            """Check that the psbt input has only the expected keys."""
            assert_equal(set(keys), set(psbt_input.keys()))

        # Create a PSBT. None of the inputs are filled initially
        psbt = self.nodes[1].createpsbt([{"txid":txid1, "vout":vout1},{"txid":txid2, "vout":vout2},{"txid":txid3, "vout":vout3}], {self.nodes[0].getnewaddress():32.999})
        decoded = self.nodes[1].decodepsbt(psbt)
        test_psbt_input_keys(decoded['inputs'][0], [])
        test_psbt_input_keys(decoded['inputs'][1], [])
        test_psbt_input_keys(decoded['inputs'][2], [])

        # Update a PSBT with UTXOs from the node
        # No inputs should be filled because they are non-witness
        updated = self.nodes[1].utxoupdatepsbt(psbt)
        decoded = self.nodes[1].decodepsbt(updated)
        test_psbt_input_keys(decoded['inputs'][1], [])
        test_psbt_input_keys(decoded['inputs'][2], [])

        # Try again, now while providing descriptors
        descs = [self.nodes[1].getaddressinfo(addr)['desc'] for addr in [addr1,addr2,addr3]]
        updated = self.nodes[1].utxoupdatepsbt(psbt=psbt, descriptors=descs)
        decoded = self.nodes[1].decodepsbt(updated)
        test_psbt_input_keys(decoded['inputs'][0], [])
        test_psbt_input_keys(decoded['inputs'][1], [])
        test_psbt_input_keys(decoded['inputs'][2], [])

        # Two PSBTs with a common input should not be joinable
        psbt1 = self.nodes[1].createpsbt([{"txid":txid1, "vout":vout1}], {self.nodes[0].getnewaddress():Decimal('10.999')})
        assert_raises_rpc_error(-8, "exists in multiple PSBTs", self.nodes[1].joinpsbts, [psbt1, updated])

        # Join two distinct PSBTs
        addr4 = self.nodes[1].getnewaddress()
        txid4 = self.nodes[0].sendtoaddress(addr4, 5)
        vout4 = find_output(self.nodes[0], txid4, 5)
        self.generate(self.nodes[0], 6)
        psbt2 = self.nodes[1].createpsbt([{"txid":txid4, "vout":vout4}], {self.nodes[0].getnewaddress():Decimal('4.999')})
        psbt2 = self.nodes[1].walletprocesspsbt(psbt2)['psbt']
        psbt2_decoded = self.nodes[0].decodepsbt(psbt2)
        assert "final_scriptwitness" not in psbt2_decoded['inputs'][0] and "final_scriptSig" in psbt2_decoded['inputs'][0]
        joined = self.nodes[0].joinpsbts([psbt, psbt2])
        joined_decoded = self.nodes[0].decodepsbt(joined)
        assert len(joined_decoded['inputs']) == 4 and len(joined_decoded['outputs']) == 2 and "final_scriptwitness" not in joined_decoded['inputs'][3] and "final_scriptSig" not in joined_decoded['inputs'][3]

        # Newly created PSBT needs UTXOs and updating
        addr = self.nodes[1].getnewaddress()
        txid = self.nodes[0].sendtoaddress(addr, 7)
        self.generate(self.nodes[0], 6)
        vout = find_output(self.nodes[0], txid, 7)
        psbt = self.nodes[1].createpsbt([{"txid":txid, "vout":vout}], {self.nodes[0].getnewaddress():Decimal('6.999')})
        analyzed = self.nodes[0].analyzepsbt(psbt)
        assert not analyzed['inputs'][0]['has_utxo'] and not analyzed['inputs'][0]['is_final'] and analyzed['inputs'][0]['next'] == 'updater' and analyzed['next'] == 'updater'

        # After update with wallet, only needs signing
        updated = self.nodes[1].walletprocesspsbt(psbt, False, 'ALL', True)['psbt']
        analyzed = self.nodes[0].analyzepsbt(updated)
        assert analyzed['inputs'][0]['has_utxo'] and not analyzed['inputs'][0]['is_final'] and analyzed['inputs'][0]['next'] == 'signer' and analyzed['next'] == 'signer'

        # Check fee and size things
        assert analyzed['fee'] == Decimal('0.00100000') and analyzed['estimated_vsize'] == 191 and analyzed['estimated_feerate'] == Decimal('0.00523560')

        # After signing and finalizing, needs extracting
        signed = self.nodes[1].walletprocesspsbt(updated)['psbt']
        analyzed = self.nodes[0].analyzepsbt(signed)
        assert analyzed['inputs'][0]['has_utxo'] and analyzed['inputs'][0]['is_final'] and analyzed['next'] == 'extractor'

        self.log.info("PSBT spending unspendable outputs should have error message and Creator as next")
        analysis = self.nodes[0].analyzepsbt('cHNidP8BAHECAAAAAQLlOzD0IqyxBs+IP6PTQ4NpMkMfJwUtvZ2MEr9+u0PtAAAAAAD/////AgD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XL87QKVAAAAABYAFPck4gF7iL4NL4wtfRAKgQbghiTUAAAAAAABABUCAAAAAAGghgEAAAAAAAJqAAAAAAABAR8AgIFq49AHABYAFJUDtxf2PHo641HEOBOAIvFMNTr2AAAA')
        assert_equal(analysis['next'], 'creator')
        assert_equal(analysis['error'], 'PSBT is not valid. Input 0 spends unspendable output')

        self.log.info("PSBT with invalid values should have error message and Creator as next")
        analysis = self.nodes[0].analyzepsbt('cHNidP8BAHECAAAAAQXbPuwX0yi0oRvSfIXqeruM5+ibuXvP92RCdIEsJR6jAAAAAAD/////AgD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XL87QKVAAAAABYAFPck4gF7iL4NL4wtfRAKgQbghiTUAAAAAAABABMCAAAAAAEBQAda8HUHAAAAAAAAAQcCagABAR8AgIFq49AHABYAFJUDtxf2PHo641HEOBOAIvFMNTr2AAAA')
        assert_equal(analysis['next'], 'creator')
        assert_equal(analysis['error'], 'PSBT is not valid. Input 0 has invalid value')

        self.log.info("PSBT with signed, but not finalized, inputs should have Finalizer as next")
        analysis = self.nodes[0].analyzepsbt('cHNidP8BAHECAAAAAXfx5nfVEBDq0BbpUjqqfvNEWyX2dGCkx7RiW9E6DXPbAAAAAAD9////AlDDAAAAAAAAFgAUy/UxxZuzZswcmFnN/E9DGSiHLUsuGPUFAAAAABYAFLsH5o0R38wXx+X2cCosTMCZnQ4baAAAAAABACwCAAAAAAEA4fUFAAAAABl2qRTgSNoebYX9/i35W9ixgrFUmcLJbYisAAAAACICAv4uHbVQEQkVrUThtBx6BnGlZJcyVHMKOYaJoBpEjQJyRzBEAiBse8ynDCBq+3BBEvQUeCnbf6cz0yt3ylnBsgiV2+zFZwIgCzgYKpTB+BmwtRw4fx5spZjueH8hMuJSnXu/wU/z1scBAQMEAQAAACIGAv4uHbVQEQkVrUThtBx6BnGlZJcyVHMKOYaJoBpEjQJyGA8FaUNUAACAAQAAgAAAAIAAAAAAAAAAAAEBHwDh9QUAAAAAFgAU4EjaHm2F/f4t+VvYsYKxVJnCyW0AACICAv4IiIHn01IKyw4lEaIxhArVyFqGABpomkhcTjULKKv7GA8FaUNUAACAAQAAgAAAAIABAAAAAAAAAAA=')
        # TODO update tx above.
        # currently I can't figure out how to generate transaction, that would be valid and will show correct stage finalizer.
        # currently it fails at `verify pubkey` for signer
        # assert_equal(analysis['next'], 'finalizer')

        analysis = self.nodes[0].analyzepsbt('cHNidP8BAHECAAAAAbmNr2X0Nc+2AtaXYjPu3kIFc3Cmj1aYy5WQs5yaUfRvAAAAAAD/////AgCAgWrj0AcAFgAUKNw0x8HRctAgmvoevm4u1SbN7XL87QKVAAAAABYAFPck4gF7iL4NL4wtfRAKgQbghiTUAAAAAAABABQCAAAAAAGghgEAAAAAAAFRAAAAAAEBHwDyBSoBAAAAFgAUlQO3F/Y8ejrjUcQ4E4Ai8Uw1OvYAAAA=')
        assert_equal(analysis['next'], 'creator')
        assert_equal(analysis['error'], 'PSBT is not valid. Output amount invalid')

        analysis = self.nodes[0].analyzepsbt('cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA==')
        assert_equal(analysis['next'], 'creator')
        assert_equal(analysis['error'], 'PSBT is not valid. Input 0 specifies invalid prevout')

        assert_raises_rpc_error(-25, 'Inputs missing or spent', self.nodes[0].walletprocesspsbt, 'cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA==')

        # Test that we can fund psbts with external inputs specified
        eckey = ECKey()
        eckey.generate()
        privkey = bytes_to_wif(eckey.get_bytes())

        # Make a weird but signable script. sh(pkh()) descriptor accomplishes this
        desc = descsum_create("sh(pkh({}))".format(privkey))
        if self.options.descriptors:
            res = self.nodes[0].importdescriptors([{"desc": desc, "timestamp": "now"}])
        else:
            res = self.nodes[0].importmulti([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        addr_info = self.nodes[0].getaddressinfo(addr)

        self.nodes[0].sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 6)
        ext_utxo = self.nodes[0].listunspent(addresses=[addr])[0]

        # An external input without solving data should result in an error
        assert_raises_rpc_error(-4, "Insufficient funds", self.nodes[1].walletcreatefundedpsbt, [ext_utxo], {self.nodes[0].getnewaddress(): 10 + ext_utxo['amount']}, 0, {'add_inputs': True})

        # But funding should work when the solving data is provided
        psbt = self.nodes[1].walletcreatefundedpsbt([ext_utxo], {self.nodes[0].getnewaddress(): 15}, 0, {'add_inputs': True, "solving_data": {"pubkeys": [addr_info['pubkey']], "scripts": [addr_info["embedded"]["scriptPubKey"]]}})
        signed = self.nodes[1].walletprocesspsbt(psbt['psbt'])
        assert not signed['complete']
        signed = self.nodes[0].walletprocesspsbt(signed['psbt'])
        assert signed['complete']
        self.nodes[0].finalizepsbt(signed['psbt'])

        psbt = self.nodes[1].walletcreatefundedpsbt([ext_utxo], {self.nodes[0].getnewaddress(): 15}, 0, {'add_inputs': True, "solving_data":{"descriptors": [desc]}})
        signed = self.nodes[1].walletprocesspsbt(psbt['psbt'])
        assert not signed['complete']
        signed = self.nodes[0].walletprocesspsbt(signed['psbt'])
        assert signed['complete']
        self.nodes[0].finalizepsbt(signed['psbt'])

if __name__ == '__main__':
    PSBTTest().main()
