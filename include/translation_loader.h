// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

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
 * @brief Locale whose translations are the tags themselves, so it needs no pack.
 */
inline constexpr const char* kIdentityLocale = "en";

} // namespace helix::ui
