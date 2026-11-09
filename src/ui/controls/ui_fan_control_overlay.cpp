// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fan_control_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_drag_surface.h"
#include "ui_error_reporting.h"
#include "ui_fan_arc_resize.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_settings_fans.h"
#include "ui_utils.h"

#include "app_constants.h"
#include "app_globals.h"
#include "chamber_air_modes.h"
#include "config.h"
#include "core/observer_factory.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "macro_executor.h"
#include "ui/fan_spin_animation.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

using namespace helix;

// ============================================================================
// LONG-PRESS RENAME HELPERS
// ============================================================================

namespace {

struct FanRenameData {
    std::string object_name;
    std::string display_name;
    lv_point_t press_start_point{};
    bool press_point_valid = false;
};

/// True when the active press landed on (or bubbled up from) a drag surface
/// (is_drag_surface_hit()): the fan speed dial. Its events bubble to the card
/// root where the rename handler lives, so a hold spent dragging the dial must
/// never read as a rename. Same test as HomePanel::should_suppress_edit_mode.
bool press_consumed_by_value_widget(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev))
        return true;

    return helix::ui::is_drag_surface_hit(lv_event_get_target_obj(e),
                                          lv_event_get_current_target_obj(e));
}

/// True when the finger drifted past the deliberate-hold threshold since the
/// press began. LVGL fires LONG_PRESSED on hold duration alone, so a press that
/// slid across the card is a drag, not a rename gesture. Mirrors
/// HomePanel::finger_drifted_since_press.
bool finger_drifted_since_press(const FanRenameData* data) {
    if (!data->press_point_valid)
        return false;
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return false;
    lv_point_t now;
    lv_indev_get_point(indev, &now);
    const int dx = now.x - data->press_start_point.x;
    const int dy = now.y - data->press_start_point.y;
    const int limit = lv_dpx(app_constants::input::kEditModeMoveCancelDpx);
    return (dx * dx + dy * dy) > (limit * limit);
}

void on_fan_pressed(lv_event_t* e) {
    auto* data = static_cast<FanRenameData*>(lv_event_get_user_data(e));
    if (!data)
        return;
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &data->press_start_point);
        data->press_point_valid = true;
    } else {
        data->press_point_valid = false;
    }
}

void on_fan_long_pressed(lv_event_t* e) {
    auto* data = static_cast<FanRenameData*>(lv_event_get_user_data(e));
    if (!data)
        return;
    // Renaming requires a deliberate, stationary hold. A hold that landed on the
    // speed dial, or one that drifted from its press point, is the user adjusting
    // fan speed — surfacing the rename modal there is the accidental trigger we
    // are guarding against.
    if (press_consumed_by_value_widget(e) || finger_drifted_since_press(data)) {
        spdlog::trace("[FanControlOverlay] long-press on '{}' ignored (dial drag / drift)",
                      data->object_name);
        return;
    }
    spdlog::debug("[FanControlOverlay] Long press on '{}'", data->object_name);
    helix::ui::end_press(e);
    helix::settings::get_fan_settings_overlay().handle_fan_rename(data->object_name,
                                                                  data->display_name);
}

void on_fan_rename_data_delete(lv_event_t* e) {
    delete static_cast<FanRenameData*>(lv_event_get_user_data(e));
}

void attach_long_press_rename(lv_obj_t* widget, const std::string& object_name,
                              const std::string& display_name) {
    auto* data = new FanRenameData{object_name, display_name};
    lv_obj_add_flag(widget, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(widget, on_fan_pressed, LV_EVENT_PRESSED, data);
    lv_obj_add_event_cb(widget, on_fan_long_pressed, LV_EVENT_LONG_PRESSED, data);
    lv_obj_add_event_cb(widget, on_fan_rename_data_delete, LV_EVENT_DELETE, data);
}

/// A chamber air mode button: user_data names the mode (chamber_air::mode_id).
void on_chamber_air_mode_clicked(lv_event_t* e) {
    const char* id = static_cast<const char*>(lv_event_get_user_data(e));
    const auto mode = helix::chamber_air::parse_mode_id(id ? id : "");
    if (!mode) {
        spdlog::warn("[FanControlOverlay] Unknown chamber air mode '{}'", id ? id : "");
        return;
    }
    const std::string gcode = helix::chamber_air::mode_gcode(
        get_printer_state().get_discovery(), *mode, get_printer_state().chamber_air().targets());
    if (gcode.empty()) {
        spdlog::warn("[FanControlOverlay] This printer does not offer chamber air mode '{}'", id);
        return;
    }
    helix::MachineGcode cmd;
    cmd.gcode = gcode;
    cmd.log_name = gcode;
    cmd.display_name = lv_tr("Chamber air");
    helix::execute_machine_gcode(get_moonraker_api(), cmd, "[FanControlOverlay]");
}

} // namespace

