// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_about.cpp
 * @brief Implementation of AboutSettingsOverlay
 */

#include "ui_settings_about.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_history_dashboard.h"
#include "ui_snake_game.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#if __has_include("contributors.h")
#include "contributors.h"
#else
// Fallback when contributors.h is not generated (e.g., Android CMake builds)
inline constexpr const char* kContributors[] = {"HelixScreen Contributors"};
inline constexpr int kContributorCount = 1;
#endif
#include "format_utils.h"
#include "helix_version.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "static_panel_registry.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "ui/ui_lazy_panel_helper.h"
#include "wizard_config_paths.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AboutSettingsOverlay> g_about_settings_overlay;

AboutSettingsOverlay& get_about_settings_overlay() {
    if (!g_about_settings_overlay) {
        g_about_settings_overlay = std::make_unique<AboutSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy("AboutSettingsOverlay",
                                                         []() { g_about_settings_overlay.reset(); });
    }
    return *g_about_settings_overlay;
}

void destroy_about_settings_overlay() {
    g_about_settings_overlay.reset();
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AboutSettingsOverlay::AboutSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AboutSettingsOverlay::~AboutSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AboutSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(version_value_subject_, version_value_buf_, "\xe2\x80\x94",
                                  "version_value", subjects_);

        UI_MANAGED_SUBJECT_STRING(about_version_description_subject_,
                                  about_version_description_buf_, "\xe2\x80\x94",
                                  "about_version_description", subjects_);

        UI_MANAGED_SUBJECT_STRING(printer_value_subject_, printer_value_buf_, "\xe2\x80\x94",
                                  "printer_value", subjects_);

        UI_MANAGED_SUBJECT_STRING(print_hours_value_subject_, print_hours_value_buf_, "\xe2\x80\x94",
                                  "print_hours_value", subjects_);

        UI_MANAGED_SUBJECT_STRING(update_current_version_subject_, update_current_version_buf_,
                                  helix_version(), "update_current_version", subjects_);

        // Copyright with compile-year range
        const char* compile_year = __DATE__ + 7; // last 4 chars of "Mon DD YYYY"
        snprintf(about_copyright_buf_, sizeof(about_copyright_buf_),
                 "\xc2\xa9 2025\xe2\x80\x93%s 356C LLC", compile_year);
        UI_MANAGED_SUBJECT_STRING(about_copyright_subject_, about_copyright_buf_,
                                  about_copyright_buf_, "about_copyright", subjects_);
    });
}

void AboutSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_about_printer_name_clicked", on_about_printer_name_clicked},
        {"on_about_version_clicked", on_about_version_clicked},
        {"on_about_update_channel_changed", on_about_update_channel_changed},
        {"on_about_check_updates_clicked", on_about_check_updates_clicked},
        {"on_about_install_update_clicked", on_about_install_update_clicked},
        {"on_about_print_hours_clicked", on_about_print_hours_clicked},
        {"on_update_download_start", on_about_update_download_start},
        {"on_update_download_cancel", on_about_update_download_cancel},
        {"on_update_download_dismiss", on_about_update_download_dismiss},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AboutSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "about_settings_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Set up the contributor marquee
    setup_contributor_marquee();

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void AboutSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack (on_activate will initialize widgets)
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void AboutSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    // Refresh info rows with current data
    populate_info_rows();
    fetch_print_hours();
}

void AboutSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// CONTRIBUTOR MARQUEE
// ============================================================================

