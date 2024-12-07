// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_VOTE_H
#define BITCOIN_GOVERNANCE_VOTE_H

#include <bls/bls.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <uint256.h>

class CActiveMasternodeManager;
class CBLSPublicKey;
class CDeterministicMNList;
class CGovernanceVote;
class CMasternodeSync;
class CKey;
class CKeyID;
class PeerManager;

// INTENTION OF MASTERNODES REGARDING ITEM
enum vote_outcome_enum_t : int {
    VOTE_OUTCOME_NONE = 0,
    VOTE_OUTCOME_YES,
    VOTE_OUTCOME_NO,
    VOTE_OUTCOME_ABSTAIN,
    VOTE_OUTCOME_UNKNOWN
};
template<> struct is_serializable_enum<vote_outcome_enum_t> : std::true_type {};

// SIGNAL VARIOUS THINGS TO HAPPEN:
enum vote_signal_enum_t : int {
    VOTE_SIGNAL_NONE = 0,
    VOTE_SIGNAL_FUNDING,  //   -- fund this object for it's stated amount
    VOTE_SIGNAL_VALID,    //   -- this object checks out in sentinel engine
    VOTE_SIGNAL_DELETE,   //   -- this object should be deleted from memory entirely
    VOTE_SIGNAL_ENDORSED, //   -- officially endorsed by the network somehow (delegation)
    VOTE_SIGNAL_UNKNOWN
};
template<> struct is_serializable_enum<vote_signal_enum_t> : std::true_type {};

/**
* Governance Voting
*
*   Static class for accessing governance data
*/

class CGovernanceVoting
{
public:
    static vote_outcome_enum_t ConvertVoteOutcome(const std::string& strVoteOutcome);
    static vote_signal_enum_t ConvertVoteSignal(const std::string& strVoteSignal);
    static std::string ConvertOutcomeToString(vote_outcome_enum_t nOutcome);
    static std::string ConvertSignalToString(vote_signal_enum_t nSignal);
};

//
// CGovernanceVote - Allow a masternode to vote and broadcast throughout the network
//

class CGovernanceVote
{
    friend bool operator==(const CGovernanceVote& vote1, const CGovernanceVote& vote2);

    friend bool operator<(const CGovernanceVote& vote1, const CGovernanceVote& vote2);

private:
    COutPoint masternodeOutpoint;
    uint256 nParentHash;
    vote_outcome_enum_t nVoteOutcome{VOTE_OUTCOME_NONE};
    vote_signal_enum_t nVoteSignal{VOTE_SIGNAL_NONE};
    int64_t nTime{0};

    /** Memory only. */
    bool is_bls{false};
    std::array<unsigned char, BLS_CURVE_SIG_SIZE> m_sig_BLS{};
    std::array<unsigned char, CPubKey::COMPACT_SIGNATURE_SIZE> m_sig_ECDSA{};
    const uint256 hash{0};

    void UpdateHash() const;

public:
    CGovernanceVote() = default;
    CGovernanceVote(const COutPoint& outpointMasternodeIn, const uint256& nParentHashIn, vote_signal_enum_t eVoteSignalIn, vote_outcome_enum_t eVoteOutcomeIn);

    int64_t GetTimestamp() const { return nTime; }

    vote_signal_enum_t GetSignal() const { return nVoteSignal; }

    vote_outcome_enum_t GetOutcome() const { return nVoteOutcome; }

    const uint256& GetParentHash() const { return nParentHash; }

    void SetTime(int64_t nTimeIn)
    {
        nTime = nTimeIn;
        UpdateHash();
    }

    void SetSignature(const std::vector<unsigned char>& vchSigIn)
    {
        if (vchSigIn.size() == BLS_CURVE_SIG_SIZE) {
            std::copy(vchSigIn.begin(), vchSigIn.end(), m_sig_BLS.begin());
            is_bls = true;
        } else if (vchSigIn.size() == CPubKey::COMPACT_SIGNATURE_SIZE) {
            std::copy(vchSigIn.begin(), vchSigIn.end(), m_sig_ECDSA.begin());
        }
    }

    bool CheckSignature(const CKeyID& keyID) const;
    bool Sign(const CActiveMasternodeManager& mn_activeman);
    bool CheckSignature(const CBLSPublicKey& pubKey) const;
    bool IsValid(const CDeterministicMNList& tip_mn_list, bool useVotingKey) const;
    std::string GetSignatureString() const;
    void Relay(PeerManager& peerman, const CMasternodeSync& mn_sync, const CDeterministicMNList& tip_mn_list) const;

    const COutPoint& GetMasternodeOutpoint() const { return masternodeOutpoint; }

    /**
    *   GetHash()
    *
    *   GET UNIQUE HASH WITH DETERMINISTIC VALUE OF THIS SPECIFIC VOTE
    */

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    std::string ToString(const CDeterministicMNList& tip_mn_list) const;

    template <typename Stream, typename Operation>
    inline void SerializationOpBase(Stream& s, Operation ser_action)
    {
        READWRITE(masternodeOutpoint, nParentHash, nVoteOutcome, nVoteSignal, nTime);
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        const_cast<CGovernanceVote*>(this)->SerializationOpBase(s, CSerActionSerialize());

        if (!(s.GetType() & SER_GETHASH)) {
            if (is_bls) {
                WriteCompactSize(s, BLS_CURVE_SIG_SIZE);
                ::Serialize(s, m_sig_BLS);
            } else {
                WriteCompactSize(s, CPubKey::COMPACT_SIGNATURE_SIZE);
                ::Serialize(s, m_sig_ECDSA);
            }
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        SerializationOpBase(s, CSerActionUnserialize());

        size_t size = ReadCompactSize(s);
        if (size == BLS_CURVE_SIG_SIZE) {
            ::Unserialize(s, m_sig_BLS);
            is_bls = true;
        } else if (size == CPubKey::COMPACT_SIGNATURE_SIZE) {
            ::Unserialize(s, m_sig_ECDSA);
        }
        UpdateHash();
    }
};

#endif // BITCOIN_GOVERNANCE_VOTE_H
