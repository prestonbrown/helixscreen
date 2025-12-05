// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_printer_identify.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_keyboard.h"
#include "ui_subject_registry.h"
#include "ui_theme.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "moonraker_client.h"
#include "printer_detector.h"
#include "printer_images.h"
#include "printer_types.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

// ============================================================================
// External Subject (defined in ui_wizard.cpp)
// ============================================================================

// Controls wizard Next button globally - shared across wizard steps
extern lv_subject_t connection_test_passed;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardPrinterIdentifyStep> g_wizard_printer_identify_step;

WizardPrinterIdentifyStep* get_wizard_printer_identify_step() {
    if (!g_wizard_printer_identify_step) {
        g_wizard_printer_identify_step = std::make_unique<WizardPrinterIdentifyStep>();
    }
    return g_wizard_printer_identify_step.get();
}

void destroy_wizard_printer_identify_step() {
    g_wizard_printer_identify_step.reset();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardPrinterIdentifyStep::WizardPrinterIdentifyStep() {
    // Zero-initialize buffers
    std::memset(printer_name_buffer_, 0, sizeof(printer_name_buffer_));
    std::memset(printer_detection_status_buffer_, 0, sizeof(printer_detection_status_buffer_));

    spdlog::debug("[{}] Instance created", get_name());
}

WizardPrinterIdentifyStep::~WizardPrinterIdentifyStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
    printer_preview_image_ = nullptr;
}

// ============================================================================
// Move Semantics
// ============================================================================

WizardPrinterIdentifyStep::WizardPrinterIdentifyStep(WizardPrinterIdentifyStep&& other) noexcept
    : screen_root_(other.screen_root_), printer_preview_image_(other.printer_preview_image_),
      printer_name_(other.printer_name_), printer_type_selected_(other.printer_type_selected_),
      printer_detection_status_(other.printer_detection_status_),
      printer_identify_validated_(other.printer_identify_validated_),
      subjects_initialized_(other.subjects_initialized_) {
    // Move buffers
    std::memcpy(printer_name_buffer_, other.printer_name_buffer_, sizeof(printer_name_buffer_));
    std::memcpy(printer_detection_status_buffer_, other.printer_detection_status_buffer_,
                sizeof(printer_detection_status_buffer_));

    // Null out other
    other.screen_root_ = nullptr;
    other.printer_preview_image_ = nullptr;
    other.subjects_initialized_ = false;
    other.printer_identify_validated_ = false;
}

WizardPrinterIdentifyStep&
WizardPrinterIdentifyStep::operator=(WizardPrinterIdentifyStep&& other) noexcept {
    if (this != &other) {
        screen_root_ = other.screen_root_;
        printer_preview_image_ = other.printer_preview_image_;
        printer_name_ = other.printer_name_;
        printer_type_selected_ = other.printer_type_selected_;
        printer_detection_status_ = other.printer_detection_status_;
        printer_identify_validated_ = other.printer_identify_validated_;
        subjects_initialized_ = other.subjects_initialized_;

        // Move buffers
        std::memcpy(printer_name_buffer_, other.printer_name_buffer_, sizeof(printer_name_buffer_));
        std::memcpy(printer_detection_status_buffer_, other.printer_detection_status_buffer_,
                    sizeof(printer_detection_status_buffer_));

        // Null out other
        other.screen_root_ = nullptr;
        other.printer_preview_image_ = nullptr;
        other.subjects_initialized_ = false;
        other.printer_identify_validated_ = false;
    }
    return *this;
}

// ============================================================================
// Helper Functions
// ============================================================================

int WizardPrinterIdentifyStep::find_printer_type_index(const std::string& printer_name) {
    std::istringstream stream(PrinterTypes::PRINTER_TYPES_ROLLER);
    std::string line;
    int index = 0;

    while (std::getline(stream, line)) {
        if (line == printer_name) {
            return index;
        }
        index++;
    }

    return PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX; // "Unknown"
}

/**
 * @brief Detect printer type from hardware discovery data
 *
 * Integrates with PrinterDetector to analyze discovered hardware.
 */
