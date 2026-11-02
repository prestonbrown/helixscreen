// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_i18n_plural.cpp
 * @brief CLDR plural categories, and count strings picking their per-category
 *        form from a translation pack (prestonbrown/helixscreen#1808).
 */

#include "ui_format_utils.h"

#include "../lvgl_test_fixture.h"
#include "display_numbering.h"
#include "i18n_plural.h"
#include "lvgl.h"
#include "translation_loader.h"

#include <cstdint>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::plural_category;
using helix::ui::PluralCategory;

namespace {

struct Row {
    uint64_t n;
    PluralCategory expected;
};

void check_rows(const char* lang, const std::vector<Row>& rows) {
    for (const auto& row : rows) {
        INFO(lang << " n=" << row.n);
        CHECK(plural_category(lang, row.n) == row.expected);
    }
}

// Packs cannot be unregistered, but selecting a language no pack declares makes
// every later lookup miss again, which is what the rest of the suite assumes.
class ScopedLanguage {
  public:
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

void add_tr(lv_translation_pack_t* pack, const char* tag, const char* text) {
    lv_translation_tag_dsc_t* dsc = lv_translation_add_tag(pack, tag);
    REQUIRE(dsc != nullptr);
    REQUIRE(lv_translation_set_tag_translation(pack, dsc, 0, text) == LV_RESULT_OK);
}

constexpr uint64_t kDayMs = 86400000ULL;

} // namespace

TEST_CASE("plural_category follows CLDR integer rules for Russian", "[i18n][plural]") {
    using C = PluralCategory;
    check_rows("ru",
               {
                   {0, C::Many},  {1, C::One},    {2, C::Few},    {3, C::Few},   {4, C::Few},
                   {5, C::Many},  {11, C::Many},  {12, C::Many},  {13, C::Many}, {14, C::Many},
                   {20, C::Many}, {21, C::One},   {22, C::Few},   {24, C::Few},  {25, C::Many},
                   {101, C::One}, {111, C::Many}, {112, C::Many}, {122, C::Few},
               });
}

TEST_CASE("plural_category for the one/other languages", "[i18n][plural]") {
    using C = PluralCategory;
    for (const char* lang : {"en", "de"}) {
        check_rows(
            lang, {{0, C::Other}, {1, C::One}, {2, C::Other}, {21, C::Other}, {1000000, C::Other}});
    }
    // es and it add CLDR's `many` for exact millions.
    for (const char* lang : {"es", "it"}) {
        check_rows(lang, {{0, C::Other},
                          {1, C::One},
                          {2, C::Other},
                          {1000000, C::Many},
                          {2000000, C::Many},
                          {1000001, C::Other}});
    }
    // fr and pt treat 0 as singular, and also have the million `many`.
    for (const char* lang : {"fr", "pt"}) {
        check_rows(
            lang,
            {{0, C::One}, {1, C::One}, {2, C::Other}, {1000000, C::Many}, {1000001, C::Other}});
    }
    for (const char* lang : {"ja", "zh"}) {
        check_rows(lang, {{0, C::Other}, {1, C::Other}, {2, C::Other}, {5, C::Other}});
    }
}

TEST_CASE("plural_category reads the primary subtag and defaults to English", "[i18n][plural]") {
    CHECK(plural_category("ru_RU", 3) == PluralCategory::Few);
    CHECK(plural_category("pt-BR", 0) == PluralCategory::One);
    CHECK(plural_category("zh_CN", 1) == PluralCategory::Other);
    CHECK(plural_category("xx", 1) == PluralCategory::One);
    CHECK(plural_category("", 2) == PluralCategory::Other);
}

TEST_CASE("plural_variant_tag names the generator's variant tags", "[i18n][plural]") {
    using helix::ui::plural_variant_tag;
    CHECK(plural_variant_tag("%d widgets", PluralCategory::Few) == "%d widgets_few");
    CHECK(plural_variant_tag("%d widgets", PluralCategory::Many) == "%d widgets_many");
    CHECK(plural_variant_tag("%d widgets", PluralCategory::One) == "%d widgets_one");
    CHECK(plural_variant_tag("%d widgets", PluralCategory::Other) == "%d widgets");
}

TEST_CASE_METHOD(LVGLTestFixture, "tr_plural picks the pack's form for the count's category",
                 "[i18n][plural]") {
    ScopedLanguage restore_lang;

    lv_translation_pack_t* pack = lv_translation_add_dynamic();
    REQUIRE(pack != nullptr);
    REQUIRE(lv_translation_add_language(pack, "ru") == LV_RESULT_OK);
    add_tr(pack, "1 plural-test gadget", "1 штуковина");
    add_tr(pack, "%d plural-test gadgets", "%d штуковины");
    add_tr(pack, "%d plural-test gadgets_one", "%d штуковина");
    add_tr(pack, "%d plural-test gadgets_few", "%d штуковины");
    add_tr(pack, "%d plural-test gadgets_many", "%d штуковин");
    // Only `other` is translated here, so every category falls back to it.
    add_tr(pack, "%d plural-test sprockets", "%d шестерёнок");

    lv_translation_set_language("ru");

    auto pick = [](uint64_t n) {
        return std::string(
            helix::ui::tr_plural("1 plural-test gadget", "%d plural-test gadgets", n));
    };
    CHECK(pick(1) == "1 штуковина");
    CHECK(pick(2) == "%d штуковины");
    CHECK(pick(5) == "%d штуковин");
    CHECK(pick(11) == "%d штуковин");
    CHECK(pick(21) == "%d штуковина");
    CHECK(pick(22) == "%d штуковины");

    SECTION("a missing variant falls back to the other form") {
        for (uint64_t n : {0ULL, 1ULL, 2ULL, 5ULL, 21ULL}) {
            INFO("n=" << n);
            CHECK(std::string(helix::ui::tr_plural(nullptr, "%d plural-test sprockets", n)) ==
                  "%d шестерёнок");
        }
    }

    SECTION("an untranslated key comes back as the English tag") {
        CHECK(std::string(helix::ui::tr_plural("1 plural-test widget", "%d plural-test widgets",
                                               3)) == "%d plural-test widgets");
        CHECK(std::string(helix::ui::tr_plural("1 plural-test widget", "%d plural-test widgets",
                                               1)) == "1 plural-test widget");
    }
}

TEST_CASE("tr_plural keeps the English one/other choice", "[i18n][plural]") {
    // No English pack is loaded: every lookup returns its tag.
    using helix::ui::tr_plural_in;
    CHECK(std::string(tr_plural_in("en", "1 thing", "%d things", 1)) == "1 thing");
    CHECK(std::string(tr_plural_in("en", "1 thing", "%d things", 0)) == "%d things");
    CHECK(std::string(tr_plural_in("en", "1 thing", "%d things", 21)) == "%d things");
    CHECK(std::string(tr_plural_in("en", nullptr, "%d things", 1)) == "%d things");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "format_relative_time agrees with the count in the shipped Russian pack",
                 "[i18n][plural][format_utils]") {
    ScopedLanguage restore_lang;
    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    CHECK(helix::ui::format_relative_time(1 * kDayMs) == "1 день назад");
    CHECK(helix::ui::format_relative_time(2 * kDayMs) == "2 дня назад");
    CHECK(helix::ui::format_relative_time(4 * kDayMs) == "4 дня назад");
    CHECK(helix::ui::format_relative_time(5 * kDayMs) == "5 дней назад");
    CHECK(helix::ui::format_relative_time(11 * kDayMs) == "11 дней назад");
    CHECK(helix::ui::format_relative_time(21 * kDayMs) == "21 день назад");
    CHECK(helix::ui::format_relative_time(22 * kDayMs) == "22 дня назад");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "lane_count_label agrees with the count in the shipped Russian pack",
                 "[i18n][plural]") {
    using helix::ui::lane_count_label;
    using helix::ui::LaneNoun;
    ScopedLanguage restore_lang;
    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

    CHECK(lane_count_label(LaneNoun::Slot, 1) == "1 слот");
    CHECK(lane_count_label(LaneNoun::Slot, 4) == "4 слота");
    CHECK(lane_count_label(LaneNoun::Slot, 8) == "8 слотов");
    CHECK(lane_count_label(LaneNoun::Slot, 12) == "12 слотов");
    CHECK(lane_count_label(LaneNoun::Lane, 21) == "21 канал");
    CHECK(lane_count_label(LaneNoun::Toolhead, 2) == "2 печатающие головки");
    CHECK(lane_count_label(LaneNoun::Toolhead, 5) == "5 печатающих головок");
}
