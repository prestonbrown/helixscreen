// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_spoolman_overlay.cpp
 * @brief Implementation of SpoolmanOverlay
 */

#include "ui_spoolman_overlay.h"

#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ui_settings_label_printer.h"
#endif
#include "ui_button.h"
#include "ui_settings_barcode_scanner.h"
#include "ui_spoolman_setup.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "config.h"
#include "host_identity.h"
#include "http_executor.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "moonraker_config_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <unistd.h>

#include "hv/json.hpp"

namespace helix::ui {

// Database keys for settings persistence
static constexpr const char* DB_NAMESPACE = "helix-screen";
static constexpr const char* DB_KEY_SYNC_ENABLED = "spoolman_sync_enabled";
static constexpr const char* DB_KEY_REFRESH_INTERVAL = "spoolman_weight_refresh_interval";

// Legacy keys (pre-rename migration)
static constexpr const char* LEGACY_DB_KEY_SYNC_ENABLED = "ams_spoolman_sync_enabled";
static constexpr const char* LEGACY_DB_KEY_REFRESH_INTERVAL = "ams_weight_refresh_interval";

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SpoolmanOverlay> g_spoolman_overlay;

SpoolmanOverlay& get_spoolman_overlay() {
    if (!g_spoolman_overlay) {
        g_spoolman_overlay = std::make_unique<SpoolmanOverlay>();
        StaticPanelRegistry::instance().register_destroy("SpoolmanOverlay",
                                                         []() { g_spoolman_overlay.reset(); });
    }
    return *g_spoolman_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SpoolmanOverlay::SpoolmanOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SpoolmanOverlay::~SpoolmanOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&sync_enabled_subject_);
        lv_subject_deinit(&refresh_interval_subject_);
        lv_subject_deinit(&scanner_device_status_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SpoolmanOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize sync enabled subject (default: true/enabled)
    lv_subject_init_int(&sync_enabled_subject_, DEFAULT_SYNC_ENABLED ? 1 : 0);
    lv_xml_register_subject(nullptr, "ams_spoolman_sync_enabled", &sync_enabled_subject_);

    // Initialize refresh interval subject (default: 30 seconds)
    lv_subject_init_int(&refresh_interval_subject_, DEFAULT_REFRESH_INTERVAL_SECONDS);
    lv_xml_register_subject(nullptr, "ams_spoolman_refresh_interval", &refresh_interval_subject_);

    // Initialize scanner device status subject
    auto scanner_name = helix::SettingsManager::instance().get_scanner_device_name();
    auto scanner_id = helix::SettingsManager::instance().get_scanner_device_id();
    const char* status = scanner_id.empty() ? lv_tr("Auto-detect") : scanner_name.c_str();
    snprintf(scanner_status_buf_, sizeof(scanner_status_buf_), "%s", status);
    lv_subject_init_string(&scanner_device_status_subject_, scanner_status_buf_, nullptr,
                           sizeof(scanner_status_buf_), scanner_status_buf_);
    lv_xml_register_subject(nullptr, "scanner_device_status", &scanner_device_status_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SpoolmanOverlay::register_callbacks() {
    // Register sync toggle callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_sync_toggled", on_sync_toggled);

    // Register interval dropdown callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_interval_changed", on_interval_changed);

#if HELIX_HAS_LABEL_PRINTER
    // Label printer sub-panel launcher
    lv_xml_register_event_cb(nullptr, "on_spoolman_label_printer_clicked",
                             on_label_printer_clicked);
#endif

    // Barcode scanner picker callback
    lv_xml_register_event_cb(nullptr, "on_barcode_scanner_clicked", on_barcode_scanner_clicked);

    // Server setup callbacks
    lv_xml_register_event_cb(nullptr, "on_spoolman_connect_clicked", on_connect_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_cancel_setup_clicked", on_cancel_setup_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_change_clicked", on_change_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_remove_clicked", on_remove_clicked);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SpoolmanOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "spoolman_settings", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find control widgets for programmatic access
    sync_toggle_ = lv_obj_find_by_name(overlay_, "sync_toggle");
    interval_dropdown_ = lv_obj_find_by_name(overlay_, "interval_dropdown");

    // Server setup widgets
    host_input_ = lv_obj_find_by_name(overlay_, "spoolman_host_input");
    port_input_ = lv_obj_find_by_name(overlay_, "spoolman_port_input");
    setup_status_text_ = lv_obj_find_by_name(overlay_, "setup_status_text");
    server_url_text_ = lv_obj_find_by_name(overlay_, "server_url_text");
    connect_btn_ = lv_obj_find_by_name(overlay_, "connect_btn");
    setup_card_ = lv_obj_find_by_name(overlay_, "setup_card");
    status_card_ = lv_obj_find_by_name(overlay_, "status_card");

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

void SpoolmanOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Load settings from database
    load_from_database();

    // Update UI controls to match subject values
    update_ui_from_subjects();

    // Show current server URL if connected
    update_server_url_display();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_, this);

    // Register close callback to destroy widget tree when overlay closes
    NavigationManager::instance().register_overlay_close_callback(overlay_, [this]() {
        // overlay_ is an alias for overlay_root_, so pass it directly
        destroy_overlay_ui(overlay_);
    });

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_);
}

void SpoolmanOverlay::refresh() {
    if (!overlay_) {
        return;
    }

    load_from_database();
    update_ui_from_subjects();
}

// ============================================================================
// DATABASE OPERATIONS
// ============================================================================

void SpoolmanOverlay::load_from_database() {
    if (!api_) {
        spdlog::warn("[{}] No API available, using default values", get_name());
        return;
    }

    // Helper: apply sync_enabled value and update polling
    auto apply_sync = [this](bool enabled) {
        lv_subject_set_int(&sync_enabled_subject_, enabled ? 1 : 0);
        spdlog::debug("[{}] Loaded sync_enabled={} from database", get_name(), enabled);
        set_poll_ref(enabled);
    };

    auto parse_sync = [](const nlohmann::json& value, bool default_val) {
        if (value.is_boolean())
            return value.get<bool>();
        if (value.is_number())
            return value.get<int>() != 0;
        return default_val;
    };

    // Load sync enabled — try new key, fall back to legacy key.
    // database_get_item() callbacks fire inline on the libhv WebSocket thread, and
    // apply_sync() touches subjects plus SpoolmanManager's LVGL poll timer — every
    // callback below must be marshalled to the main thread.
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED,
        lifetime_.bg_cb("SpoolmanOverlay::load_sync",
                        [this, apply_sync, parse_sync](const nlohmann::json& value) {
                            apply_sync(parse_sync(value, DEFAULT_SYNC_ENABLED));
                        }),
        lifetime_.bg_cb("SpoolmanOverlay::load_sync_missing", [this, apply_sync,
                                                               parse_sync](const MoonrakerError&) {
            // New key not found — try legacy key
            api_->database_get_item(
                DB_NAMESPACE, LEGACY_DB_KEY_SYNC_ENABLED,
                lifetime_.bg_cb("SpoolmanOverlay::load_sync_legacy",
                                [this, apply_sync, parse_sync](const nlohmann::json& value) {
                                    bool enabled = parse_sync(value, DEFAULT_SYNC_ENABLED);
                                    apply_sync(enabled);
                                    // Migrate to new key
                                    save_sync_enabled(enabled);
                                    spdlog::info("[{}] Migrated {} -> {}", get_name(),
                                                 LEGACY_DB_KEY_SYNC_ENABLED, DB_KEY_SYNC_ENABLED);
                                }),
                lifetime_.bg_cb("SpoolmanOverlay::load_sync_default",
                                [this, apply_sync](const MoonrakerError&) {
                                    apply_sync(DEFAULT_SYNC_ENABLED);
                                }));
        }));

    // Helper: apply refresh interval
    auto apply_interval = [this](int interval) {
        lv_subject_set_int(&refresh_interval_subject_, interval);
        spdlog::debug("[{}] Loaded refresh_interval={} from database", get_name(), interval);
    };

    auto parse_interval = [](const nlohmann::json& value, int default_val) {
        return value.is_number() ? value.get<int>() : default_val;
    };

    // Load refresh interval — try new key, fall back to legacy key
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL,
        lifetime_.bg_cb("SpoolmanOverlay::load_interval",
                        [this, apply_interval, parse_interval](const nlohmann::json& value) {
                            apply_interval(parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS));
                        }),
        lifetime_.bg_cb(
            "SpoolmanOverlay::load_interval_missing",
            [this, apply_interval, parse_interval](const MoonrakerError&) {
                // New key not found — try legacy key
                api_->database_get_item(
                    DB_NAMESPACE, LEGACY_DB_KEY_REFRESH_INTERVAL,
                    lifetime_.bg_cb(
                        "SpoolmanOverlay::load_interval_legacy",
                        [this, apply_interval, parse_interval](const nlohmann::json& value) {
                            int interval = parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS);
                            apply_interval(interval);
                            // Migrate to new key
                            save_refresh_interval(interval);
                            spdlog::info("[{}] Migrated {} -> {}", get_name(),
                                         LEGACY_DB_KEY_REFRESH_INTERVAL, DB_KEY_REFRESH_INTERVAL);
                        }),
                    lifetime_.bg_cb("SpoolmanOverlay::load_interval_default",
                                    [this, apply_interval](const MoonrakerError&) {
                                        apply_interval(DEFAULT_REFRESH_INTERVAL_SECONDS);
                                    }));
            }));
}

