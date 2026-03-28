#include <catch2/catch_test_macros.hpp>

#include "connection_manager.h"
#include "relay_client.h"

TEST_CASE("ConnectionManager derives connection cards from per-link stats", "[connections]") {
    telemy::PerLinkSnapshot per_link;
    per_link.available = true;
    per_link.conn_count = 2;

    telemy::PerLinkStats mobile;
    mobile.addr = "198.51.100.99:38201";
    mobile.asn_org = "T-Mobile USA";
    mobile.share_pct = 60.0;
    mobile.last_ms_ago = 400;
    mobile.uptime_s = 120;
    per_link.links.push_back(mobile);

    telemy::PerLinkStats wired;
    wired.addr = "192.0.2.44:45032";
    wired.asn_org = "Google Fiber Inc.";
    wired.share_pct = 40.0;
    wired.last_ms_ago = 2600;
    wired.uptime_s = 110;
    per_link.links.push_back(wired);

    telemy::RelayStats relay_stats;
    relay_stats.available = true;
    relay_stats.bitrate_kbps = 5000;

    telemy::ConnectionManager manager;
    manager.Update(per_link, &relay_stats);

    const telemy::ConnectionSnapshot snapshot = manager.CurrentSnapshot();
    REQUIRE(snapshot.available);
    REQUIRE(snapshot.items.size() == 2);

    CHECK(snapshot.items[0].name == "T-Mobile");
    CHECK(snapshot.items[0].type == "Cellular");
    CHECK(snapshot.items[0].signal == 4);
    CHECK(snapshot.items[0].bitrate_kbps == 3000);
    CHECK(snapshot.items[0].status == "connected");

    CHECK(snapshot.items[1].name == "Google Fiber");
    CHECK(snapshot.items[1].type == "Ethernet");
    CHECK(snapshot.items[1].signal == 3);
    CHECK(snapshot.items[1].bitrate_kbps == 2000);
    CHECK(snapshot.items[1].status == "degraded");
}

TEST_CASE("ConnectionManager clears when no per-link data is available", "[connections]") {
    telemy::ConnectionManager manager;
    telemy::PerLinkSnapshot per_link;

    manager.Update(per_link, nullptr);

    const telemy::ConnectionSnapshot snapshot = manager.CurrentSnapshot();
    CHECK_FALSE(snapshot.available);
    CHECK(snapshot.items.empty());
}
