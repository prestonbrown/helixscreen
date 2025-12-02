// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file app_globals.cpp
 * @brief Global application state and accessors
 *
 * Provides centralized access to global singleton instances like MoonrakerClient,
 * PrinterState, and reactive subjects. This module exists to:
 * 1. Keep main.cpp cleaner and more focused
 * 2. Provide a single point of truth for global state
 * 3. Make it easier to add new global subjects/singletons
 */

#include "app_globals.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "ui_modal.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

// Platform-specific includes for process restart
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // fork, execv, usleep
#endif

// Global singleton instances (extern declarations in header, definitions here)
// These are set by main.cpp during initialization
static MoonrakerClient* g_moonraker_client = nullptr;
static MoonrakerAPI* g_moonraker_api = nullptr;

// Global reactive subjects
static lv_subject_t g_notification_subject;

// Application quit flag
static bool g_quit_requested = false;

// Stored command-line arguments for restart capability
static std::vector<char*> g_stored_argv;
static std::string g_executable_path;

MoonrakerClient* get_moonraker_client() {
    return g_moonraker_client;
}

void set_moonraker_client(MoonrakerClient* client) {
    g_moonraker_client = client;
}

MoonrakerAPI* get_moonraker_api() {
    return g_moonraker_api;
}

void set_moonraker_api(MoonrakerAPI* api) {
    g_moonraker_api = api;
}

PrinterState& get_printer_state() {
    // Singleton instance - created once, lives for lifetime of program
    static PrinterState instance;
    return instance;
}

lv_subject_t& get_notification_subject() {
    return g_notification_subject;
}

void app_globals_init_subjects() {
    // Initialize notification subject (stores NotificationData pointer)
    lv_subject_init_pointer(&g_notification_subject, nullptr);

    // Initialize modal dialog subjects (for modal_dialog.xml binding)
    ui_modal_init_subjects();

    spdlog::debug("Global subjects initialized");
}

void app_store_argv(int argc, char** argv) {
    // Store a copy of argv for restart capability
    g_stored_argv.clear();

    if (argc > 0 && argv && argv[0]) {
        // Store executable path (argv[0] might be relative, so we keep it as-is)
        g_executable_path = argv[0];

        // Copy all arguments
        for (int i = 0; i < argc; ++i) {
            if (argv[i]) {
                g_stored_argv.push_back(strdup(argv[i]));
            }
        }
        // execv requires NULL-terminated array
        g_stored_argv.push_back(nullptr);

        spdlog::debug("Stored {} command-line arguments for restart capability", argc);
    }
}

void app_request_quit() {
    spdlog::info("Application quit requested");
    g_quit_requested = true;
}

void app_request_restart() {
    spdlog::info("Application restart requested");

    if (g_stored_argv.empty() || g_executable_path.empty()) {
        spdlog::error("Cannot restart: argv not stored. Call app_store_argv() at startup.");
        g_quit_requested = true;  // Fall back to quit
        return;
    }

#if defined(__unix__) || defined(__APPLE__)
    // Fork a new process
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        spdlog::error("Fork failed during restart: {}", strerror(errno));
        g_quit_requested = true;  // Fall back to quit
        return;
    }

    if (pid == 0) {
        // Child process - exec the new instance
        // Small delay to let parent start cleanup
        usleep(100000);  // 100ms

        execv(g_executable_path.c_str(), g_stored_argv.data());

        // If execv returns, it failed
        spdlog::error("execv failed during restart: {}", strerror(errno));
        _exit(1);  // Exit child without cleanup
    }

    // Parent process - signal main loop to exit cleanly
    spdlog::info("Forked new process (PID {}), parent exiting", pid);
    g_quit_requested = true;

#else
    // Unsupported platform - fall back to quit
    spdlog::warn("Restart not supported on this platform, falling back to quit");
    g_quit_requested = true;
#endif
}

/**
 * @brief Build modified argv for theme restart
 *
 * Filters out --dark/--light and replaces -p/--panel with "-p settings"
 */
static std::vector<char*> build_theme_restart_argv() {
    std::vector<char*> new_argv;

    bool skip_next = false;
    bool panel_added = false;

    for (size_t i = 0; i < g_stored_argv.size(); ++i) {
        char* arg = g_stored_argv[i];
        if (!arg) {
            continue;  // Skip null terminator during iteration
        }

        // Skip if previous arg was -p/--panel (this is the panel value)
        if (skip_next) {
            skip_next = false;
            continue;
        }

        // Skip --dark and --light
        if (strcmp(arg, "--dark") == 0 || strcmp(arg, "--light") == 0) {
            continue;
        }

        // Handle -p/--panel: skip it and its value, we'll add our own
        if (strcmp(arg, "-p") == 0 || strcmp(arg, "--panel") == 0) {
            skip_next = true;
            continue;
        }

        // Handle --panel=value or -p=value style
        if (strncmp(arg, "--panel=", 8) == 0 || strncmp(arg, "-p=", 3) == 0) {
            continue;
        }

        new_argv.push_back(arg);

        // After adding argv[0] (the executable), add -p settings
        if (i == 0 && !panel_added) {
            new_argv.push_back(strdup("-p"));
            new_argv.push_back(strdup("settings"));
            panel_added = true;
        }
    }

    // If no panel was added (edge case: empty argv), add it now
    if (!panel_added && !new_argv.empty()) {
        new_argv.push_back(strdup("-p"));
        new_argv.push_back(strdup("settings"));
    }

    // Null-terminate for execv
    new_argv.push_back(nullptr);

    return new_argv;
}

void app_request_restart_for_theme() {
    spdlog::info("Application restart requested for theme change");

    if (g_stored_argv.empty() || g_executable_path.empty()) {
        spdlog::error("Cannot restart: argv not stored. Call app_store_argv() at startup.");
        g_quit_requested = true;
        return;
    }

    // Build modified argv (removes --dark/--light, forces -p settings)
    std::vector<char*> theme_argv = build_theme_restart_argv();

    // Log the modified command line
    std::string cmd_line;
    for (char* arg : theme_argv) {
        if (arg) {
            if (!cmd_line.empty()) cmd_line += " ";
            cmd_line += arg;
        }
    }
    spdlog::info("Restart command: {}", cmd_line);

#if defined(__unix__) || defined(__APPLE__)
    pid_t pid = fork();

    if (pid < 0) {
        spdlog::error("Fork failed during theme restart: {}", strerror(errno));
        g_quit_requested = true;
        return;
    }

    if (pid == 0) {
        // Child process
        usleep(100000);  // 100ms delay for parent cleanup
        execv(g_executable_path.c_str(), theme_argv.data());
        spdlog::error("execv failed during theme restart: {}", strerror(errno));
        _exit(1);
    }

    spdlog::info("Forked new process (PID {}) for theme restart, parent exiting", pid);
    g_quit_requested = true;

#else
    spdlog::warn("Theme restart not supported on this platform, falling back to quit");
    g_quit_requested = true;
#endif
}

bool app_quit_requested() {
    return g_quit_requested;
}
