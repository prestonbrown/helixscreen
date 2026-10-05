// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"
#include "plugin_canvas.h"
#include "plugin_manifest.h"
#include "theme_manager.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin {

namespace {

const char kCanvasStateKey = 0;
const char kCanvasMeta[] = "helix.canvas";
constexpr size_t kMaxCanvases = 8;
constexpr int32_t kMaxStroke = 64;
constexpr int32_t kMaxOpaPercent = 100; // opa is authored 0-100; stored as 0-255
constexpr size_t kMaxTextBytes = 256;
// Left uncharged so the luaL_error that reports a refused call has Lua memory
// to raise into; on a completely full heap the error would surface as a
// different message.
constexpr size_t kRaiseRoom = 256;

struct CanvasEntry {
    std::string full;
    DisplayList pending;
    size_t pending_charge = 0;
    size_t committed_charge = 0;
    int size_ref = LUA_NOREF;
    bool size_pending = false;
    // -1 means the current handler has not been told any size yet.
    int32_t delivered_w = -1;
    int32_t delivered_h = -1;
};

struct CanvasState {
    LuaRuntime* rt = nullptr;
    LifetimeToken token;
    // Never erased: handle userdata index into it.
    std::vector<std::unique_ptr<CanvasEntry>> canvases;
};

CanvasState* canvas_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCanvasStateKey);
    auto* s = static_cast<CanvasState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

CanvasEntry* entry_of(CanvasState* s, const std::string& full) {
    for (const auto& e : s->canvases)
        if (e->full == full)
            return e.get();
    return nullptr;
}

CanvasEntry* handle_entry(lua_State* L) {
    CanvasState* s = canvas_state(L);
    uint32_t i = *static_cast<uint32_t*>(luaL_checkudata(L, 1, kCanvasMeta));
    if (i >= s->canvases.size())
        luaL_error(L, "helix.canvas: stale handle");
    return s->canvases[i].get(); // unreachable on failure; luaL_error raises
}

// Runs on the main loop, once per size change the handler has not heard.
void deliver_size(CanvasState* s, const std::string& full) {
    CanvasEntry* e = entry_of(s, full);
    if (!e)
        return;
    e->size_pending = false;
    if (e->size_ref == LUA_NOREF)
        return;
    // Plain locals, not structured bindings: capturing a binding in a lambda is
    // C++20, and cross builds compile this file as C++17.
    const std::pair<int32_t, int32_t> size = canvas_size(full);
    const int32_t w = size.first;
    const int32_t h = size.second;
    if (w <= 0 || h <= 0)
        return;
    if (w == e->delivered_w && h == e->delivered_h)
        return;
    e->delivered_w = w;
    e->delivered_h = h;
    // invoke pushes the function before running it, so a handler that replaces
    // or removes itself mid-call cannot dangle the ref it runs on.
    s->rt->invoke(e->size_ref, [w, h](lua_State* L) {
        lua_pushinteger(L, w);
        lua_pushinteger(L, h);
        return 2;
    });
}

void queue_delivery(CanvasState* s, CanvasEntry* e, const std::string& full) {
    if (e->size_pending)
        return;
    e->size_pending = true;
    s->token.defer("plugin_canvas_size", [s, full] { deliver_size(s, full); });
}

// ---- argument validation --------------------------------------------------

double checked_coord(lua_State* L, lua_Number v, const char* what) {
    if (!std::isfinite(v) || v > kMaxCanvasCoord || v < -kMaxCanvasCoord)
        luaL_error(L, "helix.canvas: coordinates must be finite numbers within +-%d (%s)",
                   static_cast<int>(kMaxCanvasCoord), what);
    return v; // unreachable; luaL_error raises
}

double coord(lua_State* L, int idx, const char* what) {
    int ok = 0;
    lua_Number v = lua_tonumberx(L, idx, &ok);
    if (!ok)
        luaL_error(L, "helix.canvas: coordinates must be finite numbers within +-%d (%s)",
                   static_cast<int>(kMaxCanvasCoord), what);
    return checked_coord(L, v, what); // unreachable on failure; luaL_error raises
}

