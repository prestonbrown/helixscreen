// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "i18n_plural.h"

#include "lvgl.h"

namespace helix::ui {

namespace {

std::string_view primary_subtag(std::string_view lang) {
    const auto cut = lang.find_first_of("_-");
    return cut == std::string_view::npos ? lang : lang.substr(0, cut);
}

/// CLDR's `many` for es, fr, it and pt: a non-zero exact multiple of a million
/// ("1 millón de", "1 million de").
bool is_exact_million(uint64_t n) {
    return n != 0 && n % 1000000 == 0;
}

} // namespace

PluralCategory plural_category(std::string_view lang, uint64_t n) {
    const std::string_view code = primary_subtag(lang);

    if (code == "ja" || code == "zh") {
        return PluralCategory::Other;
    }
    if (code == "ru") {
        const uint64_t mod10 = n % 10;
        const uint64_t mod100 = n % 100;
        if (mod10 == 1 && mod100 != 11) {
            return PluralCategory::One;
        }
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (code == "fr" || code == "pt") {
        if (n <= 1) {
            return PluralCategory::One;
        }
        return is_exact_million(n) ? PluralCategory::Many : PluralCategory::Other;
    }
    if (code == "es" || code == "it") {
        if (n == 1) {
            return PluralCategory::One;
        }
        return is_exact_million(n) ? PluralCategory::Many : PluralCategory::Other;
    }
    // en, de, and the fallback for a language with no rule here.
    return n == 1 ? PluralCategory::One : PluralCategory::Other;
}

const char* plural_category_name(PluralCategory c) {
    switch (c) {
    case PluralCategory::One:
        return "one";
    case PluralCategory::Few:
        return "few";
    case PluralCategory::Many:
        return "many";
    case PluralCategory::Other:
        break;
    }
    return "other";
}

std::string plural_variant_tag(std::string_view other_key, PluralCategory c) {
    std::string tag(other_key);
    if (c != PluralCategory::Other) {
        tag += '_';
        tag += plural_category_name(c);
    }
    return tag;
}

const char* tr_plural_in(std::string_view lang, const char* one_key, const char* other_key,
                         uint64_t n) {
    if (n == 1 && one_key != nullptr) {
        return lv_tr(one_key);
    }
    const PluralCategory c = plural_category(lang, n);
    if (c != PluralCategory::Other) {
        const std::string tag = plural_variant_tag(other_key, c);
        const char* variant = lv_tr(tag.c_str());
        // A miss hands back the tag itself, which dies with `tag`.
        if (variant != tag.c_str()) {
            return variant;
        }
    }
    return lv_tr(other_key);
}

const char* tr_plural(const char* one_key, const char* other_key, uint64_t n) {
    const char* selected = lv_translation_get_language();
    return tr_plural_in(selected ? std::string_view(selected) : std::string_view{}, one_key,
                        other_key, n);
}

} // namespace helix::ui
