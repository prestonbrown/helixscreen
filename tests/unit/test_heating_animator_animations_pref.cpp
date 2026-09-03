// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_heating_animator_animations_pref.cpp
 * @brief The "Animations" preference gates the heater icon pulse — and nothing else.
 *
 * HeatingIconAnimator runs an LV_ANIM_REPEAT_INFINITE opacity pulse while a
 * heater climbs toward a setpoint, and the preference that governs motion has
 * to reach it: a user who switched animations off still got a moving icon. Each
 * step also restyles and invalidates the icon, which is a couple of points of
 * one core on a host that shares its processor with Klipper's motion loop.
 *
 * Switching animations off stops the motion only. The four thermal states keep
 * their own colours — muted off, red heating, green at-temp, blue cooling — so
 * the icon still says what the heater is doing, at a steady full opacity. A
 * test that only checked "no animation" would accept a fix that flattened the
 * icon to one colour, so every case here pins the colour as well.
 */

#include "ui_heating_animator.h"
#include "ui_temperature_utils.h"

#include "../test_fixtures.h"
#include "../test_helpers/scoped_animations_enabled.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>

#include "../catch_amalgamated.hpp"

using helix::ui::temperature::get_heating_state_color;
using helix::ui::temperature::HeatState;

namespace {

/// True while LVGL is running any animation against @p animator as its `var`.
/// A null exec_cb matches every callback, so this finds the pulse whatever it
/// drives.
bool is_pulsing(const HeatingIconAnimator& animator) {
    return lv_anim_get(const_cast<HeatingIconAnimator*>(&animator), nullptr) != nullptr;
}

lv_color_t icon_color(lv_obj_t* icon) {
    return lv_obj_get_style_text_color(icon, LV_PART_MAIN);
}

lv_opa_t icon_opa(lv_obj_t* icon) {
    return lv_obj_get_style_text_opa(icon, LV_PART_MAIN);
}

/// Catch2 prints these as integers on failure, which is what you want to read.
uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c);
}

/// LVGL fixture that starts with the animations preference switched ON.
///
/// XMLTestFixture rather than the lighter LVGLTestFixture because it runs
/// theme_manager_init(): without a loaded palette every token resolves to the
/// same black, and both "the icon carries the state colour" and "the states are
/// distinguishable" would hold for a monochrome icon.
class AnimatorPrefFixture : public XMLTestFixture {
  public:
    AnimatorPrefFixture() {
        // Without the subject there is no preference to gate on and every case
        // below would assert against the unconditional behaviour.
        REQUIRE(animations_.available());

        // The premise the colour assertions rest on: these four tokens really
        // are four different colours in the loaded theme.
        REQUIRE(rgb(get_heating_state_color(HeatState::Heating)) !=
                rgb(get_heating_state_color(HeatState::AtTemp)));
    }

    void set_animations(bool on) {
        animations_.set(on);
    }

    /// A bare icon widget, the shape every current call site attaches to.
    lv_obj_t* make_icon() {
        return lv_obj_create(test_screen());
    }

  private:
    helix::ui::ScopedAnimationsEnabled animations_;
};

} // namespace

// The premise for every "must not pulse" case below: with animations on, a
// heater climbing toward a setpoint really does run an infinite animation.
// Without this the negative assertions could pass for the wrong reason.
TEST_CASE_METHOD(AnimatorPrefFixture, "heater icon pulses while heating and animations are on",
                 "[animator][animations_pref]") {
    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000);

    REQUIRE(animator.get_state() == HeatState::Heating);
    CHECK(is_pulsing(animator));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.detach();
}

TEST_CASE_METHOD(AnimatorPrefFixture, "animations off: heating icon holds still at full opacity",
                 "[animator][animations_pref]") {
    set_animations(false);

    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000);

    REQUIRE(animator.get_state() == HeatState::Heating);
    CHECK_FALSE(is_pulsing(animator));

    // The owner's decision: the state colour stays, only the oscillation goes.
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));
    CHECK(icon_opa(icon) == LV_OPA_COVER);

    // And it stays put — a pulse left running would have walked the opacity
    // down toward 80% by now.
    process_lvgl(500);
    CHECK_FALSE(is_pulsing(animator));
    CHECK(icon_opa(icon) == LV_OPA_COVER);
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.detach();
}

