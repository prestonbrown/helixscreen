// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_about.cpp
 * @brief Implementation of AboutSettingsOverlay
 */

#include "ui_settings_about.h"

#include "ui_callback_helpers.h"
#include "ui_panel_history_dashboard.h"
#include "ui_settings_updates.h"
#include "ui_snake_game.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#if __has_include("contributors.h")
#include "contributors.h"
#else
// Fallback when contributors.h is not generated (e.g., Android CMake builds
// without git available). Keep in sync with actual contributors from git log.
inline constexpr const char* CONTRIBUTORS[] = {
    "Andrew Basson",  "Justin Hayes", "Pierre Poissinger", "Preston Brown", "RNGIllSkillz",
    "Sergei Rozhkov", "Timo V",
};
inline constexpr int CONTRIBUTOR_COUNT = sizeof(CONTRIBUTORS) / sizeof(CONTRIBUTORS[0]);
#endif
#include "format_utils.h"
#include "helix_version.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "replace_method_callback.h"
#include "system/diagnostics.h"
#include "system/update_checker.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"
#include "wizard_config_paths.h"

#ifdef HELIX_HAS_TRACKER
#include "sound_manager.h"

#include <chrono>
#endif

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::settings {

AboutSettingsOverlay::~AboutSettingsOverlay() {
#ifdef HELIX_HAS_TRACKER
    helix::SoundManager::instance().stop_tracker();
#endif
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

        UI_MANAGED_SUBJECT_STRING(print_hours_value_subject_, print_hours_value_buf_,
                                  "\xe2\x80\x94", "print_hours_value", subjects_);

        // Copyright with compile-year range
        const char* compile_year = &__DATE__[7]; // last 4 chars of "Mon DD YYYY"
        snprintf(about_copyright_buf_, sizeof(about_copyright_buf_),
                 "\xc2\xa9 2025\xe2\x80\x93%s 356C LLC", compile_year);
        UI_MANAGED_SUBJECT_STRING(about_copyright_subject_, about_copyright_buf_,
                                  about_copyright_buf_, "about_copyright", subjects_);

        // One producer, read here as plain data. Nothing on this screen leaves
        // the machine, so the values are the raw ones.
        const helix::diagnostics::Diagnostics diag = helix::diagnostics::collect();

        UI_MANAGED_SUBJECT_STRING(install_root_value_subject_, install_root_value_buf_,
                                  diag.paths.install_root.c_str(), "install_root_value", subjects_);
        UI_MANAGED_SUBJECT_STRING(config_dir_value_subject_, config_dir_value_buf_,
                                  diag.paths.config_dir.c_str(), "config_dir_value", subjects_);
        UI_MANAGED_SUBJECT_STRING(cache_dir_value_subject_, cache_dir_value_buf_,
                                  diag.paths.cache_dir.c_str(), "cache_dir_value", subjects_);
        {
            std::string dest = diag.log.destination;
            if (dest.empty()) {
                dest = "\xe2\x80\x94"; // em dash when init hasn't completed
            }
            UI_MANAGED_SUBJECT_STRING(log_dest_value_subject_, log_dest_value_buf_, dest.c_str(),
                                      "log_dest_value", subjects_);
        }
        UI_MANAGED_SUBJECT_STRING(host_arch_value_subject_, host_arch_value_buf_,
                                  diag.identity.host_arch.c_str(), "host_arch_value", subjects_);
    });
}

// 7-tap easter egg constants (shared by version and printer name callbacks)
static constexpr int SECRET_TAP_COUNT = 7;
static constexpr uint32_t SECRET_TAP_TIMEOUT_MS = 2000;

void AboutSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_about_printer_name_clicked",
         [](lv_event_t*) {
             static int tap_count = 0;
             static uint32_t last_tap_time = 0;

             uint32_t now = lv_tick_get();

             if (now - last_tap_time > SECRET_TAP_TIMEOUT_MS) {
                 tap_count = 0;
             }
             last_tap_time = now;
             tap_count++;

             int remaining = SECRET_TAP_COUNT - tap_count;

             if (remaining > 0 && remaining <= 3) {
                 char buf[32];
                 snprintf(buf, sizeof(buf), "%d more tap%s...", remaining,
                          remaining == 1 ? "" : "s");
                 ToastManager::instance().show(ToastSeverity::INFO, buf, 800);
             } else if (remaining == 0) {
                 tap_count = 0;
                 spdlog::info("[AboutSettings] Snake easter egg triggered!");
                 helix::SnakeGame::show();
             }
         }},
        {"on_about_version_clicked",
         [](lv_event_t*) {
             static int tap_count = 0;
             static uint32_t last_tap_time = 0;

             uint32_t now = lv_tick_get();

             if (now - last_tap_time > SECRET_TAP_TIMEOUT_MS) {
                 tap_count = 0;
             }
             last_tap_time = now;
             tap_count++;

             int remaining = SECRET_TAP_COUNT - tap_count;

             if (remaining > 0 && remaining <= 3) {
                 Config* config = Config::get_instance();
                 bool currently_on = config->is_beta_features_enabled();
                 const char* action = currently_on ? lv_tr("disable") : lv_tr("enable");
                 std::string msg =
                     remaining == 1 ? fmt::format(lv_tr("1 more tap to {} beta features"), action)
                                    : fmt::format(lv_tr("{} more taps to {} beta features"),
                                                  remaining, action);
                 ToastManager::instance().show(ToastSeverity::INFO, msg.c_str(), 1000);
             } else if (remaining == 0) {
                 Config* config = Config::get_instance();
                 bool currently_enabled = config->is_beta_features_enabled();
                 bool new_value = !currently_enabled;
                 config->set("/beta_features", new_value);
                 config->save();

                 lv_subject_t* subject = lv_xml_get_subject(nullptr, "show_beta_features");
                 if (subject) {
                     lv_subject_set_int(subject, new_value ? 1 : 0);
                 }

                 // The two Update Channel rows swap on that subject, and toggling
                 // beta also moves the effective channel whenever Dev is stored, so
                 // the row arriving on screen needs its selection re-seeded. A root
                 // of nullptr (Updates never opened) is a no-op.
                 UpdatesSettingsOverlay::sync_update_channel_rows(
                     get_updates_settings_overlay().get_root(),
                     static_cast<int>(UpdateChecker::instance().get_channel()));
                 // With Dev stored, beta decides whether Moonraker should offer beta.
                 UpdateChecker::instance().sync_moonraker_channel();

                 ToastManager::instance().show(
                     ToastSeverity::SUCCESS,
                     new_value ? lv_tr("Beta features: ON") : lv_tr("Beta features: OFF"), 1500);
                 spdlog::info("[AboutSettings] Beta features toggled via 7-tap secret: {}",
                              new_value ? "ON" : "OFF");

                 tap_count = 0;
             }
         }},
        {"on_about_print_hours_clicked",
         [](lv_event_t*) {
             auto& self = get_about_settings_overlay();
             get_global_history_dashboard_panel().show(self.parent_screen_);
         }},
    });
}