namespace helix {
lv_obj_t* open_fan_control_overlay(lv_obj_t* parent_screen) {
    auto& overlay = get_fan_control_overlay();
    overlay.set_api(get_moonraker_api());
    return overlay.show(parent_screen) ? overlay.get_root() : nullptr;
}
} // namespace helix

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

FanControlOverlay::FanControlOverlay(PrinterState& printer_state) : printer_state_(printer_state) {}

FanControlOverlay::~FanControlOverlay() {
    // LVGL may already be destroyed during static destruction
    if (!lv_is_initialized()) {
        spdlog::trace("[FanControlOverlay] Destroyed (LVGL already deinit)");
        return;
    }

    // Clear vectors to destroy FanDial instances before LVGL objects are deleted
    animated_fan_dials_.clear();
    auto_fan_cards_.clear();

    spdlog::trace("[FanControlOverlay] Destroyed");
}

// ============================================================================
// OVERLAYBASE IMPLEMENTATION
// ============================================================================

lv_obj_t* FanControlOverlay::create(lv_obj_t* parent) {
    register_xml_callbacks({{"on_chamber_air_mode_clicked", on_chamber_air_mode_clicked}});
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    lv_obj_add_event_cb(overlay_root_, on_root_deleted, LV_EVENT_DELETE, nullptr);

    fans_container_ = helix::ui::find_required(overlay_root_, "fans_container", get_name());

    // Populate fans from current PrinterState
    populate_fans();

    return overlay_root_;
}

void FanControlOverlay::on_activate() {
    OverlayBase::on_activate();

    // Subscribe to fans_version subject for structural changes (fan discovery)
    // Using observer factory for type-safe lambda observer
    using helix::ui::observe;
    if (auto* fans_ver = printer_state_.fan_state().get_fans_version_subject()) {
        fans_observer_ = observe<int>(
            fans_ver, this,
            [](FanControlOverlay* self, int /* version */) {
                if (!self->is_visible())
                    return;
                // Defer rebuild (#80) AND use safe_clean_children (#776): lifetime_.defer
                // moves the rebuild off the observer callback's stack, and
                // safe_clean_children escapes UpdateQueue::process_pending() so the
                // sync clean can't corrupt LVGL's event linked list.
                self->schedule_fans_rebuild();
            },
            printer_state_.get_subjects_lifetime());
    }

    // A chamber-air accessory fitted or removed adds or drops the fans it owns.
    chamber_air_observer_ = observe<int>(
        printer_state_.chamber_air().available_subject(), this,
        [](FanControlOverlay* self, int /* available */) {
            if (self->is_visible()) {
                self->schedule_fans_rebuild();
            }
        },
        printer_state_.get_subjects_lifetime());

    // Observe animation setting changes to refresh spin animations on all fan cards
    anim_settings_observer_ = observe<int>(
        DisplaySettingsManager::instance().subject_animations_enabled(), this,
        [](FanControlOverlay* self, int /* enabled */) {
            if (self->is_visible()) {
                // Refresh controllable fan dial animations
                for (auto& afd : self->animated_fan_dials_) {
                    if (afd.dial) {
                        afd.dial->refresh_animation();
                    }
                }
                // Refresh auto fan card animations
                self->refresh_all_auto_fan_animations();
            }
        },
        DisplaySettingsManager::instance().get_subjects_lifetime());

    // Subscribe to per-fan speed subjects for reactive updates
    subscribe_to_fan_speeds();

    // Refresh fan speeds from current state
    update_fan_speeds();

    spdlog::trace("[{}] Activated", get_name());
}

