// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "detection_source.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class IMoonrakerClient;
class PrinterState;
} // namespace helix

namespace helix::detection {

enum class DetectionPolicy { Off, NotifyOnly, DeferToSource };

/// Registry of detection sources plus per-source policy and the UI presenter hook.
///
/// Sources route their (already main-thread-marshaled) events into the manager,
/// which consults the source's policy and invokes the presenter when warranted.
class DetectionManager {
  public:
    static DetectionManager& instance();

    /// Wire up Moonraker/PrinterState deps and register a post-connect hook that
    /// (re)probes detector capabilities once the WebSocket is up. The probe is NOT
    /// run here — at init() time (Application::init_panel_subjects) the WebSocket has
    /// not connected yet, so printer.objects.list would fail and capable_ would stay
    /// false forever. Hooking add_connected_observer() runs it after every connect.
    void init(helix::IMoonrakerClient* client, helix::PrinterState* state);

    /// (Re)scan printer.objects.list for "defect_detection" and update sources'
    /// capability flags. Idempotent; safe to call on every reconnect.
    void refresh_capabilities();

    /// Take ownership of a source, route its events here, default its policy to
    /// DeferToSource if one was not already set.
    void register_source(std::unique_ptr<DetectionSource> src);

    void set_policy(const std::string& source_id, DetectionPolicy p);
    DetectionPolicy policy(const std::string& source_id) const;

    bool any_available() const;

    using Presenter = std::function<void(const DetectionEvent&, DetectionPolicy)>;
    void set_presenter(Presenter p) {
        presenter_ = std::move(p);
    }

    void reset_for_test();

    /// Test seam: feed a printer.objects.list "objects" array directly and apply
    /// the same capability-detection logic the live probe uses (without a live
    /// MoonrakerClient). Returns the detected "defect_detection" capability.
    bool apply_objects_list_for_test(const nlohmann::json& objects);

  private:
    DetectionManager() = default;

    void on_event(const DetectionEvent& e);

    /// Apply a resolved capability flag to every source that consumes it.
    void apply_capability(bool has_defect_detection);

    helix::IMoonrakerClient* client_ = nullptr;
    helix::PrinterState* state_ = nullptr;

    std::vector<std::unique_ptr<DetectionSource>> sources_;
    std::map<std::string, DetectionPolicy> policies_;
    Presenter presenter_;

    helix::AsyncLifetimeGuard lifetime_;
    bool connect_observer_registered_ = false;
};

} // namespace helix::detection
