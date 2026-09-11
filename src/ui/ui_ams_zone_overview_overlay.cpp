// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_zone_overview_overlay.cpp
 * @brief Implementation of AmsZoneOverviewOverlay (environment zone list)
 */

#include "ui_ams_zone_overview_overlay.h"

#include "ui_ams_environment_overlay.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_zone_presentation.h"

#include "ams_backend.h"
#include "ams_environment_zone.h"
#include "ams_state.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace helix::ui {

namespace {

// The word for a classification zone_status() already made. The row's colour comes from
// the severity in that same value, so the two cannot name different states.
std::string zone_status_text(const ZoneStatus& status) {
    switch (status.kind) {
    case ZoneStatusKind::Drying:
        return lv_tr("Drying");
    case ZoneStatusKind::Passive:
        return lv_tr("passive");
    case ZoneStatusKind::Verdict:
    default:
        break;
    }
    switch (status.severity) {
    case ZoneVerdict::Ok:
        return lv_tr("OK");
    case ZoneVerdict::Marginal:
        return lv_tr("Marginal");
    case ZoneVerdict::TooHumid:
        return lv_tr("Too humid");
    case ZoneVerdict::Unknown:
    default:
        return "--";
    }
}

// The physical unit a zone sits in, independent of the zone's own label — a group
// header names the box the zones are grouped by, not the zone the user picked.
std::string unit_group_text(const helix::printer::EnvironmentZone& zone,
                            const std::string& type_name) {
    const std::string prefix = type_name.empty() ? std::string{} : type_name + " ";
    return prefix + lv_tr("Unit") + " " + std::to_string(lane_number(zone.unit_index));
}

std::string overview_subtitle(const std::vector<helix::printer::EnvironmentZone>& zones) {
    std::set<int> units;
    int dryers = 0;
    for (const auto& z : zones) {
        if (z.unit_index >= 0) {
            units.insert(z.unit_index);
        }
        if (z.dryer.supported) {
            ++dryers;
        }
    }
    char buf[96];
    // A labeled form rather than "%d units - %d zones", which would need a plural
    // branch per count and multiply across every translated language.
    snprintf(buf, sizeof(buf), lv_tr("Units: %d   Boxes: %d   Dryers: %d"),
             static_cast<int>(units.size()), static_cast<int>(zones.size()), dryers);
    return buf;
}

} // namespace

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AmsZoneOverviewOverlay> g_ams_zone_overview_overlay;

AmsZoneOverviewOverlay& get_ams_zone_overview_overlay() {
    if (!g_ams_zone_overview_overlay) {
        g_ams_zone_overview_overlay = std::make_unique<AmsZoneOverviewOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "AmsZoneOverviewOverlay", []() { g_ams_zone_overview_overlay.reset(); });
    }
    return *g_ams_zone_overview_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AmsZoneOverviewOverlay::AmsZoneOverviewOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AmsZoneOverviewOverlay::~AmsZoneOverviewOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AmsZoneOverviewOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(count_subject_, 0, "zone_ov_count", subjects_);
        UI_MANAGED_SUBJECT_STRING(subtitle_subject_, subtitle_buf_, "", "zone_ov_subtitle",
                                  subjects_);
    });
}

