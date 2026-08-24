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
 * not the managed-component Kconfig.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#if LV_USE_DRAW_SW_ASM != LV_DRAW_SW_ASM_CUSTOM
#warning "esp_lvgl_port_lv_blend.h included, but LV_USE_DRAW_SW_ASM is not LV_DRAW_SW_ASM_CUSTOM. Assembly rendering not used"
#else

/* Local change vs upstream: LVGL includes this hook header from more TUs than
 * just the blend ones (e.g. lv_draw_buf_convert.c), where the blend dsc
 * structs are only forward-declared. Pull in their full definitions here so
 * the static-inline wrappers below compile everywhere. lib/lvgl is on the
 * include path. */
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

/*********************
 *      DEFINES
 *********************/

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888(dsc) \
    _lv_color_blend_to_argb8888_esp(dsc)
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB565
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) \
    _lv_color_blend_to_rgb565_esp(dsc)
#endif

#ifndef LV_DRAW_SW_COLOR_BLEND_TO_RGB888
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888(dsc, dest_px_size) \
    _lv_color_blend_to_rgb888_esp(dsc, dest_px_size)
#endif

#ifndef LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc)  \
    _lv_rgb565_blend_normal_to_rgb565_esp(dsc)
#endif

#ifndef LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888
#define LV_DRAW_SW_RGB888_BLEND_NORMAL_TO_RGB888(dsc, dest_px_size, src_px_size)  \
    _lv_rgb888_blend_normal_to_rgb888_esp(dsc, dest_px_size, src_px_size)
#endif

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    uint32_t opa;
    void *dst_buf;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_stride;
    const void *src_buf;
    uint32_t src_stride;
    const lv_opa_t *mask_buf;
    uint32_t mask_stride;
} asm_dsc_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

extern int lv_color_blend_to_argb8888_esp(asm_dsc_t *asm_dsc);

static inline lv_result_t _lv_color_blend_to_argb8888_esp(lv_draw_sw_blend_fill_dsc_t *dsc)
{
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &dsc->color,
    };

    return lv_color_blend_to_argb8888_esp(&asm_dsc);
}

extern int lv_color_blend_to_rgb565_esp(asm_dsc_t *asm_dsc);

static inline lv_result_t _lv_color_blend_to_rgb565_esp(lv_draw_sw_blend_fill_dsc_t *dsc)
{
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &dsc->color,
    };

    return lv_color_blend_to_rgb565_esp(&asm_dsc);
}

extern int lv_color_blend_to_rgb888_esp(asm_dsc_t *asm_dsc);

static inline lv_result_t _lv_color_blend_to_rgb888_esp(lv_draw_sw_blend_fill_dsc_t *dsc, uint32_t dest_px_size)
{
    if (dest_px_size != 3) {
        return LV_RESULT_INVALID;
    }
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &dsc->color,
    };

    return lv_color_blend_to_rgb888_esp(&asm_dsc);
}

extern int lv_rgb565_blend_normal_to_rgb565_esp(asm_dsc_t *asm_dsc);

static inline lv_result_t _lv_rgb565_blend_normal_to_rgb565_esp(lv_draw_sw_blend_image_dsc_t *dsc)
{
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = dsc->src_buf,
        .src_stride = dsc->src_stride
    };

    return lv_rgb565_blend_normal_to_rgb565_esp(&asm_dsc);
}

extern int lv_rgb888_blend_normal_to_rgb888_esp(asm_dsc_t *asm_dsc);

static inline lv_result_t _lv_rgb888_blend_normal_to_rgb888_esp(lv_draw_sw_blend_image_dsc_t *dsc,
        uint32_t dest_px_size, uint32_t src_px_size)
{
    if (!(dest_px_size == 3 && src_px_size == 3)) {
        return LV_RESULT_INVALID;
    }

    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = dsc->src_buf,
        .src_stride = dsc->src_stride
    };

    return lv_rgb888_blend_normal_to_rgb888_esp(&asm_dsc);
}

#endif // CONFIG_LV_DRAW_SW_ASM_CUSTOM

#ifdef __cplusplus
} /*extern "C"*/
#endif
