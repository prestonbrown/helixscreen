// SPDX-License-Identifier: GPL-3.0-or-later
#include "font_registration.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/xml/lv_xml.h"

#include <stdbool.h>
#include <stddef.h>

static const char* TAG = "font_registration";

// Font symbols come from LV_FONT_CUSTOM_DECLARE in lv_conf.h (extern
// lv_font_t declarations), backed by the .c sources compiled into helixcore
// (see components/helixcore/CMakeLists.txt HELIX_FONT_SRCS). Referencing
// every symbol here keeps any individual face from being dropped — but only
// while this function itself survives the link. With no caller in the image,
// --gc-sections discards it (this Xtensa toolchain does NOT honor
// __attribute__((used)) as a GC root — verified via link map, symbols at
// 0x0). What actually anchors it is `-Wl,--undefined=helix_fonts_register`
// in main/CMakeLists.txt: do not remove that flag unless a real call site
// exists AND the font symbols are re-verified present post-link (nm/readelf).
static struct {
    const char* name;
    int64_t ms;
    bool ok;
} s_load_results[10];

// Re-log the .bin load results outside the ~2s WiFi RF-cal serial dead window.
// Called from app_boot at home-panel-up.
void helix_fonts_log_summary(void) {
    for (size_t i = 0; i < sizeof(s_load_results) / sizeof(s_load_results[0]); i++) {
        if (!s_load_results[i].name)
            continue;
        if (s_load_results[i].ok) {
            ESP_LOGI(TAG, "fonts: %s .bin %lld ms", s_load_results[i].name, s_load_results[i].ms);
        } else {
            ESP_LOGW(TAG, "fonts: %s .bin LOAD FAILED (noto_sans_18 fallback)",
                     s_load_results[i].name);
        }
    }
}

void helix_fonts_register(void) {
    // These 10 faces' glyph data lives in frogfs .bin files (moved out of the
    // compiled app image; see components/helixcore/moved_fonts_shim.c for their
    // zero-init writable symbols). Populate the shim structs HERE — before the
    // lv_xml_register_font() token registrations below AND before the
    // AssetManager::register_all() by-symbol registrations (app_boot.cpp:266) —
    // so both point at valid, fully-populated shims. On load failure we fall
    // back to a copy of noto_sans_18 (compiled in) so text still renders.
    //
    // Results are also recorded for helix_fonts_log_summary(): these loads run
    // at ~2s, inside the WiFi RF-cal power dip that knocks the CH340 off USB —
    // the immediate log lines below land in a dead serial window on every boot
    // with the radio on, so app_boot re-logs the summary at home-panel-up.
    static const struct {
        const char* name;
        const char* path;
        lv_font_t* shim;
    } moved_faces[] = {
        {"noto_sans_bold_28", "A:/assets/assets/fonts/noto_sans_bold_28.bin", &noto_sans_bold_28},
        {"noto_sans_light_16", "A:/assets/assets/fonts/noto_sans_light_16.bin",
         &noto_sans_light_16},
        {"noto_sans_light_12", "A:/assets/assets/fonts/noto_sans_light_12.bin",
         &noto_sans_light_12},
        {"mdi_icons_48", "A:/assets/assets/fonts/mdi_icons_48.bin", &mdi_icons_48},
        {"mdi_icons_64", "A:/assets/assets/fonts/mdi_icons_64.bin", &mdi_icons_64},
        {"noto_sans_26", "A:/assets/assets/fonts/noto_sans_26.bin", &noto_sans_26},
        {"source_code_pro_14", "A:/assets/assets/fonts/source_code_pro_14.bin",
         &source_code_pro_14},
        {"mdi_icons_16", "A:/assets/assets/fonts/mdi_icons_16.bin", &mdi_icons_16},
        {"mdi_icons_24", "A:/assets/assets/fonts/mdi_icons_24.bin", &mdi_icons_24},
        {"mdi_icons_32", "A:/assets/assets/fonts/mdi_icons_32.bin", &mdi_icons_32},
    };
    for (size_t i = 0; i < sizeof(moved_faces) / sizeof(moved_faces[0]); i++) {
        int64_t t0 = esp_timer_get_time();
        lv_font_t* loaded = lv_binfont_create(moved_faces[i].path);
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        s_load_results[i].name = moved_faces[i].name;
        s_load_results[i].ms = dt_ms;
        s_load_results[i].ok = (loaded != NULL);
        if (loaded) {
            *moved_faces[i].shim = *loaded;
            ESP_LOGI(TAG, "font %s loaded from .bin in %lld ms", moved_faces[i].name, dt_ms);
        } else {
            *moved_faces[i].shim = noto_sans_18;
            ESP_LOGW(TAG, "font %s .bin load FAILED (%lld ms) — fell back to noto_sans_18",
                     moved_faces[i].name, dt_ms);
        }
    }

    // The lv_binfont_create() results are intentionally never destroyed: they
    // live for the process lifetime and their glyph/cmap tables back the shim
    // struct-copies above, so lv_binfont_destroy would free data still in use.

    lv_xml_register_font(NULL, "noto_sans_26", &noto_sans_26);
    lv_xml_register_font(NULL, "noto_sans_bold_28", &noto_sans_bold_28);
    lv_xml_register_font(NULL, "noto_sans_18", &noto_sans_18);
    lv_xml_register_font(NULL, "noto_sans_light_16", &noto_sans_light_16);
    lv_xml_register_font(NULL, "noto_sans_light_12", &noto_sans_light_12);
    lv_xml_register_font(NULL, "source_code_pro_14", &source_code_pro_14);
    lv_xml_register_font(NULL, "mdi_icons_16", &mdi_icons_16);
    lv_xml_register_font(NULL, "mdi_icons_24", &mdi_icons_24);
    lv_xml_register_font(NULL, "mdi_icons_32", &mdi_icons_32);
    lv_xml_register_font(NULL, "mdi_icons_48", &mdi_icons_48);
    lv_xml_register_font(NULL, "mdi_icons_64", &mdi_icons_64);

    ESP_LOGI(TAG, "registered 11 medium-tier fonts (1 compiled + 10 runtime .bin)");
}
