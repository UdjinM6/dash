#!/usr/bin/env python3
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Helpful routines for masternode regression testing."""

def check_punished(node, mn):
    info = node.protx('info', mn.proTxHash)
    if info['state']['PoSePenalty'] > 0:
        return True
    return False

def check_banned(node, mn):
    info = node.protx('info', mn.proTxHash)
    if info['state']['PoSeBanHeight'] != -1:
        return True
    return False
