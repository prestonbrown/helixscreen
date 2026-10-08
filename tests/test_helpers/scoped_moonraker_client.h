// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app_globals.h"

/// Installs a caller-owned client as the process-global Moonraker client for
/// a scope and restores the previous one.
///
/// The client pointer is a process global that production code reads through
/// get_moonraker_client(), so a test that installs a stack client and never
/// puts it back leaves every later test in the process reading a dangling
/// pointer. RAII rather than a restore at the end of the test body: Catch2
/// aborts a failing REQUIRE by throwing, so a trailing restore is skipped
/// exactly when a test has already gone wrong.
///
/// The client stays caller-owned — construct it next to the guard, so its
/// lifetime encloses the guard's:
///
/// ```cpp
/// MoonrakerClientMock client;
/// client.set_heaters({"extruder", "heater_bed"});
/// ScopedMoonrakerClient installed(&client);
/// ```
class ScopedMoonrakerClient {
  public:
    explicit ScopedMoonrakerClient(helix::IMoonrakerClient* client)
        : previous_(get_moonraker_client()) {
        set_moonraker_client(client);
    }
    ~ScopedMoonrakerClient() {
        set_moonraker_client(previous_);
    }

    ScopedMoonrakerClient(const ScopedMoonrakerClient&) = delete;
    ScopedMoonrakerClient& operator=(const ScopedMoonrakerClient&) = delete;
    ScopedMoonrakerClient(ScopedMoonrakerClient&&) = delete;
    ScopedMoonrakerClient& operator=(ScopedMoonrakerClient&&) = delete;

  private:
    helix::IMoonrakerClient* previous_;
};
