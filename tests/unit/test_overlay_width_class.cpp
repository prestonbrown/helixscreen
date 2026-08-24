// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_width_class.cpp
 * @brief Overlay width classification (prestonbrown/helixscreen#1178)
 *
 * An overlay renders at one of two widths, and which one is a property of how
 * the user got there rather than of the overlay's own XML:
 *
 *   destination     screen - nav              parked here; drill-downs inherit
 *   transient layer screen - nav - space_lg   opened over something; you return
 *
 * These tests cover the pure resolution rule. The push-time wiring that feeds
 * it lives in test_overlay_width_push.cpp.
 */

#include "ui_nav_manager.h"

#include "overlay_class.h"

#include "../catch_amalgamated.hpp"

using helix::OverlayClass;
using helix::PanelId;
using helix::resolve_overlay_is_destination;

// ============================================================================
// Explicit promotion
// ============================================================================

TEST_CASE("Explicit destination wins over any parent", "[overlay][width][1178]") {
    // AMS and Print Status are promoted at their push sites: they are places
    // users park, regardless of which nav root happened to launch them.
    for (bool has_parent : {false, true}) {
        for (bool parent_dest : {false, true}) {
            for (bool root_dest : {false, true}) {
                INFO("has_parent=" << has_parent << " parent_dest=" << parent_dest
                                   << " root_dest=" << root_dest);
                CHECK(resolve_overlay_is_destination(OverlayClass::Destination, has_parent,
                                                     parent_dest, root_dest));
            }
        }
    }
}

// ============================================================================
// Inheritance from the overlay beneath
// ============================================================================

TEST_CASE("Inherit takes the class of the overlay beneath it", "[overlay][width][1178]") {
    SECTION("parent is a destination — Settings > Display & Sound > Theme Editor") {
        CHECK(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/true,
                                             /*parent_dest=*/true, /*root_dest=*/false));
    }

    SECTION("parent is transient — Console > Console Settings") {
        // The reported bug: Console Settings was full width while Console was
        // gapped, so the child rendered 16px WIDER than the panel it covered.
        CHECK_FALSE(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/true,
                                                   /*parent_dest=*/false, /*root_dest=*/false));
    }

    SECTION("parent class beats root class when both are present") {
        // Console is pushed from Advanced (transient root), so console_settings
        // must follow Console, not the root. Both are transient here, so flip
        // the root to prove the parent is what is actually being read.
        CHECK_FALSE(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/true,
                                                   /*parent_dest=*/false, /*root_dest=*/true));
        CHECK(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/true,
                                             /*parent_dest=*/true, /*root_dest=*/false));
    }
}

// ============================================================================
// Inheritance from the nav root (first overlay on the stack)
// ============================================================================

TEST_CASE("First overlay inherits from the nav root", "[overlay][width][1178]") {
    SECTION("Settings root — Settings > Network") {
        CHECK(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/false,
                                             /*parent_dest=*/false, /*root_dest=*/true));
    }

    SECTION("Advanced root — Advanced > Bed Mesh") {
        CHECK_FALSE(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/false,
                                                   /*parent_dest=*/false, /*root_dest=*/false));
    }

    SECTION("parent_dest is ignored when there is no parent overlay") {
        // Guards against reading a stale parent flag on an empty overlay stack.
        CHECK_FALSE(resolve_overlay_is_destination(OverlayClass::Inherit, /*has_parent=*/false,
                                                   /*parent_dest=*/true, /*root_dest=*/false));
    }
}

// ============================================================================
// Which nav roots are destinations
// ============================================================================

TEST_CASE("Settings is the only destination nav root", "[overlay][width][1178]") {
    CHECK(helix::nav_root_is_destination(PanelId::Settings));

    CHECK_FALSE(helix::nav_root_is_destination(PanelId::Home));
    CHECK_FALSE(helix::nav_root_is_destination(PanelId::PrintSelect));
    CHECK_FALSE(helix::nav_root_is_destination(PanelId::Controls));
    CHECK_FALSE(helix::nav_root_is_destination(PanelId::Filament));
    CHECK_FALSE(helix::nav_root_is_destination(PanelId::Advanced));
}

// ============================================================================
// The multi-entry case — the reason resolution happens at push time
// ============================================================================

TEST_CASE("The same overlay resolves differently per entry point", "[overlay][width][1178]") {
    // fan_control_overlay is reachable from Controls (transient) and from
    // Settings > Fans (destination). No static XML width attribute can be
    // correct for both, which is why the class is resolved on push.
    const bool from_controls = resolve_overlay_is_destination(
        OverlayClass::Inherit, /*has_parent=*/false, /*parent_dest=*/false, /*root_dest=*/false);

    const bool from_settings_fans = resolve_overlay_is_destination(
        OverlayClass::Inherit, /*has_parent=*/true, /*parent_dest=*/true, /*root_dest=*/true);

    CHECK_FALSE(from_controls);
    CHECK(from_settings_fans);
    CHECK(from_controls != from_settings_fans);
}
