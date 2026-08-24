// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_power.h"

#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_led_chip_factory.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace helix;

PowerPanel::PowerPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Loading devices..."));
    load_selected_devices();
}

PowerPanel::~PowerPanel() {
    // The widget tree normally outlives this panel: StaticPanelRegistry::destroy_all()
    // runs before lv_deinit(), so the LV_EVENT_DELETE hook setup() installed would
    // fire on freed memory when lv_deinit() finally tears the tree down. Uninstall it
    // while the object is still valid. A non-null panel_ means the tree is still
    // alive — forget_widget_tree() nulls it when the tree dies first.
    if (panel_ && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(panel_, on_panel_deleted, this);
    }

    deinit_subjects();
    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[PowerPanel] Destroyed");
    }
}

void PowerPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize status subject for reactive binding
    UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, status_buf_, "power_status", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized: power_status", get_name());
}

void PowerPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[PowerPanel] Subjects deinitialized");
}

void PowerPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::info("[{}] Setting up event handlers...", get_name());

    // The panel/navigation layer owns this tree; nothing tells the panel when it
    // dies. Without this hook the cached child pointers below outlive the widgets
    // and the deferred rebuilds run on freed memory.
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(panel_, on_panel_deleted, LV_EVENT_DELETE, this);

    // Register XML event callback (once)
    static bool callbacks_registered = false;
    if (!callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_power_device_toggle", on_power_device_toggle);
        callbacks_registered = true;
    }

    // Use standard overlay panel setup (wires header, back button, handles responsive padding)
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overlay_content");

    // Find widget references
    lv_obj_t* overlay_content = lv_obj_find_by_name(panel_, "overlay_content");
    if (overlay_content) {
        device_list_container_ = lv_obj_find_by_name(overlay_content, "device_list");
        empty_state_container_ = lv_obj_find_by_name(overlay_content, "empty_state");
        status_label_ = lv_obj_find_by_name(overlay_content, "status_message");
        chip_container_ = lv_obj_find_by_name(overlay_content, "power_chip_container");
    }

    if (!device_list_container_) {
        spdlog::error("[{}] device_list container not found!", get_name());
        return;
    }

    // Fetch devices from Moonraker
    fetch_devices();

    spdlog::info("[{}] Setup complete!", get_name());
}

