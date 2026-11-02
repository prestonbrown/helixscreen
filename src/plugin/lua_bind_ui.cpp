// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_component_keypad.h"
#include "ui_modal.h"
#include "ui_text_entry_modal.h"
#include "ui_toast_manager.h"

#include "helix/ui/text_metrics.h"
#include "lua_bindings.h"
#include "lvgl.h"
#include "lvgl/src/misc/lv_text_private.h"
#include "plugin_canvas.h"
#include "plugin_overlay_host.h"
#include "plugin_xml_policy.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kSubjectMeta[] = "helix.subject";
const char kUiStateKey = 0;
constexpr size_t kMaxString = 1024;
constexpr size_t kMaxSubjects = 128;
constexpr size_t kMaxOpenConfirms = 1;
constexpr size_t kMaxDialogs = 1;
constexpr size_t kMaxEditorTitle = 47; ///< the keypad's title buffer, less its NUL
constexpr size_t kMaxEditorUnit = 15;
constexpr size_t kMaxEditorMessage = 256;
constexpr double kMaxKeypadValue = 99999;

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

// The one keypad or text editor a plugin has shown or waiting. Shared by the editor's
// callbacks, which do nothing once `done` is set: answered, cancelled, or the plugin is
// unloading.
struct Editor {
    bool done = false;
    bool keypad = false;
    bool shown = false;
    lv_obj_t* dialog = nullptr; ///< the text entry dialog while shown
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
    std::shared_ptr<Editor> editor;
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
    forbid_in_check(L, "subject:set");
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
    forbid_in_check(L, "subject:observe");
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
    forbid_in_check(L, "helix.subject");
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
    forbid_in_check(L, "helix.ui.on");
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
    forbid_in_check(L, "helix.ui.toast");
    const char* msg = luaL_checkstring(L, 1);
    static const char* const kNames[] = {"info", "success", "warning", "error", nullptr};
    static const ToastSeverity kSeverity[] = {ToastSeverity::INFO, ToastSeverity::SUCCESS,
                                              ToastSeverity::WARNING, ToastSeverity::ERROR};
    int i = luaL_checkoption(L, 2, "info", kNames);
    ToastManager::instance().show(kSeverity[i], msg);
    return 0;
}

// Measured as canvas text draws it: the same base-token font and no letter spacing.
int ui_text_width(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    if (len > kMaxString)
        return luaL_error(L, "helix.ui.text_width: text is at most %d bytes",
                          static_cast<int>(kMaxString));
    const char* name = luaL_optstring(L, 2, "body");
    const lv_font_t* font = canvas_resolve_font(name);
    if (!font)
        return luaL_error(L,
                          "helix.ui.text_width: font token '%s' must be a base token; a "
                          "size-suffixed variant renders as the default font below its tier",
                          name);
    lua_pushinteger(L, helix::ui::text_width_n(str, static_cast<uint32_t>(len), font));
    return 1;
}

int ui_confirm(lua_State* L) {
    forbid_in_check(L, "helix.ui.confirm");
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
    auto cancel = [run, cancel_ref, release] {
        release();
        run(cancel_ref);
    };
    auto present = [ui_ptr, run, confirm_ref, release, cancel, title, msg, severity, confirm_text,
                    token = rt.token(), plugin_id = rt.plugin_id()] {
        if (token.expired())
            return false;
        helix::ui::ConfirmOptions opts;
        opts.on_cancel = cancel;
        opts.on_dismiss = cancel;
        opts.owner_token = token;
        lv_obj_t* dialog = helix::ui::modal_confirm(
            title.c_str(), msg.c_str(), severity, confirm_text.c_str(),
            [run, confirm_ref, release] {
                release();
                run(confirm_ref);
            },
            opts);
        if (!dialog) {
            release(); // never shown, so no close path will release the slot
            return false;
        }
        // Tagged with its plugin, so it holds other plugins back but never its own.
        ModalStack::instance().set_tag(ModalStack::instance().backdrop_for(dialog), plugin_id);
        ui_ptr->open_confirm = dialog;
        return true;
    };
    if (context(L).ui && context(L).ui->queue_confirm)
        context(L).ui->queue_confirm(std::move(present), release);
    else
        present();
    return 0;
}