void AboutSettingsOverlay::setup_contributor_marquee() {
    if (!overlay_root_)
        return;

    lv_obj_t* marquee_container = lv_obj_find_by_name(overlay_root_, "contributor_marquee");
    if (!marquee_container) {
        spdlog::warn("[{}] contributor_marquee container not found", get_name());
        return;
    }

    // Build a single concatenated string: "Name1  •  Name2  •  Name3"
    std::string text;
    for (int i = 0; i < kContributorCount; i++) {
        if (i > 0) {
            text += "  \xe2\x80\xa2  ";
        }
        text += kContributors[i];
    }

    // Single label with LVGL's built-in scroll long mode
    marquee_content_ = lv_label_create(marquee_container);
    lv_obj_set_width(marquee_content_, lv_pct(100));
    lv_label_set_long_mode(marquee_content_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(marquee_content_, text.c_str());
    lv_obj_set_style_text_color(marquee_content_, theme_manager_get_color("text_subtle"), 0);
    lv_obj_set_style_anim_duration(marquee_content_, text.size() * 100, 0);

    spdlog::debug("[{}] Contributor marquee set up with {} contributors", get_name(),
                  kContributorCount);
}

// ============================================================================
// INFO ROWS
// ============================================================================

void AboutSettingsOverlay::populate_info_rows() {
    // Version
    lv_subject_copy_string(&version_value_subject_, helix_version());
    std::string about_desc = std::string(lv_tr("Current Version")) + ": " + helix_version();
    lv_subject_copy_string(&about_version_description_subject_, about_desc.c_str());
    spdlog::trace("[{}] Version subject: {}", get_name(), helix_version());

    // Printer name from config
    Config* config = Config::get_instance();
    if (config) {
        std::string printer_name =
            config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "Unknown");
        lv_subject_copy_string(&printer_value_subject_, printer_name.c_str());
        spdlog::trace("[{}] Printer: {}", get_name(), printer_name);
    }
}

void AboutSettingsOverlay::fetch_print_hours() {
    // Ensure subjects are initialized (may be called before overlay is shown)
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    auto* api = get_moonraker_api();
    if (!api)
        return;

    api->history().get_history_totals(
        [this](const PrintHistoryTotals& totals) {
            std::string formatted = helix::format::duration(static_cast<int>(totals.total_time));
            helix::ui::queue_update([this, formatted]() {
                if (subjects_initialized_) {
                    lv_subject_copy_string(&print_hours_value_subject_, formatted.c_str());
                    spdlog::trace("[{}] Print hours updated: {}", get_name(), formatted);
                }
            });
        },
        [this](const MoonrakerError& err) {
            spdlog::warn("[{}] Failed to fetch print hours: {}", get_name(), err.message);
        });
}

// ============================================================================
// UPDATE DOWNLOAD MODAL
// ============================================================================

void AboutSettingsOverlay::show_update_download_modal() {
    // Ensure callbacks are registered (modal may be shown before the overlay)
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!update_download_modal_) {
        update_download_modal_ = helix::ui::modal_show("update_download_modal");
    }

    // Set to Confirming state with version info
    auto info = UpdateChecker::instance().get_cached_update();
    std::string text = info ? fmt::format(lv_tr("Download v{}?"), info->version)
                            : std::string(lv_tr("Download update?"));
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Confirming, 0,
                                                     text);
}

void AboutSettingsOverlay::hide_update_download_modal() {
    if (update_download_modal_) {
        helix::ui::modal_hide(update_download_modal_);
        update_download_modal_ = nullptr;
    }
    // Reset download state
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

// 7-tap easter egg constants (shared by version and printer name callbacks)
static constexpr int kSecretTapCount = 7;
static constexpr uint32_t kSecretTapTimeoutMs = 2000;

void AboutSettingsOverlay::on_about_printer_name_clicked(lv_event_t*) {
    static int tap_count = 0;
    static uint32_t last_tap_time = 0;

    uint32_t now = lv_tick_get();

    if (now - last_tap_time > kSecretTapTimeoutMs) {
        tap_count = 0;
    }
    last_tap_time = now;
    tap_count++;

    int remaining = kSecretTapCount - tap_count;

    if (remaining > 0 && remaining <= 3) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d more tap%s...", remaining, remaining == 1 ? "" : "s");
        ToastManager::instance().show(ToastSeverity::INFO, buf, 800);
    } else if (remaining == 0) {
        tap_count = 0;
        spdlog::info("[AboutSettings] Snake easter egg triggered!");
        helix::SnakeGame::show();
    }
}

