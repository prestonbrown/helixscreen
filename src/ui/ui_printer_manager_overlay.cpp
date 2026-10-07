// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_manager_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_keyboard_manager.h"
#include "ui_nav.h"
#include "ui_overlay_printer_image.h"
#include "ui_overlay_printer_type.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_panel_ams.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_power.h"
#include "ui_panel_screws_tilt.h"
#include "ui_panel_spoolman.h"
#include "ui_printer_list_overlay.h"
#include "ui_settings_led.h"
#include "ui_settings_sound.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "prerendered_images.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "printer_name_sync.h"
#include "static_panel_registry.h"
#include "subject_debug_registry.h"
#include "ui/ui_widget_helpers.h"
#include "wizard_config_paths.h"

#include <lvgl/src/misc/cache/lv_cache.h>
#include <spdlog/spdlog.h>

#include <cstring>

using namespace helix;

// =============================================================================
// Destructor
// =============================================================================

PrinterManagerOverlay::~PrinterManagerOverlay() {
    if (lv_is_initialized()) {
        deinit_subjects_base(subjects_);
    }
}

// =============================================================================
// Subject Initialization
// =============================================================================

void PrinterManagerOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(printer_manager_name_, name_buf_, "Unknown",
                                  "printer_manager_name", subjects_);
        UI_MANAGED_SUBJECT_STRING(printer_manager_model_, model_buf_, "", "printer_manager_model",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(helix_version_, version_buf_, "0.0.0", "helix_version",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(name_editing_, 0, "pm_name_editing", subjects_);
    });
}

// =============================================================================
// Create
// =============================================================================

lv_obj_t* PrinterManagerOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, xml_component())) {
        return nullptr;
    }

    // Find the printer image widget for programmatic image source setting
    printer_image_obj_ = helix::ui::find_required(overlay_root_, "pm_printer_image", get_name());

    // Find name editing widgets (visibility driven by pm_name_editing subject via XML bindings)
    name_input_ = helix::ui::find_required(overlay_root_, "pm_printer_name_input", get_name());

    // Register READY/CANCEL on textarea for name edit lifecycle
    // (acceptable exception to declarative rule — textarea lifecycle event, like DELETE cleanup)
    if (name_input_) {
        lv_obj_add_event_cb(
            name_input_,
            [](lv_event_t*) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] name_input_ready");
                get_printer_manager_overlay().finish_name_edit();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_READY, nullptr);
        lv_obj_add_event_cb(
            name_input_,
            [](lv_event_t*) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] name_input_cancel");
                get_printer_manager_overlay().cancel_name_edit();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CANCEL, nullptr);
    }

    // Clicking the overlay content area while editing saves and dismisses the input.
    // LVGL DEFOCUSED events don't fire reliably on touchscreens, so we use a
    // click handler on the scrollable content instead.
    if (auto* content = helix::ui::find_required(overlay_root_, "overlay_content", get_name())) {
        lv_obj_add_event_cb(
            content,
            [](lv_event_t*) {
                auto& pm = get_printer_manager_overlay();
                if (lv_subject_get_int(&pm.name_editing_) != 0) {
                    pm.finish_name_edit();
                }
            },
            LV_EVENT_CLICKED, nullptr);
    }

