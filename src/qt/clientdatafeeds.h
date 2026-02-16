// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CLIENTDATAFEEDS_H
#define BITCOIN_QT_CLIENTDATAFEEDS_H

#include <interfaces/node.h>
#include <saltedhasher.h>

#include <qt/clientfeeds.h>
#include <qt/masternodemodel.h>
#include <qt/proposalmodel.h>

class ClientModel;

struct MasternodeData {
    bool m_valid{false};
    int m_list_height{0};
    MasternodeEntryList m_entries;
};

class MasternodeFeed : public Feed<MasternodeData> {
    Q_OBJECT

public:
    explicit MasternodeFeed(QObject* parent, ClientModel& client_model);
    ~MasternodeFeed();

    void fetch() override;

private:
    ClientModel& m_client_model;
};

struct ProposalData {
    int m_abs_vote_req{0};
    interfaces::GOV::GovernanceInfo m_gov_info;
    Proposals m_proposals;
    Uint256HashSet m_fundable_hashes;
};

class ProposalFeed : public Feed<ProposalData> {
    Q_OBJECT

public:
    explicit ProposalFeed(QObject* parent, ClientModel& client_model);
    ~ProposalFeed();

    void fetch() override;

private:
    ClientModel& m_client_model;
};

#endif // BITCOIN_QT_CLIENTDATAFEEDS_H
