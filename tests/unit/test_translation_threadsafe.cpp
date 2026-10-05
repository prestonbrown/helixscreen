// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_translation_threadsafe.cpp
 * @brief lv_tr() from a background thread while the main thread switches
 *        language and registers translation packs.
 *
 * Status parsers on the WebSocket thread call lv_tr(), so a lookup must stay
 * valid across a concurrent lv_translation_set_language() and a lazily loaded
 * pack. Most meaningful under ASAN/TSan, where a lookup reading freed or
 * half-built translation state is reported even when it happens to return the
 * right bytes.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_language.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "translation_loader.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture,
                 "lv_tr from another thread survives language switches and pack loads",
                 "[translation][threading]") {
    REQUIRE(lv_xml_register_translation_from_data(
                "<translations languages=\"de fr\">"
                "  <translation tag=\"tr_threadsafe_tag\" de=\"Hallo\" fr=\"Bonjour\"/>"
                "</translations>") == LV_RESULT_OK);
    helix::ScopedLanguage restore(helix::ui::kIdentityLocale);

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<long> lookups{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const char* s = lv_tr("tr_threadsafe_tag");
            if (std::strcmp(s, "Hallo") != 0 && std::strcmp(s, "Bonjour") != 0 &&
                std::strcmp(s, "tr_threadsafe_tag") != 0)
                bad.fetch_add(1, std::memory_order_relaxed);
            lookups.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const char* langs[] = {"de", "fr", "en"};
    for (int i = 0; i < 3000; i++) {
        lv_translation_set_language(langs[i % 3]);
        // A language switch loads the target locale's pack while lookups run.
        if (i % 600 == 0) {
            std::string lang = "zz" + std::to_string(i);
            std::string pack = "<translations languages=\"" + lang +
                               "\">"
                               "<translation tag=\"tr_threadsafe_tag\" " +
                               lang + "=\"x\"/></translations>";
            REQUIRE(lv_xml_register_translation_from_data(pack.c_str()) == LV_RESULT_OK);
        }
    }

    stop = true;
    reader.join();

    CHECK(bad.load() == 0);
    CHECK(lookups.load() > 0);
}