#if defined(HELIX_PLATFORM_ESP32)
    // v1 Core+AMS cut has no camera, so timelapse is unavailable. The chip's XML
    // already hides it when printer_has_timelapse==0; force it hidden here too so
    // it never renders regardless of printer capability. Imperative (not a stacked
    // bind_flag) because the chip already carries a printer_has_timelapse binding
    // and this helix-xml build has no compound-condition (subject_expr/cond) support.
    // Compiled out on desktop → zero desktop impact.
    if (auto* timelapse_chip =
            helix::ui::find_required(overlay_root_, "pm_chip_timelapse", get_name())) {
        // DECLARATIVE_OK: compile-time capability (#if), no runtime subject exists
        lv_obj_add_flag(timelapse_chip, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    return overlay_root_;
}

namespace {

// =============================================================================
// Chip Navigation Callbacks
// =============================================================================

void on_chip_bed_mesh_clicked(lv_event_t*) {
    spdlog::debug("[Printer Manager] Bed Mesh chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_bed_mesh_panel().show(lv_display_get_screen_active(nullptr));
}

void on_chip_adxl_clicked(lv_event_t*) {
    spdlog::debug("[Printer Manager] ADXL chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_input_shaper_panel().show(lv_display_get_screen_active(nullptr));
}

void on_chip_screws_tilt_clicked(lv_event_t*) {
    spdlog::debug("[Printer Manager] Screws Tilt chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_screws_tilt_panel().set_client(get_moonraker_client(), get_moonraker_api());
    get_global_screws_tilt_panel().show(lv_display_get_screen_active(nullptr));
}

void on_chip_ams_clicked(lv_event_t*) {
    spdlog::debug("[Printer Manager] AMS chip clicked");

    auto& ams_panel = get_global_ams_panel();
    if (!ams_panel.are_subjects_initialized()) {
        ams_panel.init_subjects();
    }
    lv_obj_t* panel_obj = ams_panel.get_panel();
    if (panel_obj) {
        // Re-register before push: get_global_ams_panel() only registers in its
        // lazy-creation block, and navbar switches clear overlay_instances_, so a
        // cached panel loses its registration and on_deactivate() never fires on
        // dismiss — leaving the filament-path animation drawing into a torn-down
        // panel. Idempotent.
        helix::nav::register_overlay(panel_obj, &ams_panel);
        helix::nav::push_overlay(panel_obj);
    }
}

void on_chip_power_clicked(lv_event_t*) {
    spdlog::debug("[Printer Manager] Power Devices chip clicked");
    auto& panel = get_global_power_panel();
    lv_obj_t* overlay = panel.get_or_create_overlay(lv_display_get_screen_active(nullptr));
    if (overlay) {
        helix::nav::push_overlay(overlay);
    }
}

} // namespace

// =============================================================================
// Callbacks
// =============================================================================

void PrinterManagerOverlay::register_callbacks() {
    register_xml_callbacks({
        {"pm_chip_bed_mesh_clicked", on_chip_bed_mesh_clicked},
        {"pm_chip_leds_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] LEDs chip clicked");
             auto& overlay = helix::settings::get_led_settings_overlay();
             overlay.show(lv_display_get_screen_active(nullptr));
         }},
        {"pm_chip_adxl_clicked", on_chip_adxl_clicked},
        {"pm_chip_retraction_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Retraction chip clicked");
             get_global_retraction_settings().show(lv_display_get_screen_active(nullptr));
         }},
        {"pm_chip_spoolman_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Spoolman chip clicked");
             get_global_spoolman_panel().show(lv_display_get_screen_active(nullptr));
         }},
        {"pm_chip_timelapse_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Timelapse chip clicked");
             get_global_timelapse_settings().show(lv_display_get_screen_active(nullptr));
         }},
        {"pm_chip_screws_tilt_clicked", on_chip_screws_tilt_clicked},
        {"pm_chip_ams_clicked", on_chip_ams_clicked},
        {"pm_chip_fans_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Fans chip clicked");
             helix::open_fan_control_overlay(lv_display_get_screen_active(nullptr));
         }},
        {"pm_chip_power_clicked", on_chip_power_clicked},
        {"pm_chip_speaker_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Speaker chip clicked");
             auto& overlay = helix::settings::get_sound_settings_overlay();
             overlay.show(lv_display_get_screen_active(nullptr));
         }},
        {"pm_printer_name_clicked",
         [](lv_event_t*) { get_printer_manager_overlay().start_name_edit(); }},
        {"on_change_printer_image_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Printer image clicked, opening image picker");
             helix::settings::get_printer_image_overlay().show(
                 lv_display_get_screen_active(nullptr));
         }},
        {"on_change_printer_model_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Printer Manager] Printer model clicked, opening model picker");
             helix::settings::get_printer_type_overlay().show(
                 lv_display_get_screen_active(nullptr));
         }},
        {"pm_manage_printers_clicked",
         [](lv_event_t*) {
             spdlog::info("[Printer Manager] Manage Printers clicked, opening printer list");
             helix::ui::get_printer_list_overlay().show(lv_display_get_screen_active(nullptr));
         }},
    });
}

// =============================================================================
// Printer Name Editing
// =============================================================================

void PrinterManagerOverlay::start_name_edit() {
    if (lv_subject_get_int(&name_editing_) != 0 || !name_input_)
        return;

    // Pre-fill input with current name
    lv_textarea_set_text(name_input_, name_buf_);

    // Toggle editing state — XML bind_flag_if_eq handles visibility
    lv_subject_set_int(&name_editing_, 1);

    // Focus the input and show keyboard
    KeyboardManager::instance().show(name_input_);

    spdlog::debug("[{}] Started name edit, current: '{}'", get_name(), name_buf_);
}

