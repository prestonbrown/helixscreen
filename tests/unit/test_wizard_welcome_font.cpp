// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_welcome_font.cpp
 * @brief The cycling Welcome header renders at display size (prestonbrown/helixscreen#1599).
 *
 * Three contracts:
 *  1. The ladder: a per-breakpoint face that steps up to 48/64 where the build
 *     links them and never below the heading face at that tier elsewhere.
 *  2. The wiring: the widget built from wizard_language_chooser.xml actually
 *     wears the ladder's face, not the text_heading default.
 *  3. The restyle: a breakpoint that moves mid-wizard (fold/unfold resize)
 *     re-resolves the face the header is wearing (#1612).
 *
 * All nine cycling strings draw from the faces themselves — the font subsets
 * embed the wizard's CJK strings — so the ladder must hand back a face that
 * carries those glyphs.
 */

#include "ui_breakpoint.h"
#include "ui_fonts.h"
#include "ui_wizard.h"
#include "ui_wizard_language_chooser.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>

#include "../catch_amalgamated.hpp"

#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6
#endif
#ifndef HELIX_HAS_HIDPI_FONTS
#define HELIX_HAS_HIDPI_FONTS 0
#endif

namespace {

/// The wizard's cycling strings, by their widest codepoints: 欢 (welcome) and
/// よ (kana). Latin/Cyrillic coverage is every face's base range.
bool face_draws_welcome_glyphs(const lv_font_t* font) {
    lv_font_glyph_dsc_t dsc{};
    return lv_font_get_glyph_dsc(font, &dsc, 0x6B22, 0) && // 欢
           lv_font_get_glyph_dsc(font, &dsc, 0x3088, 0);   // よ
}

} // namespace

TEST_CASE("Welcome header ladder picks a display face per breakpoint",
          "[wizard][1599][1609][font]") {
#if HELIX_MAX_FONT_TIER >= 6 && HELIX_HAS_HIDPI_FONTS
    // Full-tier builds: the 48/64 rung the issue asks for at the tiers that
    // ship 800x480-class and larger panels.
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Medium) == &noto_sans_48);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Large) == &noto_sans_64);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::XLarge) == &noto_sans_64);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::XXLarge) == &noto_sans_64);

    // Display size is a step UP from the heading default at the same tier,
    // and a step DOWN to it at the constrained tiers.
    const int heading_medium = noto_sans_26.line_height;
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Medium)->line_height > heading_medium);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Micro)->line_height <= heading_medium);
#elif HELIX_MAX_FONT_TIER >= 5
    // k2 (FONT_TIERS := large xlarge, mk/cross.mk): the build reaches the
    // xlarge tier, so its largest linked face is noto_sans_32 and the ladder
    // must hand that back at the Large-and-up breakpoints — a size class
    // above font_heading_large's noto_sans_28 (prestonbrown/helixscreen#1609).
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Large) == &noto_sans_32);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::XLarge) == &noto_sans_32);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::XXLarge) == &noto_sans_32);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Large)->line_height >
          noto_sans_28.line_height);

    // Medium keeps the heading face at the tiers it ships; the ladder must
    // never hand back something SMALLER than that tier's heading.
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Medium)->line_height >=
          noto_sans_26.line_height);
#else
    // Constrained builds keep the heading face at the tiers they ship; the
    // ladder must never hand back something SMALLER than that tier's heading.
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Medium)->line_height >=
          noto_sans_26.line_height);
#endif

    // The small tiers step down so the 17-char Russian string is not driven
    // into a scroll by a face sized for an 800px panel.
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Micro)->line_height <
          helix::wizard_welcome_header_font(UiBreakpoint::Medium)->line_height);
    CHECK(helix::wizard_welcome_header_font(UiBreakpoint::Small)->line_height <
          helix::wizard_welcome_header_font(UiBreakpoint::Medium)->line_height);

    // Every rung can draw the cycling strings' own glyphs.
    for (int tier = to_int(UiBreakpoint::Micro); tier <= to_int(UiBreakpoint::XXLarge); ++tier) {
        CAPTURE(tier);
        CHECK(face_draws_welcome_glyphs(
            helix::wizard_welcome_header_font(static_cast<UiBreakpoint>(tier))));
    }
}

// =============================================================================
// Wiring - needs the ui_xml/ component tree readable from disk, so it is
// marked [.ui_integration] like the wizard connection UI tests: run it from
// the repo root.
// =============================================================================

class WizardLanguageUIFixture : public LVGLUITestFixture {
  public:
    WizardLanguageUIFixture() {
        wizard_ = ui_wizard_create(test_screen());
        if (!wizard_) {
            return;
        }
        force_language_chooser_step(true);
        ui_wizard_navigate_to_step(helix::wizard::StepId::Language);
        lv_obj_t* header = lv_obj_find_by_name(wizard_, "welcome_header");
        ready_ = (header != nullptr);
        // Halt the crossfade cycle so the font is asserted on a settled label.
        get_wizard_language_chooser_step()->stop_cycle_timer();
    }

    ~WizardLanguageUIFixture() override {
        get_wizard_language_chooser_step()->cleanup();
        wizard_ = nullptr;
    }

    void require_ready() {
        if (!ready_) {
            SKIP("XML infrastructure not available (ui_integration test)");
        }
    }

    lv_obj_t* wizard_ = nullptr;
    bool ready_ = false;
};

TEST_CASE_METHOD(WizardLanguageUIFixture, "Welcome header widget wears the display face",
                 "[wizard][1599][ui][.ui_integration]") {
    require_ready();

    // The expectation is only meaningful at the fixture display's tier.
    const UiBreakpoint bp = breakpoint_for(responsive_dimension(nullptr));
    REQUIRE(bp == UiBreakpoint::Medium); // fixture display is 800x480

    lv_obj_t* header = lv_obj_find_by_name(wizard_, "welcome_header");
    REQUIRE(header != nullptr);

    const lv_font_t* want = helix::wizard_welcome_header_font(bp);
    const lv_font_t* got = lv_obj_get_style_text_font(header, LV_PART_MAIN);
    CAPTURE(want, got);
    CHECK(got == want);
}

TEST_CASE_METHOD(WizardLanguageUIFixture, "Welcome header follows a mid-wizard breakpoint change",
                 "[wizard][1612][ui][.ui_integration]") {
    require_ready();

    lv_obj_t* header = lv_obj_find_by_name(wizard_, "welcome_header");
    REQUIRE(header != nullptr);

    // The fixture display is 800x480, so the header starts at the Medium rung.
    const lv_font_t* medium_face = lv_obj_get_style_text_font(header, LV_PART_MAIN);
    REQUIRE(medium_face == helix::wizard_welcome_header_font(UiBreakpoint::Medium));

    // A fold/unfold resize fires theme_manager_refresh_layout_constants(), whose
    // observable effect on this widget is the ui_breakpoint subject moving. The
    // face must follow the rung, not keep the create()-time resolution.
    {
        helix::test::ScopedBreakpoint micro(UiBreakpoint::Micro);
        const lv_font_t* micro_face = lv_obj_get_style_text_font(header, LV_PART_MAIN);
        CAPTURE(medium_face, micro_face);
        CHECK(micro_face == helix::wizard_welcome_header_font(UiBreakpoint::Micro));
        CHECK(micro_face != medium_face);
    }

    // Unfolding back: the face follows the subject up again, so the observer is
    // not a one-shot.
    CHECK(lv_obj_get_style_text_font(header, LV_PART_MAIN) == medium_face);
}
