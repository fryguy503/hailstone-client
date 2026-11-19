#pragma once

// A window close request in game is a /camp desktop (ClientApp::window_close_request, HandleWindowCloseRequest). A
// zone change ends a camp in the original: EnterZone's ResetClientStateForZone(1) @0x5319f0 clears the camp's time
// (0xddf8a0) and its kind (0xdd2664), silently, so the client stays open in the next zone. Hailstone (a deliberate
// difference, docs/mechanics.md "Camp"): a close request's camp that a zone change cut off camps again once the
// next zone is entered, so closing the window still ends the client. A camp the player ended (standing, a second
// /camp) is not started again.

#include "net/game_connection.h"

namespace hailstone::app {

class CloseCamp {
public:
    // The close request's /camp desktop was asked for.
    void camped() { phase_ = Phase::Camping; }
    // Each frame: the connection's state, and whether the player is in the world and may act (the zone shown).
    // True when the close request is to be made again (a /camp desktop in the new zone).
    bool update(net::ConnectionState state, bool in_world) {
        using S = net::ConnectionState;
        const bool zoning = state == S::Zoning || state == S::WorldConnecting || state == S::WorldAuth ||
                            state == S::EnteringWorld || state == S::ZoneConnecting || state == S::ZoneEntering;
        switch (phase_) {
        case Phase::None: return false;
        case Phase::Camping:
            if (state == S::Camping) return false;
            phase_ = zoning ? Phase::AfterZone : Phase::None; // in the zone again: the player ended the camp
            return false;
        case Phase::AfterZone:
            if (zoning || (state == S::InZone && !in_world)) return false;
            phase_ = Phase::None;
            return state == S::InZone;
        }
        return false;
    }
    // A zone change cut the camp off and the next zone has not been entered yet.
    bool waiting() const { return phase_ == Phase::AfterZone; }
    void reset() { phase_ = Phase::None; }

private:
    enum class Phase { None, Camping, AfterZone };
    Phase phase_ = Phase::None;
};

} // namespace hailstone::app