void SpoolmanOverlay::save_sync_enabled(bool enabled) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED, enabled,
        [this, enabled]() {
            spdlog::info("[{}] Saved sync_enabled={} to database", get_name(), enabled);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save sync_enabled: {}", get_name(), err.message);
        });
}

void SpoolmanOverlay::save_refresh_interval(int interval_seconds) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL, interval_seconds,
        [this, interval_seconds]() {
            spdlog::info("[{}] Saved refresh_interval={} to database", get_name(),
                         interval_seconds);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save refresh_interval: {}", get_name(), err.message);
        });
}

// ============================================================================
// UTILITY METHODS
// ============================================================================

int SpoolmanOverlay::dropdown_index_to_seconds(int index) {
    // Dropdown options: "30s", "1 min", "2 min", "5 min"
    switch (index) {
    case 0:
        return 30;
    case 1:
        return 60;
    case 2:
        return 120;
    case 3:
        return 300;
    default:
        return 30;
    }
}

int SpoolmanOverlay::seconds_to_dropdown_index(int seconds) {
    switch (seconds) {
    case 30:
        return 0;
    case 60:
        return 1;
    case 120:
        return 2;
    case 300:
        return 3;
    default:
        return 0; // Default to 30s
    }
}

void SpoolmanOverlay::update_ui_from_subjects() {
    // Update dropdown to match current interval
    if (interval_dropdown_) {
        int interval_seconds = lv_subject_get_int(&refresh_interval_subject_);
        int dropdown_index = seconds_to_dropdown_index(interval_seconds);
        lv_dropdown_set_selected(interval_dropdown_, static_cast<uint32_t>(dropdown_index));
    }

    // Toggle state is handled by subject binding in XML
}

void SpoolmanOverlay::set_poll_ref(bool want_ref) {
    if (want_ref == holds_poll_ref_) {
        return;
    }

    if (want_ref) {
        SpoolmanManager::instance().start_spoolman_polling();
    } else {
        SpoolmanManager::instance().stop_spoolman_polling();
    }
    holds_poll_ref_ = want_ref;

    spdlog::debug("[{}] Spoolman poll reference {}", get_name(),
                  want_ref ? "acquired" : "released");
}

void SpoolmanOverlay::on_ui_destroyed() {
    sync_toggle_ = nullptr;
    interval_dropdown_ = nullptr;
    host_input_ = nullptr;
    port_input_ = nullptr;
    setup_status_text_ = nullptr;
    server_url_text_ = nullptr;
    connect_btn_ = nullptr;
    setup_card_ = nullptr;
    status_card_ = nullptr;
}

void SpoolmanOverlay::on_activate() {
    OverlayBase::on_activate();
    // Scanner selection may have changed in the child overlay; re-sync the row.
    update_scanner_status_text();

    // Re-take the poll reference on_deactivate() gave back. Pushing a child
    // overlay (barcode scanner, label printer) deactivates this one, and the
    // async load_from_database() that first took the reference does not re-run
    // on the way back.
    if (subjects_initialized_) {
        set_poll_ref(lv_subject_get_int(&sync_enabled_subject_) != 0);
    }
}

void SpoolmanOverlay::on_deactivate() {
    // Give back the poll reference load_from_database() took. The panel underneath
    // takes its own on activation, so dropping ours here does not starve it of data.
    set_poll_ref(false);

    OverlayBase::on_deactivate();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SpoolmanOverlay::on_sync_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_sync_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);

        spdlog::info("[SpoolmanOverlay] Sync toggle: {}", is_checked ? "enabled" : "disabled");

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.sync_enabled_subject_, is_checked ? 1 : 0);

        // Save to database
        overlay.save_sync_enabled(is_checked);

        // Update Spoolman polling
        overlay.set_poll_ref(is_checked);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_interval_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_interval_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown || !lv_obj_is_valid(dropdown)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - dropdown no longer valid");
    } else {
        int selected = static_cast<int>(lv_dropdown_get_selected(dropdown));
        int interval_seconds = dropdown_index_to_seconds(selected);

        spdlog::info("[SpoolmanOverlay] Interval changed: {}s", interval_seconds);

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.refresh_interval_subject_, interval_seconds);

        // Save to database
        overlay.save_refresh_interval(interval_seconds);

        // Note: The actual polling interval in AmsState is currently fixed at 30s.
        // This setting is stored for future use when configurable polling is implemented.
        // For now, we just persist the user's preference.
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// LABEL PRINTER SUB-PANEL
// ============================================================================

#if HELIX_HAS_LABEL_PRINTER
void SpoolmanOverlay::on_label_printer_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_label_printer_clicked");
    auto& overlay = helix::settings::get_label_printer_settings_overlay();
    auto& spoolman = get_spoolman_overlay();
    overlay.show(spoolman.parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}
#endif

// ============================================================================
// SERVER SETUP
// ============================================================================

void SpoolmanOverlay::set_setup_status(const char* text, bool is_error) {
    if (setup_status_text_) {
        lv_label_set_text(setup_status_text_, text);
        lv_obj_set_style_text_color(setup_status_text_,
                                    theme_manager_get_color(is_error ? "danger" : "text_muted"),
                                    LV_PART_MAIN);
    }
}

void SpoolmanOverlay::set_connecting(bool connecting) {
    if (!connect_btn_ || !lv_obj_is_valid(connect_btn_))
        return;
    if (connecting) {
        lv_obj_add_state(connect_btn_, LV_STATE_DISABLED);
        ui_button_set_text(connect_btn_, lv_tr("Connecting..."));
    } else {
        lv_obj_remove_state(connect_btn_, LV_STATE_DISABLED);
        ui_button_set_text(connect_btn_, lv_tr("Connect"));
    }
}

// ============================================================================
// BARCODE SCANNER PICKER
// ============================================================================