// The second half of the decision, stated as a property: with the motion gone,
// the four states must still be told apart by colour alone.
TEST_CASE_METHOD(AnimatorPrefFixture, "animations off: the thermal states stay distinct colours",
                 "[animator][animations_pref]") {
    set_animations(false);

    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    struct Case {
        int current;
        int target;
        HeatState state;
    };
    // 150 -> 200 climbing, 200 at 200, 250 above 200, and target 0 = off.
    const Case cases[] = {
        {1500, 2000, HeatState::Heating},
        {2000, 2000, HeatState::AtTemp},
        {2500, 2000, HeatState::Cooling},
        {2000, 0, HeatState::Off},
    };

    uint32_t seen[std::size(cases)] = {};
    for (size_t i = 0; i < std::size(cases); i++) {
        animator.update(cases[i].current, cases[i].target);
        INFO("case " << i << " current=" << cases[i].current << " target=" << cases[i].target);
        REQUIRE(animator.get_state() == cases[i].state);
        CHECK_FALSE(is_pulsing(animator));
        CHECK(icon_opa(icon) == LV_OPA_COVER);
        CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(cases[i].state)));
        seen[i] = rgb(icon_color(icon));
    }

    // Distinguishable, not merely "some colour was applied".
    for (size_t i = 0; i < std::size(seen); i++) {
        for (size_t j = i + 1; j < std::size(seen); j++) {
            INFO("state " << i << " vs " << j);
            CHECK(seen[i] != seen[j]);
        }
    }

    animator.detach();
}

TEST_CASE_METHOD(AnimatorPrefFixture, "toggling the preference reaches a live heater icon",
                 "[animator][animations_pref]") {
    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000);
    REQUIRE(animator.get_state() == HeatState::Heating);
    REQUIRE(is_pulsing(animator));
    process_lvgl(200); // let the opacity walk away from full

    // Off must stop an icon that is ALREADY pulsing, not only affect icons
    // attached afterwards — and it must leave it fully opaque rather than at
    // whatever mid-pulse value the last step wrote.
    set_animations(false);
    CHECK_FALSE(is_pulsing(animator));
    CHECK(icon_opa(icon) == LV_OPA_COVER);
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    process_lvgl(500);
    CHECK(icon_opa(icon) == LV_OPA_COVER);

    // Back on, and a heater that is still heating pulses again.
    set_animations(true);
    CHECK(is_pulsing(animator));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.detach();
}

// Turning the preference on must not start a pulse for a state that never
// pulses — the gate is an extra condition, not a replacement for the state
// machine.
TEST_CASE_METHOD(AnimatorPrefFixture, "the preference does not pulse a heater that is at temp",
                 "[animator][animations_pref]") {
    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(2000, 2000);
    REQUIRE(animator.get_state() == HeatState::AtTemp);
    REQUIRE_FALSE(is_pulsing(animator));

    set_animations(false);
    CHECK_FALSE(is_pulsing(animator));
    set_animations(true);
    CHECK_FALSE(is_pulsing(animator));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::AtTemp)));

    animator.detach();
}

// ============================================================================
// Pulse cost — the per-frame path must write less, and look identical.
// ============================================================================

// The pulse moves the opacity and nothing else, so a step that skips the colour
// write must still leave the state colour on the icon.
TEST_CASE_METHOD(AnimatorPrefFixture, "a running pulse keeps the state colour on every step",
                 "[animator][animations_pref][pulse_cost]") {
    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000);
    REQUIRE(is_pulsing(animator));

    const uint32_t heating = rgb(get_heating_state_color(HeatState::Heating));
    bool saw_partial_opacity = false;
    for (int step = 0; step < 12; step++) {
        process_lvgl(60);
        INFO("step " << step);
        CHECK(rgb(icon_color(icon)) == heating);
        // 204..255 is the declared pulse range; anything outside means the
        // opacity write went somewhere it should not.
        CHECK(icon_opa(icon) >= 204);
        CHECK(icon_opa(icon) <= LV_OPA_COVER);
        if (icon_opa(icon) < LV_OPA_COVER) {
            saw_partial_opacity = true;
        }
    }
    // Premise: the pulse really did move the opacity, so the colour assertions
    // above were checked against a moving target rather than a static icon.
    CHECK(saw_partial_opacity);

    animator.detach();
}

