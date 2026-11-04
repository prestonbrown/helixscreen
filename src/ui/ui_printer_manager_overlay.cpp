// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_manager_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_nav.h"
#include "ui_overlay_printer_image.h"
#include "ui_overlay_printer_type.h"
#include "ui_printer_list_overlay.h"
#include "ui_toast_manager.h"

#include "accel_sensor_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "core/observer_factory.h"
#include "filament_sensor_manager.h"
#include "helix_version.h"
#include "i_moonraker_api.h"
#include "prerendered_images.h"
#include "printer_capabilities_state.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "printer_manager_chips.h"
#include "printer_name_sync.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "subject_debug_registry.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"
#include "width_sensor_manager.h"
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
        UI_MANAGED_SUBJECT_INT(has_kinematics_, 0, "pm_has_kinematics", subjects_);
        UI_MANAGED_SUBJECT_INT(has_build_volume_, 0, "pm_has_build_volume", subjects_);
        UI_MANAGED_SUBJECT_INT(has_filament_system_, 0, "pm_has_filament_system", subjects_);
        UI_MANAGED_SUBJECT_INT(has_tool_changer_, 0, "pm_has_tool_changer", subjects_);
        UI_MANAGED_SUBJECT_INT(has_mcus_, 0, "pm_has_mcus", subjects_);
        for (const auto& spec : cap_chip_specs()) {
            const auto i = static_cast<size_t>(spec.chip);
            UI_MANAGED_SUBJECT_STRING_N(chip_text_[i], chip_text_bufs_[i].data(), kChipTextLen, "",
                                        spec.text_subject, subjects_);
        }
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

    // Clicking anywhere in the body while editing saves and dismisses the input.
    // LVGL DEFOCUSED events don't fire reliably on touchscreens, so we use a
    // click handler on the body instead; the body's containers (the scrolling
    // details column among them) set event_bubble so a tap on them lands here.
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
    // The ESP32 image has no camera, so timelapse is unavailable whatever the
    // printer reports. That is a compile-time fact no subject carries, so the
    // chip is hidden here rather than by its XML binding.
    if (auto* timelapse_chip =
            helix::ui::find_required(overlay_root_, "pm_chip_timelapse", get_name())) {
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
        {"pm_chip_clicked",
         [](lv_event_t* e) {
             auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
             const char* name = chip ? lv_obj_get_name(chip) : nullptr;
             const CapChipSpec* spec = name ? find_cap_chip_spec(name) : nullptr;
             if (!spec) {
                 spdlog::warn("[Printer Manager] Click on unknown chip '{}'",
                              name ? name : "<unnamed>");
                 return;
             }
             open_cap_chip(*spec);
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

    // The printer's own name only: the host or type shown in its place is not one, and
    // confirming it would save it as the name and push it to Mainsail/Fluidd.
    Config* config = Config::get_instance();
    const std::string saved =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "");
    lv_textarea_set_text(name_input_, saved.c_str());

    // Toggle editing state — XML bind_flag_if_eq handles visibility
    lv_subject_set_int(&name_editing_, 1);

    // Focus the input and show keyboard
    KeyboardManager::instance().show(name_input_);

    spdlog::debug("[{}] Started name edit, current: '{}'", get_name(), name_buf_);
}

void PrinterManagerOverlay::finish_name_edit() {
    if (lv_subject_get_int(&name_editing_) == 0 || !name_input_)
        return;

    // An empty or unchanged box leaves the name as it is.
    const char* new_name = lv_textarea_get_text(name_input_);
    const std::string name_str = new_name ? new_name : "";
    Config* config = Config::get_instance();
    if (name_str.empty() ||
        name_str == config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "")) {
        cancel_name_edit();
        return;
    }

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
    observe_chip_inputs();
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
    chip_input_observers_.clear();
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

        std::string new_path =
            helix::PrinterImageManager::instance().get_displayed_image_path(screen_width);

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

// =============================================================================
// Capability Chips
// =============================================================================

void PrinterManagerOverlay::refresh_capability_chips() {
    chip_refresh_pending_ = false;
    ++chip_refresh_count_;
    const CapabilityChipFacts facts = gather_capability_chip_facts();
    for (const auto& spec : cap_chip_specs()) {
        const auto i = static_cast<size_t>(spec.chip);
        const std::string text = capability_chip_text(spec.chip, facts);
        if (text != chip_text_bufs_[i].data()) {
            lv_subject_copy_string(&chip_text_[i], text.c_str());
        }
    }

    auto set_if_changed = [](lv_subject_t& subject, bool value) {
        if (lv_subject_get_int(&subject) != (value ? 1 : 0)) {
            lv_subject_set_int(&subject, value ? 1 : 0);
        }
    };
    auto has_text = [&facts](CapChip chip) { return !capability_chip_text(chip, facts).empty(); };
    set_if_changed(has_kinematics_, has_text(CapChip::Kinematics));
    set_if_changed(has_build_volume_, has_text(CapChip::BuildVolume));
    set_if_changed(has_filament_system_, has_text(CapChip::FilamentSystem));
    set_if_changed(has_tool_changer_, has_text(CapChip::ToolChanger));
    set_if_changed(has_mcus_, has_text(CapChip::Mcus));
}

void PrinterManagerOverlay::request_chip_refresh() {
    if (chip_refresh_pending_) {
        return;
    }
    chip_refresh_pending_ = true;
    lifetime_.defer("PrinterManagerOverlay::refresh_capability_chips",
                    [this]() { refresh_capability_chips(); });
}

void PrinterManagerOverlay::observe_chip_inputs() {
    chip_input_observers_.clear();
    // A refresh deferred before the last deactivation was dropped with it.
    chip_refresh_pending_ = false;
    // Attaching notifies at once, so every observer below asks for the same
    // single refresh this activation needs.
    auto watch = [this](lv_subject_t* subject, const SubjectLifetime& lifetime) {
        if (!subject) {
            return;
        }
        chip_input_observers_.push_back(helix::ui::observe<int>(
            subject, this, [](PrinterManagerOverlay* self, int) { self->request_chip_refresh(); },
            lifetime, helix::ui::Dispatch::Immediate));
    };

    PrinterState& ps = get_printer_state();
    auto& caps = ps.capabilities_state();
    const SubjectLifetime caps_lifetime = caps.get_subjects_lifetime();
    watch(caps.subject(Capability::HardwareGeneration), caps_lifetime);
    watch(caps.subject(Capability::PowerDeviceCount), caps_lifetime);
    watch(caps.subject(Capability::WebcamCount), caps_lifetime);
    if (auto* api = get_moonraker_api()) {
        watch(api->get_build_volume_version_subject(), subject_never_freed());
    }

    auto& accel = helix::sensors::AccelSensorManager::instance();
    watch(accel.count_subject(), accel.subjects_lifetime());
    auto& filament = helix::FilamentSensorManager::instance();
    watch(filament.count_subject(), filament.subjects_lifetime());
    auto& width = helix::sensors::WidthSensorManager::instance();
    watch(width.count_subject(), width.subjects_lifetime());

    auto& ams = AmsState::instance();
    watch(ams.get_backend_count_subject(), ams.get_subjects_lifetime());
    watch(ams.get_slot_count_subject(), ams.get_subjects_lifetime());
    watch(ams.get_ams_type_subject(), ams.get_subjects_lifetime());
    auto& tools = ToolState::instance();
    watch(tools.get_tools_version_subject(), tools.get_subjects_lifetime());
}