// ---- helix.ui.edit_number / helix.ui.edit_text ----

/// The options table of an editor call: only `allowed` keys, each read by type.
struct EditorOpts {
    lua_State* L;
    int idx;
    const char* fn;

    void check_keys(std::initializer_list<const char*> allowed) const {
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            lua_pop(L, 1);
            bool ok = lua_type(L, -1) == LUA_TSTRING;
            if (ok) {
                ok = false;
                for (const char* a : allowed)
                    ok = ok || std::string_view(lua_tostring(L, -1)) == a;
            }
            if (!ok) {
                const char* key = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";
                luaL_error(L, "%s: unknown option '%s'", fn, key);
            }
        }
    }
    std::string str(const char* key, const char* def, size_t max_bytes) const {
        lua_getfield(L, idx, key);
        std::string v = def;
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING)
                luaL_error(L, "%s: %s must be a string", fn, key);
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            if (len > max_bytes)
                luaL_error(L, "%s: %s is at most %d bytes", fn, key, static_cast<int>(max_bytes));
            v.assign(s, len);
        }
        lua_pop(L, 1);
        return v;
    }
    std::optional<double> num(const char* key) const {
        lua_getfield(L, idx, key);
        std::optional<double> v;
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, -1)))
                luaL_error(L, "%s: %s must be a finite number", fn, key);
            v = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
        return v;
    }
    bool flag(const char* key) const {
        lua_getfield(L, idx, key);
        if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
            luaL_error(L, "%s: %s must be true or false", fn, key);
        const bool v = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return v;
    }
    void check_fn(const char* key, bool required) const {
        lua_getfield(L, idx, key);
        const bool missing = lua_isnil(L, -1);
        const bool bad = !missing && !lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (bad || (required && missing))
            luaL_error(L, "%s: %s must be a function", fn, key);
    }
    /// Only after every error path: luaL_error would unwind past the ref.
    int ref_fn(LuaRuntime& rt, const char* key) const {
        lua_getfield(L, idx, key);
        const int ref = lua_isfunction(L, -1) ? rt.ref_value(L, -1) : LUA_NOREF;
        lua_pop(L, 1);
        return ref;
    }
};

/// The callbacks an editor holds, released once when it ends.
struct EditorRefs {
    EditorRefs(LuaRuntime& r, UiState& u, bool keypad)
        : rt(&r), token(r.token()), editor(std::make_shared<Editor>()), ui(&u) {
        editor->keypad = keypad;
    }

    LuaRuntime* rt;
    LifetimeToken token;
    std::shared_ptr<Editor> editor;
    UiState* ui;
    int accept = LUA_NOREF;
    int cancel = LUA_NOREF;
    int validate = LUA_NOREF;
    /// The keypad's header and unit. The keypad's stored config points into these, and
    /// keeps this object alive through the hooks it holds.
    std::string keypad_title;
    std::string keypad_unit;

