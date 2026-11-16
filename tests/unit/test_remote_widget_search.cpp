// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The widget walk behind `helix-screen ctl click/state/ls <name>`: which
// widgets a name predicate sees, which subtrees it skips, and where an
// unscoped search looks.

#include "../lvgl_test_fixture.h"
#include "widget_resolution.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

auto named(const std::string& wanted) {
    return [wanted](const char* name) { return wanted == name; };
}

/// Deletes only what the test put on the process-wide top layer.
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

lv_obj_t* named_child(lv_obj_t* parent, const char* name) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_name(o, name);
    return o;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ctl search: hidden subtrees are skipped unless asked for",
                 "[remote][ctl][search]") {
    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_t* shown = named_child(root, "target");
    lv_obj_t* hidden_parent = named_child(root, "wrapper");
    lv_obj_set_hidden(hidden_parent, true);
    lv_obj_t* inside_hidden = named_child(hidden_parent, "target");
    lv_obj_t* hidden_self = named_child(root, "target");
    lv_obj_set_hidden(hidden_self, true);

    std::vector<lv_obj_t*> visible;
    helix::collect_widgets(root, named("target"), visible);
    CHECK(visible == std::vector<lv_obj_t*>{shown});

    std::vector<lv_obj_t*> everything;
    helix::collect_widgets(root, named("target"), everything, /*include_hidden=*/true);
    CHECK(everything == std::vector<lv_obj_t*>{shown, inside_hidden, hidden_self});
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl search: an unnamed widget matches on its crafted name",
                 "[remote][ctl][search]") {
    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_t* anonymous = lv_obj_create(root);

    char crafted[128];
    lv_obj_get_name_resolved(anonymous, crafted, sizeof(crafted));
    REQUIRE(crafted[0] != '\0');

    std::vector<lv_obj_t*> found;
    helix::collect_widgets(root, named(crafted), found);
    CHECK(found == std::vector<lv_obj_t*>{anonymous});
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl search: the search root itself is not tested",
                 "[remote][ctl][search]") {
    lv_obj_t* root = named_child(lv_screen_active(), "target");
    lv_obj_t* child = named_child(root, "target");

    std::vector<lv_obj_t*> found;
    helix::collect_widgets(root, named("target"), found);
    CHECK(found == std::vector<lv_obj_t*>{child});
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl search: unscoped covers the screen then the top layer",
                 "[remote][ctl][search]") {
    TopLayerObjects top_layer;
    lv_obj_t* on_screen = named_child(lv_screen_active(), "dual_target");
    lv_obj_t* in_modal = top_layer.add(named_child(lv_layer_top(), "dual_target"));

    // Screen first: the order is the discovery order topmost_visible() ties on.
    CHECK(helix::search_widgets(nullptr, named("dual_target")) ==
          std::vector<lv_obj_t*>{on_screen, in_modal});
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl search: a scope confines the search to its subtree",
                 "[remote][ctl][search]") {
    TopLayerObjects top_layer;
    lv_obj_t* scope = lv_obj_create(lv_screen_active());
    lv_obj_t* inside = named_child(scope, "scoped_target");
    named_child(lv_screen_active(), "scoped_target");
    top_layer.add(named_child(lv_layer_top(), "scoped_target"));

    CHECK(helix::search_widgets(scope, named("scoped_target")) == std::vector<lv_obj_t*>{inside});
}
