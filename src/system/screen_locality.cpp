// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_locality.h"

#include "host_identity.h"

namespace helix {

const char* screen_locality_for_host(std::string_view host) {
    if (host.empty()) {
        return "remote";
    }
    return is_moonraker_on_same_host(host) ? "local" : "remote";
}

} // namespace helix
