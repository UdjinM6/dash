// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_OBSERVER_CONTEXT_H
#define BITCOIN_LLMQ_OBSERVER_CONTEXT_H

#include <validationinterface.h>

#include <memory>

class CBLSWorker;
class CBlockIndex;
class CChainState;
class CDeterministicMNManager;
class CMasternodeMetaMan;
class CSporkManager;
namespace llmq {
class CDKGDebugManager;
class CDKGSessionManager;
class CQuorumBlockProcessor;
class CQuorumManager;
class CQuorumSnapshotManager;
} // namespace llmq
namespace util {
struct DbWrapperParams;
} // namespace util

namespace llmq {
struct ObserverContext final : public CValidationInterface {
private:
    llmq::CQuorumManager& m_qman;

public:
    ObserverContext() = delete;
    ObserverContext(const ObserverContext&) = delete;
    ObserverContext& operator=(const ObserverContext&) = delete;
    ObserverContext(CBLSWorker& bls_worker, CChainState& chainstate, CDeterministicMNManager& dmnman,
                    CMasternodeMetaMan& mn_metaman, llmq::CDKGDebugManager& dkg_debugman,
                    llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                    llmq::CQuorumSnapshotManager& qsnapman, const CSporkManager& sporkman,
                    const util::DbWrapperParams& db_params);
    ~ObserverContext();

protected:
    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

public:
    const std::unique_ptr<llmq::CDKGSessionManager> qdkgsman;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_OBSERVER_CONTEXT_H
