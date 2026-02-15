// Copyright (c) 2016-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEFEEDS_H
#define BITCOIN_QT_MASTERNODEFEEDS_H

#include <qt/clientfeeds.h>
#include <qt/masternodemodel.h>

class ClientModel;

struct MasternodeData {
    int m_list_height{0};
    MasternodeEntryList m_entries;
    bool m_valid{false};
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

#endif // BITCOIN_QT_MASTERNODEFEEDS_H