    bool live() const {
        return !editor->done && !token.expired();
    }
    /// Ends the editor: frees the plugin's slot and every ref. Returns false when it
    /// had already ended.
    bool finish() {
        if (!live())
            return false;
        editor->done = true;
        editor->dialog = nullptr;
        if (ui->editor == editor)
            ui->editor.reset();
        return true;
    }
    void release() {
        for (int ref : {accept, cancel, validate}) {
            if (ref != LUA_NOREF)
                rt->unref(ref);
        }
    }
    void run_accept(const LuaRuntime::PushFn& push) {
        if (!finish())
            return;
        rt->invoke(accept, push);
        release();
    }
    void run_cancel() {
        if (!finish())
            return;
        if (cancel != LUA_NOREF)
            rt->invoke(cancel);
        release();
    }
    /// The plugin's verdict on a value: empty to accept, else the message to show.
    std::string judge(const LuaRuntime::PushFn& push) {
        if (validate == LUA_NOREF || !live())
            return {};
        bool ok = false;
        std::string why;
        LuaRuntime::CheckLimits limits;
        limits.label = "editor validator";
        const bool ran = rt->call_sync(
            validate, push,
            [&](lua_State* co, int nres) {
                ok = nres >= 1 && lua_type(co, -nres) == LUA_TBOOLEAN && lua_toboolean(co, -nres);
                if (nres >= 2 && lua_type(co, -nres + 1) == LUA_TSTRING) {
                    size_t len = 0;
                    const char* s = lua_tolstring(co, -nres + 1, &len);
                    why.assign(s, std::min(len, kMaxEditorMessage));
                }
            },
            limits);
        if (ran && ok)
            return {};
        return why.empty() ? std::string(lv_tr("Invalid value")) : why;
    }
};

/// Queues `present` like a confirm (behind app dialogs, never behind the plugin's own),
/// or presents it at once where no host queues surfaces.
void queue_editor(PluginContext& ctx, const std::shared_ptr<EditorRefs>& refs,
                  std::function<bool()> present) {
    auto cancelled = [refs] {
        if (refs->finish())
            refs->release();
    };
    auto run = [refs, present = std::move(present)] {
        if (!refs->live())
            return false;
        if (!present()) {
            // It never appeared, so the plugin hears the request ended without a value.
            refs->run_cancel();
            return false;
        }
        refs->editor->shown = true;
        return true;
    };
    if (ctx.ui && ctx.ui->queue_confirm)
        ctx.ui->queue_confirm(std::move(run), std::move(cancelled));
    else
        run();
}

std::shared_ptr<EditorRefs> start_editor(lua_State* L, const char* fn, bool keypad) {
    forbid_in_check(L, fn);
    if (context(L).unloading)
        luaL_error(L, "%s cannot open during on_unload", fn);
    luaL_checktype(L, 1, LUA_TTABLE);
    if (ui_state(L).editor)
        luaL_error(L, "%s: at most one keypad or text editor shown or waiting per plugin", fn);
    return std::make_shared<EditorRefs>(LuaRuntime::from(L), ui_state(L), keypad);
}

void take_editor_refs(lua_State* L, const EditorOpts& o, EditorRefs& refs) {
    auto& rt = LuaRuntime::from(L);
    refs.accept = o.ref_fn(rt, "on_accept");
    refs.cancel = o.ref_fn(rt, "on_cancel");
    refs.validate = o.ref_fn(rt, "validate");
    ui_state(L).editor = refs.editor;
}