void SpoolmanOverlay::on_barcode_scanner_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_barcode_scanner_clicked");
    get_spoolman_overlay().handle_barcode_scanner_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::handle_barcode_scanner_clicked() {
    spdlog::debug("[{}] Barcode Scanner clicked - opening settings overlay", get_name());
    helix::ui::get_barcode_scanner_settings_overlay().show(parent_screen_);
}

void SpoolmanOverlay::update_scanner_status_text() {
    auto id = helix::SettingsManager::instance().get_scanner_device_id();
    auto name = helix::SettingsManager::instance().get_scanner_device_name();
    const char* status = id.empty() ? lv_tr("Auto-detect") : name.c_str();
    snprintf(scanner_status_buf_, sizeof(scanner_status_buf_), "%s", status);
    lv_subject_copy_string(&scanner_device_status_subject_, scanner_status_buf_);
}

// ============================================================================
// SERVER SETUP
// ============================================================================

void SpoolmanOverlay::on_connect_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_connect_clicked");

    auto& overlay = get_spoolman_overlay();
    const char* host_raw = overlay.host_input_ ? lv_textarea_get_text(overlay.host_input_) : "";
    const char* port_raw = overlay.port_input_ ? lv_textarea_get_text(overlay.port_input_) : "";

    std::string host(host_raw ? host_raw : "");
    std::string port(port_raw ? port_raw : "");

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };
    trim(host);
    trim(port);

    if (port.empty())
        port = DEFAULT_SPOOLMAN_PORT;

    if (!SpoolmanSetup::validate_host(host)) {
        overlay.set_setup_status(lv_tr("Please enter an IP address or hostname."), true);
        return;
    }
    if (!SpoolmanSetup::validate_port(port)) {
        overlay.set_setup_status(lv_tr("Please enter a valid port (1-65535)."), true);
        return;
    }

    overlay.set_connecting(true);
    overlay.set_setup_status(lv_tr("Checking Spoolman server..."));
    overlay.probe_spoolman_server(host, port);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::probe_spoolman_server(const std::string& host, const std::string& port) {
    // In mock mode, skip the real HTTP probe and simulate success
    if (get_runtime_config() && get_runtime_config()->should_mock_moonraker()) {
        spdlog::info("[{}] Mock mode — skipping HTTP probe, simulating Spoolman at {}:{}",
                     get_name(), host, port);
        set_setup_status(lv_tr("Spoolman found! Configuring..."));
        configure_spoolman(host, port);
        return;
    }

    auto token = lifetime_.token();
    std::string probe_url = SpoolmanSetup::build_probe_url(host, port);
    std::string host_copy = host;
    std::string port_copy = port;

    spdlog::info("[{}] Probing Spoolman at {}", get_name(), probe_url);

    // Route through HttpExecutor::fast() (bounded 4-worker pool) instead of
    // spawning a detached std::thread per click. Raw thread creation failed
    // with pthread EAGAIN under thread exhaustion on memory-constrained ARM
    // devices (#724) and crashed when users double-tapped the Connect button.
    helix::http::HttpExecutor::fast().submit([this, token, probe_url, host_copy, port_copy]() {
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = probe_url;
        req->timeout = 5;
        auto resp = requests::request(req);
        bool success = (resp && resp->status_code == 200);

        if (token.expired())
            return;
        token.defer("SpoolmanOverlay::probe_result", [this, success, host_copy, port_copy]() {
            if (success) {
                spdlog::info("[{}] Spoolman probe succeeded", get_name());
                set_setup_status(lv_tr("Spoolman found! Configuring..."));
                resolve_config_location(host_copy, port_copy);
            } else {
                spdlog::warn("[{}] Spoolman probe failed at {}:{}", get_name(), host_copy,
                             port_copy);
                auto msg = fmt::format("{} {}:{}", lv_tr("Could not reach Spoolman at"), host_copy,
                                       port_copy);
                set_setup_status(msg.c_str(), true);
                set_connecting(false);
            }
        });
    });
}

// ============================================================================
// CONFIG WRITE CHAIN
// ============================================================================

void SpoolmanOverlay::report_spoolman_error(const char* status_text, const char* toast_text,
                                            const std::string& detail) {
    spdlog::error("[{}] {}", get_name(), detail);
    set_setup_status(lv_tr(status_text), true);
    ToastManager::instance().show(ToastSeverity::ERROR, lv_tr(toast_text), 6000);
    set_connecting(false);
}

void SpoolmanOverlay::fail_config_unreachable(const std::string& detail) {
    report_spoolman_error("Moonraker's config file is not writable from HelixScreen. "
                          "Add the [spoolman] section by hand — see the log for details.",
                          "Moonraker config is not writable from HelixScreen.",
                          "Cannot write Spoolman config: " + detail);
}

void SpoolmanOverlay::fail_config_ambiguous(const std::string& detail) {
    report_spoolman_error("Your Moonraker config defines [spoolman] in more than one file. "
                          "Remove the extras and try again — see the log for details.",
                          "[spoolman] is defined in more than one config file.",
                          "Refusing to act on Spoolman config: " + detail);
}

void SpoolmanOverlay::resolve_spoolman_target(SpoolmanTargetCallback on_done) {
    if (!api_) {
        SpoolmanConfigTarget res;
        res.status = SpoolmanConfigTarget::Status::Unreachable;
        res.detail = "not connected to the printer";
        on_done(res);
        return;
    }

    auto token = lifetime_.token();

    // Mock mode has no server.config. Preserve the historical assumption there, and
    // only there: helixscreen.conf is the target if it defines [spoolman].
    if (get_runtime_config() && get_runtime_config()->should_mock_moonraker()) {
        const std::string mock_path = config_paths_.path_for("helixscreen.conf");
        api_->transfers().download_file(
            "config", mock_path,
            [this, token, mock_path, on_done](const std::string& content) {
                bool defined = helix::MoonrakerConfigManager::has_section(content, "spoolman");
                token.defer("SpoolmanOverlay::resolve_mock",
                            [this, defined, content, mock_path, on_done]() {
                                SpoolmanConfigTarget res;
                                res.path = mock_path;
                                res.content = content;
                                res.status = defined ? SpoolmanConfigTarget::Status::Defined
                                                     : SpoolmanConfigTarget::Status::Undefined;
                                write_mode_ = defined ? SpoolmanWriteMode::InPlace
                                                      : SpoolmanWriteMode::IncludeFile;
                                spoolman_config_path_ = mock_path;
                                on_done(res);
                            });
            },
            [this, token, mock_path, on_done](const MoonrakerError&) {
                token.defer("SpoolmanOverlay::resolve_mock_missing", [this, mock_path, on_done]() {
                    SpoolmanConfigTarget res;
                    res.status = SpoolmanConfigTarget::Status::Undefined;
                    res.path = mock_path;
                    write_mode_ = SpoolmanWriteMode::IncludeFile;
                    spoolman_config_path_ = mock_path;
                    on_done(res);
                });
            });
        return;
    }

    // Moonraker names a config file outside the root config's own directory by
    // absolute path, and only the file manager's roots say which absolute paths
    // are writable. Fetch that first; "" keeps the pre-existing semantics.
    with_config_root([this, on_done](const std::string& root) {
        resolve_spoolman_target_with_root(root, on_done);
    });
}

