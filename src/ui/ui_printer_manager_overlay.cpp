// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_manager_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
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
#include "ui_settings_display_sound.h"
#include "ui_settings_led.h"
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
#include "ui/ui_lazy_panel_helper.h"
#include "wizard_config_paths.h"

#include <lvgl/src/misc/cache/lv_cache.h>
#include <spdlog/spdlog.h>

#include <cstring>

using namespace helix;

// =============================================================================
// Global Instance
// =============================================================================

static std::unique_ptr<PrinterManagerOverlay> g_printer_manager_overlay;

PrinterManagerOverlay& get_printer_manager_overlay() {
    if (!g_printer_manager_overlay) {
        g_printer_manager_overlay = std::make_unique<PrinterManagerOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "PrinterManagerOverlay", []() { g_printer_manager_overlay.reset(); });
    }
    return *g_printer_manager_overlay;
}

void destroy_printer_manager_overlay() {
    g_printer_manager_overlay.reset();
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

PrinterManagerOverlay::PrinterManagerOverlay() {
    std::memset(name_buf_, 0, sizeof(name_buf_));
    std::memset(model_buf_, 0, sizeof(model_buf_));
    std::memset(version_buf_, 0, sizeof(version_buf_));
}

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
    if (!create_overlay_from_xml(parent, "printer_manager_overlay")) {
        return nullptr;
    }

    // Find the printer image widget for programmatic image source setting
    printer_image_obj_ = lv_obj_find_by_name(overlay_root_, "pm_printer_image");

    // Find name editing widgets (visibility driven by pm_name_editing subject via XML bindings)
    name_input_ = lv_obj_find_by_name(overlay_root_, "pm_printer_name_input");

    // Register READY/CANCEL on textarea for name edit lifecycle
    // (acceptable exception to declarative rule — textarea lifecycle event, like DELETE cleanup)
    if (name_input_) {
        lv_obj_add_event_cb(name_input_, pm_name_input_ready_cb, LV_EVENT_READY, nullptr);
        lv_obj_add_event_cb(name_input_, pm_name_input_cancel_cb, LV_EVENT_CANCEL, nullptr);
    }

    // Clicking the overlay content area while editing saves and dismisses the input.
    // LVGL DEFOCUSED events don't fire reliably on touchscreens, so we use a
    // click handler on the scrollable content instead.
    auto* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (content) {
        lv_obj_add_event_cb(
            content,
            [](lv_event_t* e) {
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
    if (auto* timelapse_chip = lv_obj_find_by_name(overlay_root_, "pm_chip_timelapse")) {
        // DECLARATIVE_OK: compile-time capability (#if), no runtime subject exists
        lv_obj_add_flag(timelapse_chip, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    return overlay_root_;
}

// =============================================================================
// Callbacks
// =============================================================================

void PrinterManagerOverlay::register_callbacks() {
    register_xml_callbacks({
        // Chip navigation callbacks
        {"pm_chip_bed_mesh_clicked", on_chip_bed_mesh_clicked},
        {"pm_chip_leds_clicked", on_chip_leds_clicked},
        {"pm_chip_adxl_clicked", on_chip_adxl_clicked},
        {"pm_chip_retraction_clicked", on_chip_retraction_clicked},
        {"pm_chip_spoolman_clicked", on_chip_spoolman_clicked},
        {"pm_chip_timelapse_clicked", on_chip_timelapse_clicked},
        {"pm_chip_screws_tilt_clicked", on_chip_screws_tilt_clicked},
        {"pm_chip_ams_clicked", on_chip_ams_clicked},
        {"pm_chip_fans_clicked", on_chip_fans_clicked},
        {"pm_chip_power_clicked", on_chip_power_clicked},
        {"pm_chip_speaker_clicked", on_chip_speaker_clicked},
        // Printer name click callback (inline rename)
        {"pm_printer_name_clicked", pm_printer_name_clicked_cb},
        // Image click callback (opens printer image picker)
        {"on_change_printer_image_clicked", change_printer_image_clicked_cb},
        // Model click callback (opens printer model picker)
        {"on_change_printer_model_clicked", change_printer_model_clicked_cb},
        // Manage printers callback (opens printer list)
        {"pm_manage_printers_clicked", pm_manage_printers_clicked_cb},
    });
}

// =============================================================================
// Chip Navigation Callbacks
// =============================================================================

void PrinterManagerOverlay::on_chip_bed_mesh_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Bed Mesh chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<BedMeshPanel>(
        get_global_bed_mesh_panel, pm.bed_mesh_panel_, lv_display_get_screen_active(nullptr),
        "Bed Mesh", "Printer Manager", true);
}

void PrinterManagerOverlay::on_chip_leds_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] LEDs chip clicked");
    auto& overlay = helix::settings::get_led_settings_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}

void PrinterManagerOverlay::on_chip_adxl_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] ADXL chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<InputShaperPanel>(
        get_global_input_shaper_panel, pm.input_shaper_panel_,
        lv_display_get_screen_active(nullptr), "Input Shaper", "Printer Manager", true);
}

