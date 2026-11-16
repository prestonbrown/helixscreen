// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "cjk_filename_glyphs.h"
#include "cjk_font_manager.h"
#include "core/gcode_filename.h"
#include "theme_manager.h"

#include <filesystem>

#include "../catch_amalgamated.hpp"

using namespace helix::system;

class CjkFontManagerFixture : public LVGLTestFixture {
  public:
    CjkFontManagerFixture() : LVGLTestFixture() {}

    ~CjkFontManagerFixture() override {
        CjkFontManager::instance().shutdown();
    }
};

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: not loaded by default", "[cjk_font]") {
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: English does not load CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("en");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: German does not load CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("de");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: Chinese loads CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: Japanese loads CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("ja");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: switching zh to en unloads",
                 "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("en");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: double load is idempotent", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: switching ja to zh stays loaded",
                 "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("ja");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: shutdown cleans up", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().shutdown();
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: shutdown when not loaded is safe",
                 "[cjk_font]") {
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
    CjkFontManager::instance().shutdown();
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: load sets fallback on compiled fonts",
                 "[cjk_font]") {
    const lv_font_t* pre_fallback = noto_sans_14.fallback;

    CjkFontManager::instance().on_language_changed("zh");

    REQUIRE(noto_sans_14.fallback != nullptr);
    REQUIRE(noto_sans_14.fallback != pre_fallback);
}

// The render path for a zh/ja label is lv_font_get_glyph_dsc() on a compiled
// font, which walks into the loaded .bin fallback. A glyph missing from the
// bake answers false here — exactly the tofu the user sees — so these three
// codepoints (汚 ja, 污/脏 zh, the #1620 dirty-bed string) pin the runtime
// font against the translations.
TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: baked glyphs resolve through the fallback",
                 "[cjk_font][1620]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    for (uint32_t codepoint : {0x6c5a, 0x6c61, 0x810f}) {
        lv_font_glyph_dsc_t dsc;
        REQUIRE(lv_font_get_glyph_dsc(&noto_sans_14, &dsc, codepoint, 0));
    }
}

// lv_binfont_create() reads a few bytes per lv_fs_read(). Without a driver
// cache each one is a read() syscall, about six million per CJK load, which
// takes half a minute on a host where a syscall costs a few microseconds.
TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: font drive reads through a cache",
                 "[cjk_font]") {
    lv_fs_drv_t* drv = lv_fs_get_drv(LV_FS_POSIX_LETTER);
    REQUIRE(drv != nullptr);
    CHECK(drv->cache_size >= 1024);
}

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: unload clears fallback on compiled fonts",
                 "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(noto_sans_14.fallback != nullptr);

    CjkFontManager::instance().on_language_changed("en");
    REQUIRE(noto_sans_14.fallback == nullptr);
}

// The bake (scripts/regen_text_fonts.sh) and the face table
// (include/cjk_font_faces.inc) are two hand-maintained lists that must name
// the same faces. A face compiled with
// no mapping entry renders CJK text as tofu; a mapping entry naming a bin
// that was never baked (or has since been removed) silently loads nothing
// for that face. Both failures leave loaded_font_count() short of the number
// of .bin files actually on disk, so comparing the two catches either
// direction without hardcoding either list.
TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: maps every baked CJK bin to a compiled face", "[cjk_font]") {
    size_t bin_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("assets/fonts/cjk")) {
        if (entry.is_regular_file() && entry.path().extension() == ".bin") {
            ++bin_count;
        }
    }
    REQUIRE(bin_count > 0);

    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CHECK(CjkFontManager::instance().loaded_font_count() == bin_count);
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: full lifecycle — load, switch, unload, shutdown",
                 "[cjk_font][integration]") {
    auto& mgr = CjkFontManager::instance();

    // Start with English
    mgr.on_language_changed("en");
    REQUIRE_FALSE(mgr.is_loaded());

    // Switch to Chinese
    mgr.on_language_changed("zh");
    REQUIRE(mgr.is_loaded());

    // Switch to Japanese (should stay loaded — both CJK)
    mgr.on_language_changed("ja");
    REQUIRE(mgr.is_loaded());

    // Switch to French
    mgr.on_language_changed("fr");
    REQUIRE_FALSE(mgr.is_loaded());

    // Back to Chinese
    mgr.on_language_changed("zh");
    REQUIRE(mgr.is_loaded());

    // Shutdown
    mgr.shutdown();
    REQUIRE_FALSE(mgr.is_loaded());

    // Double shutdown is safe
    mgr.shutdown();
    REQUIRE_FALSE(mgr.is_loaded());
}

