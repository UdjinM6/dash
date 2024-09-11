// Copyright (c) 2014-2017 Statoshi Developers
// Copyright (c) 2017-2023 Vincent Thiery
// Copyright (c) 2020-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
Copyright (c) 2014, Rex
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the {organization} nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <stats/client.h>

#include <util/string.h>
#include <util/system.h>

#include <cmath>
#include <cstdio>
#include <random>

namespace {
/** Threshold below which a value is considered effectively zero */
static constexpr float EPSILON{0.0001f};

/** Delimiter segmenting two fully formed Statsd messages */
static constexpr char STATSD_MSG_DELIMITER{'\n'};
/** Delimiter segmenting namespaces in a Statsd key */
static constexpr char STATSD_NS_DELIMITER{'.'};
} // anonymous namespace

std::unique_ptr<StatsdClient> g_stats_client;

std::unique_ptr<StatsdClient> InitStatsClient(const ArgsManager& args)
{
    auto is_enabled = args.GetBoolArg("-statsenabled", /*fDefault=*/false);
    auto host = args.GetArg("-statshost", /*fDefault=*/"");

    if (is_enabled && host.empty()) {
        // Stats are enabled but host has not been specified, then use
        // default host. This is to preserve old behavior.
        host = DEFAULT_STATSD_HOST;
    } else if (!host.empty()) {
        // Host is specified but stats are not explcitly enabled. Assume
        // that if a host has been specified, we want stats enabled. This
        // is new behaviour and will substitute old behaviour in a future
        // release.
        is_enabled = true;
    }

    auto sanitize_string = [](std::string& string){
        // Remove key delimiters from the front and back as they're added back
        // in formatting
        if (!string.empty()) {
            if (string.front() == STATSD_NS_DELIMITER) string.erase(string.begin());
            if (string.back() == STATSD_NS_DELIMITER) string.pop_back();
        }
    };

    // Get our suffix and if we get nothing, try again with the deprecated
    // argument. If we still get nothing, that's fine, suffixes are optional.
    auto suffix = args.GetArg("-statssuffix", DEFAULT_STATSD_SUFFIX);
    if (suffix.empty()) {
        suffix = args.GetArg("-statshostname", DEFAULT_STATSD_SUFFIX);
    } else {
        // We restrict sanitization logic to our newly added arguments to
        // prevent breaking changes.
        sanitize_string(suffix);
    }

    return std::make_unique<StatsdClient>(
        host,
        args.GetArg("-statsport", DEFAULT_STATSD_PORT),
        args.GetArg("-statsbatchsize", DEFAULT_STATSD_BATCH_SIZE),
        args.GetArg("-statsduration", DEFAULT_STATSD_DURATION),
        args.GetArg("-statsns", DEFAULT_STATSD_NAMESPACE),
        suffix,
        is_enabled
    );
}

StatsdClient::StatsdClient(const std::string& host, uint16_t port, uint64_t batch_size,
                           uint64_t interval_ms, const std::string& ns, const std::string& suffix,
                           bool enabled) :
    m_ns{ns},
    m_suffix{[suffix](){return !suffix.empty() ? STATSD_NS_DELIMITER + suffix : suffix;}()}
{
    if (!enabled) {
        LogPrintf("Transmitting stats are disabled, will not init StatsdClient\n");
        return;
    }

    std::optional<std::string> error;
    m_sender = std::make_unique<RawSender>(host, port,
                                           std::make_pair(batch_size, static_cast<uint8_t>(STATSD_MSG_DELIMITER)),
                                           interval_ms, error);
    if (error.has_value()) {
        LogPrintf("ERROR: %s, cannot initialize StatsdClient.\n", error.value());
        m_sender.reset();
        return;
    }

    LogPrintf("StatsdClient initialized to transmit stats to %s:%d\n", host, port);
}

bool StatsdClient::dec(const std::string& key, float sample_rate) { return count(key, -1, sample_rate); }

bool StatsdClient::inc(const std::string& key, float sample_rate) { return count(key, 1, sample_rate); }

bool StatsdClient::count(const std::string& key, int64_t value, float sample_rate)
{
    return send(key, value, "c", sample_rate);
}

bool StatsdClient::gauge(const std::string& key, int64_t value, float sample_rate)
{
    return send(key, value, "g", sample_rate);
}

bool StatsdClient::gaugeDouble(const std::string& key, double value, float sample_rate)
{
    return sendDouble(key, value, "g", sample_rate);
}

bool StatsdClient::timing(const std::string& key, int64_t ms, float sample_rate)
{
    return send(key, ms, "ms", sample_rate);
}

template <typename T1>
bool StatsdClient::Send(const std::string& key, T1 value, const std::string& type, float sample_rate)
{
    if (!m_sender) {
        return false;
    }

    // Determine if we should send the message at all but claim that we did even if we don't
    sample_rate = std::clamp(sample_rate, 0.f, 1.f);
    bool always_send = std::fabs(sample_rate - 1.f) < EPSILON;
    bool never_send  = std::fabs(sample_rate) < EPSILON;
    if (never_send || (!always_send &&
        WITH_LOCK(cs, return sample_rate < std::uniform_real_distribution<float>(0.f, 1.f)(insecure_rand)))) {
        return true;
    }

    // Construct the message and if our message isn't always-send, report the sample rate
    std::string buf{strprintf("%s%s%s:%s|%s", m_ns, key, m_suffix, ToString(value), type)};
    if (sample_rate < 1.f) {
        buf += strprintf("|@%.2f", sample_rate);
    }

    // Send it and report an error if we encounter one
    if (auto error = m_sender->Send(buf); error.has_value()) {
        LogPrintf("ERROR: %s.\n", error.value());
        return false;
    }

    return true;
}
