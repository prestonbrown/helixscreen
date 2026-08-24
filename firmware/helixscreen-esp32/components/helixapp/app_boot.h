// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The real HelixScreen shell bring-up (Plan 4 Task 6). Replaces the Task-1
// hello-card. app_boot_ui() mirrors the desktop Application startup phases
// without SDL/CLI and leaves the navbar + resident panels live on the active
// screen; app_boot_tick() is pumped once per render-loop iteration to drain
// the Moonraker notification/timeout queues on the UI thread (the same work
// Application does in its main loop). Both run on the UI pthread created in
// app_main — the app core calls std::this_thread::get_id() (spdlog, main-thread
// detectors), which asserts on a raw FreeRTOS task, so a pthread context is
// mandatory (proven by the Plan 2 native-audit app slice).

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Record whether the touch controller came up, so app_boot_ui() can surface a
// warning when it did not. Called from the display bring-up in main/ before
// app_boot_ui() runs. The result is pushed down rather than helixapp querying
// main/touch_input.c directly: main depends on helixapp, not the reverse, and
// helixapp does not have main/ on its include path.
void app_boot_set_touch_available(bool available);

// Build the shell. Runs once on the UI thread before the render loop starts.
void app_boot_ui(void);

// Pump per render-loop iteration. Drains queued Moonraker notifications +
// request timeouts on the UI thread. No-op until app_boot_ui() has run.
void app_boot_tick(void);

#ifdef __cplusplus
}
#endif
