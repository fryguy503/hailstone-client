// A window close request's camp across a zone change (app/close_camp.h).
#include <doctest/doctest.h>

#include "app/close_camp.h"

using namespace hailstone;
using S = net::ConnectionState;

TEST_CASE("close camp: a zone change that ends the close request's camp camps again in the next zone") {
    app::CloseCamp c;
    CHECK_FALSE(c.update(S::InZone, true)); // nothing asked
    c.camped();
    CHECK_FALSE(c.update(S::Camping, true));
    CHECK_FALSE(c.update(S::Camping, true));
    // #zone, a zone line, a gate: the camp ends with the zone change.
    CHECK_FALSE(c.update(S::Zoning, false));
    CHECK(c.waiting());
    for (S s : {S::WorldConnecting, S::WorldAuth, S::EnteringWorld, S::ZoneConnecting, S::ZoneEntering})
        CHECK_FALSE(c.update(s, false));
    CHECK_FALSE(c.update(S::InZone, false)); // in the zone, not shown yet (loading, the add-on check)
    CHECK(c.update(S::InZone, true));        // shown: the close request again
    CHECK_FALSE(c.waiting());
    CHECK_FALSE(c.update(S::InZone, true)); // once
}

TEST_CASE("close camp: a camp the player ended, or that ended the client's way, is not made again") {
    app::CloseCamp stood;
    stood.camped();
    CHECK_FALSE(stood.update(S::Camping, true));
    CHECK_FALSE(stood.update(S::InZone, true)); // stood up, a second /camp, a refused camp
    CHECK_FALSE(stood.update(S::Zoning, false));
    CHECK_FALSE(stood.update(S::InZone, true));

    app::CloseCamp out;
    out.camped();
    CHECK_FALSE(out.update(S::Camping, true));
    CHECK_FALSE(out.update(S::Disconnected, false)); // camped out, or a link death
    CHECK_FALSE(out.waiting());
    CHECK_FALSE(out.update(S::InZone, true));

    app::CloseCamp lost;
    lost.camped();
    CHECK_FALSE(lost.update(S::Zoning, false));
    CHECK_FALSE(lost.update(S::Disconnected, false)); // the zone change failed: no camp to make
    CHECK_FALSE(lost.update(S::InZone, true));

    app::CloseCamp reset;
    reset.camped();
    reset.reset();
    CHECK_FALSE(reset.update(S::Zoning, false));
    CHECK_FALSE(reset.update(S::InZone, true));
}
