#include <doctest/doctest.h>

#include "app/addon_admission.h"

using hailstone::app::AddonAdmission;
using P = hailstone::app::AddonAdmissionPhase;

TEST_CASE("addon admission: fresh verification and consent must finish before entering") {
    AddonAdmission admission;
    admission.begin_check(7);
    CHECK(admission.rules_serial() == 7);
    CHECK(admission.phase() == P::Checking);
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(true, false, {});
    admission.admit();
    CHECK(admission.phase() == P::Checking);
    CHECK_FALSE(admission.admitted());

    admission.verification_finished({});
    CHECK(admission.phase() == P::Starting);
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(false, true, {});
    CHECK(admission.phase() == P::AwaitingConsent);
    const auto pending_revision = admission.revision();
    for (int frame = 0; frame < 1000; ++frame) admission.runtime_changed(false, true, {});
    CHECK(admission.revision() == pending_revision);
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(false, false, {});
    CHECK(admission.phase() == P::Starting);
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(true, false, {});
    CHECK(admission.phase() == P::Ready);
    CHECK(admission.may_enter());
    CHECK_FALSE(admission.admitted());
    admission.admit();
    CHECK(admission.admitted());
}

TEST_CASE("addon admission: package failure cannot be cleared by a runtime result") {
    AddonAdmission admission;
    admission.begin_check(3);
    admission.verification_finished("Required package is changed");
    CHECK(admission.phase() == P::Refused);
    CHECK(admission.problem() == "Required package is changed");
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(true, false, {});
    admission.admit();
    CHECK(admission.phase() == P::Refused);
    CHECK_FALSE(admission.admitted());
    admission.begin_check(3); // explicit recovery may use the same server rules
    admission.verification_finished({});
    admission.runtime_changed(false, false, "Required add-on did not start");
    CHECK(admission.phase() == P::Refused);
    CHECK_FALSE(admission.may_enter());
    admission.runtime_changed(true, false, {});
    CHECK(admission.may_enter());
    CHECK(admission.problem().empty());
}

TEST_CASE("addon admission: failures and new rules during play never remove admission") {
    AddonAdmission admission;
    admission.begin_check(1);
    admission.verification_finished({});
    admission.runtime_changed(true, false, {});
    admission.admit();
    admission.runtime_changed(false, false, "Required add-on exited");
    CHECK(admission.phase() == P::Degraded);
    CHECK(admission.problem() == "Required add-on exited");
    CHECK(admission.may_enter());

    admission.begin_check(2); // zoning or a refreshed policy in the same connection
    CHECK(admission.phase() == P::Checking);
    CHECK(admission.may_enter());
    admission.verification_finished("New required package is missing");
    CHECK(admission.phase() == P::Degraded);
    CHECK(admission.may_enter());
    admission.runtime_changed(true, false, {});
    CHECK(admission.problem() == "New required package is missing");

    admission.reset(); // a new connection must meet its fresh requirements
    CHECK_FALSE(admission.admitted());
    admission.begin_check(2);
    admission.verification_finished("New required package is missing");
    CHECK(admission.phase() == P::Refused);
    CHECK_FALSE(admission.may_enter());
}

TEST_CASE("addon admission: emergency stop requires explicit verification before recovery") {
    for (bool enter_first : {false, true}) {
        CAPTURE(enter_first);
        AddonAdmission admission;
        admission.begin_check(1);
        admission.verification_finished({});
        admission.runtime_changed(true, false, {});
        if (enter_first) admission.admit();
        admission.emergency_stop("Required add-ons stopped by the player");
        CHECK(admission.phase() == (enter_first ? P::Degraded : P::Refused));
        CHECK(admission.may_enter() == enter_first);
        admission.runtime_changed(true, false, {});
        CHECK(admission.problem() == "Required add-ons stopped by the player");
        admission.begin_check(1);
        admission.verification_finished({});
        admission.runtime_changed(true, false, {});
        CHECK(admission.phase() == P::Ready);
        CHECK(admission.may_enter());
    }
}

TEST_CASE("addon admission: disabled policy permits legacy entry and ignores stale results") {
    AddonAdmission admission;
    CHECK(admission.phase() == P::Inactive);
    CHECK(admission.may_enter());
    admission.admit();
    admission.begin_check(4);
    CHECK(admission.may_enter()); // an admitted legacy session is not kicked by a later policy
    admission.disable();
    CHECK(admission.phase() == P::Inactive);
    CHECK(admission.admitted());
    admission.verification_finished("stale worker failure");
    admission.runtime_changed(false, false, "stale runtime failure");
    CHECK(admission.phase() == P::Inactive);
    CHECK(admission.problem().empty());
    CHECK(admission.may_enter());
    admission.reset();
    CHECK_FALSE(admission.admitted());
    CHECK(admission.rules_serial() == 0);
    admission.begin_check(5);
    CHECK_FALSE(admission.may_enter());
}
