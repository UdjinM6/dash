// Copyright (c) 2015-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <httpserver.h>

#include <chainparamsbase.h>
#include <netbase.h>
#include <node/interface_ui.h>
#include <rpc/protocol.h> // For HTTP status codes
#include <shutdown.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/translation.h>

#include <cstdio>
#include <deque>
#include <string>

#include <sys/types.h>

#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#include <support/events.h>

#include <thread>
#include <condition_variable>

/** Maximum size of http request (request line + headers) */
static const size_t MAX_HEADERS_SIZE = 8192;

/** HTTP request work item */
class HTTPWorkItem final : public HTTPClosure
{
public:
    HTTPWorkItem(std::unique_ptr<HTTPRequest> _req, const std::string &_path, const HTTPRequestHandler& _func):
        req(std::move(_req)), path(_path), func(_func)
    {
    }
    void operator()() override
    {
        func(req.get(), path);
    }

    std::unique_ptr<HTTPRequest> req;

private:
    std::string path;
    HTTPRequestHandler func;
};

/** Simple work queue for distributing work over multiple threads.
 * Work items are simply callable objects.
 */
template <typename WorkItem>
class WorkQueue
{
private:
    Mutex cs;
    std::condition_variable cond GUARDED_BY(cs);
    std::deque<std::unique_ptr<WorkItem>> queue GUARDED_BY(cs);
    std::deque<std::unique_ptr<WorkItem>> external_queue GUARDED_BY(cs);
    bool running GUARDED_BY(cs);
    const size_t maxDepth;
    const size_t m_external_depth;

public:
    explicit WorkQueue(size_t _maxDepth, size_t external_depth) : running(true),
                                 maxDepth(_maxDepth), m_external_depth(external_depth)
    {
    }
    /** Precondition: worker threads have all stopped (they have been joined).
     */
    ~WorkQueue()
    {
    }
    /** Enqueue a work item */
    bool Enqueue(WorkItem* item, bool is_external) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        if (!running) {
            return false;
        }
        if (is_external) {
            if (external_queue.size() >= m_external_depth) return false;
            external_queue.emplace_back(std::unique_ptr<WorkItem>(item));
        } else {
            if (queue.size() >= maxDepth) return false;
            queue.emplace_back(std::unique_ptr<WorkItem>(item));
        }
        cond.notify_one();
        return true;
    }
    /** Thread function */
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        while (true) {
            std::unique_ptr<WorkItem> i;
            {
                WAIT_LOCK(cs, lock);
                while (running && external_queue.empty() && queue.empty())
                    cond.wait(lock);
                if (!running && external_queue.empty() && queue.empty())
                    break;
                if (!queue.empty()) {
                    i = std::move(queue.front());
                    queue.pop_front();
                } else {
                    i = std::move(external_queue.front());
                    external_queue.pop_front();
                    LogPrintf("HTTP: Calling handler for external user...\n");
                }
            }
            (*i)();
        }
    }
    /** Interrupt and exit loops */
    void Interrupt() EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        running = false;
        cond.notify_all();
    }
};

struct HTTPPathHandler
{
    HTTPPathHandler(std::string _prefix, bool _exactMatch, HTTPRequestHandler _handler):
        prefix(_prefix), exactMatch(_exactMatch), handler(_handler)
    {
    }
    std::string prefix;
    bool exactMatch;
    HTTPRequestHandler handler;
};

/** HTTP module state */

//! libevent event loop
static struct event_base* eventBase = nullptr;
//! HTTP server
static struct evhttp* eventHTTP = nullptr;
//! List of subnets to allow RPC connections from
static std::vector<CSubNet> rpc_allow_subnets;
//! Work queue for handling longer requests off the event loop thread
static std::unique_ptr<WorkQueue<HTTPClosure>> g_work_queue{nullptr};
//! List of 'external' RPC users (global variable, used by httprpc)
std::vector<std::string> g_external_usernames;
//! Handlers for (sub)paths
static Mutex g_httppathhandlers_mutex;
static std::vector<HTTPPathHandler> pathHandlers GUARDED_BY(g_httppathhandlers_mutex);
//! Bound listening sockets
static std::vector<evhttp_bound_socket *> boundSockets;