int ui_edit_number(lua_State* L) {
    const char* fn = "helix.ui.edit_number";
    auto refs = start_editor(L, fn, true);
    EditorOpts o{L, 1, fn};
    o.check_keys(
        {"title", "value", "min", "max", "decimals", "unit", "validate", "on_accept", "on_cancel"});
    const std::string title = o.str("title", "", kMaxEditorTitle);
    const std::string unit = o.str("unit", "", kMaxEditorUnit);
    const auto min = o.num("min");
    const auto max = o.num("max");
    const double value = o.num("value").value_or(min.value_or(0));
    const double decimals = o.num("decimals").value_or(0);
    if (!min || !max)
        return luaL_error(L, "%s: min and max are required", fn);
    if (*min < -kMaxKeypadValue)
        return luaL_error(L, "%s: min must be at least -%d", fn, static_cast<int>(kMaxKeypadValue));
    if (*max <= *min || *max > kMaxKeypadValue)
        return luaL_error(L, "%s: max must be above min and at most %d", fn,
                          static_cast<int>(kMaxKeypadValue));
    if (decimals != 0 && decimals != 1 && decimals != 2)
        return luaL_error(L, "%s: decimals must be 0, 1 or 2", fn);
    o.check_fn("on_accept", true);
    o.check_fn("on_cancel", false);
    o.check_fn("validate", false);
    // A plugin dialog sits on the top layer, above the keypad's overlay.
    PluginUi* ui = context(L).ui;
    if (ui && ui->dialog_count && ui->dialog_count(LuaRuntime::from(L).plugin_id()) > 0)
        return luaL_error(L, "%s: close the plugin's dialog first; the keypad opens below it", fn);
    take_editor_refs(L, o, *refs);

    const int places = static_cast<int>(decimals);
    const double scale = std::pow(10.0, places);
    auto push_value = [places, scale](float v) {
        const double rounded = std::round(static_cast<double>(v) * scale) / scale;
        return LuaRuntime::PushFn([places, rounded](lua_State* co) {
            if (places == 0)
                lua_pushinteger(co, static_cast<lua_Integer>(rounded));
            else
                lua_pushnumber(co, rounded);
            return 1;
        });
    };
    queue_editor(
        context(L), refs, [refs, title, unit, value, min = *min, max = *max, places, push_value] {
            // The keypad is one widget for the whole app.
            if (ui_keypad_is_visible())
                return false;
            ui_keypad_config_t config{};
            config.initial_value = static_cast<float>(value);
            config.min_value = static_cast<float>(min);
            config.max_value = static_cast<float>(max);
            refs->keypad_title = title;
            refs->keypad_unit = unit;
            config.title_label = refs->keypad_title.c_str();
            config.unit_label = refs->keypad_unit.c_str();
            config.allow_decimal = places > 0;
            // A range reaching below zero gets the keypad's sign key.
            config.allow_negative = min < 0;
            config.max_fraction_digits = places > 0 ? places : 1;
            config.validate = [refs, push_value](float v) { return refs->judge(push_value(v)); };
            config.on_confirm = [refs, push_value](float v) { refs->run_accept(push_value(v)); };
            config.on_cancel = [refs] { refs->run_cancel(); };
            return ui_keypad_show(&config);
        });
    return 0;
}

int ui_edit_text(lua_State* L) {
    const char* fn = "helix.ui.edit_text";
    auto refs = start_editor(L, fn, false);
    EditorOpts o{L, 1, fn};
    o.check_keys({"title", "value", "placeholder", "max_length", "password", "validate",
                  "on_accept", "on_cancel"});
    const std::string title = o.str("title", "", kMaxString);
    const std::string placeholder = o.str("placeholder", "", kMaxString);
    const double max_length = o.num("max_length").value_or(256);
    if (max_length < 1 || max_length > kMaxString - 1 || max_length != std::floor(max_length))
        return luaL_error(L, "%s: max_length must be a whole number from 1 to %d", fn,
                          static_cast<int>(kMaxString - 1));
    const std::string value = o.str("value", "", kMaxString - 1);
    const bool password = o.flag("password");
    if (lv_text_get_encoded_length(value.c_str()) > static_cast<uint32_t>(max_length))
        return luaL_error(L, "%s: value is longer than max_length", fn);
    o.check_fn("on_accept", true);
    o.check_fn("on_cancel", false);
    o.check_fn("validate", false);
    take_editor_refs(L, o, *refs);

    auto push_text = [](const std::string& s) {
        return LuaRuntime::PushFn([s](lua_State* co) {
            lua_pushlstring(co, s.data(), s.size());
            return 1;
        });
    };
    queue_editor(context(L), refs,
                 [refs, title, placeholder, value, password, push_text,
                  max_length = static_cast<uint32_t>(max_length),
                  plugin_id = LuaRuntime::from(L).plugin_id()] {
                     helix::ui::TextEntryModal::Config c;
                     c.title = title;
                     c.initial = value;
                     c.placeholder = placeholder;
                     c.max_length = max_length;
                     c.password = password;
                     c.validate = [refs, push_text](const std::string& s) {
                         return refs->judge(push_text(s));
                     };
                     c.on_accept = [refs, push_text](const std::string& s) {
                         refs->run_accept(push_text(s));
                     };
                     c.on_cancel = [refs] { refs->run_cancel(); };
                     lv_obj_t* dialog = helix::ui::TextEntryModal::show_owned(std::move(c));
                     if (!dialog)
                         return false;
                     // Tagged with its plugin, so it holds other plugins back but never its own.
                     ModalStack::instance().set_tag(ModalStack::instance().backdrop_for(dialog),
                                                    plugin_id);
                     refs->editor->dialog = dialog;
                     return true;
                 });
    return 0;
}