double checked_radius(lua_State* L, lua_Number v) {
    if (!std::isfinite(v) || v < 0 || v > kMaxCanvasCoord)
        luaL_error(L, "helix.canvas: radius must be 0 to %d", static_cast<int>(kMaxCanvasCoord));
    return v; // unreachable; luaL_error raises
}

double angle(lua_State* L, int idx) {
    int ok = 0;
    lua_Number v = lua_tonumberx(L, idx, &ok);
    if (!ok || !std::isfinite(v))
        luaL_error(L, "helix.canvas: angles must be finite numbers");
    return v; // unreachable; luaL_error raises
}

// An opts table is either absent/nil or holds only `known` keys.
int opts_index(lua_State* L, int idx) {
    return lua_isnoneornil(L, idx) ? 0 : lua_absindex(L, idx);
}

void check_opts(lua_State* L, int idx, std::initializer_list<const char*> known) {
    if (!idx)
        return;
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        // Only a confirmed string key is stringified: lua_tostring on a number
        // key converts it in place and desynchronizes lua_next.
        const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        bool found = false;
        for (const char* name : known)
            if (k && std::strcmp(k, name) == 0) {
                found = true;
                break;
            }
        if (!found)
            luaL_error(L, "helix.canvas: unknown option '%s'", k ? k : luaL_typename(L, -2));
        lua_pop(L, 1); // the value; the key stays for lua_next
    }
}

// Reads string field `name`; false when absent. `out` keeps its prior value.
bool opt_str(lua_State* L, int idx, const char* name, const char** out) {
    if (!idx)
        return false;
    lua_getfield(L, idx, name);
    bool present = !lua_isnil(L, -1);
    if (present)
        *out = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return present;
}

int32_t opt_width(lua_State* L, int idx, const char* name, int32_t fallback) {
    if (!idx)
        return fallback;
    lua_getfield(L, idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fallback;
    }
    int ok = 0;
    lua_Number v = lua_tonumberx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok || !std::isfinite(v) || v < 0 || v > kMaxStroke)
        luaL_error(L, "helix.canvas: %s must be 0 to %d", name, static_cast<int>(kMaxStroke));
    return static_cast<int32_t>(v); // unreachable; luaL_error raises
}

// Reads a named percent field and returns LVGL's 0-255 alpha scale.
uint8_t opt_percent(lua_State* L, int idx, const char* name) {
    if (!idx)
        return LV_OPA_COVER;
    lua_getfield(L, idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return LV_OPA_COVER;
    }
    int ok = 0;
    lua_Number v = lua_tonumberx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok || !std::isfinite(v) || v < 0 || v > kMaxOpaPercent)
        luaL_error(L, "helix.canvas: %s must be 0 to %d", name, static_cast<int>(kMaxOpaPercent));
    return static_cast<uint8_t>(v * LV_OPA_COVER / kMaxOpaPercent + 0.5);
}

// Reads the opa field, a percent, and returns LVGL's 0-255 alpha scale.
uint8_t opt_opa(lua_State* L, int idx) {
    return opt_percent(L, idx, "opa");
}

int32_t opt_radius(lua_State* L, int idx) {
    if (!idx)
        return 0;
    lua_getfield(L, idx, "radius");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    int ok = 0;
    lua_Number v = lua_tonumberx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok)
        luaL_error(L, "helix.canvas: radius must be 0 to %d", static_cast<int>(kMaxCanvasCoord));
    return static_cast<int32_t>(checked_radius(L, v));
}

double radius_arg(lua_State* L, int idx) {
    int ok = 0;
    lua_Number v = lua_tonumberx(L, idx, &ok);
    if (!ok)
        luaL_error(L, "helix.canvas: radius must be 0 to %d", static_cast<int>(kMaxCanvasCoord));
    return checked_radius(L, v); // unreachable on failure; luaL_error raises
}

