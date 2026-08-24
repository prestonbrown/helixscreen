// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_control_server.h"

#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "demo_overlays.h"
#include "display_settings_manager.h"
#include "http_executor.h"
#include "input_settings_manager.h"
#include "logging_init.h"
#include "mock_scenarios.h"
#include "panel_factory.h"
#include "printer_state.h"
#include "remote_client.h"
#include "remote_pointer.h"
#include "screenshot.h"
#include "subject_debug_registry.h"
#include "widget_resolution.h"

// LVGL XML subject lookup
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_component_private.h"
#include "http_transport.h"
#include "unix_socket_transport.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace helix {

// Panel name ↔ ID mappings, derived dynamically from the panel registry
// (PanelFactory::PANEL_NAMES) so new panels are picked up with no edits here.
// The registry stores widget names ("home_panel"); the CLI protocol uses short
// names ("home", "print-select") — strip a trailing "_panel" and map '_' -> '-'.
static std::string panel_short_name(int idx) {
    std::string n = PanelFactory::PANEL_NAMES[idx];
    static const std::string SUFFIX = "_panel";
    if (n.size() > SUFFIX.size() &&
        n.compare(n.size() - SUFFIX.size(), SUFFIX.size(), SUFFIX) == 0) {
        n.erase(n.size() - SUFFIX.size());
    }
    std::replace(n.begin(), n.end(), '_', '-');
    return n;
}

static std::string panel_id_to_name(helix::PanelId id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= UI_PANEL_COUNT) {
        return "unknown";
    }
    return panel_short_name(idx);
}

static std::optional<helix::PanelId> name_to_panel_id(const std::string& name) {
    std::string norm = name;
    std::replace(norm.begin(), norm.end(), '_', '-');
    for (int i = 0; i < UI_PANEL_COUNT; ++i) {
        if (name == PanelFactory::PANEL_NAMES[i] || norm == panel_short_name(i)) {
            return static_cast<helix::PanelId>(i);
        }
    }
    return std::nullopt;
}

// Reset the LVGL inactivity timer as if the user touched the screen. Since
// helixctl injects synthetic events (not through the input device), the idle
// timer never resets on its own, so DisplayManager would start the screensaver
// mid-drive. Calling this on every interaction both prevents that and makes
// DisplayManager close an already-active screensaver on its next tick. UI thread.
static void wake_display() {
    lv_display_trigger_activity(nullptr);
}

RemoteControlServer& RemoteControlServer::instance() {
    static RemoteControlServer instance;
    return instance;
}

RemoteControlServer::~RemoteControlServer() {
    stop();
}

std::string resolve_socket_path(const std::string& override_path) {
    if (!override_path.empty()) {
        return override_path; // Explicit --remote-socket always wins.
    }

    const char* xdg_runtime = getenv("XDG_RUNTIME_DIR");
    const std::string dir =
        (xdg_runtime && xdg_runtime[0] != '\0') ? std::string(xdg_runtime) : std::string("/tmp");
    const std::string well_known = dir + "/helixscreen-control.sock";

    // Clear sockets left by instances that died without teardown before deciding
    // anything, so a run of crashed sessions cannot litter the directory forever.
    UnixSocketTransport::sweep_stale_instances(dir);

    // The well-known path is what a bare `helix-screen ctl` looks for, so the first
    // instance should own it — on a device there is only ever one. A second instance
    // (two dev sessions, or an accidental double start) takes a pid-suffixed path
    // instead of stealing it, so both stay reachable and neither is silently
    // hijacked. The client discovers these; see remote_client.cpp.
    if (!UnixSocketTransport::path_is_live(well_known)) {
        return well_known;
    }

    std::string fallback = dir + "/helixscreen-control-" + std::to_string(getpid()) + ".sock";
    spdlog::warn("[RemoteControl] {} is in use by another instance; using {}", well_known,
                 fallback);
    return fallback;
}

bool RemoteControlServer::start(const RemoteConfig& config) {
    if (running_.load()) {
        spdlog::warn("[RemoteControl] Server already running");
        return false;
    }

    switch (config.transport) {
    case RemoteConfig::Transport::Http:
        transport_ = std::make_unique<HttpTransport>(config.http_bind, config.http_port);
        break;
    case RemoteConfig::Transport::UnixSocket:
    default:
        transport_ = std::make_unique<UnixSocketTransport>(config.socket_path);
        break;
    }

    register_builtin_handlers();

    if (!transport_->start([this](const std::string& line) { return process_request(line); })) {
        transport_.reset();
        handlers_.clear();
        return false;
    }

    running_.store(true);
    spdlog::info("[RemoteControl] Server started on {}", transport_->endpoint());
    return true;
}

void RemoteControlServer::stop() {
    if (!running_.load()) {
        return;
    }

    spdlog::info("[RemoteControl] Stopping server...");
    running_.store(false);

    if (transport_) {
        transport_->stop();
        transport_.reset();
    }

    handlers_.clear();
    spdlog::info("[RemoteControl] Server stopped");
}

void RemoteControlServer::register_handler(const std::string& method, CommandHandler handler) {
    handlers_[method] = std::move(handler);
}

std::string RemoteControlServer::process_request(const std::string& request_line) {
    nlohmann::json response;

    try {
        auto request = nlohmann::json::parse(request_line);

        // Validate JSON-RPC 2.0
        if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
            response = {
                {"jsonrpc", "2.0"},
                {"error", {{"code", -32600}, {"message", "Invalid Request: missing jsonrpc 2.0"}}},
                {"id", nullptr}};
            return response.dump();
        }

        if (!request.contains("method") || !request["method"].is_string()) {
            response = {
                {"jsonrpc", "2.0"},
                {"error", {{"code", -32600}, {"message", "Invalid Request: missing method"}}},
                {"id", request.value("id", nlohmann::json(nullptr))}};
            return response.dump();
        }

        std::string method = request["method"];
        nlohmann::json params = request.value("params", nlohmann::json::object());
        nlohmann::json id = request.value("id", nlohmann::json(nullptr));

        spdlog::debug("[RemoteControl] Request: method={}, id={}", method, id.dump());

        response = dispatch(method, params, id);

    } catch (const nlohmann::json::parse_error& e) {
        response = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}},
            {"id", nullptr}};
    } catch (const std::exception& e) {
        response = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}}},
            {"id", nullptr}};
    }

    return response.dump();
}

nlohmann::json RemoteControlServer::dispatch(const std::string& method,
                                             const nlohmann::json& params,
                                             const nlohmann::json& id) {
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        return {{"jsonrpc", "2.0"},
                {"error", {{"code", -32601}, {"message", "Method not found: " + method}}},
                {"id", id}};
    }

    try {
        nlohmann::json result = it->second(params);
        return {{"jsonrpc", "2.0"}, {"result", result}, {"id", id}};
    } catch (const std::exception& e) {
        return {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32603}, {"message", std::string("Handler error: ") + e.what()}}},
            {"id", id}};
    }
}

nlohmann::json RemoteControlServer::execute_on_ui_thread(std::function<nlohmann::json()> fn) {
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    helix::ui::queue_update([promise, fn = std::move(fn)]() {
        try {
            promise->set_value(fn());
        } catch (const std::exception& e) {
            promise->set_exception(std::current_exception());
        }
    });

    // Wait with timeout, polling running_ so a concurrent stop() unblocks this
    // promptly. This runs on the transport accept thread; at app teardown the
    // main loop has already stopped servicing the update queue, so the queued fn
    // would otherwise never resolve and stop()'s join() would stall for the full
    // timeout. stop() clears running_ before joining, so bail as soon as we see
    // it — the queued fn is dropped when the queue shuts down.
    for (int i = 0; i < 100; ++i) { // 100 * 100ms = 10s
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            return future.get();
        }
        if (!running_.load()) {
            throw std::runtime_error("remote server shutting down");
        }
    }
    throw std::runtime_error("UI thread timeout (10s)");
}

// Forward declaration: the widget-resolution helpers live further down in this
// file, alongside the other widget-interaction handlers, but handle_screenshot
// (below) needs to resolve its --target before that point.
static lv_obj_t* resolve_widget(const nlohmann::json& params);

// =============================================================================
// Built-in handlers
// =============================================================================

void RemoteControlServer::register_builtin_handlers() {
    handlers_["ping"] = [this](const nlohmann::json& p) { return handle_ping(p); };
    handlers_["navigate"] = [this](const nlohmann::json& p) { return handle_navigate(p); };
    handlers_["go_back"] = [this](const nlohmann::json& p) { return handle_go_back(p); };
    handlers_["list_panels"] = [this](const nlohmann::json& p) { return handle_list_panels(p); };
    handlers_["list_components"] = [this](const nlohmann::json& p) {
        return handle_list_components(p);
    };
    handlers_["list_callbacks"] = [this](const nlohmann::json& p) {
        return handle_list_callbacks(p);
    };
    handlers_["get_current"] = [this](const nlohmann::json& p) { return handle_get_current(p); };
    handlers_["resolve"] = [this](const nlohmann::json& p) { return handle_resolve(p); };
    handlers_["screenshot"] = [this](const nlohmann::json& p) { return handle_screenshot(p); };
    handlers_["status"] = [this](const nlohmann::json& p) { return handle_status(p); };
    handlers_["demo"] = [this](const nlohmann::json& p) { return handle_demo(p); };

    // Phase 2: Subject get/set/list/wait_for
    handlers_["get"] = [this](const nlohmann::json& p) { return handle_get_subject(p); };
    handlers_["set"] = [this](const nlohmann::json& p) { return handle_set_subject(p); };
    handlers_["list_subjects"] = [this](const nlohmann::json& p) {
        return handle_list_subjects(p);
    };
    handlers_["wait_for"] = [this](const nlohmann::json& p) { return handle_wait_for(p); };
    handlers_["wait_idle"] = [this](const nlohmann::json& p) { return handle_wait_idle(p); };
    handlers_["freeze"] = [this](const nlohmann::json& p) { return handle_freeze(p); };
    handlers_["unfreeze"] = [this](const nlohmann::json& p) { return handle_unfreeze(p); };

    // Phase 3: Widget interaction + scenarios
    handlers_["click"] = [this](const nlohmann::json& p) { return handle_click(p); };
    handlers_["set_widget_value"] = [this](const nlohmann::json& p) {
        return handle_set_widget_value(p);
    };
    handlers_["scenario"] = [this](const nlohmann::json& p) { return handle_scenario(p); };
    handlers_["list_scenarios"] = [this](const nlohmann::json& p) {
        return handle_list_scenarios(p);
    };

    // Introspection
    handlers_["describe_screen"] = [this](const nlohmann::json& p) {
        return handle_describe_screen(p);
    };
    handlers_["scroll"] = [this](const nlohmann::json& p) { return handle_scroll(p); };
    handlers_["focus"] = [this](const nlohmann::json& p) { return handle_focus(p); };
    handlers_["pointer_press"] = [this](const nlohmann::json& p) {
        return handle_pointer_press(p);
    };
    handlers_["pointer_move"] = [this](const nlohmann::json& p) { return handle_pointer_move(p); };
    handlers_["pointer_long_press"] = [this](const nlohmann::json& p) {
        return handle_pointer_long_press(p);
    };
    handlers_["pointer_release"] = [this](const nlohmann::json& p) {
        return handle_pointer_release(p);
    };
    handlers_["geom"] = [this](const nlohmann::json& p) { return handle_geom(p); };
    handlers_["text"] = [this](const nlohmann::json& p) { return handle_text(p); };
    handlers_["state"] = [this](const nlohmann::json& p) { return handle_state(p); };
    handlers_["set_text"] = [this](const nlohmann::json& p) { return handle_set_text(p); };
    handlers_["get_const"] = [this](const nlohmann::json& p) { return handle_get_const(p); };
    handlers_["wake"] = [this](const nlohmann::json& p) { return handle_wake(p); };

    // Lifecycle + diagnostics
    handlers_["shutdown"] = [this](const nlohmann::json& p) { return handle_shutdown(p); };
    handlers_["reset"] = [this](const nlohmann::json& p) { return handle_reset(p); };
    handlers_["log"] = [this](const nlohmann::json& p) { return handle_log(p); };
}