static PrinterDetectionHint detect_printer_type() {
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::debug("[Wizard Printer] No MoonrakerClient available for auto-detection");
        return {PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX, 0, "No printer connection available"};
    }

    // Build hardware data from MoonrakerClient discovery
    PrinterHardwareData hardware;
    hardware.heaters = client->get_heaters();
    hardware.sensors = client->get_sensors();
    hardware.fans = client->get_fans();
    hardware.leds = client->get_leds();
    hardware.hostname = client->get_hostname();

    // Additional detection data sources (Phase 1 enhancement)
    hardware.steppers = client->get_steppers();
    hardware.printer_objects = client->get_printer_objects();
    hardware.kinematics = client->get_kinematics();
    hardware.build_volume = client->get_build_volume();

    // MCU detection data (Phase 3.1)
    hardware.mcu = client->get_mcu();
    hardware.mcu_list = client->get_mcu_list();

    spdlog::debug("[Wizard Printer] Detection data: heaters={}, sensors={}, fans={}, leds={}, "
                  "steppers={}, objects={}, kinematics={}, mcu={}, build=[{:.0f},{:.0f}]",
                  hardware.heaters.size(), hardware.sensors.size(), hardware.fans.size(),
                  hardware.leds.size(), hardware.steppers.size(), hardware.printer_objects.size(),
                  hardware.kinematics, hardware.mcu, hardware.build_volume.x_max,
                  hardware.build_volume.y_max);

    // Run detection engine
    PrinterDetectionResult result = PrinterDetector::detect(hardware);

    if (result.confidence == 0) {
        return {PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX, 0, result.type_name};
    }

    // Map detected type_name to roller index
    int type_index = WizardPrinterIdentifyStep::find_printer_type_index(result.type_name);

    if (type_index == PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX && result.confidence > 0) {
        spdlog::warn(
            "[Wizard Printer] Detected '{}' ({}% confident) but not found in PRINTER_TYPES_ROLLER",
            result.type_name, result.confidence);
        return {PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX, result.confidence,
                result.type_name + " (not in dropdown list)"};
    }

    spdlog::debug("[Wizard Printer] Auto-detected: {} (confidence: {})", result.type_name,
                  result.confidence);
    return {type_index, result.confidence, result.type_name};
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardPrinterIdentifyStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_name = "";
    std::string saved_type = "";
    int default_type = PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX;

    try {
        default_name = config->get<std::string>(WizardConfigPaths::PRINTER_NAME, "");
        saved_type = config->get<std::string>(WizardConfigPaths::PRINTER_TYPE, "");

        // Dynamic lookup: find index by type name
        if (!saved_type.empty()) {
            default_type = find_printer_type_index(saved_type);
            spdlog::debug("[{}] Loaded from config: name='{}', type='{}', resolved index={}",
                          get_name(), default_name, saved_type, default_type);
        } else {
            spdlog::debug("[{}] Loaded from config: name='{}', no type saved", get_name(),
                          default_name);
        }
    } catch (const std::exception& e) {
        spdlog::debug("[{}] No existing config, using defaults", get_name());
    }

    // Auto-fill printer name from Moonraker hostname if not saved
    if (default_name.empty()) {
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            std::string hostname = client->get_hostname();
            spdlog::debug("[{}] Moonraker hostname value: '{}' (empty={}, unknown={})", get_name(),
                          hostname, hostname.empty(), hostname == "unknown");
            if (!hostname.empty() && hostname != "unknown") {
                default_name = hostname;
                spdlog::info("[{}] Auto-filled printer name from hostname: '{}'", get_name(),
                             default_name);
            } else {
                spdlog::debug("[{}] Hostname unavailable for auto-fill", get_name());
            }
        } else {
            spdlog::debug("[{}] No Moonraker client available for hostname auto-fill", get_name());
        }
    }

    // Initialize with values from config or defaults
    strncpy(printer_name_buffer_, default_name.c_str(), sizeof(printer_name_buffer_) - 1);
    printer_name_buffer_[sizeof(printer_name_buffer_) - 1] = '\0';

    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_name_, printer_name_buffer_, printer_name_buffer_,
                                        "printer_name");

    // Run auto-detection if no saved type
    PrinterDetectionHint hint{PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX, 0, ""};
    if (saved_type.empty()) {
        hint = detect_printer_type();
        if (hint.confidence >= 70) {
            default_type = hint.type_index;
            spdlog::debug("[{}] Auto-detection: {} (confidence: {}%)", get_name(), hint.type_name,
                          hint.confidence);
        } else if (hint.confidence > 0) {
            spdlog::debug("[{}] Auto-detection suggestion: {} (confidence: {}%)", get_name(),
                          hint.type_name, hint.confidence);
        } else {
            spdlog::debug("[{}] Auto-detection: {}", get_name(), hint.type_name);
        }
    }

    UI_SUBJECT_INIT_AND_REGISTER_INT(printer_type_selected_, default_type, "printer_type_selected");

    // Initialize detection status message
    const char* status_msg;
    if (!saved_type.empty()) {
        status_msg = "Loaded from configuration";
    } else if (hint.confidence >= 70) {
        snprintf(printer_detection_status_buffer_, sizeof(printer_detection_status_buffer_), "%s",
                 hint.type_name.c_str());
        status_msg = printer_detection_status_buffer_;
    } else if (hint.confidence > 0) {
        snprintf(printer_detection_status_buffer_, sizeof(printer_detection_status_buffer_),
                 "%s (low confidence)", hint.type_name.c_str());
        status_msg = printer_detection_status_buffer_;
    } else {
        status_msg = "No printer detected - please confirm type";
    }

    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_detection_status_, printer_detection_status_buffer_,
                                        status_msg, "printer_detection_status");

    // Initialize validation state
    printer_identify_validated_ = (default_name.length() > 0);

    // Control Next button reactively
    int button_state = printer_identify_validated_ ? 1 : 0;
    lv_subject_set_int(&connection_test_passed, button_state);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (validation: {}, button_state: {})", get_name(),
                  printer_identify_validated_ ? "valid" : "invalid", button_state);
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

