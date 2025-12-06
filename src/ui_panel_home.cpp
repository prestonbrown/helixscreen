// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_home.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_temp_control.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "wifi_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <memory>

// Signal polling interval (5 seconds)
static constexpr uint32_t SIGNAL_POLL_INTERVAL_MS = 5000;

HomePanel::HomePanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Initialize buffer contents with default values
    std::strcpy(status_buffer_, "Welcome to HelixScreen");
    std::strcpy(temp_buffer_, "-- °C");
    std::strcpy(network_label_buffer_, "WiFi");

    // Subscribe to PrinterState subjects (ObserverGuard handles cleanup)
    // Note: Connection state dimming is now handled by XML binding to printer_connection_state
    extruder_temp_observer_ =
        ObserverGuard(printer_state_.get_extruder_temp_subject(), extruder_temp_observer_cb, this);

    spdlog::debug("[{}] Subscribed to PrinterState extruder temperature", get_name());

    // Load configured LED from wizard settings and tell PrinterState to track it
    Config* config = Config::get_instance();
    if (config) {
        configured_led_ = config->get<std::string>(WizardConfigPaths::LED_STRIP, "");
        if (!configured_led_.empty()) {
            // Tell PrinterState to track this LED for state updates
            printer_state_.set_tracked_led(configured_led_);

            // Subscribe to LED state changes from PrinterState
            led_state_observer_ =
                ObserverGuard(printer_state_.get_led_state_subject(), led_state_observer_cb, this);

            spdlog::info("[{}] Configured LED: {} (observing state)", get_name(), configured_led_);
        } else {
            spdlog::debug("[{}] No LED configured - light control will be hidden", get_name());
        }
    }
}

HomePanel::~HomePanel() {
    // ObserverGuard handles observer cleanup automatically
    // Timers are owned by LVGL - they will be cleaned up on shutdown
    // Don't try to delete during static destruction (causes crash after LVGL teardown)
    signal_poll_timer_ = nullptr;
    tip_rotation_timer_ = nullptr;
}

void HomePanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subjects with default values
    // Note: LED state (led_state) is managed by PrinterState and already registered
    UI_SUBJECT_INIT_AND_REGISTER_STRING(status_subject_, status_buffer_, "Welcome to HelixScreen",
                                        "status_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(temp_subject_, temp_buffer_, "-- °C", "temp_text");

    // Network icon state: integer 0-5 for conditional icon visibility
    // 0=disconnected, 1-4=wifi strength, 5=ethernet
    // Note: Uses unique name to avoid conflict with navigation_bar's network_icon_state
    lv_subject_init_int(&network_icon_state_, 0); // Default: disconnected
    lv_xml_register_subject(nullptr, "home_network_icon_state", &network_icon_state_);

    UI_SUBJECT_INIT_AND_REGISTER_STRING(network_label_subject_, network_label_buffer_, "WiFi",
                                        "network_label");

    // Printer type and host - two subjects for flexible XML layout
    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_type_subject_, printer_type_buffer_, "",
                                        "printer_type_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_host_subject_, printer_host_buffer_, "",
                                        "printer_host_text");
    lv_subject_init_int(&printer_info_visible_, 0);
    lv_xml_register_subject(nullptr, "printer_info_visible", &printer_info_visible_);

    // Register event callbacks BEFORE loading XML
    // Note: These use static trampolines that will look up the global instance
    lv_xml_register_event_cb(nullptr, "light_toggle_cb", light_toggle_cb);
    lv_xml_register_event_cb(nullptr, "print_card_clicked_cb", print_card_clicked_cb);
    lv_xml_register_event_cb(nullptr, "tip_text_clicked_cb", tip_text_clicked_cb);
    lv_xml_register_event_cb(nullptr, "temp_clicked_cb", temp_clicked_cb);
    lv_xml_register_event_cb(nullptr, "printer_status_clicked_cb", printer_status_clicked_cb);
    lv_xml_register_event_cb(nullptr, "network_clicked_cb", network_clicked_cb);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Registered subjects and event callbacks", get_name());

    // Set initial tip of the day
    update_tip_of_day();
}

void HomePanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setting up...", get_name());

    // Widget visibility (light button/divider) is handled by XML bindings to printer_has_led
    // subject Printer image opacity is handled by XML styles bound to printer_connection_state
    // subject No C++ widget manipulation needed - everything is declarative

    // Start tip rotation timer (60 seconds = 60000ms)
    if (!tip_rotation_timer_) {
        tip_rotation_timer_ = lv_timer_create(tip_rotation_timer_cb, 60000, this);
        spdlog::info("[{}] Started tip rotation timer (60s interval)", get_name());
    }

    // Initialize WiFiManager for signal strength queries
    // Use silent=true since this is just for signal monitoring, not user-initiated WiFi setup
    // (suppress error modals if WiFi isn't available)
    if (!wifi_manager_) {
        wifi_manager_ = std::make_shared<WiFiManager>(/*silent=*/true);
        wifi_manager_->init_self_reference(wifi_manager_);
        spdlog::debug("[{}] WiFiManager initialized for signal strength queries", get_name());
    }

    // Set initial network icon state and start polling
    // Note: on_activate() would normally do this, but nav system doesn't call lifecycle hooks yet
    update_network_icon_state();

    // Start signal polling timer if on WiFi
    if (!signal_poll_timer_ && current_network_ == NETWORK_WIFI) {
        signal_poll_timer_ = lv_timer_create(signal_poll_timer_cb, SIGNAL_POLL_INTERVAL_MS, this);
        spdlog::debug("[{}] Started signal polling timer ({}ms)", get_name(),
                      SIGNAL_POLL_INTERVAL_MS);
    }

    // Load printer image from config (if available)
    reload_from_config();

    spdlog::info("[{}] Setup complete!", get_name());
}

void HomePanel::on_activate() {
    // Start signal polling timer when panel becomes visible
    if (!signal_poll_timer_ && current_network_ == NETWORK_WIFI) {
        signal_poll_timer_ = lv_timer_create(signal_poll_timer_cb, SIGNAL_POLL_INTERVAL_MS, this);
        spdlog::debug("[{}] Started signal polling timer ({}ms interval)", get_name(),
                      SIGNAL_POLL_INTERVAL_MS);
    }

    // Immediately update network icon state
    update_network_icon_state();
}

void HomePanel::on_deactivate() {
    // Stop signal polling timer when panel is hidden (saves CPU)
    if (signal_poll_timer_) {
        lv_timer_delete(signal_poll_timer_);
        signal_poll_timer_ = nullptr;
        spdlog::debug("[{}] Stopped signal polling timer", get_name());
    }
}

void HomePanel::update_tip_of_day() {
    auto tip = TipsManager::get_instance()->get_random_unique_tip();

    if (!tip.title.empty()) {
        // Store full tip for dialog display
        current_tip_ = tip;

        std::snprintf(status_buffer_, sizeof(status_buffer_), "%s", tip.title.c_str());
        lv_subject_copy_string(&status_subject_, status_buffer_);
        spdlog::debug("[{}] Updated tip: {}", get_name(), tip.title);
    } else {
        spdlog::warn("[{}] Failed to get tip, keeping current", get_name());
    }
}

void HomePanel::handle_light_toggle() {
    spdlog::info("[{}] Light button clicked", get_name());

    // Check if LED is configured
    if (configured_led_.empty()) {
        spdlog::warn("[{}] Light toggle called but no LED configured", get_name());
        return;
    }

    // Toggle to opposite of current state
    // Note: UI will update when Moonraker notification arrives (via PrinterState observer)
    bool new_state = !light_on_;

    // Send command to Moonraker
    if (api_) {
        if (new_state) {
            api_->set_led_on(
                configured_led_,
                [this]() {
                    spdlog::info("[{}] LED turned ON - waiting for state update", get_name());
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to turn LED on: {}", err.message);
                    NOTIFY_ERROR("Failed to turn light on: {}", err.user_message());
                });
        } else {
            api_->set_led_off(
                configured_led_,
                [this]() {
                    spdlog::info("[{}] LED turned OFF - waiting for state update", get_name());
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to turn LED off: {}", err.message);
                    NOTIFY_ERROR("Failed to turn light off: {}", err.user_message());
                });
        }
    } else {
        spdlog::warn("[{}] API not available - cannot control LED", get_name());
        NOTIFY_ERROR("Cannot control light: printer not connected");
    }
}

