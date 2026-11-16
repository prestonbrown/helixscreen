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
 * any LVGL type exists, and again from the SW draw sources, inside the
 * lv_draw_sw_blend.h / lv_draw_sw_mask.h include cycle where the blend dsc
 * structs are not complete yet. So the hook macros are defined on whichever pass
 * comes first, the wrapper prototypes only once LVGL's types exist (LV_UNUSED is
 * defined in lv_types.h after them), and the bodies live in
 * esp_lvgl_port_lv_blend.c.
 *
 * Every hook names esp_lvgl_port_lv_blend_declared, which only the prototype
 * pass declares: a blend source that expands a hook without the prototypes in
 * scope fails to compile instead of falling back to C blending. */

#if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888(dsc)                                                    \
    ((void)esp_lvgl_port_lv_blend_declared, _lv_color_blend_to_argb8888_esp(dsc))
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB565
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc)                                                      \
    ((void)esp_lvgl_port_lv_blend_declared, _lv_color_blend_to_rgb565_esp(dsc))
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB888
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888(dsc, dest_px_size)                                        \
    ((void)esp_lvgl_port_lv_blend_declared, _lv_color_blend_to_rgb888_esp(dsc, dest_px_size))
#endif

#ifndef LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc)                                              \
    ((void)esp_lvgl_port_lv_blend_declared, _lv_rgb565_blend_normal_to_rgb565_esp(dsc))
#endif

#ifndef LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888
#define LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888(dsc, dest_px_size, src_px_size)                   \
    ((void)esp_lvgl_port_lv_blend_declared,                                                        \
     _lv_rgb888_blend_normal_to_rgb888_esp(dsc, dest_px_size, src_px_size))
#endif

#else
#warning                                                                                           \
    "esp_lvgl_port_lv_blend.h included, but LV_USE_DRAW_SW_ASM is not LV_DRAW_SW_ASM_CUSTOM. Assembly rendering not used"
#endif /* LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM */

#if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM && defined(LV_UNUSED) &&                         \
    !defined(ESP_LVGL_PORT_LV_BLEND_H)
#define ESP_LVGL_PORT_LV_BLEND_H

#ifdef __cplusplus
extern "C" {
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

enum { esp_lvgl_port_lv_blend_declared = 1 };

/* Defined in esp_lvgl_port_lv_blend.c, which has the dsc structs complete. */
lv_result_t _lv_color_blend_to_argb8888_esp(lv_draw_sw_blend_fill_dsc_t* dsc);
lv_result_t _lv_color_blend_to_rgb565_esp(lv_draw_sw_blend_fill_dsc_t* dsc);
lv_result_t _lv_color_blend_to_rgb888_esp(lv_draw_sw_blend_fill_dsc_t* dsc, uint32_t dest_px_size);
lv_result_t _lv_rgb565_blend_normal_to_rgb565_esp(lv_draw_sw_blend_image_dsc_t* dsc);
lv_result_t _lv_rgb888_blend_normal_to_rgb888_esp(lv_draw_sw_blend_image_dsc_t* dsc,
                                                  uint32_t dest_px_size, uint32_t src_px_size);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ESP_LVGL_PORT_LV_BLEND_H*/
