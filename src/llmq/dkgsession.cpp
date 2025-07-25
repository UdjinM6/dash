// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsession.h>

#include <llmq/commitment.h>
#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/options.h>
#include <llmq/utils.h>

#include <batchedlogger.h>
#include <chainparams.h>
#include <cxxtimer.hpp>
#include <deploymentstatus.h>
#include <evo/deterministicmns.h>
#include <logging.h>
#include <masternode/meta.h>
#include <masternode/node.h>
#include <util/irange.h>
#include <util/underlying.h>

#include <array>
#include <atomic>
#include <memory>

namespace llmq
{

CDKGLogger::CDKGLogger(const CDKGSession& _quorumDkg, std::string_view _func, int source_line) :
    CBatchedLogger(BCLog::LLMQ_DKG, BCLog::Level::Debug,
                   strprintf("QuorumDKG(type=%s, qIndex=%d, h=%d, member=%d)", _quorumDkg.params.name, _quorumDkg.quorumIndex,
                             _quorumDkg.m_quorum_base_block_index->nHeight, _quorumDkg.AreWeMember()),
                   __FILE__, source_line)
{
}

static std::array<std::atomic<double>, ToUnderlying(DKGError::type::_COUNT)> simDkgErrorMap{};

void SetSimulatedDKGErrorRate(DKGError::type type, double rate)
{
    if (type >= DKGError::type::_COUNT) return;
    simDkgErrorMap[ToUnderlying(type)] = rate;
}

double GetSimulatedErrorRate(DKGError::type type)
{
    if (ToUnderlying(type) >= DKGError::type::_COUNT) return 0;
    return simDkgErrorMap[ToUnderlying(type)];
}

bool CDKGSession::ShouldSimulateError(DKGError::type type) const
{
    if (params.type != Consensus::LLMQType::LLMQ_TEST) {
        return false;
    }
    double rate = GetSimulatedErrorRate(type);
    return GetRandBool(rate);
}

CDKGMember::CDKGMember(const CDeterministicMNCPtr& _dmn, size_t _idx) :
    dmn(_dmn),
    idx(_idx),
    id(_dmn->proTxHash)
{

}

CDKGSession::CDKGSession(const CBlockIndex* pQuorumBaseBlockIndex, const Consensus::LLMQParams& _params,
                         CBLSWorker& _blsWorker, CDeterministicMNManager& dmnman, CDKGSessionManager& _dkgManager,
                         CDKGDebugManager& _dkgDebugManager, CMasternodeMetaMan& mn_metaman,
                         CQuorumSnapshotManager& qsnapman, const CActiveMasternodeManager* const mn_activeman,
                         const CSporkManager& sporkman) :
    params(_params),
    blsWorker(_blsWorker),
    cache(_blsWorker),
    m_dmnman(dmnman),
    dkgManager(_dkgManager),
    dkgDebugManager(_dkgDebugManager),
    m_mn_metaman(mn_metaman),
    m_qsnapman(qsnapman),
    m_mn_activeman(mn_activeman),
    m_sporkman(sporkman),
    m_quorum_base_block_index{pQuorumBaseBlockIndex},
    m_use_legacy_bls{!DeploymentActiveAfter(m_quorum_base_block_index, Params().GetConsensus(), Consensus::DEPLOYMENT_V19)}
{
}

bool CDKGSession::Init(const uint256& _myProTxHash, int _quorumIndex)
{
    const auto mns = utils::GetAllQuorumMembers(params.type, m_dmnman, m_qsnapman, m_quorum_base_block_index);
    quorumIndex = _quorumIndex;
    members.resize(mns.size());
    memberIds.resize(members.size());
    receivedVvecs.resize(members.size());
    receivedSkContributions.resize(members.size());
    vecEncryptedContributions.resize(members.size());

    for (const auto i : irange::range(mns.size())) {
        members[i] = std::make_unique<CDKGMember>(mns[i], i);
        membersMap.emplace(members[i]->dmn->proTxHash, i);
        memberIds[i] = members[i]->id;
    }

    if (!_myProTxHash.IsNull()) {
        for (const auto i : irange::range(members.size())) {
            const auto& m = members[i];
            if (m->dmn->proTxHash == _myProTxHash) {
                myIdx = i;
                myProTxHash = _myProTxHash;
                myId = m->id;
                break;
            }
        }
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    if (LogAcceptDebug(BCLog::LLMQ) && IsQuorumRotationEnabled(params, m_quorum_base_block_index)) {
        int cycleQuorumBaseHeight = m_quorum_base_block_index->nHeight - quorumIndex;
        const CBlockIndex* pCycleQuorumBaseBlockIndex = m_quorum_base_block_index->GetAncestor(cycleQuorumBaseHeight);
        std::stringstream ss;
        for (const auto& mn : members) {
            ss << mn->dmn->proTxHash.ToString().substr(0, 4) << " | ";
        }
        logger.Batch("DKGComposition h[%d] i[%d] DKG:[%s]", pCycleQuorumBaseBlockIndex->nHeight, quorumIndex, ss.str());
    }

    if (mns.size() < size_t(params.minSize)) {
        logger.Batch("not enough members (%d < %d), aborting init", mns.size(), params.minSize);
        return false;
    }

    if (!myProTxHash.IsNull()) {
        dkgDebugManager.InitLocalSessionStatus(params, quorumIndex, m_quorum_base_block_index->GetBlockHash(), m_quorum_base_block_index->nHeight);
        relayMembers = utils::GetQuorumRelayMembers(params, m_dmnman, m_qsnapman, m_quorum_base_block_index,
                                                    myProTxHash, true);
        if (LogAcceptDebug(BCLog::LLMQ)) {
            std::stringstream ss;
            for (const auto& r : relayMembers) {
                ss << r.ToString().substr(0, 4) << " | ";
            }
            logger.Batch("forMember[%s] relayMembers[%s]", myProTxHash.ToString().substr(0, 4), ss.str());
        }
    }

    if (myProTxHash.IsNull()) {
        logger.Batch("initialized as observer. mns=%d", mns.size());
    } else {
        logger.Batch("initialized as member. mns=%d", mns.size());
    }

    return true;
}

void CDKGSession::Contribute(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    if (!AreWeMember()) {
        return;
    }

    assert(params.threshold > 1); // we should not get there with single-node-quorums

    cxxtimer::Timer t1(true);
    logger.Batch("generating contributions");
    if (!blsWorker.GenerateContributions(params.threshold, memberIds, vvecContribution, m_sk_contributions)) {
        // this should never happen actually
        logger.Batch("GenerateContributions failed");
        return;
    }
    logger.Batch("generated contributions. time=%d", t1.count());
    logger.Flush();

    SendContributions(pendingMessages, peerman);
}

void CDKGSession::SendContributions(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    assert(AreWeMember());

    logger.Batch("sending contributions");

    if (ShouldSimulateError(DKGError::type::CONTRIBUTION_OMIT)) {
        logger.Batch("omitting");
        return;
    }

    CDKGContribution qc;
    qc.llmqType = params.type;
    qc.quorumHash = m_quorum_base_block_index->GetBlockHash();
    qc.proTxHash = myProTxHash;
    qc.vvec = vvecContribution;

    cxxtimer::Timer t1(true);
    qc.contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>();
    qc.contributions->InitEncrypt(members.size());

    for (const auto i : irange::range(members.size())) {
        const auto& m = members[i];
        CBLSSecretKey skContrib = m_sk_contributions[i];

        if (i != myIdx && ShouldSimulateError(DKGError::type::CONTRIBUTION_LIE)) {
            logger.Batch("lying for %s", m->dmn->proTxHash.ToString());
            skContrib.MakeNewKey();
        }

        if (!qc.contributions->Encrypt(i, m->dmn->pdmnState->pubKeyOperator.Get(), skContrib, PROTOCOL_VERSION)) {
            logger.Batch("failed to encrypt contribution for %s", m->dmn->proTxHash.ToString());
            return;
        }
    }

    logger.Batch("encrypted contributions. time=%d", t1.count());

    qc.sig = m_mn_activeman->Sign(qc.GetSignHash(), m_use_legacy_bls);

    logger.Flush();

    dkgDebugManager.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
        status.statusBits.sentContributions = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc, peerman);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const CDKGContribution& qc, bool& retBan) const
{
    CDKGLogger logger(*this, __func__, __LINE__);

    retBan = false;

    if (qc.quorumHash != m_quorum_base_block_index->GetBlockHash()) {
        logger.Batch("contribution for wrong quorum, rejecting");
        return false;
    }

    auto* member = GetMember(qc.proTxHash);
    if (member == nullptr) {
        logger.Batch("contributor not a member of this quorum, rejecting contribution");
        retBan = true;
        return false;
    }

    if (qc.contributions->blobs.size() != members.size()) {
        logger.Batch("invalid contributions count");
        retBan = true;
        return false;
    }
    if (qc.vvec->size() != size_t(params.threshold)) {
        logger.Batch("invalid verification vector length");
        retBan = true;
        return false;
    }

    if (!CBLSWorker::VerifyVerificationVector(*qc.vvec)) {
        logger.Batch("invalid verification vector");
        retBan = true;
        return false;
    }

    if (member->contributions.size() >= 2) {
        // don't do any further processing if we got more than 1 valid contributions already
        // this is a DoS protection against members sending multiple contributions with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        logger.Batch("dropping contribution from %s as we already got %d contributions", member->dmn->proTxHash.ToString(), member->contributions.size());
        return false;
    }

    return true;
}

// TODO: remove duplicated code between all ReceiveMessage: CDKGContribution, CDKGComplaint, CDKGJustification, CDKGPrematureCommitment
std::optional<CInv> CDKGSession::ReceiveMessage(const CDKGContribution& qc)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    auto* member = GetMember(qc.proTxHash);

