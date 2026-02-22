// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SUBSYSTEMSWIDGET_H
#define BITCOIN_QT_SUBSYSTEMSWIDGET_H

#include <qt/guiutil.h>

#include <QLabel>
#include <QPointer>
#include <QWidget>

#include <map>
#include <string>

class ChainLockFeed;
class ClientModel;
class CreditPoolFeed;
class InstantSendFeed;
class MasternodeFeed;
class OptionsModel;
class QuorumFeed;
namespace Ui {
class SubsystemsWidget;
} // namespace Ui

class SubsystemsWidget : public QWidget
{
    Q_OBJECT

    Ui::SubsystemsWidget* ui;

public:
    explicit SubsystemsWidget(QWidget* parent = nullptr);
    ~SubsystemsWidget() override;

    void setClientModel(ClientModel* model);

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void handleClDataChanged();
    void handleCrDataChanged();
    void handleIsDataChanged();
    void handleMnDataChanged();
    void handleQrDataChanged();
    void updateDisplayUnit(BitcoinUnit unit);

private:
    BitcoinUnit m_display_unit{BitcoinUnit::DASH};
    CAmount m_creditpool_diff{0};
    CAmount m_creditpool_limit{0};
    CAmount m_creditpool_locked{0};
    int m_quorum_base_row{-1};
    std::map<std::string, std::pair<QPointer<QLabel>, QPointer<QLabel>>> m_quorum_labels{};

    ClientModel* clientModel{nullptr};
    ChainLockFeed* m_feed_chainlock{nullptr};
    CreditPoolFeed* m_feed_creditpool{nullptr};
    InstantSendFeed* m_feed_instantsend{nullptr};
    MasternodeFeed* m_feed_masternode{nullptr};
    QuorumFeed* m_feed_quorum{nullptr};
};

#endif // BITCOIN_QT_SUBSYSTEMSWIDGET_H
