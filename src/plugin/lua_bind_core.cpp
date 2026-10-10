// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_timer_guard.h"

#include "locale_formats.h"
#include "lua_bindings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <lvgl.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace helix::plugin {

namespace {

const char kContextKey = 0;
const char kTimersKey = 0;
const char kI18nKey = 0;
const char kTimerMeta[] = "helix.timer";
constexpr size_t kMaxLiveTimers = 64;

// A live timer or sleep. The TimerRegistry owns every TimerState, so an LVGL timer's user
// data can be a plain pointer: a state outlives the timer that points at it.
struct TimerState {
    LuaRuntime* rt = nullptr;
    uint32_t period_ms = 0;
    int fn_ref = LUA_NOREF;                       ///< timers
    std::unique_ptr<LuaRuntime::Pending> pending; ///< sleeps
    bool repeat = false;
    bool live = true;
    helix::ui::LvglTimerGuard timer;
};

void on_timer(lv_timer_t* timer);

struct TimerRegistry {
    uint64_t next_id = 1;
    std::unordered_map<uint64_t, std::unique_ptr<TimerState>> timers;

    // Returns the id Lua handles use; a handle never holds a pointer, so a pruned state
    // cannot dangle.
    uint64_t add(std::unique_ptr<TimerState> s) {
        for (auto it = timers.begin(); it != timers.end();)
            it = it->second->live ? std::next(it) : timers.erase(it);
        uint64_t id = next_id++;
        timers.emplace(id, std::move(s));
        return id;
    }
};

TimerRegistry& timers_of(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kTimersKey);
    auto* r = static_cast<TimerRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *r;
}

lv_timer_t* arm(TimerState* s) {
    lv_timer_t* t = lv_timer_create(&on_timer, s->period_ms, s);
    lv_timer_set_repeat_count(t, 1);
    return t;
}

void stop(TimerState& s) {
    if (!s.live)
        return;
    s.live = false;
    s.timer.reset();
    if (s.fn_ref != LUA_NOREF) {
        s.rt->unref(s.fn_ref);
        s.fn_ref = LUA_NOREF;
    }
    s.pending.reset();
}

void on_timer(lv_timer_t* timer) {
    auto* s = static_cast<TimerState*>(lv_timer_get_user_data(timer));
    if (!s->live)
        return;
    s->timer.release(); // LVGL deletes this one-shot after the callback returns
    LuaRuntime* rt = s->rt;
    if (s->pending) {
        auto pending = std::move(s->pending);
        s->live = false;
        pending->resolve({});
        return;
    }
    int ref = s->fn_ref;
    if (s->repeat) {
        s->timer.reset(arm(s)); // re-armed before Lua runs, so the callback can cancel it
        rt->invoke(ref);        // s may be pruned inside; not touched after this
        return;
    }
    s->live = false;
    s->fn_ref = LUA_NOREF;
    rt->invoke(ref);
    rt->unref(ref);
}

int start_timer(lua_State* L, bool repeat) {
    auto& rt = LuaRuntime::from(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 1 && ms <= 24 * 3600 * 1000, 1, "interval must be 1 ms to 24 h");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Sleeps share the registry but not this limit: they hold no Lua reference and finish
    // on their own, so only armed timers count.
    size_t live = 0;
    for (const auto& [id, s] : timers_of(L).timers)
        live += s->live && !s->pending ? 1 : 0;
    if (live >= kMaxLiveTimers)
        return luaL_error(L, "helix.timer: at most %d live timers per plugin",
                          static_cast<int>(kMaxLiveTimers));

    auto s = std::make_unique<TimerState>();
    s->rt = &rt;
    s->period_ms = static_cast<uint32_t>(ms);
    s->repeat = repeat;
    s->fn_ref = rt.ref_value(L, 2);
    s->timer.reset(arm(s.get()));
    uint64_t id = timers_of(L).add(std::move(s));

    *static_cast<uint64_t*>(lua_newuserdatauv(L, sizeof(uint64_t), 0)) = id;
    luaL_setmetatable(L, kTimerMeta);
    return 1;
}

int timer_after(lua_State* L) {
    return start_timer(L, false);
}

int timer_every(lua_State* L) {
    return start_timer(L, true);
}

