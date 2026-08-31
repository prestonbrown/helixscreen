// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which widget `helix-screen ctl click <name>` lands on when the name exists
// more than once on screen. The rule packs three fields into one comparable
// key - layer, top-level ancestor index, discovery order - and the packing is
// where it breaks: the layer rank sits at bit 40, so a 32-bit accumulator
// cannot hold it. `long` IS 32 bits on every 32-bit device target (armv7
// K1/K2/AD5M/CC1, armhf Pi); there the shift is undefined and the rank folds
// into the index field, so a click aimed at a modal lands on a widget under
// the panel behind it. The build host is LP64, so the width is not something a
// desktop run notices - hence the explicit width assertion below.

#include "../lvgl_test_fixture.h"
#include "widget_resolution.h"

#include <cstdint>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Deletes exactly what the test parented onto the top layer, and nothing else.
/// LVGLTestFixture hands out a fresh screen per test but lv_layer_top() is
/// process-wide, so a leftover modal there sits above every later test's UI.
/// Cleaning the whole layer is not an option: safe_delete_deferred_raw()
/// reparents condemned subtrees there while their deletion is pending, so a
/// blanket clean would eat another test's in-flight teardown.
class TopLayerObjects {
  public:
    lv_obj_t* add(lv_obj_t* o) {
        owned_.push_back(o);
        return o;
    }
    ~TopLayerObjects() {
        for (lv_obj_t* o : owned_) {
            lv_obj_delete(o);
        }
    }

  private:
    std::vector<lv_obj_t*> owned_;
};

/// A named button wrapped in a container, which is the shape resolution walks:
/// the container is the "top-level ancestor" whose child index ranks it.
lv_obj_t* named_button_in_container(lv_obj_t* root, const char* name) {
    lv_obj_t* container = lv_obj_create(root);
    lv_obj_t* button = lv_obj_create(container);
    lv_obj_set_name(button, name);
    return button;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ctl pick: a modal on the top layer outranks a deeper overlay",
                 "[remote][ctl][pick]") {
    TopLayerObjects top_layer;
    lv_obj_t* screen = lv_screen_active();

    // The modal the user is actually looking at.
    lv_obj_t* modal_match = named_button_in_container(lv_layer_top(), "action_button");
    top_layer.add(lv_obj_get_parent(modal_match));
    const int32_t modal_index = lv_obj_get_index(lv_obj_get_parent(modal_match));

    // Stack screen overlays until the screen-side copy of the name sits at a
    // HIGHER top-level index than the modal. That is the case a collapsed key
    // gets wrong: with the rank folded away, a large index wins outright.
    for (int32_t i = 0; i <= modal_index; ++i) {
        lv_obj_create(screen);
    }
    lv_obj_t* screen_match = named_button_in_container(screen, "action_button");

    REQUIRE(lv_obj_get_index(lv_obj_get_parent(screen_match)) > modal_index);

    // matches_for_name() collects the active screen first, then the top layer,
    // so the screen copy also carries the LOWER discovery order. Both lesser
    // fields therefore favour the wrong answer, and only the layer rank can
    // decide it.
    const std::vector<lv_obj_t*> matches{screen_match, modal_match};
    REQUIRE(helix::topmost_visible(matches) == modal_match);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl pick: the layer rank does not fit in 32 bits",
                 "[remote][ctl][pick]") {
    TopLayerObjects top_layer;

    // The behavioural case above passes on an LP64 host whether the key is
    // accumulated in `long` or int64_t, because there they are the same type.
    // This is the assertion that does not: a top-layer widget's key is above
    // INT32_MAX by construction, so any 32-bit accumulator truncates it and
    // the rank stops outranking anything.
    lv_obj_t* modal_match = named_button_in_container(lv_layer_top(), "action_button");
    top_layer.add(lv_obj_get_parent(modal_match));
    const int64_t key = helix::widget_pick_key(modal_match, 0);

    CHECK(key > static_cast<int64_t>(INT32_MAX));
    CHECK((key >> 40) == 1); // the rank field, read back where it was written

    // And a screen-side widget must leave that field clear, or the rank means
    // nothing.
    lv_obj_t* screen_match = named_button_in_container(lv_screen_active(), "action_button");
    CHECK((helix::widget_pick_key(screen_match, 0) >> 40) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl pick: within one layer the later overlay wins",
                 "[remote][ctl][pick]") {
    lv_obj_t* screen = lv_screen_active();

    // The ordinary case the rank must not disturb: overlays stack as increasing
    // child indices, so the one pushed last is the one on top.
    lv_obj_t* lower = named_button_in_container(screen, "action_button");
    lv_obj_t* upper = named_button_in_container(screen, "action_button");

    REQUIRE(helix::topmost_visible({lower, upper}) == upper);
    // Discovery order must not be able to overturn the index, so the same two
    // in the opposite order still resolve to the upper overlay.
    REQUIRE(helix::topmost_visible({upper, lower}) == upper);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl pick: siblings tie-break on discovery order",
                 "[remote][ctl][pick]") {
    // Two matches inside ONE overlay share a layer and an ancestor index, so
    // the only field left is discovery order - the later one found wins, which
    // is the depth-first-latest widget.
    lv_obj_t* overlay = lv_obj_create(lv_screen_active());
    lv_obj_t* first = lv_obj_create(overlay);
    lv_obj_set_name(first, "action_button");
    lv_obj_t* second = lv_obj_create(overlay);
    lv_obj_set_name(second, "action_button");

    REQUIRE(helix::widget_pick_key(first, 0) < helix::widget_pick_key(second, 1));
    REQUIRE(helix::topmost_visible({first, second}) == second);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl pick: nothing to pick from is not a pick",
                 "[remote][ctl][pick]") {
    // resolve_widget() feeds this straight from a search that found nothing;
    // returning a widget here would click something the caller never named.
    REQUIRE(helix::topmost_visible({}) == nullptr);
}
