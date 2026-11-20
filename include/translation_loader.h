// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

/**
 * @brief Mark a string literal for translation extraction without translating
 *        it here.
 *
 * `lv_tr()` both marks a string for the extractor and looks it up, which only
 * works when those happen in the same place. Static tables break that: the copy
 * is authored in a table initializer, but the lookup has to happen later, at
 * render time, once a language is actually loaded. Wrapping the literal in
 * `lv_tr()` at the table would resolve it once, at static-init time, against
 * whatever pack happened to be loaded — usually none.
 *
 * So the two halves are split, the same way gettext splits `N_()` from `_()`:
 * this macro expands to the literal unchanged and exists purely so
 * `scripts/translations/extractor.py` can see it, and the call site keeps
 * calling `lv_tr()` on the stored pointer.
 *
 * Without it a table's strings are extracted only by coincidence — when the
 * same English text happens to appear in some XML file — which is how 18 of the
 * 37 panel-widget names ended up untranslated in every locale.
 */
#define TR_NOOP(s) (s)

namespace helix::ui {

/**
 * @brief Ensure the given locale's translation pack is loaded into LVGL.
 *
 * HelixScreen ships one XML per locale (ui_xml/translations/<lang>.xml) so the
 * app can load only the current language at startup (~60-80 KB heap) instead
 * of the combined 9-language translations.xml (~500-700 KB heap).
 *
 * LVGL's translation system has no remove API — once a pack is registered it
 * stays until lv_translation_deinit(). This function de-duplicates via an
 * internal set so repeated calls for the same lang are no-ops. As the user
 * cycles between languages during a session, multiple packs accumulate, but
 * typical usage touches one or two.
 *
 * English is deliberately NOT loaded. Our tags ARE the English strings, so
 * en.xml maps all 2739 of them to themselves and carries no information;
 * lv_translation_get() already returns the tag when no pack matches. Skipping
 * it saves ~140 KB of heap and makes every English lookup an empty pack-list
 * walk instead of a linear scan of 2739 entries.
 *
 * @param lang Locale code, e.g. "en", "de", "zh"
 */
void ensure_translation_loaded(const std::string& lang);

/**
 * @brief Whether a literal UI string can be a translation key.
 *
 * False for numbers, short punctuation, icon glyphs, identifiers, URLs and the
 * like. The C++ twin of should_skip_text() in scripts/translations/extractor.py,
 * so the engine implies a tag exactly where the extractor would look for a key;
 * tests/fixtures/translation_key_cases.txt pins both to the same verdicts.
 * Registered with helix-xml as its translation key callback.
 */
bool is_translation_key(std::string_view text);

/**
 * @brief Translate an English word that means different things in different places.
 *
 * One English tag carries one translation per language, so a word with two
 * senses ("Neutral" the buffer position, "Neutral" the LED white tone) would get
 * one rendering for both. A context key gives one sense its own tag: a lowercase
 * context, a bar, then the English text (`"buffer|Neutral"`). It is an ordinary
 * key everywhere else: en.yml maps it to itself, translators see the context in
 * the source string, and each locale translates only the text after the bar.
 *
 * English loads no pack, so the lookup returns the key itself, and this returns
 * the text after the bar. A locale with no translation for the key gets the same
 * English text rather than the raw key.
 *
 * @p key must be a string literal: scripts/translations/extractor.py reads it
 * from the call. A key without a `context|` prefix behaves like lv_tr().
 */
const char* tr_context(const char* key);

/**
 * @brief Locale whose translations are the tags themselves, so it needs no pack.
 */
inline constexpr const char* kIdentityLocale = "en";

} // namespace helix::ui