/** Check if a network address is allowed to access the HTTP server */
static bool ClientAllowed(const CNetAddr& netaddr)
{
    if (!netaddr.IsValid())
        return false;
    for(const CSubNet& subnet : rpc_allow_subnets)
        if (subnet.Match(netaddr))
            return true;
    return false;
}

/** Initialize ACL list for HTTP server */
static bool InitHTTPAllowList()
{
    rpc_allow_subnets.clear();
    rpc_allow_subnets.push_back(CSubNet{LookupHost("127.0.0.1", false).value(), 8});  // always allow IPv4 local subnet
    rpc_allow_subnets.push_back(CSubNet{LookupHost("::1", false).value()});  // always allow IPv6 localhost
    for (const std::string& strAllow : gArgs.GetArgs("-rpcallowip")) {
        const CSubNet subnet{LookupSubNet(strAllow)};
        if (!subnet.IsValid()) {
            uiInterface.ThreadSafeMessageBox(
                strprintf(Untranslated("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24)."), strAllow),
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
        rpc_allow_subnets.push_back(subnet);
    }
    std::string strAllowed;
    for (const CSubNet& subnet : rpc_allow_subnets)
        strAllowed += subnet.ToString() + " ";
    LogPrint(BCLog::HTTP, "Allowing HTTP connections from: %s\n", strAllowed);
    return true;
}

/** HTTP request method as string - use for logging only */
std::string RequestMethodString(HTTPRequest::RequestMethod m)
{
    switch (m) {
    case HTTPRequest::GET:
        return "GET";
    case HTTPRequest::POST:
        return "POST";
    case HTTPRequest::HEAD:
        return "HEAD";
    case HTTPRequest::PUT:
        return "PUT";
    case HTTPRequest::UNKNOWN:
        return "unknown";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

/** HTTP request callback */
static void http_request_cb(struct evhttp_request* req, void* arg)
{
    // Disable reading to work around a libevent bug, fixed in 2.2.0.
    if (event_get_version_number() >= 0x02010600 && event_get_version_number() < 0x02020001) {
        evhttp_connection* conn = evhttp_request_get_connection(req);
        if (conn) {
            bufferevent* bev = evhttp_connection_get_bufferevent(conn);
            if (bev) {
                bufferevent_disable(bev, EV_READ);
            }
        }
    }
    auto hreq{std::make_unique<HTTPRequest>(req)};

    // Early address-based allow check
    if (!ClientAllowed(hreq->GetPeer())) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Client network is not allowed RPC access\n",
                 hreq->GetPeer().ToStringAddrPort());
        hreq->WriteReply(HTTP_FORBIDDEN);
        return;
    }

    // Early reject unknown HTTP methods
    if (hreq->GetRequestMethod() == HTTPRequest::UNKNOWN) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Unknown HTTP request method\n",
                 hreq->GetPeer().ToStringAddrPort());
        hreq->WriteReply(HTTP_BAD_METHOD);
        return;
    }

    LogPrint(BCLog::HTTP, "Received a %s request for %s from %s\n",
             RequestMethodString(hreq->GetRequestMethod()), SanitizeString(hreq->GetURI(), SAFE_CHARS_URI).substr(0, 100), hreq->GetPeer().ToStringAddrPort());

    // Find registered handler for prefix
    std::string strURI = hreq->GetURI();
    std::string path;
    LOCK(g_httppathhandlers_mutex);
    std::vector<HTTPPathHandler>::const_iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::const_iterator iend = pathHandlers.end();
    for (; i != iend; ++i) {
        bool match = false;
        if (i->exactMatch)
            match = (strURI == i->prefix);
        else
            match = (strURI.substr(0, i->prefix.size()) == i->prefix);
        if (match) {
            path = strURI.substr(i->prefix.size());
            break;
        }
    }
    const bool is_external_request = [&hreq]() -> bool {
        if (g_external_usernames.empty()) return false;

        const std::string strAuth = hreq->GetHeader("authorization").second;
        if (strAuth.substr(0, 6) != "Basic ")
            return false;

        std::string strUserPass64 = TrimString(strAuth.substr(6));
        auto opt_strUserPass = DecodeBase64(strUserPass64);
        if (!opt_strUserPass.has_value()) return false;
        auto it = std::find(opt_strUserPass->begin(), opt_strUserPass->end(), ':');
        if (it == opt_strUserPass->end()) return false;
        const std::string username = std::string(opt_strUserPass->begin(), it);
        return find(g_external_usernames.begin(), g_external_usernames.end(), username) != g_external_usernames.end();
    }();

    // Dispatch to worker thread
    if (i != iend) {
        auto item{std::make_unique<HTTPWorkItem>(std::move(hreq), path, i->handler)};
        assert(g_work_queue);

        if (g_work_queue->Enqueue(item.get(), is_external_request)) {
            item.release(); /* if true, queue took ownership */
        } else {
            if (is_external_request)
            {
                LogPrintf("WARNING: request rejected because http work queue depth of externals exceeded, it can be increased with the -rpcexternalworkqueue= setting\n");
                item->req->WriteReply(HTTP_SERVICE_UNAVAILABLE, "Work queue depth of externals exceeded");
            } else {
                LogPrintf("WARNING: request rejected because http work queue depth exceeded, it can be increased with the -rpcworkqueue= setting\n");
                item->req->WriteReply(HTTP_SERVICE_UNAVAILABLE, "Work queue depth exceeded");
            }
        }
    } else {
        hreq->WriteReply(HTTP_NOT_FOUND);
    }
}

