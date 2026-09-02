// Copyright (C) 2025-2026 356C LLC
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
 * `fan_stack_widget.cpp`) is no longer branch-dependent: both tiers run the
 * same leveling pass, because per-row centering put no two icons at the
 * same x once rows held different-width text (compact's "0%" vs "100%",
 * bigger's "P" vs "Hotend"). Every row is set to `LV_SIZE_CONTENT`, the
 * widest is measured, and all three are pinned to that width, so the rows
 * share one left edge while the parent's `cross_place=center` centers the
 * equal-width block on the tile. A raw row width is still not a reliable
 * cross-threshold observable (it depends on the resolved fan names and the
 * reserved speed-label box, not on the grant), so this file asserts the
 * `flex_main_place` style — `LV_FLEX_ALIGN_START` at both tiers, set
 * unconditionally by the leveling pass — and leaves the icons-share-one-x
 * property to its own case below, where real fans with deliberately
 * mismatched speeds make it a meaningful red.
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
    //
    // rowspan stays at 2 (one authored cell) so the icon-above layout is out
    // of play — that one IS span-gated, deliberately, and promotes the text to
    // font_body. colspan carries the span-contradiction this case is about.
    h.resize(4, 2, w_normal() - 1, h_tall() - 1);
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

    // Compact: rows level to the widest and left-align content within the
    // equal-width block, same as the bigger form — per-row centering is what
    // this used to assert, and it is exactly what made the icons ragged.
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

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

    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- XLarge-tier 1x1 (134x169): same story, taller still.
    h.resize(1, 1, 134, 169);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

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
 * The user-facing property the row leveling exists for: all three fan icons
 * sit at the same x, at every tier, even when the rows hold different-width
 * text. The setup gives the fans deliberately mismatched speeds before the
 * first resize (part "35%", the rest "0%") so the rows genuinely differ in
 * width — under the old per-row centering the part icon sat measurably right
 * of the others, which is the red this case was verified against.
 *
 * The second half pins the speed-label reserve: rows are leveled once at
 * on_size_changed while speed text keeps changing afterwards, so every speed
 * label reserves the width of the widest string the formatter can emit
 * ("100%"). Without it, a fan ramping from "0%" to "100%" after layout
 * would outgrow its fixed row and clip — the same defect shape the
 * fit-the-cell case below guards against at the tier band.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack icons line up one under the other",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    // Real fans so all three rows stay visible and the roles resolve. The
    // bare `fan` object maps to "Part Cooling Fan" (device_display_name.cpp
    // DIRECT_MAPPINGS) — same trio as the fit-the-cell case below.
    state().init_fans({"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    state().update_fan_speed("fan", 0.35);
    helix::ui::UpdateQueue::instance().drain();

    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* hotend_icon = h.child("fan_stack_hotend_icon");
    lv_obj_t* aux_icon = h.child("fan_stack_aux_icon");
    REQUIRE(part_icon != nullptr);
    REQUIRE(hotend_icon != nullptr);
    REQUIRE(aux_icon != nullptr);
    REQUIRE(!lv_obj_has_flag(h.child("fan_stack_aux_row"), LV_OBJ_FLAG_HIDDEN));

    // --- Compact tier: one shared left edge despite "35%" vs "0%" rows. ---
    h.resize(1, 1, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(hotend_icon));
    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(aux_icon));

    // Speeds justify against the shared right edge: right-aligned inside
    // their reserved boxes, so a "0%" row ends where the "100%" row does
    // and the leveled block reads as justified, not left-packed.
    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    lv_obj_t* hotend_speed = h.child("fan_stack_hotend_speed");
    REQUIRE(part_speed != nullptr);
    CHECK(lv_obj_get_style_text_align(part_speed, LV_PART_MAIN) == LV_TEXT_ALIGN_RIGHT);
    // The name column is leveled too, so the speed boxes share one x (and
    // with equal reserved widths, one right edge) — without it the P/H/C
    // advance-width differences rag the speed column by a pixel or two.
    REQUIRE(hotend_speed != nullptr);
    CHECK(lv_obj_get_x(part_speed) == lv_obj_get_x(hotend_speed));

    // --- Speed ramp after layout: "0%" -> "100%" must fit the fixed row. ---
    state().update_fan_speed("heater_fan hotend_fan", 1.0);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(30);

    lv_obj_t* hotend_row = h.child("fan_stack_hotend_row");
    REQUIRE(hotend_row != nullptr);
    CHECK(lv_obj_get_x(hotend_speed) + lv_obj_get_width(hotend_speed) <=
          lv_obj_get_width(hotend_row));

    // --- Bigger tier: same shared edge with resolved display names. ---
    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(hotend_icon));
    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(aux_icon));
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

