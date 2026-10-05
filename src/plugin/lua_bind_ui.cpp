// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "lua_bindings.h"
#include "plugin_overlay_host.h"
#include "plugin_xml_policy.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <climits>
#include <memory>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kSubjectMeta[] = "helix.subject";
const char kUiStateKey = 0;
constexpr size_t kMaxString = 1024;
constexpr size_t kMaxSubjects = 128;
constexpr size_t kMaxOpenConfirms = 1;

struct ObserverCtx {
    LuaRuntime* rt;
    int fn_ref;
    lv_observer_t* handle =
        nullptr; ///< set by subject_observe; the closer removes it before retiring the subject
    bool armed =
        false; ///< lv_subject_add_observer reports the current value at once; Lua sees changes only
};

struct SubjectEntry {
    std::string full_name;
    lv_subject_t subject{};
    std::vector<char> buf;
    std::vector<char> prev;
    bool is_string = false;
};

// Everything one runtime registered here. Freed by the runtime's closer.
struct UiState {
    std::vector<std::unique_ptr<SubjectEntry>> subjects;
    std::vector<std::unique_ptr<ObserverCtx>> observers;
    std::unordered_map<std::string, int> handlers; ///< name -> fn ref
    size_t open_confirms = 0;
    /// The dialog behind open_confirms, so the runtime closer can take a plugin's
    /// still-unanswered question off screen. Nulled by every close path.
    lv_obj_t* open_confirm = nullptr;
};

UiState& ui_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    auto* s = static_cast<UiState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

// A process-lifetime list, never destroyed: an object deleted during process teardown still
// finds its subject alive. LVGL deletes a parent before its children, so no root-level delete
// hook can tell when a bound subtree is gone. Every reference a widget may hold to a plugin
// subject is observer-bound (helix-xml bind records and Lua observers alike; the XML policy
// bans the raw-pointer subject_*_event elements), so an empty observer list means nothing
// can reach the subject any more and freeing it is safe.
std::vector<std::unique_ptr<SubjectEntry>>& retired_subjects() {
    static auto* list = new std::vector<std::unique_ptr<SubjectEntry>>();
    return *list;
}

bool unobserved(lv_subject_t& s) {
    return lv_ll_get_head(&s.subs_ll) == nullptr;
}

bool is_valid_local_name(std::string_view n) {
    if (n.empty() || n.size() > 48)
        return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void on_subject_change(lv_observer_t* obs, lv_subject_t* subject) {
    auto* ctx = static_cast<ObserverCtx*>(lv_observer_get_user_data(obs));
    if (!ctx->armed)
        return;
    bool is_string = subject->type == LV_SUBJECT_TYPE_STRING;
    std::string s = is_string ? lv_subject_get_string(subject) : std::string();
    int32_t v = is_string ? 0 : lv_subject_get_int(subject);
    ctx->rt->invoke(ctx->fn_ref, [is_string, s, v](lua_State* co) {
        if (is_string)
            lua_pushlstring(co, s.data(), s.size());
        else
            lua_pushinteger(co, v);
        return 1;
    });
}

SubjectEntry& check_subject(lua_State* L) {
    return **static_cast<SubjectEntry**>(luaL_checkudata(L, 1, kSubjectMeta));
}

int subject_get(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string)
        lua_pushstring(L, lv_subject_get_string(&s.subject));
    else
        lua_pushinteger(L, lv_subject_get_int(&s.subject));
    return 1;
}

int subject_set(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string) {
        size_t len = 0;
        const char* v = luaL_checklstring(L, 2, &len);
        luaL_argcheck(L, len < kMaxString, 2, "string longer than 1023 bytes");
        lv_subject_copy_string(&s.subject, v);
    } else {
        lua_Integer v = luaL_checkinteger(L, 2);
        luaL_argcheck(L, v >= INT32_MIN && v <= INT32_MAX, 2, "integer out of range");
        lv_subject_set_int(&s.subject, static_cast<int32_t>(v));
    }
    return 0;
}

