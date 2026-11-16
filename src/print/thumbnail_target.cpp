// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The thumbnail size decision, kept apart from ThumbnailProcessor's decode
// pipeline so builds without that pipeline size thumbnails by the same rule.

#include "lvgl.h"
#include "thumbnail_processor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>

namespace helix {

// LVGL 9 color format constant (magic comes from lv_image_dsc.h)
static constexpr uint8_t kColorFormatArgb8888 = 0x10;

namespace {

// Card size measured by PrintSelectPanel, or 0 when no card grid has reported one.
// Read from the thumbnail worker threads while a fetch picks its target.
std::atomic<int> g_card_hint_width{0};
std::atomic<int> g_card_hint_height{0};

} // namespace

void ThumbnailProcessor::set_card_size_hint(int card_width, int card_height) {
    if (card_width <= 0 || card_height <= 0) {
        g_card_hint_width.store(0, std::memory_order_relaxed);
        g_card_hint_height.store(0, std::memory_order_relaxed);
        return;
    }
    g_card_hint_width.store(card_width, std::memory_order_relaxed);
    g_card_hint_height.store(card_height, std::memory_order_relaxed);
}

ThumbnailTarget ThumbnailProcessor::get_target_for_card(int card_width, int card_height) {
    ThumbnailTarget target;
    target.color_format = kColorFormatArgb8888;

    if (card_width <= 0 || card_height <= 0) {
        target.width = 120;
        target.height = 120;
        return target;
    }

    // preview_offset_y in globals.xml lifts the art by 12% of its own height, so a
    // square of side N clears the card's top edge only while (H - N)/2 >= 0.12N.
    int height_bound = static_cast<int>(card_height / 1.24);
    int side = std::min(card_width, height_bound);

    side -= side % 4; // bound the number of distinct .bin cache entries
    side = std::clamp(side, 64, 220);

    target.width = side;
    target.height = side;
    return target;
}

ThumbnailTarget ThumbnailProcessor::get_target_for_resolution(int width, int height,
                                                              ThumbnailSize size) {
    ThumbnailTarget target;
    target.color_format = kColorFormatArgb8888;

    // Defensive: treat invalid dimensions as smallest breakpoint
    if (width <= 0 || height <= 0) {
        target.width = (size == ThumbnailSize::Detail) ? 200 : 120;
        target.height = target.width;
        return target;
    }

    int greater_res = std::max(width, height);

    if (size == ThumbnailSize::Detail) {
        // Detail view sizes — larger for status panel / detail overlay
        if (greater_res <= 480) {
            target.width = 200;
            target.height = 200;
        } else if (greater_res <= 800) {
            target.width = 300;
            target.height = 300;
        } else {
            target.width = 400;
            target.height = 400;
        }
    } else {
        // Card view sizes — small thumbnails for file lists
        if (greater_res <= 480) {
            // SMALL: 480x320 class → card ~107px → target 120x120
            target.width = 120;
            target.height = 120;
        } else if (greater_res <= 800) {
            // MEDIUM: 800x480 class (AD5M) → card ~151px → target 160x160
            target.width = 160;
            target.height = 160;
        } else {
            // LARGE: 1024x600, 1280x720+ → card ~205px → target 220x220
            target.width = 220;
            target.height = 220;
        }
    }

    return target;
}

ThumbnailTarget ThumbnailProcessor::get_target_for_display(ThumbnailSize size) {
    // A measured card beats guessing one from the display resolution. Card art has to
    // fit the box the grid actually built, or LVGL crops the model (#1208).
    if (size == ThumbnailSize::Card) {
        int hint_w = g_card_hint_width.load(std::memory_order_relaxed);
        int hint_h = g_card_hint_height.load(std::memory_order_relaxed);
        if (hint_w > 0 && hint_h > 0) {
            ThumbnailTarget target = get_target_for_card(hint_w, hint_h);
            spdlog::trace("[ThumbnailProcessor] Card {}x{} → target {}x{} (measured)", hint_w,
                          hint_h, target.width, target.height);
            return target;
        }
    }

    lv_display_t* display = lv_display_get_default();
    if (!display) {
        // Fallback if no display initialized yet (shouldn't happen in normal use)
        spdlog::debug("[ThumbnailProcessor] No display available, using medium defaults");
        return get_target_for_resolution(800, 480, size);
    }

    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    ThumbnailTarget target = get_target_for_resolution(hor_res, ver_res, size);

    const char* size_str = (size == ThumbnailSize::Detail) ? "detail" : "card";
    spdlog::trace("[ThumbnailProcessor] Display {}x{} → target {}x{} ({}, ARGB8888)", hor_res,
                  ver_res, target.width, target.height, size_str);

    return target;
}

} // namespace helix
