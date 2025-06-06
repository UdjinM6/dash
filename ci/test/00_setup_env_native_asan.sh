#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export PACKAGES="clang llvm python3-zmq qtbase5-dev qttools5-dev qttools5-dev-tools libevent-dev bsdmainutils libboost-dev libdb5.3++-dev libminiupnpc-dev libzmq3-dev libqrencode-dev"
export NO_DEPENDS=1
export TEST_RUNNER_EXTRA="--timeout-factor=4"  # Increase timeout because sanitizers slow down
export FUNCTIONAL_TESTS_CONFIG="--exclude wallet_multiwallet.py" # Temporarily suppress ASan heap-use-after-free (see issue #14163)
export RUN_BENCH=true
export GOAL="install"
export BITCOIN_CONFIG="--enable-zmq --with-incompatible-bdb --with-gui=qt5 CPPFLAGS=-DDEBUG_LOCKORDER --with-sanitizers=address,integer,undefined CC=clang CXX=clang++ --with-boost-process"