void FanControlOverlay::schedule_fans_rebuild() {
    if (fans_rebuild_pending_) {
        return;
    }
    fans_rebuild_pending_ = true;
    // Defer rebuild (#80) AND use safe_clean_children (#776): lifetime_.defer
    // moves the rebuild off the observer callback's stack, and
    // safe_clean_children escapes UpdateQueue::process_pending() so the
    // sync clean can't corrupt LVGL's event linked list.
    lifetime_.defer("FanControlOverlay::rebuild_fans", [this]() {
        fans_rebuild_pending_ = false;
        if (!is_visible() || !fans_container_)
            return;
        unsubscribe_from_fan_speeds();
        populate_fans();
        subscribe_to_fan_speeds();
    });
}

void FanControlOverlay::on_deactivating(DeactivateReason) {
    fans_observer_.reset();
    chamber_air_observer_.reset();
    anim_settings_observer_.reset();
    unsubscribe_from_fan_speeds();
    // Deactivation drops the confirmation's callbacks (its owner token), so none
    // of them would clear this.
    owned_confirm_open_ = false;

    spdlog::debug("[{}] Deactivated", get_name());
}

void FanControlOverlay::cleanup() {
    spdlog::debug("[{}] Cleanup", get_name());

    // Resetting the observers drops their queued callbacks, and the queued fan
    // rebuild checks the overlay is visible, so nothing queued reaches the
    // dials released here.
    release_fan_widgets();
    OverlayBase::cleanup();
}

void FanControlOverlay::release_fan_widgets() {
    fans_observer_.reset();
    chamber_air_observer_.reset();
    anim_settings_observer_.reset();
    unsubscribe_from_fan_speeds();
    // Stop spin animations before clearing cards
    for (auto& card : auto_fan_cards_) {
        helix::ui::fan_spin_stop(card.fan_icon);
    }
    animated_fan_dials_.clear();
    auto_fan_cards_.clear();
}