// ---- staging --------------------------------------------------------------

// One primitive plus the heap data its ranges point into and the token strings
// it names. Everything is validated before staging; staging never raises.
struct Staged {
    CanvasPrim prim;
    std::vector<lv_point_precise_t> points; // polyline
    std::string text;                       // text
    // Resolved token indices; kNoToken for a token the primitive does not use.
    uint8_t color = kNoToken;
    uint8_t border = kNoToken;
    uint8_t font = kNoToken;
    // Tokens this primitive introduces to the list (deduped against the list
    // and each other), appended only after the memory charge succeeds.
    std::vector<std::string> fresh_tokens;
};

// Fills st.color/border/font with the list indices the token strings map to,
// collecting tokens the list does not hold yet into fresh_tokens.
void plan_tokens(DisplayList& pending, const char* color, const char* border, const char* font,
                 Staged& st) {
    const char* names[3] = {color, border, font};
    uint8_t* slots[3] = {&st.color, &st.border, &st.font};
    for (int i = 0; i < 3; ++i) {
        const char* tok = names[i];
        if (!tok)
            continue;
        *slots[i] = kNoToken;
        for (size_t j = 0; j < pending.tokens.size(); ++j)
            if (pending.tokens[j] == tok) {
                *slots[i] = static_cast<uint8_t>(j);
                break;
            }
        if (*slots[i] != kNoToken)
            continue;
        for (size_t j = 0; *slots[i] == kNoToken && j < st.fresh_tokens.size(); ++j)
            if (st.fresh_tokens[j] == tok)
                *slots[i] = static_cast<uint8_t>(pending.tokens.size() + j);
        if (*slots[i] == kNoToken) {
            *slots[i] = static_cast<uint8_t>(pending.tokens.size() + st.fresh_tokens.size());
            st.fresh_tokens.emplace_back(tok);
        }
    }
}

// Appends a fully validated primitive, charging the cap for the exact bytes
// DisplayList::bytes() will grow by. Every raise happens before the charge, so
// a refused call adds nothing.
void add_staged(lua_State* L, CanvasEntry* e, Staged& st, size_t units) {
    if (e->pending.units + units > kMaxCanvasUnits)
        luaL_error(L,
                   "helix.canvas: a list holds at most %d primitives (a polyline counts "
                   "each point)",
                   static_cast<int>(kMaxCanvasUnits));
    if (e->pending.tokens.size() + st.fresh_tokens.size() > kMaxCanvasTokens)
        luaL_error(L, "helix.canvas: at most %d distinct tokens per list",
                   static_cast<int>(kMaxCanvasTokens));

    size_t growth =
        sizeof(CanvasPrim) + st.points.size() * sizeof(lv_point_precise_t) + st.text.size();
    for (const std::string& tok : st.fresh_tokens)
        growth += tok.size() + sizeof(std::string);
    if (e->pending_charge == 0)
        growth += sizeof(DisplayList); // the list's own footprint starts charging here

    LuaRuntime& rt = context(L).rt;
    const size_t room = memory_remaining(rt);
    if (room < kRaiseRoom || growth > room - kRaiseRoom)
        luaL_error(L, "helix.canvas: list would exceed the plugin memory cap");
    rt.reserve_external(growth);

    for (const std::string& tok : st.fresh_tokens)
        e->pending.tokens.push_back(tok);
    st.prim.color = st.color;
    st.prim.border = st.border;
    st.prim.font = st.font;
    // first is the op's offset into the shared buffer, fixed at append time.
    if (st.prim.op == CanvasOp::Polyline)
        st.prim.first = static_cast<uint32_t>(e->pending.points.size());
    else if (st.prim.op == CanvasOp::Text)
        st.prim.first = static_cast<uint32_t>(e->pending.text.size());
    e->pending.prims.push_back(st.prim);
    for (const lv_point_precise_t& p : st.points)
        e->pending.points.push_back(p);
    e->pending.text.append(st.text);
    e->pending.units += units;
    e->pending_charge += growth;
}