    cxxtimer::Timer t1(true);
    logger.Batch("received contribution from %s", qc.proTxHash.ToString());

    // relay, no matter if further verification fails
    // This ensures the whole quorum sees the bad behavior

    if (member->contributions.size() >= 2) {
        // only relay up to 2 contributions, that's enough to let the other members know about his bad behavior
        return std::nullopt;
    }

    const uint256 hash = ::SerializeHash(qc);
    WITH_LOCK(invCs, contributions.emplace(hash, qc));
    member->contributions.emplace(hash);

    CInv inv(MSG_QUORUM_CONTRIB, hash);

    dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, member->idx, [&](CDKGDebugMemberStatus& status) {
        status.statusBits.receivedContribution = true;
        return true;
    });

    if (member->contributions.size() > 1) {
        // don't do any further processing if we got more than 1 contribution. we already relayed it,
        // so others know about his bad behavior
        MarkBadMember(member->idx);
        logger.Batch("%s did send multiple contributions", member->dmn->proTxHash.ToString());
        return inv;
    }

    receivedVvecs[member->idx] = qc.vvec;

    int receivedCount = ranges::count_if(members, [](const auto& m){return !m->contributions.empty();});

    logger.Batch("received and relayed contribution. received=%d/%d, time=%d", receivedCount, members.size(), t1.count());

    cxxtimer::Timer t2(true);

    if (!AreWeMember()) {
        // can't further validate
        return inv;
    }

    dkgManager.WriteVerifiedVvecContribution(params.type, m_quorum_base_block_index, qc.proTxHash, qc.vvec);

    bool complain = false;
    CBLSSecretKey skContribution;
    if (!m_mn_activeman->Decrypt(*qc.contributions, *myIdx, skContribution, PROTOCOL_VERSION)) {
        logger.Batch("contribution from %s could not be decrypted", member->dmn->proTxHash.ToString());
        complain = true;
    } else if (member->idx != myIdx && ShouldSimulateError(DKGError::type::COMPLAIN_LIE)) {
        logger.Batch("lying/complaining for %s", member->dmn->proTxHash.ToString());
        complain = true;
    }

    if (complain) {
        member->weComplain = true;
        dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, member->idx, [&](CDKGDebugMemberStatus& status) {
            status.statusBits.weComplain = true;
            return true;
        });
        return inv;
    }

    logger.Batch("decrypted our contribution share. time=%d", t2.count());

    receivedSkContributions[member->idx] = skContribution;
    vecEncryptedContributions[member->idx] = qc.contributions;
    LOCK(cs_pending);
    pendingContributionVerifications.emplace_back(member->idx);
    if (pendingContributionVerifications.size() >= 32) {
        VerifyPendingContributions();
    }
    return inv;
}