int subject_observe(lua_State* L) {
    auto& s = check_subject(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    if (!rt.add_observer_watch(kMaxObserverWatches))
        return luaL_error(L,
                          "helix.subject observe: at most %d live observers and printer "
                          "watches per plugin",
                          static_cast<int>(kMaxObserverWatches));
    auto& ui = ui_state(L);
    ui.observers.push_back(std::make_unique<ObserverCtx>(ObserverCtx{&rt, rt.ref_value(L, 2)}));
    ObserverCtx* ctx = ui.observers.back().get();
    ctx->handle = lv_subject_add_observer(&s.subject, &on_subject_change, ctx);
    ctx->armed = true;
    return 0;
}

int make_subject(lua_State* L, bool is_string) {
    auto& rt = LuaRuntime::from(L);
    if (ui_state(L).subjects.size() >= kMaxSubjects)
        return luaL_error(L, "helix.subject: at most %d subjects per plugin",
                          static_cast<int>(kMaxSubjects));
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "subject name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    std::string full = plugin_owned_name(rt.plugin_id(), name);
    // Existence probe: a miss is the normal case, so the quiet lookup keeps
    // registration from warning on every new subject.
    if (lv_xml_find_subject(nullptr, full.c_str()))
        return luaL_error(L, "subject '%s' already exists", full.c_str());

    auto entry = std::make_unique<SubjectEntry>();
    entry->full_name = full;
    entry->is_string = is_string;
    if (is_string) {
        size_t len = 0;
        const char* init = luaL_optlstring(L, 2, "", &len);
        if (len >= kMaxString)
            return luaL_error(L, "subject '%s': initial value longer than 1023 bytes",
                              full.c_str());
        entry->buf.assign(kMaxString, 0);
        entry->prev.assign(kMaxString, 0);
        lv_subject_init_string(&entry->subject, entry->buf.data(), entry->prev.data(), kMaxString,
                               init);
    } else {
        lua_Integer init = luaL_optinteger(L, 2, 0);
        luaL_argcheck(L, init >= INT32_MIN && init <= INT32_MAX, 2, "integer out of range");
        lv_subject_init_int(&entry->subject, static_cast<int32_t>(init));
    }
    lv_xml_register_subject(nullptr, full.c_str(), &entry->subject);

    SubjectEntry* raw = entry.get();
    ui_state(L).subjects.push_back(std::move(entry));
    *static_cast<SubjectEntry**>(lua_newuserdatauv(L, sizeof(SubjectEntry*), 0)) = raw;
    luaL_setmetatable(L, kSubjectMeta);
    return 1;
}

int ui_on(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "handler name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& handlers = ui_state(L).handlers;
    if (auto it = handlers.find(name); it != handlers.end())
        rt.unref(it->second);
    handlers[name] = rt.ref_value(L, 2);
    return 0;
}

int ui_toast(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    static const char* const kNames[] = {"info", "success", "warning", "error", nullptr};
    static const ToastSeverity kSeverity[] = {ToastSeverity::INFO, ToastSeverity::SUCCESS,
                                              ToastSeverity::WARNING, ToastSeverity::ERROR};
    int i = luaL_checkoption(L, 2, "info", kNames);
    ToastManager::instance().show(kSeverity[i], msg);
    return 0;
}

int ui_confirm(lua_State* L) {
    if (context(L).unloading)
        return luaL_error(L, "helix.ui.confirm cannot open during on_unload");
    auto& rt = LuaRuntime::from(L);
    std::string title = luaL_checkstring(L, 1);
    std::string msg = luaL_checkstring(L, 2);
    ModalSeverity severity = ModalSeverity::Info;
    std::string confirm_text = "OK";
    int confirm_ref = LUA_NOREF;
    int cancel_ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        static const char* const kNames[] = {"info", "warning", "error", nullptr};
        static const ModalSeverity kSeverity[] = {ModalSeverity::Info, ModalSeverity::Warning,
                                                  ModalSeverity::Error};
        lua_getfield(L, 3, "severity");
        if (!lua_isnil(L, -1))
            severity = kSeverity[luaL_checkoption(L, -1, nullptr, kNames)];
        lua_pop(L, 1);
        lua_getfield(L, 3, "confirm_text");
        if (lua_isstring(L, -1))
            confirm_text = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_confirm");
        if (lua_isfunction(L, -1))
            confirm_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_cancel");
        if (lua_isfunction(L, -1))
            cancel_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
    }
    LuaRuntime* rtp = &rt;
    auto& ui = ui_state(L);
    if (ui.open_confirms >= kMaxOpenConfirms)
        return luaL_error(L, "helix.ui.confirm: at most %d open dialog per plugin",
                          static_cast<int>(kMaxOpenConfirms));
    ++ui.open_confirms;
    auto run = [rtp](int ref) {
        if (ref != LUA_NOREF)
            rtp->invoke(ref);
    };
    // The dialog's three close paths (confirm, cancel, dismissal) each release the slot
    // exactly once; the shared flag makes a double close a no-op.
    UiState* ui_ptr = &ui;
    auto release = [ui_ptr, open = std::make_shared<std::atomic<bool>>(true)] {
        if (open->exchange(false)) {
            ui_ptr->open_confirm = nullptr;
            --ui_ptr->open_confirms;
        }
    };
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [run, cancel_ref, release] {
        release();
        run(cancel_ref);
    };
    opts.on_dismiss = opts.on_cancel;
    opts.owner_token = rt.token();
    lv_obj_t* dialog = helix::ui::modal_confirm(
        title.c_str(), msg.c_str(), severity, confirm_text.c_str(),
        [run, confirm_ref, release] {
            release();
            run(confirm_ref);
        },
        opts);
    if (dialog)
        ui.open_confirm = dialog;
    else // never shown, so no close path will release the slot
        release();
    return 0;
}