/** Callback to reject HTTP requests after shutdown. */
static void http_reject_request_cb(struct evhttp_request* req, void*)
{
    LogPrint(BCLog::HTTP, "Rejecting request while shutting down\n");
    evhttp_send_error(req, HTTP_SERVUNAVAIL, nullptr);
}

/** Event dispatcher thread */
static void ThreadHTTP(struct event_base* base)
{
    util::ThreadRename("http");
    LogPrint(BCLog::HTTP, "Entering http event loop\n");
    event_base_dispatch(base);
    // Event loop will be interrupted by InterruptHTTPServer()
    LogPrint(BCLog::HTTP, "Exited http event loop\n");
}

/** Bind HTTP server to specified addresses */
static bool HTTPBindAddresses(struct evhttp* http)
{
    uint16_t http_port{static_cast<uint16_t>(gArgs.GetIntArg("-rpcport", BaseParams().RPCPort()))};
    std::vector<std::pair<std::string, uint16_t>> endpoints;

    // Determine what addresses to bind to
    if (!(gArgs.IsArgSet("-rpcallowip") && gArgs.IsArgSet("-rpcbind"))) { // Default to loopback if not allowing external IPs
        endpoints.push_back(std::make_pair("::1", http_port));
        endpoints.push_back(std::make_pair("127.0.0.1", http_port));
        if (gArgs.IsArgSet("-rpcallowip")) {
            LogPrintf("WARNING: option -rpcallowip was specified without -rpcbind; this doesn't usually make sense\n");
        }
        if (gArgs.IsArgSet("-rpcbind")) {
            LogPrintf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
        }
    } else if (gArgs.IsArgSet("-rpcbind")) { // Specific bind address
        for (const std::string& strRPCBind : gArgs.GetArgs("-rpcbind")) {
            uint16_t port{http_port};
            std::string host;
            SplitHostPort(strRPCBind, port, host);
            endpoints.push_back(std::make_pair(host, port));
        }
    } else { // No specific bind address specified, bind to any
        endpoints.push_back(std::make_pair("::", http_port));
        endpoints.push_back(std::make_pair("0.0.0.0", http_port));
    }

    // Bind addresses
    for (std::vector<std::pair<std::string, uint16_t> >::iterator i = endpoints.begin(); i != endpoints.end(); ++i) {
        LogPrintf("Binding RPC on address %s port %i\n", i->first, i->second);
        evhttp_bound_socket *bind_handle = evhttp_bind_socket_with_handle(http, i->first.empty() ? nullptr : i->first.c_str(), i->second);
        if (bind_handle) {
            const std::optional<CNetAddr> addr{LookupHost(i->first, false)};
            if (i->first.empty() || (addr.has_value() && addr->IsBindAny())) {
                LogPrintf("WARNING: the RPC server is not safe to expose to untrusted networks such as the public internet\n");
            }
            boundSockets.push_back(bind_handle);
        } else {
            LogPrintf("Binding RPC on address %s port %i failed.\n", i->first, i->second);
        }
    }
    return !boundSockets.empty();
}