nlohmann::json RemoteControlServer::handle_ping(const nlohmann::json& /*params*/) {
    return "pong";
}

nlohmann::json RemoteControlServer::handle_navigate(const nlohmann::json& params) {
    if (!params.contains("panel") || !params["panel"].is_string()) {
        throw std::invalid_argument("Missing required parameter: panel");
    }

    std::string target = params["panel"];

    // 1. Base panel -> take the same path a navbar tap takes.
    //
    // NOT set_active(), which deliberately preserves the overlay stack so the
    // base panel can be swapped underneath an open overlay. Driving it that way
    // left the overlay (and its opaque snapshot backdrop) covering the screen
    // while reporting a successful navigation — every screenshot after it came
    // back identical, which reads as broken rendering rather than "you are
    // still inside an overlay". A finger on the navbar clears the stack; so
    // does this now.
    auto panel_id = name_to_panel_id(target);
    if (panel_id) {
        return execute_on_ui_thread([panel_id]() -> nlohmann::json {
            wake_display();
            using PanelRequest = NavigationManager::PanelRequest;
            // Inline: this lambda already runs inside an UpdateQueue callback,
            // which is the context switch_to_panel_impl() is written for. That
            // keeps navigate synchronous — `current` right after it reports the
            // new panel instead of racing a queued switch.
            auto result = NavigationManager::instance().request_panel(
                *panel_id, NavigationManager::SwitchDispatch::Inline);

            // A declined request must not answer like a successful one. The
            // gating is silent for a finger (the button simply does nothing),
            // but a script that gets {"navigated_to": ...} back and then
            // screenshots the panel it never reached has no way to tell.
            if (result == PanelRequest::BlockedDisconnected) {
                throw std::runtime_error("Navigation to '" + panel_id_to_name(*panel_id) +
                                         "' blocked: printer not connected");
            }
            if (result == PanelRequest::BlockedKlippyNotReady) {
                throw std::runtime_error("Navigation to '" + panel_id_to_name(*panel_id) +
                                         "' blocked: Klipper not ready");
            }

            return {{"navigated_to", panel_id_to_name(*panel_id)},
                    {"kind", "panel"},
                    {"switched", result == PanelRequest::Switched},
                    {"home_retapped", result == PanelRequest::HomeRetapped}};
        });
    }

    // 2. Otherwise treat it as a named, clickable widget on the current screen
    //    and click it. This descends into whatever overlay/modal that widget's
    //    real handler opens, running the full production lifecycle
    //    (init_subjects/create/on_activate) -- never a raw lv_xml_create shell.
    //    It is the fs-metaphor "cd into something ls showed you".
    return execute_on_ui_thread([target]() -> nlohmann::json {
        wake_display();
        lv_obj_t* widget = nullptr;
        if (lv_obj_t* screen = lv_screen_active()) {
            widget = lv_obj_find_by_name(screen, target.c_str());
        }
        if (!widget) {
            // Overlays/modals can live on the top layer -- look there too.
            widget = lv_obj_find_by_name(lv_layer_top(), target.c_str());
        }
        if (!widget) {
            throw std::invalid_argument("Not a panel, and no widget named '" + target +
                                        "' on screen");
        }
        if (!lv_obj_has_flag(widget, LV_OBJ_FLAG_CLICKABLE)) {
            throw std::invalid_argument("Widget '" + target + "' is not clickable");
        }
        lv_obj_send_event(widget, LV_EVENT_CLICKED, nullptr);
        return {{"navigated_to", target}, {"kind", "widget"}};
    });
}

nlohmann::json RemoteControlServer::handle_shutdown(const nlohmann::json& /*params*/) {
    // Answer before the main loop tears down: app_request_quit() only sets a
    // flag, so the reply is written and flushed on the way out. Doing this on
    // the UI thread would race the loop we are asking to end.
    spdlog::info("[RemoteControl] shutdown requested");
    app_request_quit();
    return {{"shutting_down", true}};
}

nlohmann::json RemoteControlServer::handle_reset(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        wake_display();

        // Modals first: Modal::hide() is the live-safe dismiss path (marks the
        // stack entry "exiting" synchronously, then always finishes with
        // safe_delete_deferred_raw() -- never a synchronous lv_obj_delete()).
        // ModalStack::clear() looked tempting but is a teardown-only helper
        // (its only caller is Application::shutdown()): it calls lv_obj_delete()
        // directly in a loop, which is exactly the "multiple sync deletions in
        // one UpdateQueue batch" pattern that corrupts LVGL's event list from
        // inside a queued callback (this handler runs via execute_on_ui_thread,
        // itself dispatched through UpdateQueue). empty() only counts
        // non-exiting entries, so it flips to true as soon as every modal has
        // been marked -- no need to wait out the exit animation here.
        int modals_cleared = 0;
        constexpr int MAX_MODAL_DEPTH = 16; // matching MAX_DEPTH's reasoning below
        while (!ModalStack::instance().empty() && modals_cleared < MAX_MODAL_DEPTH) {
            lv_obj_t* top = Modal::get_top();
            if (!top) {
                break;
            }
            Modal::hide(top);
            modals_cleared++;
        }
        if (modals_cleared == MAX_MODAL_DEPTH && !ModalStack::instance().empty()) {
            spdlog::warn("[RemoteControlServer] reset: modal stack still non-empty after "
                         "{} dismissals -- hit the safety cap, something isn't draining",
                         MAX_MODAL_DEPTH);
        }

        // Toasts: ToastManager::hide() dismisses every visible toast (also via
        // safe_delete_deferred_raw internally). There is no public exact count --
        // visible_count() is private and the brief for this task says not to add
        // a ToastManager API here -- so toasts_cleared is presence-only (0 or 1),
        // not an exact count. See HELIXCTL.md for the caveat.
        auto& toasts = ToastManager::instance();
        int toasts_cleared = toasts.is_visible() ? 1 : 0;
        toasts.hide();

        // Overlays: NavigationManager::go_back() defers its actual work via
        // queue_update() -- even called from the UI thread, it does not pop
        // panel_stack_ synchronously, it enqueues a callback for the *next*
        // UpdateQueue::process_pending() tick. So overlay_stack_names() must be
        // read exactly once, before any go_back() call, to get the true depth --
        // rereading it in a loop condition would never observe a decrease within
        // this same callback and would just spin to MAX_DEPTH every time.
        // Bounded rather than unbounded: a nav stack that will not drain is a
        // bug, and spinning forever here would hang the UI thread.
        auto& nav = NavigationManager::instance();
        constexpr int MAX_DEPTH = 32;
        int actual_depth = static_cast<int>(nav.overlay_stack_names().size());
        int overlays_popped = std::min(actual_depth, MAX_DEPTH);
        if (actual_depth > MAX_DEPTH) {
            spdlog::warn("[RemoteControlServer] reset: overlay stack depth {} exceeds the "
                         "safety cap of {} -- popping {} and leaving the rest, something "
                         "isn't draining",
                         actual_depth, MAX_DEPTH, MAX_DEPTH);
        }
        for (int i = 0; i < overlays_popped; ++i) {
            nav.go_back();
        }

        nav.set_active(helix::PanelId::Home);

        return {{"panel", panel_id_to_name(nav.get_active())},
                {"overlays_popped", overlays_popped},
                {"modals_cleared", modals_cleared},
                {"toasts_cleared", toasts_cleared}};
    });
}

nlohmann::json RemoteControlServer::handle_log(const nlohmann::json& params) {
    // Serve the in-memory ring buffer the debug bundle already fills, so a
    // scripted run can read the app's own log without tee-ing it to a file.
    int lines = 50;
    if (params.contains("lines") && params["lines"].is_number_integer()) {
        lines = params["lines"].get<int>();
    }
    if (lines <= 0) {
        lines = 50;
    }

    std::string tail = helix::logging::tail_ring_buffer(lines);
    nlohmann::json arr = nlohmann::json::array();
    size_t pos = 0;
    while (pos <= tail.size()) {
        size_t nl = tail.find('\n', pos);
        std::string line = tail.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty()) {
            arr.push_back(line);
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return {{"lines", arr},
            {"count", arr.size()},
            {"capacity", helix::logging::ring_buffer_capacity()}};
}

nlohmann::json RemoteControlServer::handle_go_back(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        bool result = NavigationManager::instance().go_back();
        return {{"success", result}};
    });
}

nlohmann::json RemoteControlServer::handle_list_panels(const nlohmann::json& /*params*/) {
    nlohmann::json panels = nlohmann::json::array();
    for (int i = 0; i < UI_PANEL_COUNT; ++i) {
        panels.push_back(panel_short_name(i));
    }
    return {{"panels", panels}};
}

nlohmann::json RemoteControlServer::handle_list_components(const nlohmann::json& /*params*/) {
    // Enumerate the live XML component registry, rather than any hardcoded list.
    // Unlike list_panels (the fixed set of PanelId-bound base panels), this
    // surfaces every registered component -- panels, overlays, modals, cards,
    // rows -- so the full navigable/introspectable surface is discoverable at
    // runtime. Runs on the UI thread because the hot-reload poller can register
    // components concurrently.
    return execute_on_ui_thread([]() -> nlohmann::json {
        std::vector<std::string> names;
        lv_xml_component_foreach(
            [](const char* name, void* ud) {
                auto* out = static_cast<std::vector<std::string>*>(ud);
                if (name && std::strcmp(name, "globals") != 0) {
                    out->emplace_back(name);
                }
            },
            &names);
        std::sort(names.begin(), names.end());
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : names) {
            arr.push_back(n);
        }
        return {{"components", arr}};
    });
}

