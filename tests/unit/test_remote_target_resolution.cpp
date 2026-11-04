// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Target resolution for `helix-screen ctl`. Both behaviours here exist because
// driving the UI silently no-op'd: `click <name>` resolved to a same-named
// widget in an overlay stacked *behind* the visible one and reported success,
// and a path copied out of `ls` was rejected unless prefixed with '@'.

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "remote_client.h"
#include "remote_control_server.h"
#include "widget_resolution.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

// --- bare path locators -------------------------------------------------

TEST_CASE("ctl target: bare describe_screen paths are recognised", "[remote][ctl]") {
    // The exact shape `ls` prints, usable without remembering '@'.
    REQUIRE(helix::is_bare_path("s/0"));
    REQUIRE(helix::is_bare_path("s/18/0/1"));
    REQUIRE(helix::is_bare_path("t/0"));
    REQUIRE(helix::is_bare_path("t/1/2/3/4/5"));
}

TEST_CASE("ctl target: widget names are not mistaken for paths", "[remote][ctl]") {
    // Real widget names from the codebase — none may be read as a locator,
    // or clicking them would resolve a child index instead.
    REQUIRE_FALSE(helix::is_bare_path("swatch_0"));
    REQUIRE_FALSE(helix::is_bare_path("action_button_2"));
    REQUIRE_FALSE(helix::is_bare_path("row_theme_settings"));
    REQUIRE_FALSE(helix::is_bare_path("s"));
    REQUIRE_FALSE(helix::is_bare_path("t"));
    REQUIRE_FALSE(helix::is_bare_path(""));
    // Names that merely start with s/t must not be swallowed.
    REQUIRE_FALSE(helix::is_bare_path("slot_grid"));
    REQUIRE_FALSE(helix::is_bare_path("theme_swatch_grid"));
}

TEST_CASE("ctl target: name segments are recognised as locators", "[remote][ctl]") {
    // path_of() emits names where it can, so a locator pasted out of `ls` is
    // mostly words now. The root prefix is what makes it a path, not the
    // segment contents — widget names never contain '/'.
    REQUIRE(helix::is_bare_path("s/main_content/settings_list"));
    REQUIRE(helix::is_bare_path("s/15/toggle[1]"));
    REQUIRE(helix::is_bare_path("t/0/dialog"));
    REQUIRE(helix::is_bare_path("s/main/list/row_theme/toggle"));
}

TEST_CASE("ctl target: malformed locators are rejected", "[remote][ctl]") {
    REQUIRE_FALSE(helix::is_bare_path("s/"));     // no segment
    REQUIRE_FALSE(helix::is_bare_path("s//1"));   // empty segment
    REQUIRE_FALSE(helix::is_bare_path("s/1/"));   // trailing slash
    REQUIRE_FALSE(helix::is_bare_path("x/1"));    // unknown root
    REQUIRE_FALSE(helix::is_bare_path("s/1 /2")); // stray space
    REQUIRE_FALSE(helix::is_bare_path("row_*"));  // glob stays a glob
}

// --- indexed-name tokens ------------------------------------------------

TEST_CASE("ctl target: indexed name tokens split", "[remote][ctl]") {
    std::string name;
    int index = -1;

    REQUIRE(helix::parse_indexed_name("toggle[3]", name, index));
    REQUIRE(name == "toggle");
    REQUIRE(index == 3);

    REQUIRE(helix::parse_indexed_name("row_a[0]", name, index));
    REQUIRE(name == "row_a");
    REQUIRE(index == 0);
}

TEST_CASE("ctl target: plain and malformed names are not indexed", "[remote][ctl]") {
    std::string name = "untouched";
    int index = -1;

    // A plain name is the common case and must not be mistaken for an ordinal.
    REQUIRE_FALSE(helix::parse_indexed_name("toggle", name, index));
    REQUIRE(name == "untouched"); // outputs left alone on a false return

    // Malformed suffixes stay whole names: no widget name contains a bracket,
    // so these simply fail to resolve rather than silently addressing index 0.
    REQUIRE_FALSE(helix::parse_indexed_name("toggle[", name, index));
    REQUIRE_FALSE(helix::parse_indexed_name("toggle[]", name, index));
    REQUIRE_FALSE(helix::parse_indexed_name("toggle[x]", name, index));
    REQUIRE_FALSE(helix::parse_indexed_name("toggle[-1]", name, index));
    REQUIRE_FALSE(helix::parse_indexed_name("toggle[1", name, index));
    REQUIRE_FALSE(helix::parse_indexed_name("[1]", name, index)); // no name
    REQUIRE_FALSE(helix::parse_indexed_name("", name, index));
}

// --- readable paths: emit and resolve -----------------------------------
//
// The locator `ls` prints is the only thing a human retypes, so it emits names
// rather than child indices. These pin both halves: what path_of() writes, and
// that resolve_path() reads its own output back to the same widget.