/** Simple wrapper to set thread name and run work queue */
static void HTTPWorkQueueRun(WorkQueue<HTTPClosure>* queue, int worker_num)
{
    util::ThreadRename(strprintf("httpworker.%i", worker_num));
    queue->Run();
}

/** libevent event log callback */
static void libevent_log_cb(int severity, const char *msg)
{
    BCLog::Level level;
    switch (severity) {
    case EVENT_LOG_DEBUG:
        level = BCLog::Level::Debug;
        break;
    case EVENT_LOG_MSG:
        level = BCLog::Level::Info;
        break;
    case EVENT_LOG_WARN:
        level = BCLog::Level::Warning;
        break;
    default: // EVENT_LOG_ERR and others are mapped to error
        level = BCLog::Level::Error;
        break;
    }
    LogPrintLevel(BCLog::LIBEVENT, level, "%s\n", msg);
}

bool InitHTTPServer()
{
    if (!InitHTTPAllowList())
        return false;

    // Redirect libevent's logging to our own log
    event_set_log_callback(&libevent_log_cb);
    // Update libevent's log handling.
    UpdateHTTPServerLogging(LogInstance().WillLogCategory(BCLog::LIBEVENT));

#ifdef WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    raii_event_base base_ctr = obtain_event_base();

    /* Create a new evhttp object to handle requests. */
    raii_evhttp http_ctr = obtain_evhttp(base_ctr.get());
    struct evhttp* http = http_ctr.get();
    if (!http) {
        LogPrintf("couldn't create evhttp. Exiting.\n");
        return false;
    }

    evhttp_set_timeout(http, gArgs.GetIntArg("-rpcservertimeout", DEFAULT_HTTP_SERVER_TIMEOUT));
    evhttp_set_max_headers_size(http, MAX_HEADERS_SIZE);
    evhttp_set_max_body_size(http, MAX_SIZE);
    evhttp_set_gencb(http, http_request_cb, nullptr);

    if (!HTTPBindAddresses(http)) {
        LogPrintf("Unable to bind any endpoint for RPC server\n");
        return false;
    }

    LogPrint(BCLog::HTTP, "Initialized HTTP server\n");
    int workQueueDepth = std::max((long)gArgs.GetIntArg("-rpcworkqueue", DEFAULT_HTTP_WORKQUEUE), 1L);
    int workQueueDepthExternal = 0;
    if (const std::string rpc_externaluser{gArgs.GetArg("-rpcexternaluser", "")}; !rpc_externaluser.empty()) {
        workQueueDepthExternal = std::max((long)gArgs.GetIntArg("-rpcexternalworkqueue", DEFAULT_HTTP_WORKQUEUE), 1L);
        g_external_usernames = SplitString(rpc_externaluser, ',');
    }
    LogPrintfCategory(BCLog::HTTP, "creating work queue of depth %d external_depth %d\n", workQueueDepth, workQueueDepthExternal);
    g_work_queue = std::make_unique<WorkQueue<HTTPClosure>>(workQueueDepth, workQueueDepthExternal);
    // transfer ownership to eventBase/HTTP via .release()
    eventBase = base_ctr.release();
    eventHTTP = http_ctr.release();
    return true;
}

void UpdateHTTPServerLogging(bool enable) {
    if (enable) {
        event_enable_debug_logging(EVENT_DBG_ALL);
    } else {
        event_enable_debug_logging(EVENT_DBG_NONE);
    }
}

