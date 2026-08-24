// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "splash_screen.h"

#include "ui_utils.h"

#include "helix_timing.h"
#include "helix_version.h"
#include "prerendered_images.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <lvgl.h>

namespace helix {

namespace {

/**
 * @brief Try to find a prerendered image at the deployed path or the build path
 *
 * On embedded devices, images are at assets/images/prerendered/ (deployed).
 * On desktop dev, images are at build/assets/images/prerendered/.
 * Returns LVGL path with A: prefix, or empty string if not found.
 */
std::string find_prerendered(const std::string& relative_path) {
    // Try deployed path first (embedded)
    if (std::filesystem::exists(relative_path)) {
        return "A:" + relative_path;
    }
    // Try build path (desktop development)
    std::string build_path = "build/" + relative_path;
    if (std::filesystem::exists(build_path)) {
        return "A:" + build_path;
    }
    return "";
}

} // namespace

void show_splash_screen(int screen_width, int screen_height) {
    spdlog::debug("[Splash Screen] Showing splash screen ({}x{})", screen_width, screen_height);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Apply theme background color (screen_bg runtime constant set by theme_manager_init)
    theme_manager_apply_bg_color(screen, "screen_bg", LV_PART_MAIN);

    // Disable scrollbars on screen
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Try full-screen 3D splash first (theme-aware dark/light variant)
    bool dark_mode = theme_manager_is_dark_mode();
    const char* size_name = get_splash_3d_size_name(screen_width, screen_height);
    const char* mode_name = dark_mode ? "dark" : "light";

    std::string splash_3d_path = find_prerendered(
        std::string("assets/images/prerendered/splash-3d-") + mode_name + "-" + size_name + ".bin");

    // Fallback: try base "tiny" if tiny_alt not found
    if (splash_3d_path.empty() && std::string(size_name) == "tiny_alt") {
        size_name = "tiny";
        splash_3d_path = find_prerendered(std::string("assets/images/prerendered/splash-3d-") +
                                          mode_name + "-tiny.bin");
    }

    // Safety: skip pre-rendered image if it would be taller than the screen
    if (!splash_3d_path.empty()) {
        int target_h = get_splash_3d_target_height(size_name);
        if (target_h > 0 && target_h > screen_height) {
            spdlog::debug("[Splash Screen] Pre-rendered {} ({}px) exceeds screen height {}px, "
                          "falling back to PNG",
                          size_name, target_h, screen_height);
            splash_3d_path.clear();
        }
    }

    // The widget we'll animate and clean up
    lv_obj_t* splash_widget = nullptr;

    // Also check for 3D source PNG fallback (now has alpha transparency)
    std::string splash_3d_png;
    if (splash_3d_path.empty()) {
        std::string png_rel =
            std::string("assets/images/helixscreen-logo-3d-") + mode_name + ".png";
        if (std::filesystem::exists(png_rel)) {
            splash_3d_png = "A:" + png_rel;
        }
    }

    if (!splash_3d_path.empty() || !splash_3d_png.empty()) {
        // 3D splash: prerendered bin (full-screen) or source PNG (centered + scaled)
        lv_obj_t* img = lv_image_create(screen);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(img, 0, LV_PART_MAIN);

        if (!splash_3d_path.empty()) {
            // Prerendered bin: full-screen, no scaling needed
            lv_image_set_src(img, splash_3d_path.c_str());
            spdlog::info("[Splash Screen] Using 3D splash ({}, {})", mode_name, size_name);
        } else {
            // Source PNG fallback: scale to fit screen
            lv_image_set_src(img, splash_3d_png.c_str());

            lv_image_header_t header;
            lv_result_t res = lv_image_decoder_get_info(splash_3d_png.c_str(), &header);
            if (res == LV_RESULT_OK && header.w > 0 && header.h > 0) {
                // Fit to screen with 10% vertical margin (5% top + 5% bottom)
                int usable_height = (screen_height * 9) / 10;
                uint32_t scale_w = (static_cast<uint32_t>(screen_width) * 256U) / header.w;
                uint32_t scale_h = (static_cast<uint32_t>(usable_height) * 256U) / header.h;
                uint32_t scale = (scale_w < scale_h) ? scale_w : scale_h;
                lv_image_set_scale(img, static_cast<uint16_t>(scale));
                spdlog::info("[Splash Screen] Using 3D PNG fallback ({}, {}x{} scale={})",
                             mode_name, static_cast<int>(header.w), static_cast<int>(header.h),
                             scale);
            } else {
                spdlog::warn("[Splash Screen] Could not get 3D PNG dimensions");
            }
        }

        lv_obj_center(img);
        lv_obj_set_style_opa(img, LV_OPA_TRANSP, LV_PART_MAIN); // Start invisible for fade-in
        splash_widget = img;
    } else {
        // Fallback: centered logo in container
        lv_obj_t* container = lv_obj_create(screen);
        lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
        lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_center(container);

        lv_obj_t* logo = lv_image_create(container);

        // Try pre-rendered centered logo
        std::string prerendered_path = find_prerendered(
            std::string("assets/images/prerendered/splash-logo-") + size_name + ".bin");
        if (!prerendered_path.empty()) {
            lv_image_set_src(logo, prerendered_path.c_str());
            spdlog::info("[Splash Screen] Using pre-rendered splash (instant display)");
        } else {
            // PNG fallback with runtime scaling
            const char* png_path = "A:assets/images/helixscreen-logo.png";
            lv_image_set_src(logo, png_path);

            lv_image_header_t header;
            lv_result_t res = lv_image_decoder_get_info(png_path, &header);

            if (res == LV_RESULT_OK) {
                lv_coord_t target_size = (screen_width * 3) / 5; // 60%
                if (screen_height < 500) {
                    target_size = screen_width / 2; // 50% on tiny screens
                }

                uint32_t width = header.w;
                uint32_t height = header.h;
                uint32_t scale_w = (static_cast<uint32_t>(target_size) * 256U) / width;
                // Ensure logo fits vertically (10% margin)
                int usable_h = (screen_height * 9) / 10;
                uint32_t scale_h = (static_cast<uint32_t>(usable_h) * 256U) / height;
                uint32_t scale = (scale_w < scale_h) ? scale_w : scale_h;
                lv_image_set_scale(logo, static_cast<uint16_t>(scale));

                spdlog::info("[Splash Screen] PNG fallback: {}x{} scaled to {} (scale factor: {})",
                             width, height, target_size, scale);
            } else {
                spdlog::warn("[Splash Screen] Could not get logo dimensions, using default scale");
                lv_image_set_scale(logo, 128);
            }
        }

        splash_widget = container;
    }

    // Version number in upper-left corner (subtle, theme-aware)
    lv_obj_t* version_label = lv_label_create(screen);
    lv_label_set_text(version_label, "v" HELIX_VERSION);
    lv_obj_set_style_text_color(
        version_label, dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_opa(version_label, LV_OPA_40, LV_PART_MAIN);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 8, 6);

    // Create fade-in animation (0.5 seconds)
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, splash_widget);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 500); // 500ms = 0.5 seconds
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&anim);

    // Run LVGL timer to process fade-in animation and keep splash visible
    // Total display time: 2 seconds (including 0.5s fade-in)
    uint32_t splash_start = helix::timing::get_ticks();
    uint32_t splash_duration = 2000; // 2 seconds total

    while (helix::timing::get_ticks() - splash_start < splash_duration) {
        lv_timer_handler(); // Process animations and rendering
        helix::timing::delay(5);
    }

    // Clean up splash screen (guard against early shutdown)
    helix::ui::safe_delete(version_label);
    helix::ui::safe_delete(splash_widget);

    spdlog::debug("[Splash Screen] complete");
}

} // namespace helix
