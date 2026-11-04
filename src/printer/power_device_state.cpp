// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file power_device_state.cpp
 * @brief Power device state tracking from Moonraker WebSocket events
 *
 * Maintains per-device LVGL subjects with values:
 *   0 = off, 1 = on, 2 = locked (device is on but locked_while_printing during a print)
 */

#include "power_device_state.h"

#include "ui_emergency_stop.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

PowerDeviceState& PowerDeviceState::instance() {
    static PowerDeviceState s;
    return s;
}

int PowerDeviceState::status_string_to_int(const std::string& status) {
    if (status == "on") {
        return 1;
    }
    return 0; // "off", "error", or unknown -> off
}

void PowerDeviceState::set_devices(const std::vector<PowerDevice>& devices) {
    // Rediscovery runs after every Klipper restart. A device still present keeps
    // its subject and lifetime: tiles subscribe once at attach and the home panel
    // does not rebuild when its widget list is unchanged, so a fresh subject
    // would leave them watching a dead one.
    if (subjects_initialized_) {
        lifetime_.invalidate();
        std::vector<std::string> gone;
        for (const auto& [name, info] : devices_) {
            const bool still_present =
                std::any_of(devices.begin(), devices.end(),
                            [&name = name](const PowerDevice& d) { return d.device == name; });
            if (!still_present) {
                gone.push_back(name);
            }
        }
        for (const auto& name : gone) {
            auto it = devices_.find(name);
            // Signal death then expire the lifetime token before the deinit (#816)
            if (it->second.lifetime)
                *it->second.lifetime = false;
            it->second.lifetime.reset();
            if (it->second.status_subject) {
                lv_subject_deinit(it->second.status_subject.get());
            }
            devices_.erase(it);
        }
    }

    for (const auto& dev : devices) {
        auto existing = devices_.find(dev.device);
        if (existing != devices_.end()) {
            // reevaluate_lock_states() below publishes the new value.
            DeviceInfo& info = existing->second;
            info.type = dev.type;
            info.locked_while_printing = dev.locked_while_printing;
            info.raw_status = status_string_to_int(dev.status);
            continue;
        }

        DeviceInfo info;
        info.name = dev.device;
        info.type = dev.type;
        info.locked_while_printing = dev.locked_while_printing;
        info.raw_status = status_string_to_int(dev.status);

        info.status_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.status_subject.get(), info.raw_status);
        info.lifetime = std::make_shared<bool>(true);

        spdlog::trace(
            "[PowerDeviceState] Created subject for '{}': status={} locked_while_printing={}",
            dev.device, dev.status, dev.locked_while_printing);

        devices_.emplace(dev.device, std::move(info));
    }

    // Register with StaticSubjectRegistry on first init
    if (!subjects_initialized_) {
        subjects_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit(
            "PowerDeviceState", []() { PowerDeviceState::instance().deinit_subjects(); });

        // Observe print state to reevaluate lock states.
        // Guard: PrinterState subjects may not be initialized (e.g., during unit tests
        // where a prior test called deinit_subjects on the global singleton).
        auto& ps = get_printer_state();
        if (ps.are_subjects_initialized()) {
            // print_lifecycle, not print_state_enum. The lock now covers the
            // preparing window, and the raw enum does not move on the
            // Idle -> Preparing edge - it holds standby, or the PREVIOUS job's
            // terminal state, for the whole of a host-side pre-start block. On
            // the enum this observer simply would not fire, and the widened
            // predicate below would be dead code.
            auto* print_subj = ps.print_state().get_print_lifecycle_subject();
            if (print_subj) {
                print_state_observer_ = ui::observe<int>(
                    print_subj, this,
                    [](PowerDeviceState* self, int /*lifecycle*/) {
                        self->reevaluate_lock_states();
                    },
                    ps.get_subjects_lifetime());
            }
        }
    }

    // Publish each device's effective status: its raw state, or locked while a job holds the
    // machine.
    reevaluate_lock_states();

    spdlog::info("[PowerDeviceState] Initialized {} power devices", devices.size());
}

lv_subject_t* PowerDeviceState::get_status_subject(const std::string& device, SubjectLifetime& lt) {
    auto it = devices_.find(device);
    if (it != devices_.end() && it->second.status_subject) {
        lt = it->second.lifetime;
        return it->second.status_subject.get();
    }
    lt.reset();
    return nullptr;
}

bool PowerDeviceState::is_locked_while_printing(const std::string& device) const {
    auto it = devices_.find(device);
    if (it != devices_.end()) {
        return it->second.locked_while_printing;
    }
    return false;
}

std::vector<std::string> PowerDeviceState::device_names() const {
    std::vector<std::string> names;
    names.reserve(devices_.size());
    for (const auto& [name, info] : devices_) {
        names.push_back(name);
    }
    return names;
}

