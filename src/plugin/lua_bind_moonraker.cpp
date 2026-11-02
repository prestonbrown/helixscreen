// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "i_moonraker_client.h"
#include "json_utils.h"
#include "lua_bindings.h"
#include "moonraker_subscription_merge.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace helix::plugin {

void require_permission(lua_State* L, Permission p, const char* call) {
    if (!context(L).manifest.permissions.count(p))
        luaL_error(L, "%s needs the '%s' permission in manifest.json", call, permission_name(p));
}

RpcCallback make_resolver(LuaRuntime::Pending p, PushRpc on_ok) {
    return [p, on_ok = std::move(on_ok)](RpcResult r) {
        p.resolve([r = std::move(r), on_ok](lua_State* co) -> int {
            if (!r.ok) {
                lua_pushnil(co);
                lua_pushlstring(co, r.error.data(), r.error.size());
                return 2;
            }
            return on_ok(co, r);
        });
    };
}

namespace {

// What a pushed json value costs the Lua state: its own bytes for a string, its
// serialized form for anything else.
size_t pushed_bytes(const json& v) {
    return v.is_string() ? v.get_ref<const std::string&>().size() : v.dump().size();
}

} // namespace

// Room left before the runtime's post-push check would fault the plugin.
size_t memory_remaining(const LuaRuntime& rt) {
    size_t cap = rt.memory_cap(), used = rt.memory_used();
    return cap > used ? cap - used : 0;
}

void charge_plugin_memory(lua_State* L, size_t bytes, const char* call) {
    // Left uncharged so a plugin whose data has reached the cap can still run: the
    // luaL_error that reports the refused call needs Lua memory to raise into, and
    // the host's next entry into the plugin (compiling a chunk, starting a thread)
    // is how it gets the chance to free some. On a full heap that entry fails out
    // of memory and the runtime disables the plugin.
    constexpr size_t kRaiseRoom = 4096;
    LuaRuntime& rt = context(L).rt;
    const size_t room = memory_remaining(rt);
    if (room < kRaiseRoom || bytes > room - kRaiseRoom || !rt.reserve_external(bytes))
        luaL_error(L, "%s: plugin memory budget exceeded", call);
}

int push_rpc_true(lua_State* co, const RpcResult&) {
    lua_pushboolean(co, 1);
    return 1;
}

int push_rpc_value(lua_State* co, const RpcResult& r) {
    return push_rpc_capped_body(co, r.value, pushed_bytes(r.value));
}

int push_rpc_capped_body(lua_State* co, const json& value, size_t body_bytes) {
    if (body_bytes > memory_remaining(LuaRuntime::from(co))) {
        lua_pushnil(co);
        lua_pushliteral(co, "response larger than the plugin memory cap");
        return 2;
    }
    push_json(co, value);
    return 1;
}

namespace {

RegistryKey g_moonraker_state_key;
constexpr size_t kMaxAgentHandlers = 16;
constexpr size_t kMaxSubscriptions = 8;
constexpr size_t kMaxSubscribedObjects = 16;
constexpr size_t kMaxFieldsPerObject = 32;
constexpr size_t kMaxObjectNameBytes = 64;
const char kSubMeta[] = "helix_moonraker_subscription";

/// One live helix.moonraker.subscribe: what it asked for and the Lua function to call.
struct Subscription {
    int fn_ref = LUA_NOREF;
    json spec; ///< object name -> null (every field) or an array of field names
    /// Shared with the deferred deliveries in flight for this subscription: false once
    /// cancelled, so a delivery queued on another thread never invokes a ref the cancel
    /// already released.
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

/// The plugin's subscriptions. The status handler reads it on whichever thread dispatched
/// the frame while the main thread subscribes and cancels, so every access to `subs` takes
/// the mutex; the records are plain JSON and ints, never Lua.
struct SubscriptionHub {
    std::mutex mu;
    std::map<uint64_t, Subscription> subs;
    uint64_t next_id = 1;
    /// A re-snapshot is queued for the main thread; more whole-object frames fold into it.
    std::atomic<bool> resnapshot_queued{false};
    /// Main thread only: a re-snapshot query is out, and another was asked for meanwhile.
    bool resnapshot_in_flight = false;
    bool resnapshot_again = false;
};

// Every registered agent-event handler keeps a notify subscription alive, so their count
// is bounded here rather than by the WebSocket's fan-out.
struct MoonrakerBindState {
    size_t agent_handlers = 0;
    /// Set once the one status handler is registered; it stays until the runtime closes,
    /// delivering nothing while no subscription is live.
    bool status_registered = false;
    std::function<void()> status_off;
    std::shared_ptr<SubscriptionHub> hub = std::make_shared<SubscriptionHub>();
};

MoonrakerBindState& moonraker_state_of(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_moonraker_state_key);
    auto* s = static_cast<MoonrakerBindState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

// An empty Lua table converts to an array; Moonraker wants an object for params.
json params_arg(lua_State* L, int index) {
    if (lua_isnoneornil(L, index))
        return json::object();
    json p = to_json(L, index);
    if (p.is_array() && p.empty())
        return json::object();
    return p;
}

// A download body lands whole in the plugin's Lua state, so it goes through the memory-cap
// check shared with the http bindings.
int push_rpc_download_body(lua_State* co, const RpcResult& r) {
    size_t bytes = r.value.is_string() ? r.value.get_ref<const std::string&>().size() : 0;
    return push_rpc_capped_body(co, r.value, bytes);
}

int gcode(lua_State* L) {
    require_permission(L, Permission::Gcode, "helix.gcode");
    std::string script = luaL_checkstring(L, 1);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, script](LuaRuntime::Pending p) {
        backend->gcode(script, make_resolver(p, &push_rpc_true));
    });
}

