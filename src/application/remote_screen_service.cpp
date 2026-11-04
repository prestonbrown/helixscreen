// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_service.h"

#include "ui_timer_guard.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/update_queue.h"
#include "data_root_resolver.h"
#include "printer_state.h"
#include "remote_pointer.h"
#include "remote_screen_http_server.h"
#include "remote_screen_manager.h"
#include "remote_screen_server_sink.h"
#include "remote_screen_settings_manager.h"
#include "remote_screen_webcam.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>

namespace helix {

namespace {
/// How often to release a gone viewer's buffers and a press nobody finished.
constexpr uint32_t kHousekeepingMs = 500;
/// How long a touch waits for the UI thread to take it, then for LVGL to sample it.
constexpr auto kTouchQueueWait = std::chrono::seconds(1);
constexpr auto kTouchSampleWait = std::chrono::seconds(2);
constexpr auto kTouchWaitSlice = std::chrono::milliseconds(20);
/// Handed back by the UI thread when there is no display to point at.
constexpr uint64_t kNoPointer = UINT64_MAX;
} // namespace

bool remote_screen_firmware_provides() {
    const char* mode = std::getenv("HELIX_REMOTE_SCREEN");
    if (mode && std::strcmp(mode, "firmware") == 0) {
        return true;
    }
    // Presence, like DisplayManager's own gate on it: set at all means the mirror is attached.
    return std::getenv("HELIX_REMOTE_SCREEN_FB0") != nullptr;
}

std::string remote_screen_route(const std::string& route_file) {
    std::ifstream in(route_file);
    std::string line;
    if (!in || !std::getline(in, line)) {
        return {};
    }
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t\r");
    return line.substr(first, last - first + 1);
}

RemoteScreenService& RemoteScreenService::instance() {
    static RemoteScreenService inst;
    return inst;
}

RemoteScreenService::RemoteScreenService() : store_(std::make_shared<ScreenFrameStore>()) {}

RemoteScreenService::~RemoteScreenService() {
    cancel_housekeeping_timer();
}

bool RemoteScreenService::available() const {
#ifdef __ANDROID__
    return false;
#else
    return !remote_screen_firmware_provides();
#endif
}

void RemoteScreenService::init(RemoteScreenManager* screens) {
    auto& settings = RemoteScreenSettingsManager::instance();
    settings.set_available(available());
    if (!available()) {
        spdlog::info("[RemoteScreen] The firmware serves the screen; built-in server off");
        return;
    }
    if (screens && !sink_attached_) {
        screens->add_sink(std::make_unique<ScreenServerSink>(store_));
        screens->start();
        sink_attached_ = true;
        // On the top layer, so no panel or overlay covers it; it hides itself
        // through its subject binding.
        if (!lv_xml_create(lv_layer_top(), "remote_control_indicator", nullptr)) {
            spdlog::warn("[RemoteScreen] remote_control_indicator creation failed");
        }
    }
    settings.set_change_handler([this] { apply_settings(); });
    // The next printer's own connect reconciles its webcam list.
    settings.set_printer_handler([this] { apply_server(); });
    settings.set_forget_handler([this] {
        if (pairing_) {
            pairing_->forget_all();
        }
    });
    apply_settings();
}

void RemoteScreenService::watch_print_state() {
    auto& ps = get_printer_state();
    print_observer_ = ui::observe<int>(
        ps.print_state().get_print_active_subject(), this,
        [](RemoteScreenService* self, int active) { self->printing_.store(active != 0); },
        ps.print_state().get_static_subjects_lifetime());
}

void RemoteScreenService::apply_settings() {
    // Runs on every connect, after a printer switch has rebuilt the print subjects.
    watch_print_state();
    apply_server();
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        sync_remote_screen_webcam(*api, get_printer_state().moonraker_locality());
    }
}