void PrinterManagerOverlay::on_chip_retraction_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Retraction chip clicked");
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<RetractionSettingsOverlay>(
        get_global_retraction_settings, pm.retraction_panel_, lv_display_get_screen_active(nullptr),
        "Retraction Settings", "Printer Manager");
}

void PrinterManagerOverlay::on_chip_spoolman_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Spoolman chip clicked");
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<SpoolmanPanel>(
        get_global_spoolman_panel, pm.spoolman_panel_, lv_display_get_screen_active(nullptr),
        "Spoolman", "Printer Manager");
}

void PrinterManagerOverlay::on_chip_timelapse_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Timelapse chip clicked");
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<TimelapseSettingsOverlay>(
        get_global_timelapse_settings, pm.timelapse_panel_, lv_display_get_screen_active(nullptr),
        "Timelapse Settings", "Printer Manager");
}

void PrinterManagerOverlay::on_chip_screws_tilt_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Screws Tilt chip clicked");
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_screws_tilt_panel().set_client(get_moonraker_client(), get_moonraker_api());
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<ScrewsTiltPanel>(
        get_global_screws_tilt_panel, pm.screws_tilt_panel_, lv_display_get_screen_active(nullptr),
        "Bed Screws", "Printer Manager");
}

void PrinterManagerOverlay::on_chip_ams_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] AMS chip clicked");

    auto& ams_panel = get_global_ams_panel();
    if (!ams_panel.are_subjects_initialized()) {
        ams_panel.init_subjects();
    }
    lv_obj_t* panel_obj = ams_panel.get_panel();
    if (panel_obj) {
        // Re-register before push (see HomePanel::handle_ams_clicked):
        // get_global_ams_panel() only registers in its lazy-creation block, and
        // navbar switches clear overlay_instances_, so a cached panel loses its
        // registration and on_deactivate() never fires on dismiss — leaving the
        // filament-path animation drawing into a torn-down panel. Idempotent.
        NavigationManager::instance().register_overlay_instance(panel_obj, &ams_panel);
        NavigationManager::instance().push_overlay(panel_obj);
    }
}

void PrinterManagerOverlay::on_chip_fans_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Fans chip clicked");

    auto& pm = get_printer_manager_overlay();
    if (!pm.fan_control_panel_) {
        auto& overlay = get_fan_control_overlay();
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(get_moonraker_api());

        lv_obj_t* screen = lv_display_get_screen_active(nullptr);
        pm.fan_control_panel_ = overlay.create(screen);
        if (!pm.fan_control_panel_) {
            spdlog::warn("[Printer Manager] Failed to create fan control overlay");
            return;
        }
        NavigationManager::instance().register_overlay_instance(pm.fan_control_panel_, &overlay);
    }

    if (pm.fan_control_panel_) {
        get_fan_control_overlay().set_api(get_moonraker_api());
        NavigationManager::instance().push_overlay(pm.fan_control_panel_);
    }
}

