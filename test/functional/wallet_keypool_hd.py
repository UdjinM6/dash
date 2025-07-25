#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Exercise the wallet keypool, and interaction with wallet encryption/locking

# Add python-bitcoinrpc to module search path:

import time
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

class KeyPoolTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        nodes = self.nodes
        addr_before_encrypting = nodes[0].getnewaddress()
        addr_before_encrypting_data = nodes[0].getaddressinfo(addr_before_encrypting)
        wallet_info_old = nodes[0].getwalletinfo()
        if not self.options.descriptors:
            assert addr_before_encrypting_data['hdchainid'] == wallet_info_old['hdchainid']

        # Encrypt wallet and wait to terminate
        nodes[0].encryptwallet('test')
        if self.options.descriptors:
            # Import hardened derivation only descriptors
            nodes[0].walletpassphrase('test', 10)
            nodes[0].importdescriptors([
                {
                    "desc": "pkh(tprv8ZgxMBicQKsPd7Uf69XL1XwhmjHopUGep8GuEiJDZmbQz6o58LninorQAfcKZWARbtRtfnLcJ5MQ2AtHcQJCCRUcMRvmDUjyEmNUWwx8UbK/1h/*h)#a0nyvl0k",
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True
                },
                {
                    "desc": "sh(pkh(tprv8ZgxMBicQKsPd7Uf69XL1XwhmjHopUGep8GuEiJDZmbQz6o58LninorQAfcKZWARbtRtfnLcJ5MQ2AtHcQJCCRUcMRvmDUjyEmNUWwx8UbK/2h/*h))#lmeu2axg",
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True
                },
                {
                    "desc": "pkh(tprv8ZgxMBicQKsPd7Uf69XL1XwhmjHopUGep8GuEiJDZmbQz6o58LninorQAfcKZWARbtRtfnLcJ5MQ2AtHcQJCCRUcMRvmDUjyEmNUWwx8UbK/4h/*h)#l3crwaus",
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True,
                    "internal": True
                },
                {
                    "desc": "sh(pkh(tprv8ZgxMBicQKsPd7Uf69XL1XwhmjHopUGep8GuEiJDZmbQz6o58LninorQAfcKZWARbtRtfnLcJ5MQ2AtHcQJCCRUcMRvmDUjyEmNUWwx8UbK/5h/*h))#qg8wa75f",
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True,
                    "internal": True
                }
            ])
            nodes[0].walletlock()
        # Keep creating keys
        addr = nodes[0].getnewaddress()
        addr_data = nodes[0].getaddressinfo(addr)
        wallet_info = nodes[0].getwalletinfo()

        # we don't have feature of replacing seed due to bitcoin#17681 is DNM, no need to check if masterfingerprint is changed
        # assert addr_before_encrypting_data['hdmasterfingerprint'] != addr_data['hdmasterfingerprint']
        if not self.options.descriptors:
            assert addr_data['hdchainid'] == wallet_info['hdchainid']

        try:
            addr = nodes[0].getnewaddress()
            raise AssertionError('Keypool should be exhausted after one address')
        except JSONRPCException as e:
            assert e.error['code']==-12

        # put six (plus 2) new keys in the keypool (100% external-, +100% internal-keys, 1 in min)
        nodes[0].walletpassphrase('test', 12000)
        nodes[0].keypoolrefill(6)
        nodes[0].walletlock()
        wi = nodes[0].getwalletinfo()
        if self.options.descriptors:
            # this counters are zero, bitcoin have here 6 * 3 (3 different types)
            assert_equal(wi['keypoolsize_hd_internal'], 6)
            assert_equal(wi['keypoolsize'], 6)
        else:
            assert_equal(wi['keypoolsize_hd_internal'], 6)
            assert_equal(wi['keypoolsize'], 6)

        # drain the internal keys
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        # remember keypool sizes
        wi = nodes[0].getwalletinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        try:
            nodes[0].getrawchangeaddress()
            raise AssertionError('Keypool should be exhausted after six addresses')
        except JSONRPCException as e:
            assert e.error['code']==-12
        # check that keypool sizes did not change
        wi = nodes[0].getwalletinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        addr = set()
        # drain the external keys
        addr.add(nodes[0].getnewaddress())
        addr.add(nodes[0].getnewaddress())
        addr.add(nodes[0].getnewaddress())
        addr.add(nodes[0].getnewaddress())
        addr.add(nodes[0].getnewaddress())
        addr.add(nodes[0].getnewaddress())
        assert len(addr) == 6
        # remember keypool sizes
        wi = nodes[0].getwalletinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        try:
            addr = nodes[0].getnewaddress()
            raise AssertionError('Keypool should be exhausted after six addresses')
        except JSONRPCException as e:
            assert e.error['code']==-12
        # check that keypool sizes did not change
        wi = nodes[0].getwalletinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        # refill keypool with three new addresses
        nodes[0].walletpassphrase('test', 1)
        nodes[0].keypoolrefill(3)
        # test walletpassphrase timeout
        time.sleep(1.1)
        assert_equal(nodes[0].getwalletinfo()["unlocked_until"], 0)

        # drain the keypool
        for _ in range(3):
            nodes[0].getnewaddress()
        assert_raises_rpc_error(-12, "Keypool ran out", nodes[0].getnewaddress)

        nodes[0].walletpassphrase('test', 100)
        nodes[0].keypoolrefill(100)
        wi = nodes[0].getwalletinfo()
        if self.options.descriptors:
            # dash has only 1 type of output addresses
            assert_equal(wi['keypoolsize_hd_internal'], 100)
            assert_equal(wi['keypoolsize'], 100)
        else:
            assert_equal(wi['keypoolsize_hd_internal'], 100)
            assert_equal(wi['keypoolsize'], 100)

        if not self.options.descriptors:
            # Check that newkeypool entirely flushes the keypool
            start_keypath = nodes[0].getaddressinfo(nodes[0].getnewaddress())['hdkeypath']
            start_change_keypath = nodes[0].getaddressinfo(nodes[0].getrawchangeaddress())['hdkeypath']
            # flush keypool and get new addresses
            nodes[0].newkeypool()
            end_keypath = nodes[0].getaddressinfo(nodes[0].getnewaddress())['hdkeypath']
            end_change_keypath = nodes[0].getaddressinfo(nodes[0].getrawchangeaddress())['hdkeypath']
            # The new keypath index should be 100 more than the old one
            new_index = int(start_keypath.rsplit('/',  1)[1]) + 100
            new_change_index = int(start_change_keypath.rsplit('/',  1)[1]) + 100
            assert_equal(end_keypath, "m/44'/1'/0'/0/" + str(new_index))
            assert_equal(end_change_keypath, "m/44'/1'/0'/1/" + str(new_change_index))

        # create a blank wallet
        nodes[0].createwallet(wallet_name='w2', blank=True, disable_private_keys=True)
        w2 = nodes[0].get_wallet_rpc('w2')

        # refer to initial wallet as w1
        w1 = nodes[0].get_wallet_rpc(self.default_wallet_name)

        # import private key and fund it
        address = addr.pop()
        desc = w1.getaddressinfo(address)['desc']
        if self.options.descriptors:
            res = w2.importdescriptors([{'desc': desc, 'timestamp': 'now'}])
        else:
            res = w2.importmulti([{'desc': desc, 'timestamp': 'now'}])
        assert_equal(res[0]['success'], True)
        w1.walletpassphrase('test', 100)

        res = w1.sendtoaddress(address=address, amount=0.00010000)
        self.generate(nodes[0], 1)
        destination = addr.pop()

        # Using a fee rate (10 sat / byte) well above the minimum relay rate
        # creating a 5,000 sat transaction with change should not be possible
        assert_raises_rpc_error(-4, "Transaction needs a change address, but we can't generate it. Please call keypoolrefill first.", w2.walletcreatefundedpsbt, inputs=[], outputs=[{addr.pop(): 0.00005000}], options={"subtractFeeFromOutputs": [0], "feeRate": 0.000010})

        # creating a 10,000 sat transaction without change, with a manual input, should still be possible
        res = w2.walletcreatefundedpsbt(inputs=w2.listunspent(), outputs=[{destination: 0.00010000}], options={"subtractFeeFromOutputs": [0], "feeRate": 0.000010})
        assert_equal("psbt" in res, True)

        # creating a 10,000 sat transaction without change should still be possible
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], options={"subtractFeeFromOutputs": [0], "feeRate": 0.000010})
        assert_equal("psbt" in res, True)
        # should work without subtractFeeFromOutputs if the exact fee is subtracted from the amount
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00008900}], options={"feeRate": 0.000010})
        assert_equal("psbt" in res, True)

        # dust change should be removed
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00008800}], options={"feeRate": 0.000010})
        assert_equal("psbt" in res, True)

        # create a transaction without change at the maximum fee rate, such that the output is still spendable:
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], options={"subtractFeeFromOutputs": [0], "feeRate": 0.00008824})
        assert_equal("psbt" in res, True)
        assert_equal(res["fee"], Decimal("0.00001694"))

        # creating a 10,000 sat transaction with a manual change address should be possible
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], options={"subtractFeeFromOutputs": [0], "feeRate": 0.000010, "changeAddress": addr.pop()})
        assert_equal("psbt" in res, True)

if __name__ == '__main__':
    KeyPoolTest().main()