/**
 * The icon-above layout (stacked_row_layout.h), the fan_stack twin of the
 * temp_stack case in test_widget_size_temp_stack.cpp.
 *
 * Two things are specific to fan_stack and asserted here rather than there.
 * The name and speed must stay on ONE line under the glyph — that is what the
 * `fan_stack_*_value` wrapper in panel_widget_fan_stack.xml exists for, and a
 * row flipped to a column without it would break them onto separate lines.
 * And the name gets a MEASURED ladder here rather than the width band: a
 * stacked row hands the name+speed pair the widget's whole content width, so
 * the band (which answers "does a resolved name fit BESIDE the glyph") is
 * calibrated for the wrong layout. Both ends of that ladder are pinned — a
 * sub-band column that still fits a word gets one, a column that fits nothing
 * falls back to the single letter.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack stacks icons above name+speed only when it fits",
                 "[widget_size][fan_stack][panel_widget]") {
    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_row = h.child("fan_stack_part_row");
    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* part_value = h.child("fan_stack_part_value");
    lv_obj_t* part_name = h.child("fan_stack_part_name");
    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    REQUIRE(part_row != nullptr);
    REQUIRE(part_icon != nullptr);
    REQUIRE(part_value != nullptr);
    REQUIRE(part_name != nullptr);
    REQUIRE(part_speed != nullptr);

    // The name and speed share one inner row in both layouts. Assert it here
    // once: everything below reasons about `part_value` as a single block.
    REQUIRE(lv_obj_get_parent(part_name) == part_value);
    REQUIRE(lv_obj_get_parent(part_speed) == part_value);
    REQUIRE(lv_obj_get_parent(part_value) == part_row);

    auto name_and_speed_share_a_line = [&]() {
        // Vertical overlap, and the name to the left of the speed.
        return lv_obj_get_y(part_name) < lv_obj_get_y(part_speed) + lv_obj_get_height(part_speed) &&
               lv_obj_get_y(part_speed) < lv_obj_get_y(part_name) + lv_obj_get_height(part_name) &&
               lv_obj_get_x(part_name) < lv_obj_get_x(part_speed);
    };

    // One authored cell: compact rows, however tall the cell is.
    h.resize(2, 2, w_normal() - 1, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_x(part_icon) + lv_obj_get_width(part_icon) <= lv_obj_get_x(part_value));
    CHECK(name_and_speed_share_a_line());

    // Taller than a cell, with room: glyph moves above the pair, text goes to
    // font_body — a rung past the font_small that the width band alone buys.
    h.resize(2, 4, w_normal() - 1, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_get_y(part_icon) + lv_obj_get_height(part_icon) <= lv_obj_get_y(part_value));
    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));

    // ...and the pair is still one line, not two.
    CHECK(name_and_speed_share_a_line());

    // A stacked row puts the name on its own line, so the glyph is no longer
    // competing for that width and a longer form fits than the width band
    // would allow beside it. This column is BELOW w_normal() — the band alone
    // would force the single letter — yet the name is a word.
    CHECK(std::string(lv_label_get_text(part_name)) != lv_tr("P"));

    // The longer form is measured, not assumed: a column with no room for any
    // word still falls back to the single letter rather than overflowing.
    h.resize(2, 4, 40, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));

    // Wide AND tall: the name still shares its line with the speed under the
    // glyph.
    h.resize(4, 4, w_normal(), 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(std::string(lv_label_get_text(part_name)) != lv_tr("P"));
    CHECK(name_and_speed_share_a_line());

    // Every visible row uses the SAME form, at every width. Picking each row's
    // own longest-that-fits would render "Part / Hotend / C", which reads as a
    // rendering fault rather than a deliberate abbreviation. "Hotend" is the
    // longer of the two visible short words, so the pair disagreeing is
    // exactly the failure this sweeps for.
    lv_obj_t* hotend_name = h.child("fan_stack_hotend_name");
    REQUIRE(hotend_name != nullptr);
    for (int w = 30; w <= w_normal() + 60; w += 7) {
        h.resize(2, 4, w, 400);
        process_lvgl(30);
        const bool part_is_letter = std::string(lv_label_get_text(part_name)) == lv_tr("P");
        const bool hotend_is_letter = std::string(lv_label_get_text(hotend_name)) == lv_tr("H");
        INFO("width " << w << ": part '" << lv_label_get_text(part_name) << "', hotend '"
                      << lv_label_get_text(hotend_name) << "'");
        CHECK(part_is_letter == hotend_is_letter);
    }

    // Taller than a cell but with nowhere to put the second line: compact
    // rows, fully restored rather than left half-converted.
    h.resize(2, 4, w_normal() - 1, 40);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_x(part_icon) + lv_obj_get_width(part_icon) <= lv_obj_get_x(part_value));
    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
}