static std::thread g_thread_http;
static std::vector<std::thread> g_thread_http_workers;

void StartHTTPServer()
{
    LogPrint(BCLog::HTTP, "Starting HTTP server\n");
    int rpcThreads = std::max((long)gArgs.GetIntArg("-rpcthreads", DEFAULT_HTTP_THREADS), 1L);
    LogPrintfCategory(BCLog::HTTP, "starting %d worker threads\n", rpcThreads);
    g_thread_http = std::thread(ThreadHTTP, eventBase);

    for (int i = 0; i < rpcThreads; i++) {
        g_thread_http_workers.emplace_back(HTTPWorkQueueRun, g_work_queue.get(), i);
    }
}

void InterruptHTTPServer()
{
    LogPrint(BCLog::HTTP, "Interrupting HTTP server\n");
    if (eventHTTP) {
        // Reject requests on current connections
        evhttp_set_gencb(eventHTTP, http_reject_request_cb, nullptr);
    }
    if (g_work_queue) {
        g_work_queue->Interrupt();
    }
}

void StopHTTPServer()
{
    LogPrint(BCLog::HTTP, "Stopping HTTP server\n");
    if (g_work_queue) {
        LogPrint(BCLog::HTTP, "Waiting for HTTP worker threads to exit\n");
        for (auto& thread : g_thread_http_workers) {
            thread.join();
        }
        g_thread_http_workers.clear();
    }
    // Unlisten sockets, these are what make the event loop running, which means
    // that after this and all connections are closed the event loop will quit.
    for (evhttp_bound_socket *socket : boundSockets) {
        evhttp_del_accept_socket(eventHTTP, socket);
    }
    boundSockets.clear();
    if (eventBase) {
        LogPrint(BCLog::HTTP, "Waiting for HTTP event thread to exit\n");
        if (g_thread_http.joinable()) g_thread_http.join();
    }
    if (eventHTTP) {
        evhttp_free(eventHTTP);
        eventHTTP = nullptr;
    }
    if (eventBase) {
        event_base_free(eventBase);
        eventBase = nullptr;
    }
    g_work_queue.reset();
    LogPrint(BCLog::HTTP, "Stopped HTTP server\n");
}

struct event_base* EventBase()
{
    return eventBase;
}

static void httpevent_callback_fn(evutil_socket_t, short, void* data)
{
    // Static handler: simply call inner handler
    HTTPEvent *self = static_cast<HTTPEvent*>(data);
    self->handler();
    if (self->deleteWhenTriggered)
        delete self;
}

HTTPEvent::HTTPEvent(struct event_base* base, bool _deleteWhenTriggered, const std::function<void()>& _handler):
    deleteWhenTriggered(_deleteWhenTriggered), handler(_handler)
{
    ev = event_new(base, -1, 0, httpevent_callback_fn, this);
    assert(ev);
}
HTTPEvent::~HTTPEvent()
{
    event_free(ev);
}
void HTTPEvent::trigger(struct timeval* tv)
{
    if (tv == nullptr)
        event_active(ev, 0, 0); // immediately trigger event in main thread
    else
        evtimer_add(ev, tv); // trigger after timeval passed
}
HTTPRequest::HTTPRequest(struct evhttp_request* _req, bool _replySent) : req(_req), replySent(_replySent)
{
}

HTTPRequest::~HTTPRequest()
{
    if (!replySent) {
        // Keep track of whether reply was sent to avoid request leaks
        LogPrintf("%s: Unhandled request\n", __func__);
        WriteReply(HTTP_INTERNAL_SERVER_ERROR, "Unhandled request");
    }
    // evhttpd cleans up the request, as long as a reply was sent.
}

std::pair<bool, std::string> HTTPRequest::GetHeader(const std::string& hdr) const
{
    const struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    assert(headers);
    const char* val = evhttp_find_header(headers, hdr.c_str());
    if (val)
        return std::make_pair(true, val);
    else
        return std::make_pair(false, "");
}

