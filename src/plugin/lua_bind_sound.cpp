// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include <lvgl.h>
#include <new>

namespace helix::plugin {

namespace {

RegistryKey g_sound_state_key;

// The cues of the app's sound themes a plugin may request. The boot chime and the
// urgent alarm stay the app's own, so a plugin can never sound like a printer fault.
const char* const kCueNames[] = {
    "button_tap",     "toggle_on",       "toggle_off",  "nav_forward", "nav_back",  "dropdown_open",
    "print_complete", "print_cancelled", "error_alert", "error_tone",  "test_beep", nullptr};

// ALARM priority is never offered: it preempts everything and is never interrupted.
const char* const kPriorityNames[] = {"ui", "event", nullptr};
const SoundPriority kPriorities[] = {SoundPriority::UI, SoundPriority::EVENT};
const uint32_t kIntervalsMs[] = {kPluginUiCueIntervalMs, kPluginEventCueIntervalMs};

// When each priority last played, in lv_tick time. Plain data inside a Lua userdata, so
// it goes with the Lua state and needs no closer.
struct SoundState {
    bool played[2] = {false, false};
    uint32_t last_tick[2] = {0, 0};
};

SoundState& sound_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_sound_state_key);
    auto* s = static_cast<SoundState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

int sound_play(lua_State* L) {
    forbid_in_check(L, "helix.sound.play");
    const int cue = luaL_checkoption(L, 1, nullptr, kCueNames);
    const int p = luaL_checkoption(L, 2, "ui", kPriorityNames);
    const auto& play = context(L).backend.play_sound;
    if (!play) {
        lua_pushboolean(L, 0);
        return 1;
    }
    SoundState& s = sound_state(L);
    if (s.played[p] && lv_tick_elaps(s.last_tick[p]) < kIntervalsMs[p]) {
        lua_pushboolean(L, 0);
        return 1;
    }
    s.played[p] = true;
    s.last_tick[p] = lv_tick_get();
    play(kCueNames[cue], kPriorities[p]);
    lua_pushboolean(L, 1);
    return 1;
}

} // namespace

void install_sound_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    new (lua_newuserdatauv(L, sizeof(SoundState), 0)) SoundState;
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_sound_state_key);

    static const luaL_Reg sound_fns[] = {{"play", &sound_play}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, sound_fns, 0);
    lua_setfield(L, -2, "sound");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