void AmsZoneOverviewOverlay::register_callbacks() {
    // Registration must happen before create() parses ams_zone_overview_overlay.xml,
    // whose <repeat> instantiates <zone_row> — a name lv_xml has to already know.
    // A function-local static keeps this a one-time cost per process: a second
    // lv_xml_register_component_from_data() call would orphan the first scope node.
    static bool s_registered = false;
    if (s_registered) {
        return;
    }

    lv_xml_register_event_cb(nullptr, "on_zone_row_clicked", on_zone_row_clicked);
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/zone_row.xml").c_str());

    // Self-register so the overlay is safe to open directly without first visiting a
    // host panel, the same reasoning as AmsEnvironmentOverlay::create().
    if (!lv_xml_component_get_scope("ams_zone_overview_overlay")) {
        lv_xml_register_component_from_file(
            helix::asset_component_uri("ui_xml/ams_zone_overview_overlay.xml").c_str());
    }

    s_registered = true;
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsZoneOverviewOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    if (!subjects_initialized_) {
        init_subjects();
    }
    register_callbacks();

    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_zone_overview_overlay", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

// ============================================================================
// SHOW
// ============================================================================

void AmsZoneOverviewOverlay::show(lv_obj_t* parent_screen,
                                  std::vector<helix::printer::EnvironmentZone> zones) {
    zones_ = std::move(zones);

    if (!overlay_) {
        create(parent_screen);
    }
    ui_alive_ = true;
    rebuild_rows();

    NavigationManager::instance().register_overlay_instance(overlay_, this);
    NavigationManager::instance().push_overlay(overlay_);
}

void AmsZoneOverviewOverlay::rebuild_rows() {
    if (!ui_alive_) {
        spdlog::debug("[{}] rebuild_rows() skipped - overlay UI not alive", get_name());
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    const std::string type_name = backend ? backend->get_system_info().type_name : std::string{};
    const bool grouped = zones_span_units(zones_);
    const size_t n = zones_.size();

    label_pool_.ensure_size(n);
    slots_pool_.ensure_size(n);
    reading_pool_.ensure_size(n);
    status_pool_.ensure_size(n);
    verdict_pool_.ensure_size(n);
    group_hidden_pool_.ensure_size(n);
    group_text_pool_.ensure_size(n);

    int last_unit = -1;
    for (size_t i = 0; i < n; ++i) {
        const auto& z = zones_[i];

        label_pool_.set_string(i, zone_display_label(z, lv_tr("Unit"), lv_tr("Slot"), type_name));
        slots_pool_.set_string(i, zone_slot_text(z, lv_tr("Slots"), lv_tr("Slot")));

        char reading[32] = {};
        if (z.env.has_humidity) {
            snprintf(reading, sizeof(reading),
                     "%.0f%%  %.0f\xC2\xB0"
                     "C",
                     z.env.humidity_pct, z.env.temperature_c);
        } else {
            snprintf(reading, sizeof(reading),
                     "%.0f\xC2\xB0"
                     "C",
                     z.env.temperature_c);
        }
        reading_pool_.set_string(i, reading);

        const ZoneStatus status = zone_status(z);
        status_pool_.set_string(i, zone_status_text(status));
        verdict_pool_.set_int(i, static_cast<int>(status.severity));

        // The header shows on the first row of each unit, and never when the set sits
        // inside one unit.
        const bool starts_group = grouped && z.unit_index != last_unit;
        group_hidden_pool_.set_int(i, starts_group ? 0 : 1);
        group_text_pool_.set_string(i, starts_group ? unit_group_text(z, type_name) : "");
        last_unit = z.unit_index;
    }

    snprintf(subtitle_buf_, sizeof(subtitle_buf_), "%s", overview_subtitle(zones_).c_str());
    lv_subject_copy_string(&subtitle_subject_, subtitle_buf_);

    lv_subject_set_int(&count_subject_, static_cast<int>(n));
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void AmsZoneOverviewOverlay::on_activate() {
    OverlayBase::on_activate();

    // A row drilled into (dryer state, a fresh reading) can go stale while this
    // list sits paused underneath, so re-pull on the way back to the top of the
    // stack. Re-fetching everything and matching by id, the way
    // AmsEnvironmentOverlay::update_from_backend() does, keeps whichever set (one
    // unit's or every zone's) this overlay was shown with rather than narrowing it.
    if (AmsBackend* backend = AmsState::instance().get_backend(); backend && !zones_.empty()) {
        const auto fresh = backend->get_environment_zones(-1);
        for (auto& z : zones_) {
            const auto it = std::find_if(
                fresh.begin(), fresh.end(),
                [&z](const helix::printer::EnvironmentZone& f) { return f.id == z.id; });
            if (it != fresh.end()) {
                z = *it;
            }
        }
        rebuild_rows();
    }

    spdlog::trace("[{}] Activated", get_name());
}

void AmsZoneOverviewOverlay::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] Deactivated", get_name());
}

void AmsZoneOverviewOverlay::on_ui_destroyed() {
    ui_alive_ = false;
    label_pool_.reclaim();
    slots_pool_.reclaim();
    reading_pool_.reclaim();
    status_pool_.reclaim();
    verdict_pool_.reclaim();
    group_hidden_pool_.reclaim();
    group_text_pool_.reclaim();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AmsZoneOverviewOverlay::on_zone_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsZoneOverviewOverlay] on_zone_row_clicked");
    // An XML event_cb's user_data is always a heap-owned string: lv_obj_xml_event_cb_apply
    // strdups the attribute value. Reading it as an encoded integer yields the string's
    // address, which is never a valid index.
    const char* ud = static_cast<const char*>(lv_event_get_user_data(e));
    auto& self = get_ams_zone_overview_overlay();
    if (ud) {
        const auto index = static_cast<size_t>(atoi(ud));
        if (index < self.zones_.size()) {
            get_ams_environment_overlay().show_zone(lv_screen_active(), {self.zones_[index]}, 0,
                                                    false);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
