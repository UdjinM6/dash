#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A limited-functionality wallet, which may replace a real wallet in tests"""

from copy import deepcopy
from decimal import Decimal
from enum import Enum
from random import choice
from typing import (
    Any,
    List,
    Optional,
)
from test_framework.address import (
    base58_to_byte,
    key_to_p2pkh,
    ADDRESS_BCRT1_P2SH_OP_TRUE,
)
from test_framework.descriptors import descsum_create
from test_framework.key import ECKey
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    SignatureHash,
    OP_TRUE,
    OP_NOP,
    SIGHASH_ALL,
)
from test_framework.script_util import (
    key_to_p2pk_script,
    key_to_p2pkh_script,
    keyhash_to_p2pkh_script,
    scripthash_to_p2sh_script,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
)
from test_framework.blocktools import COINBASE_MATURITY

DEFAULT_FEE = Decimal("0.0001")

class MiniWalletMode(Enum):
    """Determines the transaction type the MiniWallet is creating and spending.

    For most purposes, the default mode ADDRESS_OP_TRUE should be sufficient;
    it simply uses a fixed P2SH address whose coins are spent with a
    witness stack of OP_TRUE, i.e. following an anyone-can-spend policy.
    However, if the transactions need to be modified by the user (e.g. prepending
    scriptSig for testing opcodes that are activated by a soft-fork), or the txs
    should contain an actual signature, the raw modes RAW_OP_TRUE and RAW_P2PK
    can be useful. Summary of modes:

                    |      output       |           |  tx is   | can modify |  needs
         mode       |    description    |  address  | standard | scriptSig  | signing
    ----------------+-------------------+-----------+----------+------------+----------
    ADDRESS_OP_TRUE | anyone-can-spend  |  bech32   |   yes    |    no      |   no
    RAW_OP_TRUE     | anyone-can-spend  |  - (raw)  |   no     |    yes     |   no
    RAW_P2PK        | pay-to-public-key |  - (raw)  |   yes    |    yes     |   yes
    """
    ADDRESS_OP_TRUE = 1
    RAW_OP_TRUE = 2
    RAW_P2PK = 3