namespace {

// A settings-page shape: named scaffolding, two same-named toggles under one
// parent (the case an index suffix exists for), and an unnamed container that
// has to fall back to a numeric segment.
struct PathTree {
    lv_obj_t* main_content;
    lv_obj_t* settings_list;
    lv_obj_t* row_theme;
    lv_obj_t* theme_toggle;
    lv_obj_t* toggle_a;
    lv_obj_t* toggle_b;
    lv_obj_t* unnamed_box;
    lv_obj_t* deep_label;
};

PathTree build_path_tree(lv_obj_t* screen) {
    PathTree t{};
    t.main_content = lv_obj_create(screen);
    lv_obj_set_name(t.main_content, "main_content");

    t.settings_list = lv_obj_create(t.main_content);
    lv_obj_set_name(t.settings_list, "settings_list");

    t.row_theme = lv_obj_create(t.settings_list);
    lv_obj_set_name(t.row_theme, "row_theme");
    t.theme_toggle = lv_switch_create(t.row_theme);
    lv_obj_set_name(t.theme_toggle, "toggle");

    // Siblings sharing a name: LVGL only auto-indexes names ending in '#', so
    // both of these resolve to "toggle" and need an ordinal to tell apart.
    t.toggle_a = lv_switch_create(t.settings_list);
    lv_obj_set_name(t.toggle_a, "toggle");
    t.toggle_b = lv_switch_create(t.settings_list);
    lv_obj_set_name(t.toggle_b, "toggle");

    t.unnamed_box = lv_obj_create(t.settings_list); // deliberately nameless
    t.deep_label = lv_label_create(t.unnamed_box);
    lv_obj_set_name(t.deep_label, "deep_label");
    return t;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: unique names are emitted as words", "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // The whole point: readable, and no child index in sight.
    REQUIRE(helix::path_of(t.settings_list) == "s/main_content/settings_list");
    REQUIRE(helix::path_of(t.row_theme) == "s/main_content/settings_list/row_theme");

    // Unique among ITS siblings (it is row_theme's only child) — so no ordinal,
    // even though other widgets elsewhere on screen are also called "toggle".
    REQUIRE(helix::path_of(t.theme_toggle) == "s/main_content/settings_list/row_theme/toggle");
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: same-named siblings get an ordinal", "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // theme_toggle is not a sibling of these two, so the ordinal counts only
    // the same-named children of settings_list, in child order.
    REQUIRE(helix::path_of(t.toggle_a) == "s/main_content/settings_list/toggle[0]");
    REQUIRE(helix::path_of(t.toggle_b) == "s/main_content/settings_list/toggle[1]");
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: unnamed widgets fall back to an index",
                 "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // unnamed_box is settings_list's 4th child (row_theme, toggle, toggle, box).
    REQUIRE(lv_obj_get_index(t.unnamed_box) == 3);
    REQUIRE(helix::path_of(t.deep_label) == "s/main_content/settings_list/3/deep_label");
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: every emitted path resolves back", "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // Round-trip is the real contract: whatever `ls` prints must be retypeable.
    for (lv_obj_t* w : {t.main_content, t.settings_list, t.row_theme, t.theme_toggle, t.toggle_a,
                        t.toggle_b, t.unnamed_box, t.deep_label}) {
        CAPTURE(helix::path_of(w));
        REQUIRE(helix::resolve_path(helix::path_of(w)) == w);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: numeric locators still resolve", "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // Back-compat. path_of() no longer emits this shape, but anything holding
    // an older locator — a saved script, a stale terminal buffer — must keep
    // working, so resolve_path still walks pure child indices.
    REQUIRE(helix::resolve_path("s/0/0/0") == t.row_theme);
    REQUIRE(helix::resolve_path("s/0/0/3/0") == t.deep_label);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: a name survives sibling insertion, an index does not",
                 "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    const std::string by_name = helix::path_of(t.row_theme);
    const std::string by_index = "s/0/0/0";
    REQUIRE(helix::resolve_path(by_name) == t.row_theme);
    REQUIRE(helix::resolve_path(by_index) == t.row_theme);

    // A new row lands at the front, shifting every later child index by one.
    // This is the reason the readable form is also the stable one.
    lv_obj_t* inserted = lv_obj_create(t.settings_list);
    lv_obj_set_name(inserted, "row_new");
    lv_obj_move_to_index(inserted, 0);

    REQUIRE(helix::resolve_path(by_name) == t.row_theme); // still the same widget
    REQUIRE(helix::resolve_path(by_index) == inserted);   // now points elsewhere
    REQUIRE(helix::resolve_path(by_index) != t.row_theme);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: relative locators resolve against a base",
                 "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // What `cd settings_list` buys: short targets from where you are.
    REQUIRE(helix::resolve_path("row_theme", t.settings_list) == t.row_theme);
    REQUIRE(helix::resolve_path("row_theme/toggle", t.settings_list) == t.theme_toggle);
    REQUIRE(helix::resolve_path("toggle[1]", t.settings_list) == t.toggle_b);

    // An absolute locator ignores the base entirely, so a full path pasted from
    // `ls` behaves the same wherever you happen to be cd'd.
    REQUIRE(helix::resolve_path("s/main_content/settings_list/row_theme", t.row_theme) ==
            t.row_theme);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: an ambiguous name reports candidates",
                 "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());

    // "toggle" alone under settings_list matches two children. Resolving to the
    // first would click a switch the caller never addressed — the same failure
    // the #1179 descent bound exists to prevent — so it resolves to nothing and
    // hands back both candidates for the error message.
    std::vector<lv_obj_t*> ambiguous;
    REQUIRE(helix::resolve_path("toggle", t.settings_list, &ambiguous) == nullptr);
    REQUIRE(ambiguous.size() == 2);
    REQUIRE(ambiguous[0] == t.toggle_a);
    REQUIRE(ambiguous[1] == t.toggle_b);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl path: unresolvable segments yield nothing",
                 "[remote][ctl]") {
    PathTree t = build_path_tree(lv_screen_active());
    (void)t;

    REQUIRE(helix::resolve_path("s/main_content/nope") == nullptr);
    REQUIRE(helix::resolve_path("s/main_content/settings_list/toggle[9]") == nullptr);
    REQUIRE(helix::resolve_path("s/99") == nullptr);
    REQUIRE(helix::resolve_path("") == nullptr);
}

// --- describe scoping -----------------------------------------------------
//
// `ls @s` scopes to lv_screen_active(), which can be a screen carrying no
// name of its own (a demo screen). Resolving the scope root's reported name
// must not assume one exists: lv_obj_get_name_resolved() reads the NULL name
// of an unnamed parentless object, so a screen that never got a name takes
// the app down the moment it is listed.

TEST_CASE_METHOD(LVGLTestFixture, "ctl ls: an unnamed active screen scopes without crashing",
                 "[remote][ctl]") {
    lv_obj_t* previous = lv_screen_active();
    // A child allocates the screen's spec_attr while its own name stays NULL,
    // the exact state `ls @s` meets on a demo screen. The second child stays
    // nameless too: LVGL gives IT a crafted "<class>_#" name, which `resolve`
    // must keep answering.
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_name(lv_obj_create(screen), "probe");
    lv_obj_create(screen); // index 1, deliberately nameless
    lv_screen_load(screen);

    // The handler is only reachable through dispatch, over a real socket; it
    // parks on a future until the UI queue runs its payload, so the queue is
    // drained while the response is polled for, never after.
    const std::string sock_path =
        "/tmp/helix-test-ls-unnamed-" + std::to_string(::getpid()) + ".sock";
    ::unlink(sock_path.c_str());
    helix::RemoteConfig config;
    config.socket_path = sock_path;
    helix::RemoteControlServer server;
    REQUIRE(server.start(config));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    REQUIRE(sock_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // One newline-framed JSON-RPC round trip, draining the UI queue while the
    // response is polled for (the handler parks on a future the drain
    // resolves). Empty string on any transport failure.
    auto rpc_roundtrip = [&](int id, const std::string& method, const std::string& params) {
        const std::string request = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method +
                                    "\",\"params\":" + params + ",\"id\":" + std::to_string(id) +
                                    "}\n";
        REQUIRE(send(fd, request.data(), request.size(), 0) ==
                static_cast<ssize_t>(request.size()));

        std::string response;
        char buf[4096];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (response.find('\n') == std::string::npos) {
            struct pollfd pfd {
                fd, POLLIN, 0
            };
            const int ready = poll(&pfd, 1, 20);
            if (ready > 0) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                response.append(buf, static_cast<size_t>(n));
            } else if (ready < 0) {
                break;
            }
            helix::ui::UpdateQueue::instance().drain();
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
        }
        // A queued follow-up from the handler lands before the next request.
        helix::ui::UpdateQueue::instance().drain();
        if (response.find('\n') == std::string::npos) {
            return std::string();
        }
        response.erase(response.find('\n'));
        return response;
    };

    // `ls @s`: the scope root reports no name, its named child is listed, and
    // the nameless screen does not take the app down.
    const std::string ls = rpc_roundtrip(1, "describe_screen", R"({"path":"s"})");
    REQUIRE_FALSE(ls.empty());
    const nlohmann::json ls_rpc = nlohmann::json::parse(ls);
    REQUIRE(ls_rpc.contains("result"));
    const nlohmann::json& result = ls_rpc["result"];
    REQUIRE(result.value("scope", "") == "s");
    REQUIRE(result.contains("widgets"));
    REQUIRE(result["widgets"].size() == 2); // the scope root, then its named child
    REQUIRE(result["widgets"][0].value("name", "") == "");
    REQUIRE(result["widgets"][1].value("path", "") == "s/probe");

    // `resolve` on the nameless CHILD still answers LVGL's crafted
    // "<class>_<index>" name, which stays addressable.
    const std::string res = rpc_roundtrip(2, "resolve", R"({"path":"s/1"})");
    REQUIRE_FALSE(res.empty());
    const nlohmann::json res_rpc = nlohmann::json::parse(res);
    REQUIRE(res_rpc.contains("result"));
    const std::string crafted = res_rpc["result"].value("name", "");
    REQUIRE_FALSE(crafted.empty());
    REQUIRE(crafted.rfind("lv_obj_", 0) == 0);

    close(fd);
    server.stop();
    ::unlink(sock_path.c_str());

    lv_screen_load(previous);
    lv_obj_delete(screen);
}

// --- topmost-visible name resolution ------------------------------------
//
// resolve_widget() is file-static in remote_control_server.cpp, so this pins
// the LVGL-level invariant it depends on: lv_obj_find_by_name() returns the
// FIRST depth-first match, which on a stacked screen is the bottom overlay.
// If LVGL ever changed that, the ctl fix would be silently redundant — and if
// someone "simplifies" resolve_widget back to lv_obj_find_by_name, this test
// documents exactly why that regresses.

TEST_CASE_METHOD(LVGLTestFixture, "lv_obj_find_by_name returns the bottom-most stacked match",
                 "[remote][ctl]") {
    lv_obj_t* screen = lv_screen_active();

    // Two overlays stacked on the screen, each containing a button with the
    // same name — the exact shape that broke `ctl click action_button_2`.
    lv_obj_t* lower = lv_obj_create(screen);
    lv_obj_t* lower_btn = lv_obj_create(lower);
    lv_obj_set_name(lower_btn, "action_button_2");

    lv_obj_t* upper = lv_obj_create(screen);
    lv_obj_t* upper_btn = lv_obj_create(upper);
    lv_obj_set_name(upper_btn, "action_button_2");

    REQUIRE(lv_obj_get_index(upper) > lv_obj_get_index(lower));

    // The naive lookup finds the one the user cannot see.
    lv_obj_t* naive = lv_obj_find_by_name(screen, "action_button_2");
    REQUIRE(naive == lower_btn);
    REQUIRE(naive != upper_btn);
}

TEST_CASE_METHOD(LVGLTestFixture, "hidden subtrees still contain findable names", "[remote][ctl]") {
    lv_obj_t* screen = lv_screen_active();

    // A hidden overlay's children are still reachable by name — which is why
    // resolution has to filter on LV_OBJ_FLAG_HIDDEN rather than trust the
    // lookup. Clicking one of these is always a no-op.
    lv_obj_t* hidden = lv_obj_create(screen);
    lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* buried = lv_obj_create(hidden);
    lv_obj_set_name(buried, "btn_only_in_hidden");

    REQUIRE(lv_obj_has_flag(hidden, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_find_by_name(screen, "btn_only_in_hidden") == buried);
}

// --- bounded container-to-control descent (#1179) -----------------------
//
// resolve_actionable() descends a composite row to the control inside it, so
// `click <row>` behaves like tapping the switch. Unbounded, that same descent
// walks a context-menu backdrop's entire subtree and resolves onto whatever
// control the menu contains — dispatching its event and, on a live printer,
// sending G-code from a click whose only intent was to dismiss the menu.

namespace {

void noop_event_cb(lv_event_t*) {}

// The ams_context_menu shape: a full-screen backdrop with a dismiss handler,
// wrapping a card that holds one visible dropdown. The second dropdown row is
// hidden (no toolchanger), which is what leaves exactly one candidate and made
// the descent look unambiguous.
struct ContextMenuTree {
    lv_obj_t* backdrop;
    lv_obj_t* card;
    lv_obj_t* visible_dropdown;
    lv_obj_t* hidden_dropdown;
};

ContextMenuTree build_context_menu(lv_obj_t* parent) {
    ContextMenuTree t{};
    t.backdrop = lv_obj_create(parent);
    lv_obj_add_flag(t.backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t.backdrop, noop_event_cb, LV_EVENT_CLICKED, nullptr);

    t.card = lv_obj_create(t.backdrop);
    lv_obj_add_flag(t.card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hidden_row = lv_obj_create(t.card);
    lv_obj_add_flag(hidden_row, LV_OBJ_FLAG_HIDDEN);
    t.hidden_dropdown = lv_dropdown_create(hidden_row);

    lv_obj_t* visible_row = lv_obj_create(t.card);
    t.visible_dropdown = lv_dropdown_create(visible_row);
    return t;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ctl: a backdrop click never resolves into the menu it covers",
                 "[remote][ctl][1179]") {
    ContextMenuTree t = build_context_menu(lv_screen_active());

    lv_obj_t* descended = nullptr;
    std::vector<lv_obj_t*> ambiguous;
    lv_obj_t* resolved = helix::resolve_actionable(t.backdrop, &descended, &ambiguous);

    // The backdrop dismisses; that IS the action. Resolving anywhere else
    // fires an unrelated widget's handler.
    REQUIRE(resolved == t.backdrop);
    REQUIRE(descended == nullptr);
    REQUIRE(resolved != t.visible_dropdown);
    REQUIRE(ambiguous.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl: descent stops at a nested click target",
                 "[remote][ctl][1179]") {
    // Same tree minus the backdrop's own handler: the card below it is still a
    // click target of its own, so the search must not tunnel through it even
    // though nothing above it claims the click.
    lv_obj_t* backdrop = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = lv_obj_create(backdrop);
    lv_obj_add_event_cb(card, noop_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* buried = lv_dropdown_create(card);

    lv_obj_t* descended = nullptr;
    lv_obj_t* resolved = helix::resolve_actionable(backdrop, &descended, nullptr);

    REQUIRE(resolved == backdrop);
    REQUIRE(descended == nullptr);
    REQUIRE(resolved != buried);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl: a composite row still descends to its control",
                 "[remote][ctl][1179]") {
    // The behaviour the descent exists for, and the reason the bound is a
    // handler test rather than a depth or geometry cap: a settings row that
    // does nothing on its own must still reach the switch nested inside it,
    // through as much non-interactive scaffolding as the layout uses.
    lv_obj_t* row = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE); // clickable, but no handler
    lv_obj_t* inner = lv_obj_create(row);
    lv_obj_t* deeper = lv_obj_create(inner);
    lv_obj_t* sw = lv_switch_create(deeper);

    lv_obj_t* descended = nullptr;
    std::vector<lv_obj_t*> ambiguous;
    lv_obj_t* resolved = helix::resolve_actionable(row, &descended, &ambiguous);

    REQUIRE(resolved == sw);
    REQUIRE(descended == sw);
    REQUIRE(ambiguous.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl: a value control is returned as itself",
                 "[remote][ctl][1179]") {
    // Addressing the control directly must be unaffected — including a
    // dropdown, whose own internals must never be mistaken for a nested
    // target now that the search stops at value controls.
    lv_obj_t* dd = lv_dropdown_create(lv_screen_active());

    lv_obj_t* descended = nullptr;
    lv_obj_t* resolved = helix::resolve_actionable(dd, &descended, nullptr);

    REQUIRE(resolved == dd);
    REQUIRE(descended == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl: several candidates are reported, not guessed between",
                 "[remote][ctl][1179]") {
    // Pre-existing contract, pinned because the new bound changes what the
    // search reaches: two visible controls under a handler-less container are
    // still ambiguous rather than silently resolved to the first.
    lv_obj_t* box = lv_obj_create(lv_screen_active());
    lv_obj_t* a = lv_switch_create(box);
    lv_obj_t* b = lv_slider_create(box);

    lv_obj_t* descended = nullptr;
    std::vector<lv_obj_t*> ambiguous;
    lv_obj_t* resolved = helix::resolve_actionable(box, &descended, &ambiguous);

    REQUIRE(resolved == box);
    REQUIRE(descended == nullptr);
    REQUIRE(ambiguous.size() == 2);
    REQUIRE(ambiguous[0] == a);
    REQUIRE(ambiguous[1] == b);
}

// --- reachability -------------------------------------------------------
//
// lv_obj_send_event(o, LV_EVENT_CLICKED) reaches any object at all, so a
// synthetic click reports success on widgets no finger could activate. That is
// not a theoretical gap: it produced a green "tap opens the install modal"
// against a card carrying LV_STATE_DISABLED, which on a real panel did nothing.

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: a reachable widget has no blocker", "[remote][ctl]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    CHECK(helix::click_blocker(btn) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: every reason a tap would miss", "[remote][ctl]") {
    SECTION("null is reported, not dereferenced") {
        CHECK(std::string(helix::click_blocker(nullptr)) == "not found");
    }

    SECTION("a plain object is not a click target") {
        lv_obj_t* obj = lv_obj_create(lv_screen_active());
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        CHECK(std::string(helix::click_blocker(obj)) == "not clickable");
    }

    SECTION("disabled is what LVGL's indev refuses") {
        lv_obj_t* btn = lv_button_create(lv_screen_active());
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        CHECK(std::string(helix::click_blocker(btn)) == "disabled");
    }

    SECTION("hidden is inherited from any ancestor") {
        // The child is visible and clickable in its own right; nothing is drawn
        // or hit-tested inside a hidden parent regardless.
        lv_obj_t* parent = lv_obj_create(lv_screen_active());
        lv_obj_t* btn = lv_button_create(parent);
        CHECK(helix::click_blocker(btn) == nullptr);
        lv_obj_add_flag(parent, LV_OBJ_FLAG_HIDDEN);
        CHECK(std::string(helix::click_blocker(btn)) == "hidden");
    }

    SECTION("a disabled parent does not block an enabled child") {
        // lv_indev.c checks the state of the hit object only, so this must not
        // read as blocked - it is how a help affordance stays live on a card
        // that is itself greyed out.
        lv_obj_t* parent = lv_obj_create(lv_screen_active());
        lv_obj_add_state(parent, LV_STATE_DISABLED);
        lv_obj_t* btn = lv_button_create(parent);
        CHECK(helix::click_blocker(btn) == nullptr);
    }

    SECTION("hidden outranks the other two, matching the order a finger hits them") {
        lv_obj_t* obj = lv_obj_create(lv_screen_active());
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        CHECK(std::string(helix::click_blocker(obj)) == "hidden");
    }
}

// --- ctl click over the socket ------------------------------------------

namespace {

/// A live RemoteControlServer on a private socket, and a client connection to it.
struct CtlClickRig {
    std::string sock = (std::filesystem::temp_directory_path() /
                        ("helix_ctl_click_" + std::to_string(::getpid()) + ".sock"))
                           .string();
    helix::RemoteControlServer server;
    int fd = -1;
    int next_id = 1;

    CtlClickRig() {
        ::unlink(sock.c_str());
        helix::RemoteConfig config;
        config.socket_path = sock;
        REQUIRE(server.start(config));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        REQUIRE(sock.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    }

    ~CtlClickRig() {
        if (fd >= 0) {
            ::close(fd);
        }
        server.stop();
        ::unlink(sock.c_str());
    }

    CtlClickRig(const CtlClickRig&) = delete;
    CtlClickRig& operator=(const CtlClickRig&) = delete;

    /// `click <name>`.
    nlohmann::json click(const std::string& name) {
        return call("click", "name", name);
    }

    /// One request with one string parameter.
    nlohmann::json call(const std::string& method, const std::string& key,
                        const std::string& value) {
        const nlohmann::json reply = request(method, {{key, value}});
        REQUIRE(reply.contains("result"));
        return reply["result"];
    }

    /// The whole JSON-RPC reply, result or error, to one request. The transport
    /// thread services it through the UI queue, so this thread must drain while
    /// waiting for the reply.
    nlohmann::json request(const std::string& method, const nlohmann::json& params) {
        const nlohmann::json req = {
            {"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", next_id++}};
        const std::string line = req.dump() + "\n";
        REQUIRE(send(fd, line.data(), line.size(), 0) == static_cast<ssize_t>(line.size()));

        std::string response;
        char buf[4096];
        for (int i = 0; i < 50 && response.find('\n') == std::string::npos; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            struct pollfd pfd {
                fd, POLLIN, 0
            };
            if (poll(&pfd, 1, 200) <= 0) {
                continue;
            }
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            response.append(buf, static_cast<size_t>(n));
        }
        REQUIRE(response.find('\n') != std::string::npos);
        response.erase(response.find('\n'));
        const nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
        REQUIRE(!reply.is_discarded());
        return reply;
    }
};

/// Appends the event code it receives to the std::vector<int> in its user data.
void record_code_cb(lv_event_t* e) {
    static_cast<std::vector<int>*>(lv_event_get_user_data(e))
        ->push_back(static_cast<int>(lv_event_get_code(e)));
}

} // namespace

// A checkable button acts on CLICKED (extruder selector, gcode test Travels).
// A `click` that only flips CHECKED leaves every such button dead, so the
// handler must deliver the tap too, in the order a real tap does.
TEST_CASE_METHOD(LVGLTestFixture, "ctl click: a checkable button toggles and still fires its tap",
                 "[remote][ctl]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_name(btn, "ctl_checkable_button");
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

    std::vector<int> codes;
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_SHORT_CLICKED, &codes);
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_CLICKED, &codes);

    CtlClickRig rig;
    const nlohmann::json first = rig.click("ctl_checkable_button");
    CHECK(first.value("toggled_to", -1) == 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_CHECKED));
    CHECK(codes == std::vector<int>{LV_EVENT_SHORT_CLICKED, LV_EVENT_CLICKED});

    const nlohmann::json second = rig.click("ctl_checkable_button");
    CHECK(second.value("toggled_to", -1) == 0);
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_CHECKED));
    CHECK(codes.size() == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: delivers what a quick tap does, in order",
                 "[remote][ctl][tap_hold]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_name(btn, "ctl_tap_button");
    std::vector<int> codes;
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_SHORT_CLICKED, &codes);
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_CLICKED, &codes);

    CtlClickRig rig;
    const nlohmann::json r = rig.click("ctl_tap_button");
    CHECK(codes == std::vector<int>{LV_EVENT_SHORT_CLICKED, LV_EVENT_CLICKED});
    CHECK(r.value("tap_handled", false));
    CHECK_FALSE(r.contains("warning"));
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl navigate: a widget target gets a whole quick tap",
                 "[remote][ctl][tap_hold]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_name(btn, "ctl_navigate_button");
    std::vector<int> codes;
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_SHORT_CLICKED, &codes);
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_CLICKED, &codes);

    CtlClickRig rig;
    const nlohmann::json r = rig.call("navigate", "panel", "ctl_navigate_button");
    CHECK(r.value("kind", std::string()) == "widget");
    CHECK(codes == std::vector<int>{LV_EVENT_SHORT_CLICKED, LV_EVENT_CLICKED});
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: a hold-only widget says a tap does nothing",
                 "[remote][ctl][tap_hold]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_name(btn, "ctl_hold_only");
    std::vector<int> codes;
    lv_obj_add_event_cb(btn, record_code_cb, LV_EVENT_LONG_PRESSED, &codes);

    CtlClickRig rig;
    const nlohmann::json r = rig.click("ctl_hold_only");
    CHECK(r.value("handlers", 0) == 1);
    CHECK_FALSE(r.value("tap_handled", true));
    CHECK(r.value("warning", std::string()).find("long_press") != std::string::npos);
    CHECK(codes.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: a tap handled by a bubbled-to parent counts",
                 "[remote][ctl][tap_hold]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    std::vector<int> codes;
    lv_obj_add_event_cb(parent, record_code_cb, LV_EVENT_SHORT_CLICKED, &codes);
    lv_obj_t* child = lv_button_create(parent);
    lv_obj_set_name(child, "ctl_bubbling_child");
    lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);

    CtlClickRig rig;
    const nlohmann::json r = rig.click("ctl_bubbling_child");
    CHECK(r.value("tap_handled", false));
    CHECK_FALSE(r.contains("warning"));
    CHECK(codes == std::vector<int>{LV_EVENT_SHORT_CLICKED});

    // Without EVENT_BUBBLE the parent's handler never hears the tap.
    lv_obj_remove_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    CHECK_FALSE(rig.click("ctl_bubbling_child").value("tap_handled", true));
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl click: a widget its tap deletes gets nothing more",
                 "[remote][ctl][tap_hold]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    std::vector<int> parent_codes;
    lv_obj_add_event_cb(parent, record_code_cb, LV_EVENT_CLICKED, &parent_codes);
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_name(btn, "ctl_self_deleting");
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* e) { lv_obj_delete(lv_event_get_current_target_obj(e)); },
        LV_EVENT_SHORT_CLICKED, nullptr);

    CtlClickRig rig;
    rig.click("ctl_self_deleting");
    CHECK(lv_obj_get_child_count(parent) == 0);
    CHECK(parent_codes.empty());
}

