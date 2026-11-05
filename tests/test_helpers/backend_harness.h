// tests/test_helpers/backend_harness.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reaches an AMS backend the way a printer does: over the Moonraker client.
#pragma once

#include "../fake_moonraker_client.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

/// A FakeMoonrakerClient and a production MoonrakerAPI over it. A backend given
/// these and started is subscribed exactly as on a printer, so a status frame
/// reaches it through its own subscription and the UI thread hop that follows.
///
/// Requests stay unanswered unless respond_to() names their method, except the
/// database read a backend's start blocks on, which reads as an empty namespace.
class BackendHarness {
  public:
    BackendHarness() : api_(client_, get_printer_state()) {
        respond_to("server.database.get_item", nlohmann::json{{"value", nlohmann::json::object()}});
    }

    IMoonrakerAPI* api() {
        return &api_;
    }
    IMoonrakerClient* client() {
        return &client_;
    }
    FakeMoonrakerClient& fake_client() {
        return client_;
    }

    /// Delivers @p frame as a notify_status_update and runs one pass of the UI
    /// queue, which is the backend's handling of it; whatever that handling
    /// queues in turn waits for the test's own drain. A frame already in the
    /// notify envelope goes as it is.
    void fire_status(const nlohmann::json& frame) {
        const nlohmann::json notification =
            frame.contains("params")
                ? frame
                : nlohmann::json{{"method", "notify_status_update"},
                                 {"params", nlohmann::json::array({frame, 1.0})}};
        REQUIRE(client_.fire_notification("notify_status_update", notification));
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Answer every later request for @p method with @p result.
    void respond_to(const std::string& method, nlohmann::json result) {
        client_.responses[method] = std::move(result);
    }

  private:
    struct AnsweringClient : FakeMoonrakerClient {
        std::map<std::string, nlohmann::json> responses;

        using FakeMoonrakerClient::send_jsonrpc;
        RequestId send_jsonrpc(const std::string& method, const nlohmann::json& params,
                               SharedCallback<void(const nlohmann::json&)> cb) override {
            const RequestId id = FakeMoonrakerClient::send_jsonrpc(method, params, cb);
            answer(method, cb);
            return id;
        }
        RequestId send_jsonrpc(const std::string& method, const nlohmann::json& params,
                               SharedCallback<void(const nlohmann::json&)> success_cb,
                               SharedCallback<void(const MoonrakerError&)> error_cb,
                               uint32_t timeout_ms, bool silent,
                               std::optional<rpc_error_policy::CallerIntent> intent) override {
            const RequestId id = FakeMoonrakerClient::send_jsonrpc(
                method, params, success_cb, std::move(error_cb), timeout_ms, silent, intent);
            answer(method, success_cb);
            return id;
        }

      private:
        void answer(const std::string& method,
                    const SharedCallback<void(const nlohmann::json&)>& cb) {
            auto it = responses.find(method);
            if (it != responses.end() && cb) {
                cb(nlohmann::json{{"result", it->second}});
            }
        }
    };

    AnsweringClient client_;
    MoonrakerAPI api_;
};

/// Delivers @p frame to whatever subscribes on a client the test already has, a
/// mock printer's for instance, and runs one pass of the UI queue.
inline void fire_status(IMoonrakerClient& client, const nlohmann::json& frame) {
    client.dispatch_status_update(frame);
    helix::ui::UpdateQueue::instance().drain();
}

/// Holds the harness ahead of the backend base it is handed to.
struct HarnessHolder {
    BackendHarness harness;
};

/// @p Backend built over its own harness, not yet started.
template <typename Backend> class WiredBackend : private HarnessHolder, public Backend {
  public:
    WiredBackend() : Backend(harness.api(), harness.client()) {}
    /// For a test whose API is a mock holding its data; the wire stays the harness's.
    explicit WiredBackend(IMoonrakerAPI* api) : Backend(api, harness.client()) {}
    /// What the last frame's handling queued runs against the backend it was queued for.
    ~WiredBackend() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    BackendHarness& wire() {
        return harness;
    }
    void fire_status(const nlohmann::json& frame) {
        harness.fire_status(frame);
    }
};

/// A WiredBackend started by construction, for a test nothing registers.
template <typename Backend> class StartedBackend : public WiredBackend<Backend> {
  public:
    template <typename... Args>
    explicit StartedBackend(Args&&... args) : WiredBackend<Backend>(std::forward<Args>(args)...) {
        REQUIRE(this->start().success());
    }
};

/// A WiredBackend that AmsState owns and has stamped, then started: the order a
/// printer brings one up, which is what lets a start file lane records.
template <typename Backend>
class RegisteredWired : public RegisteredBackend<WiredBackend<Backend>> {
  public:
    template <typename... Args>
    explicit RegisteredWired(Args&&... args)
        : RegisteredBackend<WiredBackend<Backend>>(std::forward<Args>(args)...) {
        REQUIRE((**this).start().success());
    }
};

} // namespace helix::test
