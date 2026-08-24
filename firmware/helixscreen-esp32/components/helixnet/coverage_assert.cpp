// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-time proof that EspMoonrakerClient fully implements the Task-2
// IMoonrakerClient contract. If any pure virtual is left unimplemented (or a
// signature drifts), the class stays abstract and this TU fails to compile —
// the done-bar's "class fully implements the interface" gate.

#include "esp_moonraker_client.h"

#include <type_traits>

static_assert(!std::is_abstract_v<helix::EspMoonrakerClient>,
              "EspMoonrakerClient must implement every IMoonrakerClient pure virtual");
static_assert(std::is_base_of_v<helix::IMoonrakerClient, helix::EspMoonrakerClient>,
              "EspMoonrakerClient must derive from IMoonrakerClient");
