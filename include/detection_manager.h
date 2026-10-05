// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "detection_source.h"
#include "lvgl/lvgl.h"

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

/// What the UI should do with a detection, combining the per-source policy
/// with the global settings (enabled / pause on detect).
enum class DetectionResponse {
    Suppressed,      ///< detection off or source policy Off: show nothing
    WarnOnly,        ///< notify without pausing (or the source paused itself)
    PauseAndRespond, ///< pause the print (unless already paused) and respond
};

/// Registry of detection sources plus per-source policy and the UI presenter hook.
///
/// Sources route their (already main-thread-marshaled) events into the manager,
/// which consults the source's policy and invokes the presenter when warranted.
class DetectionManager {
  public:
    static DetectionManager& instance();

    /// Wire up Moonraker/PrinterState deps and register a post-connect hook that
    /// (re)probes detector capabilities once the WebSocket is up. The probe is NOT
    /// run here — at init() time (PrinterSession::init_panel_subjects) the WebSocket has
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
    bool any_pause_applicable() const;

    /// Classify what a detection from a source with policy @p p should do,
    /// from the global detection settings. Sources never decide this.
    DetectionResponse response_for(DetectionPolicy p) const;

    /// "Some detection source is capable on this printer" (integer 0/1),
    /// bindable from XML to gate detection UI.
    lv_subject_t* subject_detection_available() {
        return &detection_available_subject_;
    }

    /// "Some capable source does NOT pause the print by itself" (integer
    /// 0/1): the pause-on-detect setting governs those, so the row shows.
    /// Firmware-pausing sources escalate warn-only to the response modal
    /// instead, so for them the setting is inert and the row hides.
    lv_subject_t* subject_detection_pause_applicable() {
        return &detection_pause_applicable_subject_;
    }

    /// Whether the named registered source exposes tuning
    /// (DetectionSource::can_tune()). Lets generic presenters offer a Tune
    /// button without naming any vendor's source id.
    bool source_can_tune(const std::string& source_id) const;

    /// Ask the named registered source to lower its sensitivity. No-op when
    /// the id is unknown or the source cannot tune.
    void tune_source(const std::string& source_id);

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

    /// Init + register the availability subject on first use; re-init would
    /// wipe observers bound since the first call, so only the flag gates it.
    void ensure_availability_subject();

    /// Push any_available() / pause-applicability into the subjects and run
    /// the one-time preference seed when a capable source exists.
    void update_availability();

    /// One-time copy of the printer's stored detection preference into the
    /// settings, the first start where a capable source exists.
    void maybe_seed_settings();

    lv_subject_t detection_available_subject_{};
    lv_subject_t detection_pause_applicable_subject_{};
    bool availability_subject_ready_ = false;

    helix::IMoonrakerClient* client_ = nullptr;
    helix::PrinterState* state_ = nullptr;

    std::vector<std::unique_ptr<DetectionSource>> sources_;
    std::map<std::string, DetectionPolicy> policies_;
    Presenter presenter_;

    helix::AsyncLifetimeGuard lifetime_;
    bool connect_observer_registered_ = false;
};

} // namespace helix::detection