void PrinterManagerOverlay::on_chip_power_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Power Devices chip clicked");
    auto& panel = get_global_power_panel();
    lv_obj_t* overlay = panel.get_or_create_overlay(lv_display_get_screen_active(nullptr));
    if (overlay) {
        NavigationManager::instance().push_overlay(overlay);
    }
}

void PrinterManagerOverlay::on_chip_speaker_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Printer Manager] Speaker chip clicked");
    auto& overlay = helix::settings::get_display_sound_settings_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}

// =============================================================================
// Manage Printers
// =============================================================================

void PrinterManagerOverlay::pm_manage_printers_clicked_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] manage_printers_clicked");
    get_printer_manager_overlay().handle_manage_printers_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterManagerOverlay::handle_manage_printers_clicked() {
    spdlog::info("[{}] Manage Printers clicked — opening printer list", get_name());
    auto& overlay = helix::ui::get_printer_list_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}

// =============================================================================
// Printer Image Click
// =============================================================================

void PrinterManagerOverlay::change_printer_image_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] change_printer_image_clicked_cb");
    (void)e;
    get_printer_manager_overlay().handle_change_printer_image_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterManagerOverlay::handle_change_printer_image_clicked() {
    spdlog::debug("[{}] Printer image clicked — opening image picker", get_name());
    auto& overlay = helix::settings::get_printer_image_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}

// =============================================================================
// Printer Model Click
// =============================================================================

void PrinterManagerOverlay::change_printer_model_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] change_printer_model_clicked_cb");
    (void)e;
    get_printer_manager_overlay().handle_change_printer_model_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterManagerOverlay::handle_change_printer_model_clicked() {
    spdlog::debug("[{}] Printer model clicked — opening model picker", get_name());
    auto& overlay = helix::settings::get_printer_type_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}

// =============================================================================
// Printer Name Editing
// =============================================================================

void PrinterManagerOverlay::pm_printer_name_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] pm_printer_name_clicked_cb");
    (void)e;
    get_printer_manager_overlay().start_name_edit();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterManagerOverlay::pm_name_input_ready_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] pm_name_input_ready_cb");
    (void)e;
    get_printer_manager_overlay().finish_name_edit();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterManagerOverlay::pm_name_input_cancel_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] pm_name_input_cancel_cb");
    (void)e;
    get_printer_manager_overlay().cancel_name_edit();
    LVGL_SAFE_EVENT_CB_END();
}

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
    if (config) {
        config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, name_str);
        config->save();
        spdlog::info("[{}] Printer name changed to: '{}'", get_name(), name_str);
        // Sync name to Mainsail/Fluidd DB
        helix::PrinterNameSync::write_back(get_moonraker_api(), name_str);
    }

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

void PrinterManagerOverlay::on_deactivate() {
    // Save any in-progress name edit before overlay closes
    // (user expects typing a name and navigating away to save it)
    if (lv_subject_get_int(&name_editing_) != 0) {
        finish_name_edit();
        KeyboardManager::instance().hide();
    }

    OverlayBase::on_deactivate();
}

// =============================================================================
// Refresh Printer Info
// =============================================================================

void PrinterManagerOverlay::refresh_printer_info() {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[{}] Config not available", get_name());
        return;
    }

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

        // Invalidate caches so the new image displays immediately
        if (!current_image_path_.empty()) {
            lv_image_cache_drop(current_image_path_.c_str());
            helix::invalidate_printer_image_cache(current_image_path_);
        }

        current_image_path_ = new_path;
        lv_image_set_src(printer_image_obj_, current_image_path_.c_str());
        spdlog::debug("[{}] Printer image: '{}'", get_name(), current_image_path_);
    }
}