void HomePanel::handle_print_card_clicked() {
    spdlog::info("[{}] Print card clicked - navigating to print select panel", get_name());

    // Navigate to print select panel
    ui_nav_set_active(UI_PANEL_PRINT_SELECT);
}

void HomePanel::handle_tip_text_clicked() {
    if (current_tip_.title.empty()) {
        spdlog::warn("[{}] No tip available to display", get_name());
        return;
    }

    spdlog::info("[{}] Tip text clicked - showing detail dialog", get_name());

    // Show tip using unified modal_dialog (INFO severity, single Ok button)
    ui_modal_config_t config = {.position = {.use_alignment = true, .alignment = LV_ALIGN_CENTER},
                                .backdrop_opa = 180,
                                .keyboard = nullptr,
                                .persistent = false,
                                .on_close = nullptr};

    const char* attrs[] = {"title", current_tip_.title.c_str(), "message",
                           current_tip_.content.c_str(), nullptr};

    ui_modal_configure(UI_MODAL_SEVERITY_INFO, false, "Ok", nullptr);
    lv_obj_t* tip_dialog = ui_modal_show("modal_dialog", &config, attrs);

    if (!tip_dialog) {
        spdlog::error("[{}] Failed to show tip detail modal", get_name());
        return;
    }

    // Wire up Ok button to close
    lv_obj_t* ok_btn = lv_obj_find_by_name(tip_dialog, "btn_primary");
    if (ok_btn) {
        lv_obj_set_user_data(ok_btn, tip_dialog);
        lv_obj_add_event_cb(
            ok_btn,
            [](lv_event_t* e) {
                auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto* dialog = static_cast<lv_obj_t*>(lv_obj_get_user_data(btn));
                if (dialog) {
                    ui_modal_hide(dialog);
                }
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void HomePanel::handle_tip_rotation_timer() {
    update_tip_of_day();
}

void HomePanel::set_temp_control_panel(TempControlPanel* temp_panel) {
    temp_control_panel_ = temp_panel;
    spdlog::debug("[{}] TempControlPanel reference set", get_name());
}

void HomePanel::handle_temp_clicked() {
    spdlog::info("[{}] Temperature icon clicked - opening nozzle temp panel", get_name());

    if (!temp_control_panel_) {
        spdlog::error("[{}] TempControlPanel not initialized", get_name());
        NOTIFY_ERROR("Temperature panel not available");
        return;
    }

    // Create nozzle temp panel on first access (lazy initialization)
    if (!nozzle_temp_panel_ && parent_screen_) {
        spdlog::debug("[{}] Creating nozzle temperature panel...", get_name());

        // Create from XML
        nozzle_temp_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "nozzle_temp_panel", nullptr));
        if (nozzle_temp_panel_) {
            // Setup via injected TempControlPanel
            temp_control_panel_->setup_nozzle_panel(nozzle_temp_panel_, parent_screen_);

            // Initially hidden
            lv_obj_add_flag(nozzle_temp_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Nozzle temp panel created and initialized", get_name());
        } else {
            spdlog::error("[{}] Failed to create nozzle temp panel from XML", get_name());
            NOTIFY_ERROR("Failed to load temperature panel");
            return;
        }
    }

    // Push nozzle temp panel onto navigation history and show it
    if (nozzle_temp_panel_) {
        ui_nav_push_overlay(nozzle_temp_panel_);
    }
}

void HomePanel::handle_printer_status_clicked() {
    spdlog::info("[{}] Printer status icon clicked - navigating to advanced settings", get_name());

    // Navigate to advanced settings panel
    ui_nav_set_active(UI_PANEL_ADVANCED);
}

void HomePanel::handle_network_clicked() {
    spdlog::info("[{}] Network icon clicked - navigating to settings panel", get_name());

    // Navigate to settings panel (network settings is a sub-overlay there)
    // For quick access, we navigate to settings first - user can tap Network row
    // TODO: Add a dedicated NetworkSettingsPanel class to share overlay creation logic
    ui_nav_set_active(UI_PANEL_SETTINGS);
}

void HomePanel::on_led_state_changed(int state) {
    // Update local light_on_ state from PrinterState's led_state subject
    light_on_ = (state != 0);

    spdlog::debug("[{}] LED state changed: {} (from PrinterState)", get_name(),
                  light_on_ ? "ON" : "OFF");
}

void HomePanel::on_extruder_temp_changed(int temp) {
    // Format temperature for display and update the string subject
    std::snprintf(temp_buffer_, sizeof(temp_buffer_), "%d °C", temp);
    lv_subject_copy_string(&temp_subject_, temp_buffer_);

    spdlog::trace("[{}] Extruder temperature updated: {}°C", get_name(), temp);
}

void HomePanel::reload_from_config() {
    // LED visibility is handled via XML bindings to printer_has_led subject

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[{}] reload_from_config: Config not available", get_name());
        return;
    }

    // Update printer image based on configured printer type
    std::string printer_type = config->get<std::string>(WizardConfigPaths::PRINTER_TYPE, "");
    if (!printer_type.empty()) {
        // Look up image filename from printer database
        std::string image_filename = PrinterDetector::get_image_for_printer(printer_type);
        std::string image_path;

        if (!image_filename.empty()) {
            image_path = "A:assets/images/printers/" + image_filename;
        } else {
            // Fall back to generic CoreXY image
            spdlog::info("[{}] No specific image for '{}' - using generic CoreXY", get_name(),
                         printer_type);
            image_path = "A:assets/images/printers/generic-corexy.png";
        }

        // Find and update the printer_image widget
        if (panel_) {
            lv_obj_t* printer_image = lv_obj_find_by_name(panel_, "printer_image");
            if (printer_image) {
                lv_image_set_src(printer_image, image_path.c_str());
                spdlog::info("[{}] Printer image: '{}' for '{}'", get_name(), image_path,
                             printer_type);
            }
        }
    }

    // Update printer type/host overlay (hidden for localhost)
    std::string host = config->get<std::string>(WizardConfigPaths::MOONRAKER_HOST, "");

    if (host.empty() || host == "127.0.0.1" || host == "localhost") {
        lv_subject_set_int(&printer_info_visible_, 0);
    } else {
        std::strncpy(printer_type_buffer_, printer_type.empty() ? "Printer" : printer_type.c_str(),
                     sizeof(printer_type_buffer_) - 1);
        std::strncpy(printer_host_buffer_, host.c_str(), sizeof(printer_host_buffer_) - 1);
        lv_subject_copy_string(&printer_type_subject_, printer_type_buffer_);
        lv_subject_copy_string(&printer_host_subject_, printer_host_buffer_);
        lv_subject_set_int(&printer_info_visible_, 1);
    }
}

void HomePanel::light_toggle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] light_toggle_cb");
    (void)e;
    // XML-registered callbacks don't have user_data set to 'this'
    // Use the global instance via legacy API bridge
    // This will be fixed when main.cpp switches to class-based instantiation
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_light_toggle();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::print_card_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] print_card_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_print_card_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::tip_text_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] tip_text_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_tip_text_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::temp_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] temp_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_temp_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::printer_status_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] printer_status_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_printer_status_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::network_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] network_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_network_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::tip_rotation_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
    if (self) {
        self->handle_tip_rotation_timer();
    }
}