int surface_handle_close(lua_State* L) {
    forbid_in_check(L, "close");
    auto& ctx = context(L);
    if (ctx.ui)
        ctx.ui->close(static_cast<int>(lua_tointeger(L, lua_upvalueindex(1))));
    return 0;
}

int surface_handle_is_open(lua_State* L) {
    auto& ctx = context(L);
    const int handle = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    lua_pushboolean(L, ctx.ui && ctx.ui->is_open && ctx.ui->is_open(handle));
    return 1;
}

enum class SurfaceArg { Overlay, Dialog };

/// helix.ui.overlay and helix.ui.dialog: (component[, attrs]) -> handle. Every
/// string key but on_open/on_close is an attribute for lv_xml_create.
int open_surface(lua_State* L, SurfaceArg which) {
    const char* fn = which == SurfaceArg::Dialog ? "helix.ui.dialog" : "helix.ui.overlay";
    forbid_in_check(L, fn);
    auto& rt = LuaRuntime::from(L);
    PluginUi* ui = context(L).ui;
    const PluginUi::OpenFn* open = !ui                           ? nullptr
                                   : which == SurfaceArg::Dialog ? &ui->dialog
                                                                 : &ui->open;
    if (!open || !*open)
        return luaL_error(L, "%s is not available here", fn);
    if (context(L).unloading)
        return luaL_error(L, "%s cannot open during on_unload", fn);
    std::string component = luaL_checkstring(L, 1);
    if (!is_owned_name(rt.plugin_id(), component))
        return luaL_error(L, "%s: component '%s' is not owned by this plugin", fn,
                          component.c_str());
    if (which == SurfaceArg::Dialog && ui->dialog_count &&
        ui->dialog_count(rt.plugin_id()) >= kMaxDialogs)
        return luaL_error(L, "%s: at most %d dialog per plugin", fn, static_cast<int>(kMaxDialogs));

    int on_open_ref = LUA_NOREF;
    int on_close_ref = LUA_NOREF;
    PluginUi::Attrs attrs;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        auto is_callback_key = [](const std::string& k) {
            return k == "on_open" || k == "on_close";
        };
        // Anything else that is not a string is a mistake Lua should hear about
        // now rather than a widget silently ignoring.
        std::string bad_value_key;
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && !is_callback_key(lua_tostring(L, -2))) {
                if (lua_type(L, -1) != LUA_TSTRING)
                    bad_value_key = lua_tostring(L, -2);
                else
                    attrs.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
        if (!bad_value_key.empty())
            return luaL_error(L, "%s: attribute '%s' must be a string", fn, bad_value_key.c_str());
        for (const auto& [name, value] : attrs) {
            if (auto why = check_plugin_attr(rt.plugin_id(), name, value))
                return luaL_error(L, "%s: attribute '%s': %s", fn, name.c_str(), why->c_str());
        }
        for (const char* key : {"on_open", "on_close"}) {
            lua_getfield(L, 2, key);
            const bool bad = !lua_isnil(L, -1) && !lua_isfunction(L, -1);
            lua_pop(L, 1);
            if (bad)
                return luaL_error(L, "%s: %s must be a function", fn, key);
        }
        // Refs are taken only once every error path is past: luaL_error unwinds
        // without running anything that could release them.
        lua_getfield(L, 2, "on_open");
        if (lua_isfunction(L, -1))
            on_open_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "on_close");
        if (lua_isfunction(L, -1))
            on_close_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
    }

    LuaRuntime* rtp = &rt;
    auto token = rt.token();
    PluginUi::Callbacks cb;
    cb.on_open = [rtp, on_open_ref, token] {
        if (on_open_ref != LUA_NOREF && !token.expired())
            rtp->invoke(on_open_ref);
    };
    cb.on_close = [rtp, on_open_ref, on_close_ref, token](CloseHow how) {
        if (token.expired())
            return;
        if (on_close_ref != LUA_NOREF) {
            rtp->invoke(on_close_ref, [how](lua_State* co) {
                lua_pushstring(co, close_how_name(how));
                return 1;
            });
            rtp->unref(on_close_ref);
        }
        if (on_open_ref != LUA_NOREF)
            rtp->unref(on_open_ref);
    };
    const size_t before =
        which == SurfaceArg::Overlay && ui->open_count ? ui->open_count(rt.plugin_id()) : 0;
    const int handle = (*open)(component, std::move(cb), attrs);
    // A failed or no-op open (the component is already requested) never runs
    // these callbacks, so their refs are released here.
    const bool adopted = handle != 0 && (which == SurfaceArg::Dialog || !ui->open_count ||
                                         ui->open_count(rt.plugin_id()) != before);
    if (!adopted) {
        if (on_open_ref != LUA_NOREF)
            rt.unref(on_open_ref);
        if (on_close_ref != LUA_NOREF)
            rt.unref(on_close_ref);
    }
    if (handle == 0)
        return luaL_error(L, "%s: cannot open '%s'", fn, component.c_str());

    lua_newtable(L);
    lua_pushinteger(L, handle);
    lua_pushcclosure(L, &surface_handle_close, 1);
    lua_setfield(L, -2, "close");
    lua_pushinteger(L, handle);
    lua_pushcclosure(L, &surface_handle_is_open, 1);
    lua_setfield(L, -2, "is_open");
    return 1;
}

