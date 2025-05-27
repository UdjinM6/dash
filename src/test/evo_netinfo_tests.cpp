// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <clientversion.h>
#include <evo/netinfo.h>
#include <netbase.h>
#include <streams.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(evo_netinfo_tests, BasicTestingSetup)

const std::vector<std::pair</*input=*/std::string, /*expected_ret=*/NetInfoStatus>> vals{
    // Address and port specified
    {"1.1.1.1:9999", NetInfoStatus::Success},
    // Address specified, port should default to default P2P core
    {"1.1.1.1", NetInfoStatus::Success},
    // Non-mainnet port on mainnet
    {"1.1.1.1:9998", NetInfoStatus::BadPort},
    // Internal addresses not allowed on mainnet
    {"127.0.0.1:9999", NetInfoStatus::NotRoutable},
    // Valid IPv4 formatting but invalid IPv4 address
    {"0.0.0.0:9999", NetInfoStatus::BadAddress},
    // Port greater than uint16_t max
    {"1.1.1.1:99999", NetInfoStatus::BadInput},
    // Only IPv4 allowed
    {"[2606:4700:4700::1111]:9999", NetInfoStatus::BadType},
    // Domains are not allowed
    {"example.com:9999", NetInfoStatus::BadInput},
    // Incorrect IPv4 address
    {"1.1.1.256:9999", NetInfoStatus::BadInput},
    // Missing address
    {":9999", NetInfoStatus::BadInput},
};

void ValidateGetEntries(const CServiceList& entries, const size_t expected_size)
{
    BOOST_CHECK_EQUAL(entries.size(), expected_size);
}

BOOST_AUTO_TEST_CASE(mnnetinfo_rules)
{
    // Validate AddEntry() rules enforcement
    for (const auto& [input, expected_ret] : vals) {
        MnNetInfo netInfo;
        BOOST_CHECK_EQUAL(netInfo.AddEntry(input), expected_ret);
        if (expected_ret != NetInfoStatus::Success) {
            // An empty MnNetInfo is considered malformed
            BOOST_CHECK_EQUAL(netInfo.Validate(), NetInfoStatus::Malformed);
            BOOST_CHECK(netInfo.GetEntries().empty());
        } else {
            BOOST_CHECK_EQUAL(netInfo.Validate(), NetInfoStatus::Success);
            ValidateGetEntries(netInfo.GetEntries(), /*expected_size=*/1);
        }
    }
}

BOOST_AUTO_TEST_CASE(netinfo_ser)
{
    {
        // An empty object should only store one byte to denote it is invalid
        CDataStream ds(SER_DISK, CLIENT_VERSION);
        NetInfoEntry entry{};
        ds << entry;
        BOOST_CHECK_EQUAL(ds.size(), sizeof(uint8_t));
    }

    {
        // Reading a nonsense byte should return an empty object
        CDataStream ds(SER_DISK, CLIENT_VERSION);
        NetInfoEntry entry{};
        ds << 0xfe;
        ds >> entry;
        BOOST_CHECK(entry.IsEmpty() && !entry.IsTriviallyValid());
    }

    {
        // Reading an invalid CService should fail trivial validation and return an empty object
        CDataStream ds(SER_DISK, CLIENT_VERSION);
        NetInfoEntry entry{};
        ds << NetInfoEntry::NetInfoType::Service << CService{};
        ds >> entry;
        BOOST_CHECK(entry.IsEmpty() && !entry.IsTriviallyValid());
    }

    {
        // Reading an unrecognized type should fail trivial validation and return an empty object
        CDataStream ds(SER_DISK, CLIENT_VERSION);
        NetInfoEntry entry{};
        ds << NetInfoEntry::NetInfoType::Service << uint256{};
        ds >> entry;
        BOOST_CHECK(entry.IsEmpty() && !entry.IsTriviallyValid());
    }

    {
        // A valid CService should be constructable, readable and pass validation
        CDataStream ds(SER_DISK, CLIENT_VERSION | ADDRV2_FORMAT);
        CService service{LookupNumeric("1.1.1.1", Params().GetDefaultPort())};
        BOOST_CHECK(service.IsValid());
        NetInfoEntry entry{service}, entry2{};
        ds << NetInfoEntry::NetInfoType::Service << service;
        ds >> entry2;
        BOOST_CHECK(entry == entry2);
        BOOST_CHECK(!entry.IsEmpty() && entry.IsTriviallyValid());
        BOOST_CHECK(entry.GetAddrPort().value() == service);
    }

    {
        // NetInfoEntry should be able to read and write ADDRV2 addresses
        CService service{};
        service.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion");
        BOOST_CHECK(service.IsValid() && service.IsTor());

        CDataStream ds(SER_DISK, CLIENT_VERSION | ADDRV2_FORMAT);
        ds << NetInfoEntry::NetInfoType::Service << service;
        ds.SetVersion(CLIENT_VERSION); // Drop the explicit format flag

        NetInfoEntry entry{};
        ds >> entry;
        BOOST_CHECK(!entry.IsEmpty() && entry.IsTriviallyValid());
        BOOST_CHECK(entry.GetAddrPort().value() == service);
        ds.clear();

        NetInfoEntry entry2{};
        ds << entry;
        ds >> entry2;
        BOOST_CHECK(entry == entry2 && entry2.GetAddrPort().value() == service);
    }
}