nlohmann::json RemoteControlServer::handle_list_callbacks(const nlohmann::json& /*params*/) {
    // Enumerate the event callbacks registered in the global scope -- where
    // overlay/modal open-handlers, button callbacks, etc. register via
    // lv_xml_register_event_cb(nullptr, ...). Read-only discovery surface: it
    // lists names, it does NOT fire anything (firing an open-handler with a
    // synthetic event needs the arg-ignore allowlist, a separate follow-on).
    // Runs on the UI thread; hot-reload can register callbacks concurrently.
    return execute_on_ui_thread([]() -> nlohmann::json {
        std::vector<std::string> names;
        lv_xml_event_cb_foreach(
            nullptr, // NULL -> the "globals" scope
            [](const char* name, lv_event_cb_t /*cb*/, void* ud) {
                auto* out = static_cast<std::vector<std::string>*>(ud);
                if (name) {
                    out->emplace_back(name);
                }
            },
            &names);
        std::sort(names.begin(), names.end());
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : names) {
            arr.push_back(n);
        }
        return {{"callbacks", arr}};
    });
}

static std::string topmost_layer(); // defined with the other locator helpers

nlohmann::json RemoteControlServer::handle_get_current(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        auto& nav = NavigationManager::instance();
        std::string current_panel = panel_id_to_name(nav.get_active());
        // root_path is where a client's working directory sits when it has not
        // cd'd anywhere: the frontmost thing on screen. The client also watches
        // it for change — when a navigate/click swaps the active root out, a cwd
        // pointing into the old subtree is stale and gets dropped.
        return {{"panel", current_panel},
                {"overlays", nav.overlay_stack_names()},
                {"root_path", topmost_layer()}};
    });
}

// Build resolve_widget()-compatible params from a plain locator string, the
// same rule the ctl client's target_param() applies before it ever reaches
// the wire for click/set_value/scroll: "@s/3/1" (or a bare "s/3/1") addresses
// a describe_screen path, anything else is a widget name.
static nlohmann::json widget_target_params(const std::string& target) {
    if (!target.empty() && target[0] == '@') {
        return {{"path", target.substr(1)}};
    }
    if (helix::is_bare_path(target)) {
        return {{"path", target}};
    }
    return {{"name", target}};
}

nlohmann::json RemoteControlServer::handle_screenshot(const nlohmann::json& params) {
    // Optional destination. A ".png" suffix selects PNG encoding; the default
    // (no path) stays a timestamped BMP in the runtime dir.
    std::string out_path = params.value("path", "");
    std::string target = params.value("target", "");
    bool stable = params.value("stable", false);

    // Resolved fresh on the UI thread every time it's needed (once per
    // stability sample, then once more for the final capture) rather than
    // resolved once up front — a widget pointer must never cross the
    // transport/UI thread boundary or outlive the UI-thread call that found it.
    // The caller's working directory scopes --target the same way it scopes
    // every other widget command; without this, `cd`-ing to a card and then
    // cropping to a name inside it would search the whole screen instead.
    const std::string scope = params.value("scope", "");
    auto resolve_crop = [target, scope]() -> lv_obj_t* {
        if (target.empty()) {
            return nullptr;
        }
        nlohmann::json tp = widget_target_params(target);
        if (!scope.empty()) {
            tp["scope"] = scope;
        }
        lv_obj_t* w = resolve_widget(tp);
        if (!w) {
            throw std::invalid_argument("Widget not found: " + target);
        }
        return w;
    };

    // Stability is sampled across frames, so this loop must live on the
    // transport thread and hop to the UI thread per sample — a loop *on* the
    // UI thread would block the very redraws it is waiting to observe.
    int stable_frames = 0;
    if (stable) {
        constexpr int REQUIRED = 3;
        constexpr int MAX_SAMPLES = 180; // ~3s at 16ms
        uint64_t last = 0;
        int run = 0;
        for (int i = 0; i < MAX_SAMPLES; i++) {
            uint64_t h = execute_on_ui_thread([resolve_crop]() -> nlohmann::json {
                             lv_obj_t* crop = resolve_crop();
                             helix::CapturedFrame f;
                             if (!helix::capture_frame(f, crop)) {
                                 throw std::runtime_error("Frame capture failed");
                             }
                             return helix::frame_hash(f);
                         }).get<uint64_t>();

            run = (i > 0 && h == last) ? run + 1 : 1;
            last = h;
            if (run >= REQUIRED) {
                stable_frames = run;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (stable_frames < REQUIRED) {
            throw std::runtime_error(
                "Screen never stabilized: no " + std::to_string(REQUIRED) +
                " identical consecutive frames within 3s. Try `freeze` first.");
        }
    }

    return execute_on_ui_thread([out_path, resolve_crop, stable_frames]() -> nlohmann::json {
        lv_obj_t* crop = resolve_crop();
        helix::CapturedFrame f;
        if (!helix::capture_frame(f, crop)) {
            throw std::runtime_error("Frame capture failed");
        }
        std::string written = helix::write_frame(f, out_path);
        if (written.empty()) {
            throw std::runtime_error("Screenshot failed" +
                                     (out_path.empty() ? std::string() : ": " + out_path));
        }
        // Report where it actually landed — callers scripting a capture should
        // never have to guess the filename.
        return {{"saved", true},
                {"path", written},
                {"w", f.width},
                {"h", f.height},
                {"stable_frames", stable_frames}};
    });
}

nlohmann::json RemoteControlServer::handle_demo(const nlohmann::json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name");
    }
    std::string name = params["name"];

    return execute_on_ui_thread([name]() -> nlohmann::json {
        wake_display();
        if (!helix::show_demo_overlay(name)) {
            throw std::invalid_argument("Unknown demo overlay: " + name);
        }
        return {{"shown", true}, {"name", name}};
    });
}

nlohmann::json RemoteControlServer::handle_status(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        auto& nav = NavigationManager::instance();
        std::string current_panel = panel_id_to_name(nav.get_active());

        auto& ps = get_printer_state();
        int conn_state = lv_subject_get_int(ps.get_printer_connection_state_subject());
        int klippy_state = lv_subject_get_int(ps.get_klippy_state_subject());

        return {{"panel", current_panel},
                {"overlays", nav.overlay_stack_names()},
                {"connection_state", conn_state},
                {"klippy_state", klippy_state}};
    });
}

// =============================================================================
// Phase 2: Subject handlers
// =============================================================================

static const char* subject_type_name(lv_subject_type_t type) {
    switch (type) {
    case LV_SUBJECT_TYPE_INT:
        return "INT";
    case LV_SUBJECT_TYPE_STRING:
        return "STRING";
    case LV_SUBJECT_TYPE_FLOAT:
        return "FLOAT";
    case LV_SUBJECT_TYPE_POINTER:
        return "POINTER";
    case LV_SUBJECT_TYPE_COLOR:
        return "COLOR";
    case LV_SUBJECT_TYPE_GROUP:
        return "GROUP";
    default:
        return "UNKNOWN";
    }
}

// Look up a subject by name using the LVGL XML subject registry (which has all subjects).
// Must be called on the UI thread.
static lv_subject_t* find_subject_by_name(const std::string& name) {
    return lv_xml_get_subject(nullptr, name.c_str());
}

nlohmann::json RemoteControlServer::handle_get_subject(const nlohmann::json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name");
    }

    std::string name = params["name"];

    return execute_on_ui_thread([name]() -> nlohmann::json {
        lv_subject_t* subject = find_subject_by_name(name);
        if (!subject) {
            throw std::invalid_argument("Unknown subject: " + name);
        }

        auto type = static_cast<lv_subject_type_t>(subject->type);
        if (type == LV_SUBJECT_TYPE_INT) {
            return {{"name", name},
                    {"type", subject_type_name(type)},
                    {"value", lv_subject_get_int(subject)}};
        } else if (type == LV_SUBJECT_TYPE_STRING) {
            const char* s = lv_subject_get_string(subject);
            return {{"name", name}, {"type", subject_type_name(type)}, {"value", s ? s : ""}};
        } else {
            return {
                {"name", name}, {"type", subject_type_name(type)}, {"value", "<unsupported type>"}};
        }
    });
}

nlohmann::json RemoteControlServer::handle_set_subject(const nlohmann::json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name");
    }
    if (!params.contains("value")) {
        throw std::invalid_argument("Missing required parameter: value");
    }

    std::string name = params["name"];
    auto value_json = params["value"];

    return execute_on_ui_thread([name, value_json]() -> nlohmann::json {
        lv_subject_t* subject = find_subject_by_name(name);
        if (!subject) {
            throw std::invalid_argument("Unknown subject: " + name);
        }

        auto type = static_cast<lv_subject_type_t>(subject->type);

        if (type == LV_SUBJECT_TYPE_INT) {
            int value;
            if (value_json.is_number()) {
                value = value_json.get<int>();
            } else if (value_json.is_string()) {
                try {
                    value = std::stoi(value_json.get<std::string>());
                } catch (...) {
                    throw std::invalid_argument("Cannot convert value to int: " +
                                                value_json.dump());
                }
            } else {
                throw std::invalid_argument("Invalid value type for INT subject");
            }
            lv_subject_set_int(subject, value);
            return {{"name", name}, {"set", value}};

        } else if (type == LV_SUBJECT_TYPE_STRING) {
            std::string value = value_json.get<std::string>();
            lv_subject_copy_string(subject, value.c_str());
            return {{"name", name}, {"set", value}};

        } else {
            throw std::invalid_argument(std::string("Cannot set subject of type ") +
                                        subject_type_name(type));
        }
    });
}

nlohmann::json RemoteControlServer::handle_list_subjects(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        nlohmann::json subjects = nlohmann::json::array();

        // Get the global XML scope which holds all globally registered subjects
        auto* scope = lv_xml_component_get_scope("globals");
        if (scope) {
            lv_xml_subject_t* xml_sub =
                static_cast<lv_xml_subject_t*>(_lv_ll_get_head(&scope->subjects_ll));
            while (xml_sub) {
                if (xml_sub->name && xml_sub->subject) {
                    auto type = static_cast<lv_subject_type_t>(xml_sub->subject->type);
                    subjects.push_back(
                        {{"name", xml_sub->name}, {"type", subject_type_name(type)}});
                }
                xml_sub =
                    static_cast<lv_xml_subject_t*>(_lv_ll_get_next(&scope->subjects_ll, xml_sub));
            }
        }

        // Sort by name
        std::sort(subjects.begin(), subjects.end(),
                  [](const nlohmann::json& a, const nlohmann::json& b) {
                      return a["name"].get<std::string>() < b["name"].get<std::string>();
                  });

        return {{"subjects", subjects}, {"count", subjects.size()}};
    });
}