int overlay_handle_close(lua_State* L) {
    auto& ctx = context(L);
    if (ctx.ui)
        ctx.ui->close(static_cast<int>(lua_tointeger(L, lua_upvalueindex(1))));
    return 0;
}

int ui_overlay(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    if (!context(L).ui)
        return luaL_error(L, "helix.ui.overlay is not available here");
    if (context(L).unloading)
        return luaL_error(L, "helix.ui.overlay cannot open during on_unload");
    std::string component = luaL_checkstring(L, 1);
    if (!is_owned_name(rt.plugin_id(), component))
        return luaL_error(L, "helix.ui.overlay: component '%s' is not owned by this plugin",
                          component.c_str());

    int on_close_ref = LUA_NOREF;
    PluginUi::Attrs attrs;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        // Every string key but on_close is an attribute for lv_xml_create; anything
        // else is a mistake Lua should hear about now rather than a widget
        // silently ignoring.
        std::string bad_value_key;
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && std::string(lua_tostring(L, -2)) != "on_close") {
                if (lua_type(L, -1) != LUA_TSTRING)
                    bad_value_key = lua_tostring(L, -2);
                else
                    attrs.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
        if (!bad_value_key.empty())
            return luaL_error(L, "helix.ui.overlay: attribute '%s' must be a string",
                              bad_value_key.c_str());
        for (const auto& [name, value] : attrs) {
            if (auto why = check_plugin_attr(rt.plugin_id(), name, value))
                return luaL_error(L, "helix.ui.overlay: attribute '%s': %s", name.c_str(),
                                  why->c_str());
        }
        // The ref is taken only once every error path is past: luaL_error unwinds
        // without running anything that could release it.
        lua_getfield(L, 2, "on_close");
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return luaL_error(L, "helix.ui.overlay: on_close must be a function");
            }
            on_close_ref = rt.ref_value(L, -1);
        }
        lua_pop(L, 1);
    }

    LuaRuntime* rtp = &rt;
    auto& ui = *context(L).ui;
    size_t opens_before = ui.open_count ? ui.open_count(rt.plugin_id()) : 0;
    int handle = ui.open(
        component,
        [rtp, on_close_ref, token = rt.token()] {
            if (on_close_ref != LUA_NOREF && !token.expired()) {
                rtp->invoke(on_close_ref);
                rtp->unref(on_close_ref);
            }
        },
        attrs);
    // A failed or no-op open (the component is already showing) never runs this
    // call's on_close, so its ref is released here; the showing overlay keeps
    // the callback it was opened with.
    if (on_close_ref != LUA_NOREF &&
        (handle == 0 || (ui.open_count && ui.open_count(rt.plugin_id()) == opens_before)))
        rt.unref(on_close_ref);
    if (handle == 0) {
        return luaL_error(L, "helix.ui.overlay: cannot open '%s'", component.c_str());
    }

    lua_newtable(L);
    lua_pushinteger(L, handle);
    lua_pushcclosure(L, &overlay_handle_close, 1);
    lua_setfield(L, -2, "close");
    return 1;
}

} // namespace