int timer_cancel(lua_State* L) {
    uint64_t id = *static_cast<uint64_t*>(luaL_checkudata(L, 1, kTimerMeta));
    auto& reg = timers_of(L);
    if (auto it = reg.timers.find(id); it != reg.timers.end())
        stop(*it->second);
    return 0;
}

int sleep_ms(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= 24 * 3600 * 1000, 1, "duration must be 0 ms to 24 h");
    auto& reg = timers_of(L);
    return rt.await_async(L, [&rt, &reg, ms](LuaRuntime::Pending p) {
        auto s = std::make_unique<TimerState>();
        s->rt = &rt;
        s->period_ms = static_cast<uint32_t>(std::max<lua_Integer>(ms, 1));
        s->pending = std::make_unique<LuaRuntime::Pending>(p);
        s->timer.reset(arm(s.get()));
        reg.add(std::move(s));
    });
}

json to_json_impl(lua_State* L, int index, int depth, std::unordered_set<const void*>& seen) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return static_cast<bool>(lua_toboolean(L, index));
    case LUA_TNUMBER:
        if (lua_isinteger(L, index))
            return static_cast<int64_t>(lua_tointeger(L, index));
        return lua_tonumber(L, index);
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, index, &len);
        return std::string(s, len);
    }
    case LUA_TTABLE: {
        if (depth > 32)
            luaL_error(L, "json: table nested deeper than 32");
        const void* id = lua_topointer(L, index);
        if (!seen.insert(id).second)
            luaL_error(L, "json: table contains a cycle");
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, index));
        lua_Integer count = 0;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            ++count;
            lua_pop(L, 1);
        }
        json out;
        if (count == n) { // a sequence, including the empty table
            out = json::array();
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, index, i);
                out.push_back(to_json_impl(L, -1, depth + 1, seen));
                lua_pop(L, 1);
            }
        } else {
            out = json::object();
            lua_pushnil(L);
            while (lua_next(L, index)) {
                if (lua_type(L, -2) != LUA_TSTRING)
                    luaL_error(L, "json: object keys must be strings");
                out[lua_tostring(L, -2)] = to_json_impl(L, -1, depth + 1, seen);
                lua_pop(L, 1);
            }
        }
        seen.erase(id);
        return out;
    }
    default:
        luaL_error(L, "json: cannot encode a %s", luaL_typename(L, index));
        return nullptr;
    }
}

int json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    std::string s = to_json(L, 1).dump();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

// Nesting beyond this depth becomes nil. A hostile payload can nest arbitrarily deep,
// and push_json runs in host pushes (async results, agent events) no protected call
// would catch a raise or a C++ stack overflow from.
constexpr int kJsonPushMaxDepth = 64;

// Every call pushes exactly one value, even when the depth cap or a failed
// lua_checkstack stops the descent, so callers' stack arithmetic holds.
void push_json_impl(lua_State* L, const json& j, int depth) {
    if (depth >= kJsonPushMaxDepth) {
        lua_pushnil(L);
        return;
    }
    switch (j.type()) {
    case json::value_t::boolean:
        lua_pushboolean(L, j.get<bool>());
        break;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        lua_pushinteger(L, j.get<lua_Integer>());
        break;
    case json::value_t::number_float:
        lua_pushnumber(L, j.get<double>());
        break;
    case json::value_t::string: {
        const auto& s = j.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        break;
    }
    case json::value_t::array:
    case json::value_t::object:
        // Descending needs a slot for the child about to be pushed; without that room
        // the subtree becomes one nil in the slot the caller reserved.
        if (!lua_checkstack(L, 3)) {
            lua_pushnil(L);
            return;
        }
        if (j.is_array()) {
            lua_createtable(L, static_cast<int>(j.size()), 0);
            for (size_t i = 0; i < j.size(); ++i) {
                push_json_impl(L, j[i], depth + 1);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
        } else {
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (auto it = j.begin(); it != j.end(); ++it) {
                push_json_impl(L, it.value(), depth + 1);
                lua_setfield(L, -2, it.key().c_str());
            }
        }
        break;
    default: // null, discarded, binary
        lua_pushnil(L);
        break;
    }
}

// Deepest run of containers, counted from 1 at the root. Iterative: the documents it
// measures are deep enough to overflow the C++ stack if walked recursively.
size_t json_depth(const json& j) {
    size_t max_depth = 0;
    std::vector<std::pair<const json*, size_t>> pending{{&j, 1}};
    while (!pending.empty()) {
        auto [node, depth] = pending.back();
        pending.pop_back();
        if (!node->is_structured())
            continue;
        max_depth = std::max(max_depth, depth);
        for (const auto& child : *node)
            pending.push_back({&child, depth + 1});
    }
    return max_depth;
}

int json_decode(lua_State* L) {
    json j = json::parse(luaL_checkstring(L, 1), nullptr, false);
    if (j.is_discarded()) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid JSON");
        return 2;
    }
    if (json_depth(j) > kJsonPushMaxDepth) {
        lua_pushnil(L);
        lua_pushstring(L, "JSON nested too deeply");
        return 2;
    }
    push_json(L, j);
    return 1;
}