nlohmann::json RemoteControlServer::handle_wait_for(const nlohmann::json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name");
    }
    if (!params.contains("value")) {
        throw std::invalid_argument("Missing required parameter: value");
    }

    std::string name = params["name"];
    int timeout_secs = params.value("timeout", 30);
    std::string target_str;
    if (params["value"].is_string()) {
        target_str = params["value"].get<std::string>();
    } else {
        target_str = params["value"].dump();
    }

    // Shared state between observer callback (UI thread) and waiting thread
    struct WaitState {
        std::mutex mtx;
        std::condition_variable cv;
        bool matched = false;
        bool cancelled = false;
        lv_observer_t* observer = nullptr;
        // Target values (set once during setup, read-only after)
        int target_int = 0;
        std::string target_str;
        bool is_int = false;
        bool parse_as_int = false;
    };
    auto state = std::make_shared<WaitState>();

    // Parse target int value
    state->target_str = target_str;
    try {
        state->target_int = std::stoi(target_str);
        state->parse_as_int = true;
    } catch (...) {
        state->parse_as_int = false;
    }

    // Post observer creation to UI thread — resolves subject and checks value there
    auto setup_promise = std::make_shared<std::promise<bool>>();
    auto setup_future = setup_promise->get_future();

    // Prevent WaitState from being destroyed while observer is alive by capturing
    // shared_ptr in the observer's setup closure (ensures ref count stays > 0)
    helix::ui::queue_update([name, state, setup_promise]() {
        lv_subject_t* subject = find_subject_by_name(name);
        if (!subject) {
            setup_promise->set_value(false);
            return;
        }

        auto type = static_cast<lv_subject_type_t>(subject->type);
        state->is_int = (type == LV_SUBJECT_TYPE_INT);

        if (!state->is_int && type != LV_SUBJECT_TYPE_STRING) {
            setup_promise->set_value(false);
            return;
        }

        // Check current value immediately (on UI thread — safe)
        bool already_matched = false;
        if (state->is_int && state->parse_as_int) {
            already_matched = (lv_subject_get_int(subject) == state->target_int);
        } else if (!state->is_int) {
            already_matched = (std::string(lv_subject_get_string(subject)) == state->target_str);
        }

        if (already_matched) {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->matched = true;
            state->cv.notify_one();
            setup_promise->set_value(true);
            return;
        }

        // Create observer — callback runs on UI thread so reads are safe.
        // Capture shared_ptr to keep WaitState alive as long as observer exists.
        auto captured_state = state;
        state->observer = lv_subject_add_observer(
            subject,
            [](lv_observer_t* obs, lv_subject_t* sub) {
                auto* ws = static_cast<WaitState*>(lv_observer_get_user_data(obs));
                if (!ws || ws->cancelled)
                    return;

                // Read value on UI thread (safe) and check match
                bool match = false;
                if (ws->is_int && ws->parse_as_int) {
                    match = (lv_subject_get_int(sub) == ws->target_int);
                } else if (!ws->is_int) {
                    match = (std::string(lv_subject_get_string(sub)) == ws->target_str);
                }

                std::lock_guard<std::mutex> lock(ws->mtx);
                if (match) {
                    ws->matched = true;
                }
                ws->cv.notify_one();
            },
            state.get());

        setup_promise->set_value(true);
    });

    // Wait for setup
    auto setup_status = setup_future.wait_for(std::chrono::seconds(5));
    if (setup_status == std::future_status::timeout) {
        throw std::runtime_error("Timeout setting up observer");
    }

    if (!setup_future.get()) {
        throw std::invalid_argument("Unknown or unsupported subject: " + name);
    }

    if (!state->matched) {
        // Wait on the condition variable with timeout
        // The observer callback sets matched=true on the UI thread when value matches
        std::unique_lock<std::mutex> lock(state->mtx);
        state->cv.wait_for(lock, std::chrono::seconds(timeout_secs),
                           [&state]() { return state->matched; });
    }

    // Signal cancellation so observer callback becomes a no-op
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->cancelled = true;
    }

    // Remove observer on UI thread
    if (state->observer) {
        auto cleanup_promise = std::make_shared<std::promise<void>>();
        auto cleanup_future = cleanup_promise->get_future();

        helix::ui::queue_update([state, cleanup_promise]() {
            if (state->observer) {
                lv_observer_remove(state->observer);
                state->observer = nullptr;
            }
            cleanup_promise->set_value();
        });

        // Block until cleanup completes — don't let state die with observer still attached
        cleanup_future.wait_for(std::chrono::seconds(10));
    }

    if (state->matched) {
        return {{"matched", true}, {"name", name}, {"value", target_str}};
    } else {
        throw std::runtime_error("Timeout waiting for " + name + " == " + target_str + " (waited " +
                                 std::to_string(timeout_secs) + "s)");
    }
}

// =============================================================================
// Phase 3: Widget interaction + scenario handlers
// =============================================================================

// ---------------------------------------------------------------------------
// Name globbing
// ---------------------------------------------------------------------------

static bool is_glob(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

// Shell-style glob: '*' matches any run (including empty), '?' exactly one char.
// Iterative with backtracking, so a pathological pattern can't blow the stack.
static bool glob_match(const char* pat, const char* str) {
    const char* star = nullptr;
    const char* retry = str;
    while (*str) {
        if (*pat == '?' || *pat == *str) {
            pat++;
            str++;
        } else if (*pat == '*') {
            star = pat++; // remember where the star was
            retry = str;  // and how much of str it had consumed
        } else if (star) {
            pat = star + 1; // backtrack: let the star eat one more char
            str = ++retry;
        } else {
            return false;
        }
    }
    while (*pat == '*') {
        pat++;
    }
    return *pat == '\0';
}

// Every visible widget in a subtree whose name matches the pattern.
//
// A widget the author never named is not nameless: lv_obj_get_name_resolved()
// crafts "<class>_<index>" for it ("lv_label_0"), and lv_obj_find_by_name()
// already matches that form, so it was addressable all along and only this
// walker hid it. Reporting it costs nothing — the crafted name is built on
// demand, never stored, which matters on the ESP32 target where naming every
// widget for real would be paid in heap.
//
// Explicit names are still the better answer for anything a test drives: a
// crafted index counts siblings, so inserting a widget renumbers the ones
// after it.
static void collect_glob_matches(lv_obj_t* parent, const std::string& pattern,
                                 std::vector<lv_obj_t*>& out) {
    if (!parent) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue; // hidden subtree — not on screen, same rule as describe_walk
        }
        char resolved[128];
        lv_obj_get_name_resolved(child, resolved, sizeof(resolved));
        const char* raw = lv_obj_get_name(child);
        const char* name = resolved[0] != '\0' ? resolved : raw;
        if (name && name[0] != '\0' && glob_match(pattern.c_str(), name)) {
            out.push_back(child);
        }
        collect_glob_matches(child, pattern, out);
    }
}

// Match a name pattern within @p scope, or across the active screen and the top
// layer (modals) when no scope is given.
static std::vector<lv_obj_t*> glob_widgets(const std::string& pattern, lv_obj_t* scope = nullptr) {
    std::vector<lv_obj_t*> out;
    if (scope) {
        collect_glob_matches(scope, pattern, out);
        return out;
    }
    collect_glob_matches(lv_screen_active(), pattern, out);
    collect_glob_matches(lv_layer_top(), pattern, out);
    return out;
}

// Drop matches that sit inside another match. `ls row_*` on a settings page hits
// both each row and the `row_icon` nested in it; listing every match as its own
// scope would then emit the icon twice — once as a scope root, once inside its
// parent row's subtree walk — and report a match count that double-counts. Only
// the outermost matches are real scopes; the rest are already covered.
static std::vector<lv_obj_t*> drop_nested_matches(const std::vector<lv_obj_t*>& matches) {
    std::vector<lv_obj_t*> out;
    for (lv_obj_t* o : matches) {
        bool nested = false;
        for (lv_obj_t* p = lv_obj_get_parent(o); p != nullptr; p = lv_obj_get_parent(p)) {
            if (std::find(matches.begin(), matches.end(), p) != matches.end()) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            out.push_back(o);
        }
    }
    return out;
}

// path_of() and resolve_path() live in widget_resolution.{h,cpp} — this object
// is excluded from the test link (it drags in the transports and the toast
// manager), and the locator format is where the interesting mistakes live.

// Resolve a target widget from command params: an explicit "path" wins, else
// fall back to "name" (searched on the active screen then the top layer).
//
// A name containing '*' or '?' is a glob. Acting on a glob requires it to match
// exactly one widget — clicking whichever of several matches happened to come
// first is the kind of thing that silently drives the UI somewhere unintended,
// so ambiguity is an error that names the candidates instead.
// Path of a widget's top-level ancestor — the stacking layer it belongs to.
// "s/19" for the 19th child of the active screen, "t/0" for a modal on the top
// layer. UI thread only.
static std::string layer_of(lv_obj_t* o) {
    if (!o) {
        return {};
    }
    lv_obj_t* cur = o;
    while (lv_obj_t* parent = lv_obj_get_parent(cur)) {
        if (!lv_obj_get_parent(parent)) {
            break; // parent is a root; cur is the top-level container
        }
        cur = parent;
    }
    return path_of(cur);
}

// The layer a click would land on: the highest-index visible child of the top
// layer if a modal is up, else of the active screen.
static std::string topmost_layer() {
    for (lv_obj_t* root : {lv_layer_top(), lv_screen_active()}) {
        if (!root) {
            continue;
        }
        uint32_t count = lv_obj_get_child_count(root);
        for (uint32_t i = count; i > 0; --i) {
            lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i - 1));
            if (child && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                return path_of(child);
            }
        }
    }
    return {};
}

// "settings > display_sound_overlay > theme_preview_overlay" — the breadcrumb
// of what is on screen right now. Echoed in mutating responses so a caller can
// tell at a glance that it is driving the screen it thinks it is (a first-run
// wizard swallows every navigate/click otherwise, and every response still
// reads as success). UI thread only.
static std::string active_screen_label() {
    auto& nav = NavigationManager::instance();
    std::string label = panel_id_to_name(nav.get_active());
    for (const std::string& overlay : nav.overlay_stack_names()) {
        label += " > " + overlay;
    }
    return label;
}

// Collect every visible widget with this exact resolved name. Mirrors
// collect_glob_matches: hidden subtrees are skipped, because a widget the user
// cannot see is never what `click <name>` meant. `state` asks
// include_hidden=true for its retry: asking "is it hidden?" requires finding
// the hidden widget first.
static void collect_by_name(lv_obj_t* parent, const std::string& name, std::vector<lv_obj_t*>& out,
                            bool include_hidden = false) {
    if (!parent) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child || (!include_hidden && lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))) {
            continue;
        }
        // Unnamed widgets match on LVGL's crafted "<class>_<index>" — see the
        // note on collect_glob_matches.
        char resolved[128];
        lv_obj_get_name_resolved(child, resolved, sizeof(resolved));
        const char* raw = lv_obj_get_name(child);
        const char* candidate = resolved[0] != '\0' ? resolved : raw;
        if (candidate && name == candidate) {
            out.push_back(child);
        }
        collect_by_name(child, name, out, include_hidden);
    }
}

