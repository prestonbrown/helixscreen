// SPDX-License-Identifier: GPL-3.0-or-later
#include "detection_manager.h"

#include "i_moonraker_client.h"
#include "u1_stock_detection_source.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::detection {

DetectionManager& DetectionManager::instance() {
    static DetectionManager s_instance;
    return s_instance;
}

void DetectionManager::init(helix::IMoonrakerClient* client, helix::PrinterState* state) {
    client_ = client;
    state_ = state;
    // Do NOT probe here: init() runs during Application::init_panel_subjects, before
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

void DetectionManager::on_event(const DetectionEvent& e) {
    DetectionPolicy p = policy(e.source_id);
    if (p == DetectionPolicy::Off) {
        spdlog::debug("DetectionManager: event from '{}' suppressed (policy Off)", e.source_id);
        return;
    }
    spdlog::info("DetectionManager: detection from '{}' (kind={}, paused={})", e.source_id,
                 static_cast<int>(e.kind), e.already_paused);
    if (presenter_) {
        presenter_(e, p);
    }
}

// Scan a printer.objects.list "objects" array for the U1 "defect_detection" module.
static bool objects_have_defect_detection(const json& objects) {
    if (!objects.is_array()) {
        return false;
    }
    for (const auto& obj : objects) {
        if (obj.is_string() && obj.get<std::string>() == "defect_detection") {
            return true;
        }
    }
    return false;
}

void DetectionManager::apply_capability(bool has_defect_detection) {
    spdlog::info("DetectionManager: defect_detection capability = {}", has_defect_detection);
    for (const auto& src : sources_) {
        // id() is the source's unique registration key — one id names exactly
        // one concrete DetectionSource — so the match already establishes the
        // type and static_cast is equivalent to the dynamic_cast this replaces.
        // Needed because the firmware builds -fno-rtti.
        if (src && src->id() == U1StockSource::SOURCE_ID) {
            static_cast<U1StockSource*>(src.get())->set_capable(has_defect_detection);
        }
    }
}

void DetectionManager::refresh_capabilities() {
    if (!client_) {
        return;
    }
    client_->send_jsonrpc(
        "printer.objects.list", json::object(),
        lifetime_.bg_cb("DetectionManager::on_objects_list",
                        [this](const json& resp) {
                            bool has = false;
                            auto result_it = resp.find("result");
                            if (result_it != resp.end() && result_it->is_object()) {
                                auto objects_it = result_it->find("objects");
                                if (objects_it != result_it->end()) {
                                    has = objects_have_defect_detection(*objects_it);
                                }
                            }
                            apply_capability(has);
                        }),
        [](const MoonrakerError& err) {
            spdlog::warn("DetectionManager: capability probe failed: {}", err.message);
        });
}

bool DetectionManager::apply_objects_list_for_test(const nlohmann::json& objects) {
    bool has = objects_have_defect_detection(objects);
    apply_capability(has);
    return has;
}

void DetectionManager::reset_for_test() {
    sources_.clear();
    policies_.clear();
    presenter_ = nullptr;
    client_ = nullptr;
    state_ = nullptr;
    connect_observer_registered_ = false;
    lifetime_.invalidate();
}

} // namespace helix::detection