void AboutSettingsOverlay::on_about_version_clicked(lv_event_t*) {
    static int tap_count = 0;
    static uint32_t last_tap_time = 0;

    uint32_t now = lv_tick_get();

    if (now - last_tap_time > kSecretTapTimeoutMs) {
        tap_count = 0;
    }
    last_tap_time = now;
    tap_count++;

    int remaining = kSecretTapCount - tap_count;

    if (remaining > 0 && remaining <= 3) {
        Config* config = Config::get_instance();
        bool currently_on = config && config->is_beta_features_enabled();
        const char* action = currently_on ? lv_tr("disable") : lv_tr("enable");
        std::string msg =
            remaining == 1
                ? fmt::format(lv_tr("1 more tap to {} beta features"), action)
                : fmt::format(lv_tr("{} more taps to {} beta features"), remaining, action);
        ToastManager::instance().show(ToastSeverity::INFO, msg.c_str(), 1000);
    } else if (remaining == 0) {
        Config* config = Config::get_instance();
        if (config) {
            bool currently_enabled = config->is_beta_features_enabled();
            bool new_value = !currently_enabled;
            config->set("/beta_features", new_value);
            config->save();

            lv_subject_t* subject = lv_xml_get_subject(nullptr, "show_beta_features");
            if (subject) {
                lv_subject_set_int(subject, new_value ? 1 : 0);
            }

            ToastManager::instance().show(
                ToastSeverity::SUCCESS,
                new_value ? lv_tr("Beta features: ON") : lv_tr("Beta features: OFF"), 1500);
            spdlog::info("[AboutSettings] Beta features toggled via 7-tap secret: {}",
                         new_value ? "ON" : "OFF");
        }
        tap_count = 0;
    }
}

void AboutSettingsOverlay::on_about_update_channel_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_update_channel_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));

    bool rejected = false;
    if (index == 2) {
        auto* config = Config::get_instance();
        std::string dev_url = config ? config->get<std::string>("/update/dev_url", "") : "";
        if (dev_url.empty()) {
            spdlog::warn("[AboutSettings] Dev channel selected but no dev_url configured");
            int current = SystemSettingsManager::instance().get_update_channel();
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current));
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("Dev channel requires dev_url in config"), 3000);
            rejected = true;
        }
    }

    if (!rejected) {
        spdlog::info("[AboutSettings] Update channel changed: {} ({})", index,
                     index == 0 ? "Stable" : (index == 1 ? "Beta" : "Dev"));
        SystemSettingsManager::instance().set_update_channel(index);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_check_updates_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_check_updates_clicked");
    spdlog::info("[AboutSettings] Check for updates requested");
    UpdateChecker::instance().check_for_updates();
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_install_update_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_install_update_clicked");
    spdlog::info("[AboutSettings] Install update requested");
    get_about_settings_overlay().show_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_print_hours_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_print_hours_clicked");
    get_about_settings_overlay().handle_print_hours_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_update_download_start(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_update_download_start");
    spdlog::info("[AboutSettings] Starting update download");
    UpdateChecker::instance().start_download();
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_update_download_cancel(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_update_download_cancel");
    spdlog::info("[AboutSettings] Download cancelled by user");
    UpdateChecker::instance().cancel_download();
    get_about_settings_overlay().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

void AboutSettingsOverlay::on_about_update_download_dismiss(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_update_download_dismiss");
    get_about_settings_overlay().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// PRIVATE HANDLERS
// ============================================================================

void AboutSettingsOverlay::handle_print_hours_clicked() {
    helix::ui::lazy_create_and_push_overlay<HistoryDashboardPanel>(
        get_global_history_dashboard_panel, history_dashboard_panel_, parent_screen_,
        "Print History", get_name());
}

} // namespace helix::settings
