// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "connection_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lua_bindings.h"
#include "printer_state.h"

namespace helix::plugin {

namespace {

RegistryKey g_printer_state_key;

struct Watch {
    LuaRuntime* rt;
    int fn_ref;
    const PrinterField* field;
    bool armed =
        false; ///< lv_subject_add_observer reports the current value at once; Lua sees changes only
    lv_observer_t* observer = nullptr;
};

// Printer subjects outlive every plugin, so every observer is removed when the runtime closes.
struct PrinterBindState {
    std::vector<std::unique_ptr<Watch>> watches;
};

PrinterBindState& printer_state_of(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_printer_state_key);
    auto* s = static_cast<PrinterBindState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

const PrinterField* find_field(std::string_view name) {
    for (const auto& f : printer_fields()) {
        if (name == f.lua_name)
            return &f;
    }
    return nullptr;
}

// A plain copy of a field's value, so it can cross into a Lua entry after the LVGL notify.
struct FieldValue {
    PrinterValueKind kind;
    int32_t i = 0;
    std::string s = {};
    bool nil = false;
};

// Klipper's state is unknown while Moonraker is away: the subject holds
// whatever it last heard, or its startup default.
bool moonraker_connected() {
    lv_subject_t* conn = lv_xml_get_subject(nullptr, "printer_connection_state");
    return conn && lv_subject_get_int(conn) == static_cast<int>(helix::ConnectionState::CONNECTED);
}

FieldValue read_field(const PrinterField& f, lv_subject_t* subject) {
    FieldValue v{f.kind};
    if (f.kind == PrinterValueKind::String)
        v.s = lv_subject_get_string(subject);
    else
        v.i = lv_subject_get_int(subject);
    if (f.kind == PrinterValueKind::KlippyState)
        v.nil = !moonraker_connected();
    return v;
}

int push_field(lua_State* L, const FieldValue& v) {
    if (v.nil) {
        lua_pushnil(L);
        return 1;
    }
    switch (v.kind) {
    case PrinterValueKind::Bool:
        lua_pushboolean(L, v.i == static_cast<int>(helix::ConnectionState::CONNECTED));
        break;
    case PrinterValueKind::String:
        lua_pushlstring(L, v.s.data(), v.s.size());
        break;
    case PrinterValueKind::Int:
        lua_pushinteger(L, v.i);
        break;
    case PrinterValueKind::DeciDegrees:
        lua_pushnumber(L, v.i / 10.0);
        break;
    case PrinterValueKind::KlippyState:
        lua_pushstring(L, helix::klippy_state_name(static_cast<helix::KlippyState>(v.i)));
        break;
    }
    return 1;
}

void on_printer_change(lv_observer_t* obs, lv_subject_t* subject) {
    auto* w = static_cast<Watch*>(lv_observer_get_user_data(obs));
    if (!w->armed)
        return;
    FieldValue v = read_field(*w->field, subject);
    w->rt->invoke(w->fn_ref, [v](lua_State* co) { return push_field(co, v); });
}

int printer_get(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    const PrinterField* f = find_field(name);
    if (!f)
        return luaL_error(L, "unknown printer field '%s'", name.c_str());
    lv_subject_t* subject = lv_xml_get_subject(nullptr, f->subject);
    if (!subject) {
        lua_pushnil(L);
        return 1;
    }
    return push_field(L, read_field(*f, subject));
}

int printer_watch(lua_State* L) {
    forbid_in_check(L, "helix.printer.watch");
    std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const PrinterField* f = find_field(name);
    if (!f)
        return luaL_error(L, "unknown printer field '%s'", name.c_str());
    lv_subject_t* subject = lv_xml_get_subject(nullptr, f->subject);
    if (!subject)
        return luaL_error(L, "printer field '%s' is not available yet", name.c_str());
    auto& rt = LuaRuntime::from(L);
    if (!rt.add_observer_watch(kMaxObserverWatches))
        return luaL_error(L,
                          "helix.printer.watch: at most %d live observers and printer "
                          "watches per plugin",
                          static_cast<int>(kMaxObserverWatches));
    auto& state = printer_state_of(L);
    state.watches.push_back(std::make_unique<Watch>(Watch{&rt, rt.ref_value(L, 2), f}));
    Watch* w = state.watches.back().get();
    w->observer = lv_subject_add_observer(subject, &on_printer_change, w);
    w->armed = true;
    return 0;
}

} // namespace

const std::vector<PrinterField>& printer_fields() {
    static const std::vector<PrinterField> kFields{
        {"connected", "printer_connection_state", PrinterValueKind::Bool},
        {"klippy_state", "klippy_state", PrinterValueKind::KlippyState},
        {"print_state", "print_state", PrinterValueKind::String},
        {"progress", "print_progress", PrinterValueKind::Int},
        {"filename", "print_filename", PrinterValueKind::String},
        {"extruder_temp", "extruder_temp", PrinterValueKind::DeciDegrees},
        {"extruder_target", "extruder_target", PrinterValueKind::DeciDegrees},
        {"bed_temp", "bed_temp", PrinterValueKind::DeciDegrees},
        {"bed_target", "bed_target", PrinterValueKind::DeciDegrees},
        {"chamber_temp", "chamber_temp", PrinterValueKind::DeciDegrees},
        {"chamber_target", "chamber_effective_target", PrinterValueKind::DeciDegrees},
    };
    return kFields;
}

void install_printer_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new PrinterBindState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_printer_state_key);
    ctx.rt.on_close([state] {
        for (auto& w : state->watches) {
            if (w->observer)
                lv_observer_remove(w->observer);
        }
        delete state;
    });

    static const luaL_Reg fns[] = {
        {"get", &printer_get}, {"watch", &printer_watch}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "printer");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
