// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AmsBackend::runout_backup_for(): the slot a runout continues from, as the
// base answers it for every backend that does not override it. Three answers:
// a slot, -1 when the slot has nothing to switch to, and nullopt when the
// backend cannot say per slot.

#include "ams_backend.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"

#include <optional>

#include "../catch_amalgamated.hpp"

using helix::AmsBackend;
using helix::AmsBackendMock;
using helix::AmsBackendToolChanger;

TEST_CASE("An editable endless-spool lane answers -1 with no backup set and its backup once set",
          "[ams][endless_spool][runout_backup]") {
    AmsBackendMock mock(4);
    REQUIRE(mock.get_endless_spool_capabilities().editable());
    REQUIRE(mock.get_system_info().total_slots == 4);
    for (int slot = 0; slot < mock.get_system_info().total_slots; ++slot) {
        REQUIRE(mock.set_endless_spool_backup(slot, -1).success());
    }
    REQUIRE(mock.get_endless_spool_config().groups.empty());

    // -1, not nullopt: the relation is the UI's to edit, so the lane's page
    // offers the other lanes to pick from. Nullopt would mean the firmware
    // picks a spool at runout and says nothing in advance.
    CHECK(mock.runout_backup_for(1) == std::optional<int>(-1));
    CHECK(mock.runout_backup_for(3) == std::optional<int>(-1));

    REQUIRE(mock.set_endless_spool_backup(1, 2).success());
    CHECK(mock.runout_backup_for(1) == std::optional<int>(2));
    CHECK(mock.runout_backup_for(0) == std::optional<int>(-1));

    // Firmware-managed once it is published: still answered, only not editable.
    mock.set_endless_spool_editable(false);
    REQUIRE_FALSE(mock.get_endless_spool_capabilities().editable());
    CHECK(mock.runout_backup_for(1) == std::optional<int>(2));
}

TEST_CASE("A backend that neither edits nor publishes an endless-spool relation answers nothing",
          "[ams][endless_spool][runout_backup]") {
    AmsBackendToolChanger tool_changer(nullptr, nullptr);
    REQUIRE_FALSE(tool_changer.get_endless_spool_capabilities().editable());
    REQUIRE(tool_changer.get_endless_spool_config().groups.empty());
    CHECK(tool_changer.runout_backup_for(0) == std::nullopt);
}
