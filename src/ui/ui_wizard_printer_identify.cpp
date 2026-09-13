// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_printer_identify.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_subject_registry.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_detector.h"
#include "printer_images.h"
#include "printer_name_sync.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "wizard_config_paths.h"
#include "wizard_step_logic.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

using namespace helix;

// Selector view states, values of the wizard_printer_view subject the XML
// containers and header bind to.
namespace {
constexpr int kViewTiles = 0;  // vendor tile grid (browse level)
constexpr int kViewVendor = 1; // one vendor's models, back affordance visible
constexpr int kViewSearch = 2; // flat matches across every machine
} // namespace

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
        StaticPanelRegistry::instance().register_destroy(
            "WizardPrinterIdentifyStep", []() { g_wizard_printer_identify_step.reset(); });
    }
    return g_wizard_printer_identify_step.get();
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
    // list_cache_container_ / tile_cache_container_ are parented to
    // lv_layer_sys() — lv_deinit() handles them
    list_cache_container_ = nullptr;
    tile_cache_container_ = nullptr;
}

// ============================================================================
// ============================================================================
// Helper Functions
// ============================================================================

int WizardPrinterIdentifyStep::find_printer_type_index(const std::string& printer_name) {
    // Use dynamic list from PrinterDetector (data-driven from database)
    // NOTE: This static method uses the unfiltered list. For kinematics-filtered
    // lookups, call PrinterDetector::find_list_index(name, kinematics) directly.
    return PrinterDetector::find_list_index(printer_name);
}

/**
 * @brief Detect printer type from hardware discovery data
 *
 * Uses PrinterDetector::auto_detect() and maps result to list index.
 * Uses kinematics-filtered list when kinematics is provided.
 */