void SpoolmanOverlay::with_config_root(std::function<void(const std::string&)> next) {
    if (!api_) {
        next("");
        return;
    }

    auto token = lifetime_.token();
    api_->files().get_file_roots(
        [this, token, next](const std::vector<FileRoot>& roots) {
            // === BG THREAD: pure lookup into a local ===
            const std::string root = helix::writable_root_path(roots, "config");
            token.defer("SpoolmanOverlay::config_root", [this, root, next]() {
                config_root_abs_ = root;
                spdlog::debug("[{}] Writable config root: '{}'", get_name(), root);
                next(root);
            });
        },
        [this, token, next](const MoonrakerError& err) {
            auto msg = err.message;
            token.defer("SpoolmanOverlay::config_root_error", [this, msg, next]() {
                // Not fatal. Older forks have no server.files.roots, and without a
                // root we simply keep treating an absolute config path as out of
                // reach — which is what every release before this one did.
                spdlog::debug("[{}] server.files.roots unavailable ({}); "
                              "absolute config paths stay unreachable",
                              get_name(), msg);
                config_root_abs_.clear();
                next("");
            });
        });
}

void SpoolmanOverlay::resolve_spoolman_target_with_root(const std::string& config_root_abs,
                                                        SpoolmanTargetCallback on_done) {
    auto token = lifetime_.token();

    api_->rest().get_server_config(
        [this, token, config_root_abs, on_done](const RestResponse& resp) {
            // === BG THREAD: parse into locals only, never touch `this` ===
            std::string config_file; // absolute path, when this build exposes it
            std::string data_path;   // ditto
            std::vector<helix::LoadedConfigFile> files;

            if (resp.data.is_object()) {
                const json& root = resp.data.contains("result") ? resp.data["result"] : resp.data;

                if (root.is_object() && root.contains("config") && root["config"].is_object()) {
                    const json& cfg = root["config"];
                    if (cfg.contains("server") && cfg["server"].is_object()) {
                        const json& srv = cfg["server"];
                        // Only newer Moonraker builds expose these. Most do not.
                        if (srv.contains("config_file") && srv["config_file"].is_string())
                            config_file = srv["config_file"].get<std::string>();
                        if (srv.contains("data_path") && srv["data_path"].is_string())
                            data_path = srv["data_path"].get<std::string>();
                    }
                }

                // files[] is the config chain Moonraker actually loaded, each entry
                // carrying the sections it defines. This is the reliable signal.
                if (root.is_object() && root.contains("files") && root["files"].is_array()) {
                    for (const auto& entry : root["files"]) {
                        if (!entry.is_object())
                            continue;
                        helix::LoadedConfigFile lcf;
                        if (entry.contains("filename") && entry["filename"].is_string())
                            lcf.filename = entry["filename"].get<std::string>();
                        if (entry.contains("sections") && entry["sections"].is_array()) {
                            for (const auto& sec : entry["sections"]) {
                                if (sec.is_string())
                                    lcf.sections.push_back(sec.get<std::string>());
                            }
                        }
                        files.push_back(std::move(lcf));
                    }
                }
            }

            // Two different questions, and on most firmwares one file answers both.
            // `primary` is the file that can PROVE the config root is addressable,
            // because it carries a section list to verify downloaded content against.
            // `root` is the file we may WRITE to. COSMOS splits them: its root holds
            // nothing but includes, and [server] sits in a vendor directory the
            // firmware replaces on upgrade (#1242).
            int primary = helix::MoonrakerConfigManager::select_primary_config_index(files);
            int root = helix::MoonrakerConfigManager::select_root_config_index(files);

            // Which loaded files already define [spoolman]? This counts a helixscreen.conf
            // pulled in by an [include] from an earlier run exactly like a natively
            // defined section, which is what stops us re-creating a duplicate.
            auto defining =
                helix::MoonrakerConfigManager::find_files_defining_section(files, "spoolman");

            SpoolmanConfigTarget res;

            if (defining.size() > 1) {
                res.status = SpoolmanConfigTarget::Status::Ambiguous;
                res.detail = "more than one loaded config file defines a [spoolman] section (";
                for (size_t i = 0; i < defining.size(); ++i) {
                    if (i)
                        res.detail += ", ";
                    res.detail += files[defining[i]].filename;
                }
                res.detail += "). Remove all but one and try again.";
                token.defer("SpoolmanOverlay::resolve_ambiguous",
                            [res, on_done]() { on_done(res); });
                return;
            }

            // Prefer absolute paths when the build exposes them (authoritative), else
            // derive the candidate from the reported config-root-relative filename.
            helix::ConfigPathInfo info;
            bool path_authoritative = false;
            if (!config_file.empty() && !data_path.empty()) {
                info = helix::MoonrakerConfigManager::resolve_config_upload_location(config_file,
                                                                                     data_path);
                path_authoritative = true;
            } else if (root >= 0) {
                // Derive the config root from the root config, never from the [server]
                // file: on COSMOS the latter would put helixscreen.conf inside the
                // vendor directory. The reported name may be absolute and in a tree
                // the file API does not serve, so resolve it to a candidate first.
                const auto root_candidates = helix::MoonrakerConfigManager::candidate_config_paths(
                    files[static_cast<size_t>(root)].filename, config_root_abs);
                if (root_candidates.empty()) {
                    info.error = "Moonraker loads its config from '" +
                                 files[static_cast<size_t>(root)].filename +
                                 "', which HelixScreen cannot address through the file "
                                 "manager's config folder. Add the [spoolman] section by hand.";
                } else {
                    info = helix::MoonrakerConfigManager::config_path_from_relative(
                        root_candidates.front());
                }
            } else {
                info.error = "Moonraker did not report which configuration files it loaded, "
                             "so HelixScreen cannot tell where [spoolman] belongs.";
            }

            bool in_place = (defining.size() == 1);
            std::string target_path; // what we edit
            std::string verify_path; // what we download to prove reachability
            std::vector<std::string> required;
            if (in_place) {
                // The file already defining [spoolman] is both, and it necessarily
                // has a section list to verify against.
                target_path = files[defining[0]].filename;
                verify_path = target_path;
                required = files[defining[0]].sections;
            } else {
                if (root >= 0)
                    target_path = files[static_cast<size_t>(root)].filename;
                if (primary >= 0) {
                    verify_path = files[static_cast<size_t>(primary)].filename;
                    required = files[static_cast<size_t>(primary)].sections;
                }
            }

            // files[] names anything outside the root config's own directory by
            // ABSOLUTE path, and on the AD5M that path is in a tree the file manager
            // does not serve even though the same file IS served under the config
            // root by its tail. So resolve each to a ranked list of candidates and
            // let content-verification pick the winner. The raw names stay for the
            // operator-facing messages, which should say what Moonraker said.
            const auto target_candidates =
                helix::MoonrakerConfigManager::candidate_config_paths(target_path, config_root_abs);
            const auto verify_candidates =
                helix::MoonrakerConfigManager::candidate_config_paths(verify_path, config_root_abs);
            // Whether the verify candidates were derived from the config root or merely
            // guessed from the tail of a foreign path decides how strict the content
            // proof has to be — see verify_config_reachable().
            const bool verify_speculative =
                helix::MoonrakerConfigManager::candidates_are_speculative(verify_path,
                                                                          config_root_abs);

            // === MAIN THREAD: apply the resolution, then verify by content ===
            token.defer(
                "SpoolmanOverlay::resolve_target",
                [this, info, target_path, target_candidates, verify_path, verify_candidates,
                 verify_speculative, required, in_place, path_authoritative, on_done]() {
                    SpoolmanConfigTarget res;

                    if (!info.uploadable) {
                        res.status = SpoolmanConfigTarget::Status::Unreachable;
                        res.detail = info.error;
                        res.proved_out_of_reach = true;
                        on_done(res);
                        return;
                    }
                    if (!target_path.empty() && target_candidates.empty()) {
                        res.status = SpoolmanConfigTarget::Status::Unreachable;
                        res.detail = "Moonraker loads '" + target_path +
                                     "', which HelixScreen cannot address through the file "
                                     "manager's config folder. Add the [spoolman] section by hand.";
                        res.proved_out_of_reach = true;
                        on_done(res);
                        return;
                    }
                    // The in-place branch reads target_candidates.front() below, and the
                    // guard above lets an empty target_path through — that combination is
                    // only unreachable because find_files_defining_section() skips entries
                    // Moonraker named nothing at all. Assert it here rather than rely on a
                    // second function to keep doing so.
                    if (in_place && target_candidates.empty()) {
                        res.status = SpoolmanConfigTarget::Status::Unreachable;
                        res.detail = "Moonraker reported a loaded config file with no name, so "
                                     "HelixScreen cannot tell which file defines [spoolman].";
                        res.proved_out_of_reach = true;
                        on_done(res);
                        return;
                    }

                    config_paths_ = info;
                    write_mode_ =
                        in_place ? SpoolmanWriteMode::InPlace : SpoolmanWriteMode::IncludeFile;
                    // In place, the verified winner replaces this below; it is the best
                    // guess until then.
                    spoolman_config_path_ = in_place ? target_candidates.front()
                                                     : config_paths_.path_for("helixscreen.conf");

                    // Absolute paths already proved reachability and there is nothing to
                    // cross-check against; anything else is verified by content.
                    if (path_authoritative && required.empty()) {
                        res.status = in_place ? SpoolmanConfigTarget::Status::Defined
                                              : SpoolmanConfigTarget::Status::Undefined;
                        res.path = spoolman_config_path_;
                        on_done(res);
                        return;
                    }

                    if (required.empty() || verify_candidates.empty()) {
                        res.status = SpoolmanConfigTarget::Status::Unreachable;
                        res.detail = "Moonraker reported config file '" + info.config_filename +
                                     "' with no section list, so HelixScreen cannot confirm it "
                                     "is the config actually in use.";
                        // Nothing was proved out of reach here — we simply could not tell,
                        // and a local write on a hunch is exactly what that does not license.
                        on_done(res);
                        return;
                    }

                    // Proving the [server] file is addressable proves the config root
                    // maps correctly, and the write target sits under that same root.
                    // Only the in-place path needs the target's own content, and there
                    // verify_path is the target.
                    verify_config_reachable(verify_path, verify_candidates, 0, required, in_place,
                                            verify_speculative, on_done);
                });
        },
        [this, token, on_done](const MoonrakerError& err) {
            auto msg = err.message;
            token.defer("SpoolmanOverlay::resolve_error", [msg, on_done]() {
                // Without server.config we cannot tell where Moonraker reads from, and
                // guessing is what produced the silent-failure bug. Fail visibly.
                SpoolmanConfigTarget res;
                res.status = SpoolmanConfigTarget::Status::Unreachable;
                res.detail = "could not query server.config (" + msg + ")";
                on_done(res);
            });
        });
}

