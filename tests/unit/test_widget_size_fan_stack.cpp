// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_fan_stack.cpp
 * @brief fan_stack picks label fonts and name-label text from physical
 * pixels, not colspan/rowspan.
 *
 * `on_size_changed` (fan_stack_widget.cpp, stack mode only) restyles:
 *   - Speed labels `fan_stack_{part,hotend,aux}_speed`: text font
 *     `font_small` when bigger, `font_xs` otherwise.
 *   - Name labels `fan_stack_{part,hotend,aux}_name`: same font switch as
 *     the speed labels, **and** text — `lv_tr("P"/"H"/"C")` (single letter)
 *     when compact, the resolved fan display name (or a hardcoded English
 *     fallback when no fan of that role was discovered) when bigger. The
 *     text assertion is the stronger one: a font can leak in from a stray
 *     style, but the text string cannot.
 *
 * `on_size_changed` also loops over `fan_stack_{part,hotend,aux}_icon` and
 * restyles the icon object's own font. The `icon` XML widget
 * (`ui_icon_xml_create`, `ui_icon.cpp`) is itself an `lv_label` with no
 * child beneath it, so the glyph font lives on the icon object directly —
 * styling it (rather than a nonexistent child) is what makes fan_stack
 * icons scale with the widget, covered by the font assertions below (16px
 * xs / 24px sm, the same size tiers the text already used).
 *
 * `bigger = (width_px >= w_normal())` — width only. Each row lays out icon +
 * name + speed horizontally (panel_widget_fan_stack.xml), so a resolved name
 * ("Hotend" instead of "H") only ever competes for room along the width
 * axis; extra height just centers the same three rows, it never widens one.
 * Height used to also drive "bigger" (an OR), but Large/XLarge's single-row
 * cell height alone already clears h_tall() (141px, 169px) — wider than
 * h_tall()'s own calibration point (Micro's genuinely-2-row 131px) — so a
 * plain 1x1 widget on those two tiers read as "bigger" and squeezed resolved
 * names into a ~107-134px row. Three cases cover this: sub-threshold on both
 * axes (compact), width alone (bigger), and height alone at a Large/XLarge-
 * shaped extent, which must stay compact now that height no longer counts.
 *
 * Row geometry (`fan_stack_{part,hotend,aux}_row` width/flex alignment,
 * `fan_stack_widget.cpp` ~:294-331) is also driven by this predicate, but
 * its *width* is not a reliable cross-threshold observable and this file
 * does not assert one as such. Compact rows are pinned to `LV_PCT(100)` of
 * the widget's own granted width — which itself ranges anywhere up to
 * `w_normal() - 1` px, since that is the whole compact domain — while bigger
 * rows are sized to `LV_SIZE_CONTENT` (icon + display-name text + speed
 * value), a fixed size that does not scale with the grant at all. Measured
 * at a realistic near-threshold compact grant (harness now applies
 * width_px/height_px to the widget's own `lv_obj_t`, matching
 * `PanelWidgetManager`'s real `grid_track_extent()` allocation —
 * `panel_widget_size_harness.h`'s `resize()`): compact width ~123px
 * (`w_normal() - 1`, minus `#space_xs` padding both sides — the exact figure
 * shifts by a pixel with w_normal()'s own calibration) versus bigger width
 * 111px — compact is *wider* here, the opposite of what an
 * unconstrained harness object (which floors out at `style_min_width="80"`
 * with nothing to widen it) had shown before that fix. Neither direction
 * generalizes: a longer resolved fan name pushes the bigger row's fixed
 * content width up, and a wider compact grant pushes the compact row's
 * width up too, independently. So this file asserts the one part of that
 * relationship that IS architecturally guaranteed rather than data/grant
 * dependent — `LV_PCT(100)` resolving to the container's actual content
 * width in the compact case — plus the `flex_main_place` style
 * (`LV_FLEX_ALIGN_CENTER` compact / `LV_FLEX_ALIGN_START` bigger), which
 * `on_size_changed` sets unconditionally per branch and is true regardless
 * of any pixel measurement.
 */

#include "ui_fonts.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack labels/names follow pixels, not spans",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    lv_obj_t* hotend_speed = h.child("fan_stack_hotend_speed");
    lv_obj_t* aux_speed = h.child("fan_stack_aux_speed");
    REQUIRE(part_speed != nullptr);
    REQUIRE(hotend_speed != nullptr);
    REQUIRE(aux_speed != nullptr);

    lv_obj_t* part_name = h.child("fan_stack_part_name");
    lv_obj_t* hotend_name = h.child("fan_stack_hotend_name");
    lv_obj_t* aux_name = h.child("fan_stack_aux_name");
    REQUIRE(part_name != nullptr);
    REQUIRE(hotend_name != nullptr);
    REQUIRE(aux_name != nullptr);

    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* hotend_icon = h.child("fan_stack_hotend_icon");
    lv_obj_t* aux_icon = h.child("fan_stack_aux_icon");
    REQUIRE(part_icon != nullptr);
    REQUIRE(hotend_icon != nullptr);
    REQUIRE(aux_icon != nullptr);

    lv_obj_t* part_row = h.child("fan_stack_part_row");
    REQUIRE(part_row != nullptr);

    // No fans are discovered under this fixture (PrinterState starts with
    // an empty fan list), so bind_fans() never populates the display-name
    // strings and on_size_changed falls back to the hardcoded English
    // names. That fallback still differs from the compact single letter,
    // so it is a valid target for the "bigger" text assertion.
    const char* part_bigger_text = lv_tr("Part");
    const char* hotend_bigger_text = lv_tr("Hotend");
    const char* aux_bigger_text = lv_tr("Chamber");

    // --- Neither flag: large span, sub-threshold pixels on both axes. ---
    // A span-reading implementation would go "bigger" here; pixels must win.
    h.resize(4, 4, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) == theme_manager_get_font("font_xs"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(std::string(lv_label_get_text(hotend_name)) == lv_tr("H"));
    CHECK(std::string(lv_label_get_text(aux_name)) == lv_tr("C"));

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_16);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_16);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_16);

    // Compact: row fills the full column width (LV_PCT(100)) — an
    // architectural guarantee regardless of how wide "compact" happens to be
    // granted here, unlike a raw pixel comparison against the bigger form
    // (see file header) — and content is centered within it.
    CHECK(lv_obj_get_width(part_row) == lv_obj_get_content_width(h.root()));
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_CENTER);

    // --- Width alone: at the width threshold, height still sub-threshold. ---
    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(std::string(lv_label_get_text(part_name)) == part_bigger_text);
    CHECK(std::string(lv_label_get_text(hotend_name)) == hotend_bigger_text);
    CHECK(std::string(lv_label_get_text(aux_name)) == aux_bigger_text);

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_24);

    // Bigger: row switches to LV_FLEX_ALIGN_START (left-aligned content,
    // widened to the widest row) instead of compact's CENTER. Unlike a row
    // width comparison this does not depend on how wide the grant is or how
    // long the resolved fan name is — it is set unconditionally per branch
    // (see file header for why a width-based assertion was dropped).
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- Height alone, at a real Large-tier 1x1 extent (107x141, from the
    // measured tier table): a plain 1x1 widget must stay compact here.
    // Before this fix, height_px(141) >= h_tall() alone flipped this to
    // "bigger" and squeezed "Hotend"/"Chamber" into a 107px-wide row.
    // Contradicting span: 1x1 (old rowspan/colspan predicate -> compact, so
    // this also proves pixels don't silently regress to spans).
    h.resize(1, 1, 107, 141);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) == theme_manager_get_font("font_xs"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(std::string(lv_label_get_text(hotend_name)) == lv_tr("H"));
    CHECK(std::string(lv_label_get_text(aux_name)) == lv_tr("C"));

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_16);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_16);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_16);

    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_CENTER);

    // --- XLarge-tier 1x1 (134x169): same story, taller still.
    h.resize(1, 1, 134, 169);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_CENTER);

    // --- Medium-tier 1x1 (114x112): both axes below their floor anyway —
    // never promoted the old way either, kept here as a same-shape baseline.
    h.resize(1, 1, 114, 112);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));

    // --- Micro-tier 1x2 (70x131): the legitimate tall case. Narrower than
    // any of the Large/XLarge false positives above, and genuinely
    // rowspan==2 — but this widget's "bigger" is width-only now, so a
    // colspan==1 widget stays compact here regardless of row count. That is
    // a real behavior change from the pre-fix OR (which showed resolved
    // names in this same 70px row), traded deliberately: full names never
    // fit a 70px row named/iconed/valued in three parts, tall or not.
    h.resize(1, 2, 70, 131);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
}