class MiniWallet:
    def __init__(self, test_node, *, mode=MiniWalletMode.ADDRESS_OP_TRUE):
        self._test_node = test_node
        self._utxos = []
        self._mode = mode

        assert isinstance(mode, MiniWalletMode)
        if mode == MiniWalletMode.RAW_OP_TRUE:
            self._scriptPubKey = bytes(CScript([OP_TRUE]))
        elif mode == MiniWalletMode.RAW_P2PK:
            # use simple deterministic private key (k=1)
            self._priv_key = ECKey()
            self._priv_key.set((1).to_bytes(32, 'big'), True)
            pub_key = self._priv_key.get_pubkey()
            self._scriptPubKey = key_to_p2pk_script(pub_key.get_bytes())
        elif mode == MiniWalletMode.ADDRESS_OP_TRUE:
            self._address = ADDRESS_BCRT1_P2SH_OP_TRUE
            self._scriptPubKey = bytes.fromhex(self._test_node.validateaddress(self._address)['scriptPubKey'])

    def _create_utxo(self, *, txid, vout, value, height, coinbase, confirmations):
        return {"txid": txid, "vout": vout, "value": value, "height": height, "coinbase": coinbase, "confirmations": confirmations}

    def get_balance(self):
        return sum(u['value'] for u in self._utxos)

    def rescan_utxos(self, *, include_mempool=True):
        """Drop all utxos and rescan the utxo set"""
        self._utxos = []
        res = self._test_node.scantxoutset(action="start", scanobjects=[self.get_descriptor()])
        assert_equal(True, res['success'])
        for utxo in res['unspents']:
            self._utxos.append(
                self._create_utxo(txid=utxo["txid"],
                                  vout=utxo["vout"],
                                  value=utxo["amount"],
                                  height=utxo["height"],
                                  coinbase=utxo["coinbase"],
                                  confirmations=res["height"] - utxo["height"] + 1))
        if include_mempool:
            mempool = self._test_node.getrawmempool(verbose=True)
            # Sort tx by ancestor count. See BlockAssembler::SortForBlock in src/node/miner.cpp
            sorted_mempool = sorted(mempool.items(), key=lambda item: (item[1]["ancestorcount"], int(item[0], 16)))
            for txid, _ in sorted_mempool:
                self.scan_tx(self._test_node.getrawtransaction(txid=txid, verbose=True))

    def scan_tx(self, tx):
        """Scan the tx and adjust the internal list of owned utxos"""
        for spent in tx["vin"]:
            # Mark spent. This may happen when the caller has ownership of a
            # utxo that remained in this wallet. For example, by passing
            # mark_as_spent=False to get_utxo or by using an utxo returned by a
            # create_self_transfer* call.
            try:
                self.get_utxo(txid=spent["txid"], vout=spent["vout"])
            except StopIteration:
                pass
        for out in tx['vout']:
            if out['scriptPubKey']['hex'] == self._scriptPubKey.hex():
                self._utxos.append(self._create_utxo(txid=tx["txid"], vout=out["n"], value=out["value"], height=0, coinbase=False, confirmations=0))

    def sign_tx(self, tx, fixed_length=True):
        if self._mode == MiniWalletMode.RAW_P2PK:
            (sighash, err) = SignatureHash(CScript(self._scriptPubKey), tx, 0, SIGHASH_ALL)
            assert err is None
            # for exact fee calculation, create only signatures with fixed size by default (>49.89% probability):
            # 65 bytes: high-R val (33 bytes) + low-S val (32 bytes)
            # with the DER header/skeleton data of 6 bytes added, this leads to a target size of 71 bytes
            der_sig = b''
            while not len(der_sig) == 71:
                der_sig = self._priv_key.sign_ecdsa(sighash)
                if not fixed_length:
                    break
            tx.vin[0].scriptSig = CScript([der_sig + bytes(bytearray([SIGHASH_ALL]))])
            tx.rehash()
        elif self._mode == MiniWalletMode.RAW_OP_TRUE:
            for i in range(len(tx.vin)):
                tx.vin[i].scriptSig = CScript([OP_NOP] * 24)  # pad to identical size
        elif self._mode == MiniWalletMode.ADDRESS_OP_TRUE:
            for i in range(len(tx.vin)):
                tx.vin[i].scriptSig = CScript([CScript([OP_TRUE])])
        else:
            assert False

    def generate(self, num_blocks, **kwargs):
        """Generate blocks with coinbase outputs to the internal address, and call rescan_utxos"""
        blocks = self._test_node.generatetodescriptor(num_blocks, self.get_descriptor(), **kwargs)
        # Calling rescan_utxos here makes sure that after a generate the utxo
        # set is in a clean state. For example, the wallet will update
        # - if the caller consumed utxos, but never used them
        # - if the caller sent a transaction that is not mined or got rbf'd
        # - after block re-orgs
        # - the utxo height for mined mempool txs
        # - However, the wallet will not consider remaining mempool txs
        self.rescan_utxos()
        return blocks

    def get_scriptPubKey(self):
        return self._scriptPubKey

    def get_descriptor(self):
        return descsum_create(f'raw({self._scriptPubKey.hex()})')

    def get_address(self):
        assert_equal(self._mode, MiniWalletMode.ADDRESS_OP_TRUE)
        return self._address

    def get_utxo(self, *, txid: str = '', vout: Optional[int] = None, mark_as_spent=True) -> dict:
        """
        Returns a utxo and marks it as spent (pops it from the internal list)

        Args:
        txid: get the first utxo we find from a specific transaction
        """
        self._utxos = sorted(self._utxos, key=lambda k: (k['value'], -k['height']))  # Put the largest utxo last
        if txid:
            utxo_filter: Any = filter(lambda utxo: txid == utxo['txid'], self._utxos)
        else:
            utxo_filter = reversed(self._utxos)  # By default the largest utxo
        if vout is not None:
            utxo_filter = filter(lambda utxo: vout == utxo['vout'], utxo_filter)
        index = self._utxos.index(next(utxo_filter))
        if mark_as_spent:
            return self._utxos.pop(index)
        else:
            return self._utxos[index]

    def get_utxos(self, *, include_immature_coinbase=False, mark_as_spent=True):
        """Returns the list of all utxos and optionally mark them as spent"""
        if not include_immature_coinbase:
            utxo_filter = filter(lambda utxo: not utxo['coinbase'] or COINBASE_MATURITY <= utxo['confirmations'], self._utxos)
        else:
            utxo_filter = self._utxos
        utxos = deepcopy(list(utxo_filter))
        if mark_as_spent:
            self._utxos = []
        return utxos

    def send_self_transfer(self, *, from_node, **kwargs):
        """Call create_self_transfer and send the transaction."""
        tx = self.create_self_transfer(**kwargs)
        self.sendrawtransaction(from_node=from_node, tx_hex=tx['hex'])
        return tx

    def send_to(self, *, from_node, scriptPubKey, amount, fee=1000):
        """
        Create and send a tx with an output to a given scriptPubKey/amount,
        plus a change output to our internal address. To keep things simple, a
        fixed fee given in Satoshi is used.

        Note that this method fails if there is no single internal utxo
        available that can cover the cost for the amount and the fixed fee
        (the utxo with the largest value is taken).

        Returns a tuple (txid, n) referring to the created external utxo outpoint.
        """
        tx = self.create_self_transfer(fee_rate=0)["tx"]
        assert_greater_than_or_equal(tx.vout[0].nValue, amount + fee)
        tx.vout[0].nValue -= (amount + fee)           # change output -> MiniWallet
        tx.vout.append(CTxOut(amount, scriptPubKey))  # arbitrary output -> to be returned
        txid = self.sendrawtransaction(from_node=from_node, tx_hex=tx.serialize().hex())
        return txid, 1

    def send_self_transfer_multi(self, *, from_node, **kwargs):
        """Call create_self_transfer_multi and send the transaction."""
        tx = self.create_self_transfer_multi(**kwargs)
        self.sendrawtransaction(from_node=from_node, tx_hex=tx["hex"])
        return tx

    def create_self_transfer_multi(
        self,
        *,
        utxos_to_spend: Optional[List[dict]] = None,
        num_outputs=1,
        amount_per_output=0,
        locktime=0,
        sequence=0,
        fee_per_output=1000,
    ):
        """
        Create and return a transaction that spends the given UTXOs and creates a
        certain number of outputs with equal amounts. The output amounts can be
        set by amount_per_output or automatically calculated with a fee_per_output.
        """
        utxos_to_spend = utxos_to_spend or [self.get_utxo()]
        sequence = [sequence] * len(utxos_to_spend) if type(sequence) is int else sequence
        assert_equal(len(utxos_to_spend), len(sequence))

        # calculate output amount
        inputs_value_total = sum([int(COIN * utxo['value']) for utxo in utxos_to_spend])
        outputs_value_total = inputs_value_total - fee_per_output * num_outputs
        amount_per_output = amount_per_output or (outputs_value_total // num_outputs)

        # create tx
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo_to_spend['txid'], 16), utxo_to_spend['vout']), nSequence=seq) for utxo_to_spend,seq in zip(utxos_to_spend, sequence)]
        tx.vout = [CTxOut(amount_per_output, bytearray(self._scriptPubKey)) for _ in range(num_outputs)]
        tx.nLockTime = locktime

        self.sign_tx(tx)

        txid = tx.rehash()
        return {
            "new_utxos": [self._create_utxo(
                txid=txid,
                vout=i,
                value=Decimal(tx.vout[i].nValue) / COIN,
                height=0,
                coinbase=False,
                confirmations=0,
            ) for i in range(len(tx.vout))],
            "txid": txid,
            "hex": tx.serialize().hex(),
            "tx": tx,
        }

    def create_self_transfer(self, *, fee_rate=Decimal("0.003"), fee=Decimal("0"), utxo_to_spend=None, locktime=0, sequence=0):
        """Create and return a tx with the specified fee. If fee is 0, use fee_rate, where the resulting fee may be exact or at most one satoshi higher than needed."""
        utxo_to_spend = utxo_to_spend or self.get_utxo()
        assert fee_rate >= 0
        assert fee >= 0
        # calculate fee
        if self._mode in (MiniWalletMode.RAW_OP_TRUE, MiniWalletMode.ADDRESS_OP_TRUE):
            vsize = Decimal(85)  # anyone-can-spend
        elif self._mode == MiniWalletMode.RAW_P2PK:
            vsize = Decimal(168)  # P2PK (73 bytes scriptSig + 35 bytes scriptPubKey + 60 bytes other)
        else:
            assert False
        send_value = utxo_to_spend["value"] - (fee or (fee_rate * vsize / 1000))
        assert send_value > 0

        # create tx
        tx = self.create_self_transfer_multi(utxos_to_spend=[utxo_to_spend], locktime=locktime, sequence=sequence, amount_per_output=int(COIN * send_value))
        assert_equal(tx["tx"].get_vsize(), vsize)

        return {"txid": tx["txid"], "hex": tx["hex"], "tx": tx["tx"], "new_utxo": tx["new_utxos"][0]}

    def sendrawtransaction(self, *, from_node, tx_hex, maxfeerate=0, **kwargs):
        txid = from_node.sendrawtransaction(hexstring=tx_hex, maxfeerate=maxfeerate, **kwargs)
        self.scan_tx(from_node.decoderawtransaction(tx_hex))
        return txid