PluginEventTarget parse_plugin_event(std::string_view user_data) {
    PluginEventTarget t;
    std::string_view head = user_data;
    std::optional<std::string> arg;
    if (auto colon = user_data.find(':'); colon != std::string_view::npos) {
        head = user_data.substr(0, colon);
        arg = std::string(user_data.substr(colon + 1));
    }
    std::string_view id = owner_of(head);
    if (!is_valid_plugin_id(id) || head.size() <= id.size() + kPluginNameSeparator.size())
        return t;
    t.id = std::string(id);
    t.name = std::string(head.substr(id.size() + kPluginNameSeparator.size()));
    t.arg = std::move(arg);
    return t;
}

bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name,
                         const std::optional<std::string>& arg) {
    auto& handlers = ui_state(rt.state()).handlers;
    auto it = handlers.find(name);
    if (it == handlers.end())
        return false;
    rt.invoke(it->second, [arg](lua_State* co) {
        if (arg)
            lua_pushlstring(co, arg->data(), arg->size());
        else
            lua_pushnil(co);
        return 1;
    });
    return true;
}

// ponytail: retired subjects are freed at the next plugin load or unload rather than the
// moment their last observer goes; a sweep on a timer is the upgrade if a device ever shows
// the list growing.
void sweep_retired_subjects() {
    auto& list = retired_subjects();
    for (auto it = list.begin(); it != list.end();) {
        if (unobserved((*it)->subject)) {
            lv_subject_deinit(&(*it)->subject);
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

size_t retired_subject_count() {
    return retired_subjects().size();
}

void install_ui_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new UiState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    ctx.rt.on_close([state] {
        // The plugin is going away; take its still-unanswered question off screen. A
        // dialog the user already closed cleared the pointer on its close path.
        if (state->open_confirm)
            Modal::hide(state->open_confirm);
        for (auto& o : state->observers) {
            if (o->handle)
                lv_observer_remove(o->handle);
        }
        for (auto& s : state->subjects) {
            // A later registration under the same name replaced the record's pointer, so
            // only a record still pointing at this subject is the plugin's to remove.
            if (lv_xml_find_subject(nullptr, s->full_name.c_str()) == &s->subject)
                lv_xml_unregister_subject(nullptr, s->full_name.c_str());
            if (unobserved(s->subject))
                lv_subject_deinit(&s->subject);
            else
                retired_subjects().push_back(std::move(s));
        }
        delete state;
    });

    static const luaL_Reg methods[] = {{"get", &subject_get},
                                       {"set", &subject_set},
                                       {"observe", &subject_observe},
                                       {nullptr, nullptr}};
    luaL_newmetatable(L, kSubjectMeta);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Lambdas rather than named wrappers: file-scope `subject_int`/`subject_string`
    // collide with helpers in existing test files and trip the test-mirror ratchet.
    static const luaL_Reg subject_fns[] = {
        {"int", [](lua_State* L) { return make_subject(L, false); }},
        {"string", [](lua_State* L) { return make_subject(L, true); }},
        {nullptr, nullptr}};
    static const luaL_Reg ui_fns[] = {{"on", &ui_on},
                                      {"toast", &ui_toast},
                                      {"confirm", &ui_confirm},
                                      {"overlay", &ui_overlay},
                                      {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, subject_fns, 0);
    lua_setfield(L, -2, "subject");
    lua_newtable(L);
    luaL_setfuncs(L, ui_fns, 0);
    lua_setfield(L, -2, "ui");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
