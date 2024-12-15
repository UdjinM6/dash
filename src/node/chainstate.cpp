// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <chainparams.h> // for CChainParams
#include <deploymentstatus.h> // for DeploymentActiveAfter
#include <rpc/blockchain.h> // for RPCNotifyBlockChange
#include <util/time.h> // for GetTime
#include <node/blockstorage.h> // for CleanupBlockRevFiles, fHavePruned, fReindex
#include <node/context.h> // for NodeContext
#include <shutdown.h> // for ShutdownRequested
#include <validation.h> // for a lot of things

#include <evo/chainhelper.h> // for CChainstateHelper
#include <evo/creditpool.h> // for CCreditPoolManager
#include <evo/deterministicmns.h> // for CDeterministicMNManager
#include <evo/evodb.h> // for CEvoDB
#include <evo/mnhftx.h> // for CMNHFManager
#include <llmq/chainlocks.h> // for llmq::chainLocksHandler
#include <llmq/context.h> // for LLMQContext
#include <llmq/instantsend.h> // for llmq::quorumInstantSendManager
#include <llmq/snapshot.h> // for llmq::quorumSnapshotManager

std::optional<ChainstateLoadingError> LoadChainstate(bool fReset,
                                                     ChainstateManager& chainman,
                                                     CGovernanceManager& govman,
                                                     CMasternodeMetaMan& mn_metaman,
                                                     CMasternodeSync& mn_sync,
                                                     CSporkManager& sporkman,
                                                     std::unique_ptr<CActiveMasternodeManager>& mn_activeman,
                                                     std::unique_ptr<CChainstateHelper>& chain_helper,
                                                     std::unique_ptr<CCreditPoolManager>& cpoolman,
                                                     std::unique_ptr<CDeterministicMNManager>& dmnman,
                                                     std::unique_ptr<CEvoDB>& evodb,
                                                     std::unique_ptr<CMNHFManager>& mnhf_manager,
                                                     std::unique_ptr<llmq::CChainLocksHandler>& clhandler,
                                                     std::unique_ptr<llmq::CInstantSendManager>& isman,
                                                     std::unique_ptr<llmq::CQuorumSnapshotManager>& qsnapman,
                                                     std::unique_ptr<LLMQContext>& llmq_ctx,
                                                     CTxMemPool* mempool,
                                                     bool fPruneMode,
                                                     bool is_addrindex_enabled,
                                                     bool is_governance_enabled,
                                                     bool is_spentindex_enabled,
                                                     bool is_timeindex_enabled,
                                                     bool is_txindex_enabled,
                                                     const CChainParams& chainparams,
                                                     bool fReindexChainState,
                                                     int64_t nBlockTreeDBCache,
                                                     int64_t nCoinDBCache,
                                                     int64_t nCoinCacheUsage,
                                                     std::function<void()> coins_error_cb)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    {
        LOCK(cs_main);

        int64_t nEvoDbCache{64 * 1024 * 1024}; // TODO
        evodb.reset();
        evodb = std::make_unique<CEvoDB>(nEvoDbCache, false, fReset || fReindexChainState);

        mnhf_manager.reset();
        mnhf_manager = std::make_unique<CMNHFManager>(*evodb);

        chainman.InitializeChainstate(mempool, *evodb, chain_helper, clhandler, isman);
        chainman.m_total_coinstip_cache = nCoinCacheUsage;
        chainman.m_total_coinsdb_cache = nCoinDBCache;

        auto& pblocktree{chainman.m_blockman.m_block_tree_db};
        // new CBlockTreeDB tries to delete the existing file, which
        // fails if it's still open from the previous loop. Close it first:
        pblocktree.reset();
        pblocktree.reset(new CBlockTreeDB(nBlockTreeDBCache, false, fReset));

        // Same logic as above with pblocktree
        dmnman.reset();
        dmnman = std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), *evodb);
        mempool->ConnectManagers(dmnman.get());

        cpoolman.reset();
        cpoolman = std::make_unique<CCreditPoolManager>(*evodb);

        qsnapman.reset();
        qsnapman.reset(new llmq::CQuorumSnapshotManager(*evodb));

        if (llmq_ctx) {
            llmq_ctx->Interrupt();
            llmq_ctx->Stop();
        }
        llmq_ctx.reset();
        llmq_ctx = std::make_unique<LLMQContext>(chainman, *dmnman, *evodb, mn_metaman, *mnhf_manager, sporkman,
                                                 *mempool, mn_activeman.get(), mn_sync, /*unit_tests=*/false, /*wipe=*/fReset || fReindexChainState);
        // Enable CMNHFManager::{Process, Undo}Block
        mnhf_manager->ConnectManagers(&chainman, llmq_ctx->qman.get());

        chain_helper.reset();
        chain_helper = std::make_unique<CChainstateHelper>(*cpoolman, *dmnman, *mnhf_manager, govman, *(llmq_ctx->quorum_block_processor), chainman,
                                                            chainparams.GetConsensus(), mn_sync, sporkman, *(llmq_ctx->clhandler), *(llmq_ctx->qman));

        if (fReset) {
            pblocktree->WriteReindexing(true);
            //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
            if (fPruneMode)
                CleanupBlockRevFiles();
        }

        if (ShutdownRequested()) return ChainstateLoadingError::SHUTDOWN_PROBED;

        // LoadBlockIndex will load m_have_pruned if we've ever removed a
        // block file from disk.
        // Note that it also sets fReindex based on the disk flag!
        // From here on out fReindex and fReset mean something different!
        if (!chainman.LoadBlockIndex()) {
            if (ShutdownRequested()) return ChainstateLoadingError::SHUTDOWN_PROBED;
            return ChainstateLoadingError::ERROR_LOADING_BLOCK_DB;
        }

        // TODO: Remove this when pruning is fixed.
        // See https://github.com/dashpay/dash/pull/1817 and https://github.com/dashpay/dash/pull/1743
        if (is_governance_enabled && !is_txindex_enabled && chainparams.NetworkIDString() != CBaseChainParams::REGTEST) {
            return ChainstateLoadingError::ERROR_TXINDEX_DISABLED_WHEN_GOV_ENABLED;
        }

        if (!chainman.BlockIndex().empty() &&
                !chainman.m_blockman.LookupBlockIndex(chainparams.GetConsensus().hashGenesisBlock)) {
            return ChainstateLoadingError::ERROR_BAD_GENESIS_BLOCK;
        }

        if (!chainparams.GetConsensus().hashDevnetGenesisBlock.IsNull() && !chainman.BlockIndex().empty() &&
                !chainman.m_blockman.LookupBlockIndex(chainparams.GetConsensus().hashDevnetGenesisBlock)) {
            return ChainstateLoadingError::ERROR_BAD_DEVNET_GENESIS_BLOCK;
        }

        if (!fReset && !fReindexChainState) {
            // Check for changed -addressindex state
            if (!fAddressIndex && fAddressIndex != is_addrindex_enabled) {
                return ChainstateLoadingError::ERROR_ADDRIDX_NEEDS_REINDEX;
            }

            // Check for changed -timestampindex state
            if (!fTimestampIndex && fTimestampIndex != is_timeindex_enabled) {
                return ChainstateLoadingError::ERROR_TIMEIDX_NEEDS_REINDEX;
            }

            // Check for changed -spentindex state
            if (!fSpentIndex && fSpentIndex != is_spentindex_enabled) {
                return ChainstateLoadingError::ERROR_SPENTIDX_NEEDS_REINDEX;
            }
        }

        chainman.InitAdditionalIndexes();

        LogPrintf("%s: address index %s\n", __func__, fAddressIndex ? "enabled" : "disabled");
        LogPrintf("%s: timestamp index %s\n", __func__, fTimestampIndex ? "enabled" : "disabled");
        LogPrintf("%s: spent index %s\n", __func__, fSpentIndex ? "enabled" : "disabled");

        // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
        // in the past, but is now trying to run unpruned.
        if (chainman.m_blockman.m_have_pruned && !fPruneMode) {
            return ChainstateLoadingError::ERROR_PRUNED_NEEDS_REINDEX;
        }

        // At this point blocktree args are consistent with what's on disk.
        // If we're not mid-reindex (based on disk + args), add a genesis block on disk
        // (otherwise we use the one already on disk).
        // This is called again in ThreadImport after the reindex completes.
        if (!fReindex && !chainman.ActiveChainstate().LoadGenesisBlock()) {
            return ChainstateLoadingError::ERROR_LOAD_GENESIS_BLOCK_FAILED;
        }

        // At this point we're either in reindex or we've loaded a useful
        // block tree into BlockIndex()!

        for (CChainState* chainstate : chainman.GetAll()) {
            chainstate->InitCoinsDB(
                /* cache_size_bytes */ nCoinDBCache,
                /* in_memory */ false,
                /* should_wipe */ fReset || fReindexChainState);

            if (coins_error_cb) {
                chainstate->CoinsErrorCatcher().AddReadErrCallback(coins_error_cb);
            }

            // If necessary, upgrade from older database format.
            // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
            if (!chainstate->CoinsDB().Upgrade()) {
                return ChainstateLoadingError::ERROR_CHAINSTATE_UPGRADE_FAILED;
            }

            // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
            if (!chainstate->ReplayBlocks()) {
                return ChainstateLoadingError::ERROR_REPLAYBLOCKS_FAILED;
            }

            // The on-disk coinsdb is now in a good state, create the cache
            chainstate->InitCoinsCache(nCoinCacheUsage);
            assert(chainstate->CanFlushToDisk());

            // flush evodb
            // TODO: CEvoDB instance should probably be a part of CChainState
            // (for multiple chainstates to actually work in parallel)
            // and not a global
            if (&chainman.ActiveChainstate() == chainstate && !evodb->CommitRootTransaction()) {
                return ChainstateLoadingError::ERROR_COMMITING_EVO_DB;
            }

            if (!is_coinsview_empty(chainstate)) {
                // LoadChainTip initializes the chain based on CoinsTip()'s best block
                if (!chainstate->LoadChainTip()) {
                    return ChainstateLoadingError::ERROR_LOADCHAINTIP_FAILED;
                }
                assert(chainstate->m_chain.Tip() != nullptr);
            }
        }

        if (!dmnman->MigrateDBIfNeeded() || !dmnman->MigrateDBIfNeeded2()) {
            return ChainstateLoadingError::ERROR_UPGRADING_EVO_DB;
        }
        if (!mnhf_manager->ForceSignalDBUpdate()) {
            return ChainstateLoadingError::ERROR_UPGRADING_SIGNALS_DB;
        }
    }

    return std::nullopt;
}