// Pick the match the user is actually looking at. Overlays stack as increasing
// child indices under the screen, and the top layer (modals) sits above all of
// them, so rank by (top layer, top-level ancestor index, discovery order) and
// take the highest. Without this, a name that exists in both a background
// overlay and the visible one resolves to the background copy and the click
// becomes a silent no-op.
static lv_obj_t* topmost_visible(const std::vector<lv_obj_t*>& matches) {
    lv_obj_t* best = nullptr;
    long best_key = -1;
    for (size_t i = 0; i < matches.size(); ++i) {
        lv_obj_t* cur = matches[i];
        lv_obj_t* top_ancestor = cur;
        while (lv_obj_t* parent = lv_obj_get_parent(top_ancestor)) {
            if (!lv_obj_get_parent(parent)) {
                break; // parent is the screen/layer root; top_ancestor is its child
            }
            top_ancestor = parent;
        }
        lv_obj_t* root = lv_obj_get_parent(top_ancestor);
        const long layer_rank = (root == lv_layer_top()) ? 1 : 0;
        const long key = (layer_rank << 40) |
                         (static_cast<long>(lv_obj_get_index(top_ancestor)) << 20) |
                         static_cast<long>(i);
        if (key > best_key) {
            best_key = key;
            best = cur;
        }
    }
    return best;
}

// The subtree a search is confined to — the caller's working directory, sent as
// an absolute locator in "scope". Absent or unresolvable means the whole active
// screen plus the top layer, which is what every caller got before `cd` existed.
//
// An unresolvable scope is deliberately not an error: the client's cwd can go
// stale under it (the panel rebuilt, the overlay closed), and falling back to a
// screen-wide search is far better than refusing every subsequent command.
static lv_obj_t* scope_root(const nlohmann::json& params) {
    if (!params.contains("scope") || !params["scope"].is_string()) {
        return nullptr;
    }
    const std::string scope = params["scope"].get<std::string>();
    return scope.empty() ? nullptr : resolve_path(scope);
}

// Every visible widget matching @p name, confined to @p scope when given.
static std::vector<lv_obj_t*> matches_for_name(const std::string& name, lv_obj_t* scope) {
    std::vector<lv_obj_t*> matches;
    if (scope) {
        collect_by_name(scope, name, matches);
        return matches;
    }
    // lv_obj_find_by_name() returns the first depth-first hit, which on a
    // screen with stacked overlays is the one in the *bottom* overlay — a
    // widget the user cannot see. Clicking it looks like a successful no-op.
    // Collect every match instead so the caller can prefer the visible one.
    if (lv_obj_t* screen = lv_screen_active()) {
        collect_by_name(screen, name, matches);
    }
    collect_by_name(lv_layer_top(), name, matches);
    return matches;
}

static lv_obj_t* resolve_widget(const nlohmann::json& params) {
    lv_obj_t* scope = scope_root(params);

    if (params.contains("path") && params["path"].is_string()) {
        // A locator starting "s/" or "t/" is absolute and ignores the scope, so
        // a path pasted out of `ls` means the same thing wherever you are cd'd.
        std::vector<lv_obj_t*> ambiguous;
        lv_obj_t* found = resolve_path(params["path"].get<std::string>(), scope, &ambiguous);
        if (!found && !ambiguous.empty()) {
            std::string msg = "Ambiguous path segment — " + std::to_string(ambiguous.size()) +
                              " siblings share that name: ";
            for (size_t i = 0; i < ambiguous.size() && i < 8; i++) {
                msg += (i ? ", " : "") + path_of(ambiguous[i]);
            }
            msg += " — add an index, e.g. name[0]";
            throw std::invalid_argument(msg);
        }
        return found;
    }

    if (params.contains("name") && params["name"].is_string()) {
        std::string name = params["name"];
        if (is_glob(name)) {
            std::vector<lv_obj_t*> matches = glob_widgets(name, scope);
            if (matches.empty()) {
                return nullptr; // caller reports "not found" with the pattern
            }
            if (matches.size() > 1) {
                std::string msg = "Pattern '" + name + "' matches " +
                                  std::to_string(matches.size()) + " widgets: ";
                for (size_t i = 0; i < matches.size() && i < 8; i++) {
                    char nm[128];
                    lv_obj_get_name_resolved(matches[i], nm, sizeof(nm));
                    msg += (i ? ", " : "") + std::string(nm) + " (@" + path_of(matches[i]) + ")";
                }
                if (matches.size() > 8) {
                    msg += ", ...";
                }
                msg += " — narrow the pattern or use one @path";
                throw std::invalid_argument(msg);
            }
            return matches[0];
        }

        // "toggle[3]" — the 4th match in document order within the search
        // scope. The ordinal only means something relative to a scope, which is
        // exactly what `cd` gives it; unscoped it counts across the whole
        // screen, where an opening overlay can shift it. Both are honest, and
        // the scoped form is the one people use.
        std::string base_name;
        int wanted = -1;
        const bool indexed = parse_indexed_name(name, base_name, wanted);

        std::vector<lv_obj_t*> matches = matches_for_name(indexed ? base_name : name, scope);
        if (matches.empty()) {
            return nullptr;
        }
        if (indexed) {
            if (wanted < 0 || static_cast<size_t>(wanted) >= matches.size()) {
                throw std::invalid_argument("'" + name + "' is out of range — " +
                                            std::to_string(matches.size()) + " match" +
                                            (matches.size() == 1 ? "" : "es") + " here");
            }
            return matches[static_cast<size_t>(wanted)];
        }
        return topmost_visible(matches);
    }
    return nullptr;
}

// Human-readable label for a target (path or name), for result messages.
static std::string target_label(const nlohmann::json& params) {
    if (params.contains("path") && params["path"].is_string()) {
        return params["path"].get<std::string>();
    }
    if (params.contains("name") && params["name"].is_string()) {
        return params["name"].get<std::string>();
    }
    return "?";
}