static PrinterDetectionHint detect_printer_type(const std::string& kinematics) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[Wizard Printer] No IMoonrakerAPI available for auto-detection");
        return {PrinterDetector::get_unknown_list_index(kinematics), 0,
                "No printer connection available"};
    }

    // Use shared auto_detect() which handles building PrinterHardwareData
    PrinterDetectionResult result = PrinterDetector::auto_detect(api->hardware());

    if (result.confidence == 0) {
        return {PrinterDetector::get_unknown_list_index(kinematics), 0, result.type_name};
    }

    // Map detected type_name to list index (filtered by kinematics)
    int type_index = PrinterDetector::find_list_index(result.type_name, kinematics);

    if (type_index == PrinterDetector::get_unknown_list_index(kinematics) &&
        result.confidence > 0) {
        spdlog::warn("[Wizard Printer] Detected '{}' ({}% confident) but not found in printer list",
                     result.type_name, result.confidence);
        return {PrinterDetector::get_unknown_list_index(kinematics), result.confidence,
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
    // Check if we're connected to a DIFFERENT printer than last time
    std::string current_url;
    IMoonrakerClient* client = get_moonraker_client();
    if (client) {
        current_url = client->get_last_url();
    }

    bool printer_changed = !last_detected_url_.empty() && current_url != last_detected_url_;
    if (printer_changed) {
        spdlog::info("[{}] Printer URL changed from '{}' to '{}' - forcing re-detection",
                     get_name(), last_detected_url_, current_url);
        subjects_initialized_ = false; // Force re-initialization

        // Invalidate cached list — kinematics filter may differ for new printer
        if (list_cache_container_) {
            lv_obj_clean(list_cache_container_);
        }
        if (tile_cache_container_) {
            lv_obj_clean(tile_cache_container_);
        }

        // Clear saved printer type so detection runs fresh for new printer
        Config* config = Config::get_instance();
        config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
        config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, "");
        spdlog::debug("[{}] Cleared saved printer config for new printer", get_name());
    }

    // Only initialize subjects once - they persist across wizard navigation
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized, skipping", get_name());
        return;
    }

    // Track current URL for change detection on future visits
    last_detected_url_ = current_url;
    spdlog::debug("[{}] Tracking printer URL: '{}'", get_name(), last_detected_url_);

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Detect kinematics FIRST — all list index lookups below use filtered APIs
    {
        IMoonrakerAPI* api = get_moonraker_api();
        if (api) {
            detected_kinematics_ = api->hardware().kinematics();
            spdlog::info("[{}] Detected kinematics: '{}' (will filter printer list)", get_name(),
                         detected_kinematics_);
        } else {
            spdlog::debug("[{}] No IMoonrakerAPI — printer list will be unfiltered", get_name());
        }
    }

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_name = "";
    std::string saved_type = "";
    int default_type = PrinterDetector::get_unknown_list_index(detected_kinematics_);

    try {
        default_name = config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "");
        saved_type = config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");

        // Dynamic lookup: find index by type name (using filtered list)
        if (!saved_type.empty()) {
            default_type = PrinterDetector::find_list_index(saved_type, detected_kinematics_);
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
        IMoonrakerAPI* api = get_moonraker_api();
        if (api) {
            std::string hostname = api->hardware().hostname();
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
            spdlog::debug("[{}] No IMoonrakerAPI available for hostname auto-fill", get_name());
        }
    }

    // Initialize with values from config or defaults
    strncpy(printer_name_buffer_, default_name.c_str(), sizeof(printer_name_buffer_) - 1);
    printer_name_buffer_[sizeof(printer_name_buffer_) - 1] = '\0';

    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_name_, printer_name_buffer_, printer_name_buffer_,
                                        "printer_name");

    // Always run auto-detection (even when config has a saved type, e.g. re-running wizard)
    PrinterDetectionHint hint = detect_printer_type(detected_kinematics_);
    if (hint.confidence >= 70) {
        // High-confidence detection overrides saved type
        default_type = hint.type_index;
        spdlog::info("[{}] Auto-detection: {} (confidence: {}%)", get_name(), hint.type_name,
                     hint.confidence);
    } else if (hint.confidence > 0) {
        spdlog::info("[{}] Auto-detection suggestion: {} (confidence: {}%)", get_name(),
                     hint.type_name, hint.confidence);
        // Low confidence: keep saved type if available, otherwise use suggestion
        if (saved_type.empty()) {
            default_type = hint.type_index;
        }
    } else {
        spdlog::debug("[{}] Auto-detection: no match", get_name());
    }

    UI_SUBJECT_INIT_AND_REGISTER_INT(printer_type_selected_, default_type, "printer_type_selected");

    // Selector view state: starts at the vendor tile grid; create() re-targets
    // it to the selected machine's bucket when a real machine is selected.
    UI_SUBJECT_INIT_AND_REGISTER_INT(printer_view_, kViewTiles, "wizard_printer_view");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(vendor_title_, vendor_title_buffer_, "",
                                        "wizard_printer_vendor_title");
    UI_SUBJECT_INIT_AND_REGISTER_INT(match_count_, 1, "wizard_printer_match_count");

    // Initialize detection status message
    const char* status_msg;
    if (hint.confidence >= 70) {
        snprintf(printer_detection_status_buffer_, sizeof(printer_detection_status_buffer_), "%s",
                 hint.type_name.c_str());
        status_msg = printer_detection_status_buffer_;
    } else if (hint.confidence > 0) {
        snprintf(printer_detection_status_buffer_, sizeof(printer_detection_status_buffer_),
                 "%s (low confidence)", hint.type_name.c_str());
        status_msg = printer_detection_status_buffer_;
    } else if (!saved_type.empty()) {
        status_msg = "Loaded from configuration";
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

    // Re-entry guard: if we're updating FROM the subject, don't update it again
    if (updating_from_subject_) {
        return;
    }

    // Trim leading/trailing whitespace for validation
    std::string trimmed(text);
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);

    if (trimmed != text) {
        spdlog::debug("[{}] Name changed (trimmed): '{}' -> '{}'", get_name(), text, trimmed);
    } else {
        spdlog::debug("[{}] Name changed: '{}'", get_name(), text);
    }

    // Update subject with raw text (guard prevents re-entry from observer notification)
    updating_from_subject_ = true;
    lv_subject_copy_string(&printer_name_, text);
    updating_from_subject_ = false;

    // Validate
    const size_t max_length = sizeof(printer_name_buffer_) - 1;
    bool is_empty = (trimmed.length() == 0);
    bool is_too_long = (trimmed.length() > max_length);
    bool is_valid = !is_empty && !is_too_long;

    printer_identify_validated_ = is_valid;
    lv_subject_set_int(&connection_test_passed, printer_identify_validated_ ? 1 : 0);

    // Log validation issues for debugging (Next button state is the user-facing feedback)
    if (is_too_long) {
        spdlog::debug("[{}] Validation: name too long ({} > {})", get_name(), trimmed.length(),
                      max_length);
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

    // Update printer preview image (resolve name from filtered list)
    if (printer_preview_image_) {
        std::string name = PrinterDetector::get_list_name_at(selected, detected_kinematics_);
        std::string image_path = PrinterImages::get_image_path_for_name(name);
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
    lv_xml_register_event_cb(nullptr, "on_wizard_printer_search_changed",
                             on_wizard_printer_search_changed);
    lv_xml_register_event_cb(nullptr, "on_wizard_vendor_back_clicked",
                             on_wizard_vendor_back_clicked);

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardPrinterIdentifyStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating printer identification screen", get_name());
    crash_handler::breadcrumb::note("wpi", "create_enter", 0);

    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    // Create from XML
    screen_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_printer_identify", nullptr));
    crash_handler::breadcrumb::note("wpi", "xml_created", screen_root_ ? 1 : 0);

    if (!screen_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Find the printer type list container from XML
    lv_obj_t* xml_list = lv_obj_find_by_name(screen_root_, "printer_type_list");
    if (xml_list) {
        if (list_cache_container_ && lv_obj_get_child_count(list_cache_container_) > 0) {
            // Cached list exists from a previous visit — reparent children back
            // instead of rebuilding ~105 buttons (slow on MIPS, see issue #231)
            printer_type_list_ = xml_list;
            while (lv_obj_get_child_count(list_cache_container_) > 0) {
                lv_obj_t* child = lv_obj_get_child(list_cache_container_, 0);
                lv_obj_set_parent(child, printer_type_list_);
            }
            // Update selection highlight to reflect current state
            int selected = lv_subject_get_int(&printer_type_selected_);
            update_list_selection(selected);
            spdlog::debug("[{}] Restored cached printer type list ({} items)", get_name(),
                          lv_obj_get_child_count(printer_type_list_));
        } else {
            // First visit — build the list from scratch
            printer_type_list_ = xml_list;
            populate_printer_type_list();
            spdlog::debug("[{}] Printer type list populated with {} items", get_name(),
                          PrinterDetector::get_list_names(detected_kinematics_).size());
        }
    } else {
        spdlog::warn("[{}] Printer type list not found in XML", get_name());
    }

    // Vendor tile grid: same build-once/cache-across-visits treatment as the
    // rows, for the same reason.
    vendor_tiles_ = lv_obj_find_by_name(screen_root_, "vendor_tiles");
    if (vendor_tiles_) {
        if (tile_cache_container_ && lv_obj_get_child_count(tile_cache_container_) > 0) {
            while (lv_obj_get_child_count(tile_cache_container_) > 0) {
                lv_obj_t* child = lv_obj_get_child(tile_cache_container_, 0);
                lv_obj_set_parent(child, vendor_tiles_);
            }
        } else {
            populate_vendor_tiles();
        }
    } else {
        spdlog::warn("[{}] Vendor tile grid not found in XML", get_name());
    }

    // A fresh textarea comes back empty; browsing state follows it.
    search_query_.clear();
    enter_initial_view();

    // Find and set up the name textarea
    lv_obj_t* name_ta = lv_obj_find_by_name(screen_root_, "printer_name_input");
    if (name_ta) {
        lv_textarea_set_text(name_ta, printer_name_buffer_);
        lv_obj_add_event_cb(name_ta, on_printer_name_changed_static, LV_EVENT_VALUE_CHANGED, this);
        spdlog::debug("[{}] Name textarea configured (initial: '{}')", get_name(),
                      printer_name_buffer_);
    }

    // Find and set up the printer preview image (with fallback to generic CoreXY if missing)
    printer_preview_image_ = lv_obj_find_by_name(screen_root_, "printer_preview_image");
    if (printer_preview_image_) {
        int selected = lv_subject_get_int(&printer_type_selected_);
        // Resolve name from filtered list, then look up image by name
        std::string name = PrinterDetector::get_list_name_at(selected, detected_kinematics_);
        std::string image_path = PrinterImages::get_image_path_for_name(name);
        lv_image_set_src(printer_preview_image_, image_path.c_str());
        spdlog::debug("[{}] Preview image configured: {}", get_name(), image_path);
    } else {
        spdlog::warn("[{}] Printer preview image not found in XML", get_name());
    }

    lv_obj_update_layout(screen_root_);

    spdlog::debug("[{}] Screen created successfully", get_name());
    crash_handler::breadcrumb::note("wpi", "create_end", 0);
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardPrinterIdentifyStep::cleanup() {
    spdlog::debug("[{}] Cleaning up printer identification screen", get_name());
    crash_handler::breadcrumb::note("wpi", "cleanup_enter", 0);

    // Save current subject values to config
    Config* config = Config::get_instance();
    try {
        // Get current name from SUBJECT using lv_subject_get_string() for string subjects
        const char* subject_value = lv_subject_get_string(&printer_name_);

        spdlog::debug("[{}] Subject value: '{}'", get_name(),
                      subject_value ? subject_value : "(null)");

        std::string current_name(subject_value ? subject_value : "");

        // Trim whitespace
        current_name.erase(0, current_name.find_first_not_of(" \t\n\r\f\v"));
        current_name.erase(current_name.find_last_not_of(" \t\n\r\f\v") + 1);

        spdlog::debug("[{}] After trim: '{}' (length={})", get_name(), current_name,
                      current_name.length());

        // Save printer name if valid
        if (current_name.length() > 0) {
            config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, current_name);
            spdlog::debug("[{}] Saving printer name to config: '{}'", get_name(), current_name);
            // Sync name to Mainsail/Fluidd DB
            helix::PrinterNameSync::write_back(get_moonraker_api(), current_name);
        } else {
            spdlog::debug("[{}] Printer name empty, not saving", get_name());
        }

        // Get current type index and convert to type name (via dynamic database lookup)
        int type_index = lv_subject_get_int(&printer_type_selected_);
        std::string type_name = PrinterDetector::get_list_name_at(type_index, detected_kinematics_);

        // Save the printer type and merge its preset. Shared with the Printer
        // Manager's model row via PrinterDetector::apply_type_choice() - both
        // are "the user picked this model", and detection now declines to guess
        // whenever it is unsure, so these hand-pick paths carry real traffic.
        spdlog::debug("[{}] Saving printer type to config: '{}' (index {})", get_name(), type_name,
                      type_index);

        IMoonrakerAPI* api = get_moonraker_api();
        if (api) {
            std::string applied =
                PrinterDetector::apply_type_choice(config, type_name, api->hardware());
            if (!applied.empty()) {
                // apply_preset_with_variants() persists the top-level "preset"
                // marker, and Config::has_preset() is what collapses this step
                // plus every hardware picker and the summary. cleanup() runs on
                // Back as well as Next, so an interrupted run (crash, power cut,
                // user quits) previously came back with all of them gone and no
                // in-app way to revisit a mis-detected pick. Mark it provisional;
                // it becomes authoritative only when wizard_completed flips.
                config->set<bool>(config->df() + helix::WIZARD_PRESET_PROVISIONAL, true);
                // Keep the current run collapsing the now-redundant steps.
                helix::wizard_mark_preset_applied_this_session();
            }
        } else {
            // No Moonraker yet: still record the pick so the summary and the
            // next boot agree with what the user selected.
            config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, type_name);
        }

        // Display/backlight hardware settings are the responsibility of the
        // printer preset (assets/config/presets/*.json), not the identify
        // wizard. Previously this block force-wrote AD5X/CC1-specific display
        // overrides on every confirmation, which contradicted the ad5x preset
        // after #431 and caused user-visible sleep bugs — see
        // docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md "Known Issue: Random
        // solid colors during sleep". Config migration v14→v15 cleans up stale
        // values for users who ran the pre-fix wizard.

        // Persist config changes
        if (config->save()) {
            spdlog::debug("[{}] Saved printer identification settings", get_name());
        } else {
            NOTIFY_ERROR(lv_tr("Failed to save printer configuration"));
            LOG_ERROR_INTERNAL("[{}] Failed to save config to disk!", get_name());
        }
    } catch (const std::exception& e) {
        NOTIFY_ERROR(lv_tr("Error saving printer settings: {}"), e.what());
        LOG_ERROR_INTERNAL("[{}] Failed to save config: {}", get_name(), e.what());
    }

    // Cache the printer type list so we don't rebuild ~105 buttons on revisit.
    // Reparent children to a persistent off-screen container before the wizard
    // framework deletes the step's widget tree. (issue #231)
    if (printer_type_list_ && lv_obj_get_child_count(printer_type_list_) > 0) {
        if (!list_cache_container_) {
            // Create a persistent off-screen container (parented to active screen's layer)
            list_cache_container_ = lv_obj_create(lv_layer_sys());
            lv_obj_add_flag(list_cache_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(list_cache_container_, 0, 0);
        }
        while (lv_obj_get_child_count(printer_type_list_) > 0) {
            lv_obj_t* child = lv_obj_get_child(printer_type_list_, 0);
            lv_obj_set_parent(child, list_cache_container_);
        }
        spdlog::debug("[{}] Cached {} printer list items for reuse", get_name(),
                      lv_obj_get_child_count(list_cache_container_));
    }

    // Same for the vendor tiles.
    if (vendor_tiles_ && lv_obj_get_child_count(vendor_tiles_) > 0) {
        if (!tile_cache_container_) {
            tile_cache_container_ = lv_obj_create(lv_layer_sys());
            lv_obj_add_flag(tile_cache_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(tile_cache_container_, 0, 0);
        }
        while (lv_obj_get_child_count(vendor_tiles_) > 0) {
            lv_obj_t* child = lv_obj_get_child(vendor_tiles_, 0);
            lv_obj_set_parent(child, tile_cache_container_);
        }
        spdlog::debug("[{}] Cached {} vendor tiles for reuse", get_name(),
                      lv_obj_get_child_count(tile_cache_container_));
    }

    // Reset UI references (wizard framework handles deletion)
    screen_root_ = nullptr;
    printer_preview_image_ = nullptr;
    printer_type_list_ = nullptr;
    vendor_tiles_ = nullptr;

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

// ============================================================================
// Printer Type List Helpers
// ============================================================================

void WizardPrinterIdentifyStep::populate_printer_type_list() {
    if (!printer_type_list_) {
        return;
    }

    // Clear any existing children
    lv_obj_clean(printer_type_list_);

    // Mirror the detector's list as selector entries (names + manufacturers);
    // rows are created in the same order so child i is entry i.
    build_selector_entries();
    int selected = lv_subject_get_int(&printer_type_selected_);

    for (size_t i = 0; i < selector_entries_.size(); ++i) {
        const auto& entry = selector_entries_[i];

        // Create button for each printer type
        lv_obj_t* btn = lv_obj_create(printer_type_list_);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(btn, theme_manager_get_spacing("space_md"), LV_PART_MAIN);
        lv_obj_set_style_radius(btn, theme_manager_get_spacing("border_radius"), LV_PART_MAIN);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

        // Style based on selection state - non-selected items are transparent
        if (static_cast<int>(i) == selected) {
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        }

        // Create label inside button
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, entry.label.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
        lv_obj_set_flex_grow(label, 1);

        // Set text color based on selection
        if (static_cast<int>(i) == selected) {
            // Use contrast color for selected item
            lv_color_t primary = theme_manager_get_color("primary");
            uint8_t lum = lv_color_luminance(primary);
            lv_color_t text_color = (lum > 140) ? lv_color_black() : lv_color_white();
            lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);
        }

        // Vendor shown beside the model while searching, hidden otherwise (a
        // drilled-in list is single-vendor by construction). Pseudo-machines
        // have no vendor to show. Child 1 of the row.
        lv_obj_t* vendor_label = lv_label_create(btn);
        lv_label_set_text(vendor_label, entry.group.c_str());
        lv_obj_set_style_text_font(vendor_label, theme_manager_get_font("font_small"),
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(vendor_label, theme_manager_get_color("text_muted"),
                                    LV_PART_MAIN);
        if (entry.group.empty()) {
            lv_obj_add_flag(vendor_label, LV_OBJ_FLAG_HIDDEN);
        }

        // Store index in user_data and attach click handler
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(btn, on_printer_type_item_clicked, LV_EVENT_CLICKED, this);
    }
}

void WizardPrinterIdentifyStep::update_list_selection(int selected_index) {
    if (!printer_type_list_) {
        return;
    }

    uint32_t child_count = lv_obj_get_child_count(printer_type_list_);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* btn = lv_obj_get_child(printer_type_list_, static_cast<int32_t>(i));
        if (!btn)
            continue;

        lv_obj_t* label = lv_obj_get_child(btn, 0);
        bool is_selected = (static_cast<int>(i) == selected_index);

        if (is_selected) {
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            if (label) {
                lv_color_t primary = theme_manager_get_color("primary");
                uint8_t lum = lv_color_luminance(primary);
                lv_color_t text_color = (lum > 140) ? lv_color_black() : lv_color_white();
                lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);
            }
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            if (label) {
                lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);
            }
        }
    }
}

// ============================================================================
// Vendor Drill-In + Search
// ============================================================================

void WizardPrinterIdentifyStep::build_selector_entries() {
    const auto& list = PrinterDetector::get_list_entries(detected_kinematics_);
    selector_entries_.clear();
    selector_entries_.reserve(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        selector_entries_.push_back({list[i].name, list[i].manufacturer, static_cast<int>(i)});
    }
}

void WizardPrinterIdentifyStep::populate_vendor_tiles() {
    if (!vendor_tiles_) {
        return;
    }

    lv_obj_clean(vendor_tiles_);
    vendor_groups_ = ui::group_selector_entries(selector_entries_);

    for (size_t g = 0; g < vendor_groups_.size(); ++g) {
        lv_obj_t* tile = lv_obj_create(vendor_tiles_);
        char tile_name[32];
        snprintf(tile_name, sizeof(tile_name), "vendor_tile_%u", static_cast<unsigned>(g));
        lv_obj_set_name(tile, tile_name);
        lv_obj_set_width(tile, lv_pct(48));
        lv_obj_set_height(tile, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(tile, theme_manager_get_spacing("space_md"), LV_PART_MAIN);
        lv_obj_set_style_radius(tile, theme_manager_get_spacing("border_radius"), LV_PART_MAIN);
        lv_obj_set_style_bg_color(tile, theme_manager_get_color("card_bg"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* label = lv_label_create(tile);
        lv_label_set_text(label, vendor_groups_[g].name.c_str());
        // Vendor names are single unbreakable words at this tile width
        // ("PrintersForAnts"), so wrapping would clip; dots keep them legible.
        // Width is the tile's content width so the dots mode has a bound.
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);

        lv_obj_set_user_data(tile, reinterpret_cast<void*>(g));
        lv_obj_add_event_cb(tile, on_vendor_tile_clicked, LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[{}] Built {} vendor tiles", get_name(), vendor_groups_.size());
}

void WizardPrinterIdentifyStep::apply_view(int view) {
    lv_subject_set_int(&printer_view_, view);

    if (!printer_type_list_) {
        return;
    }

    int visible = 0;
    const uint32_t row_count = lv_obj_get_child_count(printer_type_list_);
    for (uint32_t i = 0; i < row_count; ++i) {
        lv_obj_t* row = lv_obj_get_child(printer_type_list_, static_cast<int32_t>(i));
        if (!row || i >= selector_entries_.size()) {
            continue;
        }
        const auto& entry = selector_entries_[i];

        const bool show =
            (view == kViewSearch)
                ? ui::selector_entry_matches(entry, search_query_)
                : (view == kViewVendor) && (ui::selector_bucket_of(entry) == active_vendor_);
        if (show) {
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
            ++visible;
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }

        // Child 1 of a row is its vendor label (child 0 is the model name);
        // shown only while searching, where the flat list mixes vendors.
        lv_obj_t* vendor_label = lv_obj_get_child(row, 1);
        if (vendor_label) {
            if (view == kViewSearch && !entry.group.empty()) {
                lv_obj_remove_flag(vendor_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(vendor_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    lv_subject_set_int(&match_count_, visible);
    lv_obj_scroll_to_y(printer_type_list_, 0, LV_ANIM_OFF);
}

void WizardPrinterIdentifyStep::enter_initial_view() {
    const int selected = lv_subject_get_int(&printer_type_selected_);
    if (selected >= 0 && static_cast<size_t>(selected) < selector_entries_.size()) {
        const auto& entry = selector_entries_[selected];
        // A detected or previously selected machine opens inside its vendor's
        // bucket, scrolled to the machine. Pseudo-machines ("Custom/Other",
        // "Unknown") and no selection start at the tile grid.
        if (!entry.group.empty()) {
            active_vendor_ = entry.group;
            snprintf(vendor_title_buffer_, sizeof(vendor_title_buffer_), "%s", entry.group.c_str());
            lv_subject_notify(&vendor_title_);
            apply_view(kViewVendor);
            if (printer_type_list_) {
                lv_obj_t* row = lv_obj_get_child(printer_type_list_, selected);
                if (row) {
                    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
                }
            }
            spdlog::debug("[{}] Opened vendor '{}' at selected machine", get_name(),
                          active_vendor_);
            return;
        }
    }

    active_vendor_.clear();
    apply_view(kViewTiles);
}

void WizardPrinterIdentifyStep::on_wizard_printer_search_changed(lv_event_t* e) {
    WizardPrinterIdentifyStep* self = get_wizard_printer_identify_step();
    if (!self) {
        return;
    }

    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    std::string query(text ? text : "");
    query.erase(0, query.find_first_not_of(" \t\n\r\f\v"));
    query.erase(query.find_last_not_of(" \t\n\r\f\v") + 1);

    self->search_query_ = query;
    self->apply_view(query.empty() ? kViewTiles : kViewSearch);
}

void WizardPrinterIdentifyStep::on_wizard_vendor_back_clicked(lv_event_t* e) {
    (void)e;
    WizardPrinterIdentifyStep* self = get_wizard_printer_identify_step();
    if (!self) {
        return;
    }

    self->active_vendor_.clear();
    self->apply_view(kViewTiles);
    spdlog::debug("[{}] Returned to vendor tile grid", self->get_name());
}

void WizardPrinterIdentifyStep::on_vendor_tile_clicked(lv_event_t* e) {
    auto* self = static_cast<WizardPrinterIdentifyStep*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }

    lv_obj_t* tile = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int group_index =
        static_cast<int>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(tile)));
    if (group_index < 0 || static_cast<size_t>(group_index) >= self->vendor_groups_.size()) {
        return;
    }

    self->active_vendor_ = self->vendor_groups_[group_index].name;
    snprintf(self->vendor_title_buffer_, sizeof(self->vendor_title_buffer_), "%s",
             self->active_vendor_.c_str());
    lv_subject_notify(&self->vendor_title_);
    self->apply_view(kViewVendor);
    spdlog::debug("[{}] Drilled into vendor '{}'", self->get_name(), self->active_vendor_);
}

void WizardPrinterIdentifyStep::on_printer_type_item_clicked(lv_event_t* e) {
    auto* self = static_cast<WizardPrinterIdentifyStep*>(lv_event_get_user_data(e));
    if (!self)
        return;

    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int index = static_cast<int>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn)));

    const auto& names = PrinterDetector::get_list_names(self->detected_kinematics_);
    if (index >= 0 && index < static_cast<int>(names.size())) {
        spdlog::debug("[{}] Type selected: index {} ({})", self->get_name(), index, names[index]);

        // Update subject
        lv_subject_set_int(&self->printer_type_selected_, index);

        // Update visual selection
        self->update_list_selection(index);

        // Update printer preview image (resolve name from filtered list)
        if (self->printer_preview_image_) {
            std::string image_path = PrinterImages::get_image_path_for_name(names[index]);
            lv_image_set_src(self->printer_preview_image_, image_path.c_str());
            spdlog::debug("[{}] Preview image updated: {}", self->get_name(), image_path);
        }
    }
}