// 猫 is in the filename bins and not in the translation set, so only the
// filename glyphs can draw it; 测 is in the translation bins only.
constexpr uint32_t kFilenameOnlyGlyph = 0x732b;
constexpr uint32_t kTranslationGlyph = 0x6d4b;

class CjkFilenameFixture : public LVGLUITestFixture {
  public:
    ~CjkFilenameFixture() override {
        helix::gcode::set_displayed_filename_hook(nullptr);
        CjkFontManager::instance().shutdown();
    }

    static bool filename_fonts_draw(uint32_t codepoint) {
        for (const char* token : {"font_body", "font_small", "font_xs"}) {
            lv_font_glyph_dsc_t dsc;
            if (!lv_font_get_glyph_dsc(theme_manager_get_font(token), &dsc, codepoint, 0))
                return false;
        }
        return true;
    }
};

TEST_CASE_METHOD(CjkFilenameFixture,
                 "CjkFontManager: a CJK filename loads filename glyphs, an ASCII one does not",
                 "[cjk_font][cjk_filename]") {
    auto& mgr = CjkFontManager::instance();
    helix::init_cjk_fonts("en");

    helix::gcode::get_display_filename("models/benchy_v2.gcode");
    REQUIRE(mgr.filename_glyph_sizes().empty());
    REQUIRE_FALSE(filename_fonts_draw(kFilenameOnlyGlyph));

    helix::gcode::get_display_filename("models/猫测试.gcode");
    CHECK_FALSE(mgr.filename_glyph_sizes().empty());
    CHECK_FALSE(mgr.is_loaded());
    CHECK(filename_fonts_draw(kFilenameOnlyGlyph));
    CHECK(filename_fonts_draw(kTranslationGlyph));
}

TEST_CASE_METHOD(CjkFilenameFixture,
                 "CjkFontManager: filename glyphs survive a switch to zh and back",
                 "[cjk_font][cjk_filename]") {
    auto& mgr = CjkFontManager::instance();
    helix::init_cjk_fonts("en");
    helix::gcode::get_display_filename("猫.gcode");
    REQUIRE(filename_fonts_draw(kFilenameOnlyGlyph));

    mgr.on_language_changed("zh");
    CHECK(filename_fonts_draw(kFilenameOnlyGlyph));
    CHECK(filename_fonts_draw(kTranslationGlyph));

    mgr.on_language_changed("en");
    CHECK_FALSE(mgr.is_loaded());
    CHECK(filename_fonts_draw(kFilenameOnlyGlyph));
    CHECK(filename_fonts_draw(kTranslationGlyph));

    mgr.shutdown();
    CHECK(theme_manager_get_font("font_body")->fallback == nullptr);
    CHECK_FALSE(filename_fonts_draw(kFilenameOnlyGlyph));
}

TEST_CASE_METHOD(CjkFilenameFixture,
                 "CjkFontManager: a label laid out before the load is measured again after it",
                 "[cjk_font][cjk_filename]") {
    const lv_font_t* body = theme_manager_get_font("font_body");
    auto make_label = [body]() {
        lv_obj_t* label = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(label, body, 0);
        lv_label_set_text(label, "猫猫猫猫");
        return label;
    };
    lv_obj_t* early = make_label();
    lv_obj_update_layout(early);
    const int32_t placeholder_width = lv_obj_get_width(early);

    helix::init_cjk_fonts("en");
    helix::gcode::get_display_filename("猫.gcode");

    lv_obj_t* fresh = make_label();
    lv_obj_update_layout(lv_screen_active());
    REQUIRE(lv_obj_get_width(fresh) != placeholder_width);
    CHECK(lv_obj_get_width(early) == lv_obj_get_width(fresh));
}