template <spdlog::level::level_enum Level> int log_at(lua_State* L) {
    spdlog::log(Level, "[plugin {}] {}", LuaRuntime::from(L).plugin_id(), luaL_checkstring(L, 1));
    return 0;
}

void add_module(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

} // namespace

void push_json(lua_State* L, const json& j) {
    push_json_impl(L, j, 0);
}

json to_json(lua_State* L, int index) {
    std::unordered_set<const void*> seen;
    return to_json_impl(L, index, 0, seen);
}

// ---- helix.i18n ----

// Per-plugin i18n state: on_change handler refs plus one language listener
// that fans each change out to them. t() needs no state: it resolves through
// LVGL's global table, where the plugin's own pack, the app's packs and the
// identity English key all live in lookup order.
struct I18nState {
    LuaRuntime* rt = nullptr;
    std::vector<int> on_change;
    uint64_t listener = 0;
};

I18nState& i18n_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kI18nKey);
    auto* st = static_cast<I18nState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *st;
}

int i18n_t(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    lua_pushstring(L, lv_tr(key));
    return 1;
}

int i18n_locale(lua_State* L) {
    std::string lang = helix::ui::locale_current_language();
    lua_pushlstring(L, lang.data(), lang.size());
    return 1;
}

int i18n_on_change(lua_State* L) {
    auto& ctx = context(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    I18nState& st = i18n_state(L);
    if (st.listener == 0) {
        st.listener = helix::ui::locale_add_language_listener([&st](const std::string& code) {
            for (int ref : st.on_change) {
                st.rt->invoke(ref, [&code](lua_State* co) {
                    lua_pushlstring(co, code.data(), code.size());
                    return 1;
                });
            }
        });
    }
    st.on_change.push_back(ctx.rt.ref_value(L, 1));
    return 0;
}

PluginContext& context(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kContextKey);
    auto* ctx = static_cast<PluginContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *ctx;
}

void install_core_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    lua_pushlightuserdata(L, &ctx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kContextKey);

    auto* reg = new TimerRegistry;
    lua_pushlightuserdata(L, reg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTimersKey);
    ctx.rt.on_close([reg] {
        for (auto& [id, s] : reg->timers)
            stop(*s);
        delete reg;
    });

    auto* i18n = new I18nState{&ctx.rt};
    lua_pushlightuserdata(L, i18n);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kI18nKey);
    ctx.rt.on_close([i18n] {
        if (i18n->listener != 0)
            helix::ui::locale_remove_language_listener(i18n->listener);
        delete i18n;
    });

    luaL_newmetatable(L, kTimerMeta);
    lua_newtable(L);
    lua_pushcfunction(L, &timer_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg log_fns[] = {{"debug", &log_at<spdlog::level::debug>},
                                       {"info", &log_at<spdlog::level::info>},
                                       {"warn", &log_at<spdlog::level::warn>},
                                       {"error", &log_at<spdlog::level::err>},
                                       {nullptr, nullptr}};
    static const luaL_Reg json_fns[] = {
        {"encode", &json_encode}, {"decode", &json_decode}, {nullptr, nullptr}};
    static const luaL_Reg i18n_fns[] = {{"t", &i18n_t},
                                        {"locale", &i18n_locale},
                                        {"on_change", &i18n_on_change},
                                        {nullptr, nullptr}};
    static const luaL_Reg timer_fns[] = {
        {"after", &timer_after}, {"every", &timer_every}, {nullptr, nullptr}};
    add_module(L, "i18n", i18n_fns);
    add_module(L, "log", log_fns);
    add_module(L, "json", json_fns);
    add_module(L, "timer", timer_fns);

    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &sleep_ms);
    lua_setfield(L, -2, "sleep");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