void SpoolmanOverlay::verify_config_reachable(
    const std::string& reported_name, const std::vector<std::string>& candidates, size_t index,
    const std::vector<std::string>& required_sections, bool in_place, bool speculative,
    SpoolmanTargetCallback on_done, const std::string& last_detail) {
    if (index >= candidates.size()) {
        SpoolmanConfigTarget res;
        res.status = SpoolmanConfigTarget::Status::Unreachable;
        res.detail = "Moonraker loaded '" + reported_name +
                     "' but no file under the writable config folder matches it, so its "
                     "real config lives outside the area HelixScreen can write.";
        // Keep why the closest candidate was rejected — "no match" alone does not
        // tell an operator whether the file was absent or simply the wrong one.
        if (!last_detail.empty())
            res.detail += " (" + last_detail + ")";
        // Every candidate was downloaded and judged, or 404'd. That is a proof, not
        // a guess, and it is what licenses the local-write fallback.
        res.proved_out_of_reach = true;
        on_done(res);
        return;
    }

    const std::string candidate = candidates[index];
    auto token = lifetime_.token();

    api_->transfers().download_file(
        "config", candidate,
        [this, token, required_sections, reported_name, candidates, index, candidate, in_place,
         speculative, on_done](const std::string& content) {
            // === BG THREAD: pure string comparison, no `this` ===
            // Moonraker reports the sections it parsed when it last started, so a file
            // edited since then legitimately disagrees with the list. Only a wholesale
            // disagreement means we are looking at a different file.
            auto match =
                helix::MoonrakerConfigManager::classify_section_match(content, required_sections);
            bool drifted = match.verdict == helix::SectionMatch::Drifted;
            bool mismatch = match.verdict == helix::SectionMatch::Mismatch;

            // A guessed path may not lean on drift tolerance. Separately, the in-place
            // branch exists only because some file defines the section we are about to
            // rewrite, so a candidate without it is not that file whatever else matches.
            std::string rejected_by;
            if (!mismatch && speculative && drifted)
                rejected_by = "its path was inferred, not derived, so it has to match "
                              "Moonraker's section list exactly";
            else if (!mismatch && in_place &&
                     !helix::MoonrakerConfigManager::has_section(content, "spoolman"))
                rejected_by = "it does not define the [spoolman] section that selected it";
            if (!rejected_by.empty()) {
                mismatch = true;
                drifted = false;
            }

            std::string missing;
            for (const auto& s : match.missing) {
                if (!missing.empty())
                    missing += ", ";
                missing += s;
            }

            token.defer(
                "SpoolmanOverlay::verify_config_reachable",
                [this, mismatch, drifted, missing, rejected_by, match, reported_name, candidates,
                 index, candidate, content, required_sections, in_place, speculative, on_done]() {
                    if (mismatch) {
                        // Wrong file under a plausible name. Try the next
                        // candidate rather than writing to it.
                        const std::string why = rejected_by.empty()
                                                    ? "missing section(s): " + missing
                                                    : rejected_by + " (missing: " + missing + ")";
                        spdlog::info("[{}] '{}' is not the config Moonraker loaded as "
                                     "'{}' — {}; trying the next candidate",
                                     get_name(), candidate, reported_name, why);
                        verify_config_reachable(
                            reported_name, candidates, index + 1, required_sections, in_place,
                            speculative, on_done,
                            "a file named '" + candidate +
                                "' exists under the writable config folder but is not "
                                "the config Moonraker loaded; " +
                                why);
                        return;
                    }

                    if (drifted) {
                        spdlog::info("[{}] {} matches {}/{} of the sections Moonraker "
                                     "reported; it has been edited since Moonraker last "
                                     "restarted (missing: {}). Proceeding.",
                                     get_name(), candidate, match.matched, match.total, missing);
                    }

                    // The one line that says which path won, and for which
                    // reported name. Info level on purpose: this is the first
                    // thing to read in a live log when a write goes astray.
                    spdlog::info("[{}] Resolved Moonraker's '{}' to '{}' under the "
                                 "config root (candidate {} of {})",
                                 get_name(), reported_name, candidate, index + 1,
                                 candidates.size());

                    if (in_place) {
                        // The verified candidate IS the write target here.
                        spoolman_config_path_ = candidate;
                        check_stale_helix_conf(candidate, content, on_done);
                        return;
                    }
                    SpoolmanConfigTarget res;
                    res.status = SpoolmanConfigTarget::Status::Undefined;
                    res.path = spoolman_config_path_;
                    on_done(res);
                });
        },
        [this, token, reported_name, candidates, index, candidate, required_sections, in_place,
         speculative, on_done](const MoonrakerError& err) {
            auto msg = err.message;
            bool not_found = (err.type == MoonrakerErrorType::FILE_NOT_FOUND);
            token.defer("SpoolmanOverlay::verify_config_error",
                        [this, msg, not_found, reported_name, candidates, index, candidate,
                         required_sections, in_place, speculative, on_done]() {
                            if (not_found) {
                                // Only a genuine 404 justifies moving on. Anything else
                                // is a transport problem, and treating it as "wrong
                                // path" would walk the whole list on a flaky link.
                                spdlog::info("[{}] No '{}' under the config root for "
                                             "Moonraker's '{}'; trying the next candidate",
                                             get_name(), candidate, reported_name);
                                verify_config_reachable(reported_name, candidates, index + 1,
                                                        required_sections, in_place, speculative,
                                                        on_done,
                                                        "no '" + candidate +
                                                            "' exists under the writable config "
                                                            "folder");
                                return;
                            }
                            // A transport failure proves nothing about where the config
                            // lives, so proved_out_of_reach stays false and the local
                            // write is not licensed.
                            SpoolmanConfigTarget res;
                            res.status = SpoolmanConfigTarget::Status::Unreachable;
                            res.detail = "could not read '" + candidate +
                                         "' from the config folder (" + msg + ")";
                            on_done(res);
                        });
        });
}