void PrinterManagerOverlay::finish_name_edit() {
    if (lv_subject_get_int(&name_editing_) == 0 || !name_input_)
        return;

    // Get the new name from the textarea
    const char* new_name = lv_textarea_get_text(name_input_);
    std::string name_str = (new_name && new_name[0] != '\0') ? new_name : "My Printer";

    // Save to config
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, name_str);
    config->save();
    spdlog::info("[{}] Printer name changed to: '{}'", get_name(), name_str);
    // Sync name to Mainsail/Fluidd DB
    helix::PrinterNameSync::write_back(get_moonraker_api(), name_str);

    // Update subjects to reflect new name (local overlay + global PrinterState + image widget)
    std::strncpy(name_buf_, name_str.c_str(), sizeof(name_buf_) - 1);
    name_buf_[sizeof(name_buf_) - 1] = '\0';
    lv_subject_copy_string(&printer_manager_name_, name_buf_);
    get_printer_state().set_active_printer_name(name_str);

    // Update the printer image widget's name subject directly
    auto* type_subject = lv_xml_get_subject(nullptr, "printer_type_text");
    if (type_subject) {
        lv_subject_copy_string(type_subject, name_str.c_str());
    }

    // Toggle back to viewing mode — XML bind_flag_if_eq handles visibility
    lv_subject_set_int(&name_editing_, 0);

    // Hide keyboard
    KeyboardManager::instance().hide();
}

void PrinterManagerOverlay::cancel_name_edit() {
    if (lv_subject_get_int(&name_editing_) == 0)
        return;

    // Hide keyboard (restores screen shift)
    KeyboardManager::instance().hide();

    // Toggle back to viewing mode — XML bind_flag_if_eq handles visibility
    lv_subject_set_int(&name_editing_, 0);

    spdlog::debug("[{}] Name edit cancelled", get_name());
}

// =============================================================================
// Lifecycle
// =============================================================================

void PrinterManagerOverlay::on_activate() {
    OverlayBase::on_activate();

    // Ensure name editing is off (clean state on every open)
    if (lv_subject_get_int(&name_editing_) != 0) {
        lv_subject_set_int(&name_editing_, 0);
        KeyboardManager::instance().hide();
    }

    refresh_printer_info();
}

void PrinterManagerOverlay::on_deactivating(DeactivateReason) {
    // Save any in-progress name edit before overlay closes
    // (user expects typing a name and navigating away to save it)
    if (lv_subject_get_int(&name_editing_) != 0) {
        finish_name_edit();
        KeyboardManager::instance().hide();
    }
}

void PrinterManagerOverlay::on_ui_destroyed() {
    printer_image_obj_ = nullptr;
    name_input_ = nullptr;
}

// =============================================================================
// Refresh Printer Info
// =============================================================================

void PrinterManagerOverlay::refresh_printer_info() {
    Config* config = Config::get_instance();

    // Printer name: saved name → model/type → "My Printer"
    std::string name = helix::get_printer_display_name();
    std::strncpy(name_buf_, name.c_str(), sizeof(name_buf_) - 1);
    name_buf_[sizeof(name_buf_) - 1] = '\0';
    lv_subject_copy_string(&printer_manager_name_, name_buf_);

    // Printer model/type from config
    std::string model = config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    std::strncpy(model_buf_, model.c_str(), sizeof(model_buf_) - 1);
    model_buf_[sizeof(model_buf_) - 1] = '\0';
    lv_subject_copy_string(&printer_manager_model_, model_buf_);

    // HelixScreen version
    const char* version = helix_version();
    std::strncpy(version_buf_, version, sizeof(version_buf_) - 1);
    version_buf_[sizeof(version_buf_) - 1] = '\0';
    lv_subject_copy_string(&helix_version_, version_buf_);

    spdlog::debug("[{}] Refreshed: name='{}', model='{}', version='{}'", get_name(), name_buf_,
                  model_buf_, version_buf_);

    // Update printer image — check user-selected image first, then auto-detect
    if (printer_image_obj_) {
        lv_display_t* disp = lv_display_get_default();
        int screen_width = disp ? lv_display_get_horizontal_resolution(disp) : 800;

        auto& pim = helix::PrinterImageManager::instance();
        std::string new_path = pim.get_active_image_path(screen_width);
        if (new_path.empty()) {
            new_path = PrinterImages::get_best_printer_image(model);
        }

        // LVGL keys its decoded copy on the path alone, and an import can rewrite an
        // image in place under that same path, so the decoded copy is dropped on
        // every refresh. The scaled entries on disk need no such sweep: their names
        // carry the source's mtime and size, so the entry holding the old pixels is
        // never named again.
        if (!current_image_path_.empty()) {
            lv_image_cache_drop(current_image_path_.c_str());
        }

        current_image_path_ = new_path;
        lv_image_set_src(printer_image_obj_, current_image_path_.c_str());
        spdlog::debug("[{}] Printer image: '{}'", get_name(), current_image_path_);
    }
}
