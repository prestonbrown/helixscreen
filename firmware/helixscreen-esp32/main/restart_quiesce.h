// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Registers the shutdown handler that parks the other CPU in IRAM, with its
// interrupts off, at the end of esp_restart(). Call it from app_main before
// anything else registers one: esp_restart runs the handlers newest first, so
// this one then runs after the panel and WiFi handlers that still need both
// CPUs.
void restart_quiesce_register(void);

#ifdef __cplusplus
}
#endif