void FanControlOverlay::on_root_deleted(lv_event_t* e) {
    // Resolved through the global, not user_data: a printer switch destroys the
    // overlay before freeing its tree, and the re-created overlay can be opened
    // on a new root before the old one is freed.
    auto* overlay = helix::lazy_global_if_exists<FanControlOverlay>();
    if (!overlay || !overlay->overlay_root_.names_dying(lv_event_get_target_obj(e))) {
        return;
    }
    // LVGL sends LV_EVENT_DELETE before deleting the children, so the dials'
    // widgets are still alive here.
    auto& self = *overlay;
    self.release_fan_widgets();
    self.fans_container_ = nullptr;
    self.overlay_root_ = nullptr;
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void FanControlOverlay::populate_fans() {
    if (!fans_container_) {
        spdlog::warn("[{}] Cannot populate fans - container not found", get_name());
        return;
    }

    // Clear tracking vectors BEFORE lv_obj_clean — FanDial destructors call
    // stop_spin(fan_icon_) which dereferences the icon widget pointer. If we
    // destroy LVGL widgets first, that pointer is freed and we crash.
    animated_fan_dials_.clear();
    for (auto& card : auto_fan_cards_) {
        helix::ui::fan_spin_stop(card.fan_icon);
    }
    auto_fan_cards_.clear();

    // Now safe to destroy the LVGL widget tree. safe_clean_children reparents each
    // child to lv_layer_top() + schedules lv_obj_delete_async — escaping the
    // UpdateQueue batch that this observer-deferred rebuild runs inside (#776).
    helix::ui::safe_clean_children(fans_container_);

    const auto& fans = printer_state_.fan_state().get_fans();

    // Fans chamber air control owns. A printer that republishes one as a Klipper
    // fan shows that fan's dial; the rest get a dial of their own. Either kind
    // exists only while the accessory is fitted: without it the firmware drops
    // every write.
    const auto owned = helix::chamber_air::owned_fans(printer_state_.get_discovery());
    const bool chamber_air_available = printer_state_.chamber_air().available();
    auto owned_role_of =
        [&owned](const std::string& object_name) -> std::optional<helix::chamber_air::FanRole> {
        for (const auto& f : owned) {
            if (f.equivalent_object && object_name == f.equivalent_object) {
                return f.role;
            }
        }
        return std::nullopt;
    };

    // First pass: create controllable fans (FanDial widgets with animation)
    for (const auto& fan : fans) {
        if (fan.is_controllable) {
            const auto role = owned_role_of(fan.object_name);
            if (role && !chamber_air_available) {
                continue;
            }
            AnimatedFanDial afd;
            afd.object_name = fan.object_name;
            afd.owned_role = role;
            afd.dial = std::make_unique<FanDial>(fans_container_, fan.display_name, fan.object_name,
                                                 fan.speed_percent);

            // Set callback for user-initiated speed changes (dial interaction)
            if (role) {
                afd.dial->set_hold_until_release(true);
                afd.dial->set_on_speed_changed(
                    [this, r = *role](const std::string& fan_id, int speed_percent) {
                        handle_owned_fan_change(fan_id, r, false, speed_percent);
                    });
            } else {
                afd.dial->set_on_speed_changed(
                    [this](const std::string& fan_id, int speed_percent) {
                        send_fan_speed(fan_id, speed_percent);
                    });
            }

            // Long-press to rename — registered on card root only.
            // The arc is excluded so long-press on the arc/handle controls speed,
            // not rename.
            attach_long_press_rename(afd.dial->get_root(), fan.object_name, fan.display_name);

            animated_fan_dials_.push_back(std::move(afd));

            spdlog::trace("[{}] Created AnimatedFanDial for '{}' ({}%)", get_name(),
                          fan.display_name, fan.speed_percent);
        }
    }

    // Owned fans the printer does not publish as Klipper fans.
    if (chamber_air_available) {
        for (const auto& f : owned) {
            const bool republished =
                f.equivalent_object && std::any_of(fans.begin(), fans.end(), [&f](const auto& fan) {
                    return fan.object_name == f.equivalent_object;
                });
            if (republished) {
                continue;
            }
            const char* name = f.role == helix::chamber_air::FanRole::Exhaust
                                   ? lv_tr("Exhaust fan")
                                   : lv_tr("Recirculation fan");
            AnimatedFanDial afd;
            afd.object_name = f.id;
            afd.owned_role = f.role;
            afd.synthetic = true;
            afd.dial = std::make_unique<FanDial>(fans_container_, name, f.id,
                                                 printer_state_.chamber_air().speed_pct(f.role));
            afd.dial->set_hold_until_release(true);
            afd.dial->set_on_speed_changed(
                [this, r = f.role](const std::string& fan_id, int speed_percent) {
                    handle_owned_fan_change(fan_id, r, true, speed_percent);
                });
            animated_fan_dials_.push_back(std::move(afd));
        }
    }

    // Second pass: create auto-controlled fans (fan_status_card widgets)
    for (const auto& fan : fans) {
        if (!fan.is_controllable) {
            // Pass numeric value for arc, then format label with % suffix
            char speed_num_str[16];
            std::snprintf(speed_num_str, sizeof(speed_num_str), "%d", fan.speed_percent);

            const char* attrs[] = {"fan_name", fan.display_name.c_str(), "speed_percent",
                                   speed_num_str, nullptr};

            lv_obj_t* card =
                static_cast<lv_obj_t*>(lv_xml_create(fans_container_, "fan_status_card", attrs));

            if (card) {
                // Find speed label and format with % suffix, including RPM when available
                lv_obj_t* speed_label = helix::ui::find_required(card, "speed_label", get_name());
                if (speed_label) {
                    if (fan.rpm.has_value() && fan.rpm.value() > 0) {
                        char speed_str[32];
                        std::snprintf(speed_str, sizeof(speed_str), "%d%% \xC2\xB7 %d RPM",
                                      fan.speed_percent, fan.rpm.value());
                        lv_label_set_text(speed_label, speed_str);
                    } else {
                        char speed_str[16];
                        helix::format::format_percent(fan.speed_percent, speed_str,
                                                      sizeof(speed_str));
                        lv_label_set_text(speed_label, speed_str);
                    }
                }

                // Find arc and make read-only (fan_arc_core is interactive by default)
                // Also bubble events so long-press reaches the card
                lv_obj_t* arc = helix::ui::find_required(card, "dial_arc", get_name());
                if (arc) {
                    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_flag(arc, LV_OBJ_FLAG_EVENT_BUBBLE);
                    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
                    lv_obj_set_style_shadow_width(arc, 0, LV_PART_KNOB);
                    lv_obj_set_style_outline_width(arc, 0, LV_PART_KNOB);
                }

                // Find fan icon for spin animation
                lv_obj_t* fan_icon = helix::ui::find_required(card, "fan_icon", get_name());
                if (fan_icon) {
                    lv_obj_set_style_transform_pivot_x(fan_icon, LV_PCT(50), 0);
                    lv_obj_set_style_transform_pivot_y(fan_icon, LV_PCT(50), 0);
                }

                // Attach auto-resize for dynamic arc scaling
                helix::ui::fan_arc_attach_auto_resize(card);

                // Long-press to rename
                attach_long_press_rename(card, fan.object_name, fan.display_name);

                AutoFanCard afc;
                afc.object_name = fan.object_name;
                afc.card = card;
                afc.speed_label = speed_label;
                afc.arc = arc;
                afc.fan_icon = fan_icon;
                afc.last_speed_pct = fan.speed_percent;
                auto_fan_cards_.push_back(std::move(afc));

                // Start spin animation if fan is running
                update_auto_fan_animation(auto_fan_cards_.back(), fan.speed_percent);

                spdlog::trace("[{}] Created fan_status_card for '{}' ({}%)", get_name(),
                              fan.display_name, fan.speed_percent);
            } else {
                spdlog::error("[{}] Failed to create fan_status_card for '{}'", get_name(),
                              fan.display_name);
            }
        }
    }

    spdlog::trace("[{}] Populated {} animated fan dials and {} auto fan cards", get_name(),
                  animated_fan_dials_.size(), auto_fan_cards_.size());
}

void FanControlOverlay::update_fan_speeds() {
    const auto& fans = printer_state_.fan_state().get_fans();

    // Note: FanDial widgets are updated via AnimatedValue bindings in subscribe_to_fan_speeds()
    // This method only updates auto fan cards which don't need animation

    // Update auto fan card labels and arcs (immediate, no animation)
    for (auto& card_info : auto_fan_cards_) {
        for (const auto& fan : fans) {
            if (fan.object_name == card_info.object_name) {
                // Update speed label with RPM when available
                if (card_info.speed_label) {
                    if (fan.rpm.has_value() && fan.rpm.value() > 0) {
                        char speed_str[32];
                        std::snprintf(speed_str, sizeof(speed_str), "%d%% \xC2\xB7 %d RPM",
                                      fan.speed_percent, fan.rpm.value());
                        lv_label_set_text(card_info.speed_label, speed_str);
                    } else {
                        char speed_str[16];
                        helix::format::format_percent(fan.speed_percent, speed_str,
                                                      sizeof(speed_str));
                        lv_label_set_text(card_info.speed_label, speed_str);
                    }
                }
                if (card_info.arc) {
                    lv_arc_set_value(card_info.arc, fan.speed_percent);
                }
                // fan_spin_start() restarts from 0 degrees, and an rpm change alone lands here.
                if (fan.speed_percent != card_info.last_speed_pct) {
                    update_auto_fan_animation(card_info, fan.speed_percent);
                }
                break;
            }
        }
    }

    spdlog::trace("[{}] Updated auto fan card speeds", get_name());
}

void FanControlOverlay::send_fan_speed(const std::string& object_name, int speed_percent) {
    if (!api_) {
        spdlog::warn("[{}] Cannot send fan speed - no API connection", get_name());
        NOTIFY_WARNING(lv_tr("No printer connection"));
        return;
    }

    spdlog::trace("[{}] Setting '{}' to {}%", get_name(), object_name, speed_percent);

    // Optimistic update: immediately reflect the new speed in PrinterState so
    // other UI (e.g. controls card secondary fan rows) updates without waiting
    // for the Moonraker round-trip confirmation.
    printer_state_.fan_state().update_fan_speed(object_name,
                                                static_cast<double>(speed_percent) / 100.0);

    // IMoonrakerAPI::set_fan_speed expects:
    // - "fan" for part cooling fan (uses M106)
    // - Fan name for generic fans (uses SET_FAN_SPEED)
    api_->set_fan_speed(
        object_name, static_cast<double>(speed_percent),
        []() {
            // Silent success
        },
        [object_name](const MoonrakerError& err) {
            helix::ui::notify_error_tr(TR_NOOP("Fan control failed: {}"), err);
        });
}

void FanControlOverlay::restore_dial(const std::string& dial_id, int speed_percent) {
    for (auto& afd : animated_fan_dials_) {
        if (afd.object_name == dial_id && afd.dial) {
            afd.dial->restore_speed(speed_percent);
            return;
        }
    }
}

void FanControlOverlay::send_owned_fan_speed(const std::string& dial_id,
                                             helix::chamber_air::FanRole role, bool synthetic,
                                             int speed_percent) {
    restore_dial(dial_id, speed_percent);
    if (!synthetic) {
        send_fan_speed(dial_id, speed_percent);
        return;
    }
    const std::string gcode =
        helix::chamber_air::fan_speed_gcode(printer_state_.get_discovery(), role, speed_percent);
    if (gcode.empty()) {
        return;
    }
    helix::MachineGcode cmd;
    cmd.gcode = gcode;
    cmd.log_name = gcode;
    cmd.display_name = lv_tr("Fan control");
    helix::execute_machine_gcode(api_, cmd, "[FanControlOverlay]");
}

void FanControlOverlay::handle_owned_fan_change(const std::string& dial_id,
                                                helix::chamber_air::FanRole role, bool synthetic,
                                                int speed_percent) {
    using helix::chamber_air::FanGuardAction;
    using helix::chamber_air::FanRole;
    const auto& ca = printer_state_.chamber_air();
    const int current = synthetic ? ca.speed_pct(role) : [&] {
        for (const auto& fan : printer_state_.fan_state().get_fans()) {
            if (fan.object_name == dial_id) {
                return fan.speed_percent;
            }
        }
        return ca.speed_pct(role);
    }();
    const auto guard = helix::chamber_air::guard_fan_change(
        ca.mode(), role, speed_percent, current, ca.threshold_pct(role),
        helix::chamber_air::guard_floor_pct(printer_state_.get_discovery()));

    switch (guard.action) {
    case FanGuardAction::Apply:
        send_owned_fan_speed(dial_id, role, synthetic, guard.pct);
        return;
    case FanGuardAction::Blocked:
        restore_dial(dial_id, current);
        NOTIFY_WARNING(role == FanRole::Exhaust
                           ? lv_tr("With internal circulation on, the exhaust fan cannot be "
                                   "turned on")
                           : lv_tr("With exhaust cooling on, the recirculation fan cannot be "
                                   "turned on"));
        return;
    case FanGuardAction::Confirm: {
        // One question at a time: a second change while it is open puts the dial back.
        if (owned_confirm_open_) {
            restore_dial(dial_id, current);
            return;
        }
        owned_confirm_open_ = true;
        const int pct = guard.pct;
        helix::ui::ConfirmOptions opts;
        opts.owner_token = lifetime_.token();
        opts.on_cancel = [this, dial_id, current]() {
            owned_confirm_open_ = false;
            restore_dial(dial_id, current);
        };
        opts.on_dismiss = [this, dial_id, current]() {
            owned_confirm_open_ = false;
            restore_dial(dial_id, current);
        };
        helix::ui::modal_confirm(
            lv_tr("Lower fan speed?"),
            role == FanRole::Exhaust
                ? lv_tr("Reducing the fan speed may affect print quality. Do you want to "
                        "continue?")
                : lv_tr("Reducing the fan speed may affect filtration performance. Do you "
                        "want to continue?"),
            ModalSeverity::Warning, lv_tr("Lower"),
            [this, dial_id, role, synthetic, pct]() {
                owned_confirm_open_ = false;
                send_owned_fan_speed(dial_id, role, synthetic, pct);
            },
            opts);
        return;
    }
    }
}

void FanControlOverlay::subscribe_to_fan_speeds() {
    using helix::ui::observe;

    // Bind AnimatedValue for each FanDial - provides smooth animation when speed changes
    for (auto& afd : animated_fan_dials_) {
        SubjectLifetime lifetime;
        lv_subject_t* subject = nullptr;
        if (afd.synthetic) {
            auto& ca = printer_state_.chamber_air();
            subject = afd.owned_role == helix::chamber_air::FanRole::Exhaust
                          ? ca.exhaust_pct_subject()
                          : ca.recirculate_pct_subject();
            lifetime = printer_state_.get_subjects_lifetime();
        } else {
            subject = printer_state_.fan_state().get_fan_speed_subject(afd.object_name, lifetime);
        }
        if (subject) {
            FanDial* dial_ptr = afd.dial.get();
            // 2% threshold to avoid micro-updates
            helix::ui::AnimatedValueConfig anim_config;
            anim_config.duration_ms = 300;
            anim_config.threshold = 2;
            afd.animation.bind(
                subject, [dial_ptr](int percent) { dial_ptr->set_speed(percent); }, anim_config,
                lifetime);
            spdlog::trace("[{}] Bound AnimatedValue for '{}'", get_name(), afd.object_name);
        }
    }

    // Subscribe to auto fan subjects using observer factory (deferred, no animation)
    fan_speed_observers_.reserve(auto_fan_cards_.size());
    for (const auto& card : auto_fan_cards_) {
        SubjectLifetime lifetime;
        if (auto* subject =
                printer_state_.fan_state().get_fan_speed_subject(card.object_name, lifetime)) {
            fan_speed_observers_.push_back(observe<int>(
                subject, this,
                [](FanControlOverlay* self, int /*speed*/) {
                    if (self->is_visible()) {
                        self->update_fan_speeds();
                    }
                },
                lifetime));
            spdlog::trace("[{}] Subscribed to auto fan subject for '{}'", get_name(),
                          card.object_name);
        }
    }

    // A tachometer reading changes at a steady speed, which no speed subject reports.
    fan_rpm_observer_ = observe<int>(
        printer_state_.fan_state().get_fan_rpm_version_subject(), this,
        [](FanControlOverlay* self, int /*version*/) {
            if (self->is_visible()) {
                self->update_fan_speeds();
            }
        },
        printer_state_.get_subjects_lifetime());

    spdlog::trace("[{}] Bound {} animated fan dials, subscribed to {} auto fan subjects",
                  get_name(), animated_fan_dials_.size(), fan_speed_observers_.size());
}

void FanControlOverlay::unsubscribe_from_fan_speeds() {
    // Unbind AnimatedValue instances
    for (auto& afd : animated_fan_dials_) {
        afd.animation.unbind();
    }

    // Clear auto fan observers
    fan_speed_observers_.clear();
    fan_rpm_observer_.reset();

    spdlog::trace("[{}] Unsubscribed from fan speed subjects", get_name());
}

// ============================================================================
// FAN ICON SPIN ANIMATION
// ============================================================================

void FanControlOverlay::update_auto_fan_animation(AutoFanCard& card, int speed_pct) {
    card.last_speed_pct = speed_pct;
    if (!card.fan_icon)
        return;

    if (!DisplaySettingsManager::instance().get_animations_enabled() || speed_pct <= 0) {
        helix::ui::fan_spin_stop(card.fan_icon);
    } else {
        helix::ui::fan_spin_start(card.fan_icon, speed_pct);
    }
}

void FanControlOverlay::refresh_all_auto_fan_animations() {
    for (auto& card : auto_fan_cards_) {
        update_auto_fan_animation(card, card.last_speed_pct);
    }
}
