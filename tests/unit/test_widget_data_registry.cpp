// SPDX-License-Identifier: GPL-3.0-or-later

#include "widget_data_registry.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::WidgetDataRegistry;

namespace {

// The registry only compares widget pointers; it never dereferences them.
char g_widgets[3];

lv_obj_t* widget(int i) {
    return reinterpret_cast<lv_obj_t*>(&g_widgets[i]);
}

} // namespace

TEST_CASE("WidgetDataRegistry returns what was registered per widget", "[widget_data_registry]") {
    WidgetDataRegistry<int> reg;
    int a = 1, b = 2;
    CHECK(reg.get(widget(0)) == nullptr);
    reg.set(widget(0), &a);
    reg.set(widget(1), &b);
    CHECK(reg.get(widget(0)) == &a);
    CHECK(reg.get(widget(1)) == &b);
    CHECK(reg.get(widget(2)) == nullptr);
    CHECK(reg.size() == 2);
}

TEST_CASE("WidgetDataRegistry set replaces an earlier entry", "[widget_data_registry]") {
    WidgetDataRegistry<int> reg;
    int a = 1, b = 2;
    reg.set(widget(0), &a);
    reg.set(widget(0), &b);
    CHECK(reg.get(widget(0)) == &b);
    CHECK(reg.size() == 1);
}

TEST_CASE("WidgetDataRegistry take removes and returns the entry", "[widget_data_registry]") {
    WidgetDataRegistry<int> reg;
    int a = 1, b = 2;
    reg.set(widget(0), &a);
    reg.set(widget(1), &b);
    CHECK(reg.take(widget(0)) == &a);
    CHECK(reg.get(widget(0)) == nullptr);
    CHECK(reg.take(widget(0)) == nullptr);
    CHECK(reg.get(widget(1)) == &b);
    CHECK(reg.size() == 1);
}

TEST_CASE("WidgetDataRegistry for_each visits every entry and clear empties it",
          "[widget_data_registry]") {
    WidgetDataRegistry<int> reg;
    int a = 1, b = 2, c = 4;
    reg.set(widget(2), &c);
    reg.set(widget(0), &a);
    reg.set(widget(1), &b);
    int sum = 0;
    int visits = 0;
    reg.for_each([&](lv_obj_t* obj, int* data) {
        CHECK(reg.get(obj) == data);
        sum += *data;
        ++visits;
    });
    CHECK(visits == 3);
    CHECK(sum == 7);
    reg.clear();
    CHECK(reg.empty());
    CHECK(reg.get(widget(1)) == nullptr);
}