// Verifies all pending secret key contributions in one batch
// This is done by aggregating the verification vectors belonging to the secret key contributions
// The resulting aggregated vvec is then used to recover a public key share
// The public key share must match the public key belonging to the aggregated secret key contributions
// See CBLSWorker::VerifyContributionShares for more details.
void CDKGSession::VerifyPendingContributions()
{
    AssertLockHeld(cs_pending);

    CDKGLogger logger(*this, __func__, __LINE__);

    cxxtimer::Timer t1(true);

    if (pendingContributionVerifications.empty()) {
        return;
    }

    std::vector<size_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;

    for (const auto& idx : pendingContributionVerifications) {
        const auto& m = members[idx];
        if (m->bad || m->weComplain) {
            continue;
        }
        memberIndexes.emplace_back(idx);
        vvecs.emplace_back(receivedVvecs[idx]);
        skContributions.emplace_back(receivedSkContributions[idx]);
        // Write here to definitely store one contribution for each member no matter if
        // our share is valid or not, could be that others are still correct
        dkgManager.WriteEncryptedContributions(params.type, m_quorum_base_block_index, m->dmn->proTxHash, *vecEncryptedContributions[idx]);
    }

    auto result = blsWorker.VerifyContributionShares(myId, vvecs, skContributions);
    if (result.size() != memberIndexes.size()) {
        logger.Batch("VerifyContributionShares returned result of size %d but size %d was expected, something is wrong", result.size(), memberIndexes.size());
        return;
    }

    for (const auto i : irange::range(memberIndexes.size())) {
        if (!result[i]) {
            const auto& m = members[memberIndexes[i]];
            logger.Batch("invalid contribution from %s. will complain later", m->dmn->proTxHash.ToString());
            m->weComplain = true;
            dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, m->idx, [&](CDKGDebugMemberStatus& status) {
                status.statusBits.weComplain = true;
                return true;
            });
        } else {
            size_t memberIdx = memberIndexes[i];
            dkgManager.WriteVerifiedSkContribution(params.type, m_quorum_base_block_index, members[memberIdx]->dmn->proTxHash, skContributions[i]);
        }
    }

    logger.Batch("verified %d pending contributions. time=%d", pendingContributionVerifications.size(), t1.count());
    pendingContributionVerifications.clear();
}

