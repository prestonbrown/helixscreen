// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upgrade_banner.h"

#include "ui_nav_manager.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "system/update_checker.h"
#include "upgrade_nudge.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix {

namespace {

// Subject-backed message text ("HelixScreen <version> is available — update
// now"). Buffer is static since one banner exists for the app's lifetime.
constexpr size_t MESSAGE_BUF_SIZE = 128;
char g_message_buf[MESSAGE_BUF_SIZE] = "";

} // namespace

UpgradeBanner& UpgradeBanner::instance() {
    static UpgradeBanner singleton;
    return singleton;
}

void UpgradeBanner::init() {
    if (banner_) {
        return; // already initialized
    }

    // Create + register the text subject before the XML is instantiated so
    // the `bind_text="upgrade_banner_text"` binding resolves to our buffer.
    if (!message_subject_initialized_) {
        lv_subject_init_string(&message_subject_, g_message_buf, nullptr, MESSAGE_BUF_SIZE, "");
        lv_xml_register_subject(nullptr, "upgrade_banner_text", &message_subject_);
        message_subject_initialized_ = true;
    }

    // Register XML event callbacks exactly once. lv_xml_register_event_cb
    // tolerates double-registration but we guard for clarity.
    lv_xml_register_event_cb(nullptr, "on_upgrade_banner_update",
                             &UpgradeBanner::on_update_clicked);
    lv_xml_register_event_cb(nullptr, "on_upgrade_banner_dismiss",
                             &UpgradeBanner::on_dismiss_clicked);

    // Create the banner as a child of lv_layer_top so it floats above panels.
    banner_ = static_cast<lv_obj_t*>(lv_xml_create(lv_layer_top(), "upgrade_banner", nullptr));
    if (!banner_) {
        spdlog::error("[UpgradeBanner] Failed to instantiate upgrade_banner XML component");
        return;
    }

    // Align to the top edge of the screen; layer_top already covers the
    // full display, so the banner just sits at y=0.
    lv_obj_align(banner_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN); // start hidden

    // Observe UpdateChecker status so the banner reappears/reevaluates when
    // a new version is detected. lifetime token keeps deferred callbacks
    // safe if init() is somehow called late (shouldn't happen, but defensive).
    auto tok = lifetime_.token();
    status_observer_ =
        helix::ui::observe_int_sync<UpgradeBanner>(UpdateChecker::instance().status_subject(), this,
                                                   [tok](UpgradeBanner* self, int /*status*/) {
                                                       if (tok.expired())
                                                           return;
                                                       self->refresh();
                                                   });

    // Also re-render when the available version string changes — the text
    // shown in the banner comes from UpdateChecker::get_cached_update().
    version_observer_ =
        helix::ui::observe_string(UpdateChecker::instance().new_version_subject(), this,
                                  [tok](UpgradeBanner* self, const char* /*version*/) {
                                      if (tok.expired())
                                          return;
                                      self->refresh();
                                  });

    refresh();
    spdlog::info("[UpgradeBanner] Initialized (hidden)");
}

void UpgradeBanner::shutdown() {
    // Lifetimes reset before observers per #705 (same rule that bit thermistor).
    lifetime_.invalidate();
    status_observer_.reset();
    version_observer_.reset();

    if (banner_ && lv_is_initialized()) {
        lv_obj_delete(banner_);
    }
    banner_ = nullptr;

    if (message_subject_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&message_subject_);
    }
    message_subject_initialized_ = false;
}

void UpgradeBanner::refresh() {
    // Re-read UpgradeNudge intensity in case it was toggled via settings
    // without an explicit reload call. Cheap: a single lock + Config read.
    UpgradeNudge::instance().reload();
    evaluate_visibility();
}

void UpgradeBanner::update_message_text() {
    std::string version = UpgradeNudge::instance().get_available_version();
    if (version == last_rendered_version_) {
        return;
    }
    last_rendered_version_ = version;

    // Schema: "HelixScreen <version> is available — tap Update". Keep it
    // short; wider locales may wrap. The label uses long_mode="dots" so
    // overflow truncates rather than overflowing the banner height.
    std::string text;
    if (version.empty()) {
        text = lv_tr("An update is available");
    } else {
        text = fmt::format(lv_tr("HelixScreen {} is available — tap Update"), version);
    }

    size_t n = std::min<size_t>(text.size(), MESSAGE_BUF_SIZE - 1);
    std::memcpy(g_message_buf, text.data(), n);
    g_message_buf[n] = '\0';
    lv_subject_notify(&message_subject_);
}

void UpgradeBanner::evaluate_visibility() {
    if (!banner_) {
        return;
    }

    bool should_show = UpgradeNudge::instance().should_show_banner();
    if (should_show) {
        update_message_text();
        lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    }
}

void UpgradeBanner::on_update_clicked(lv_event_t* /*e*/) {
    spdlog::info("[UpgradeBanner] Update clicked — navigating to Settings");
    NavigationManager::instance().set_active(PanelId::Settings);
}

void UpgradeBanner::on_dismiss_clicked(lv_event_t* /*e*/) {
    spdlog::info("[UpgradeBanner] Dismissed for current version");
    UpgradeNudge::instance().dismiss_current_version();
    UpgradeBanner::instance().evaluate_visibility();
}

} // namespace helix