nlohmann::json RemoteControlServer::handle_wait_idle(const nlohmann::json& params) {
    // Default generous enough for a gcode preview to land, short enough that a
    // wedged test fails inside a coffee break.
    double timeout_s = params.value("timeout", 10.0);

    // Sampled on the UI thread; compared on this (transport) thread. Polling
    // rather than blocking is deliberate — a handler that spun on the UI thread
    // would prevent the very work it is waiting for from running.
    //
    // Animations are deliberately NOT counted here. A real UI has legitimately
    // perpetual animations (a heater icon pulsing while genuinely heating, a
    // fan icon spinning while genuinely spinning), so "zero animations
    // running" is not a reachable idle state in general — it is a property of
    // one screen in one settings configuration, not of the app having
    // finished its work. See the design spec's "Determinism model" for the
    // concrete case that proved this (print_file_detail's loading spinner).
    // Animation-driven pixel churn is the frame-hash screenshot gate's job,
    // which measures whether pixels actually stopped changing instead of
    // inferring it from a counter.
    struct Counters {
        size_t queue = 0;
        size_t http = 0;
        bool idle() const {
            return queue == 0 && http == 0;
        }
    };

    auto sample = [this]() -> Counters {
        auto j = execute_on_ui_thread([]() -> nlohmann::json {
            // Read http before queue (list-init evaluates left-to-right): a
            // worker decrements its inflight count only after the job body
            // returns, and any UI work that job posted via queue_update() is
            // already sitting in pending_ by then. Reading http first means
            // "http already dropped to 0" implies "its queued follow-up work,
            // if any, is already visible in queue" — reading queue first
            // could catch it empty a moment before the worker's own
            // queue_update() call lands, then see http already decremented
            // too, missing both signals in one sample.
            return {{"http", helix::http::HttpExecutor::fast().inflight() +
                                 helix::http::HttpExecutor::slow().inflight()},
                    {"queue", helix::ui::UpdateQueue::instance().pending_count()}};
        });
        return Counters{j["queue"].get<size_t>(), j["http"].get<size_t>()};
    };

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration<double>(timeout_s);
    Counters last{1, 1}; // force at least two samples before declaring idle

    while (true) {
        Counters now = sample();
        // Require idle on two consecutive samples: a single zero reading can
        // land in the gap between one callback finishing and the next being
        // enqueued by the work it just completed.
        if (now.idle() && last.idle()) {
            auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            return {{"idle", true}, {"waited_ms", waited.count()}};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // fmt::format, not std::to_string(double) — the latter renders
            // "0.000000s", unreadable in the log someone reads at 2am.
            throw std::runtime_error(
                fmt::format("wait_idle timed out after {:.1f}s — update_queue={} http={}",
                            timeout_s, now.queue, now.http));
        }
        last = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

nlohmann::json RemoteControlServer::handle_freeze(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([this]() -> nlohmann::json {
        // Stop animations already in flight.
        lv_anim_delete_all();

        // Idempotent: a second freeze() while already frozen must not
        // re-scan and clobber paused_timers_ — every timer would already be
        // paused, so the scan would track none of them and unfreeze() would
        // forget the original set, leaving it paused forever.
        if (frozen_) {
            return {{"frozen", true}, {"timers_paused", static_cast<int>(paused_timers_.size())}};
        }

        // Prevent new animations from starting by flipping the subject
        // directly rather than through DisplaySettingsManager::set_animations_enabled(),
        // which calls Config::save() on every call. freeze is a transient
        // test-mode toggle — persisting it would mean a --remote dev
        // instance killed or crashed between freeze and unfreeze leaves the
        // user's real settings.json with animations permanently disabled.
        // Remember the real value so unfreeze restores it exactly, not "on".
        pre_freeze_animations_enabled_ =
            DisplaySettingsManager::instance().get_animations_enabled();
        lv_subject_set_int(DisplaySettingsManager::instance().subject_animations_enabled(), 0);

        // Pause periodic timers one at a time rather than lv_timer_enable(false):
        // UpdateQueue's processor is itself an lv_timer, and this very handler
        // was dispatched through it via execute_on_ui_thread(). A global disable
        // would stop the queue that delivers unfreeze, wedging the channel.
        lv_timer_t* const queue_timer = helix::ui::UpdateQueue::instance().timer();
        lv_timer_t* const refr_timer = lv_display_get_refr_timer(lv_display_get_default());

        paused_timers_.clear();
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            const bool skip = (t == queue_timer) || (t == refr_timer);
            // A no-op lv_timer_pause() on an already-paused timer is harmless,
            // but resuming it later would not be — only track timers we
            // actually changed, so unfreeze never touches one paused by its
            // own owner for an unrelated reason.
            if (!skip && !lv_timer_get_paused(t)) {
                lv_timer_pause(t);
                paused_timers_.push_back(t);
            }
            t = next;
        }
        frozen_ = true;

        spdlog::debug("[RemoteControl] freeze: paused {} timers (skipped queue+refresh)",
                      paused_timers_.size());
        return {{"frozen", true}, {"timers_paused", static_cast<int>(paused_timers_.size())}};
    });
}

nlohmann::json RemoteControlServer::handle_unfreeze(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([this]() -> nlohmann::json {
        // Not frozen: a no-op rather than an error, so a defensive `finally:
        // unfreeze()` never needs its own guard around whether freeze ran.
        if (!frozen_) {
            return {{"frozen", false}, {"timers_resumed", 0}};
        }

        int resumed = 0;
        for (lv_timer_t* t : paused_timers_) {
            // A tracked timer may have been deleted while frozen (e.g. panel
            // teardown), so confirm it is still in LVGL's live list before
            // touching what would otherwise be a dangling pointer.
            bool still_live = false;
            for (lv_timer_t* live = lv_timer_get_next(nullptr); live;
                 live = lv_timer_get_next(live)) {
                if (live == t) {
                    still_live = true;
                    break;
                }
            }
            if (still_live) {
                lv_timer_resume(t);
                resumed++;
            }
        }
        paused_timers_.clear();
        frozen_ = false;
        // Restore the exact pre-freeze value (not a hardcoded "on") via the
        // subject directly — see handle_freeze() for why set_animations_enabled()
        // (which persists to settings.json) must not be used here.
        lv_subject_set_int(DisplaySettingsManager::instance().subject_animations_enabled(),
                           pre_freeze_animations_enabled_ ? 1 : 0);

        spdlog::debug("[RemoteControl] unfreeze: resumed {} timers", resumed);
        return {{"frozen", false}, {"timers_resumed", resumed}};
    });
}

nlohmann::json RemoteControlServer::handle_click(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }

    return execute_on_ui_thread([params]() -> nlohmann::json {
        wake_display();
        lv_obj_t* target = resolve_widget(params);
        if (!target) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        // Composite rows wrap the real control — descend to it when unambiguous.
        lv_obj_t* descended = nullptr;
        std::vector<lv_obj_t*> ambiguous;
        lv_obj_t* widget = resolve_actionable(target, &descended, &ambiguous);

        nlohmann::json result;
        result["clicked"] = target_label(params);
        // Always report the widget actually hit, and whether anything is
        // listening. A click on a widget with no handlers is a no-op, and
        // without this the response is indistinguishable from a real one.
        result["path"] = path_of(widget);
        result["handlers"] = lv_obj_get_event_count(widget);
        result["active_screen"] = active_screen_label();
        if (descended) {
            result["descended_to"] = path_of(descended);
        }
        if (!ambiguous.empty()) {
            nlohmann::json cands = nlohmann::json::array();
            for (lv_obj_t* c : ambiguous) {
                cands.push_back(path_of(c));
            }
            result["candidates"] = cands; // clicked the container; @path one of these
        }

        // A synthetic CLICKED does not flip a switch/checkbox (LVGL toggles those
        // on the indev press/release). Toggle the state explicitly and notify, so
        // `click <switch>` behaves like a real tap.
        if (lv_obj_check_type(widget, &lv_switch_class) ||
            lv_obj_check_type(widget, &lv_checkbox_class)) {
            bool now = !lv_obj_has_state(widget, LV_STATE_CHECKED);
            if (now) {
                lv_obj_add_state(widget, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(widget, LV_STATE_CHECKED);
            }
            lv_obj_send_event(widget, LV_EVENT_VALUE_CHANGED, nullptr);
            result["toggled_to"] = now ? 1 : 0;
            return result;
        }
        lv_obj_send_event(widget, LV_EVENT_CLICKED, nullptr);
        return result;
    });
}

nlohmann::json RemoteControlServer::handle_set_widget_value(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }
    if (!params.contains("value")) {
        throw std::invalid_argument("Missing required parameter: value");
    }

    auto value_json = params["value"];

    return execute_on_ui_thread([params, value_json]() -> nlohmann::json {
        wake_display();
        lv_obj_t* target = resolve_widget(params);
        if (!target) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        // Same container-to-control descent as click: `set_value <row> <v>`
        // should reach the row's slider/dropdown/switch.
        lv_obj_t* descended = nullptr;
        lv_obj_t* widget = resolve_actionable(target, &descended, nullptr);
        std::string widget_name = target_label(params);

        // Try to set value based on widget type
        if (value_json.is_number()) {
            int32_t val = value_json.get<int32_t>();
            // Works for sliders, bars, arcs, switches, spinboxes
            if (lv_obj_check_type(widget, &lv_slider_class)) {
                lv_slider_set_value(widget, val, LV_ANIM_OFF);
            } else if (lv_obj_check_type(widget, &lv_bar_class)) {
                lv_bar_set_value(widget, val, LV_ANIM_OFF);
            } else if (lv_obj_check_type(widget, &lv_arc_class)) {
                lv_arc_set_value(widget, val);
            } else if (lv_obj_check_type(widget, &lv_switch_class) ||
                       lv_obj_check_type(widget, &lv_checkbox_class)) {
                if (val) {
                    lv_obj_add_state(widget, LV_STATE_CHECKED);
                } else {
                    lv_obj_remove_state(widget, LV_STATE_CHECKED);
                }
            } else if (lv_obj_check_type(widget, &lv_dropdown_class)) {
                lv_dropdown_set_selected(widget, static_cast<uint32_t>(val));
            } else if (lv_obj_check_type(widget, &lv_textarea_class)) {
                // Accept a number for a textarea by stringifying it, so
                // `set_value <textarea> 220` fills it like the keypad would.
                lv_textarea_set_text(widget, std::to_string(val).c_str());
            } else {
                throw std::invalid_argument("Widget type does not support numeric value");
            }
            // Notify the app as if the user changed it, so bound handlers /
            // observers run instead of the widget silently moving.
            lv_obj_send_event(widget, LV_EVENT_VALUE_CHANGED, nullptr);
            return {{"widget", widget_name}, {"set", val}};

        } else if (value_json.is_string()) {
            std::string val = value_json.get<std::string>();
            if (lv_obj_check_type(widget, &lv_textarea_class)) {
                lv_textarea_set_text(widget, val.c_str());
                lv_obj_send_event(widget, LV_EVENT_VALUE_CHANGED, nullptr);
            } else if (lv_obj_check_type(widget, &lv_label_class)) {
                lv_label_set_text(widget, val.c_str());
            } else {
                throw std::invalid_argument("Widget type does not support string value");
            }
            return {{"widget", widget_name}, {"set", val}};

        } else {
            throw std::invalid_argument("Value must be a number or string");
        }
    });
}

// Classify a widget into a short type string for introspection output.
static const char* describe_widget_type(lv_obj_t* o) {
    if (lv_obj_check_type(o, &lv_switch_class))
        return "switch";
    if (lv_obj_check_type(o, &lv_checkbox_class))
        return "checkbox";
    if (lv_obj_check_type(o, &lv_slider_class))
        return "slider";
    if (lv_obj_check_type(o, &lv_arc_class))
        return "arc";
    if (lv_obj_check_type(o, &lv_bar_class))
        return "bar";
    if (lv_obj_check_type(o, &lv_dropdown_class))
        return "dropdown";
    if (lv_obj_check_type(o, &lv_textarea_class))
        return "textarea";
    if (lv_obj_check_type(o, &lv_button_class))
        return "button";
    if (lv_obj_check_type(o, &lv_label_class))
        return "label";
    if (lv_obj_check_type(o, &lv_image_class))
        return "image";
    return "obj";
}

// Build the descriptor for one named widget: type, what you can do to it, and
// its current value where that's meaningful.
static nlohmann::json describe_one(lv_obj_t* o, const char* name, const std::string& path) {
    nlohmann::json entry;
    entry["name"] = name;
    entry["path"] = path; // unique locator for click/set/scroll (see resolve_path)
    entry["type"] = describe_widget_type(o);

    // Which stacking layer this widget lives in — the path of its top-level
    // ancestor ("s/19"). Overlays stacked behind the visible one are not
    // LV_OBJ_FLAG_HIDDEN, so a flat listing gives no way to tell two same-named
    // widgets apart. Compare against the response's "topmost_layer".
    entry["layer"] = layer_of(o);

    nlohmann::json actions = nlohmann::json::array();
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        actions.push_back("click");
    }
    // Report containers that can actually scroll (flag set AND content overflows),
    // so callers know what to target with `scroll`.
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) {
        int32_t below = lv_obj_get_scroll_bottom(o);
        int32_t above = lv_obj_get_scroll_top(o);
        int32_t right = lv_obj_get_scroll_right(o);
        int32_t left = lv_obj_get_scroll_left(o);
        if (below > 0 || above > 0 || right > 0 || left > 0) {
            actions.push_back("scroll");
            entry["scroll"] = {{"top", above}, {"bottom", below}, {"left", left}, {"right", right}};
        }
    }
    if (lv_obj_check_type(o, &lv_textarea_class)) {
        actions.push_back("fill");
        const char* txt = lv_textarea_get_text(o);
        entry["value"] = txt ? txt : "";
    } else if (lv_obj_check_type(o, &lv_switch_class) || lv_obj_check_type(o, &lv_checkbox_class)) {
        actions.push_back("toggle");
        entry["value"] = lv_obj_has_state(o, LV_STATE_CHECKED) ? 1 : 0;
    } else if (lv_obj_check_type(o, &lv_slider_class)) {
        actions.push_back("set");
        entry["value"] = lv_slider_get_value(o);
    } else if (lv_obj_check_type(o, &lv_arc_class)) {
        actions.push_back("set");
        entry["value"] = lv_arc_get_value(o);
    } else if (lv_obj_check_type(o, &lv_bar_class)) {
        entry["value"] = lv_bar_get_value(o);
    } else if (lv_obj_check_type(o, &lv_dropdown_class)) {
        actions.push_back("set");
        entry["value"] = static_cast<int>(lv_dropdown_get_selected(o));
    }
    entry["actions"] = actions;
    return entry;
}

// Recursively collect named, non-hidden widgets under `parent` into `out`.
// Only named widgets are emitted (unnamed ones can't be addressed), but the
// walk still descends through them to reach named descendants.
static void describe_walk(lv_obj_t* parent, const std::string& path_prefix, nlohmann::json& out) {
    if (!parent) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child) {
            continue;
        }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue; // hidden subtree — not on screen
        }
        // Segments are names where a name uniquely identifies the child among
        // its siblings, "name[k]" where it does not, and a real child index
        // (not the filtered emit order) where the child has no name — so a path
        // stays resolvable regardless of hidden or unnamed siblings, and stays
        // readable enough that a person can retype it.
        std::string child_path = path_prefix + "/" + path_segment_for(child);
        const char* raw = lv_obj_get_name(child);
        if (raw && raw[0] != '\0') {
            // Resolve "foo_#" index placeholders to the concrete "foo_2" name
            // so the reported name is actually addressable via find_by_name.
            char resolved[128];
            lv_obj_get_name_resolved(child, resolved, sizeof(resolved));
            out.push_back(describe_one(child, resolved[0] != '\0' ? resolved : raw, child_path));
        }
        describe_walk(child, child_path, out);
    }
}