// Skipping unchanged writes must not skip a change. A colour that moves while
// the pulse is mid-flight has to reach the icon.
TEST_CASE_METHOD(AnimatorPrefFixture, "a state change mid-pulse repaints the icon colour",
                 "[animator][animations_pref][pulse_cost]") {
    lv_obj_t* icon = make_icon();
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000);
    REQUIRE(is_pulsing(animator));
    process_lvgl(150);
    REQUIRE(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.update(2000, 2000);
    CHECK(animator.get_state() == HeatState::AtTemp);
    CHECK_FALSE(is_pulsing(animator));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::AtTemp)));
    CHECK(icon_opa(icon) == LV_OPA_COVER);

    // Straight back into heating: the colour must return too.
    animator.update(1500, 2000);
    CHECK(is_pulsing(animator));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.detach();
}

// Attaching to a wrapper component tints its glyph child rather than nothing,
// and the child has to follow the pulse and the preference exactly as the
// wrapper does.
TEST_CASE_METHOD(AnimatorPrefFixture, "a wrapper's child icon follows the tint and the pulse",
                 "[animator][animations_pref][pulse_cost]") {
    lv_obj_t* wrapper = make_icon();
    lv_obj_t* glyph = lv_obj_create(wrapper);

    // A real glyph carries its own variant style, so it does NOT inherit the
    // wrapper's text color. Give it one here, or LVGL's inheritance would
    // satisfy every assertion below even with the child walk removed.
    lv_obj_set_style_text_color(glyph, lv_color_hex(0x123456), LV_PART_MAIN);
    lv_obj_set_style_text_opa(glyph, LV_OPA_50, LV_PART_MAIN);

    HeatingIconAnimator animator;
    animator.attach(wrapper);

    animator.update(1500, 2000);
    REQUIRE(is_pulsing(animator));
    CHECK(rgb(icon_color(glyph)) == rgb(get_heating_state_color(HeatState::Heating)));

    process_lvgl(150);
    CHECK(rgb(icon_color(glyph)) == rgb(get_heating_state_color(HeatState::Heating)));
    CHECK(icon_opa(glyph) == icon_opa(wrapper));

    set_animations(false);
    CHECK_FALSE(is_pulsing(animator));
    CHECK(icon_opa(glyph) == LV_OPA_COVER);
    CHECK(rgb(icon_color(glyph)) == rgb(get_heating_state_color(HeatState::Heating)));

    animator.detach();
}

// ============================================================================
// Move semantics — the pulse and the observers must follow the object.
// ============================================================================

// A move leaves the source a corpse the caller is free to destroy. Anything
// still registered against it — the pulse animation's `var`, the preference
// observer's user data — is a dangling pointer LVGL will dereference on its
// next tick or notification.
TEST_CASE_METHOD(AnimatorPrefFixture, "moving an animator carries the pulse to the new owner",
                 "[animator][animations_pref][move]") {
    lv_obj_t* icon = make_icon();

    auto source = std::make_unique<HeatingIconAnimator>();
    source->attach(icon);
    source->update(1500, 2000);
    REQUIRE(is_pulsing(*source));

    HeatingIconAnimator moved(std::move(*source));
    CHECK(moved.is_attached());
    CHECK_FALSE(source->is_attached());

    // Nothing may still be animating against the moved-from object.
    CHECK(lv_anim_get(source.get(), nullptr) == nullptr);
    CHECK(is_pulsing(moved));

    source.reset(); // the corpse goes away; LVGL must not still hold a pointer

    process_lvgl(200);
    CHECK(is_pulsing(moved));
    CHECK(rgb(icon_color(icon)) == rgb(get_heating_state_color(HeatState::Heating)));

    // The preference observer has to have moved too, or the toggle reaches a
    // freed object instead of this one.
    set_animations(false);
    CHECK_FALSE(is_pulsing(moved));
    CHECK(icon_opa(icon) == LV_OPA_COVER);

    set_animations(true);
    CHECK(is_pulsing(moved));

    moved.detach();
}
