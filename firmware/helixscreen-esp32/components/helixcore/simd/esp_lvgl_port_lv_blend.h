/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Vendored from espressif/esp-bsp components/esp_lvgl_port (commit 74e3cd2),
 * include/esp_lvgl_port_lv_blend.h, together with the sibling simd assembly files
 * ESP32-S3 PIE assembly. Local change: the CONFIG_LV_DRAW_SW_ASM_CUSTOM
 * Kconfig guard below is replaced with the lv_conf.h-driven
 * LV_USE_DRAW_SW_ASM check — this project configures LVGL via lv_conf.h,
 * not the managed-component Kconfig; and #pragma once is replaced with the two-pass
 * guard below.
 */

/* LVGL 9.6 includes this header twice: at the end of lv_conf_internal.h, before
 * any LVGL type exists, and again from the SW draw sources. Only the second pass
 * can compile the wrappers below, so the first does nothing and leaves the guard
 * unset. LV_UNUSED is defined in lv_types.h after its types. */
#if defined(LV_UNUSED) && !defined(ESP_LVGL_PORT_LV_BLEND_H)
#define ESP_LVGL_PORT_LV_BLEND_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#if LV_USE_DRAW_SW_ASM != LV_DRAW_SW_ASM_CUSTOM
#warning                                                                                           \
    "esp_lvgl_port_lv_blend.h included, but LV_USE_DRAW_SW_ASM is not LV_DRAW_SW_ASM_CUSTOM. Assembly rendering not used"
#else

/* Local change vs upstream: LVGL includes this header from lv_draw_sw_blend.h,
 * part of an include cycle with lv_draw_sw_mask.h, where the blend dsc structs
 * are not complete yet. So it declares the wrappers only; their bodies live in
 * esp_lvgl_port_lv_blend.c. */

/*********************
 *      DEFINES
 *********************/

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888(dsc) _lv_color_blend_to_argb8888_esp(dsc)
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB565
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) _lv_color_blend_to_rgb565_esp(dsc)
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB888
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888(dsc, dest_px_size)                                        \
    _lv_color_blend_to_rgb888_esp(dsc, dest_px_size)
#endif

#ifndef LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) _lv_rgb565_blend_normal_to_rgb565_esp(dsc)
#endif

#ifndef LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888
#define LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888(dsc, dest_px_size, src_px_size)                   \
    _lv_rgb888_blend_normal_to_rgb888_esp(dsc, dest_px_size, src_px_size)
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/* Defined in esp_lvgl_port_lv_blend.c, which has the dsc structs complete. */
lv_result_t _lv_color_blend_to_argb8888_esp(lv_draw_sw_blend_fill_dsc_t* dsc);
lv_result_t _lv_color_blend_to_rgb565_esp(lv_draw_sw_blend_fill_dsc_t* dsc);
lv_result_t _lv_color_blend_to_rgb888_esp(lv_draw_sw_blend_fill_dsc_t* dsc, uint32_t dest_px_size);
lv_result_t _lv_rgb565_blend_normal_to_rgb565_esp(lv_draw_sw_blend_image_dsc_t* dsc);
lv_result_t _lv_rgb888_blend_normal_to_rgb888_esp(lv_draw_sw_blend_image_dsc_t* dsc,
                                                  uint32_t dest_px_size, uint32_t src_px_size);

#endif // CONFIG_LV_DRAW_SW_ASM_CUSTOM

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ESP_LVGL_PORT_LV_BLEND_H*/