void check_color(lua_State* L, const char* tok) {
    if (!theme_manager_has_color(tok))
        luaL_error(L, "helix.canvas: unknown color token '%s'", tok);
}

// ---- methods --------------------------------------------------------------

int m_line(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    Staged st;
    st.prim.op = CanvasOp::Line;
    st.prim.a = coord(L, 2, "x1");
    st.prim.b = coord(L, 3, "y1");
    st.prim.c = coord(L, 4, "x2");
    st.prim.d = coord(L, 5, "y2");
    int opts = opts_index(L, 6);
    check_opts(L, opts, {"color", "width", "opa"});
    const char* color = "text";
    opt_str(L, opts, "color", &color);
    check_color(L, color);
    st.prim.width = opt_width(L, opts, "width", 1);
    st.prim.opa = opt_opa(L, opts);
    plan_tokens(e->pending, color, nullptr, nullptr, st);
    add_staged(L, e, st, 1);
    return 0;
}

int m_polyline(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    if (lua_type(L, 2) != LUA_TTABLE)
        return luaL_error(L, "helix.canvas: polyline needs an even-length array of at least "
                             "2 points");
    const lua_Integer n = lua_rawlen(L, 2);
    if (n < 4 || n % 2 != 0)
        return luaL_error(L, "helix.canvas: polyline needs an even-length array of at least "
                             "2 points");
    Staged st;
    st.prim.op = CanvasOp::Polyline;
    st.points.reserve(static_cast<size_t>(n / 2));
    char what[24];
    for (lua_Integer i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        int ok = 0;
        lua_Number v = lua_tonumberx(L, -1, &ok);
        lua_pop(L, 1);
        snprintf(what, sizeof(what), "point %d", static_cast<int>(i + 1));
        if (!ok) {
            luaL_error(L, "helix.canvas: coordinates must be finite numbers within +-%d (%s)",
                       static_cast<int>(kMaxCanvasCoord), what);
            return 0; // unreachable; luaL_error raises
        }
        v = checked_coord(L, v, what);
        if (i % 2 == 0)
            st.points.push_back({static_cast<lv_value_precise_t>(v), 0});
        else
            st.points.back().y = static_cast<lv_value_precise_t>(v);
    }
    st.prim.count = static_cast<uint32_t>(n / 2);
    int opts = opts_index(L, 3);
    check_opts(L, opts, {"color", "width", "opa", "fill", "fill_opa", "baseline"});
    const char* color = "text";
    opt_str(L, opts, "color", &color);
    check_color(L, color);
    const char* fill = nullptr;
    opt_str(L, opts, "fill", &fill);
    if (fill)
        check_color(L, fill);
    // An area fill runs from the line down to a baseline y; the pair is one
    // option in practice, so neither half is accepted alone.
    lua_Number baseline = 0;
    bool has_baseline = false;
    if (opts) {
        lua_getfield(L, opts, "baseline");
        if (!lua_isnil(L, -1)) {
            int ok = 0;
            baseline = lua_tonumberx(L, -1, &ok);
            if (!ok)
                return luaL_error(L,
                                  "helix.canvas: baseline must be a finite number within "
                                  "+-%d",
                                  static_cast<int>(kMaxCanvasCoord));
            checked_coord(L, baseline, "baseline");
            has_baseline = true;
        }
        lua_pop(L, 1);
    }
    if (fill && !has_baseline)
        return luaL_error(L, "helix.canvas: polyline fill needs a baseline");
    if (has_baseline && !fill)
        return luaL_error(L, "helix.canvas: polyline baseline needs a fill");
    st.prim.a = static_cast<lv_value_precise_t>(baseline);
    st.prim.fill_opa = opt_percent(L, opts, "fill_opa");
    st.prim.width = opt_width(L, opts, "width", 1);
    st.prim.opa = opt_opa(L, opts);
    // The fill token rides in the border slot; the triangles are derived at
    // draw time from the same points, so the fill adds no units.
    plan_tokens(e->pending, color, fill, nullptr, st);
    add_staged(L, e, st, static_cast<size_t>(n / 2));
    return 0;
}

