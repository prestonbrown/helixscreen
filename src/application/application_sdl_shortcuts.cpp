// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application_sdl_shortcuts.cpp
 * @brief Desktop (SDL) keyboard shortcuts for Application: quit, screenshot, dark
 *        mode, mock-printer toggles. A no-op on builds without the SDL display.
 */

#include "ui_nav_manager.h"
#include "ui_panel_memory_stats.h"
#include "ui_toast_manager.h"

#include "action_prompt_manager.h"
#include "app_globals.h"
#include "application.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "keyboard_shortcuts.h"
#include "memory_utils.h"
#include "moonraker_manager.h"
#include "runtime_config.h"
#include "screenshot.h"
#include "theme_manager.h"
#ifdef HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif

#include <algorithm>

using namespace helix;

void Application::handle_keyboard_shortcuts() {
#ifdef HELIX_DISPLAY_SDL
    // Static shortcut registry - initialized once
    static helix::input::KeyboardShortcuts shortcuts;
    static bool shortcuts_initialized = false;

    if (!shortcuts_initialized) {
        // Cmd+Q / Win+Q to quit
        shortcuts.register_combo(KMOD_GUI, SDL_SCANCODE_Q, []() {
            spdlog::info("[Application] Cmd+Q/Win+Q pressed - exiting");
            app_request_quit();
        });

        // S key - take screenshot
        shortcuts.register_key(SDL_SCANCODE_S, []() {
            spdlog::info("[Application] S key - taking screenshot");
            auto path = helix::save_screenshot();
            if (!path.empty()) {
                auto basename = path.substr(path.rfind('/') + 1);
                auto msg = "Screenshot " + basename + " taken!";
                ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 3000);
            }
        });

        // M key - toggle memory stats
        shortcuts.register_key(SDL_SCANCODE_M, []() { MemoryStatsOverlay::instance().toggle(); });

        // D key - toggle dark/light mode
        shortcuts.register_key(SDL_SCANCODE_D, []() {
            spdlog::info("[Application] D key - toggling dark/light mode");
            theme_manager_toggle_dark_mode();
        });

        // F key - toggle filament runout simulation (needs m_session.moonraker())
        shortcuts.register_key_if(
            SDL_SCANCODE_F,
            [this]() {
                spdlog::info("[Application] F key - toggling filament runout simulation");
                m_session.moonraker()->client()->toggle_filament_runout_simulation();
            },
            [this]() { return m_session.moonraker() && m_session.moonraker()->client(); });

        // P key - cycle through configured printers (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_P,
            [this]() {
                auto ids = m_config->get_printer_ids();
                if (ids.size() > 1) {
                    auto current = m_config->get_active_printer_id();
                    auto it = std::find(ids.begin(), ids.end(), current);
                    auto next = (it != ids.end() && std::next(it) != ids.end()) ? *std::next(it)
                                                                                : ids.front();
                    spdlog::info("[Application] P key - switching to printer '{}'", next);
                    m_session.switch_printer(next);
                } else {
                    // Create a second test printer so we can test switching
                    spdlog::info(
                        "[Application] P key - creating test printer for multi-printer testing");
                    nlohmann::json test_data;
                    test_data["printer_name"] = "Voron 2.4";
                    test_data["type"] = "Voron 2.4 350mm";
                    test_data["moonraker_host"] = "127.0.0.1";
                    test_data["moonraker_port"] = 7125;
                    m_config->add_printer("voron-24", test_data);
                    m_config->save();
                }
            },
            [this]() { return get_runtime_config()->is_test_mode() && m_config; });

        // A key - test action prompt (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_A,
            [this]() {
                spdlog::info("[Application] A key - triggering test action prompt");
                m_session.routing().action_prompt_manager()->trigger_test_prompt();
            },
            [this]() {
                return get_runtime_config()->is_test_mode() &&
                       m_session.routing().action_prompt_manager();
            });

        // N key - test action notification (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_N,
            [this]() {
                spdlog::info("[Application] N key - triggering test action notification");
                m_session.routing().action_prompt_manager()->trigger_test_notify();
            },
            [this]() {
                return get_runtime_config()->is_test_mode() &&
                       m_session.routing().action_prompt_manager();
            });

        // Android back button — pop navigation stack (overlay/modal/panel)
        // At root panel, do nothing (Android convention: don't exit on back)
        shortcuts.register_key(SDL_SCANCODE_AC_BACK, []() {
            auto& nav = NavigationManager::instance();
            if (nav.go_back()) {
                spdlog::debug("[Application] Android back button - popped navigation");
            } else {
                spdlog::trace("[Application] Android back button - at root, ignoring");
            }
        });

#ifdef HELIX_ENABLE_SCREENSAVER
        // Z key - cycle through screensavers (Off → Toasters → Starfield → Pipes → Off)
        shortcuts.register_key(SDL_SCANCODE_Z, []() {
            auto& mgr = ScreensaverManager::instance();
            if (mgr.is_active()) {
                mgr.stop();
                spdlog::info("[Application] Z key - screensaver stopped");
            } else {
                auto type = ScreensaverManager::configured_type();
                if (type == ScreensaverType::OFF) {
                    type = ScreensaverType::FLYING_TOASTERS;
                }
                mgr.start(type);
                spdlog::info("[Application] Z key - screensaver started (type {})",
                             static_cast<int>(type));
            }
        });
#endif

        shortcuts_initialized = true;
    }

    // Suppress plain-key shortcuts when a textarea has focus (e.g., typing a password)
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    bool text_input_active = focused != nullptr && lv_obj_check_type(focused, &lv_textarea_class);

    // Process shortcuts with SDL key state
    const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
    shortcuts.process([keyboard_state](int scancode) { return keyboard_state[scancode] != 0; },
                      SDL_GetModState(), text_input_active);
#endif
}
