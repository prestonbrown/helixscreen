// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_qidi.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "test_helpers/seeded_override.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "hv/json.hpp"

using json = nlohmann::json;

namespace helix {

// Friend-class shim per L065 — exposes private parse helpers for unit tests.
// Mirrors the Ad5xIfsTestAccess pattern in test_ams_backend_ad5x_ifs.cpp.
class QidiBoxTestAccess {
  public:
    static void parse_vars(AmsBackendQidi& b, const json& v) {
        b.parse_save_variables(v);
    }
    static void handle_status(AmsBackendQidi& b, const json& n) {
        b.handle_status_update(n);
    }
    static int filament_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).filament_id;
    }
    static int color_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).color_id;
    }
    static int vendor_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).vendor_id;
    }
    static void apply_query(AmsBackendQidi& b, const json& response) {
        b.apply_query_response(response);
    }
    static void apply_filas_list(AmsBackendQidi& b, const std::string& content) {
        b.apply_filas_list(content);
    }
    static std::optional<AmsBackendQidi::FilaProfile> get_profile(const AmsBackendQidi& b,
                                                                  int fila_id) {
        auto it = b.fila_profiles_.find(fila_id);
        if (it == b.fila_profiles_.end())
            return std::nullopt;
        return it->second;
    }
    static std::optional<uint32_t> get_color(const AmsBackendQidi& b, int color_id) {
        auto it = b.color_palette_.find(color_id);
        if (it == b.color_palette_.end())
            return std::nullopt;
        return it->second;
    }
    static std::optional<std::string> get_vendor(const AmsBackendQidi& b, int vendor_id) {
        auto it = b.vendor_names_.find(vendor_id);
        if (it == b.vendor_names_.end())
            return std::nullopt;
        return it->second;
    }
    static size_t color_count(const AmsBackendQidi& b) {
        return b.color_palette_.size();
    }
    static size_t vendor_count(const AmsBackendQidi& b) {
        return b.vendor_names_.size();
    }
    static int resolve_fila_id(const std::map<int, AmsBackendQidi::FilaProfile>& profiles,
                               const std::string& material, const std::string& name) {
        return AmsBackendQidi::resolve_fila_id(profiles, material, name);
    }
    static int resolve_color_id(const std::map<int, uint32_t>& palette, uint32_t rgb) {
        return AmsBackendQidi::resolve_color_id(palette, rgb);
    }
    static int resolve_vendor_id(const std::map<int, std::string>& vendors,
                                 const std::string& brand) {
        return AmsBackendQidi::resolve_vendor_id(vendors, brand);
    }
    static DryerInfo get_dryer(const AmsBackendQidi& b, int unit = 0) {
        return b.get_dryer_info(unit);
    }
    static void set_clock(AmsBackendQidi& b, std::function<std::time_t()> fn) {
        b.now_fn_ = std::move(fn);
    }
    static void apply_box_extras(AmsBackendQidi& b, const json& e) {
        b.apply_box_extras(e);
    }
    static void set_drying_timer_supported(AmsBackendQidi& b, bool v) {
        b.drying_timer_supported_ = v;
    }
    static void apply_config_settings(AmsBackendQidi& b, const json& s) {
        b.apply_config_settings(s);
    }
    static void set_fw_caps(AmsBackendQidi& b, bool has_m603, bool has_clear_nozzle) {
        b.fw_has_m603_ = has_m603;
        b.fw_has_clear_nozzle_ = has_clear_nozzle;
    }
    static void seed_override(AmsBackendQidi& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        {
            std::lock_guard<std::mutex> lock(b.mutex_);
            b.overrides_[slot_index] = ovr;
        }
        // The backend's own init files both stores together; a fixture that
        // wrote only this map would leave the lane reading empty.
        helix::test::file_override_as_lane_records(b, slot_index, ovr);
    }
    static std::optional<helix::ams::FilamentSlotOverride> get_override(const AmsBackendQidi& b,
                                                                        int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendQidi& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    // Runs only the startup store load: a null client makes on_started return
    // immediately after it, skipping the subscription machinery.
    static void start_load(AmsBackendQidi& b) {
        b.on_started();
    }
    static std::optional<std::string> last_fingerprint(const AmsBackendQidi& b, int slot_index) {
        return b.rfid_tracker_.baseline(slot_index);
    }
    static std::string normalize_legacy_fingerprint(const std::string& stored) {
        return AmsBackendQidi::normalize_legacy_fingerprint(stored);
    }
};
} // namespace helix