// Shared by rect and circle: fill/border/border_width (+radius for rect).
void parse_shape_opts(lua_State* L, int opts, DisplayList& pending, bool with_radius, Staged& st) {
    check_opts(
        L, opts,
        with_radius
            ? std::initializer_list<const char*>{"fill", "border", "border_width", "radius", "opa"}
            : std::initializer_list<const char*>{"fill", "border", "border_width", "opa"});
    const char* fill = nullptr;
    const char* border = nullptr;
    opt_str(L, opts, "fill", &fill);
    opt_str(L, opts, "border", &border);
    if (!fill && !border)
        luaL_error(L, "helix.canvas: %s needs fill or border", with_radius ? "rect" : "circle");
    if (fill)
        check_color(L, fill);
    if (border)
        check_color(L, border);
    st.prim.width = opt_width(L, opts, "border_width", 1);
    st.prim.opa = opt_opa(L, opts);
    if (with_radius)
        st.prim.radius = opt_radius(L, opts);
    plan_tokens(pending, fill, border, nullptr, st);
}

int m_rect(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    Staged st;
    st.prim.op = CanvasOp::Rect;
    st.prim.a = coord(L, 2, "x");
    st.prim.b = coord(L, 3, "y");
    st.prim.c = coord(L, 4, "w");
    st.prim.d = coord(L, 5, "h");
    int opts = opts_index(L, 6);
    parse_shape_opts(L, opts, e->pending, true, st);
    add_staged(L, e, st, 1);
    return 0;
}

int m_circle(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    Staged st;
    st.prim.op = CanvasOp::Circle;
    st.prim.a = coord(L, 2, "cx");
    st.prim.b = coord(L, 3, "cy");
    st.prim.radius = static_cast<int32_t>(radius_arg(L, 4));
    int opts = opts_index(L, 5);
    parse_shape_opts(L, opts, e->pending, false, st);
    add_staged(L, e, st, 1);
    return 0;
}

int m_arc(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    Staged st;
    st.prim.op = CanvasOp::Arc;
    st.prim.a = coord(L, 2, "cx");
    st.prim.b = coord(L, 3, "cy");
    st.prim.radius = static_cast<int32_t>(radius_arg(L, 4));
    st.prim.c = angle(L, 5);
    st.prim.d = angle(L, 6);
    int opts = opts_index(L, 7);
    check_opts(L, opts, {"color", "width", "opa"});
    const char* color = "text";
    opt_str(L, opts, "color", &color);
    check_color(L, color);
    st.prim.width = opt_width(L, opts, "width", 1);
    st.prim.opa = opt_opa(L, opts);
    plan_tokens(e->pending, color, nullptr, nullptr, st);
    add_staged(L, e, st, 1);
    return 0;
}

int m_text(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    size_t len = 0;
    const char* str = luaL_checklstring(L, 4, &len);
    if (len > kMaxTextBytes)
        return luaL_error(L, "helix.canvas: text is at most %d bytes",
                          static_cast<int>(kMaxTextBytes));
    Staged st;
    st.prim.op = CanvasOp::Text;
    st.prim.a = coord(L, 2, "x");
    st.prim.b = coord(L, 3, "y");
    int opts = opts_index(L, 5);
    check_opts(L, opts, {"font", "color"});
    const char* font = "body";
    const char* color = "text";
    opt_str(L, opts, "font", &font);
    opt_str(L, opts, "color", &color);
    if (!canvas_resolve_font(font))
        return luaL_error(L,
                          "helix.canvas: font token '%s' must be a base token; a size-suffixed "
                          "variant renders as the default font below its tier",
                          font);
    check_color(L, color);
    st.text.assign(str, len);
    st.prim.count = static_cast<uint32_t>(len);
    plan_tokens(e->pending, color, nullptr, font, st);
    add_staged(L, e, st, 1);
    return 0;
}

