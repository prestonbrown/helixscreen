// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_air_state.h"

#include "chamber_air_settings.h"
#include "core/observer_factory.h"
#include "core/temp_unit_subject.h"
#include "core/temperature_units.h"
#include "printer_discovery.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

using chamber_air::FanRole;

namespace {

int to_pct(double fraction) {
    return std::clamp(static_cast<int>(std::lround(fraction * 100.0)), 0, 100);
}

template <typename T> void keep_news(std::optional<T>& last, const std::optional<T>& news) {
    if (news) {
        last = news;
    }
}

} // namespace

void ChamberAirState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        return;
    }
    INIT_SUBJECT_INT(chamber_air_available, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_mode, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_exhaust_pct, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_recirculate_pct, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_has_targets, 0, subjects_, register_xml);
    // Read from C++ only (the maintenance model observes it), so it takes no XML name.
    lv_subject_init_int(&chamber_air_filter_minutes_, -1);
    subjects_.register_subject(&chamber_air_filter_minutes_, nullptr);
    const chamber_air::ModeTargets d{};
    INIT_SUBJECT_INT(chamber_air_target_c, static_cast<int>(d.target_c), subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_exhaust_target_pct, d.exhaust_pct, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_air_recirculate_target_pct, d.recirculate_pct, subjects_,
                     register_xml);
    INIT_SUBJECT_STRING(chamber_air_target_text, "", subjects_, register_xml);
    // Immediate: the handler only copies a string, and the slider drags it live.
    target_text_observer_ = ui::observe<int>(
        &chamber_air_target_c_, this,
        [](ChamberAirState* self, int celsius) {
            lv_subject_set_string(&self->chamber_air_target_text_,
                                  units::format_display_temp(celsius).c_str());
        },
        subjects_.get_subjects_lifetime(), ui::Dispatch::Immediate);
    unit_observer_ = ui::observe_temp_unit_change(this, [](ChamberAirState* self) {
        lv_subject_set_string(
            &self->chamber_air_target_text_,
            units::format_display_temp(lv_subject_get_int(&self->chamber_air_target_c_)).c_str());
    });
    subjects_initialized_ = true;
}

void ChamberAirState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    target_text_observer_.reset();
    unit_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void ChamberAirState::set_hardware(const PrinterDiscovery& hw) {
    supported_ = chamber_air::supported(hw);
    live_presence_ = supported_ && chamber_air::presence_is_live(hw);
    takes_targets_ = supported_ && chamber_air::takes_targets(hw);
    startup_pending_ = live_presence_;
    present_.reset();
    last_ = chamber_air::Reading{};
    if (subjects_initialized_ && takes_targets_) {
        const chamber_air::ModeTargets t = chamber_air::load_targets();
        lv_subject_set_int(&chamber_air_target_c_, static_cast<int>(std::lround(t.target_c)));
        lv_subject_set_int(&chamber_air_exhaust_target_pct_, t.exhaust_pct);
        lv_subject_set_int(&chamber_air_recirculate_target_pct_, t.recirculate_pct);
    }
    spdlog::debug("[ChamberAir] Provider '{}' (live presence {}, targets {})",
                  chamber_air::provider_name(hw), live_presence_, takes_targets_);
    publish();
}

void ChamberAirState::apply(const chamber_air::Reading& r) {
    if (!supported_) {
        return;
    }
    const bool was_available = available();
    keep_news(present_, r.present);
    keep_news(last_.mode, r.mode);
    keep_news(last_.exhaust_speed, r.exhaust_speed);
    keep_news(last_.recirculate_speed, r.recirculate_speed);
    keep_news(last_.exhaust_threshold, r.exhaust_threshold);
    keep_news(last_.recirculate_threshold, r.recirculate_threshold);
    keep_news(last_.critical_temp_c, r.critical_temp_c);
    keep_news(last_.filter_seconds, r.filter_seconds);
    publish();

    if (!live_presence_ || !present_) {
        return;
    }
    const bool now_available = available();
    if (startup_pending_) {
        startup_pending_ = false;
        spdlog::info("[ChamberAir] Accessory {} at connect", now_available ? "fitted" : "absent");
        if (on_edge_) {
            on_edge_(now_available, true);
        }
    } else if (now_available != was_available) {
        spdlog::info("[ChamberAir] Accessory {}", now_available ? "fitted" : "removed");
        if (on_edge_) {
            on_edge_(now_available, false);
        }
    }
}

void ChamberAirState::set_presence_edge_handler(PresenceEdge handler) {
    on_edge_ = std::move(handler);
}

bool ChamberAirState::available() const {
    if (!supported_) {
        return false;
    }
    return !live_presence_ || present_.value_or(false);
}

std::optional<chamber_air::AirMode> ChamberAirState::mode() const {
    return last_.mode;
}

std::optional<double> ChamberAirState::filter_seconds() const {
    return last_.filter_seconds;
}

std::optional<double> ChamberAirState::critical_temp_c() const {
    return last_.critical_temp_c;
}

int ChamberAirState::speed_pct(FanRole role) const {
    const auto& v = role == FanRole::Exhaust ? last_.exhaust_speed : last_.recirculate_speed;
    return v ? to_pct(*v) : 0;
}

std::optional<int> ChamberAirState::threshold_pct(FanRole role) const {
    const auto& v =
        role == FanRole::Exhaust ? last_.exhaust_threshold : last_.recirculate_threshold;
    return v ? std::optional<int>(to_pct(*v)) : std::nullopt;
}

chamber_air::ModeTargets ChamberAirState::targets() const {
    if (!subjects_initialized_) {
        return chamber_air::ModeTargets{};
    }
    chamber_air::ModeTargets t;
    t.target_c = lv_subject_get_int(const_cast<lv_subject_t*>(&chamber_air_target_c_));
    t.exhaust_pct = lv_subject_get_int(const_cast<lv_subject_t*>(&chamber_air_exhaust_target_pct_));
    t.recirculate_pct =
        lv_subject_get_int(const_cast<lv_subject_t*>(&chamber_air_recirculate_target_pct_));
    return chamber_air::clamp_targets(t);
}

void ChamberAirState::commit_targets() {
    chamber_air::save_targets(targets());
}

void ChamberAirState::publish() {
    if (!subjects_initialized_) {
        return;
    }
    const bool avail = available();
    auto set_if_changed = [](lv_subject_t* s, int v) {
        if (lv_subject_get_int(s) != v) {
            lv_subject_set_int(s, v);
        }
    };
    set_if_changed(&chamber_air_available_, avail ? 1 : 0);
    set_if_changed(&chamber_air_mode_, avail && last_.mode ? static_cast<int>(*last_.mode) : -1);
    set_if_changed(&chamber_air_exhaust_pct_, speed_pct(FanRole::Exhaust));
    set_if_changed(&chamber_air_recirculate_pct_, speed_pct(FanRole::Recirculate));
    set_if_changed(&chamber_air_has_targets_, avail && takes_targets_ ? 1 : 0);
    // The counter is the firmware's and survives the accessory coming off, so it is
    // published whether or not the accessory is fitted.
    set_if_changed(
        &chamber_air_filter_minutes_,
        last_.filter_seconds ? static_cast<int>(std::max(0.0, *last_.filter_seconds) / 60.0) : -1);
}

} // namespace helix