lv_obj_t* AboutSettingsOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, xml_component())) {
        return nullptr;
    }
    setup_contributor_marquee();
    return overlay_root_;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void AboutSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    // Refresh info rows with current data
    populate_info_rows();
    fetch_print_hours();

    // Re-trigger marquee scroll animation now that the overlay is visible and laid out.
    // The label may have been created while the overlay was hidden, so LVGL couldn't
    // determine the correct text bounds to start the scroll animation.
    if (marquee_content_) {
        lv_label_set_long_mode(marquee_content_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

#ifdef HELIX_HAS_TRACKER
    // Debounce: don't restart tracker if we just deactivated (inadvertent re-open from
    // touch lift registering as click on the About row in Settings)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_deactivate_);
    if (elapsed.count() > 500) {
        helix::SoundManager::instance().play_file("assets/sounds/space_debris.mod",
                                                  SoundPriority::EVENT);
    } else {
        spdlog::debug("[{}] Skipping tracker start - re-activated {}ms after deactivation",
                      get_name(), elapsed.count());
    }
#endif
}

void AboutSettingsOverlay::on_deactivating(DeactivateReason) {
#ifdef HELIX_HAS_TRACKER
    helix::SoundManager::instance().stop_tracker();
    last_deactivate_ = std::chrono::steady_clock::now();
#endif
}

// ============================================================================
// CONTRIBUTOR MARQUEE
// ============================================================================

void AboutSettingsOverlay::setup_contributor_marquee() {
    lv_obj_t* marquee_container =
        helix::ui::find_required(overlay_root_, "contributor_marquee", get_name());
    if (!marquee_container) {
        return;
    }

    // Build a single concatenated string: "Name1  •  Name2  •  Name3  •  "
    // Trailing separator ensures continuity when LVGL wraps circular scroll
    std::string text;
    for (int i = 0; i < CONTRIBUTOR_COUNT; i++) {
        if (i > 0) {
            text += "  \xe2\x80\xa2  ";
        }
        text += CONTRIBUTORS[i];
    }
    text += "  \xe2\x80\xa2  ";

    // Single label with LVGL's built-in scroll long mode
    marquee_content_ = lv_label_create(marquee_container);
    lv_obj_set_width(marquee_content_, lv_pct(100));
    lv_label_set_long_mode(marquee_content_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(marquee_content_, text.c_str());
    lv_obj_set_style_text_color(marquee_content_, theme_manager_get_color("text_subtle"), 0);
    lv_obj_set_style_anim_duration(marquee_content_, text.size() * 100, 0);

    spdlog::debug("[{}] Contributor marquee set up with {} contributors", get_name(),
                  CONTRIBUTOR_COUNT);
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
    std::string printer_name =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, lv_tr("Unknown"));
    lv_subject_copy_string(&printer_value_subject_, printer_name.c_str());
    spdlog::trace("[{}] Printer: {}", get_name(), printer_name);
}

void AboutSettingsOverlay::fetch_print_hours() {
    // Called after discovery, before the overlay is ever shown.
    if (!subjects_initialized_) {
        init_subjects();
    }

    auto* api = get_moonraker_api();
    if (!api)
        return;

    // Both callbacks fire on the HTTP thread and touch members (`get_name()` in
    // the error path as much as the subject write in the success path), so both
    // go through bg_cb — it marshals to the main thread and drops the body if the
    // overlay was torn down while the request was in flight (#1165).
    api->history().get_history_totals(
        lifetime_.bg_cb("AboutSettingsOverlay::get_history_totals",
                        [this](const PrintHistoryTotals& totals) {
                            if (!subjects_initialized_) {
                                return;
                            }
                            std::string formatted =
                                helix::format::duration(static_cast<int>(totals.total_time));
                            lv_subject_copy_string(&print_hours_value_subject_, formatted.c_str());
                            spdlog::trace("[{}] Print hours updated: {}", get_name(), formatted);
                        }),
        lifetime_.bg_cb(
            "AboutSettingsOverlay::get_history_totals_error", [this](const MoonrakerError& err) {
                spdlog::warn("[{}] Failed to fetch print hours: {}", get_name(), err.message);
            }));
}

namespace {
constexpr const char* HISTORY_METHOD = "notify_history_changed";
constexpr const char* PRINT_HOURS_HANDLER = "AboutOverlay_print_hours";
} // namespace

void AboutSettingsOverlay::attach_print_hours(IMoonrakerClient& client) {
    fetch_print_hours();
    replace_method_callback(
        client, HISTORY_METHOD, PRINT_HOURS_HANDLER, [](const nlohmann::json& data) {
            // Moonraker accumulates its job totals only when a job
            // finishes, so the "added" half of this notification cannot
            // move print hours and must not cost a round-trip.
            if (helix::json_util::notification_action(data) != "finished") {
                return;
            }
            helix::ui::queue_update("AboutSettingsOverlay::attach_print_hours",
                                    []() { get_about_settings_overlay().fetch_print_hours(); });
        });
}

void AboutSettingsOverlay::detach_print_hours(IMoonrakerClient& client) {
    client.unregister_method_callback(HISTORY_METHOD, PRINT_HOURS_HANDLER);
}

} // namespace helix::settings