std::optional<ChainstateLoadVerifyError> VerifyLoadedChainstate(ChainstateManager& chainman,
                                                                CEvoDB& evodb,
                                                                bool fReset,
                                                                bool fReindexChainState,
                                                                const CChainParams& chainparams,
                                                                unsigned int check_blocks,
                                                                unsigned int check_level)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    {
        LOCK(cs_main);

        for (CChainState* chainstate : chainman.GetAll()) {
            if (!is_coinsview_empty(chainstate)) {
                const CBlockIndex* tip = chainstate->m_chain.Tip();
                RPCNotifyBlockChange(tip);
                if (tip && tip->nTime > GetTime() + MAX_FUTURE_BLOCK_TIME) {
                    return ChainstateLoadVerifyError::ERROR_BLOCK_FROM_FUTURE;
                }
                const bool v19active{DeploymentActiveAfter(tip, chainparams.GetConsensus(), Consensus::DEPLOYMENT_V19)};
                if (v19active) {
                    bls::bls_legacy_scheme.store(false);
                    LogPrintf("%s: bls_legacy_scheme=%d\n", __func__, bls::bls_legacy_scheme.load());
                }

                if (!CVerifyDB().VerifyDB(
                        *chainstate, chainparams, chainstate->CoinsDB(),
                        evodb,
                        check_level,
                        check_blocks)) {
                    return ChainstateLoadVerifyError::ERROR_CORRUPTED_BLOCK_DB;
                }

                // VerifyDB() disconnects blocks which might result in us switching back to legacy.
                // Make sure we use the right scheme.
                if (v19active && bls::bls_legacy_scheme.load()) {
                    bls::bls_legacy_scheme.store(false);
                    LogPrintf("%s: bls_legacy_scheme=%d\n", __func__, bls::bls_legacy_scheme.load());
                }

                if (check_level >= 3) {
                    chainstate->ResetBlockFailureFlags(nullptr);
                }

            } else {
                // TODO: CEvoDB instance should probably be a part of CChainState
                // (for multiple chainstates to actually work in parallel)
                // and not a global
                if (&chainman.ActiveChainstate() == chainstate && !evodb.IsEmpty()) {
                    // EvoDB processed some blocks earlier but we have no blocks anymore, something is wrong
                    return ChainstateLoadVerifyError::ERROR_EVO_DB_SANITY_FAILED;
                }
            }
        }
    }

    return std::nullopt;
}
