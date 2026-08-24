// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_endless_spool_reset_guard.cpp
 * @brief AmsBackend::reset_endless_spool() must refuse a backend that has not
 *        reported a slot count yet, instead of reporting success for a loop it
 *        never entered.
 *
 * set_endless_spool_backup() has always carried this guard. reset_endless_spool()
 * did not, so `for (slot = 0; slot < 0; ...)` simply did not run and first_error
 * stayed success(). That became user-reachable when the AMS device-operations
 * overlay grew a Reset button: on a backend that is editable() but whose
 * total_slots is still 0 (AFC before any lane has been parsed), the user confirms
 * a destructive warning and gets no error and no effect.
 */

#include "ams_backend_mock.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Editable endless spool, no slot count yet — the window between "the backend
/// is up and says it supports per-slot failover" and "the backend has parsed its
/// lanes". Also counts writes, so the test can prove nothing was touched.
class SlotlessEditableBackend : public AmsBackendMock {
  public:
    SlotlessEditableBackend() : AmsBackendMock(4) {}

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.total_slots = 0;
        return info;
    }

    AmsError apply_endless_spool_backup(int slot_index, int backup_slot) override {
        ++writes;
        return AmsBackendMock::apply_endless_spool_backup(slot_index, backup_slot);
    }

    /// The protected accessor reset_endless_spool() actually loops over.
    int slot_count() const {
        return endless_spool_slot_count();
    }

    int writes = 0;
};

} // namespace

TEST_CASE("reset_endless_spool refuses a backend with no slot count",
          "[ams][endless_spool][reset]") {
    SlotlessEditableBackend backend;
    REQUIRE(backend.get_endless_spool_capabilities().editable());
    REQUIRE(backend.slot_count() == 0);

    auto result = backend.reset_endless_spool();

    // The whole point: NOT success. A silent success behind a destructive
    // confirmation is indistinguishable from a working reset.
    CHECK_FALSE(result.success());
    CHECK(result.result == AmsResult::NOT_SUPPORTED);
    CHECK_FALSE(result.user_msg.empty()); // something to put in front of the user
    CHECK(backend.writes == 0);

    // The sibling that always had this guard answers the same way, which is the
    // consistency the fix is about.
    auto sibling = backend.set_endless_spool_backup(0, -1);
    CHECK_FALSE(sibling.success());
    CHECK(sibling.result == result.result);
}

TEST_CASE("reset_endless_spool still clears every slot once the count is known",
          "[ams][endless_spool][reset]") {
    // The guard must not swallow the working case.
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    REQUIRE(backend.set_endless_spool_backup(0, 1));
    REQUIRE(backend.set_endless_spool_backup(2, 3));
    REQUIRE_FALSE(backend.get_endless_spool_config().empty());

    CHECK(backend.reset_endless_spool().success());
    CHECK(backend.get_endless_spool_config().empty());

    backend.stop();
}
