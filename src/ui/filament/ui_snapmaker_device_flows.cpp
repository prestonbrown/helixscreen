// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_device_flow.h"

#if HELIX_HAS_SNAPMAKER

#include "ui_snapmaker_nozzle_offset_overlay.h"

#include "snapmaker_nozzle_offset.h"

namespace helix::ui {

void register_snapmaker_device_flows() {
    // A guided overlay with its own step machine, like the CFS purge chute.
    register_guided_device_flow(snapmaker::nozzle_offset::kGuidedFlowId,
                                {nullptr, [](const std::string&) {
                                     get_snapmaker_nozzle_offset_overlay().open(lv_screen_active());
                                 }});
}

} // namespace helix::ui

#else

namespace helix::ui {
void register_snapmaker_device_flows() {}
} // namespace helix::ui

#endif
