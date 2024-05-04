// Copyright (c) 2020-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_EDGE_H
#define BITCOIN_UTIL_EDGE_H

#include <util/sock.h>

#include <cstdint>

/* Should be in sync with SocketEventsMode */
enum class EdgeEventsMode : uint8_t {
    EPoll = 2,
    KQueue = 3
};

/**
 * A manager for abstracting logic surrounding edge-triggered socket events
 * modes like kqueue and epoll.
 */
class EdgeTriggeredEvents
{
public:
    explicit EdgeTriggeredEvents(EdgeEventsMode events_mode);
    ~EdgeTriggeredEvents();

    /* Add socket to interest list */
    bool AddSocket(SOCKET socket) const;
    /* Remove socket from interest list */
    bool RemoveSocket(SOCKET socket) const;

    bool IsValid() const { return m_valid; }

public:
    /* File descriptor used to interact with events mode */
    int m_fd{-1};

private:
    /* Instance validity flag set during construction */
    bool m_valid{false};
    /* Flag for storing selected socket events mode */
    EdgeEventsMode m_mode;
};

#endif /* BITCOIN_UTIL_EDGE_H */
