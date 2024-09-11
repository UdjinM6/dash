// Copyright (c) 2017-2023 Vincent Thiery
// Copyright (c) 2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stats/rawsender.h>

#include <netaddress.h>
#include <netbase.h>
#include <util/sock.h>
#include <util/thread.h>

RawSender::RawSender(const std::string& host, uint16_t port, uint64_t interval_ms,
                     std::optional<std::string>& error) noexcept :
    m_host{host},
    m_port{port},
    m_interval_ms{interval_ms}
{
    if (host.empty()) {
        error = "No host specified";
        return;
    }

    CNetAddr netaddr;
    if (!LookupHost(m_host, netaddr, /*fAllowLookup=*/true)) {
        error = strprintf("Unable to lookup host %s", m_host);
        return;
    }
    if (!netaddr.IsIPv4()) {
        error = strprintf("Host %s on unsupported network", m_host);
        return;
    }
    if (!CService(netaddr, port).GetSockAddr(reinterpret_cast<struct sockaddr*>(&m_server.first), &m_server.second)) {
        error = strprintf("Cannot get socket address for %s", m_host);
        return;
    }

    SOCKET hSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (hSocket == INVALID_SOCKET) {
        error = strprintf("Cannot create socket (socket() returned error %s)", NetworkErrorString(WSAGetLastError()));
        return;
    }
    m_sock = std::make_unique<Sock>(hSocket);

    if (m_interval_ms == 0) {
        LogPrintf("Send interval is zero, not starting RawSender queueing thread.\n");
    } else {
        m_interrupt.reset();
        m_thread = std::thread(&util::TraceThread, "rawsender", [this] { QueueThreadMain(); });
    }

    LogPrintf("Started %sRawSender sending messages to %s:%d\n", m_thread.joinable() ? "threaded " : "",
              m_host, m_port);
}

RawSender::~RawSender()
{
    // If there is a thread, interrupt and stop it
    if (m_thread.joinable()) {
        m_interrupt();
        m_thread.join();
    }
    // Flush queue of uncommitted messages
    QueueFlush();
    // Reset connection socket
    m_sock.reset();

    LogPrintf("Stopped RawSender instance sending messages to %s:%d. %d successes, %d failures.\n",
              m_host, m_port, m_successes, m_failures);
}

std::optional<std::string> RawSender::Send(const RawMessage& msg) noexcept
{
    // If there is a thread, append to queue
    if (m_thread.joinable()) {
        QueueAdd(msg);
        return std::nullopt;
    }
    // There isn't a queue, send directly
    return SendDirectly(msg);
}

std::optional<std::string> RawSender::SendDirectly(const RawMessage& msg) noexcept
{
    if (!m_sock) {
        m_failures++;
        return "Socket not initialized, cannot send message";
    }

    if (::sendto(m_sock->Get(), reinterpret_cast<const char*>(msg.data()),
#ifdef WIN32
                 static_cast<int>(msg.size()),
#else
                 msg.size(),
#endif // WIN32
                 /*flags=*/0, reinterpret_cast<struct sockaddr*>(&m_server.first), m_server.second) == SOCKET_ERROR) {
        m_failures++;
        return strprintf("Unable to send message to %s (sendto() returned error %s)", this->ToStringHostPort(),
                         NetworkErrorString(WSAGetLastError()));
    }

    m_successes++;
    return std::nullopt;
}

std::string RawSender::ToStringHostPort() const noexcept
{
    return strprintf("%s:%d", m_host, m_port);
}

void RawSender::QueueAdd(const RawMessage& msg) noexcept
{
    AssertLockNotHeld(cs);
    WITH_LOCK(cs, m_queue.push_back(msg));
}

void RawSender::QueueFlush() noexcept
{
    AssertLockNotHeld(cs);
    WITH_LOCK(cs, QueueFlush(m_queue));
}

void RawSender::QueueFlush(std::deque<RawMessage>& queue) noexcept
{
    while (!queue.empty()) {
        SendDirectly(queue.front());
        queue.pop_front();
    }
}

void RawSender::QueueThreadMain() noexcept
{
    AssertLockNotHeld(cs);

    while (!m_interrupt) {
        // Swap the queues to commit the existing queue of messages
        std::deque<RawMessage> queue;
        WITH_LOCK(cs, m_queue.swap(queue));

        // Flush the committed queue
        QueueFlush(queue);
        assert(queue.empty());

        if (!m_interrupt.sleep_for(std::chrono::milliseconds(m_interval_ms))) {
            return;
        }
    }
}