int call(lua_State* L) {
    std::string method = luaL_checkstring(L, 1);
    if (!is_readonly_moonraker_method(method))
        require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.call");
    json params = params_arg(L, 2);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, method, params](LuaRuntime::Pending p) {
        backend->call(method, params, make_resolver(p, &push_rpc_value));
    });
}

int query(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    json objects = json::object();
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "helix.moonraker.query: object names must be strings");
        std::string name = lua_tostring(L, -2);
        objects[name] = lua_isboolean(L, -1) ? json(nullptr) : to_json(L, -1);
        lua_pop(L, 1);
    }
    json params{{"objects", objects}};
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, params](LuaRuntime::Pending p) {
        backend->call("printer.objects.query", params, make_resolver(p, &push_rpc_value));
    });
}

int upload(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.upload");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 3, &len);
    std::string content(data, len);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(
        L, [backend, root, path, content](LuaRuntime::Pending p) {
            backend->upload(root, path, content, make_resolver(p, &push_rpc_true));
        });
}

int download(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.download");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    PluginBackend* backend = &context(L).backend;
    // One byte more than fits: a body that fills the ask is then refused by the memory-cap
    // check as (nil, error) instead of faulting the runtime.
    size_t max_bytes = memory_remaining(LuaRuntime::from(L)) + 1;
    return LuaRuntime::from(L).await_async(
        L, [backend, root, path, max_bytes](LuaRuntime::Pending p) {
            backend->download(root, path, max_bytes, make_resolver(p, &push_rpc_download_body));
        });
}

int on_agent_event(lua_State* L) {
    forbid_in_check(L, "helix.moonraker.on_agent_event");
    std::string event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    auto& mstate = moonraker_state_of(L);
    if (mstate.agent_handlers >= kMaxAgentHandlers)
        return luaL_error(L, "helix.moonraker.on_agent_event: at most %d handlers per plugin",
                          static_cast<int>(kMaxAgentHandlers));
    ++mstate.agent_handlers;
    int ref = rt.ref_value(L, 2);
    LuaRuntime* rtp = &rt;
    LifetimeToken token = rt.token();
    // Runs on the WebSocket thread: inspects JSON only, then defers to the main thread.
    auto off = context(L).backend.on_notify(
        "notify_agent_event", [rtp, token, ref, event](const json& msg) {
            auto params = msg.find("params");
            if (params == msg.end() || !params->is_array() || params->empty() ||
                !(*params)[0].is_object())
                return;
            const json& p0 = (*params)[0];
            if (helix::json_util::safe_string(p0, "event", "") != event)
                return;
            std::string agent = helix::json_util::safe_string(p0, "agent", "");
            json data = p0.contains("data") ? p0["data"] : json();
            size_t bytes = pushed_bytes(data);
            token.defer("plugin_agent_event", [rtp, ref, agent, data, bytes]() {
                if (bytes > memory_remaining(*rtp)) {
                    spdlog::warn("[plugin {}] agent event data over the memory cap; "
                                 "handler saw nil",
                                 rtp->plugin_id());
                    rtp->invoke(ref, [agent](lua_State* co) {
                        lua_pushlstring(co, agent.data(), agent.size());
                        lua_pushnil(co);
                        return 2;
                    });
                    return;
                }
                rtp->invoke(ref, [agent, &data](lua_State* co) {
                    lua_pushlstring(co, agent.data(), agent.size());
                    push_json(co, data);
                    return 2;
                });
            });
        });
    rt.on_close([off]() { off(); });
    return 0;
}