int m_clear(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    context(L).rt.release_external(e->pending_charge);
    e->pending_charge = 0;
    e->pending = DisplayList{};
    return 0;
}

int m_commit(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    LuaRuntime& rt = context(L).rt;
    auto list = std::make_unique<DisplayList>(std::move(e->pending));
    e->pending = DisplayList{};
    // The charge moves from pending to committed; the replaced committed
    // list's bytes go back to the cap.
    rt.release_external(e->committed_charge);
    e->committed_charge = e->pending_charge;
    e->pending_charge = 0;
    canvas_commit(e->full, std::move(list));
    return 0;
}

int m_size(lua_State* L) {
    auto [w, h] = canvas_size(handle_entry(L)->full);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

int m_on_size(lua_State* L) {
    CanvasEntry* e = handle_entry(L);
    LuaRuntime& rt = context(L).rt;
    // Validate before unref, so a bad argument cannot silence a live handler.
    if (!lua_isnone(L, 2) && !lua_isnil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (e->size_ref != LUA_NOREF) {
        rt.unref(e->size_ref);
        e->size_ref = LUA_NOREF;
    }
    if (lua_isnone(L, 2) || lua_isnil(L, 2))
        return 0;
    e->size_ref = rt.ref_value(L, 2);
    // A new handler has not been told any size; a known one schedules a call.
    e->delivered_w = -1;
    e->delivered_h = -1;
    if (auto [w, h] = canvas_size(e->full); w > 0 && h > 0)
        queue_delivery(canvas_state(L), e, e->full);
    return 0;
}

int canvas_new(lua_State* L) {
    PluginContext& ctx = context(L);
    const char* local = luaL_checkstring(L, 1);
    if (*local == '\0')
        return luaL_error(L, "helix.canvas: name must not be empty");
    std::string full = plugin_owned_name(ctx.manifest.id, local);
    CanvasState* s = canvas_state(L);
    size_t index = s->canvases.size();
    for (size_t i = 0; i < s->canvases.size(); ++i)
        if (s->canvases[i]->full == full) {
            index = i; // the same canvas for repeated calls
            break;
        }
    if (index == s->canvases.size()) {
        if (s->canvases.size() >= kMaxCanvases)
            return luaL_error(L, "helix.canvas: at most %d canvases per plugin",
                              static_cast<int>(kMaxCanvases));
        s->canvases.push_back(std::make_unique<CanvasEntry>());
        index = s->canvases.size() - 1;
        s->canvases[index]->full = full;
        canvas_set_size_listener(full, [s, full](int32_t, int32_t) {
            // Never enters Lua from the size event: coalesce, defer, deliver.
            if (CanvasEntry* cur = entry_of(s, full); cur && cur->size_ref != LUA_NOREF)
                queue_delivery(s, cur, full);
        });
    }
    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) =
        static_cast<uint32_t>(index);
    luaL_setmetatable(L, kCanvasMeta);
    return 1;
}

} // namespace

void install_canvas_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new CanvasState{&ctx.rt, ctx.rt.token(), {}};
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCanvasStateKey);
    ctx.rt.on_close([state] {
        // Cleared from outside any executing handler, so a listener that
        // removed itself is not destroyed under its own feet.
        for (const auto& e : state->canvases) {
            canvas_set_size_listener(e->full, {});
            canvas_commit(e->full, nullptr); // an unloaded plugin's canvases go blank
        }
        delete state;
    });

    luaL_newmetatable(L, kCanvasMeta);
    lua_newtable(L);
    static const luaL_Reg methods[] = {
        {"line", &m_line}, {"polyline", &m_polyline}, {"rect", &m_rect},   {"circle", &m_circle},
        {"arc", &m_arc},   {"text", &m_text},         {"clear", &m_clear}, {"commit", &m_commit},
        {"size", &m_size}, {"on_size", &m_on_size},   {nullptr, nullptr}};
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &canvas_new);
    lua_setfield(L, -2, "canvas");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
