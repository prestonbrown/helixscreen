/*
 * TinyGL Dithering Verification Test
 *
 * Tests RGB_TO_PIXEL_COND macro in isolation before integration.
 * Verifies dithering math and runtime enable/disable toggle.
 *
 * Copyright (c) 2025 HelixScreen Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Include TinyGL dithering header */
#include "../../tinygl/src/zdither.h"

/* Define RGB_TO_PIXEL if not already defined (from zbuffer.h) */
#ifndef RGB_TO_PIXEL
#if TGL_FEATURE_RENDER_BITS == 32
#define RGB_TO_PIXEL(r,g,b) (((r) & 0xff0000) | (((g) >> 8) & 0xff00) | ((b) >> 16))
#elif TGL_FEATURE_RENDER_BITS == 16
#define RGB_TO_PIXEL(r,g,b) (((((r) >> 8) & 0xf800) | (((g) >> 13) & 0x7E0) | ((b) >> 19)))
#endif
#endif

/* Global dithering flag (defined in zdither.h) */
int tgl_dithering_enabled = 0;

/* Test helper: Check if two pixels are within threshold */
static int pixels_similar(unsigned int p1, unsigned int p2, int threshold) {
    int r1 = (p1 >> 16) & 0xff, g1 = (p1 >> 8) & 0xff, b1 = p1 & 0xff;
    int r2 = (p2 >> 16) & 0xff, g2 = (p2 >> 8) & 0xff, b2 = p2 & 0xff;
    int dr = abs(r1 - r2), dg = abs(g1 - g2), db = abs(b1 - b2);
    return (dr <= threshold && dg <= threshold && db <= threshold);
}

int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf("    TinyGL Dithering Verification Test\n");
    printf("═══════════════════════════════════════════════════\n\n");

    /* Test 1: Dithering disabled by default */
    printf("Test 1: Dithering disabled by default\n");
    tgl_dither_init();
    assert(tgl_dithering_enabled == 0);
    printf("  ✅ tgl_dithering_enabled = 0\n\n");

    /* Test 2: RGB_TO_PIXEL_COND with dithering disabled */
    printf("Test 2: RGB_TO_PIXEL_COND with dithering OFF\n");
    tgl_dithering_enabled = 0;

    unsigned int r = 128 << 16;  /* Mid-gray red */
    unsigned int g = 128 << 16;  /* Mid-gray green */
    unsigned int b = 128 << 16;  /* Mid-gray blue */

    unsigned int pixel_nodither = RGB_TO_PIXEL_COND(r, g, b, 0, 0);
    unsigned int expected_nodither = RGB_TO_PIXEL(r, g, b);

    printf("  RGB(128,128,128) at (0,0)\n");
    printf("  Result:   0x%06x\n", pixel_nodither);
    printf("  Expected: 0x%06x\n", expected_nodither);
    assert(pixel_nodither == expected_nodither);
    printf("  ✅ Non-dithered output matches RGB_TO_PIXEL\n\n");

    /* Test 3: RGB_TO_PIXEL_COND with dithering enabled */
    printf("Test 3: RGB_TO_PIXEL_COND with dithering ON\n");
    tgl_dithering_enabled = 1;

    unsigned int pixel_dither_00 = RGB_TO_PIXEL_COND(r, g, b, 0, 0);
    unsigned int pixel_dither_11 = RGB_TO_PIXEL_COND(r, g, b, 1, 1);
    unsigned int pixel_dither_22 = RGB_TO_PIXEL_COND(r, g, b, 2, 2);

    printf("  RGB(128,128,128) at (0,0): 0x%06x\n", pixel_dither_00);
    printf("  RGB(128,128,128) at (1,1): 0x%06x\n", pixel_dither_11);
    printf("  RGB(128,128,128) at (2,2): 0x%06x\n", pixel_dither_22);

    /* Pixels at different positions should vary (dithering adds noise) */
    assert(pixel_dither_00 != pixel_dither_11 || pixel_dither_11 != pixel_dither_22);
    printf("  ✅ Dithered pixels vary by position\n");

    /* But should be within reasonable range (±DITHER_AMPLITUDE) */
    assert(pixels_similar(pixel_dither_00, expected_nodither, 20));
    assert(pixels_similar(pixel_dither_11, expected_nodither, 20));
    assert(pixels_similar(pixel_dither_22, expected_nodither, 20));
    printf("  ✅ Dithered pixels within ±20 of original\n\n");

    /* Test 4: Runtime toggle works */
    printf("Test 4: Runtime enable/disable toggle\n");
    tgl_set_dithering(1);
    assert(tgl_dithering_enabled == 1);
    printf("  ✅ tgl_set_dithering(1) enabled\n");

    tgl_set_dithering(0);
    assert(tgl_dithering_enabled == 0);
    printf("  ✅ tgl_set_dithering(0) disabled\n\n");

    /* Test 5: Bayer pattern correctness */
    printf("Test 5: Bayer 4x4 pattern produces correct thresholds\n");
    tgl_dithering_enabled = 1;

    /* Test all 16 positions in 4x4 pattern */
    unsigned int prev_pixel = 0;
    int variation_count = 0;

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            unsigned int pixel = RGB_TO_PIXEL_COND(r, g, b, x, y);
            if (x > 0 || y > 0) {
                if (pixel != prev_pixel) variation_count++;
            }
            prev_pixel = pixel;
        }
    }

    /* At least half of the 16 positions should produce different values */
    printf("  Unique values in 4x4: %d/16\n", variation_count);
    assert(variation_count >= 8);
    printf("  ✅ Bayer pattern produces spatial variation\n\n");

    printf("═══════════════════════════════════════════════════\n");
    printf("  ✅ All dithering tests passed!\n");
    printf("═══════════════════════════════════════════════════\n");

    return 0;
}
