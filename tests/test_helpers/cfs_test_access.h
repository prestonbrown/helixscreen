// tests/test_helpers/cfs_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_cfs.h"
#include "ams_error.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "hv/json.hpp"

// Friend-class shim for AmsBackendCfs -- declared as friend in the backend
// header (`friend class ::CfsTestAccess;`). Gives tests narrow accessors for
// private override state and the homing-dispatch surface without going
// through the public get_slot_info path (which layers apply_overrides on top
// and obscures what the internal maps actually hold), and without widening
// any member's access level in production code just to let a test reach it.
class CfsTestAccess {
  public:
    static void handle_status(helix::printer::AmsBackendCfs& b, const nlohmann::json& n) {
        b.handle_status_update(n);
    }
    static void seed_override(helix::printer::AmsBackendCfs& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const helix::printer::AmsBackendCfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(helix::printer::AmsBackendCfs& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    static std::optional<std::string> last_rfid_uid(const helix::printer::AmsBackendCfs& b,
                                                    int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.rfid_tracker_.baseline(slot_index);
    }
    static helix::printer::CfsMacroVariant macro_variant(const helix::printer::AmsBackendCfs& b) {
        return b.macro_variant_;
    }
    // Seed the nozzle-loaded signal + preloaded-slot index used by change_tool's
    // WITH/WITHOUT-material selection (#968 Phase 2). filament_loaded reflects
    // filament physically at the nozzle; current_slot can be a *preloaded*
    // (cassette) slot with the nozzle still empty on K1 CFS.
    static void set_loaded_state(helix::printer::AmsBackendCfs& b, bool filament_loaded,
                                 int current_slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.filament_loaded = filament_loaded;
        b.system_info_.current_slot = current_slot;
    }
    // Seed only the seated bay (current_slot) for tests that pin the seated
    // lane independently of the nozzle-loaded flag (e.g. the
    // toolhead-unaccounted gate, which reads the pair separately).
    /// The latched stock-dialect declaration itself, not is_bypass_active() —
    /// that ORs in `current_slot == -2` and so cannot tell a live derivation
    /// from the flag that permits one.
    static bool bypass_declared(const helix::printer::AmsBackendCfs& b) {
        return b.bypass_declared_;
    }

    static void set_bypass_declared(helix::printer::AmsBackendCfs& b, bool declared) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.bypass_declared_ = declared;
        b.system_info_.supports_bypass = true;
    }

    static void set_seated_bay(helix::printer::AmsBackendCfs& b, int slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot;
    }
    static void set_last_rfid_uid(helix::printer::AmsBackendCfs& b, int slot_index,
                                  const std::string& uid) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.rfid_tracker_.observe(slot_index, uid);
    }
    static void set_macro_variant_k1(helix::printer::AmsBackendCfs& b) {
        b.macro_variant_ = helix::printer::CfsMacroVariant::K1;
    }
    static void set_macro_variant_fork(helix::printer::AmsBackendCfs& b) {
        b.macro_variant_ = helix::printer::CfsMacroVariant::Fork;
    }
    // Seed the flat-schema state a real payload would have latched: schema,
    // Fork dialect, and the external spool entry's slots[] index (-1 = no
    // external entry in the payload). Lets enable_bypass's Fork branch be
    // exercised without a live box frame.
    static void set_flat_fork(helix::printer::AmsBackendCfs& b, int external_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.schema_ = helix::printer::CfsSchema::Flat;
        b.macro_variant_ = helix::printer::CfsMacroVariant::Fork;
        b.external_slot_index_ = external_index;
        b.system_info_.supports_bypass = external_index >= 0;
    }
    // Seed N connected CFS units (unit_index 0..N-1) so device-action code that
    // iterates system_info_.units (e.g. refresh_rfid → BOX_INFO_REFRESH) has
    // addressable units without a live Moonraker parse.
    static void set_connected_units(helix::printer::AmsBackendCfs& b, int count) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.units.clear();
        for (int u = 0; u < count; ++u) {
            AmsUnit unit;
            unit.unit_index = u;
            unit.connected = true;
            unit.slot_count = 4;
            unit.first_slot_global_index = u * 4;
            b.system_info_.units.push_back(std::move(unit));
        }
    }

    /// Call the REAL dispatch_action_script implementation directly. The
    /// caller does not subclass AmsBackendCfs to reach this -- the virtual
    /// stays private, and this friend shim is the one sanctioned way in.
    static AmsError call_dispatch_action_script(helix::printer::AmsBackendCfs& b,
                                                std::string gcode) {
        return b.dispatch_action_script(std::move(gcode));
    }

    /// Put the backend in the state dispatch leaves it in: action set, phase
    /// tracker armed with the latched intent. Mirrors do_load_filament /
    /// do_unload_filament without needing a live Moonraker.
    static void force_phase_intent(helix::printer::AmsBackendCfs& b, AmsAction op) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.action = op;
        b.begin_phase_tracking();
    }

    /// Read back the bypass flag do_unload_filament latched on the phase
    /// tracker. The verdict for a bypass unload hangs off this, and nothing on
    /// the public surface reports which script actually went out.
    static bool phase_bypass_unload(const helix::printer::AmsBackendCfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.phase_tracker_.bypass_unload;
    }

    /// Seed the toolhead filament switch. `seen` distinguishes "the sensor has
    /// published a real boolean" from "we have only the default", which is the
    /// difference between a verdict and Unverifiable.
    static void set_filament_sensor(helix::printer::AmsBackendCfs& b, bool seen, bool detected) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.filament_sensor_seen_ = seen;
        b.last_filament_detected_ = detected;
        b.system_info_.filament_loaded = detected;
    }

    /// Run the real completion path (the gcode-script success callback body),
    /// including phase verification.
    static void complete_action(helix::printer::AmsBackendCfs& b) {
        b.finish_action();
    }

    /// Run the real on_started (declaration restore + initial query; the
    /// Moonraker query itself is null-guarded in test constructions).
    static void call_on_started(helix::printer::AmsBackendCfs& b) {
        b.on_started();
    }
};