BOOST_AUTO_TEST_CASE(netinfo_retvals)
{
    uint16_t p2p_port{Params().GetDefaultPort()};
    CService service{LookupNumeric("1.1.1.1", p2p_port)}, service2{LookupNumeric("1.1.1.2", p2p_port)};
    NetInfoEntry entry{service}, entry2{service2}, entry_empty{};

    // Check that values are correctly recorded and pass trivial validation
    BOOST_CHECK(service.IsValid());
    BOOST_CHECK(!entry.IsEmpty() && entry.IsTriviallyValid());
    BOOST_CHECK(entry.GetAddrPort().value() == service);
    BOOST_CHECK(!entry2.IsEmpty() && entry2.IsTriviallyValid());
    BOOST_CHECK(entry2.GetAddrPort().value() == service2);

    // Check that dispatch returns the expected values
    BOOST_CHECK_EQUAL(entry.GetPort(), service.GetPort());
    BOOST_CHECK_EQUAL(entry.ToString(), strprintf("CService(addr=%s, port=%u)", service.ToStringAddr(), service.GetPort()));
    BOOST_CHECK_EQUAL(entry.ToStringAddr(), service.ToStringAddr());
    BOOST_CHECK_EQUAL(entry.ToStringAddrPort(), service.ToStringAddrPort());
    BOOST_CHECK_EQUAL(service < service2, entry < entry2);

    // Check that empty/invalid entries return error messages
    BOOST_CHECK_EQUAL(entry_empty.GetPort(), 0);
    BOOST_CHECK_EQUAL(entry_empty.ToString(), "[invalid entry]");
    BOOST_CHECK_EQUAL(entry_empty.ToStringAddr(), "[invalid entry]");
    BOOST_CHECK_EQUAL(entry_empty.ToStringAddrPort(), "[invalid entry]");

    // The invalid entry type code is 0xff (highest possible value) and therefore will return as greater
    // in comparison to any valid entry
    BOOST_CHECK(entry < entry_empty);
}

bool CheckIfSerSame(const CService& lhs, const MnNetInfo& rhs)
{
    CHashWriter ss_lhs(SER_GETHASH, 0), ss_rhs(SER_GETHASH, 0);
    ss_lhs << lhs;
    ss_rhs << rhs;
    return ss_lhs.GetSHA256() == ss_rhs.GetSHA256();
}

BOOST_AUTO_TEST_CASE(cservice_compatible)
{
    // Empty values should be the same
    CService service;
    MnNetInfo netInfo;
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Valid IPv4 address, valid port
    service = LookupNumeric("1.1.1.1", 9999);
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.1:9999"), NetInfoStatus::Success);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Valid IPv4 address, default P2P port implied
    service = LookupNumeric("1.1.1.1", Params().GetDefaultPort());
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.1"), NetInfoStatus::Success);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Lookup() failure (domains not allowed), MnNetInfo should remain empty if Lookup() failed
    service = CService();
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("example.com"), NetInfoStatus::BadInput);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Validation failure (non-IPv4 not allowed), MnNetInfo should remain empty if ValidateService() failed
    service = CService();
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("[2606:4700:4700::1111]:9999"), NetInfoStatus::BadType);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));
}

BOOST_AUTO_TEST_SUITE_END()
