// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_lcd_panel_ops.h"

// Spawns the UI pthread and returns immediately. The pthread body does the
// ENTIRE UI bring-up — board_display_init(), lv_init + lv_xml_init (LVGL does
// NOT call the latter; skipping it is heap-corruption-shaped TLSF crashes), the
// display with static internal-DRAM draw buffers, touch, ui_build() once, then the
// lv_timer_handler loop (ui_tick, if non-NULL, runs each iteration after the
// timer handler). Everything that touches the panel/LVGL/app code runs on that
// pthread — the app core calls std::this_thread::get_id() (spdlog, main-thread
// detectors), which asserts on a raw FreeRTOS task, so a pthread context is
// mandatory.
//
// CALL THIS FIRST in app_main — before spawning any network task. pthread_create
// is the boot's first sizeable internal-heap allocation (the 48KB UI stack);
// the panel's 32KB RGB bounce DMA buffer is the second gate, inside the thread
// body. Both must find contiguous internal DRAM; with the app core's static
// footprint that only holds once the large statics are moved to PSRAM (see
// lvgl_glue.c / sdkconfig.defaults). Deferring the pthread behind the net task's
// WiFi startup would make it a boot lottery on top of the DRAM pressure.
void lvgl_glue_start(void (*ui_build)(void), void (*ui_tick)(void));