void RemoteScreenService::apply_server() {
    auto& settings = RemoteScreenSettingsManager::instance();
    const bool want = available() && settings.get_enabled();

    if (!want) {
        stop_server();
        store_->release();
        return;
    }

    // Before this connection's verdict, assume this host: most screens run beside
    // their Moonraker, and the verdict re-decides on connect.
    const std::string route = remote_screen_effective_route(
        get_printer_state().moonraker_locality() == MoonrakerLocality::Remote,
        remote_screen_route(writable_path(".remote_screen_route")));
    const bool lan_mode = route.empty();
    settings.set_lan_mode(lan_mode);
    if (server_ && lan_mode != lan_mode_) {
        stop_server();
    }
    if (server_) {
        server_->set_control_allowed(settings.get_control());
        return;
    }

    if (lan_mode) {
        PairingStore::Persist persist;
        // The store is used from the HTTP thread; config and subjects belong to the UI thread.
        persist.save_code = [](const std::string& code) {
            ui::queue_update("remote_screen::save_pair_code", [code] {
                RemoteScreenSettingsManager::instance().set_pair_code(code);
            });
        };
        persist.save_tokens = [](const std::vector<std::string>& tokens) {
            ui::queue_update("remote_screen::save_paired", [tokens] {
                RemoteScreenSettingsManager::instance().set_paired_tokens(tokens);
            });
        };
        pairing_ = std::make_shared<PairingStore>(settings.get_pair_code(),
                                                  settings.get_paired_tokens(), persist);
    } else {
        pairing_.reset();
    }

    ScreenServerHooks hooks;
    hooks.request_full_redraw = [store = store_] {
        ui::queue_update("remote_screen::full_redraw", [store] {
            if (lv_obj_t* screen = lv_screen_active()) {
                lv_obj_invalidate(screen);
            }
            lv_obj_invalidate(lv_layer_top());
            // On the UI thread, between refreshes: the next one to finish is whole.
            store->note_full_invalidate();
        });
    };
    hooks.is_printing = [this] { return printing_.load(); };
    hooks.apply_touch = [this](const TouchEvent& e) { return apply_touch(e); };
    hooks.pairing = pairing_;

    ScreenServerConfig cfg;
    cfg.bind_host = lan_mode ? "0.0.0.0" : "127.0.0.1";
    cfg.port = settings.get_port();
    cfg.control_allowed = settings.get_control();
    cfg.lan_mode = lan_mode;
    cfg.page_path = asset_path("assets/remote_screen/index.html");

    auto server = std::make_unique<ScreenHttpServer>(store_, hooks);
    if (!server->start(cfg)) {
        return;
    }
    server_ = std::move(server);
    lan_mode_ = lan_mode;
    serving_.store(true);

    if (!housekeeping_timer_) {
        housekeeping_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                static_cast<RemoteScreenService*>(lv_timer_get_user_data(t))
                    ->housekeeping_tick(std::chrono::steady_clock::now());
            },
            kHousekeepingMs, this);
    }
}

void RemoteScreenService::housekeeping_tick(std::chrono::steady_clock::time_point now) {
    if (store_->release_if_idle()) {
        spdlog::debug("[RemoteScreen] Viewer gone; frame buffers released");
    }
    if (auto rel = touch_session_.take_stale_release(now)) {
        spdlog::info("[RemoteScreen] Releasing a remote press nobody finished");
        remote::RemotePointer::instance().set_logical_state(rel->x, rel->y, false);
    }
    RemoteScreenSettingsManager::instance().set_control_active(
        serving_.load() && remote_control_active(touch_session_.last_event(), now));
}

void RemoteScreenService::stop_server() {
    serving_.store(false);
    if (server_) {
        server_->stop();
        server_.reset();
    }
    cancel_housekeeping_timer();
    touch_session_.reset();
    RemoteScreenSettingsManager::instance().set_control_active(false);
    if (remote::RemotePointer::instance().pressed()) {
        auto& ptr = remote::RemotePointer::instance();
        ptr.set_logical_state(ptr.x(), ptr.y(), false);
    }
}

bool RemoteScreenService::apply_touch(const TouchEvent& e) {
    if (!touch_session_.on_event(e, std::chrono::steady_clock::now())) {
        return true; // a move after the stale press was released: nothing to drive
    }
    auto baseline = std::make_shared<std::promise<uint64_t>>();
    auto taken = baseline->get_future();
    ui::queue_update("remote_screen::touch", [baseline, e] {
        // The server may have stopped while this waited in the queue; nothing would
        // release a press made now.
        if (!RemoteScreenService::instance().serving_.load()) {
            baseline->set_value(kNoPointer);
            return;
        }
        // RemotePointer's read callback owns waking and swallowing the wake
        // gesture. Activity here could wake the display before it samples the press.
        auto& ptr = remote::RemotePointer::instance();
        if (!ptr.ensure_created()) {
            baseline->set_value(kNoPointer);
            return;
        }
        const uint64_t before = ptr.read_count();
        ptr.set_logical_state(e.x, e.y, e.type != TouchEvent::Type::Up);
        baseline->set_value(before);
    });
    // Waited in slices so stop_server(), which joins this thread from the UI thread,
    // is never held for the whole wait by a queue only the UI thread can drain.
    const auto deadline = std::chrono::steady_clock::now() + kTouchQueueWait;
    while (taken.wait_for(kTouchWaitSlice) != std::future_status::ready) {
        if (!serving_.load() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    uint64_t before = kNoPointer;
    try {
        before = taken.get();
    } catch (const std::future_error&) {
        // The queue dropped the update unrun (shutdown).
        return false;
    }
    if (before == kNoPointer) {
        return false;
    }
    return remote::RemotePointer::instance().wait_for_samples(before, kTouchSampleWait, [this] {
        return serving_.load();
    }) == remote::RemotePointer::WaitResult::Sampled;
}

void RemoteScreenService::cancel_housekeeping_timer() {
    ui::lv_timer_cancel_safe(housekeeping_timer_);
    housekeeping_timer_ = nullptr;
}

void RemoteScreenService::shutdown() {
    auto& settings = RemoteScreenSettingsManager::instance();
    settings.set_change_handler(nullptr);
    settings.set_printer_handler(nullptr);
    settings.set_forget_handler(nullptr);
    print_observer_.reset();
    stop_server();
    pairing_.reset();
    store_->release();
}

bool RemoteScreenService::viewer_active() const {
    return serving_.load() && store_->page_viewer_present();
}

bool RemoteScreenService::running() const {
    return server_ != nullptr;
}

int RemoteScreenService::port() const {
    return server_ ? server_->port() : 0;
}

} // namespace helix
