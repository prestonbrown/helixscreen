// SPDX-License-Identifier: GPL-3.0-or-later

#include "calibration_owned_heat.h"

#include "ui_temperature_utils.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/timer_guard.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix {

namespace {

bool printer_connected() {
    return lv_subject_get_int(
               get_printer_state().network_state().get_printer_connection_state_subject()) ==
           static_cast<int>(ConnectionState::CONNECTED);
}

int live_target_c(const std::string& heater) {
    auto& temps = get_printer_state().temperature_state();
    lv_subject_t* target = heater == "heater_bed" ? temps.get_bed_target_subject()
                                                  : temps.get_extruder_target_subject(heater);
    return target ? ui::temperature::deci_to_degrees(lv_subject_get_int(target)) : 0;
}

} // namespace

CalibrationOwnedHeat::CalibrationOwnedHeat(const char* log_tag, SendOff send_off)
    : log_tag_(log_tag), send_off_(std::move(send_off)) {}

CalibrationOwnedHeat::~CalibrationOwnedHeat() {
    cancel_settle_timer();
}

void CalibrationOwnedHeat::own(const std::string& heater, int expected_c, bool hold) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry& e) { return e.heater == heater; });
    if (it == entries_.end()) {
        entries_.push_back({heater, expected_c, hold, false});
    } else {
        it->expected_c = expected_c;
        it->owed = false;
    }
    if (std::none_of(entries_.begin(), entries_.end(), [](const Entry& e) { return e.owed; })) {
        stop_waiting();
    }
    update_hold();
}

bool CalibrationOwnedHeat::owns(const std::string& heater) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.heater == heater; });
}

bool CalibrationOwnedHeat::any() const {
    return !entries_.empty();
}

bool CalibrationOwnedHeat::owes_off(const std::string& heater) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.heater == heater && e.owed; });
}

void CalibrationOwnedHeat::send_unless_taken(const Entry& entry) {
    auto& state = get_printer_state();
    const int live_c = live_target_c(entry.heater);
    if (!owned_heat_may_clear(entry.expected_c, live_c,
                              job_holds_machine(state.print_state().get_print_lifecycle()))) {
        spdlog::info("[{}] Leaving {} on: it is no longer this run's", log_tag_, entry.heater);
        return;
    }
    spdlog::info("[{}] Turning {} off", log_tag_, entry.heater);
    send_off_(entry.heater);
}

void CalibrationOwnedHeat::turn_off() {
    if (entries_.empty()) {
        return;
    }
    if (!printer_connected()) {
        spdlog::info("[{}] Link is down: heaters go off on reconnect", log_tag_);
        for (Entry& entry : entries_) {
            entry.owed = true;
        }
        watch_connection();
        update_hold();
        return;
    }
    const std::vector<Entry> sending = std::move(entries_);
    entries_.clear();
    stop_waiting();
    update_hold();
    for (const Entry& entry : sending) {
        send_unless_taken(entry);
    }
}

void CalibrationOwnedHeat::flush_owed() {
    if (!printer_connected()) {
        return;
    }
    std::vector<Entry> kept;
    std::vector<Entry> sending;
    for (Entry& entry : entries_) {
        (entry.owed ? sending : kept).push_back(std::move(entry));
    }
    entries_ = std::move(kept);
    if (!sending.empty()) {
        stop_waiting();
    }
    update_hold();
    for (const Entry& entry : sending) {
        send_unless_taken(entry);
    }
}

void CalibrationOwnedHeat::before_disconnect() {
    const std::vector<Entry> ending = std::move(entries_);
    entries_.clear();
    stop_waiting();
    update_hold();
    if (!printer_connected()) {
        return;
    }
    for (const Entry& entry : ending) {
        if (entry.hold || entry.owed) {
            send_unless_taken(entry);
        }
    }
}

void CalibrationOwnedHeat::forget() {
    entries_.clear();
    stop_waiting();
    update_hold();
}

void CalibrationOwnedHeat::update_hold() {
    heater_hold_.set(std::any_of(entries_.begin(), entries_.end(),
                                 [](const Entry& e) { return e.hold || e.owed; }));
}

void CalibrationOwnedHeat::watch_connection() {
    auto& state = get_printer_state();
    connection_observer_ = ui::observe<int>(
        state.network_state().get_printer_connection_state_subject(), this,
        [](CalibrationOwnedHeat* self, int) {
            // The first status after a reconnect is what shows a job or a new target.
            if (printer_connected() && !self->settle_timer_) {
                self->settle_timer_ = lv_timer_create(on_settle_timer, kOwedOffSettleMs, self);
            }
        },
        state.get_subjects_lifetime());
}

void CalibrationOwnedHeat::stop_waiting() {
    connection_observer_.reset();
    cancel_settle_timer();
}

void CalibrationOwnedHeat::cancel_settle_timer() {
    ui::lv_timer_cancel_safe(settle_timer_);
    settle_timer_ = nullptr;
}

void CalibrationOwnedHeat::on_settle_timer(lv_timer_t* timer) {
    auto* self = static_cast<CalibrationOwnedHeat*>(lv_timer_get_user_data(timer));
    self->cancel_settle_timer();
    self->flush_owed();
}

} // namespace helix
