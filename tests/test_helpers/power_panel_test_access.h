// tests/test_helpers/power_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_power.h"

#include <cstddef>
#include <vector>

namespace helix::ui {

// Test-only access to PowerPanel's cached widget pointers.
//
// PowerPanel caches four raw pointers into a widget tree it does not own, plus
// one per device row. Nothing on the panel owns that tree's lifetime, so a
// teardown between "work queued" and "queue drained" leaves those pointers
// dangling while the deferred callbacks still consider them live. These
// accessors let a test observe the cached pointers directly rather than
// inferring their state from a crash.
struct PowerPanelTestAccess {
    static lv_obj_t* chip_container(PowerPanel& p) {
        return p.chip_container_;
    }
    static lv_obj_t* device_list_container(PowerPanel& p) {
        return p.device_list_container_;
    }
    static lv_obj_t* empty_state_container(PowerPanel& p) {
        return p.empty_state_container_;
    }
    static lv_obj_t* cached_overlay(PowerPanel& p) {
        return p.cached_overlay_;
    }
    static std::size_t device_row_count(PowerPanel& p) {
        return p.device_rows_.size();
    }

    /// Seed cached_overlay_ the way get_or_create_overlay() would, without
    /// dragging NavigationManager registration into a widget-lifetime test.
    static void set_cached_overlay(PowerPanel& p, lv_obj_t* overlay) {
        p.cached_overlay_ = overlay;
    }

    /// Invoke the real deferred-rebuild entry point under test.
    static void populate_device_list(PowerPanel& p, const std::vector<PowerDevice>& devices) {
        p.populate_device_list(devices);
    }

    /// Invoke the chip-rebuild producer reached from a chip tap.
    static void handle_chip_clicked(PowerPanel& p, const std::string& device_name) {
        p.handle_chip_clicked(device_name);
    }
};

} // namespace helix::ui
