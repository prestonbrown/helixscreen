// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hardware_validator.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "job_queue_state.h"
#include "lvgl/lvgl.h"
#include "printer_discovery.h"

#include <cstddef>
#include <optional>

#include "hv/json.hpp"

namespace helix {

class HardwareSetupPrompter;

/**
 * @file discovery_steps.h
 * @brief What the app does each time Moonraker discovery completes, as ordered tables.
 *
 * wire_discovery() (session_wiring.h) walks the core table (discovery_steps_core.cpp) on
 * the main thread on every build, the ESP32 firmware included; desktop runs the tail
 * (discovery_steps.cpp) after it. No tail step has to precede a core
 * step. The order is the contract: later steps read what earlier ones stored, so the
 * tables are spelled out rather than self-registered.
 */

/// What a step may read. Built once per discovery pass, after the hardware has been
/// copied into the API and fingerprinted.
struct DiscoveryContext {
    IMoonrakerAPI& api;
    IMoonrakerClient& client;
    /// api.hardware(): the hardware this pass discovered. Steps read hardware from here,
    /// never from `snapshot`.
    const PrinterDiscovery& hw;
    /// The BG-thread copy of the hardware. set_hardware_step moves it into PrinterState,
    /// after which it is empty.
    PrinterDiscovery& snapshot;
    /// Subscription status that arrived with this discovery, replayed once as a cached
    /// snapshot.
    const nlohmann::json& status;
    /// Null where the session has no setup wizard (the firmware); only the tail reads it.
    HardwareSetupPrompter* prompter;
    JobQueueState* job_queue_state;
    lv_obj_t* screen;
    /// Invocation number of this pass; every "disc" breadcrumb carries it.
    long n;
    /// The hardware shape differs from the previous discovery of this printer session.
    bool hw_changed;
    /// A print is running. Computed once for the pass: the print_active subject is not
    /// updated from this discovery's status until a tick after the pass ends, so a
    /// mid-print reconnect must read the status itself.
    bool print_active;

    /// Written by the acknowledge step, read by the prompt step.
    bool hardware_setup_deferred = false;
    /// Created by the validate step; the snapshot step closes it after the prompts.
    std::optional<HardwareValidator> validator;
};

struct DiscoveryStep {
    /// Stable name, for logs and for the table's own tests.
    const char* name;
    /// Skipped, with its breadcrumb still recorded, when the hardware shape is unchanged
    /// since the last discovery: the step is a pure function of the hardware shape.
    bool only_when_hw_changed;
    void (*run)(DiscoveryContext&);
    /// "disc" breadcrumb recorded after the step (and when it was skipped); may be null.
    const char* breadcrumb;
};

/// A contiguous run of steps.
struct DiscoveryStepRange {
    const DiscoveryStep* first;
    const DiscoveryStep* last;
    const DiscoveryStep* begin() const {
        return first;
    }
    const DiscoveryStep* end() const {
        return last;
    }
    size_t size() const {
        return static_cast<size_t>(last - first);
    }
};

/// The steps every build runs, in order.
DiscoveryStepRange discovery_core_steps();

/// The desktop-only steps, run after the core ones.
DiscoveryStepRange discovery_tail_steps();

/// Run @p steps in order against @p ctx. A step marked only_when_hw_changed is skipped
/// when ctx.hw_changed is false; its breadcrumb is recorded either way.
void run_discovery_steps(DiscoveryStepRange steps, DiscoveryContext& ctx);

/// Whether a print is running during this discovery pass. The status that arrived with
/// the discovery wins over the print_active subject, which still holds its pre-connect
/// value on a fresh connection.
bool discovery_print_active(bool subject_active, const nlohmann::json& status);

} // namespace helix