// `status` reduced to what `spec` asked for: only the named objects, and for a field list
// only those fields. Pure, so it runs on the WebSocket thread.
json filter_status(const json& status, const json& spec) {
    json out = json::object();
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        auto d = status.find(it.key());
        if (d == status.end() || !d->is_object())
            continue;
        if (it.value().is_null()) {
            out[it.key()] = *d;
            continue;
        }
        json fields = json::object();
        for (const auto& f : it.value()) {
            if (!f.is_string())
                continue;
            const std::string& name = f.get_ref<const std::string&>();
            auto v = d->find(name);
            if (v != d->end())
                fields[name] = *v;
        }
        if (!fields.empty())
            out[it.key()] = std::move(fields);
    }
    return out;
}

// One status table on its way to a subscription's callback, `fn(status, snapshot)`:
// measured on the producing thread, then handed to the main thread through the runtime's
// token with the same memory-cap check the agent-event deliveries use.
void deliver_status(LuaRuntime* rtp, LifetimeToken token,
                    const std::shared_ptr<std::atomic<bool>>& alive, int ref, json data,
                    bool snapshot) {
    size_t bytes = pushed_bytes(data);
    token.defer("plugin_status_update",
                [rtp, alive, ref, data = std::move(data), bytes, snapshot]() {
                    if (!alive->load())
                        return;
                    if (bytes > memory_remaining(*rtp)) {
                        spdlog::warn("[plugin {}] status update over the memory cap; "
                                     "callback saw nil",
                                     rtp->plugin_id());
                        rtp->invoke(ref, [snapshot](lua_State* co) {
                            lua_pushnil(co);
                            lua_pushboolean(co, snapshot);
                            return 2;
                        });
                        return;
                    }
                    rtp->invoke(ref, [&data, snapshot](lua_State* co) {
                        push_json(co, data);
                        lua_pushboolean(co, snapshot);
                        return 2;
                    });
                });
}

// The status table of a printer.objects.query reply, which the backend hands over as the
// whole JSON-RPC message: {"result": {"status": {...}}}. Empty when it has none.
json query_status(const json& response) {
    auto res = response.is_object() ? response.find("result") : response.end();
    if (res == response.end() || !res->is_object())
        return json::object();
    auto st = res->find("status");
    return st != res->end() && st->is_object() ? *st : json::object();
}

// Delivers `status` as a snapshot to subscription `only`, or to every live subscription
// when `only` is 0. Each gets its own fields, an empty table when none of its objects
// reported. Pure JSON under the hub's lock, so it runs on any thread.
void deliver_snapshot(LuaRuntime* rtp, LifetimeToken token, SubscriptionHub& hub,
                      const json& status, uint64_t only) {
    std::vector<std::tuple<std::shared_ptr<std::atomic<bool>>, int, json>> targets;
    {
        std::lock_guard<std::mutex> lk(hub.mu);
        for (const auto& [id, s] : hub.subs) {
            if (only != 0 && id != only)
                continue;
            targets.emplace_back(s.alive, s.fn_ref, filter_status(status, s.spec));
        }
    }
    for (auto& [alive, ref, data] : targets)
        deliver_status(rtp, token, alive, ref, std::move(data), /*snapshot=*/true);
}

json merged_specs(SubscriptionHub& hub);