void HomePanel::led_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<HomePanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_led_state_changed(lv_subject_get_int(subject));
    }
}

void HomePanel::extruder_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<HomePanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_extruder_temp_changed(lv_subject_get_int(subject));
    }
}

void HomePanel::update(const char* status_text, int temp) {
    // Update subjects - all bound widgets update automatically
    if (status_text) {
        lv_subject_copy_string(&status_subject_, status_text);
        spdlog::debug("[{}] Updated status_text subject to: {}", get_name(), status_text);
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d °C", temp);
    lv_subject_copy_string(&temp_subject_, buf);
    spdlog::debug("[{}] Updated temp_text subject to: {}", get_name(), buf);
}

void HomePanel::set_network(network_type_t type) {
    current_network_ = type;

    // Update label text
    switch (type) {
    case NETWORK_WIFI:
        lv_subject_copy_string(&network_label_subject_, "WiFi");
        break;
    case NETWORK_ETHERNET:
        lv_subject_copy_string(&network_label_subject_, "Ethernet");
        break;
    case NETWORK_DISCONNECTED:
        lv_subject_copy_string(&network_label_subject_, "Disconnected");
        break;
    }

    // Update the icon state (will query WiFi signal strength if connected)
    update_network_icon_state();

    spdlog::debug("[{}] Network type set to {} (icon state will be computed)", get_name(),
                  static_cast<int>(type));
}

int HomePanel::compute_network_icon_state() {
    // State values:
    // 0 = Disconnected (wifi_off, disabled variant)
    // 1 = WiFi strength 1 (≤25%, warning variant)
    // 2 = WiFi strength 2 (26-50%, accent variant)
    // 3 = WiFi strength 3 (51-75%, accent variant)
    // 4 = WiFi strength 4 (>75%, accent variant)
    // 5 = Ethernet connected (accent variant)

    if (current_network_ == NETWORK_DISCONNECTED) {
        spdlog::trace("[{}] Network disconnected -> state 0", get_name());
        return 0;
    }

    if (current_network_ == NETWORK_ETHERNET) {
        spdlog::trace("[{}] Network ethernet -> state 5", get_name());
        return 5;
    }

    // WiFi - get signal strength from WiFiManager
    int signal = 0;
    if (wifi_manager_) {
        signal = wifi_manager_->get_signal_strength();
        spdlog::trace("[{}] WiFi signal strength: {}%", get_name(), signal);
    } else {
        spdlog::warn("[{}] WiFiManager not available for signal query", get_name());
    }

    // Map signal percentage to icon state (1-4)
    int state;
    if (signal <= 25)
        state = 1; // Weak (warning)
    else if (signal <= 50)
        state = 2; // Fair
    else if (signal <= 75)
        state = 3; // Good
    else
        state = 4; // Strong

    spdlog::trace("[{}] WiFi signal {}% -> state {}", get_name(), signal, state);
    return state;
}

void HomePanel::update_network_icon_state() {
    int new_state = compute_network_icon_state();
    int old_state = lv_subject_get_int(&network_icon_state_);

    if (new_state != old_state) {
        lv_subject_set_int(&network_icon_state_, new_state);
        spdlog::debug("[{}] Network icon state: {} -> {}", get_name(), old_state, new_state);
    }
}

void HomePanel::signal_poll_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
    if (self && self->current_network_ == NETWORK_WIFI) {
        self->update_network_icon_state();
    }
}

void HomePanel::set_light(bool is_on) {
    // Note: The actual LED state is managed by PrinterState via Moonraker notifications.
    // This method is only used for local state updates when API is unavailable.
    light_on_ = is_on;
    spdlog::debug("[{}] Local light state: {}", get_name(), is_on ? "ON" : "OFF");
}

static std::unique_ptr<HomePanel> g_home_panel;

HomePanel& get_global_home_panel() {
    if (!g_home_panel) {
        g_home_panel = std::make_unique<HomePanel>(get_printer_state(), nullptr);
    }
    return *g_home_panel;
}