void CDKGSession::VerifyAndComplain(CConnman& connman, CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    if (!AreWeMember()) {
        return;
    }

    {
        LOCK(cs_pending);
        VerifyPendingContributions();
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    // we check all members if they sent us their contributions
    // we consider members as bad if they missed to send anything or if they sent multiple
    // in both cases we won't give them a second chance as they might be either down, buggy or an adversary
    // we assume that such a participant will be marked as bad by the whole network in most cases,
    // as propagation will ensure that all nodes see the same vvecs/contributions. In case nodes come to
    // different conclusions, the aggregation phase will handle this (most voted quorum key wins).

    cxxtimer::Timer t1(true);

    for (const auto& m : members) {
        if (m->bad) {
            continue;
        }
        if (m->contributions.empty()) {
            logger.Batch("%s did not send any contribution", m->dmn->proTxHash.ToString());
            MarkBadMember(m->idx);
            continue;
        }
    }

    logger.Batch("verified contributions. time=%d", t1.count());
    logger.Flush();

    VerifyConnectionAndMinProtoVersions(connman);

    SendComplaint(pendingMessages, peerman);
}

void CDKGSession::VerifyConnectionAndMinProtoVersions(CConnman& connman) const
{
    assert(m_mn_metaman.IsValid());

    if (!IsQuorumPoseEnabled(params.type, m_sporkman)) {
        return;
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    std::unordered_map<uint256, int, StaticSaltedHasher> protoMap;
    connman.ForEachNode([&](const CNode* pnode) {
        auto verifiedProRegTxHash = pnode->GetVerifiedProRegTxHash();
        if (verifiedProRegTxHash.IsNull()) {
            return;
        }
        protoMap.emplace(verifiedProRegTxHash, pnode->nVersion);
    });

    bool fShouldAllMembersBeConnected = IsAllMembersConnectedEnabled(params.type, m_sporkman);
    for (const auto& m : members) {
        if (m->dmn->proTxHash == myProTxHash) {
            continue;
        }
        if (auto it = protoMap.find(m->dmn->proTxHash); it == protoMap.end()) {
            m->badConnection = fShouldAllMembersBeConnected;
            if (m->badConnection) {
                logger.Batch("%s is not connected to us, badConnection=1", m->dmn->proTxHash.ToString());
            }
        } else if (it->second < MIN_MASTERNODE_PROTO_VERSION) {
            m->badConnection = true;
            logger.Batch("%s does not have min proto version %d (has %d)", m->dmn->proTxHash.ToString(), MIN_MASTERNODE_PROTO_VERSION, it->second);
        }

        if (m_mn_metaman.GetMetaInfo(m->dmn->proTxHash)->OutboundFailedTooManyTimes()) {
            m->badConnection = true;
            logger.Batch("%s failed to connect to it too many times", m->dmn->proTxHash.ToString());
        }
    }
}

void CDKGSession::SendComplaint(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    assert(AreWeMember());

    CDKGComplaint qc(params);
    qc.llmqType = params.type;
    qc.quorumHash = m_quorum_base_block_index->GetBlockHash();
    qc.proTxHash = myProTxHash;

    int badCount = 0;
    int complaintCount = 0;
    for (const auto i : irange::range(members.size())) {
        const auto& m = members[i];
        if (m->bad || m->badConnection) {
            qc.badMembers[i] = true;
            badCount++;
        } else if (m->weComplain) {
            qc.complainForMembers[i] = true;
            complaintCount++;
        }
    }

    if (badCount == 0 && complaintCount == 0) {
        return;
    }

    logger.Batch("sending complaint. badCount=%d, complaintCount=%d", badCount, complaintCount);

    qc.sig = m_mn_activeman->Sign(qc.GetSignHash(), m_use_legacy_bls);

    logger.Flush();

    dkgDebugManager.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
        status.statusBits.sentComplaint = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc, peerman);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const CDKGComplaint& qc, bool& retBan) const
{
    CDKGLogger logger(*this, __func__, __LINE__);

    retBan = false;

    if (qc.quorumHash != m_quorum_base_block_index->GetBlockHash()) {
        logger.Batch("complaint for wrong quorum, rejecting");
        return false;
    }

    auto* member = GetMember(qc.proTxHash);
    if (member == nullptr) {
        logger.Batch("complainer not a member of this quorum, rejecting complaint");
        retBan = true;
        return false;
    }

    if (qc.badMembers.size() != (size_t)params.size) {
        logger.Batch("invalid badMembers bitset size");
        retBan = true;
        return false;
    }

    if (qc.complainForMembers.size() != (size_t)params.size) {
        logger.Batch("invalid complainForMembers bitset size");
        retBan = true;
        return false;
    }

    if (member->complaints.size() >= 2) {
        // don't do any further processing if we got more than 1 valid complaints already
        // this is a DoS protection against members sending multiple complaints with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        logger.Batch("dropping complaint from %s as we already got %d complaints",
                      member->dmn->proTxHash.ToString(), member->complaints.size());
        return false;
    }

    return true;
}

std::optional<CInv> CDKGSession::ReceiveMessage(const CDKGComplaint& qc)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    logger.Batch("received complaint from %s", qc.proTxHash.ToString());

    auto* member = GetMember(qc.proTxHash);

    if (member->complaints.size() >= 2) {
        // only relay up to 2 complaints, that's enough to let the other members know about his bad behavior
        return std::nullopt;
    }

    const uint256 hash = ::SerializeHash(qc);
    WITH_LOCK(invCs, complaints.emplace(hash, qc));
    member->complaints.emplace(hash);

    CInv inv(MSG_QUORUM_COMPLAINT, hash);

    dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, member->idx, [&](CDKGDebugMemberStatus& status) {
        status.statusBits.receivedComplaint = true;
        return true;
    });

    if (member->complaints.size() > 1) {
        // don't do any further processing if we got more than 1 complaint. we already relayed it,
        // so others know about his bad behavior
        MarkBadMember(member->idx);
        logger.Batch("%s did send multiple complaints", member->dmn->proTxHash.ToString());
        return inv;
    }

    int receivedCount = 0;
    for (const auto i : irange::range(members.size())) {
        const auto& m = members[i];
        if (qc.badMembers[i]) {
            logger.Batch("%s voted for %s to be bad", member->dmn->proTxHash.ToString(), m->dmn->proTxHash.ToString());
            m->badMemberVotes.emplace(qc.proTxHash);
            if (AreWeMember() && i == myIdx) {
                logger.Batch("%s voted for us to be bad", member->dmn->proTxHash.ToString());
            }
        }
        if (qc.complainForMembers[i]) {
            m->complaintsFromOthers.emplace(qc.proTxHash);
            m->someoneComplain = true;
            dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, m->idx, [&](CDKGDebugMemberStatus& status) {
                return status.complaintsFromMembers.emplace(member->idx).second;
            });
            if (AreWeMember() && i == myIdx) {
                logger.Batch("%s complained about us", member->dmn->proTxHash.ToString());
            }
        }
        if (!m->complaints.empty()) {
            receivedCount++;
        }
    }

    logger.Batch("received and relayed complaint. received=%d", receivedCount);
    return inv;
}