// Queries every subscribed object and delivers the answer to every subscription as a
// snapshot. Main thread only. One query is out at a time; asks arriving meanwhile fold
// into one more once it settles, because its answer may predate what they announced.
void resnapshot(LuaRuntime* rtp, LifetimeToken token, std::shared_ptr<SubscriptionHub> hub,
                PluginBackend* backend) {
    if (hub->resnapshot_in_flight) {
        hub->resnapshot_again = true;
        return;
    }
    json objects = merged_specs(*hub);
    if (objects.empty())
        return;
    hub->resnapshot_in_flight = true;
    // The callback can outlive the runtime, so it logs a copy of the id.
    backend->call("printer.objects.query", json{{"objects", objects}},
                  [rtp, token, hub, backend, id = std::string(rtp->plugin_id())](RpcResult r) {
                      if (r.ok)
                          deliver_snapshot(rtp, token, *hub, query_status(r.value), 0);
                      else
                          spdlog::debug("[plugin {}] subscription snapshot query failed: {}", id,
                                        r.error);
                      token.defer("plugin_resnapshot_settle", [rtp, token, hub, backend]() {
                          hub->resnapshot_in_flight = false;
                          if (std::exchange(hub->resnapshot_again, false))
                              resnapshot(rtp, token, hub, backend);
                      });
                  });
}

// The one status handler a plugin registers. A whole-object frame (discovery after a
// connect or Klipper restart, a subscription refresh) re-snapshots every subscription
// from a fresh query instead of replaying the frame, which discovery may have carried for
// seconds. Any other frame is a delta, reduced per subscription under the hub's lock.
std::function<void(const json&)> make_status_handler(LuaRuntime* rtp, LifetimeToken token,
                                                     std::shared_ptr<SubscriptionHub> hub,
                                                     PluginBackend* backend) {
    return [rtp, token, hub, backend](const json& msg) {
        if (helix::json_util::safe_bool(msg, helix::kWholeObjectsMarker)) {
            if (!hub->resnapshot_queued.exchange(true))
                token.defer("plugin_resnapshot", [rtp, token, hub, backend]() {
                    hub->resnapshot_queued = false;
                    resnapshot(rtp, token, hub, backend);
                });
            return;
        }
        auto params = msg.find("params");
        if (params == msg.end() || !params->is_array() || params->empty() ||
            !(*params)[0].is_object())
            return;
        const json& delta = (*params)[0];
        // A cancel between the copy and the invoke leaves alive false, so the main-thread
        // check in deliver_status never touches the released ref.
        std::vector<std::tuple<std::shared_ptr<std::atomic<bool>>, int, json>> targets;
        {
            std::lock_guard<std::mutex> lk(hub->mu);
            for (const auto& [id, s] : hub->subs) {
                json f = filter_status(delta, s.spec);
                if (f.empty())
                    continue;
                targets.emplace_back(s.alive, s.fn_ref, std::move(f));
            }
        }
        for (auto& [alive, ref, data] : targets)
            deliver_status(rtp, token, alive, ref, std::move(data), /*snapshot=*/false);
    };
}

// The subscribe argument as a printer.objects.subscribe map: object name -> null (every
// field) or an array of field names. Raises a Lua error naming the limit it broke.
json parse_subscribe_spec(lua_State* L) {
    json spec = json::object();
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "helix.moonraker.subscribe: object names must be strings");
        std::string name = lua_tostring(L, -2);
        if (name.size() > kMaxObjectNameBytes)
            luaL_error(L, "helix.moonraker.subscribe: object names are at most %d bytes",
                       static_cast<int>(kMaxObjectNameBytes));
        for (unsigned char c : name)
            if (c < 0x20 || c > 0x7E)
                luaL_error(L, "helix.moonraker.subscribe: object names are printable ASCII");
        json fields;
        if (lua_isboolean(L, -1)) {
            if (!lua_toboolean(L, -1))
                luaL_error(L,
                           "helix.moonraker.subscribe: values are true or a list of field names");
            fields = nullptr; // every field
        } else {
            json arr = to_json(L, -1);
            if (!arr.is_array())
                luaL_error(L,
                           "helix.moonraker.subscribe: values are true or a list of field names");
            if (arr.size() > kMaxFieldsPerObject)
                luaL_error(L, "helix.moonraker.subscribe: at most %d fields per object",
                           static_cast<int>(kMaxFieldsPerObject));
            for (const auto& f : arr)
                if (!f.is_string())
                    luaL_error(L, "helix.moonraker.subscribe: field names must be strings");
            fields = std::move(arr);
        }
        spec[name] = std::move(fields);
        lua_pop(L, 1);
    }
    return spec;
}

// The union of every live subscription's spec, so a plugin's published set shrinks again
// when one cancels. merge_subscription_objects is the same union the client applies across
// plugins.
json merged_specs(SubscriptionHub& hub) {
    std::lock_guard<std::mutex> lk(hub.mu);
    json merged = json::object();
    for (const auto& [id, s] : hub.subs)
        merged = helix::merge_subscription_objects(merged, s.spec);
    return merged;
}