std::string HTTPRequest::ReadBody()
{
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (!buf)
        return "";
    size_t size = evbuffer_get_length(buf);
    /** Trivial implementation: if this is ever a performance bottleneck,
     * internal copying can be avoided in multi-segment buffers by using
     * evbuffer_peek and an awkward loop. Though in that case, it'd be even
     * better to not copy into an intermediate string but use a stream
     * abstraction to consume the evbuffer on the fly in the parsing algorithm.
     */
    const char* data = (const char*)evbuffer_pullup(buf, size);
    if (!data) // returns nullptr in case of empty buffer
        return "";
    std::string rv(data, size);
    evbuffer_drain(buf, size);
    return rv;
}

void HTTPRequest::WriteHeader(const std::string& hdr, const std::string& value)
{
    struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
    assert(headers);
    evhttp_add_header(headers, hdr.c_str(), value.c_str());
}

/** Closure sent to main thread to request a reply to be sent to
 * a HTTP request.
 * Replies must be sent in the main loop in the main http thread,
 * this cannot be done from worker threads.
 */
void HTTPRequest::WriteReply(int nStatus, const std::string& strReply)
{
    assert(!replySent && req);
    if (ShutdownRequested()) {
        WriteHeader("Connection", "close");
    }
    // Send event to main http thread to send reply message
    struct evbuffer* evb = evhttp_request_get_output_buffer(req);
    assert(evb);
    evbuffer_add(evb, strReply.data(), strReply.size());
    auto req_copy = req;
    HTTPEvent* ev = new HTTPEvent(eventBase, true, [req_copy, nStatus]{
        evhttp_send_reply(req_copy, nStatus, nullptr, nullptr);
        // Re-enable reading from the socket. This is the second part of the libevent
        // workaround above.
        if (event_get_version_number() >= 0x02010600 && event_get_version_number() < 0x02020001) {
            evhttp_connection* conn = evhttp_request_get_connection(req_copy);
            if (conn) {
                bufferevent* bev = evhttp_connection_get_bufferevent(conn);
                if (bev) {
                    bufferevent_enable(bev, EV_READ | EV_WRITE);
                }
            }
        }
    });
    ev->trigger(nullptr);
    replySent = true;
    req = nullptr; // transferred back to main thread
}

CService HTTPRequest::GetPeer() const
{
    evhttp_connection* con = evhttp_request_get_connection(req);
    CService peer;
    if (con) {
        // evhttp retains ownership over returned address string
        const char* address = "";
        uint16_t port = 0;

#ifdef HAVE_EVHTTP_CONNECTION_GET_PEER_CONST_CHAR
        evhttp_connection_get_peer(con, &address, &port);
#else
        evhttp_connection_get_peer(con, (char**)&address, &port);
#endif // HAVE_EVHTTP_CONNECTION_GET_PEER_CONST_CHAR

        peer = MaybeFlipIPv6toCJDNS(LookupNumeric(address, port));
    }
    return peer;
}

std::string HTTPRequest::GetURI() const
{
    return evhttp_request_get_uri(req);
}

HTTPRequest::RequestMethod HTTPRequest::GetRequestMethod() const
{
    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_GET:
        return GET;
    case EVHTTP_REQ_POST:
        return POST;
    case EVHTTP_REQ_HEAD:
        return HEAD;
    case EVHTTP_REQ_PUT:
        return PUT;
    default:
        return UNKNOWN;
    }
}

void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler)
{
    LogPrint(BCLog::HTTP, "Registering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
    LOCK(g_httppathhandlers_mutex);
    pathHandlers.push_back(HTTPPathHandler(prefix, exactMatch, handler));
}

void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch)
{
    LOCK(g_httppathhandlers_mutex);
    std::vector<HTTPPathHandler>::iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::iterator iend = pathHandlers.end();
    for (; i != iend; ++i)
        if (i->prefix == prefix && i->exactMatch == exactMatch)
            break;
    if (i != iend)
    {
        LogPrint(BCLog::HTTP, "Unregistering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
        pathHandlers.erase(i);
    }
}