nlohmann::json RemoteControlServer::handle_describe_screen(const nlohmann::json& params) {
    // Optional scoping: `ls <name|@path>` lists only that widget's subtree.
    // A full-screen dump on a busy panel runs to hundreds of entries, which is
    // unreadable when you already know which row you care about.
    bool scoped = params.contains("name") || params.contains("path");

    return execute_on_ui_thread([params, scoped]() -> nlohmann::json {
        nlohmann::json widgets = nlohmann::json::array();
        if (scoped) {
            // Unlike click/set_value, a glob here is not ambiguous — listing
            // several subtrees is exactly what `ls row_*` should do.
            std::vector<lv_obj_t*> roots;
            if (params.contains("name") && params["name"].is_string() &&
                is_glob(params["name"].get<std::string>())) {
                roots = drop_nested_matches(
                    glob_widgets(params["name"].get<std::string>(), scope_root(params)));
            } else if (lv_obj_t* one = resolve_widget(params)) {
                roots.push_back(one);
            }
            if (roots.empty()) {
                throw std::invalid_argument("Widget not found: " + target_label(params));
            }
            nlohmann::json scopes = nlohmann::json::array();
            for (lv_obj_t* root : roots) {
                std::string root_path = path_of(root);
                scopes.push_back(root_path);
                // Include the root itself, then its subtree, so the listing is
                // self-contained (you can see what you scoped to).
                char resolved[128];
                lv_obj_get_name_resolved(root, resolved, sizeof(resolved));
                const char* raw = lv_obj_get_name(root);
                widgets.push_back(describe_one(
                    root, resolved[0] != '\0' ? resolved : (raw ? raw : ""), root_path));
                describe_walk(root, root_path, widgets);
            }
            if (scopes.size() == 1) {
                return {{"widgets", widgets},
                        {"scope", scopes[0]},
                        {"topmost_layer", topmost_layer()},
                        {"active_screen", active_screen_label()}};
            }
            return {{"widgets", widgets},
                    {"scope", scopes},
                    {"matched", scopes.size()},
                    {"topmost_layer", topmost_layer()},
                    {"active_screen", active_screen_label()}};
        }
        // No explicit target: list the caller's working directory. `cd` into a
        // container and a bare `ls` shows what is in front of you rather than
        // the whole screen — the reason a cwd exists at all.
        if (lv_obj_t* cwd = scope_root(params)) {
            const std::string cwd_path = path_of(cwd);
            describe_walk(cwd, cwd_path, widgets);
            return {{"widgets", widgets},
                    {"scope", cwd_path},
                    {"topmost_layer", topmost_layer()},
                    {"active_screen", active_screen_label()}};
        }
        // Active screen ("s") holds panels + pushed overlays; the top layer ("t")
        // holds modals and their backdrops. Walk both so nothing on screen is missed.
        describe_walk(lv_screen_active(), "s", widgets);
        describe_walk(lv_layer_top(), "t", widgets);
        return {{"widgets", widgets},
                {"topmost_layer", topmost_layer()},
                {"active_screen", active_screen_label()}};
    });
}

// `cd <target>` needs the absolute locator of wherever it landed, so the client
// can hold it as a working directory. Read-only — unlike the old `cd`, which
// reached its destination by clicking.
nlohmann::json RemoteControlServer::handle_resolve(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }

    return execute_on_ui_thread([params]() -> nlohmann::json {
        lv_obj_t* obj = resolve_widget(params);
        if (!obj) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        char resolved[128];
        lv_obj_get_name_resolved(obj, resolved, sizeof(resolved));
        return {{"path", path_of(obj)},
                {"name", resolved},
                {"children", lv_obj_get_child_count(obj)},
                {"active_screen", active_screen_label()}};
    });
}

nlohmann::json RemoteControlServer::handle_focus(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }

    return execute_on_ui_thread([params]() -> nlohmann::json {
        wake_display();
        lv_obj_t* obj = resolve_widget(params);
        if (!obj) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }

        // Focus through the group rather than by sending LV_EVENT_FOCUSED directly,
        // so the production path runs: the group defocuses whatever held focus, and
        // KeyboardManager's LV_EVENT_FOCUSED handler raises the on-screen keyboard
        // for a registered textarea. A synthesized CLICKED never triggers any of it,
        // which is why `ctl click <textarea>` leaves the keyboard hidden.
        lv_group_t* group = lv_obj_get_group(obj);
        if (!group) {
            throw std::invalid_argument("Widget is not in an input group (not focusable): " +
                                        target_label(params));
        }
        lv_group_focus_obj(obj);

        return {{"focused", target_label(params)},
                {"keyboard_visible", KeyboardManager::instance().is_visible()},
                {"active_screen", active_screen_label()}};
    });
}

namespace {

// Read text out of whichever widget type carries it. Returns false when the
// widget has no text concept at all — distinct from having empty text, which
// is why this can't just return an empty string for both cases.
bool read_widget_text(lv_obj_t* o, std::string& out, std::string& source) {
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char* t = lv_label_get_text(o);
        out = t ? t : "";
        source = "label";
        return true;
    }
    if (lv_obj_check_type(o, &lv_textarea_class)) {
        const char* t = lv_textarea_get_text(o);
        out = t ? t : "";
        source = "textarea";
        return true;
    }
    if (lv_obj_check_type(o, &lv_dropdown_class)) {
        char buf[128];
        lv_dropdown_get_selected_str(o, buf, sizeof(buf));
        out = buf;
        source = "dropdown";
        return true;
    }
    return false;
}

// Depth-first search for the first descendant carrying text. Mirrors how
// resolve_actionable() descends a composite row to its value-control — a
// button (ui_button) carries no text of its own; the text lives on the
// lv_label it creates internally.
lv_obj_t* find_text_descendant(lv_obj_t* root) {
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        std::string ignored_text, ignored_source;
        if (read_widget_text(child, ignored_text, ignored_source)) {
            return child;
        }
        if (lv_obj_t* deeper = find_text_descendant(child)) {
            return deeper;
        }
    }
    return nullptr;
}

} // namespace

nlohmann::json RemoteControlServer::handle_text(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }

    return execute_on_ui_thread([params]() -> nlohmann::json {
        lv_obj_t* widget = resolve_widget(params);
        if (!widget) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        std::string value, source;
        lv_obj_t* holder = widget;
        if (!read_widget_text(holder, value, source)) {
            holder = find_text_descendant(widget);
            if (!holder || !read_widget_text(holder, value, source)) {
                throw std::invalid_argument("Widget has no text: " + target_label(params));
            }
        }
        return {{"text", value}, {"path", path_of(holder)}, {"source", source}};
    });
}

nlohmann::json RemoteControlServer::handle_set_text(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }
    if (!params.contains("text") || !params["text"].is_string()) {
        throw std::invalid_argument("Missing required parameter: text");
    }
    const std::string text = params["text"].get<std::string>();

    return execute_on_ui_thread([params, text]() -> nlohmann::json {
        lv_obj_t* widget = resolve_widget(params);
        if (!widget) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        // Resolve the same way `text` reads it: the named widget is often a
        // wrapper whose label is a descendant.
        lv_obj_t* holder = widget;
        std::string existing, source;
        if (!read_widget_text(holder, existing, source)) {
            holder = find_text_descendant(widget);
            if (!holder || !read_widget_text(holder, existing, source)) {
                throw std::invalid_argument("Widget has no text: " + target_label(params));
            }
        }
        if (!lv_obj_check_type(holder, &lv_label_class)) {
            throw std::invalid_argument("Not a label, cannot set text: " + target_label(params));
        }
        // Writes straight to the label. A label driven by bind_text is restored
        // the next time its subject changes — set the subject instead when one
        // exists. This exists for text the app sets imperatively, which is
        // otherwise unreachable from a test (e.g. the AMS loading-error message,
        // which comes from a backend field rather than a subject).
        lv_label_set_text(holder, text.c_str());
        return {{"set", text}, {"path", path_of(holder)}};
    });
}

nlohmann::json RemoteControlServer::handle_state(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }

    return execute_on_ui_thread([params]() -> nlohmann::json {
        lv_obj_t* target = resolve_widget(params);
        if (!target && params.contains("name") && params["name"].is_string()) {
            // Name lookup skips hidden subtrees so `click <name>` never hits an
            // invisible widget — but "is it hidden?" is a question `state`
            // exists to answer. Retry with hidden matches included (plain
            // names only: a @path already resolves hidden widgets, and a glob
            // is a discovery aid like `ls`, which also filters).
            const std::string name = params["name"].get<std::string>();
            if (!is_glob(name)) {
                std::vector<lv_obj_t*> matches;
                if (lv_obj_t* scope = scope_root(params)) {
                    collect_by_name(scope, name, matches, /*include_hidden=*/true);
                } else {
                    if (lv_obj_t* screen = lv_screen_active()) {
                        collect_by_name(screen, name, matches, /*include_hidden=*/true);
                    }
                    collect_by_name(lv_layer_top(), name, matches, /*include_hidden=*/true);
                }
                if (!matches.empty()) {
                    target = topmost_visible(matches);
                }
            }
        }
        if (!target) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        // Same container-to-control descent as click/set_value: `state <row>`
        // reports on the switch/slider the row wraps, because that is what
        // those commands would act on.
        lv_obj_t* descended = nullptr;
        lv_obj_t* widget = resolve_actionable(target, &descended, nullptr);

        nlohmann::json result;
        nlohmann::json states = nlohmann::json::array();
        const struct {
            lv_state_t bit;
            const char* name;
        } state_table[] = {
            {LV_STATE_CHECKED, "checked"}, {LV_STATE_DISABLED, "disabled"},
            {LV_STATE_FOCUSED, "focused"}, {LV_STATE_FOCUS_KEY, "focus_key"},
            {LV_STATE_PRESSED, "pressed"}, {LV_STATE_HOVERED, "hovered"},
            {LV_STATE_EDITED, "edited"},
        };
        for (const auto& entry : state_table) {
            const bool on = lv_obj_has_state(widget, entry.bit);
            result[entry.name] = on; // flat booleans for jq-friendly assertions
            if (on) {
                states.push_back(entry.name);
            }
        }
        result["states"] = states; // and the set form for human eyes in the REPL

        result["path"] = path_of(widget);
        if (descended) {
            result["descended_to"] = path_of(descended);
        }
        // Flags answer "why won't it show up / respond": a HIDDEN widget is
        // still resolvable by name (only `ls` filters hidden subtrees), so
        // bind_flag_if contracts are assertable here.
        auto flags_of = [](lv_obj_t* w) {
            return nlohmann::json{
                {"hidden", lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN)},
                {"clickable", lv_obj_has_flag(w, LV_OBJ_FLAG_CLICKABLE)},
                {"scrollable", lv_obj_has_flag(w, LV_OBJ_FLAG_SCROLLABLE)},
            };
        };
        result["flags"] = flags_of(widget);
        if (descended) {
            // What you named vs what was driven can differ in visibility: a
            // row hides itself while its inner switch carries no flag of its
            // own, so `flags` alone would call a hidden row's control visible.
            result["target"] = {{"path", path_of(target)}, {"flags", flags_of(target)}};
        }
        return result;
    });
}

