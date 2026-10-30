// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_mesh_state.h"

#include "format_utils.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix {

BedMeshState& BedMeshState::instance() {
    static HELIX_PSRAM_BSS BedMeshState state;
    return state;
}

void BedMeshState::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(available_, 0, "bed_mesh_available", subjects_);
    UI_MANAGED_SUBJECT_STRING(profile_name_, profile_name_buf_, "", "bed_mesh_profile_name",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(dimensions_, dimensions_buf_, "No mesh data", "bed_mesh_dimensions",
                              subjects_);
    // "Max"/"Min" labels are static text in bed_mesh_current_mesh_card.xml; only the
    // measured value and its coordinates change.
    UI_MANAGED_SUBJECT_STRING(max_value_, max_value_buf_, "--", "bed_mesh_max_value", subjects_);
    UI_MANAGED_SUBJECT_STRING(max_coord_, max_coord_buf_, "", "bed_mesh_max_coord", subjects_);
    UI_MANAGED_SUBJECT_STRING(min_value_, min_value_buf_, "--", "bed_mesh_min_value", subjects_);
    UI_MANAGED_SUBJECT_STRING(min_coord_, min_coord_buf_, "", "bed_mesh_min_coord", subjects_);
    UI_MANAGED_SUBJECT_STRING(variance_, variance_buf_, "", "bed_mesh_variance", subjects_);
    UI_MANAGED_SUBJECT_INT(profile_count_, 0, "bed_mesh_profile_count", subjects_);
    UI_MANAGED_SUBJECT_INT(version_, 0, "bed_mesh_version", subjects_);
    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "BedMeshState", []() { BedMeshState::instance().deinit_subjects(); });
}

void BedMeshState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    detach();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    stats_ = {};
    profile_ = {};
}

void BedMeshState::attach(IMoonrakerAPI* api) {
    detach();
    if (!api || !subjects_initialized_) {
        return;
    }
    const auto token = lifetime_.token();
    const SubscriptionId id =
        api->subscribe_notifications([this, api, token](const nlohmann::json& notification) {
            // Runs on the WebSocket thread: look only at the key, then hand the read to
            // the main thread.
            if (!notification.contains("params") || !notification["params"].is_array() ||
                notification["params"].empty()) {
                return;
            }
            const nlohmann::json& params = notification["params"][0];
            if (!params.is_object() || !params.contains("bed_mesh") ||
                !params["bed_mesh"].is_object()) {
                return;
            }
            token.defer("BedMeshState::bed_mesh_update", [this, api]() { refresh_from_api(api); });
        });
    subscription_ = SubscriptionGuard(api, id);
    refresh_from_api(api);
}

void BedMeshState::detach() {
    lifetime_.invalidate();
    subscription_.reset();
}

void BedMeshState::refresh_from_api(IMoonrakerAPI* api) {
    const BedMeshProfile* active = api->advanced().get_active_bed_mesh();
    apply(active, static_cast<int>(api->advanced().get_bed_mesh_profiles().size()));
}

void BedMeshState::retranslate() {
    if (!subjects_initialized_ || stats_.loaded) {
        return;
    }
    lv_subject_copy_string(&dimensions_, lv_tr("No mesh data"));
}

void BedMeshState::apply(const BedMeshProfile* active, int profile_count) {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&profile_count_, profile_count);

    const MeshStats stats = active ? compute_mesh_stats(*active) : MeshStats{};
    stats_ = stats;
    profile_ = stats.loaded ? *active : BedMeshProfile{};

    if (!stats.loaded) {
        lv_subject_set_int(&available_, 0);
        lv_subject_copy_string(&dimensions_, lv_tr("No mesh data"));
        lv_subject_copy_string(&max_value_, "--");
        // Empty, not "[]": no coordinates exist without a probed mesh.
        lv_subject_copy_string(&max_coord_, "");
        lv_subject_copy_string(&min_value_, "--");
        lv_subject_copy_string(&min_coord_, "");
        lv_subject_copy_string(&variance_, "");
    } else {
        lv_subject_copy_string(&profile_name_, active->name.c_str());
        std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d points", stats.cols,
                      stats.rows);
        lv_subject_copy_string(&dimensions_, dimensions_buf_);

        // Coordinates round to whole mm: probe points land on fractional positions, and
        // 0.1 mm precision on where the high or low spot sits is not actionable. The
        // measured Z keeps three decimals.
        std::snprintf(max_coord_buf_, sizeof(max_coord_buf_), "[%.0f, %.0f]", stats.max_x,
                      stats.max_y);
        lv_subject_copy_string(&max_coord_, max_coord_buf_);
        format::format_distance_mm(stats.max_z, 3, max_value_buf_, sizeof(max_value_buf_));
        lv_subject_copy_string(&max_value_, max_value_buf_);

        std::snprintf(min_coord_buf_, sizeof(min_coord_buf_), "[%.0f, %.0f]", stats.min_x,
                      stats.min_y);
        lv_subject_copy_string(&min_coord_, min_coord_buf_);
        format::format_distance_mm(stats.min_z, 3, min_value_buf_, sizeof(min_value_buf_));
        lv_subject_copy_string(&min_value_, min_value_buf_);

        format::format_distance_mm(stats.range, 3, variance_buf_, sizeof(variance_buf_));
        lv_subject_copy_string(&variance_, variance_buf_);
        lv_subject_set_int(&available_, 1);
    }
    lv_subject_set_int(&version_, lv_subject_get_int(&version_) + 1);
    spdlog::debug("[BedMeshState] applied: loaded={} profile='{}' {}x{} range={:.3f} profiles={}",
                  stats.loaded, active ? active->name : "", stats.cols, stats.rows, stats.range,
                  profile_count);
}

} // namespace helix
