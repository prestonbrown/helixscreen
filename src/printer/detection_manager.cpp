// SPDX-License-Identifier: GPL-3.0-or-later
#include "detection_manager.h"

#include "ui_callback_helpers.h"

#include "core/static_subject_registry.h"
#include "helix/xml/scoped_subject_registry.h"
#include "i_moonraker_client.h"
#include "non_filament_pause.h"
#include "notice.h"
#include "settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::detection {

DetectionManager& DetectionManager::instance() {
    static DetectionManager s_instance;
    return s_instance;
}

void DetectionManager::ensure_availability_subject() {
    if (availability_subject_ready_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(detection_available_subject_, any_available() ? 1 : 0,
                           "detection_available", subjects_);
    UI_MANAGED_SUBJECT_INT(detection_pause_applicable_subject_, any_pause_applicable() ? 1 : 0,
                           "detection_pause_applicable", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_check_available_subject_, bed_check_source() ? 1 : 0,
                           "detection_bed_check_available", subjects_);
    // The firmware's default until the printer answers.
    UI_MANAGED_SUBJECT_INT(bed_check_enabled_subject_, 1, "detection_bed_check_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_check_switchable_subject_, 1, "detection_bed_check_switchable",
                           subjects_);
    // The Safety & Alerts row's toggle. Registered with its subject so the
    // row and what it drives exist together.
    register_xml_callbacks({
        {"on_bed_check_changed",
         [](lv_event_t* e) {
             DetectionManager::instance().set_bed_check_enabled(helix::ui::event_checked(e));
         }},
    });
    availability_subject_ready_ = true;
    // Bound only by XML and written only here, so a printer switch leaves them up.
    StaticSubjectRegistry::instance().register_deinit(
        "DetectionManager",
        [this] {
            subjects_.deinit_all();
            availability_subject_ready_ = false;
        },
        StaticSubjectRegistry::Lifetime::Process);
}

void DetectionManager::update_availability() {
    if (availability_subject_ready_) {
        lv_subject_set_int(&detection_available_subject_, any_available() ? 1 : 0);
        lv_subject_set_int(&detection_pause_applicable_subject_, any_pause_applicable() ? 1 : 0);
        lv_subject_set_int(&bed_check_available_subject_, bed_check_source() ? 1 : 0);
    }
    maybe_seed_settings();
}

DetectionSource* DetectionManager::bed_check_source() const {
    for (const auto& src : sources_) {
        if (src && src->available() && src->has_bed_check()) {
            return src.get();
        }
    }
    return nullptr;
}

void DetectionManager::refresh_bed_check() {
    DetectionSource* src = bed_check_source();
    if (!src) {
        return;
    }
    src->read_bed_check(lifetime_.bg_cb(
        "DetectionManager::on_bed_check_read",
        [this](std::optional<DetectionSource::BedCheckState> state) {
            if (!state || !availability_subject_ready_) {
                return;
            }
            // With the module off, the check is off and its own switch does nothing.
            lv_subject_set_int(&bed_check_enabled_subject_,
                               state->enabled && state->module_enabled ? 1 : 0);
            lv_subject_set_int(&bed_check_switchable_subject_, state->module_enabled ? 1 : 0);
        }));
}

bool DetectionManager::set_bed_check_enabled(bool enabled, std::function<void()> on_accepted) {
    DetectionSource* src = bed_check_source();
    if (!src) {
        spdlog::warn("DetectionManager: no bed check to switch");
        return false;
    }
    const int before = lv_subject_get_int(&bed_check_enabled_subject_);
    lv_subject_set_int(&bed_check_enabled_subject_, enabled ? 1 : 0);
    spdlog::info("DetectionManager: bed check -> {}", enabled ? "on" : "off");
    src->set_bed_check(
        enabled,
        lifetime_.bg_cb("DetectionManager::on_bed_check_set",
                        [this, before, on_accepted](bool accepted, const std::string& refusal) {
                            if (accepted) {
                                if (on_accepted) {
                                    on_accepted();
                                }
                                return;
                            }
                            lv_subject_set_int(&bed_check_enabled_subject_, before);
                            show_bed_check_refusal(refusal);
                        }));
    return true;
}

void DetectionManager::show_bed_check_refusal(const std::string& refusal) {
    const std::string text =
        refusal.empty()
            ? std::string(lv_tr("The printer did not change its bed check"))
            : fmt::format(lv_tr("The printer did not change its bed check: {}"), refusal);
    publish_notice({NoticeSeverity::ERROR, text, 6000});
}

bool DetectionManager::pause_explained_by_detection() const {
    for (const auto& src : sources_) {
        if (src && src->available() && src->explains_current_pause()) {
            return true;
        }
    }
    return false;
}

void DetectionManager::maybe_seed_settings() {
    SettingsManager& sm = SettingsManager::instance();
    if (sm.is_detection_seeded()) {
        return;
    }
    bool any_capable = false;
    for (const auto& src : sources_) {
        if (!src || !src->available()) {
            continue;
        }
        any_capable = true;
        // First capable source with a stored preference wins; only one source
        // is ever capable on a given printer, so this never arbitrates.
        if (const auto pref = src->printer_preference()) {
            sm.set_detection_enabled(pref->enabled);
            sm.set_detection_pause_on_detect(pref->pause);
            spdlog::info("DetectionManager: seeded detection settings from '{}' (enabled={}, "
                         "pause={})",
                         src->id(), pref->enabled, pref->pause);
            break;
        }
    }
    // Mark even when every capable source had no stored preference: the
    // defaults are then the user's, and re-asking on every start would fight
    // later setting changes. A printer with no capable source stays unseeded
    // so detection added later still seeds once.
    if (any_capable) {
        sm.mark_detection_seeded();
    }
}

void DetectionManager::init(helix::IMoonrakerClient* client, helix::PrinterState* state) {
    client_ = client;
    state_ = state;
    ensure_availability_subject();
    // Do NOT probe here: init() runs during PrinterSession::init_panel_subjects, before
    // the WebSocket connects. printer.objects.list would fail (not connected) and the
    // capability would latch false forever. Instead, run the probe on every connect.
    if (client_ && !connect_observer_registered_) {
        client_->add_connected_observer("DetectionManager::refresh_capabilities",
                                        lifetime_.bg_cb("DetectionManager::on_connected",
                                                        [this]() { refresh_capabilities(); }));
        connect_observer_registered_ = true;
    }
}

void DetectionManager::register_source(std::unique_ptr<DetectionSource> src) {
    if (!src) {
        return;
    }
    const std::string id = src->id();
    src->set_callback([this](const DetectionEvent& e) { on_event(e); });
    if (policies_.find(id) == policies_.end()) {
        policies_[id] = DetectionPolicy::DeferToSource;
    }
    sources_.push_back(std::move(src));
    helix::non_filament_pause::set_explainer([this]() { return pause_explained_by_detection(); });
    ensure_availability_subject();
    update_availability();
    spdlog::debug("DetectionManager: registered source '{}'", id);
}

void DetectionManager::set_policy(const std::string& source_id, DetectionPolicy p) {
    policies_[source_id] = p;
}

DetectionPolicy DetectionManager::policy(const std::string& source_id) const {
    auto it = policies_.find(source_id);
    if (it == policies_.end()) {
        return DetectionPolicy::DeferToSource;
    }
    return it->second;
}

bool DetectionManager::any_available() const {
    for (const auto& src : sources_) {
        if (src && src->available()) {
            return true;
        }
    }
    return false;
}

bool DetectionManager::any_pause_applicable() const {
    for (const auto& src : sources_) {
        if (src && src->available() && !src->self_pauses()) {
            return true;
        }
    }
    return false;
}

DetectionResponse DetectionManager::response_for(DetectionPolicy p) const {
    const SettingsManager& sm = SettingsManager::instance();
    if (!sm.get_detection_enabled() || p == DetectionPolicy::Off) {
        return DetectionResponse::Suppressed;
    }
    if (p == DetectionPolicy::NotifyOnly || !sm.get_detection_pause_on_detect()) {
        return DetectionResponse::WarnOnly;
    }
    return DetectionResponse::PauseAndRespond;
}

bool DetectionManager::source_can_tune(const std::string& source_id) const {
    for (const auto& src : sources_) {
        if (src && src->id() == source_id)
            return src->can_tune();
    }
    return false;
}

void DetectionManager::tune_source(const std::string& source_id) {
    for (const auto& src : sources_) {
        if (src && src->id() == source_id) {
            if (src->can_tune())
                src->tune();
            return;
        }
    }
}

void DetectionManager::on_event(const DetectionEvent& e) {
    if (e.already_paused) {
        // The pause is this detection's, whatever the policy shows of it: a
        // runout surface that answered it first stands down.
        helix::non_filament_pause::announce();
    }
    DetectionPolicy p = policy(e.source_id);
    // A dirty bed stops a print the printer cannot resume: the user owes it a
    // decision whatever the policy says about defects found mid-print.
    if (p == DetectionPolicy::Off && e.kind != DetectionKind::DirtyBed) {
        spdlog::debug("DetectionManager: event from '{}' suppressed (policy Off)", e.source_id);
        return;
    }
    spdlog::info("DetectionManager: detection from '{}' (kind={}, paused={})", e.source_id,
                 static_cast<int>(e.kind), e.already_paused);
    if (presenter_) {
        presenter_(e, p);
    }
}

// The object names out of a printer.objects.list "objects" array; anything
// that is not an array of strings contributes nothing.
static std::vector<std::string> object_names(const json& objects) {
    std::vector<std::string> names;
    if (!objects.is_array()) {
        return names;
    }
    names.reserve(objects.size());
    for (const auto& obj : objects) {
        if (obj.is_string()) {
            names.push_back(obj.get<std::string>());
        }
    }
    return names;
}

void DetectionManager::apply_objects_list(const json& objects) {
    const std::vector<std::string> names = object_names(objects);
    for (const auto& src : sources_) {
        if (src) {
            src->on_objects_listed(names);
        }
    }
    update_availability();
}

void DetectionManager::refresh_capabilities() {
    if (!client_) {
        return;
    }
    // Sources with a local capability probe re-run it here: answers that
    // depend on state saved after boot can change without a restart, and
    // connect is the natural re-check point. Sources whose capability
    // arrives over the wire no-op and take the objects.list round-trip
    // below.
    for (const auto& src : sources_) {
        if (src) {
            src->refresh_capability();
        }
    }
    update_availability();
    client_->send_jsonrpc(
        "printer.objects.list", json::object(),
        lifetime_.bg_cb("DetectionManager::on_objects_list",
                        [this](const json& resp) {
                            json objects = json::array();
                            auto result_it = resp.find("result");
                            if (result_it != resp.end() && result_it->is_object()) {
                                auto objects_it = result_it->find("objects");
                                if (objects_it != result_it->end()) {
                                    objects = *objects_it;
                                }
                            }
                            apply_objects_list(objects);
                            refresh_bed_check();
                        }),
        [](const MoonrakerError& err) {
            spdlog::warn("DetectionManager: capability probe failed: {}", err.message);
        });
}

bool DetectionManager::apply_objects_list_for_test(const nlohmann::json& objects) {
    apply_objects_list(objects);
    return any_available();
}

void DetectionManager::reset_for_test() {
    helix::non_filament_pause::set_explainer(nullptr);
    sources_.clear();
    policies_.clear();
    presenter_ = nullptr;
    client_ = nullptr;
    state_ = nullptr;
    connect_observer_registered_ = false;
    lifetime_.invalidate();
    // Keep the subject initialized (observers may still be bound from an
    // earlier test in this process); only its value resets.
    if (availability_subject_ready_) {
        lv_subject_set_int(&detection_available_subject_, 0);
        lv_subject_set_int(&detection_pause_applicable_subject_, 0);
        lv_subject_set_int(&bed_check_available_subject_, 0);
        lv_subject_set_int(&bed_check_enabled_subject_, 1);
        lv_subject_set_int(&bed_check_switchable_subject_, 1);
    }
}

} // namespace helix::detection