def getnewdestination(address_type='legacy'):
    """Generate a random destination of the specified type and return the
       corresponding public key, scriptPubKey and address. Supported types are
       'legacy'. Can be used when a random destination is needed, but no
       compiled wallet is available (e.g. as replacement to the
       getnewaddress/getaddressinfo RPCs)."""
    key = ECKey()
    key.generate()
    pubkey = key.get_pubkey().get_bytes()
    if address_type == 'legacy':
        scriptpubkey = key_to_p2pkh_script(pubkey)
        address = key_to_p2pkh(pubkey)
    else:
        assert False
    return pubkey, scriptpubkey, address


def address_to_scriptpubkey(address):
    """Converts a given address to the corresponding output script (scriptPubKey)."""
    payload, version = base58_to_byte(address)
    if version == 140:  # testnet pubkey hash
        return keyhash_to_p2pkh_script(payload)
    elif version == 19:  # testnet script hash
        return scripthash_to_p2sh_script(payload)
    # TODO: also support other address formats
    else:
        assert False


def make_chain(node, address, privkeys, parent_txid, parent_value, n=0, parent_locking_script=None, fee=DEFAULT_FEE):
    """Build a transaction that spends parent_txid.vout[n] and produces one output with
    amount = parent_value with a fee deducted.
    Return tuple (CTransaction object, raw hex, nValue, scriptPubKey of the output created).
    """
    inputs = [{"txid": parent_txid, "vout": n}]
    my_value = parent_value - fee
    outputs = {address : my_value}
    rawtx = node.createrawtransaction(inputs, outputs)
    prevtxs = [{
        "txid": parent_txid,
        "vout": n,
        "scriptPubKey": parent_locking_script,
        "amount": parent_value,
    }] if parent_locking_script else None
    signedtx = node.signrawtransactionwithkey(hexstring=rawtx, privkeys=privkeys, prevtxs=prevtxs)
    assert signedtx["complete"]
    tx = tx_from_hex(signedtx["hex"])
    return (tx, signedtx["hex"], my_value, tx.vout[0].scriptPubKey.hex())