void CDKGSession::VerifyAndJustify(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    if (!AreWeMember()) {
        return;
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    std::set<uint256> justifyFor;

    for (const auto& m : members) {
        if (m->bad) {
            continue;
        }
        if (m->badMemberVotes.size() >= size_t(params.dkgBadVotesThreshold)) {
            logger.Batch("%s marked as bad as %d other members voted for this", m->dmn->proTxHash.ToString(), m->badMemberVotes.size());
            MarkBadMember(m->idx);
            continue;
        }
        if (m->complaints.empty()) {
            continue;
        }
        if (m->complaints.size() != 1) {
            logger.Batch("%s sent multiple complaints", m->dmn->proTxHash.ToString());
            MarkBadMember(m->idx);
            continue;
        }

        LOCK(invCs);
        if (const auto& qc = complaints.at(*m->complaints.begin());
                qc.complainForMembers[*myIdx]) {
            justifyFor.emplace(qc.proTxHash);
        }
    }

    logger.Flush();
    if (!justifyFor.empty()) {
        SendJustification(pendingMessages, peerman, justifyFor);
    }
}

void CDKGSession::SendJustification(CDKGPendingMessages& pendingMessages, PeerManager& peerman,
                                    const std::set<uint256>& forMembers)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    assert(AreWeMember());

    logger.Batch("sending justification for %d members", forMembers.size());

    CDKGJustification qj;
    qj.llmqType = params.type;
    qj.quorumHash = m_quorum_base_block_index->GetBlockHash();
    qj.proTxHash = myProTxHash;
    qj.contributions.reserve(forMembers.size());

    for (const uint32_t i : irange::range(members.size())) {
        const auto& m = members[i];
        if (forMembers.count(m->dmn->proTxHash) == 0) {
            continue;
        }
        logger.Batch("justifying for %s", m->dmn->proTxHash.ToString());

        CBLSSecretKey skContribution = m_sk_contributions[i];

        if (i != myIdx && ShouldSimulateError(DKGError::type::JUSTIFY_LIE)) {
            logger.Batch("lying for %s", m->dmn->proTxHash.ToString());
            skContribution.MakeNewKey();
        }

        qj.contributions.emplace_back(CDKGJustification::Contribution{i, skContribution});
    }

    if (ShouldSimulateError(DKGError::type::JUSTIFY_OMIT)) {
        logger.Batch("omitting");
        return;
    }

    qj.sig = m_mn_activeman->Sign(qj.GetSignHash(), m_use_legacy_bls);

    logger.Flush();

    dkgDebugManager.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
        status.statusBits.sentJustification = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qj, peerman);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const CDKGJustification& qj, bool& retBan) const
{
    CDKGLogger logger(*this, __func__, __LINE__);

    retBan = false;

    if (qj.quorumHash != m_quorum_base_block_index->GetBlockHash()) {
        logger.Batch("justification for wrong quorum, rejecting");
        return false;
    }

    auto* member = GetMember(qj.proTxHash);
    if (member == nullptr) {
        logger.Batch("justifier not a member of this quorum, rejecting justification");
        retBan = true;
        return false;
    }

    if (qj.contributions.empty()) {
        logger.Batch("justification with no contributions");
        retBan = true;
        return false;
    }

    std::set<size_t> contributionsSet;
    for (const auto& p : qj.contributions) {
        if (p.index > members.size()) {
            logger.Batch("invalid contribution index");
            retBan = true;
            return false;
        }

        if (!contributionsSet.emplace(p.index).second) {
            logger.Batch("duplicate contribution index");
            retBan = true;
            return false;
        }

        const auto& skShare = p.key;
        if (!skShare.IsValid()) {
            logger.Batch("invalid contribution");
            retBan = true;
            return false;
        }
    }

    if (member->justifications.size() >= 2) {
        // don't do any further processing if we got more than 1 valid justification already
        // this is a DoS protection against members sending multiple justifications with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        logger.Batch("dropping justification from %s as we already got %d justifications",
                      member->dmn->proTxHash.ToString(), member->justifications.size());
        return false;
    }

    return true;
}