void WizardPrinterIdentifyStep::on_printer_name_changed_static(lv_event_t* e) {
    auto* self = static_cast<WizardPrinterIdentifyStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_printer_name_changed(e);
    }
}

void WizardPrinterIdentifyStep::on_printer_type_changed_static(lv_event_t* e) {
    auto* self = static_cast<WizardPrinterIdentifyStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_printer_type_changed(e);
    }
}

// ============================================================================
// Event Handler Implementations
// ============================================================================

void WizardPrinterIdentifyStep::handle_printer_name_changed(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Printer] handle_printer_name_changed");

    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const char* text = lv_textarea_get_text(ta);

    // Trim leading/trailing whitespace for validation
    std::string trimmed(text);
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);

    if (trimmed != text) {
        spdlog::debug("[{}] Name changed (trimmed): '{}' -> '{}'", get_name(), text, trimmed);
    } else {
        spdlog::debug("[{}] Name changed: '{}'", get_name(), text);
    }

    // Update subject with raw text
    lv_subject_copy_string(&printer_name_, text);

    // Validate
    const size_t max_length = sizeof(printer_name_buffer_) - 1;
    bool is_empty = (trimmed.length() == 0);
    bool is_too_long = (trimmed.length() > max_length);
    bool is_valid = !is_empty && !is_too_long;

    printer_identify_validated_ = is_valid;
    lv_subject_set_int(&connection_test_passed, printer_identify_validated_ ? 1 : 0);

    // Apply error state to textarea for validation feedback
    if (is_too_long) {
        lv_color_t error_color = ui_theme_get_color("error_color");
        lv_obj_set_style_border_color(ta, error_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN);
        spdlog::debug("[{}] Validation: name too long ({} > {})", get_name(), trimmed.length(),
                      max_length);
    } else if (!is_empty) {
        const char* sec_color_str = lv_xml_get_const(NULL, "secondary_color");
        lv_color_t valid_color =
            sec_color_str ? ui_theme_parse_color(sec_color_str) : lv_color_hex(0x000000);
        lv_obj_set_style_border_color(ta, valid_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    } else {
        lv_obj_remove_style(ta, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void WizardPrinterIdentifyStep::handle_printer_type_changed(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Printer] handle_printer_type_changed");

    lv_obj_t* roller = static_cast<lv_obj_t*>(lv_event_get_target(event));
    uint16_t selected = static_cast<uint16_t>(lv_roller_get_selected(roller));

    char buf[64];
    lv_roller_get_selected_str(roller, buf, sizeof(buf));

    spdlog::debug("[{}] Type changed: index {} ({})", get_name(), selected, buf);

    // Update subject
    lv_subject_set_int(&printer_type_selected_, selected);

    // Update printer preview image (with fallback to generic CoreXY if missing)
    if (printer_preview_image_) {
        std::string image_path = PrinterImages::get_validated_image_path(selected);
        lv_image_set_src(printer_preview_image_, image_path.c_str());
        spdlog::debug("[{}] Preview image updated: {}", get_name(), image_path);
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardPrinterIdentifyStep::register_callbacks() {
    spdlog::debug("[{}] Registering event callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_printer_name_changed", on_printer_name_changed_static);
    lv_xml_register_event_cb(nullptr, "on_printer_type_changed", on_printer_type_changed_static);

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardPrinterIdentifyStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating printer identification screen", get_name());

    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    // Create from XML
    screen_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_printer_identify", nullptr));

    if (!screen_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Find and set up the roller with printer types
    lv_obj_t* roller = lv_obj_find_by_name(screen_root_, "printer_type_roller");
    if (roller) {
        lv_roller_set_options(roller, PrinterTypes::PRINTER_TYPES_ROLLER, LV_ROLLER_MODE_NORMAL);

        // Set to the saved selection
        int selected = lv_subject_get_int(&printer_type_selected_);
        lv_roller_set_selected(roller, static_cast<uint32_t>(selected), LV_ANIM_OFF);

        // Attach change handler with 'this' as user_data
        lv_obj_add_event_cb(roller, on_printer_type_changed_static, LV_EVENT_VALUE_CHANGED, this);
        spdlog::debug("[{}] Roller configured with {} options", get_name(),
                      PrinterTypes::PRINTER_TYPE_COUNT);
    } else {
        spdlog::warn("[{}] Roller not found in XML", get_name());
    }

    // Find and set up the name textarea
    lv_obj_t* name_ta = lv_obj_find_by_name(screen_root_, "printer_name_input");
    if (name_ta) {
        lv_textarea_set_text(name_ta, printer_name_buffer_);
        lv_obj_add_event_cb(name_ta, on_printer_name_changed_static, LV_EVENT_VALUE_CHANGED, this);
        ui_keyboard_register_textarea(name_ta);
        spdlog::debug("[{}] Name textarea configured (initial: '{}')", get_name(),
                      printer_name_buffer_);
    }

    // Find and set up the printer preview image (with fallback to generic CoreXY if missing)
    printer_preview_image_ = lv_obj_find_by_name(screen_root_, "printer_preview_image");
    if (printer_preview_image_) {
        int selected = lv_subject_get_int(&printer_type_selected_);
        std::string image_path = PrinterImages::get_validated_image_path(selected);
        lv_image_set_src(printer_preview_image_, image_path.c_str());
        spdlog::debug("[{}] Preview image configured: {}", get_name(), image_path);
    } else {
        spdlog::warn("[{}] Printer preview image not found in XML", get_name());
    }

    lv_obj_update_layout(screen_root_);

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardPrinterIdentifyStep::cleanup() {
    spdlog::debug("[{}] Cleaning up printer identification screen", get_name());

    // Save current subject values to config
    Config* config = Config::get_instance();
    try {
        // Get current name from SUBJECT (not buffer - buffer only updates on user typing)
        // The subject contains the authoritative value including auto-fill
        const char* subject_value =
            static_cast<const char*>(lv_subject_get_pointer(&printer_name_));
        std::string current_name(subject_value ? subject_value : "");

        // Trim whitespace
        current_name.erase(0, current_name.find_first_not_of(" \t\n\r\f\v"));
        current_name.erase(current_name.find_last_not_of(" \t\n\r\f\v") + 1);

        // Save printer name if valid
        if (current_name.length() > 0) {
            config->set<std::string>(WizardConfigPaths::PRINTER_NAME, current_name);
            spdlog::debug("[{}] Saving printer name to config: '{}'", get_name(), current_name);
        } else {
            spdlog::debug("[{}] Printer name empty, not saving", get_name());
        }

        // Get current type index and convert to type name
        int type_index = lv_subject_get_int(&printer_type_selected_);

        // Extract type name from PRINTER_TYPES_ROLLER by index
        std::istringstream stream(PrinterTypes::PRINTER_TYPES_ROLLER);
        std::string line;
        int idx = 0;
        std::string type_name = "Unknown";

        while (std::getline(stream, line)) {
            if (idx == type_index) {
                type_name = line;
                break;
            }
            idx++;
        }

        // Save printer type name
        config->set<std::string>(WizardConfigPaths::PRINTER_TYPE, type_name);
        spdlog::debug("[{}] Saving printer type to config: '{}' (index {})", get_name(), type_name,
                      type_index);

        // Persist config changes
        if (config->save()) {
            spdlog::debug("[{}] Saved printer identification settings", get_name());
        } else {
            NOTIFY_ERROR("Failed to save printer configuration");
            LOG_ERROR_INTERNAL("[{}] Failed to save config to disk!", get_name());
        }
    } catch (const std::exception& e) {
        NOTIFY_ERROR("Error saving printer settings: {}", e.what());
        LOG_ERROR_INTERNAL("[{}] Failed to save config: {}", get_name(), e.what());
    }

    // Reset UI references (wizard framework handles deletion)
    screen_root_ = nullptr;
    printer_preview_image_ = nullptr;

    // Reset connection_test_passed to enabled (1) for other wizard steps
    lv_subject_set_int(&connection_test_passed, 1);

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardPrinterIdentifyStep::is_validated() const {
    return printer_identify_validated_;
}
