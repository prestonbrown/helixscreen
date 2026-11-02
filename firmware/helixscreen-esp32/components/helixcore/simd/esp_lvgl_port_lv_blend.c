/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The wrapper bodies of esp_lvgl_port_lv_blend.h (vendored from espressif/esp-bsp
 * components/esp_lvgl_port, commit 74e3cd2), moved out of the header so they compile
 * where the blend dsc structs are complete. They adapt LVGL's blend descriptors to the
 * ESP32-S3 PIE assembly in the sibling *_esp32s3.S files.
 */

#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

#if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM

#include "esp_lvgl_port_lv_blend.h"

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    uint32_t opa;
    void* dst_buf;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_stride;
    const void* src_buf;
    uint32_t src_stride;
    const lv_opa_t* mask_buf;
    uint32_t mask_stride;
} asm_dsc_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

extern int lv_color_blend_to_argb8888_esp(asm_dsc_t* asm_dsc);

lv_result_t _lv_color_blend_to_argb8888_esp(lv_draw_sw_blend_fill_dsc_t* dsc) {
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &dsc->color,
    };

    return lv_color_blend_to_argb8888_esp(&asm_dsc);
}

extern int lv_color_blend_to_rgb565_esp(asm_dsc_t* asm_dsc);

lv_result_t _lv_color_blend_to_rgb565_esp(lv_draw_sw_blend_fill_dsc_t* dsc) {
    asm_dsc_t asm_dsc = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &dsc->color,
    };

    return lv_color_blend_to_rgb565_esp(&asm_dsc);
}

extern int lv_color_blend_to_rgb888_esp(asm_dsc_t* asm_dsc);

lv_result_t _lv_color_blend_to_rgb888_esp(lv_draw_sw_blend_fill_dsc_t* dsc,
                                                        uint32_t dest_px_size) {
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

extern int lv_rgb565_blend_normal_to_rgb565_esp(asm_dsc_t* asm_dsc);

lv_result_t _lv_rgb565_blend_normal_to_rgb565_esp(lv_draw_sw_blend_image_dsc_t* dsc) {
    asm_dsc_t asm_dsc = {.dst_buf = dsc->dest_buf,
                         .dst_w = dsc->dest_w,
                         .dst_h = dsc->dest_h,
                         .dst_stride = dsc->dest_stride,
                         .src_buf = dsc->src_buf,
                         .src_stride = dsc->src_stride};

    return lv_rgb565_blend_normal_to_rgb565_esp(&asm_dsc);
}

extern int lv_rgb888_blend_normal_to_rgb888_esp(asm_dsc_t* asm_dsc);

lv_result_t _lv_rgb888_blend_normal_to_rgb888_esp(lv_draw_sw_blend_image_dsc_t* dsc,
                                                                uint32_t dest_px_size,
                                                                uint32_t src_px_size) {
    if (!(dest_px_size == 3 && src_px_size == 3)) {
        return LV_RESULT_INVALID;
    }

    asm_dsc_t asm_dsc = {.dst_buf = dsc->dest_buf,
                         .dst_w = dsc->dest_w,
                         .dst_h = dsc->dest_h,
                         .dst_stride = dsc->dest_stride,
                         .src_buf = dsc->src_buf,
                         .src_stride = dsc->src_stride};

    return lv_rgb888_blend_normal_to_rgb888_esp(&asm_dsc);
}

#endif /* LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM */
