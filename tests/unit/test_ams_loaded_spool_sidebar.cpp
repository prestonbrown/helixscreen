// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_loaded_spool_sidebar.cpp
 * @brief The AMS sidebar: at 800x480 it paints the loaded card's spool (the
 *        loaded filament's colour and how much is left on it, through every
 *        current-loaded branch), and at 800x480 and 480x320 its action buttons
 *        keep their labels inside them in every shipped language.
 */

#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_button.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/scoped_geometry.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lvgl.h"
#include "translation_loader.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr std::pair<const char*, const char*> kAmsComponents[] = {
    {"ams_unit_detail", "A:ui_xml/components/ams_unit_detail.xml"},
    {"ams_loaded_card", "A:ui_xml/components/ams_loaded_card.xml"},
    {"ams_environment_indicator", "A:ui_xml/components/ams_environment_indicator.xml"},
    {"ams_env_pill", "A:ui_xml/components/ams_env_pill.xml"},
    {"ams_sidebar", "A:ui_xml/components/ams_sidebar.xml"},
    {"ams_panel", "A:ui_xml/ams_panel.xml"},
};

void register_ams_widgets_and_xml_once() {
    static bool done = false;
    if (done) {
        return;
    }
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    for (const auto& [name, path] : kAmsComponents) {
        (void)name;
        lv_xml_register_component_from_file(path);
    }
    done = true;
}

/// Registers the AMS components again, so their styles take the design tokens
/// of the current geometry. A component resolves its tokens when it is
/// registered, and the first registration in the process may have run at
/// another size.
void reregister_ams_components() {
    register_ams_widgets_and_xml_once();
    for (const auto& [name, path] : kAmsComponents) {
        lv_xml_component_unregister(name);
        lv_xml_register_component_from_file(path);
    }
}

/// Re-registers the AMS components when it goes out of scope. Declared before a
/// ScopedGeometry, it runs after that geometry is restored.
class ReregisterAmsComponentsOnExit {
  public:
    ReregisterAmsComponentsOnExit() = default;
    ~ReregisterAmsComponentsOnExit() {
        reregister_ams_components();
    }
    ReregisterAmsComponentsOnExit(const ReregisterAmsComponentsOnExit&) = delete;
    ReregisterAmsComponentsOnExit& operator=(const ReregisterAmsComponentsOnExit&) = delete;
};

uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

// LVGL has no pack-unregister API; selecting the identity locale restores the
// English keys for whatever runs next.
class ScopedLanguage {
  public:
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

/// The button's own text label: the child that is not its icon.
lv_obj_t* button_label(lv_obj_t* btn) {
    lv_obj_t* label = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) && child != ui_button_get_icon(btn)) {
            label = child;
        }
    }
    return label;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "AMS sidebar paints the loaded spool's colour and fill",
                 "[ui_integration][ams][loaded_card]") {
    auto mock = std::make_unique<AmsBackendMock>(4);
    REQUIRE(mock->start().success());
    mock->set_operation_delay(10);
    auto* backend = mock.get();
    {
        SlotInfo info = backend->get_slot_info(1);
        info.color_rgb = 0x2266AA;
        info.remaining_weight_g = 250.0f;
        info.total_weight_g = 1000.0f;
        REQUIRE(helix::test::apply_edit(*backend, 1, info).success());
    }

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    register_ams_widgets_and_xml_once();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    panel.on_activate();
    process_lvgl(20);

    lv_obj_t* spool = lv_obj_find_by_name(panel_obj, "loaded_spool");
    REQUIRE(spool != nullptr);
    lv_subject_t* fill = AmsState::instance().get_current_fill_pct_subject();

    auto settle = [&]() {
        backend->wait_for_operation_thread();
        AmsState::instance().sync_from_backend();
        process_lvgl(20);
    };

    SECTION("a loaded slot: its colour, filled to its remaining weight") {
        REQUIRE(backend->load_filament(1).success());
        settle();
        REQUIRE(backend->get_current_slot() == 1);
        CHECK(lv_subject_get_int(fill) == 25);
        CHECK(rgb(ui_spool_canvas_get_color(spool)) == 0x2266AA);
        CHECK(std::fabs(ui_spool_canvas_get_fill_level(spool) - 0.25f) < 0.01f);

        // Nothing loaded: an empty spool in the neutral grey.
        REQUIRE(backend->unload_filament(1).success());
        settle();
        REQUIRE_FALSE(backend->is_filament_loaded());
        CHECK(lv_subject_get_int(fill) == 0);
        CHECK(rgb(ui_spool_canvas_get_color(spool)) == 0x505050);
        CHECK(ui_spool_canvas_get_fill_level(spool) == 0.0f);
    }

    SECTION("bypass: the external spool when one is assigned, else a full neutral spool") {
        if (backend->is_filament_loaded()) {
            REQUIRE(backend->unload_filament(backend->get_current_slot()).success());
            settle();
        }
        REQUIRE(backend->enable_bypass().success());
        settle();
        REQUIRE(backend->is_bypass_active());

        AmsState::instance().clear_external_spool_info();
        AmsState::instance().sync_from_backend();
        process_lvgl(20);
        CHECK(lv_subject_get_int(fill) == -1);
        CHECK(rgb(ui_spool_canvas_get_color(spool)) == 0x888888);
        CHECK(ui_spool_canvas_get_fill_level(spool) == 1.0f);

        SlotInfo ext;
        ext.color_rgb = 0xCC3311;
        ext.material = "PETG";
        ext.remaining_weight_g = 600.0f;
        ext.total_weight_g = 1000.0f;
        AmsState::instance().set_external_spool_info_in_memory(ext);
        AmsState::instance().sync_from_backend();
        process_lvgl(20);
        CHECK(lv_subject_get_int(fill) == ext.display_fill_pct());
        CHECK(lv_subject_get_int(fill) == 60);
        CHECK(rgb(ui_spool_canvas_get_color(spool)) == 0xCC3311);
        CHECK(std::fabs(ui_spool_canvas_get_fill_level(spool) - 0.6f) < 0.01f);

        AmsState::instance().clear_external_spool_info();
        REQUIRE(backend->disable_bypass().success());
        settle();
    }

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}

namespace {

/// Every visible sidebar action on the AMS panel at the current geometry, in
/// every shipped language, on Happy Hare's mock (Home, Check slots), AFC's
/// (Reset) and the Snapmaker's (Load... and Unload... for its parallel heads).
/// Each label sits inside its button and each button on screen. With
/// @p whole_in, the languages listed must show their full text; the rest may
/// ellipsize, inside the button.
void check_action_labels(XMLTestFixture& fx, std::initializer_list<const char*> whole_in) {
    ScopedLanguage restore_lang;
    register_ams_widgets_and_xml_once();

    for (const char* lang : {"en", "de", "fr", "es", "pt", "it", "ru", "ja", "zh"}) {
        CAPTURE(lang);
        if (std::strcmp(lang, "en") == 0) {
            lv_translation_set_language(helix::ui::kIdentityLocale);
        } else {
            helix::ui::ensure_translation_loaded(lang);
            lv_translation_set_language(lang);
        }
        bool whole = false;
        for (const char* w : whole_in) {
            whole = whole || std::strcmp(w, lang) == 0;
        }

        for (const char* backend : {"happy_hare", "afc", "snapmaker"}) {
            CAPTURE(backend);
            auto mock = std::make_unique<AmsBackendMock>(4);
            mock->set_afc_mode(std::strcmp(backend, "afc") == 0);
            mock->set_snapmaker_mode(std::strcmp(backend, "snapmaker") == 0);
            REQUIRE(mock->start().success());
            AmsState::instance().set_backend(std::move(mock));
            AmsState::instance().init_subjects(true);
            AmsState::instance().sync_from_backend();

            AmsPanel panel(fx.state(), &fx.api());
            panel.init_subjects();
            lv_obj_t* panel_obj =
                static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "ams_panel", nullptr));
            REQUIRE(panel_obj != nullptr);
            panel.setup(panel_obj, fx.test_screen());
            panel.on_activate();
            fx.process_lvgl(20);
            lv_obj_update_layout(fx.test_screen());

            lv_area_t screen;
            lv_obj_get_coords(fx.test_screen(), &screen);
            int checked = 0;
            const std::string reset_key = AmsState::instance().get_backend()->reset_button_label();
            const std::pair<const char*, std::string> buttons[] = {
                {"btn_unload", "Unload"},
                {"btn_batch_unload", "Unload..."},
                {"btn_load", "Load..."},
                {"btn_reset", reset_key},
                {"btn_check_gates", "Check Slots"},
                {"btn_settings", "Settings"},
            };
            for (const auto& [name, key] : buttons) {
                CAPTURE(name);
                lv_obj_t* btn = lv_obj_find_by_name(panel_obj, name);
                REQUIRE(btn != nullptr);
                if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
                    continue;
                }
                lv_area_t outer;
                lv_obj_get_coords(btn, &outer);
                CHECK(outer.y2 <= screen.y2);
                CHECK(outer.x2 <= screen.x2);
                lv_obj_t* label = button_label(btn);
                REQUIRE(label != nullptr);
                lv_area_t inner;
                lv_obj_get_content_coords(btn, &inner);
                lv_area_t text;
                lv_obj_get_coords(label, &text);
                CHECK(text.x1 >= inner.x1);
                CHECK(text.x2 <= inner.x2);
                CHECK(text.y1 >= inner.y1);
                CHECK(text.y2 <= inner.y2);
                if (whole) {
                    // The label shows its full translation (a dots label rewrites
                    // its own text), and that text fits its width.
                    const std::string want = lv_tr(key.c_str());
                    CAPTURE(want);
                    CHECK(std::string(lv_label_get_text(label)) == want);
                    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
                    lv_point_t natural;
                    lv_text_get_size(&natural, want.c_str(), font, 0, 0, LV_COORD_MAX,
                                     LV_TEXT_FLAG_NONE);
                    CHECK(natural.x <= lv_area_get_width(&text));
                }
                ++checked;
            }
            CHECK(checked >= 3);

            panel.on_deactivate(DeactivateReason::NavigateAway);
            panel.clear_panel_reference();
            lv_obj_delete(panel_obj);
            fx.process_lvgl(10);
            AmsState::instance().set_backend(nullptr);
        }
    }
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "AMS sidebar action labels stay inside their buttons in every language",
                 "[ui_integration][ams][i18n]") {
    check_action_labels(*this, {"en", "de", "fr", "es", "pt", "it", "ru", "ja", "zh"});
}

// The Snapmaker U1's screen: two half-width cells of a 145px column, icon over
// label. English shows whole; a longer translation ellipsizes on one line.
TEST_CASE_METHOD(XMLTestFixture, "AMS sidebar action labels fit at 480x320",
                 "[ui_integration][ams][i18n]") {
    ReregisterAmsComponentsOnExit restore_tokens;
    helix::test::ScopedGeometry tiny(480, 320);
    reregister_ams_components();
    check_action_labels(*this, {"en"});
}
