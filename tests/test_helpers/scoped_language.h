// tests/test_helpers/scoped_language.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/src/others/translation/lv_translation.h"
#include "translation_loader.h"

namespace helix {

/// RAII switch of the active translation language.
///
/// LVGL has no pack-unregister API, so the restore selects the identity locale:
/// every later lookup misses and returns its English key, which is what a test
/// that never switched would see. A test that sets a language and returns early
/// without this leaves every test after it in the same process translated.
class ScopedLanguage {
  public:
    explicit ScopedLanguage(const char* lang) {
        helix::ui::ensure_translation_loaded(lang);
        lv_translation_set_language(lang);
    }
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

} // namespace helix