int subscribe(lua_State* L) {
    forbid_in_check(L, "helix.moonraker.subscribe");
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    auto& mstate = moonraker_state_of(L);
    PluginBackend* backend = &context(L).backend;

    json spec = parse_subscribe_spec(L);
    {
        std::lock_guard<std::mutex> lk(mstate.hub->mu);
        if (mstate.hub->subs.size() >= kMaxSubscriptions)
            return luaL_error(L, "helix.moonraker.subscribe: at most %d subscriptions per plugin",
                              static_cast<int>(kMaxSubscriptions));
    }
    json merged = helix::merge_subscription_objects(merged_specs(*mstate.hub), spec);
    if (merged.size() > kMaxSubscribedObjects)
        return luaL_error(L, "helix.moonraker.subscribe: at most %d subscribed objects per plugin",
                          static_cast<int>(kMaxSubscribedObjects));

    int ref = rt.ref_value(L, 2);
    uint64_t sid;
    {
        std::lock_guard<std::mutex> lk(mstate.hub->mu);
        sid = mstate.hub->next_id++;
        mstate.hub->subs.emplace(sid, Subscription{ref, spec});
    }
    backend->set_plugin_objects(rt.plugin_id(), merged);

    LuaRuntime* rtp = &rt;
    LifetimeToken token = rt.token();
    auto hub = mstate.hub;
    if (!mstate.status_registered) {
        mstate.status_registered = true;
        mstate.status_off = backend->on_status(make_status_handler(rtp, token, hub, backend));
    }

    // First delivery: a snapshot of the current values, from one query for this
    // subscription's own objects.
    backend->call("printer.objects.query", json{{"objects", spec}},
                  [rtp, token, hub, sid, id = std::string(rt.plugin_id())](RpcResult r) {
                      if (!r.ok) {
                          spdlog::debug("[plugin {}] initial subscribe query failed: {}", id,
                                        r.error);
                          return;
                      }
                      deliver_snapshot(rtp, token, *hub, query_status(r.value), sid);
                  });

    *static_cast<uint64_t*>(lua_newuserdatauv(L, sizeof(uint64_t), 0)) = sid;
    luaL_setmetatable(L, kSubMeta);
    return 1;
}

int subscription_cancel(lua_State* L) {
    forbid_in_check(L, "subscription:cancel");
    uint64_t sid = *static_cast<uint64_t*>(luaL_checkudata(L, 1, kSubMeta));
    auto& rt = LuaRuntime::from(L);
    auto& mstate = moonraker_state_of(L);
    auto alive = std::shared_ptr<std::atomic<bool>>();
    int ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> lk(mstate.hub->mu);
        if (auto it = mstate.hub->subs.find(sid); it != mstate.hub->subs.end()) {
            alive = it->second.alive;
            ref = it->second.fn_ref;
            mstate.hub->subs.erase(it);
        }
    }
    if (ref == LUA_NOREF)
        return 0; // already cancelled
    alive->store(false);
    rt.unref(ref);
    context(L).backend.set_plugin_objects(rt.plugin_id(), merged_specs(*mstate.hub));
    return 0;
}

} // namespace

void install_moonraker_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new MoonrakerBindState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_moonraker_state_key);
    ctx.rt.on_close([state] { delete state; });
    // Runs before the state's own closer (closers run in reverse registration order):
    // drops the status handler, stops every delivery and clears the plugin's objects, so
    // unload shrinks the union subscription. The backend outlives the runtime.
    PluginBackend* backend = &ctx.backend;
    std::string id = ctx.manifest.id;
    ctx.rt.on_close([state, backend, id] {
        if (state->status_off) {
            state->status_off();
            state->status_off = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(state->hub->mu);
            for (auto& [sid, s] : state->hub->subs)
                s.alive->store(false);
            state->hub->subs.clear();
        }
        backend->set_plugin_objects(id, json::object());
    });

    new_locked_metatable(L, kSubMeta);
    lua_newtable(L);
    lua_pushcfunction(L, &subscription_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg fns[] = {{"call", &call},
                                   {"query", &query},
                                   {"upload", &upload},
                                   {"download", &download},
                                   {"on_agent_event", &on_agent_event},
                                   {"subscribe", &subscribe},
                                   {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &gcode);
    lua_setfield(L, -2, "gcode");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "moonraker");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