std::optional<CInv> CDKGSession::ReceiveMessage(const CDKGJustification& qj)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    logger.Batch("received justification from %s", qj.proTxHash.ToString());

    auto* member = GetMember(qj.proTxHash);

    if (member->justifications.size() >= 2) {
        // only relay up to 2 justifications, that's enough to let the other members know about his bad behavior
        return std::nullopt;
    }

    const uint256 hash = ::SerializeHash(qj);
    WITH_LOCK(invCs, justifications.emplace(hash, qj));
    member->justifications.emplace(hash);

    // we always relay, even if further verification fails
    CInv inv(MSG_QUORUM_JUSTIFICATION, hash);

    dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, member->idx, [&](CDKGDebugMemberStatus& status) {
        status.statusBits.receivedJustification = true;
        return true;
    });

    if (member->justifications.size() > 1) {
        // don't do any further processing if we got more than 1 justification. we already relayed it,
        // so others know about his bad behavior
        logger.Batch("%s did send multiple justifications", member->dmn->proTxHash.ToString());
        MarkBadMember(member->idx);
        return inv;
    }

    if (member->bad) {
        // we locally determined him to be bad (sent none or more then one contributions)
        // don't give him a second chance (but we relay the justification in case other members disagree)
        return inv;
    }

    for (const auto& p : qj.contributions) {
        const auto& member2 = members[p.index];

        if (member->complaintsFromOthers.count(member2->dmn->proTxHash) == 0) {
            logger.Batch("got justification from %s for %s even though he didn't complain",
                            member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            MarkBadMember(member->idx);
        }
    }
    if (member->bad) {
        return inv;
    }

    cxxtimer::Timer t1(true);

    std::list<std::future<bool>> futures;
    for (const auto& [index, skContribution] : qj.contributions) {
        const auto& member2 = members[index];

        // watch out to not bail out before these async calls finish (they rely on valid references)
        futures.emplace_back(blsWorker.AsyncVerifyContributionShare(member2->id, receivedVvecs[member->idx], skContribution));
    }
    auto resultIt = futures.begin();
    for (const auto& [index, skContribution] : qj.contributions) {
        const auto& member2 = members[index];

        bool result = (resultIt++)->get();
        if (!result) {
            logger.Batch("  %s did send an invalid justification for %s", member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            MarkBadMember(member->idx);
        } else {
            logger.Batch("  %s justified for %s", member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            if (AreWeMember() && member2->id == myId) {
                receivedSkContributions[member->idx] = skContribution;
                member->weComplain = false;

                dkgManager.WriteVerifiedSkContribution(params.type, m_quorum_base_block_index, member->dmn->proTxHash, skContribution);
            }
            member->complaintsFromOthers.erase(member2->dmn->proTxHash);
        }
    }

    auto receivedCount = std::count_if(members.cbegin(), members.cend(), [](const auto& m){
        return !m->justifications.empty();
    });
    auto expectedCount = std::count_if(members.cbegin(), members.cend(), [](const auto& m){
        return m->someoneComplain;
    });

    logger.Batch("verified justification: received=%d/%d time=%d", receivedCount, expectedCount, t1.count());
    return inv;
}

void CDKGSession::VerifyAndCommit(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    if (!AreWeMember()) {
        return;
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    std::vector<size_t> badMembers;
    badMembers.reserve(members.size());
    std::vector<size_t> openComplaintMembers;
    openComplaintMembers.reserve(members.size());

    for (const auto& m : members) {
        if (m->bad) {
            badMembers.emplace_back(m->idx);
            continue;
        }
        if (!m->complaintsFromOthers.empty()) {
            MarkBadMember(m->idx);
            openComplaintMembers.emplace_back(m->idx);
        }
    }

    if (!badMembers.empty() || !openComplaintMembers.empty()) {
        logger.Batch("verification result:");
    }
    if (!badMembers.empty()) {
        logger.Batch("  members previously determined as bad:");
        for (const auto& idx : badMembers) {
            logger.Batch("    %s", members[idx]->dmn->proTxHash.ToString());
        }
    }
    if (!openComplaintMembers.empty()) {
        logger.Batch("  members with open complaints and now marked as bad:");
        for (const auto& idx : openComplaintMembers) {
            logger.Batch("    %s", members[idx]->dmn->proTxHash.ToString());
        }
    }

    logger.Flush();

    SendCommitment(pendingMessages, peerman);
}

void CDKGSession::SendCommitment(CDKGPendingMessages& pendingMessages, PeerManager& peerman)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    assert(AreWeMember());

    logger.Batch("sending commitment");

    CDKGPrematureCommitment qc(params);
    qc.llmqType = params.type;
    qc.quorumHash = m_quorum_base_block_index->GetBlockHash();
    qc.proTxHash = myProTxHash;

    for (const auto i : irange::range(members.size())) {
        const auto& m = members[i];
        if (!m->bad) {
            qc.validMembers[i] = true;
        }
    }

    if (qc.CountValidMembers() < params.minSize) {
        logger.Batch("not enough valid members. not sending commitment");
        return;
    }

    if (ShouldSimulateError(DKGError::type::COMMIT_OMIT)) {
        logger.Batch("omitting");
        return;
    }

    cxxtimer::Timer timerTotal(true);

    cxxtimer::Timer t1(true);
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;
    if (!dkgManager.GetVerifiedContributions(params.type, m_quorum_base_block_index, qc.validMembers, memberIndexes, vvecs, skContributions)) {
        logger.Batch("failed to get valid contributions");
        return;
    }

    BLSVerificationVectorPtr vvec = cache.BuildQuorumVerificationVector(::SerializeHash(memberIndexes), vvecs);
    if (vvec == nullptr) {
        logger.Batch("failed to build quorum verification vector");
        return;
    }
    t1.stop();

    cxxtimer::Timer t2(true);
    CBLSSecretKey skShare = cache.AggregateSecretKeys(::SerializeHash(memberIndexes), skContributions);
    if (!skShare.IsValid()) {
        logger.Batch("failed to build own secret share");
        return;
    }
    t2.stop();

    logger.Batch("pubKeyShare=%s", skShare.GetPublicKey().ToString());

    cxxtimer::Timer t3(true);
    qc.quorumPublicKey = (*vvec)[0];
    qc.quorumVvecHash = ::SerializeHash(*vvec);

    int lieType = -1;
    if (ShouldSimulateError(DKGError::type::COMMIT_LIE)) {
        lieType = GetRand<int>(/*nMax=*/5);
        logger.Batch("lying on commitment. lieType=%d", lieType);
    }

    if (lieType == 0) {
        CBLSSecretKey k;
        k.MakeNewKey();
        qc.quorumPublicKey = k.GetPublicKey();
    } else if (lieType == 1) {
        (*qc.quorumVvecHash.begin())++;
    }

    uint256 commitmentHash = BuildCommitmentHash(qc.llmqType, qc.quorumHash, qc.validMembers, qc.quorumPublicKey, qc.quorumVvecHash);

    if (lieType == 2) {
        (*commitmentHash.begin())++;
    }

    qc.sig = m_mn_activeman->Sign(commitmentHash, m_use_legacy_bls);
    qc.quorumSig = skShare.Sign(commitmentHash, m_use_legacy_bls);

    if (lieType == 3) {
        auto buf = qc.sig.ToBytes(m_use_legacy_bls);
        buf[5]++;
        qc.sig.SetBytes(buf, m_use_legacy_bls);
    } else if (lieType == 4) {
        auto buf = qc.quorumSig.ToBytes(m_use_legacy_bls);
        buf[5]++;
        qc.quorumSig.SetBytes(buf, m_use_legacy_bls);
    }

    t3.stop();
    timerTotal.stop();

    logger.Batch("built premature commitment. time1=%d, time2=%d, time3=%d, totalTime=%d",
                    t1.count(), t2.count(), t3.count(), timerTotal.count());


    logger.Flush();

    dkgDebugManager.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
        status.statusBits.sentPrematureCommitment = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc, peerman);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const CDKGPrematureCommitment& qc, bool& retBan) const
{
    CDKGLogger logger(*this, __func__, __LINE__);

    retBan = false;

    if (qc.quorumHash != m_quorum_base_block_index->GetBlockHash()) {
        logger.Batch("commitment for wrong quorum, rejecting");
        return false;
    }

    auto* member = GetMember(qc.proTxHash);
    if (member == nullptr) {
        logger.Batch("committer not a member of this quorum, rejecting premature commitment");
        retBan = true;
        return false;
    }

    if (qc.validMembers.size() != (size_t)params.size) {
        logger.Batch("invalid validMembers bitset size");
        retBan = true;
        return false;
    }

    if (qc.CountValidMembers() < params.minSize) {
        logger.Batch("invalid validMembers count. validMembersCount=%d", qc.CountValidMembers());
        retBan = true;
        return false;
    }
    if (!qc.sig.IsValid()) {
        logger.Batch("invalid membersSig");
        retBan = true;
        return false;
    }
    if (!qc.quorumSig.IsValid()) {
        logger.Batch("invalid quorumSig");
        retBan = true;
        return false;
    }

    for (const auto i : irange::range(members.size(), size_t(params.size))) {
        // cppcheck-suppress useStlAlgorithm
        if (qc.validMembers[i]) {
            retBan = true;
            logger.Batch("invalid validMembers bitset. bit %d should not be set", i);
            return false;
        }
    }

    if (member->prematureCommitments.size() >= 2) {
        // don't do any further processing if we got more than 1 valid commitment already
        // this is a DoS protection against members sending multiple commitments with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        logger.Batch("dropping commitment from %s as we already got %d commitments",
                      member->dmn->proTxHash.ToString(), member->prematureCommitments.size());
        return false;
    }

    return true;
}

