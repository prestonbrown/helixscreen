// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "cjk_font_manager.h"

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

TEST_CASE_METHOD(CjkFontManagerFixture, "CjkFontManager: unload clears fallback on compiled fonts",
                 "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(noto_sans_14.fallback != nullptr);

    CjkFontManager::instance().on_language_changed("en");
    REQUIRE(noto_sans_14.fallback == nullptr);
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