void SpoolmanOverlay::check_stale_helix_conf(const std::string& target_path,
                                             const std::string& content,
                                             SpoolmanTargetCallback on_done) {
    const std::string helix_path = config_paths_.path_for("helixscreen.conf");

    auto ready = [target_path, content](SpoolmanTargetCallback cb) {
        SpoolmanConfigTarget res;
        res.status = SpoolmanConfigTarget::Status::Defined;
        res.path = target_path;
        res.content = content;
        cb(res);
    };

    // If the target IS helixscreen.conf there is nothing to collide with.
    if (target_path == helix_path) {
        ready(on_done);
        return;
    }

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", helix_path,
        [this, token, ready, helix_path, target_path, on_done](const std::string& helix) {
            // === BG THREAD: pure check ===
            bool stale = helix::MoonrakerConfigManager::has_section(helix, "spoolman");
            token.defer("SpoolmanOverlay::stale_helix_check", [stale, ready, helix_path,
                                                               target_path, on_done]() {
                if (stale) {
                    // Invisible to files[] because Moonraker has not loaded it,
                    // but acting on the native config now would duplicate as
                    // soon as the include takes effect.
                    SpoolmanConfigTarget res;
                    res.status = SpoolmanConfigTarget::Status::Ambiguous;
                    res.detail = "'" + target_path + "' defines [spoolman] and '" + helix_path +
                                 "' also contains one from an earlier HelixScreen "
                                 "run. Remove the [spoolman] section from " +
                                 helix_path + " and retry.";
                    on_done(res);
                    return;
                }
                ready(on_done);
            });
        },
        [this, token, ready, helix_path, on_done](const MoonrakerError& err) {
            auto msg = err.message;
            bool not_found = (err.type == MoonrakerErrorType::FILE_NOT_FOUND);
            token.defer("SpoolmanOverlay::stale_helix_error",
                        [ready, not_found, msg, helix_path, on_done]() {
                            if (not_found) {
                                // No helixscreen.conf at all — nothing to collide with.
                                ready(on_done);
                                return;
                            }
                            SpoolmanConfigTarget res;
                            res.status = SpoolmanConfigTarget::Status::Unreachable;
                            res.detail = "could not read '" + helix_path +
                                         "' to check for a duplicate [spoolman] (" + msg + ")";
                            on_done(res);
                        });
        });
}

void SpoolmanOverlay::resolve_config_location(const std::string& host, const std::string& port) {
    // A plan left over from a previous attempt must never redirect a healthy
    // write to the local filesystem.
    local_plan_ = {};

    resolve_spoolman_target([this, host, port](const SpoolmanConfigTarget& res) {
        switch (res.status) {
        case SpoolmanConfigTarget::Status::Ambiguous:
            fail_config_ambiguous(res.detail);
            return;
        case SpoolmanConfigTarget::Status::Unreachable:
            if (!res.proved_out_of_reach) {
                // Unreachable also covers "we could not tell" — a dropped socket, a
                // 500, a build with no section list. Editing a vendor config and
                // restarting Moonraker on the strength of a transport hiccup is a far
                // worse outcome than telling the user to retry.
                spdlog::info("[{}] Config unreachable but not proved out of reach; no local "
                             "write: {}",
                             get_name(), res.detail);
                fail_config_unreachable(res.detail);
                return;
            }
            try_local_config_fallback(res.detail, host, port);
            return;
        case SpoolmanConfigTarget::Status::Defined:
            spdlog::info("[{}] Updating [spoolman] in place in {}", get_name(), res.path);
            write_spoolman_in_place(res.content, host, port);
            return;
        case SpoolmanConfigTarget::Status::Undefined:
            spdlog::info("[{}] No loaded config defines [spoolman]; writing {} + include",
                         get_name(), spoolman_config_path_);
            configure_spoolman(host, port);
            return;
        }
    });
}

void SpoolmanOverlay::try_local_config_fallback(const std::string& detail, const std::string& host,
                                                const std::string& port) {
    std::string moonraker_host;
    if (Config* cfg = Config::get_instance())
        moonraker_host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");

    // Both gates must hold. Our /proc says nothing about a printer across the
    // network, and without a writable root there is nowhere to put the file the
    // include would name — an include with no matching file stops Moonraker dead.
    if (!helix::is_moonraker_on_same_host(moonraker_host) || config_root_abs_.empty()) {
        fail_config_unreachable(detail);
        return;
    }

    auto token = lifetime_.token();
    const std::string root = config_root_abs_;
    helix::http::HttpExecutor::slow().submit([this, token, root, detail, host, port]() {
        // === BG THREAD: /proc walk (blocking IO) + pure planning, never touch `this` ===
        auto plan = helix::diag::plan_local_include(helix::diag::find_moonraker_processes(), root);
        // The write lands by temp-file-plus-rename in the config's own directory, so
        // the DIRECTORY has to be writable and searchable too — a mode-666 file inside
        // a read-only /usr/share passes the file check and then fails at the rename,
        // after helixscreen.conf has already been uploaded.
        if (plan.viable) {
            const size_t slash = plan.vendor_config_abs.rfind('/');
            const std::string dir = slash == std::string::npos
                                        ? std::string(".")
                                        : plan.vendor_config_abs.substr(0, slash);
            if (::access((dir.empty() ? "/" : dir).c_str(), W_OK | X_OK) != 0) {
                plan.viable = false;
                plan.error = dir + " is not writable by HelixScreen, so the replacement config "
                                   "cannot be renamed into place";
            }
        }
        if (plan.viable && ::access(plan.vendor_config_abs.c_str(), W_OK) != 0) {
            plan.viable = false;
            plan.error = plan.vendor_config_abs + " is not writable by HelixScreen";
        }

        token.defer("SpoolmanOverlay::local_fallback_plan", [this, plan, detail, host, port]() {
            if (!plan.viable) {
                spdlog::info("[{}] No local-write fallback: {}", get_name(), plan.error);
                fail_config_unreachable(detail);
                return;
            }

            spdlog::info("[{}] Moonraker reads {} from the local disk; writing {} and "
                         "including it from there",
                         get_name(), plan.vendor_config_abs, plan.helix_conf_abs);

            local_plan_ = plan;
            config_paths_.uploadable = true;
            config_paths_.upload_subdir.clear();
            write_mode_ = SpoolmanWriteMode::IncludeFile;
            spoolman_config_path_ = plan.helix_conf_upload;

            // Step 1 of the bootstrap: create helixscreen.conf through the file
            // API. append_include_locally() is step 2 — see the ordering note there.
            configure_spoolman(host, port);
        });
    });
}