int ui_overlay(lua_State* L) {
    return open_surface(L, SurfaceArg::Overlay);
}

int ui_dialog(lua_State* L) {
    return open_surface(L, SurfaceArg::Dialog);
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
    std::string_view name = local_part_of(head);
    if (!is_valid_plugin_id(id) || name.empty())
        return t;
    t.id = std::string(id);
    t.name = std::string(name);
    t.arg = std::move(arg);
    return t;
}

bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name,
                         const std::optional<std::string>& arg) {
    return dispatch_ui_handler(rt, name, [arg](lua_State* co) {
        if (arg)
            lua_pushlstring(co, arg->data(), arg->size());
        else
            lua_pushnil(co);
        return 1;
    });
}

bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name, const LuaRuntime::PushFn& args) {
    auto& handlers = ui_state(rt.state()).handlers;
    auto it = handlers.find(name);
    if (it == handlers.end())
        return false;
    rt.invoke(it->second, args);
    return true;
}

bool call_ui_handler_sync(LuaRuntime& rt, const std::string& name, const LuaRuntime::PushFn& args,
                          const LuaRuntime::ReadFn& read) {
    auto& handlers = ui_state(rt.state()).handlers;
    auto it = handlers.find(name);
    if (it == handlers.end()) {
        rt.report_error("menu check '" + name + "' is not registered with helix.ui.on");
        return false;
    }
    return rt.call_sync(it->second, args, read);
}

// Retired subjects are freed at the next plugin load or unload rather than the
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
        // Its editor too, with every callback already disarmed.
        if (auto editor = state->editor) {
            editor->done = true;
            if (editor->dialog)
                Modal::hide(editor->dialog);
            else if (editor->keypad && editor->shown && ui_keypad_is_visible())
                ui_keypad_hide();
        }
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
    new_locked_metatable(L, kSubjectMeta);
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
                                      {"dialog", &ui_dialog},
                                      {"text_width", &ui_text_width},
                                      {"edit_number", &ui_edit_number},
                                      {"edit_text", &ui_edit_text},
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