void PowerPanel::fetch_devices() {
    if (!api_) {
        spdlog::warn("[{}] No IMoonrakerAPI available - cannot fetch devices", get_name());
        std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Not connected to printer"));
        lv_subject_copy_string(&status_subject_, status_buf_);
        return;
    }

    spdlog::debug("[{}] Fetching power devices...", get_name());
    std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Loading devices..."));
    lv_subject_copy_string(&status_subject_, status_buf_);

    auto token = lifetime_.token();
    api_->get_power_devices(
        [this, token](const std::vector<PowerDevice>& devices) {
            if (token.expired())
                return;
            // Marshal onto UI thread — API callbacks fire on a background thread.
            auto devices_copy = std::make_shared<std::vector<PowerDevice>>(devices);
            token.defer("PowerPanel::list_power_devices", [this, devices_copy]() {
                spdlog::info("[{}] Received {} power devices", get_name(), devices_copy->size());
                populate_device_list(*devices_copy);
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("PowerPanel::fetch_error", [this, msg]() {
                spdlog::error("[{}] Failed to fetch power devices: {}", get_name(), msg);
                std::snprintf(status_buf_, sizeof(status_buf_), "%s",
                              lv_tr("Failed to load devices"));
                lv_subject_copy_string(&status_subject_, status_buf_);
            });
        });
}

void PowerPanel::clear_device_list() {
    // Deferred, never synchronous: the only caller is populate_device_list(),
    // which runs from the deferred get_power_devices reply. N sync deletions in
    // one UpdateQueue batch corrupt LVGL's global event linked list (#776, #190,
    // #80 — THREADING.md invariant 3). safe_delete_deferred() detaches each row
    // immediately, so the container is empty for the rebuild that follows, and
    // hands the deletion to LVGL's own async list outside our drain.
    for (auto& row : device_rows_) {
        helix::ui::safe_delete_deferred(row.container);
    }
    device_rows_.clear();
}

void PowerPanel::populate_device_list(const std::vector<PowerDevice>& devices) {
    clear_device_list();

    // Notify about discovered devices (for selection config)
    on_devices_discovered(devices);

    bool has_devices = !devices.empty();

    // Toggle visibility: show device list OR empty state
    if (device_list_container_) {
        if (has_devices) {
            lv_obj_remove_flag(device_list_container_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(device_list_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (empty_state_container_) {
        if (has_devices) {
            lv_obj_add_flag(empty_state_container_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(empty_state_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!has_devices) {
        status_buf_[0] = '\0'; // Clear status
        lv_subject_copy_string(&status_subject_, status_buf_);
        return;
    }

    for (const auto& device : devices) {
        create_device_row(device);
    }

    // Populate chip selector for home button
    populate_device_chips();

    // Clear status message on success
    status_buf_[0] = '\0';
    lv_subject_copy_string(&status_subject_, status_buf_);
}

void PowerPanel::create_device_row(const PowerDevice& device) {
    if (!device_list_container_) {
        return;
    }

    // Convert technical name to user-friendly label
    std::string friendly_name =
        helix::get_display_name(device.device, helix::DeviceType::POWER_DEVICE);

    // Create row using XML component with prettified device_name prop
    const char* attrs[] = {"device_name", friendly_name.c_str(), nullptr, nullptr};
    lv_obj_t* row =
        static_cast<lv_obj_t*>(lv_xml_create(device_list_container_, "power_device_row", attrs));

    if (!row) {
        spdlog::error("[{}] Failed to create power_device_row for '{}'", get_name(), device.device);
        return;
    }

    // Find the toggle within the component
    lv_obj_t* toggle = lv_obj_find_by_name(row, "device_toggle");
    if (!toggle) {
        spdlog::error("[{}] device_toggle not found in row", get_name());
        helix::ui::safe_delete(row);
        return;
    }

    // Set initial state based on device status
    if (device.status == "on") {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    }

    // Check if device is locked during printing. job_holds_machine() rather
    // than the wire: the lock has to cover the pre-print window, where the
    // printer still reports standby while the toolhead homes and probes, and
    // cutting the PSU there is what the flag exists to prevent.
    bool is_locked =
        device.locked_while_printing && job_holds_machine(printer_state_.get_print_lifecycle());

    if (is_locked) {
        // Disable toggle interaction
        lv_obj_add_state(toggle, LV_STATE_DISABLED);

        // Show lock icon
        lv_obj_t* lock_icon = lv_obj_find_by_name(row, "lock_icon");
        if (lock_icon) {
            lv_obj_remove_flag(lock_icon, LV_OBJ_FLAG_HIDDEN);
        }

        // Show status text explaining why it's locked
        lv_obj_t* status_label = lv_obj_find_by_name(row, "device_status");
        if (status_label) {
            lv_label_set_text(status_label, lv_tr("Locked during print"));
            lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Store device row info (use technical name for API calls)
    DeviceRow device_row;
    device_row.container = row;
    device_row.toggle = toggle;
    device_row.device_name = device.device; // Keep technical name for API
    device_row.locked = is_locked;
    device_rows_.push_back(device_row);

    // Store index to DeviceRow in the row's user_data (avoids dangling pointer when vector resizes)
    size_t index = device_rows_.size() - 1;
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    spdlog::debug("[{}] Created row for device '{}' (status: {}, locked: {})", get_name(),
                  device.device, device.status, is_locked);
}

void PowerPanel::handle_device_toggle(const std::string& device, bool power_on) {
    if (!api_) {
        spdlog::warn("[{}] No IMoonrakerAPI available - cannot toggle device", get_name());
        return;
    }

    const char* action = power_on ? "on" : "off";
    spdlog::info("[{}] Toggling device '{}' to {}", get_name(), device, action);

    // Suppress "Printer Firmware Disconnected" modal when turning off a power device.
    // The device may have bound_services: klipper, causing an expected Klipper disconnect.
    if (!power_on) {
        EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);
    }

    auto token = lifetime_.token();
    api_->set_device_power(
        device, action,
        [device, power_on]() {
            spdlog::debug("[PowerPanel] Device '{}' set to {} successfully", device,
                          power_on ? "on" : "off");
        },
        [this, token, device](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("PowerPanel::toggle_error", [this, device, msg]() {
                spdlog::error("[{}] Failed to toggle device '{}': {}", get_name(), device, msg);
                std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Failed to toggle %s"),
                              device.c_str());
                lv_subject_copy_string(&status_subject_, status_buf_);
                fetch_devices();
            });
        });
}

void PowerPanel::on_power_device_toggle(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PowerPanel] on_power_device_toggle");

    // Get the global instance (XML callbacks can't pass instance via user_data)
    auto& self = get_global_power_panel();

    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle) {
        spdlog::warn("[PowerPanel] No target in toggle event");
    } else {
        // Navigate from toggle → right container → power_device_row root (has user_data)
        lv_obj_t* right_container = lv_obj_get_parent(toggle);
        lv_obj_t* row = right_container ? lv_obj_get_parent(right_container) : nullptr;
        if (!row) {
            spdlog::warn("[PowerPanel] Toggle has no parent row");
        } else {
            // Retrieve index from user_data
            auto index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));

            // Bounds check before accessing vector
            if (index >= self.device_rows_.size()) {
                spdlog::warn("[PowerPanel] Invalid device_row index {} (size: {})", index,
                             self.device_rows_.size());
            } else {
                auto& device_row = self.device_rows_[index];

                if (device_row.locked) {
                    spdlog::debug("[PowerPanel] Device '{}' is locked - ignoring toggle",
                                  device_row.device_name);
                } else {
                    bool is_on = lv_obj_has_state(toggle, LV_STATE_CHECKED);
                    self.handle_device_toggle(device_row.device_name, is_on);
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PowerPanel::load_selected_devices() {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[{}] No config available for loading selected devices", get_name());
        return;
    }

    auto devices = config->get<std::vector<std::string>>(config->df() + "power/selected_devices",
                                                         std::vector<std::string>{});
    if (devices.empty()) {
        // No config exists yet - will auto-select all on first discovery
        config_loaded_ = false;
        spdlog::debug("[{}] No selected_devices config found (will auto-select on discovery)",
                      get_name());
        return;
    }

    selected_devices_ = devices;
    config_loaded_ = true;
    spdlog::debug("[{}] Loaded {} selected devices from config", get_name(),
                  selected_devices_.size());
}

void PowerPanel::set_selected_devices(const std::vector<std::string>& devices) {
    selected_devices_ = devices;

    Config* config = Config::get_instance();
    if (config) {
        config->set(config->df() + "power/selected_devices", devices);
        config->save();
        spdlog::debug("[{}] Saved {} selected devices to config", get_name(), devices.size());
    }
}

void PowerPanel::on_devices_discovered(const std::vector<PowerDevice>& devices) {
    // Update discovered device list
    discovered_devices_.clear();
    for (const auto& d : devices) {
        discovered_devices_.push_back(d.device);
    }

    if (!config_loaded_) {
        // First time: auto-select all devices
        selected_devices_ = discovered_devices_;
        set_selected_devices(selected_devices_); // Persist
        spdlog::info("[{}] Auto-selected all {} discovered devices", get_name(),
                     selected_devices_.size());
    } else {
        // Prune stale devices that no longer exist
        std::set<std::string> discovered_set(discovered_devices_.begin(),
                                             discovered_devices_.end());
        std::vector<std::string> pruned;
        for (const auto& d : selected_devices_) {
            if (discovered_set.count(d) > 0) {
                pruned.push_back(d);
            }
        }
        if (pruned.size() != selected_devices_.size()) {
            spdlog::info("[{}] Pruned {} stale devices from selection", get_name(),
                         selected_devices_.size() - pruned.size());
            set_selected_devices(pruned); // Save pruned list
        }
    }
}

void PowerPanel::on_panel_deleted(lv_event_t* e) {
    auto* self = static_cast<PowerPanel*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* dying = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only the tree the panel is currently pointing at matters. A root the panel
    // has already replaced is deleted asynchronously, so its event can land after
    // a later setup() installed the successor's pointers — clearing them then
    // would blank a live tree.
    if (dying != self->panel_) {
        return;
    }

    self->forget_widget_tree();
    spdlog::debug("[PowerPanel] Widget tree deleted - cached pointers dropped");
}

void PowerPanel::forget_widget_tree() {
    // panel_ and cached_overlay_ are the same widget: get_or_create_overlay() is
    // the only site that creates the tree and the only caller of setup().
    panel_ = nullptr;
    cached_overlay_ = nullptr;
    device_list_container_ = nullptr;
    empty_state_container_ = nullptr;
    chip_container_ = nullptr;

    // The rows are children of device_list_container_ and died with it. Keeping
    // them would leave clear_device_list() calling safe_delete() on freed widgets
    // and on_power_device_toggle() indexing rows whose toggles no longer exist.
    device_rows_.clear();

    // status_label_ is deliberately left alone: setup() caches it and no code
    // reads it, so no path can dereference it. The status message reaches the XML
    // through the power_status subject, not through this pointer.
}

void PowerPanel::populate_device_chips() {
    if (!chip_container_)
        return;

    // Defer rebuild (#80) AND use safe_clean_children (#776): lifetime_.defer moves
    // the rebuild off the click handler stack (avoids deleting the clicked chip during
    // its own event), and safe_clean_children escapes UpdateQueue::process_pending()
    // so sync lv_obj_clean() can't corrupt LVGL's event linked list.
    if (!chips_rebuild_pending_) {
        chips_rebuild_pending_ = true;
        lifetime_.defer("PowerPanel::populate_device_chips", [this]() {
            chips_rebuild_pending_ = false;
            if (chip_container_)
                populate_device_chips_impl();
        });
    }
}

void PowerPanel::populate_device_chips_impl() {
    if (!chip_container_)
        return;

    helix::ui::safe_clean_children(chip_container_);

    std::set<std::string> selected_set(selected_devices_.begin(), selected_devices_.end());

    for (const auto& device : discovered_devices_) {
        bool is_selected = selected_set.count(device) > 0;
        std::string display_name = helix::get_display_name(device, helix::DeviceType::POWER_DEVICE);

        helix::ui::create_led_chip(chip_container_, device, display_name, is_selected,
                                   [this](const std::string& name) { handle_chip_clicked(name); });
    }

    spdlog::debug("[{}] Populated {} device chips ({} selected)", get_name(),
                  discovered_devices_.size(), selected_devices_.size());
}

void PowerPanel::handle_chip_clicked(const std::string& device_name) {
    // Toggle selection
    auto it = std::find(selected_devices_.begin(), selected_devices_.end(), device_name);
    if (it != selected_devices_.end()) {
        selected_devices_.erase(it);
    } else {
        selected_devices_.push_back(device_name);
    }

    // Save immediately
    set_selected_devices(selected_devices_);

    // Rebuild chips to update visual state
    populate_device_chips();
}

lv_obj_t* PowerPanel::get_or_create_overlay(lv_obj_t* parent_screen) {
    if (cached_overlay_) {
        // Re-register before returning the cached overlay: switch_to_panel_impl()
        // clears the non-persistent overlay_instances_ map on navbar switches, so
        // a cached overlay re-opened after a navbar tap would otherwise be pushed
        // unregistered (invisible to lifecycle machinery, no on_deactivate on
        // dismiss). Idempotent (keyed by widget). All push sites call this
        // immediately before push_overlay().
        NavigationManager::instance().register_overlay_instance(cached_overlay_, this);
        return cached_overlay_;
    }

    if (!parent_screen)
        return nullptr;

    if (!are_subjects_initialized()) {
        init_subjects();
    }

    auto* obj =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen, get_xml_component_name(), nullptr));
    if (!obj) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    setup(obj, parent_screen);
    NavigationManager::instance().register_overlay_instance(obj, this);
    cached_overlay_ = obj;
    return cached_overlay_;
}

// Global instance accessor
static std::unique_ptr<PowerPanel> g_power_panel;

PowerPanel& get_global_power_panel() {
    if (!g_power_panel) {
        g_power_panel = std::make_unique<PowerPanel>(get_printer_state(), get_moonraker_api());
        StaticPanelRegistry::instance().register_destroy("PowerPanel",
                                                         []() { g_power_panel.reset(); });
    }
    return *g_power_panel;
}