void SpoolmanOverlay::append_include_locally() {
    auto token = lifetime_.token();
    const auto plan = local_plan_;

    helix::http::HttpExecutor::slow().submit([this, token, plan]() {
        // === BG THREAD: blocking file IO, never touch `this` ===
        std::string err;
        const bool ok = helix::diag::append_include_to_local_config(plan.vendor_config_abs,
                                                                    plan.helix_conf_abs, err);

        token.defer("SpoolmanOverlay::local_include_written", [this, ok, err, plan]() {
            if (!ok) {
                spdlog::error("[{}] Could not add '{}' to {}: {}", get_name(), plan.include_line,
                              plan.vendor_config_abs, err);
                set_setup_status(lv_tr("Failed to update moonraker.conf."), true);
                set_connecting(false);
                return;
            }
            spdlog::info("[{}] Added '{}' to {}", get_name(), plan.include_line,
                         plan.vendor_config_abs);
            restart_and_verify();
        });
    });
}

void SpoolmanOverlay::write_spoolman_in_place(const std::string& content, const std::string& host,
                                              const std::string& port) {
    if (!api_ || spoolman_config_path_.empty()) {
        set_setup_status(lv_tr("Not connected to printer."), true);
        set_connecting(false);
        return;
    }

    auto entries = SpoolmanSetup::build_spoolman_config_entries(host, port);
    std::string modified = helix::MoonrakerConfigManager::upsert_section(
        content, "spoolman", entries, "Spoolman - added by HelixScreen");

    const std::string path = spoolman_config_path_;

    auto token = lifetime_.token();
    api_->transfers().upload_file(
        "config", path, modified,
        [this, token]() {
            // No include to add: the target file is already part of the loaded chain.
            token.defer("SpoolmanOverlay::in_place_uploaded", [this]() { restart_and_verify(); });
        },
        [this, token, path](const MoonrakerError& err) {
            auto msg = err.message;
            token.defer("SpoolmanOverlay::in_place_upload_error", [this, msg, path]() {
                spdlog::error("[{}] Failed to upload {}: {}", get_name(), path, msg);
                set_setup_status(lv_tr("Failed to save config."), true);
                set_connecting(false);
            });
        });
}

void SpoolmanOverlay::configure_spoolman(const std::string& host, const std::string& port) {
    if (!api_) {
        set_setup_status(lv_tr("Not connected to printer."), true);
        set_connecting(false);
        return;
    }
    auto token = lifetime_.token();
    auto entries = SpoolmanSetup::build_spoolman_config_entries(host, port);

    api_->transfers().download_file(
        "config", spoolman_config_path_,
        [this, token, entries](const std::string& content) {
            if (token.expired())
                return;
            // Defer to main thread — finish_configure() accesses lifetime_
            token.defer([this, content, entries]() { finish_configure(content, entries); });
        },
        [this, token, entries](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                token.defer([this, entries]() { finish_configure("", entries); });
            } else {
                token.defer("SpoolmanOverlay::configure_error", [this]() {
                    set_setup_status(lv_tr("Failed to read config. Check connection."), true);
                    set_connecting(false);
                });
            }
        });
}

void SpoolmanOverlay::finish_configure(
    const std::string& helix_content,
    const std::vector<std::pair<std::string, std::string>>& entries) {
    auto token = lifetime_.token();
    // upsert, not add: add_section() is a no-op when [spoolman] already exists, which
    // silently discarded every server-URL change after the first successful setup.
    std::string modified = helix::MoonrakerConfigManager::upsert_section(
        helix_content, "spoolman", entries, "Spoolman - added by HelixScreen");

    api_->transfers().upload_file(
        "config", spoolman_config_path_, modified,
        [this, token]() {
            if (token.expired())
                return;
            // Defer to main thread — ensure_moonraker_include() accesses lifetime_
            token.defer([this]() { ensure_moonraker_include(); });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("SpoolmanOverlay::upload_error", [this, msg]() {
                spdlog::error("[{}] Failed to upload helixscreen.conf: {}", get_name(), msg);
                set_setup_status(lv_tr("Failed to save config."), true);
                set_connecting(false);
            });
        });
}

void SpoolmanOverlay::ensure_moonraker_include() {
    if (local_plan_.viable) {
        // ORDER IS A SAFETY REQUIREMENT, and this is the second half of it.
        // helixscreen.conf was uploaded through the file API before we got here,
        // so by the time the [include] naming it lands, the file it names exists.
        // Doing these the other way round leaves Moonraker with an include it
        // cannot match, and Moonraker treats that as fatal: it raises
        // ConfigError("No files matching include directive") and refuses to start,
        // taking the printer's whole web stack down with it.
        append_include_locally();
        return;
    }

    auto token = lifetime_.token();
    const std::string moonraker_path = config_paths_.path_for(config_paths_.config_filename);
    api_->transfers().download_file(
        "config", moonraker_path,
        [this, token, moonraker_path](const std::string& content) {
            if (token.expired())
                return;
            // Defer to main thread — helper methods access lifetime_
            token.defer([this, content, moonraker_path]() {
                if (helix::MoonrakerConfigManager::has_include_line(content)) {
                    restart_and_verify();
                    return;
                }
                auto token2 = lifetime_.token();
                std::string modified = helix::MoonrakerConfigManager::add_include_line(content);
                api_->transfers().upload_file(
                    "config", moonraker_path, modified,
                    [this, token2]() {
                        if (token2.expired())
                            return;
                        token2.defer([this]() { restart_and_verify(); });
                    },
                    [this, token2, moonraker_path](const MoonrakerError& err) {
                        auto msg = err.message;
                        token2.defer(
                            "SpoolmanOverlay::include_upload_error", [this, msg, moonraker_path]() {
                                spdlog::error("[{}] Failed to upload {}: {}", get_name(),
                                              moonraker_path, msg);
                                set_setup_status(lv_tr("Failed to update moonraker.conf."), true);
                                set_connecting(false);
                            });
                    });
            });
        },
        [this, token, moonraker_path](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                // Defer to main thread — upload chain accesses lifetime_
                token.defer([this, moonraker_path]() {
                    auto token2 = lifetime_.token();
                    std::string fresh = helix::MoonrakerConfigManager::add_include_line("");
                    api_->transfers().upload_file(
                        "config", moonraker_path, fresh,
                        [this, token2]() {
                            if (token2.expired())
                                return;
                            token2.defer([this]() { restart_and_verify(); });
                        },
                        [this, token2, moonraker_path](const MoonrakerError& err2) {
                            auto msg = err2.message;
                            token2.defer("SpoolmanOverlay::include_create_error",
                                         [this, msg, moonraker_path]() {
                                             spdlog::error("[{}] Failed to create {}: {}",
                                                           get_name(), moonraker_path, msg);
                                             set_setup_status(
                                                 lv_tr("Failed to update moonraker.conf."), true);
                                             set_connecting(false);
                                         });
                        });
                });
            } else {
                auto msg = err.message;
                token.defer([this, msg, moonraker_path]() {
                    spdlog::error("[{}] Failed to read {}: {}", get_name(), moonraker_path, msg);
                    set_setup_status(lv_tr("Failed to read moonraker.conf."), true);
                    set_connecting(false);
                });
            }
        });
}