nlohmann::json RemoteControlServer::apply_pointer_state(int32_t x, int32_t y, bool pressed,
                                                        const char* what) {
    uint64_t baseline = 0;

    nlohmann::json result = execute_on_ui_thread([&baseline, x, y, pressed]() -> nlohmann::json {
        wake_display();
        auto& pointer = helix::remote::RemotePointer::instance();
        if (!pointer.ensure_created()) {
            throw std::runtime_error("No display available for the synthetic pointer");
        }
        pointer.set_state(x, y, pressed);
        baseline = pointer.read_count();
        return {{"x", x}, {"y", y}, {"pressed", pressed}};
    });

    // Indevs are sampled on a timer, so the state above is not visible to LVGL yet.
    // Wait for two reads: the first delivers the new state, the second lets LVGL's
    // press/release state machine act on it before the next command lands. Without
    // this, a fast press-then-release sequence can be coalesced into a single sample
    // and the press is never seen at all.
    auto& pointer = helix::remote::RemotePointer::instance();
    const uint64_t target = baseline + 2;
    for (int i = 0; i < 400; ++i) { // 400 * 5ms = 2s
        if (pointer.read_count() >= target) {
            result["what"] = what;
            result["reads"] = pointer.read_count();
            return result;
        }
        if (!running_.load()) {
            throw std::runtime_error("remote server shutting down");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Timed out waiting for the synthetic pointer to be sampled");
}

nlohmann::json RemoteControlServer::handle_pointer_press(const nlohmann::json& params) {
    if (!params.contains("x") || !params.contains("y")) {
        throw std::invalid_argument("Missing required parameters: x and y");
    }
    return apply_pointer_state(params["x"].get<int32_t>(), params["y"].get<int32_t>(), true,
                               "press");
}

nlohmann::json RemoteControlServer::handle_pointer_move(const nlohmann::json& params) {
    if (!params.contains("x") || !params.contains("y")) {
        throw std::invalid_argument("Missing required parameters: x and y");
    }
    // Keeps the current button state, so this is a drag while pressed and a hover
    // while released.
    return apply_pointer_state(params["x"].get<int32_t>(), params["y"].get<int32_t>(),
                               helix::remote::RemotePointer::instance().pressed(), "move");
}

nlohmann::json RemoteControlServer::handle_pointer_release(const nlohmann::json& params) {
    auto& pointer = helix::remote::RemotePointer::instance();
    // Release where the pointer already is unless told otherwise — the common case
    // is lifting after a drag, and repeating the coordinates is noise.
    const int32_t x = params.contains("x") ? params["x"].get<int32_t>() : pointer.x();
    const int32_t y = params.contains("y") ? params["y"].get<int32_t>() : pointer.y();
    return apply_pointer_state(x, y, false, "release");
}

nlohmann::json RemoteControlServer::handle_pointer_long_press(const nlohmann::json& params) {
    if (!params.contains("x") || !params.contains("y")) {
        throw std::invalid_argument("Missing required parameters: x and y");
    }
    const int32_t x = params["x"].get<int32_t>();
    const int32_t y = params["y"].get<int32_t>();

    // Default the hold to comfortably past LVGL's own threshold rather than
    // hardcoding a duration here: the threshold is a user setting (Touch & Input)
    // and a hold tuned to the default would silently stop being a long press if
    // someone raised it.
    int32_t hold_ms = params.contains("hold_ms") ? params["hold_ms"].get<int32_t>() : 0;
    if (hold_ms <= 0) {
        hold_ms = helix::remote::pointer_long_press_hold_ms(
            helix::InputSettingsManager::instance().get_long_press_time());
    }

    nlohmann::json result = apply_pointer_state(x, y, true, "long_press");

    // Hold without touching the pointer. The press is already latched in
    // RemotePointer and LVGL keeps sampling it on its own timer, so the gesture
    // accumulates here exactly as it does under a resting finger. Doing this
    // server-side is the whole point: a shell doing press / sleep / release spends
    // the hold with no client connected, and any command that lands in between
    // resamples the device and can restart the press.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!running_.load()) {
            throw std::runtime_error("remote server shutting down");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    nlohmann::json release = apply_pointer_state(x, y, false, "long_press");
    result["held_ms"] = hold_ms;
    result["reads"] = release["reads"];
    return result;
}

nlohmann::json RemoteControlServer::handle_scroll(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }
    bool has_delta = params.contains("dx") || params.contains("dy");
    int dx = params.value("dx", 0);
    int dy = params.value("dy", 0);

    return execute_on_ui_thread([params, has_delta, dx, dy]() -> nlohmann::json {
        wake_display();
        lv_obj_t* obj = resolve_widget(params);
        if (!obj) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        if (has_delta) {
            // Scroll the container's content by the requested delta.
            lv_obj_scroll_by(obj, dx, dy, LV_ANIM_OFF);
            return {{"scrolled", target_label(params)}, {"dx", dx}, {"dy", dy}};
        }
        // Bring the target into its scroll parent's viewport (walks ancestors).
        lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
        return {{"scrolled_into_view", target_label(params)}};
    });
}

/**
 * Render a raw style size coord as the value the XML author wrote.
 *
 * LVGL packs LV_SIZE_CONTENT and percentages into the coord, so a bare integer
 * print turns "content" into a sentinel like 2001 and "50%" into 2049. Reporting
 * the authored form is what makes a collapsed widget diagnosable: a declared
 * "content" or "50%" sitting next to a computed 0 localises the layout fault.
 */
static std::string describe_style_size(int32_t v) {
    if (v == LV_SIZE_CONTENT) {
        return "content";
    }
    if (LV_COORD_IS_PCT(v)) {
        return std::to_string(LV_COORD_GET_PCT(v)) + "%";
    }
    return std::to_string(v);
}

/** Collect one widget's geometry, then recurse `depth` more levels. */
static void geom_walk(lv_obj_t* obj, const std::string& path, int depth, nlohmann::json& out) {
    lv_obj_update_layout(obj);

    nlohmann::json e;
    e["path"] = path;
    const char* name = lv_obj_get_name(obj);
    e["name"] = name ? name : "";
    // Coordinates are relative to the parent; absolute screen coords come from the
    // object's own area, which is what a screenshot measurement compares against.
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    e["x"] = area.x1;
    e["y"] = area.y1;
    e["w"] = lv_obj_get_width(obj);
    e["h"] = lv_obj_get_height(obj);
    e["content_w"] = lv_obj_get_content_width(obj);
    e["content_h"] = lv_obj_get_content_height(obj);
    // Declared vs computed is the whole point of this command.
    e["req_w"] = describe_style_size(lv_obj_get_style_width(obj, LV_PART_MAIN));
    e["req_h"] = describe_style_size(lv_obj_get_style_height(obj, LV_PART_MAIN));
    e["flex_grow"] = lv_obj_get_style_flex_grow(obj, LV_PART_MAIN);
    e["hidden"] = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    e["scrollable"] = lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    e["scroll"] = {{"top", lv_obj_get_scroll_top(obj)},
                   {"bottom", lv_obj_get_scroll_bottom(obj)},
                   {"left", lv_obj_get_scroll_left(obj)},
                   {"right", lv_obj_get_scroll_right(obj)}};
    out.push_back(e);

    if (depth <= 0) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        geom_walk(lv_obj_get_child(obj, i), path + "/" + std::to_string(i), depth - 1, out);
    }
}

nlohmann::json RemoteControlServer::handle_geom(const nlohmann::json& params) {
    if (!params.contains("name") && !params.contains("path")) {
        throw std::invalid_argument("Missing required parameter: name or path");
    }
    int depth = params.value("depth", 0);

    return execute_on_ui_thread([params, depth]() -> nlohmann::json {
        lv_obj_t* obj = resolve_widget(params);
        if (!obj) {
            throw std::invalid_argument("Widget not found: " + target_label(params));
        }
        nlohmann::json widgets = nlohmann::json::array();
        geom_walk(obj, target_label(params), depth, widgets);
        return {{"widgets", widgets}};
    });
}

nlohmann::json RemoteControlServer::handle_get_const(const nlohmann::json& params) {
    // Resolution order mirrors the XML engine: a named scope first, then the
    // implicit "globals" fallback that unqualified #consts resolve against.
    std::string scope_name = params.value("scope", "globals");
    bool want_all = !params.contains("name");
    std::string const_name = params.value("name", "");

    return execute_on_ui_thread([scope_name, const_name, want_all]() -> nlohmann::json {
        lv_xml_component_scope_t* scope = lv_xml_component_get_scope(scope_name.c_str());
        if (!scope) {
            throw std::invalid_argument("No such component scope: " + scope_name);
        }
        if (!want_all) {
            const char* value = lv_xml_get_const_silent(scope, const_name.c_str());
            if (!value) {
                // Unqualified consts fall back to globals, so mirror that here
                // rather than reporting a miss the renderer would have resolved.
                lv_xml_component_scope_t* g = lv_xml_component_get_scope("globals");
                if (g && g != scope) {
                    value = lv_xml_get_const_silent(g, const_name.c_str());
                }
                if (value) {
                    return {{"scope", "globals"}, {"name", const_name}, {"value", value}};
                }
                throw std::invalid_argument("No const '" + const_name + "' in scope " + scope_name);
            }
            return {{"scope", scope_name}, {"name", const_name}, {"value", value}};
        }
        nlohmann::json consts = nlohmann::json::object();
        // LV_LL_READ is a C macro that relies on implicit void* conversion — expand it
        // manually for C++ so the element pointer can be cast explicitly.
        for (void* node = lv_ll_get_head(&scope->const_ll); node != nullptr;
             node = lv_ll_get_next(&scope->const_ll, node)) {
            auto* c = static_cast<lv_xml_const_t*>(node);
            consts[c->name] = c->value;
        }
        return {{"scope", scope_name}, {"consts", consts}};
    });
}

nlohmann::json RemoteControlServer::handle_wake(const nlohmann::json& /*params*/) {
    return execute_on_ui_thread([]() -> nlohmann::json {
        wake_display();
        return {{"awake", true}};
    });
}

nlohmann::json RemoteControlServer::handle_scenario(const nlohmann::json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name");
    }

    std::string scenario_name = params["name"];
    const auto* scenario = find_scenario(scenario_name);
    if (!scenario) {
        throw std::invalid_argument("Unknown scenario: " + scenario_name);
    }

    auto apply_fn = scenario->apply;
    return execute_on_ui_thread([apply_fn, scenario_name]() -> nlohmann::json {
        apply_fn();
        return {{"applied", scenario_name}};
    });
}

nlohmann::json RemoteControlServer::handle_list_scenarios(const nlohmann::json& /*params*/) {
    const auto& scenarios = get_mock_scenarios();

    nlohmann::json result = nlohmann::json::array();
    for (const auto& s : scenarios) {
        result.push_back({{"name", s.name}, {"description", s.description}});
    }

    return {{"scenarios", result}, {"count", scenarios.size()}};
}

} // namespace helix
