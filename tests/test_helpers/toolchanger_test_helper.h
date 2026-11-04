// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_shared_callback.h"

// Real backend with gcode captured. client_ is null, so ensure_homed_then()
// routes straight to execute_gcode().

#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "json_fwd.h"
#include "moonraker_error.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace helix::test {

using json = nlohmann::json;

/// A tool-changer backend of type @p Base with its gcode captured instead of
/// sent. The ack callback never runs, so a dispatched swap stays in flight.
template <typename Base> class GcodeCaptureBackend : public Base {
  public:
    template <typename... Args>
    explicit GcodeCaptureBackend(Args&&... args)
        : Base(nullptr, nullptr, std::forward<Args>(args)...) {
        this->running_ = true;
    }

    ~GcodeCaptureBackend() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        if (fail_gcode_) {
            return AmsErrorHelper::command_failed(gcode, "simulated send failure");
        }
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        sent_.push_back(gcode);
        (void)on_complete;
        if (fail_gcode_) {
            return AmsErrorHelper::command_failed(gcode, "simulated send failure");
        }
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool silent) override {
        sent_.push_back(gcode);
        (void)on_complete;
        (void)silent;
        last_on_error_ = std::move(on_error);
        if (fail_gcode_) {
            return AmsErrorHelper::command_failed(gcode, "simulated send failure");
        }
        return AmsErrorHelper::success();
    }

    /// Answer the last send that asked for an error callback with Klipper's
    /// rejection, then run what the backend queued for the main thread.
    void reject_last_send(MoonrakerErrorType type, const std::string& message) {
        if (!last_on_error_) {
            return;
        }
        MoonrakerError err;
        err.type = type;
        err.message = message;
        last_on_error_(err);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Make every subsequent execute_gcode() fail, to exercise the error legs.
    void set_fail_gcode(bool fail) {
        fail_gcode_ = fail;
    }

    void feed(const json& status) {
        this->handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

    /// Every gcode any execute_gcode virtual sent. Public, and shared with
    /// every derived fixture: a redeclared copy in a subclass silently
    /// collects only the overloads that subclass overrides, and the tests
    /// read an empty history of everything else.
    std::vector<std::string> sent_;
    bool fail_gcode_ = false;
    helix::SharedCallback<void(const MoonrakerError&)> last_on_error_;
};

class ToolChangerHelper : public GcodeCaptureBackend<AmsBackendToolChanger> {
  public:
    explicit ToolChangerHelper(int tool_count) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
    }

    void feed_status(const char* status, int tool_number) {
        feed(json{{"toolchanger", {{"status", status}, {"tool_number", tool_number}}}});
    }
};

} // namespace helix::test