void SpoolmanOverlay::restart_and_verify() {
    lifetime_.defer("SpoolmanOverlay::restart_status",
                    [this]() { set_setup_status(lv_tr("Restarting Moonraker...")); });

    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    auto token = lifetime_.token();
    api_->restart_moonraker(
        [this, token]() {
            if (token.expired())
                return;
            spdlog::info("[{}] Moonraker restart initiated", get_name());
            token.defer("SpoolmanOverlay::restart_wait", [this]() {
                set_setup_status(lv_tr("Waiting for Moonraker..."));
                lv_timer_create(
                    [](lv_timer_t* timer) {
                        lv_timer_delete(timer);
                        auto& overlay = get_spoolman_overlay();
                        overlay.verify_spoolman_connected();
                    },
                    8000, nullptr);
            });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::restart_error", [this]() {
                set_setup_status(lv_tr("Failed to restart Moonraker."), true);
                set_connecting(false);
            });
        });
}

void SpoolmanOverlay::verify_spoolman_connected() {
    if (!api_)
        return;
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_status(
        [this, token](bool connected, int /*spool_id*/) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::verify_status", [this, connected]() {
                if (connected) {
                    spdlog::info("[{}] Spoolman verified connected!", get_name());
                    set_setup_status("");
                    update_server_url_display();
                    ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                  lv_tr("Spoolman connected!"), 3000);
                } else {
                    set_setup_status(
                        lv_tr("Moonraker restarted but Spoolman not connected. Check server."),
                        true);
                }
                set_connecting(false);
            });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::verify_error", [this]() {
                set_setup_status(
                    lv_tr("Could not verify Spoolman status. Moonraker may still be restarting."),
                    true);
                set_connecting(false);
            });
        });
}

// ============================================================================
// CHANGE, REMOVE, URL DISPLAY
// ============================================================================

void SpoolmanOverlay::update_server_url_display() {
    if (!server_url_text_ || !api_)
        return;

    resolve_spoolman_target([this](const SpoolmanConfigTarget& res) {
        if (!server_url_text_)
            return;

        if (res.status != SpoolmanConfigTarget::Status::Defined) {
            // Nothing configured, or we could not determine where it lives. Say
            // "Connected" without inventing a URL.
            if (res.status != SpoolmanConfigTarget::Status::Undefined)
                spdlog::warn("[{}] Cannot show Spoolman URL: {}", get_name(), res.detail);
            lv_label_set_text(server_url_text_, lv_tr("Connected"));
            return;
        }

        // Read the URL from whichever loaded file actually defines [spoolman].
        auto url =
            helix::MoonrakerConfigManager::get_section_value(res.content, "spoolman", "server");
        if (url.empty()) {
            lv_label_set_text(server_url_text_, lv_tr("Connected"));
            return;
        }
        auto text = fmt::format("{} {}", lv_tr("Connected to"), url);
        lv_label_set_text(server_url_text_, text.c_str());
    });
}

void SpoolmanOverlay::on_change_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_change_clicked");
    auto& overlay = get_spoolman_overlay();

    if (overlay.api_) {
        // Prefill from whichever loaded file actually defines [spoolman], not from an
        // assumed helixscreen.conf.
        overlay.resolve_spoolman_target([&overlay](const SpoolmanConfigTarget& res) {
            if (res.status != SpoolmanConfigTarget::Status::Defined)
                return;
            auto url =
                helix::MoonrakerConfigManager::get_section_value(res.content, "spoolman", "server");
            auto parsed = SpoolmanSetup::parse_url_components(url);
            if (overlay.host_input_)
                lv_textarea_set_text(overlay.host_input_, parsed.first.c_str());
            if (overlay.port_input_)
                lv_textarea_set_text(overlay.port_input_, parsed.second.c_str());
        });
    }

    if (overlay.setup_card_)
        lv_obj_remove_flag(overlay.setup_card_, LV_OBJ_FLAG_HIDDEN);
    if (overlay.status_card_)
        lv_obj_add_flag(overlay.status_card_, LV_OBJ_FLAG_HIDDEN);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_cancel_setup_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_cancel_setup_clicked");

    // Restore default visibility — let the subject bindings take over again
    auto& overlay = get_spoolman_overlay();
    if (overlay.setup_card_)
        lv_obj_add_flag(overlay.setup_card_, LV_OBJ_FLAG_HIDDEN);
    if (overlay.status_card_)
        lv_obj_remove_flag(overlay.status_card_, LV_OBJ_FLAG_HIDDEN);
    overlay.set_setup_status("");
    overlay.set_connecting(false);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_remove_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_remove_clicked");

    modal_show_confirmation(
        lv_tr("Remove Spoolman?"),
        lv_tr(
            "This will remove the Spoolman configuration from Moonraker and restart the service."),
        ModalSeverity::Warning, lv_tr("Remove"),
        [](lv_event_t*) {
            auto& overlay = get_spoolman_overlay();
            overlay.remove_spoolman_config();
        },
        nullptr, nullptr);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::remove_spoolman_config() {
    if (!api_)
        return;

    set_setup_status(lv_tr("Locating Spoolman configuration..."));

    resolve_spoolman_target([this](const SpoolmanConfigTarget& res) {
        switch (res.status) {
        case SpoolmanConfigTarget::Status::Ambiguous:
            // Removal is destructive — never guess which [spoolman] to delete.
            fail_config_ambiguous(res.detail);
            return;

        case SpoolmanConfigTarget::Status::Unreachable:
            report_spoolman_error(
                "Could not remove Spoolman: its configuration is not writable from HelixScreen. "
                "Remove the [spoolman] section by hand — see the log for details.",
                "Could not remove Spoolman configuration.",
                "Cannot remove Spoolman config: " + res.detail);
            return;

        case SpoolmanConfigTarget::Status::Undefined:
            // Honest report: there was nothing to remove. Not a success toast.
            spdlog::info("[{}] Remove requested but no loaded config defines [spoolman]",
                         get_name());
            set_setup_status(lv_tr("Spoolman is not configured — nothing to remove."));
            ToastManager::instance().show(ToastSeverity::INFO,
                                          lv_tr("Spoolman was not configured."), 3000);
            return;

        case SpoolmanConfigTarget::Status::Defined:
            break;
        }

        const std::string path = res.path;
        std::string modified =
            helix::MoonrakerConfigManager::remove_section(res.content, "spoolman");
        spdlog::info("[{}] Removing [spoolman] from {}", get_name(), path);

        auto tok = lifetime_.token();
        api_->transfers().upload_file(
            "config", path, modified,
            [this, tok, path]() {
                tok.defer("SpoolmanOverlay::remove_uploaded", [this]() {
                    set_setup_status(lv_tr("Restarting Moonraker..."));
                    EmergencyStopOverlay::instance().suppress_recovery_dialog(
                        RecoverySuppression::LONG);
                    auto tok2 = lifetime_.token();
                    api_->restart_moonraker(
                        [this, tok2]() {
                            tok2.defer("SpoolmanOverlay::remove_restarted", [this]() {
                                set_setup_status("");
                                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                              lv_tr("Spoolman removed."), 3000);
                            });
                        },
                        [this, tok2](const MoonrakerError& err) {
                            auto msg = err.message;
                            tok2.defer("SpoolmanOverlay::remove_restart_error", [this, msg]() {
                                spdlog::error("[{}] Failed to restart Moonraker: {}", get_name(),
                                              msg);
                                set_setup_status(lv_tr("Failed to restart Moonraker."), true);
                            });
                        });
                });
            },
            [this, tok, path](const MoonrakerError& err) {
                auto msg = err.message;
                tok.defer("SpoolmanOverlay::remove_upload_error", [this, msg, path]() {
                    spdlog::error("[{}] Failed to upload {}: {}", get_name(), path, msg);
                    set_setup_status(lv_tr("Failed to save config."), true);
                });
            });
    });
}

} // namespace helix::ui