/**
 * Carousel mode takes the early return at the top of on_size_changed
 * (`!widget_obj_ || is_carousel_mode()`), below which Task 4's pixel
 * predicate lives. This proves two things: the carousel component is
 * genuinely what gets built (not the stack component silently substituted
 * by a harness that forgot set_config()), and driving resize() across the
 * same w_normal()/h_tall() threshold used above never reaches that predicate.
 *
 * `fan_stack_part_name`/`fan_stack_part_speed`/`fan_stack_part_row` (the
 * objects the stack-mode test above asserts on) do not exist anywhere in
 * `panel_widget_fan_carousel.xml` or the `fan_arc_core` component it embeds
 * — confirmed below by asserting the lookups miss. So the strong per-object
 * "this label's font moved" check from the stack test is not available
 * here; there is nothing named that to check. What carousel mode DOES have,
 * from bind_carousel_fans() (fan_stack_widget.cpp), is a `speed_label`
 * (text_heading) and `fan_icon` per carousel page, explicitly given
 * "font_xs" at construction time — those are the closest carousel
 * equivalent of the stack test's font assertions, so this test pins them
 * instead.
 *
 * Mutation testing this against `is_carousel_mode()` removed from the guard
 * (leaving only `!widget_obj_`) does NOT turn this test red: every branch in
 * on_size_changed's body is gated behind either a `fan_stack_*` name lookup
 * (all miss, per the CHECKs above) or a `part_label_`/`hotend_label_`/
 * `aux_label_`/`part_icon_`/`hotend_icon_`/`aux_icon_` member pointer, none
 * of which attach_carousel() ever populates — those stay their default
 * nullptr for a carousel-mode widget. So the whole function body is
 * unreachable from a carousel-built widget regardless of the guard, and
 * `is_carousel_mode()` is defensive rather than load-bearing *as far as this
 * test's own observables go* — it does not prove no other path could ever
 * depend on it. Recorded here rather than papering over it with an
 * assertion that happens to fail.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack carousel mode ignores the pixel size predicate",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    PanelWidgetHarness<FanStackWidget> h(
        test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack", state());

    // Proves the carousel component was actually built, not the stack one
    // (see harness's own test for what a missing HarnessConfig would do).
    REQUIRE(h.child("fan_carousel") != nullptr);

    // The stack-only names on_size_changed's pixel predicate would move
    // simply do not exist under the carousel component.
    CHECK(h.child("fan_stack_part_name") == nullptr);
    CHECK(h.child("fan_stack_part_speed") == nullptr);
    CHECK(h.child("fan_stack_part_row") == nullptr);

    // Placeholder fan entries (no fans discovered under this fixture) still
    // produce a carousel page with a real speed_label/fan_icon pair.
    lv_obj_t* speed_label = h.child("speed_label");
    lv_obj_t* fan_icon = h.child("fan_icon");
    REQUIRE(speed_label != nullptr);
    REQUIRE(fan_icon != nullptr);

    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    const lv_font_t* speed_font_before = lv_obj_get_style_text_font(speed_label, LV_PART_MAIN);
    std::string speed_text_before = lv_label_get_text(speed_label);
    CHECK(speed_font_before == xs_font);

    // Drive resize() past the threshold in both directions — the same
    // stimuli that flip the stack test's labels to font_small/full names.
    h.resize(4, 4, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);

    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);

    h.resize(1, 1, w_normal() - 1, h_tall());
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);
}

/**
 * The threshold assertions above are threshold-relative: they drive the
 * harness from the band itself (`W_NORMAL - 1`, `W_NORMAL`) and check that the
 * layout flipped. That proves the predicate is wired to the band. It cannot
 * prove the band is calibrated, because both sides of the comparison move
 * together — which is why they stayed green through the defect this case is
 * about.
 *
 * The defect: the band was a flat 135px, measured against the Small tier's
 * 14px font_body. On the roomier tiers font_body is 18/20/24/32px, so a cell
 * that clears 135px there buys far fewer glyphs than the same 135px bought at
 * Small. `bigger` fired anyway, the rows switched to LV_SIZE_CONTENT with
 * resolved fan names in the tier's larger font, and the resulting row was
 * WIDER than the cell it was drawn into — "Part Cooling Fan" clipped where the
 * compact layout would have drawn "P".
 *
 * So this case asserts the property that actually matters and that no
 * threshold-relative check can express: whatever layout the predicate picks,
 * the row it produces has to fit inside the width the widget was granted.
 * Each case names a real tier, moves the display there (which is what moves
 * the font ladder — theme_manager_refresh_layout_constants), and grants a
 * width inside the window where the flat band promoted and the tier's own
 * band does not.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack rows fit the cell they were granted",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Real discovered fans, so the "bigger" branch draws the resolved display
    // names it draws on a printer rather than the short hardcoded fallbacks.
    // The bare Klipper `fan` object maps to "Part Cooling Fan"
    // (device_display_name.cpp DIRECT_MAPPINGS) — the exact string the defect
    // report showed clipped.
    state().init_fans({"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    helix::ui::UpdateQueue::instance().drain();

    struct FitCase {
        const char* name;
        int32_t hor;
        int32_t ver;
        int width_px;
        int height_px;
    };

    // Widths sit above the flat 135px band and below the tier's own band, the
    // exact window the flat band got wrong. Heights are a real single-row
    // extent for the tier so nothing else about the case is artificial.
    const FitCase cases[] = {
        {"medium 800x480", 800, 480, 136, 112},
        {"large 1024x600", 1024, 600, 160, 141},
        {"xxlarge 1080x1920", 1080, 1920, 211, 200},
    };

    for (const auto& c : cases) {
        ScopedResolution res(disp, c.hor, c.ver);
        theme_manager_refresh_layout_constants(disp);

        PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());
        lv_obj_t* part_row = h.child("fan_stack_part_row");
        REQUIRE(part_row != nullptr);

        h.resize(1, 1, c.width_px, c.height_px);
        process_lvgl(30);

        // on_size_changed levels all three rows to the widest, so any one of
        // them reports the overflow; check every row rather than assuming
        // which name resolved longest on this tier.
        const int cell_w = lv_obj_get_content_width(h.root());
        for (const char* rn : {"fan_stack_part_row", "fan_stack_hotend_row", "fan_stack_aux_row"}) {
            lv_obj_t* row = h.child(rn);
            REQUIRE(row != nullptr);
            if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN))
                continue;
            INFO(c.name << ": granted " << c.width_px << "px, cell content " << cell_w << "px, "
                        << rn << " " << lv_obj_get_width(row) << "px, band " << w_normal()
                        << ", part name '" << lv_label_get_text(h.child("fan_stack_part_name"))
                        << "'");
            CHECK(lv_obj_get_width(row) <= cell_w);
        }
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
}
