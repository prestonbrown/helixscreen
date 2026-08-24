// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Cooperative boot-yield hook.
//
// On the ESP32 firmware the UI pthread runs the whole synchronous boot build
// (XML component registration + panel layout) at a higher priority than the
// idle task. A multi-second uninterrupted stretch starves idle, and the Task
// WDT — which watches the idle task — fires (TG1WDT_SYS_RST). Shared boot code
// calls HELIX_BOOT_YIELD() between units of work; on ESP it does one
// vTaskDelay(1) tick so idle runs and feeds the WDT (defined in the firmware's
// app_boot.cpp). On every other platform it compiles to nothing, so desktop and
// device behavior is byte-identical.
#if defined(HELIX_PLATFORM_ESP32)
extern "C" void helix_boot_yield(void);
#define HELIX_BOOT_YIELD() helix_boot_yield()
#else
#define HELIX_BOOT_YIELD() ((void)0)
#endif