std::optional<CInv> CDKGSession::ReceiveMessage(const CDKGPrematureCommitment& qc)
{
    CDKGLogger logger(*this, __func__, __LINE__);

    cxxtimer::Timer t1(true);

    logger.Batch("received premature commitment from %s. validMembers=%d", qc.proTxHash.ToString(), qc.CountValidMembers());

    auto* member = GetMember(qc.proTxHash);
    const uint256 hash = ::SerializeHash(qc);

    {
        LOCK(invCs);

        // keep track of ALL commitments but only relay valid ones (or if we couldn't build the vvec)
        // relaying is done further down
        prematureCommitments.emplace(hash, qc);
        member->prematureCommitments.emplace(hash);
    }

    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;
    BLSVerificationVectorPtr quorumVvec;
    if (dkgManager.GetVerifiedContributions(params.type, m_quorum_base_block_index, qc.validMembers, memberIndexes, vvecs, skContributions)) {
        quorumVvec = cache.BuildQuorumVerificationVector(::SerializeHash(memberIndexes), vvecs);
    }

    if (quorumVvec == nullptr) {
        logger.Batch("failed to build quorum verification vector. skipping full verification");
        // we might be the unlucky one who didn't receive all contributions, but we still have to relay
        // the premature commitment as others might be luckier
    } else {
        // we got all information that is needed to verify everything (even though we might not be a member of the quorum)
        // if any of this verification fails, we won't relay this message. This ensures that invalid messages are lost
        // in the network. Nodes relaying such invalid messages to us are not punished as they might have not known
        // all contributions. We only handle up to 2 commitments per member, so a DoS shouldn't be possible

        if ((*quorumVvec)[0] != qc.quorumPublicKey) {
            logger.Batch("calculated quorum public key does not match");
            return std::nullopt;
        }
        uint256 vvecHash = ::SerializeHash(*quorumVvec);
        if (qc.quorumVvecHash != vvecHash) {
            logger.Batch("calculated quorum vvec hash does not match");
            return std::nullopt;
        }

        CBLSPublicKey pubKeyShare = cache.BuildPubKeyShare(::SerializeHash(std::make_pair(memberIndexes, member->id)), quorumVvec, member->id);
        if (!pubKeyShare.IsValid()) {
            logger.Batch("failed to calculate public key share");
            return std::nullopt;
        }

        if (!qc.quorumSig.VerifyInsecure(pubKeyShare, qc.GetSignHash())) {
            logger.Batch("failed to verify quorumSig");
            return std::nullopt;
        }
    }

    WITH_LOCK(invCs, validCommitments.emplace(hash));

    CInv inv(MSG_QUORUM_PREMATURE_COMMITMENT, hash);

    dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, member->idx, [&](CDKGDebugMemberStatus& status) {
        status.statusBits.receivedPrematureCommitment = true;
        return true;
    });

    int receivedCount = ranges::count_if(members, [](const auto& m){ return !m->prematureCommitments.empty(); });

    t1.stop();

    logger.Batch("verified premature commitment. received=%d/%d, time=%d", receivedCount, members.size(), t1.count());
    return inv;
}

