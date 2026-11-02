// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace helix::plugin {

namespace {

RegistryKey g_widget_state_key{"widget_state"};
constexpr int kHookCount = 5;

// The helix.widget handlers one runtime registered, keyed by full widget id. The state
// dies with the Lua state, so the closer deletes it without unrefing: the registry the
// refs live in is going away in the same lua_close.
struct WidgetState {
    std::unordered_map<std::string, std::array<int, kHookCount>> hooks;
};

WidgetState* widget_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_widget_state_key);
    auto* s = static_cast<WidgetState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

int widget_register(lua_State* L) {
    forbid_in_check(L, "helix.widget");
    PluginContext& ctx = context(L);
    std::string local = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::string full = plugin_owned_name(ctx.manifest.id, local);
    bool declared = std::any_of(ctx.manifest.widgets.begin(), ctx.manifest.widgets.end(),
                                [&](const WidgetDecl& d) { return d.id == full; });
    if (!declared)
        return luaL_error(L, "helix.widget: '%s' is not in manifest.json widgets", full.c_str());
    static const char* const kNames[] = {"on_attach", "on_detach", "on_size", "on_activate",
                                         "on_deactivate"};
    auto [it, fresh] = widget_state(L)->hooks.try_emplace(full);
    if (fresh)
        it->second.fill(LUA_NOREF);
    auto& refs = it->second;
    for (int i = 0; i < kHookCount; ++i) {
        if (refs[i] != LUA_NOREF)
            ctx.rt.unref(refs[i]);
        refs[i] = LUA_NOREF;
        lua_getfield(L, 2, kNames[i]);
        if (lua_isfunction(L, -1))
            refs[i] = ctx.rt.ref_value(L, -1);
        else if (!lua_isnil(L, -1))
            return luaL_error(L, "helix.widget: hook '%s' must be a function", kNames[i]);
        lua_pop(L, 1);
    }
    return 0;
}

} // namespace

bool dispatch_widget_hook(LuaRuntime& rt, const std::string& widget_id, WidgetHook hook,
                          const LuaRuntime::PushFn& args) {
    WidgetState* s = widget_state(rt.state());
    if (!s)
        return false;
    auto it = s->hooks.find(widget_id);
    if (it == s->hooks.end())
        return false;
    int ref = it->second[static_cast<size_t>(hook)];
    if (ref == LUA_NOREF)
        return false;
    rt.invoke(ref, args);
    return true;
}

void install_widget_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new WidgetState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_widget_state_key);
    ctx.rt.on_close([state] { delete state; });

    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &widget_register);
    lua_setfield(L, -2, "widget");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