void PowerDeviceState::subscribe(IMoonrakerAPI& api) {
    api.register_method_callback("notify_power_changed", "power_device_state",
                                 [this](const nlohmann::json& msg) { on_power_changed(msg); });
    spdlog::debug("[PowerDeviceState] Subscribed to notify_power_changed");
}

void PowerDeviceState::unsubscribe(IMoonrakerAPI& api) {
    api.unregister_method_callback("notify_power_changed", "power_device_state");
    print_state_observer_.reset();
    deinit_subjects();
    spdlog::debug("[PowerDeviceState] Unsubscribed and cleaned up");
}

void PowerDeviceState::update_device_status(const std::string& device, const std::string& status) {
    int new_raw = status_string_to_int(status);
    spdlog::debug("[PowerDeviceState] update_device_status: device='{}' status='{}'", device,
                  status);
    auto tok = lifetime_.token();
    tok.defer("PowerDeviceState::update_device_status", [this, device, new_raw]() {
        auto it = devices_.find(device);
        if (it == devices_.end())
            return;
        it->second.raw_status = new_raw;
        publish_effective_status(it->second);
    });
}

void PowerDeviceState::on_power_changed(const nlohmann::json& msg) {
    // Moonraker sends: {"method": "notify_power_changed", "params": [{"device": "...", "status":
    // "on|off", ...}]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    auto tok = lifetime_.token();

    for (const auto& param : msg["params"]) {
        if (!param.is_object() || !param.contains("device") || !param.contains("status")) {
            continue;
        }
        if (!param["device"].is_string() || !param["status"].is_string()) {
            continue;
        }

        std::string device_name = param["device"].get<std::string>();
        std::string new_status = param["status"].get<std::string>();
        int new_raw = status_string_to_int(new_status);

        spdlog::debug("[PowerDeviceState] notify_power_changed: device='{}' status='{}'",
                      device_name, new_status);

        // Suppress "Printer Firmware Disconnected" dialog when ANY power device turns off.
        // The device may have bound_services: klipper, causing an expected Klipper disconnect.
        // Must set BEFORE queue_update() — notify_klippy_disconnected arrives on the same
        // WebSocket thread and checks suppression synchronously.
        if (new_raw == 0) {
            EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::NORMAL);
        }

        tok.defer("PowerDeviceState::on_power_changed", [this, device_name, new_raw]() {
            auto it = devices_.find(device_name);
            if (it == devices_.end()) {
                spdlog::trace("[PowerDeviceState] Ignoring update for unknown device '{}'",
                              device_name);
                return;
            }

            it->second.raw_status = new_raw;
            publish_effective_status(it->second);
        });
    }
}

void PowerDeviceState::reevaluate_lock_states() {
    for (auto& [name, info] : devices_) {
        publish_effective_status(info);
    }
}

void PowerDeviceState::publish_effective_status(DeviceInfo& info) {
    if (!info.status_subject) {
        return;
    }
    auto& ps = get_printer_state();
    // The LIFECYCLE subject, matching the PrintState cast below. These enums do
    // not share numbering past index 0 - PrintJobState COMPLETE is 3, PrintState
    // Paused is 3 - so reading one and interpreting it as the other compiles,
    // runs, and answers a different question (a finished print would read as
    // "paused" and hold the lock on).

    // A device flagged locked_while_printing is the PSU or a bound relay. During
    // a host-side pre-start block the printer is homing and probing under power,
    // so cutting it there is as destructive as cutting it mid-layer - and the
    // wire cannot say so.
    const bool is_printing =
        ps.are_subjects_initialized() && job_holds_machine(ps.print_state().get_print_lifecycle());

    int effective = info.raw_status;
    if (info.locked_while_printing && info.raw_status == 1 && is_printing) {
        effective = 2; // locked
    }

    if (lv_subject_get_int(info.status_subject.get()) != effective) {
        lv_subject_set_int(info.status_subject.get(), effective);
        spdlog::debug("[PowerDeviceState] '{}' effective status {}", info.name, effective);
    }
}

void PowerDeviceState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PowerDeviceState] Deinitializing subjects");

    lifetime_.invalidate();

    // The observer watches PrinterState's print_lifecycle_ subject, which
    // outlives this teardown. It carries PrinterState's subjects lifetime, so
    // reset() detaches it from a live subject and skips the removal when
    // PrinterState has already freed that subject; either way its context is
    // freed. Leaving it attached would keep calling back into torn-down state.
    print_state_observer_.reset();

    // Signal death then expire lifetime tokens (#816)
    for (auto& [name, info] : devices_) {
        if (info.lifetime)
            *info.lifetime = false;
        info.lifetime.reset();
    }

    // Now safe to deinit subjects
    for (auto& [name, info] : devices_) {
        if (info.status_subject) {
            lv_subject_deinit(info.status_subject.get());
        }
    }
    devices_.clear();

    subjects_initialized_ = false;
}

} // namespace helix
