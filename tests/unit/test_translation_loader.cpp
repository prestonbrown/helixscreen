// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Covers the English-pack skip in helix::ui::ensure_translation_loaded() and
// the warn-once behaviour it depends on
// (patches/lvgl_translation_warn_once.patch). The two are one change: skipping
// en.xml is only tolerable because LVGL no longer logs "language is not found"
// on every single lookup.

#include "../lvgl_test_fixture.h"
#include "translation_loader.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// Counts LVGL log lines containing a needle. No other test installs a print
// callback, so restoring nullptr (LVGL's default path) is a faithful restore.
int g_missing_lang_lines = 0;

void counting_log_cb(lv_log_level_t level, const char* buf) {
    if (level == LV_LOG_LEVEL_WARN &&
        std::string(buf).find("language is not found") != std::string::npos) {
        ++g_missing_lang_lines;
    }
}

class ScopedLogCounter {
  public:
    ScopedLogCounter() {
        g_missing_lang_lines = 0;
        lv_log_register_print_cb(counting_log_cb);
    }
    ~ScopedLogCounter() {
        lv_log_register_print_cb(nullptr);
    }
    ScopedLogCounter(const ScopedLogCounter&) = delete;
    ScopedLogCounter& operator=(const ScopedLogCounter&) = delete;

    static int count() {
        return g_missing_lang_lines;
    }
};

// Packs cannot be unregistered (LVGL has no remove API), but selecting a
// language no pack declares makes every lookup miss again, which is what the
// rest of the suite assumes.
class ScopedLanguage {
  public:
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "English registers no translation pack", "[translation][i18n]") {
    ScopedLanguage restore_lang;

    helix::ui::ensure_translation_loaded(helix::ui::kIdentityLocale);
    lv_translation_set_language(helix::ui::kIdentityLocale);

    // With no pack registered for the selected language, lv_translation_get()
    // hands back the caller's own pointer. A registered pack — even an
    // identity one like en.xml — returns its own heap copy instead, so pointer
    // identity is what distinguishes "skipped" from "loaded", not string
    // equality (en.xml maps every tag to itself).
    const char* tag = "Cancel";
    REQUIRE(lv_translation_get(tag) == tag);
}

TEST_CASE_METHOD(LVGLTestFixture, "Non-English locales still load their pack",
                 "[translation][i18n]") {
    ScopedLanguage restore_lang;

    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    const char* tag = "Cancel";
    const char* translated = lv_translation_get(tag);

    // Guards the early return in ensure_translation_loaded(): a predicate that
    // matched every locale rather than just "en" would leave this returning
    // the tag itself.
    REQUIRE(translated != tag);
    REQUIRE(std::string(translated) == "Abbrechen");
}

TEST_CASE_METHOD(LVGLTestFixture, "Missing-language warning is logged once per language",
                 "[translation][i18n]") {
    ScopedLanguage restore_lang;
    ScopedLogCounter counter;

    // A locale nothing registers a pack for — the same situation English is in
    // once en.xml is skipped, but without depending on which packs earlier
    // tests in this shard happened to leave behind.
    lv_translation_set_language("qq-nonexistent");
    for (int i = 0; i < 50; ++i) {
        (void)lv_translation_get("Cancel");
    }

    // Before the patch this was 50 — one warning per lv_tr() call, which is
    // the entire reason en.xml was being loaded.
    REQUIRE(ScopedLogCounter::count() == 1);

    // Switching language must re-arm the warning, or a genuinely missing pack
    // for a language the user switches into would go unreported forever.
    lv_translation_set_language("zz-nonexistent");
    (void)lv_translation_get("Cancel");
    REQUIRE(ScopedLogCounter::count() == 2);

    // ...and stay armed only once for the new language too.
    for (int i = 0; i < 10; ++i) {
        (void)lv_translation_get("Cancel");
    }
    REQUIRE(ScopedLogCounter::count() == 2);
}