def create_child_with_parents(node, address, privkeys, parents_tx, values, locking_scripts, fee=DEFAULT_FEE):
    """Creates a transaction that spends the first output of each parent in parents_tx."""
    num_parents = len(parents_tx)
    total_value = sum(values)
    inputs = [{"txid": tx.rehash(), "vout": 0} for tx in parents_tx]
    outputs = {address : total_value - fee}
    rawtx_child = node.createrawtransaction(inputs, outputs)
    prevtxs = []
    for i in range(num_parents):
        prevtxs.append({"txid": parents_tx[i].rehash(), "vout": 0, "scriptPubKey": locking_scripts[i], "amount": values[i]})
    signedtx_child = node.signrawtransactionwithkey(hexstring=rawtx_child, privkeys=privkeys, prevtxs=prevtxs)
    assert signedtx_child["complete"]
    return signedtx_child["hex"]

def create_raw_chain(node, first_coin, address, privkeys, chain_length=25):
    """Helper function: create a "chain" of chain_length transactions. The nth transaction in the
    chain is a child of the n-1th transaction and parent of the n+1th transaction.
    """
    parent_locking_script = None
    txid = first_coin["txid"]
    chain_hex = []
    chain_txns = []
    value = first_coin["amount"]

    for _ in range(chain_length):
        (tx, txhex, value, parent_locking_script) = make_chain(node, address, privkeys, txid, value, 0, parent_locking_script)
        txid = tx.rehash()
        chain_hex.append(txhex)
        chain_txns.append(tx)

    return (chain_hex, chain_txns)

def bulk_transaction(tx, node, target_weight, privkeys, prevtxs=None):
    """Pad a transaction with extra outputs until it reaches a target weight (or higher).
    returns CTransaction object
    """
    tx_heavy = deepcopy(tx)
    assert_greater_than_or_equal(target_weight, tx_heavy.get_weight())
    while tx_heavy.get_weight() < target_weight:
        random_spk = "6a4d0200"  # OP_RETURN OP_PUSH2 512 bytes
        for _ in range(512*2):
            random_spk += choice("0123456789ABCDEF")
        tx_heavy.vout.append(CTxOut(0, bytes.fromhex(random_spk)))
    # Re-sign the transaction
    if privkeys:
        signed = node.signrawtransactionwithkey(tx_heavy.serialize().hex(), privkeys, prevtxs)
        return tx_from_hex(signed["hex"])
    # OP_TRUE
    tx_heavy.vin[0].scriptSig = CScript([CScript([OP_TRUE])])
    return tx_heavy