std::vector<CFinalCommitment> CDKGSession::FinalizeCommitments()
{
    if (!AreWeMember()) {
        return {};
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    using Key = std::vector<bool>;
    std::map<Key, std::vector<CDKGPrematureCommitment>> commitmentsMap;

    {
        LOCK(invCs);

        for (const auto& p : prematureCommitments) {
            const auto& qc = p.second;
            if (validCommitments.count(p.first) == 0) {
                continue;
            }

            // should have been verified before
            assert(qc.CountValidMembers() >= params.minSize);

            auto it = commitmentsMap.find(qc.validMembers);
            if (it == commitmentsMap.end()) {
                it = commitmentsMap.emplace(qc.validMembers, std::vector<CDKGPrematureCommitment>()).first;
            }

            it->second.emplace_back(qc);
        }
    }

    std::vector<CFinalCommitment> finalCommitments;
    for (const auto& p : commitmentsMap) {
        const auto& cvec = p.second;
        if (cvec.size() < size_t(params.minSize)) {
            // commitment was signed by a minority
            continue;
        }

        std::vector<CBLSId> signerIds;
        std::vector<CBLSSignature> thresholdSigs;

        const auto& first = cvec[0];

        CFinalCommitment fqc(params, first.quorumHash);
        fqc.validMembers = first.validMembers;
        fqc.quorumPublicKey = first.quorumPublicKey;
        fqc.quorumVvecHash = first.quorumVvecHash;

        const bool isQuorumRotationEnabled{IsQuorumRotationEnabled(params, m_quorum_base_block_index)};
        // TODO: always put `true` here: so far as v19 is activated, we always write BASIC now
        fqc.nVersion = CFinalCommitment::GetVersion(isQuorumRotationEnabled, DeploymentActiveAfter(m_quorum_base_block_index, Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
        fqc.quorumIndex = isQuorumRotationEnabled ? quorumIndex : 0;

        uint256 commitmentHash = BuildCommitmentHash(fqc.llmqType, fqc.quorumHash, fqc.validMembers, fqc.quorumPublicKey, fqc.quorumVvecHash);

        std::vector<CBLSSignature> aggSigs;
        std::vector<CBLSPublicKey> aggPks;
        aggSigs.reserve(cvec.size());
        aggPks.reserve(cvec.size());

        for (const auto& qc : cvec) {
            if (qc.quorumPublicKey != first.quorumPublicKey || qc.quorumVvecHash != first.quorumVvecHash) {
                logger.Batch("quorumPublicKey or quorumVvecHash does not match, skipping");
                continue;
            }

            size_t signerIndex = membersMap[qc.proTxHash];
            const auto& m = members[signerIndex];

            fqc.signers[signerIndex] = true;
            aggSigs.emplace_back(qc.sig);
            aggPks.emplace_back(m->dmn->pdmnState->pubKeyOperator.Get());

            signerIds.emplace_back(m->id);
            thresholdSigs.emplace_back(qc.quorumSig);
        }

        cxxtimer::Timer t1(true);
        fqc.membersSig = CBLSSignature::AggregateSecure(aggSigs, aggPks, commitmentHash);
        t1.stop();

        cxxtimer::Timer t2(true);
        if (!fqc.quorumSig.Recover(thresholdSigs, signerIds)) {
            logger.Batch("failed to recover quorum sig");
            continue;
        }
        t2.stop();

        cxxtimer::Timer t3(true);
        if (!fqc.Verify(m_dmnman, m_qsnapman, m_quorum_base_block_index, true)) {
            logger.Batch("failed to verify final commitment");
            continue;
        }
        t3.stop();

        finalCommitments.emplace_back(fqc);

        logger.Batch("final commitment: validMembers=%d, signers=%d, quorumPublicKey=%s, time1=%d, time2=%d, time3=%d",
                        fqc.CountValidMembers(), fqc.CountSigners(), fqc.quorumPublicKey.ToString(),
                        t1.count(), t2.count(), t3.count());
    }

    logger.Flush();

    return finalCommitments;
}

CFinalCommitment CDKGSession::FinalizeSingleCommitment()
{
    if (!AreWeMember()) {
        return {};
    }

    CDKGLogger logger(*this, __func__, __LINE__);

    std::vector<CBLSId> signerIds;
    std::vector<CBLSSignature> thresholdSigs;

    CFinalCommitment fqc(params, m_quorum_base_block_index->GetBlockHash());


    fqc.signers = {true};
    fqc.validMembers = {true};

    CBLSSecretKey sk1;
    sk1.MakeNewKey();

    fqc.quorumPublicKey = sk1.GetPublicKey();
    fqc.quorumVvecHash = {};

    // use just MN's operator public key as quorum pubkey.
    // TODO: use sk1 here instead and use recovery mechanism from shares, but that's not trivial to do
    const bool workaround_qpublic_key = true;
    if (workaround_qpublic_key) {
        fqc.quorumPublicKey = m_mn_activeman->GetPubKey();
    }
    const bool isQuorumRotationEnabled{false};
    fqc.nVersion = CFinalCommitment::GetVersion(isQuorumRotationEnabled,
                                                DeploymentActiveAfter(m_quorum_base_block_index, Params().GetConsensus(),
                                                                      Consensus::DEPLOYMENT_V19));
    fqc.quorumIndex = 0;

    uint256 commitmentHash = BuildCommitmentHash(fqc.llmqType, fqc.quorumHash, fqc.validMembers, fqc.quorumPublicKey,
                                                 fqc.quorumVvecHash);
    fqc.quorumSig = sk1.Sign(commitmentHash, m_use_legacy_bls);

    fqc.membersSig = m_mn_activeman->Sign(commitmentHash, m_use_legacy_bls);

    if (workaround_qpublic_key) {
        fqc.quorumSig = fqc.membersSig;
    }

    if (!fqc.Verify(m_dmnman, m_qsnapman, m_quorum_base_block_index, true)) {
        logger.Batch("failed to verify final commitment");
        assert(false);
    }

    logger.Batch("final commitment: validMembers=%d, signers=%d, quorumPublicKey=%s", fqc.CountValidMembers(),
                 fqc.CountSigners(), fqc.quorumPublicKey.ToString());

    logger.Flush();

    return fqc;
}

CDKGMember* CDKGSession::GetMember(const uint256& proTxHash) const
{
    auto it = membersMap.find(proTxHash);
    if (it == membersMap.end()) {
        return nullptr;
    }
    return members[it->second].get();
}

void CDKGSession::MarkBadMember(size_t idx)
{
    auto* member = members.at(idx).get();
    if (member->bad) {
        return;
    }
    dkgDebugManager.UpdateLocalMemberStatus(params.type, quorumIndex, idx, [&](CDKGDebugMemberStatus& status) {
        status.statusBits.bad = true;
        return true;
    });
    member->bad = true;
}


} // namespace llmq