// --- ctl overflow ------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "ctl overflow: reports content past the box, not content inside it",
                 "[remote][ctl]") {
    auto box = [&](lv_obj_t* parent, const char* name, int32_t w, int32_t h) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_set_name(o, name);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        return o;
    };
    lv_obj_t* root = box(test_screen(), "root", 400, 400);
    lv_obj_t* tight = box(root, "tight", 100, 100);
    box(tight, "wide_child", 104, 50);
    lv_obj_t* roomy = box(root, "roomy", 100, 100);
    box(roomy, "small_child", 90, 90);
    lv_obj_t* hidden = box(root, "hidden", 100, 100);
    box(hidden, "hidden_child", 150, 150);
    lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* dotted = lv_label_create(box(root, "dotted_host", 100, 30));
    lv_obj_set_size(dotted, 40, 20);
    lv_label_set_long_mode(dotted, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(dotted, "a label far too long to fit in forty pixels");
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_update_layout(root);

    const auto found = helix::find_overflow(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].obj == tight);
    CHECK(found[0].right == 4);
    CHECK(found[0].bottom == 0);
}

// --- ctl scroll --------------------------------------------------------------

namespace {

// An overlay shape: a scrolling viewport holding a plain, non-scrolling column
// that is taller than the viewport, with the target row at its far end.
struct ScrollTree {
    lv_obj_t* viewport;
    lv_obj_t* column;
    lv_obj_t* row;
};

ScrollTree build_scroll_tree(lv_obj_t* screen) {
    auto box = [](lv_obj_t* parent, int32_t w, int32_t h) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        return o;
    };
    ScrollTree t{};
    t.viewport = box(screen, 200, 200);
    t.column = box(t.viewport, 200, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t.column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t.column, 0, 0);
    for (int i = 0; i < 9; ++i) {
        box(t.column, 200, 100);
    }
    t.row = box(t.column, 200, 100);
    lv_obj_update_layout(screen);
    return t;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: a row below the fold ends up inside the viewport",
                 "[remote][ctl][scroll]") {
    ScrollTree t = build_scroll_tree(test_screen());
    REQUIRE(helix::view_clipper(t.row) == t.viewport);

    CHECK(helix::scroll_into_view(t.row) == nullptr);

    lv_area_t row, view;
    lv_obj_get_coords(t.row, &row);
    lv_obj_get_coords(t.viewport, &view);
    CHECK(row.y1 >= view.y1);
    CHECK(row.y2 <= view.y2);
    CHECK(lv_obj_get_scroll_y(t.viewport) == 800);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: an ancestor that cannot scroll is reported",
                 "[remote][ctl][scroll]") {
    ScrollTree t = build_scroll_tree(test_screen());
    lv_obj_remove_flag(t.viewport, LV_OBJ_FLAG_SCROLLABLE);

    CHECK(helix::scroll_into_view(t.row) == t.viewport);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ctl scroll: a widget taller than the viewport only has to overlap it",
                 "[remote][ctl][scroll]") {
    ScrollTree t = build_scroll_tree(test_screen());
    lv_obj_set_height(t.row, 300);
    lv_obj_update_layout(test_screen());

    CHECK(helix::scroll_into_view(t.row) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: overflow-visible ancestors do not clip",
                 "[remote][ctl][scroll]") {
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_size(host, 100, 100);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_t* child = lv_obj_create(host);
    lv_obj_set_size(child, 50, 50);
    lv_obj_set_pos(child, 0, 150);
    lv_obj_update_layout(test_screen());
    REQUIRE(helix::view_clipper(child) == host);

    lv_obj_add_flag(host, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    CHECK(helix::view_clipper(child) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: a widget inside a hidden ancestor is not in view",
                 "[remote][ctl][scroll]") {
    ScrollTree t = build_scroll_tree(test_screen());
    lv_obj_add_flag(t.column, LV_OBJ_FLAG_HIDDEN);

    CHECK(helix::scroll_into_view(t.row) == t.column);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ctl scroll: a widget on a screen that is not loaded is not in view",
                 "[remote][ctl][scroll]") {
    lv_obj_t* other = lv_obj_create(nullptr);
    ScrollTree t = build_scroll_tree(other);

    CHECK(helix::scroll_into_view(t.row) == other);
    lv_obj_delete(other);
}

TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: an empty clipping ancestor shows nothing",
                 "[remote][ctl][scroll]") {
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_size(host, 0, 100);
    lv_obj_set_pos(host, 50, 0);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_t* child = lv_obj_create(host);
    lv_obj_set_size(child, 50, 50);
    // Straddling the empty box's edge, so an overlap test alone would pass it.
    lv_obj_set_pos(child, -10, 0);
    lv_obj_update_layout(test_screen());

    CHECK(helix::view_clipper(child) == host);
}

namespace {

/// The error message `scroll` answers for @p obj, empty when it succeeds.
std::string scroll_error(CtlClickRig& rig, lv_obj_t* obj) {
    const nlohmann::json reply = rig.request("scroll", {{"path", helix::path_of(obj)}});
    if (!reply.contains("error")) {
        return {};
    }
    return reply["error"].value("message", std::string());
}

void load_screen_cb(lv_event_t* e) {
    lv_screen_load(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

} // namespace

// The reply names which kind of ancestor keeps the widget out of view, so the
// caller knows whether to unhide something, change screens or scroll it.
TEST_CASE_METHOD(LVGLTestFixture, "ctl scroll: the reply says why a widget stays out of view",
                 "[remote][ctl][scroll]") {
    ScrollTree t = build_scroll_tree(lv_screen_active());
    CtlClickRig rig;

    SECTION("a hidden ancestor") {
        lv_obj_add_flag(t.column, LV_OBJ_FLAG_HIDDEN);
        CHECK_THAT(scroll_error(rig, t.row),
                   Catch::Matchers::EndsWith("is still out of view: hidden by " +
                                             helix::path_of(t.column)));
    }
    SECTION("an ancestor that clips without scrolling") {
        lv_obj_remove_flag(t.viewport, LV_OBJ_FLAG_SCROLLABLE);
        CHECK_THAT(scroll_error(rig, t.row),
                   Catch::Matchers::EndsWith("is still out of view: clipped by " +
                                             helix::path_of(t.viewport)));
    }
    SECTION("a screen that stopped being the loaded one") {
        // Scrolling the viewport loads another screen, so the row's own screen
        // is no longer on the display once it has been scrolled into view.
        lv_obj_t* const home = lv_screen_active();
        lv_obj_t* other = lv_obj_create(nullptr);
        lv_obj_add_event_cb(t.viewport, load_screen_cb, LV_EVENT_SCROLL, other);
        const std::string error = scroll_error(rig, t.row);
        lv_screen_load(home);
        lv_obj_delete(other);
        CHECK_THAT(error, Catch::Matchers::EndsWith("is still out of view: on inactive screen s"));
    }
    SECTION("the loaded screen itself, when it does not scroll") {
        lv_obj_t* const screen = lv_screen_active();
        const bool screen_scrolls = lv_obj_has_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* below = lv_obj_create(screen);
        lv_obj_set_size(below, 50, 50);
        lv_obj_set_pos(below, 0, lv_obj_get_height(screen) + 100);
        lv_obj_update_layout(screen);
        const std::string error = scroll_error(rig, below);
        lv_obj_delete(below);
        if (screen_scrolls) {
            lv_obj_add_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        }
        CHECK_THAT(error, Catch::Matchers::EndsWith("is still out of view: clipped by " +
                                                    helix::path_of(screen)));
    }
    SECTION("nothing in the way") {
        CHECK(scroll_error(rig, t.row).empty());
    }
}
