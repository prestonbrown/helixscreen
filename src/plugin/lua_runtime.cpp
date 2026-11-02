// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_runtime.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <time.h>

namespace helix::plugin {

namespace {

/// CPU time the calling thread has used. The budget charges a plugin for its own
/// work: a busy machine preempting the main thread must not fault a plugin that
/// stays within it, and a runaway loop burns CPU time just as it burns wall time.
/// Where the clock is unavailable it reads as the earliest time, so the CPU check
/// never trips and the wall ceiling alone bounds the entry.
std::chrono::nanoseconds thread_cpu_time() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return std::chrono::nanoseconds::min();
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}

// Trims the base library. Runs as trusted code before any plugin code.
constexpr const char* kSandboxPrelude = R"(
local load, collect = load, collectgarbage
dofile, loadfile, string.dump = nil, nil, nil
_G.load = function(chunk, name, _, ...)
    if select('#', ...) > 0 then return load(chunk, name, "t", (...)) end
    return load(chunk, name, "t")
end
local allowed = { count = true, collect = true, step = true }
_G.collectgarbage = function(opt, ...)
    opt = opt or "collect"
    if not allowed[opt] then
        error("collectgarbage('" .. tostring(opt) .. "') is not available to plugins", 2)
    end
    return collect(opt, ...)
end
-- Finalizers run with hooks off, so a __gc that loops would hang the UI thread past any
-- budget, and one armed in a menu check would act later, outside check mode. Lua marks
-- an object for finalization only here, so refusing __gc at setmetatable is enough.
local set_mt, raw_get = setmetatable, rawget
_G.setmetatable = function(t, mt)
    if type(mt) == "table" and raw_get(mt, "__gc") ~= nil then
        error("__gc is not available to plugins", 2)
    end
    return set_mt(t, mt)
end
-- The shared string metatable is the app's, like the bindings' userdata ones.
getmetatable("").__metatable = "locked"
-- An empty result returns at once: stock rep loops n times building nothing, with no
-- allocation for the memory cap to refuse and no instruction for the time budget to see.
-- Any non-empty result is sized up front, so the memory cap bounds it.
local rep, tointeger = string.rep, math.tointeger
string.rep = function(s, n, sep)
    local count = tointeger(n)
    if count and count > 1 and type(s) == "string" and #s == 0
            and (sep == nil or (type(sep) == "string" and #sep == 0)) then
        return ""
    end
    return rep(s, n, sep)
end
)";

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

RegistryKey g_loaded_key{"loaded"}; // its address keys the require cache in the registry

std::string budget_reason(const LuaRuntime::Limits& limits) {
    return "exceeded its time budget (" + std::to_string(limits.time_budget.count()) +
           " ms of CPU, " + std::to_string(limits.wall_ceiling.count()) + " ms in all)";
}

std::string memory_cap_reason(size_t cap_bytes) {
    return "out of memory (cap " + std::to_string(cap_bytes / 1024) + " KB)";
}

// A refusal is only safe inside a protected entry, where lua_resume turns it into a Lua
// error. Host-side bookkeeping (refs, thread setup, result pushes) allocates on L_ or on
// a coroutine that is not running, where a refusal escapes as a panic and aborts the
// process; this guard suspends the cap for its duration.
class HostWorkGuard {
  public:
    explicit HostWorkGuard(int& counter) : counter_(counter) {
        ++counter_;
    }
    ~HostWorkGuard() {
        --counter_;
    }
    HostWorkGuard(const HostWorkGuard&) = delete;
    HostWorkGuard& operator=(const HostWorkGuard&) = delete;

  private:
    int& counter_;
};

} // namespace

ErrorWindow::ErrorWindow(size_t threshold, std::chrono::seconds window)
    : threshold_(threshold), window_(window) {}

bool ErrorWindow::record(Clock::time_point now) {
    times_.push_back(now);
    while (!times_.empty() && now - times_.front() > window_)
        times_.pop_front();
    return times_.size() >= threshold_;
}

std::vector<std::string> require_candidates(const std::string& plugin_dir,
                                            const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.')
        return {};
    std::string rel;
    char prev = 0;
    for (char c : name) {
        bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (c == '.') {
            if (prev == '.')
                return {};
            rel += '/';
        } else if (word) {
            rel += c;
        } else {
            return {};
        }
        prev = c;
    }
    return {plugin_dir + "/" + rel + ".lua", plugin_dir + "/lib/" + rel + ".lua"};
}

bool is_plugin_relative_path(std::string_view path, std::string_view ext) {
    if (path.empty() || path.size() > 128 || path.front() == '/' || path.size() <= ext.size() ||
        path.substr(path.size() - ext.size()) != ext)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        std::string_view seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        for (char c : seg) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '.' || c == '-';
            if (!ok)
                return false;
        }
        start = end + 1;
    }
    return true;
}

LuaRuntime::Pending::Pending(LuaRuntime* rt, lua_State* co, LifetimeToken token)
    : rt_(rt), co_(co), token_(std::move(token)),
      done_(std::make_shared<std::atomic<bool>>(false)) {}

LuaRuntime::LuaRuntime(std::string plugin_id, std::string plugin_dir, Limits limits,
                       FaultHandler on_fault)
    : plugin_id_(std::move(plugin_id)), plugin_dir_(std::move(plugin_dir)), limits_(limits),
      on_fault_(std::move(on_fault)) {
    L_ = lua_newstate(&LuaRuntime::alloc, this);
    *static_cast<LuaRuntime**>(lua_getextraspace(L_)) = this;
    lua_atpanic(L_, [](lua_State* L) -> int {
        spdlog::critical("[plugin] unprotected Lua error: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)");
        return 0; // Lua aborts when the panic handler returns
    });
    install_sandbox();
    lua_sethook(L_, &LuaRuntime::budget_hook, LUA_MASKCOUNT, kHookInterval);
}

LuaRuntime::~LuaRuntime() {
    guard_.invalidate();
    for (auto it = closers_.rbegin(); it != closers_.rend(); ++it)
        (*it)();
    closers_.clear();
    lua_close(L_);
}

LuaRuntime& LuaRuntime::from(lua_State* L) {
    return **static_cast<LuaRuntime**>(lua_getextraspace(L));
}

bool LuaRuntime::reserve_external(size_t bytes) {
    if (bytes > limits_.memory_bytes || used_ > limits_.memory_bytes - bytes)
        return false;
    used_ += bytes;
    return true;
}

void LuaRuntime::release_external(size_t bytes) {
    used_ = used_ >= bytes ? used_ - bytes : 0;
}

void* LuaRuntime::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* rt = static_cast<LuaRuntime*>(ud);
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        std::free(ptr);
        rt->used_ -= old;
        return nullptr;
    }
    // Only a protected entry may be refused, and only while no host bookkeeping is in
    // progress. A refusal anywhere else reaches Lua's panic handler and aborts.
    if (nsize > old && rt->depth_ > 0 && rt->host_work_ == 0 &&
        rt->used_ - old + nsize > rt->limits_.memory_bytes)
        return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p)
        rt->used_ = rt->used_ - old + nsize;
    return p;
}

void LuaRuntime::install_sandbox() {
    static const luaL_Reg kLibs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const auto& lib : kLibs) {
        luaL_requiref(L_, lib.name, lib.func, 1);
        lua_pop(L_, 1);
    }
    if (luaL_dostring(L_, kSandboxPrelude) != LUA_OK) {
        spdlog::critical("[plugin] sandbox prelude failed: {}", lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    lua_newtable(L_);
    lua_rawsetp(L_, LUA_REGISTRYINDEX, &g_loaded_key);
    lua_pushcfunction(L_, &LuaRuntime::lua_require);
    lua_setglobal(L_, "require");
    // A C function rather than prelude Lua: the prelude runs before the helix global
    // exists, and the base library's print writes straight to stdout.
    lua_pushcfunction(L_, &LuaRuntime::lua_print);
    lua_setglobal(L_, "print");
    lua_newtable(L_);
    lua_setglobal(L_, "helix");
}

int LuaRuntime::lua_print(lua_State* L) {
    // The line lives outside the Lua cap, and many arguments can reference one large string,
    // so it stops growing at a fixed size.
    constexpr size_t kMaxLine = 4096;
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n && line.size() < kMaxLine; ++i) {
        if (i > 1)
            line += '\t';
        size_t len = 0;
        const char* text = luaL_tolstring(L, i, &len); // print's tostring semantics
        line.append(text, std::min(len, kMaxLine - line.size()));
        lua_pop(L, 1); // the converted copy
    }
    if (line.size() >= kMaxLine)
        line += "...";
    spdlog::info("[plugin {}] {}", from(L).plugin_id_, line);
    return 0;
}

int LuaRuntime::lua_require(lua_State* L) {
    auto& rt = from(L);
    // A module's top level is an ordinary entry's work; run from a check, a refused
    // call inside it would leave a half-loaded module cached.
    if (rt.in_check_)
        return luaL_error(L, "require is not allowed in %s %s", rt.check_article(),
                          rt.check_label_);
    std::string name = luaL_checkstring(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_loaded_key);
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    auto candidates = require_candidates(rt.plugin_dir_, name);
    if (candidates.empty())
        return luaL_error(L, "require('%s'): not a module name", name.c_str());
    for (const auto& path : candidates) {
        if (!file_exists(path))
            continue;
        if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK)
            return lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
        return 1;
    }
    return luaL_error(L, "require('%s'): module not found in the plugin", name.c_str());
}

bool LuaRuntime::run_string(const std::string& code, const std::string& chunk_name) {
    if (faulted_)
        return false;
    // Compiling counts as an entry, so the memory cap bounds the source a plugin can load.
    ++depth_;
    int status = luaL_loadbufferx(L_, code.data(), code.size(), chunk_name.c_str(), "t");
    --depth_;
    if (status != LUA_OK) {
        std::string msg = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        if (status == LUA_ERRMEM)
            fault(memory_cap_reason(limits_.memory_bytes));
        else
            report_error(msg);
        return false;
    }
    return spawn({});
}

bool LuaRuntime::run_file(const std::string& relative_path) {
    if (!is_plugin_relative_path(relative_path)) {
        report_error("refusing to run '" + relative_path + "': not a path inside the plugin");
        return false;
    }
    const std::string path = plugin_dir_ + "/" + relative_path;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        report_error("cannot read " + relative_path);
        return false;
    }
    if (static_cast<size_t>(st.st_size) > limits_.memory_bytes) {
        report_error(relative_path + " is larger than the plugin memory cap");
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        report_error("cannot read " + relative_path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return run_string(ss.str(), "@" + relative_path);
}

int LuaRuntime::ref_value(lua_State* L, int index) {
    HostWorkGuard host_work(host_work_);
    lua_pushvalue(L, index);
    if (L != L_)
        lua_xmove(L, L_, 1);
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

void LuaRuntime::unref(int ref) {
    luaL_unref(L_, LUA_REGISTRYINDEX, ref);
}

void LuaRuntime::invoke(int fn_ref, const PushFn& push_args) {
    if (faulted_)
        return;
    {
        // A stack push on L_ can grow the stack; from inside a running entry a
        // refusal there reaches the panic handler and aborts, so the cap is held off.
        HostWorkGuard host_work(host_work_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, fn_ref);
    }
    spawn(push_args);
}

const char* LuaRuntime::check_article() const {
    const char c = check_label_[0];
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' ? "an" : "a";
}

bool LuaRuntime::call_sync(int fn_ref, const PushFn& args, const ReadFn& read, CheckLimits limits) {
    if (faulted_)
        return false;
    if (depth_ > 0) {
        report_error(std::string(limits.label) + " cannot run inside another plugin entry");
        return false;
    }
    lua_State* co = nullptr;
    int nargs = 0;
    {
        HostWorkGuard host_work(host_work_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, fn_ref);
        co = lua_newthread(L_);
        threads_[co] = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_xmove(L_, co, 1);
        nargs = args ? args(co) : 0;
    }
    if (used_ > limits_.memory_bytes) {
        lua_settop(co, 0);
        drop(co);
        report_error(std::string(limits.label) + " arguments exceed the plugin memory cap");
        return false;
    }

    // The overrun of a check is reported, not faulted, so it never takes the enter()
    // path; the kill flag is cleared here before any later entry can see it.
    deadline_ = thread_cpu_time() + limits.time_budget;
    wall_deadline_ = Clock::now() + limits.wall_ceiling;
    killed_ = false;
    yielded_for_async_ = false;
    in_check_ = true;
    check_label_ = limits.label;
    ++depth_;
    int nres = 0;
    int status = lua_resume(co, L_, nargs, &nres);

    // Everything that can run plugin code stays inside check mode and the check's
    // budget: reading the results, and closing the thread, which runs any pending
    // <close> handlers (protected, so one that raises is only a failure).
    std::string failure;
    if (killed_) {
        failure = std::string(limits.label) + " exceeded its time budget";
    } else if (status == LUA_YIELD) {
        failure = std::string(limits.label) + " yielded; a check must return without waiting";
    } else if (status != LUA_OK) {
        HostWorkGuard host_work(host_work_);
        const char* msg = lua_tostring(co, -1);
        luaL_traceback(L_, co, msg ? msg : "(error object is not a string)", 0);
        if (const char* text = lua_tostring(L_, -1))
            failure = text;
        lua_pop(L_, 1);
    } else if (read) {
        HostWorkGuard host_work(host_work_);
        read(co, nres);
    }
    const bool killed_before_close = killed_;
    if (lua_closethread(co, L_) != LUA_OK) {
        std::string why;
        if (killed_ && !killed_before_close) {
            why = std::string(limits.label) + " exceeded its time budget in a <close> handler";
        } else {
            const char* msg = lua_tostring(co, -1);
            why = std::string(limits.label) +
                  " <close> handler failed: " + (msg ? msg : "(error object is not a string)");
        }
        failure = failure.empty() ? why : failure + "; " + why;
    }
    --depth_;
    in_check_ = false;
    killed_ = false;
    yielded_for_async_ = false;

    drop(co);
    if (failure.empty())
        return true;
    report_error(failure);
    return false;
}

void LuaRuntime::on_close(std::function<void()> fn) {
    closers_.push_back(std::move(fn));
}

// The function to run is on top of L_'s stack.
bool LuaRuntime::spawn(const PushFn& push_args) {
    lua_State* co = nullptr;
    int nargs = 0;
    {
        HostWorkGuard host_work(host_work_);
        co = lua_newthread(L_);
        int thread_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_xmove(L_, co, 1);
        nargs = push_args ? push_args(co) : 0;
        threads_[co] = thread_ref;
    }
    // Args are pushed outside any entry, so the cap is enforced here, before running.
    if (used_ > limits_.memory_bytes) {
        lua_settop(co, 0);
        drop(co);
        fault(memory_cap_reason(limits_.memory_bytes));
        return false;
    }
    return enter(co, nargs);
}

bool LuaRuntime::enter(lua_State* co, int nargs) {
    if (depth_ >= kMaxEntryDepth) {
        lua_settop(co, 0);
        drop(co);
        report_error("entries nested more than " + std::to_string(kMaxEntryDepth) +
                     " deep (an observer setting the subject it observes?)");
        return false;
    }
    if (depth_ == 0) {
        deadline_ = thread_cpu_time() + limits_.time_budget;
        wall_deadline_ = Clock::now() + limits_.wall_ceiling;
        killed_ = false;
    }
    bool outer_yielded = yielded_for_async_;
    yielded_for_async_ = false;
    ++depth_;
    int nres = 0;
    int status = lua_resume(co, L_, nargs, &nres);
    bool yielded_for_async = yielded_for_async_;
    yielded_for_async_ = outer_yielded;

    // A coroutine kept for its async reply stays as it is. Any other is closed here,
    // still inside the entry: its pending <close> handlers run with the memory cap and
    // the time budget applying, and a kill inside one faults below like any other.
    const bool keep = status == LUA_YIELD && yielded_for_async && !(depth_ == 1 && killed_);
    std::string text;
    std::string close_error;
    if (!keep) {
        if (status != LUA_OK && status != LUA_YIELD) {
            HostWorkGuard host_work(host_work_);
            const char* msg = lua_tostring(co, -1);
            luaL_traceback(L_, co, msg ? msg : "(error object is not a string)", 0);
            if (const char* t = lua_tostring(L_, -1))
                text = t;
            lua_pop(L_, 1);
        }
        if (lua_closethread(co, L_) != LUA_OK) {
            const char* msg = lua_tostring(co, -1);
            close_error = std::string("<close> handler failed: ") +
                          (msg ? msg : "(error object is not a string)");
        }
    }
    --depth_;

    // The count hook's error can come back as a plain value from coroutine.resume or end
    // as a bare yield, so the overrun is checked on every exit of the outermost entry.
    // A killed plugin must never be resumed later, so a suspended coroutine is dropped too.
    if (depth_ == 0 && killed_) {
        drop(co);
        fault(budget_reason(limits_));
        return false;
    }
    if (keep) {
        lua_pop(co, nres);
        return true; // stays in threads_ until its Pending resolves
    }
    drop(co);
    if (status == LUA_OK) {
        if (close_error.empty())
            return true;
        report_error(close_error);
        return false;
    }
    if (status == LUA_YIELD) {
        report_error("coroutine.yield() outside an async call");
        return false;
    }
    if (killed_)
        fault(budget_reason(limits_));
    else if (status == LUA_ERRMEM)
        fault(memory_cap_reason(limits_.memory_bytes));
    else
        report_error(close_error.empty() ? text : text + "; " + close_error);
    return false;
}

void LuaRuntime::drop(lua_State* co) {
    auto it = threads_.find(co);
    if (it == threads_.end())
        return;
    lua_closethread(co, L_);
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    threads_.erase(it);
}

void LuaRuntime::report_error(const std::string& message) {
    spdlog::warn("[plugin {}] {}", plugin_id_, message);
    if (errors_.record(Clock::now()))
        fault("3 errors within 60 s");
}

void LuaRuntime::fault(const std::string& reason) {
    if (faulted_)
        return;
    faulted_ = true;
    fault_reason_ = reason;
    spdlog::error("[plugin {}] disabled: {}", plugin_id_, reason);
    if (on_fault_)
        on_fault_(reason);
}

// The string library also runs this from inside a pattern match, which no VM instruction
// interrupts, passing a NULL ar (patches/lua-pattern-step-budget.patch): it must not read ar.
void LuaRuntime::budget_hook(lua_State* L, lua_Debug*) {
    auto& rt = from(L);
    if (!rt.killed_ && thread_cpu_time() < rt.deadline_ && Clock::now() < rt.wall_deadline_)
        return;
    rt.killed_ = true;
    // Firing on every instruction means each instruction outside the innermost pcall raises
    // again, so no depth of pcall can hold the entry open.
    lua_sethook(L, &LuaRuntime::budget_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "exceeded the plugin time budget");
}

int LuaRuntime::await_async(lua_State* co, const std::function<void(Pending)>& start) {
    // Only entry coroutines are in threads_, which rejects plugin-made coroutines.
    if (in_check_) {
        lua_Debug ar{};
        const char* name = lua_getstack(co, 0, &ar) && lua_getinfo(co, "n", &ar) && ar.name
                               ? ar.name
                               : "an async call";
        return luaL_error(co, "%s is not allowed in %s %s", name, check_article(), check_label_);
    }
    if (!threads_.count(co) || !lua_isyieldable(co))
        return luaL_error(co, "async call not allowed here: call it from a handler or the top "
                              "level of main.lua, not a metamethod, iterator, sort comparator, "
                              "module top level or plugin-made coroutine");
    start(Pending(this, co, guard_.token()));
    yielded_for_async_ = true;
    return lua_yield(co, 0);
}

void LuaRuntime::Pending::resolve(PushFn push_results) const {
    if (done_->exchange(true))
        return;
    LuaRuntime* rt = rt_;
    lua_State* co = co_;
    token_.defer("plugin_resume",
                 [rt, co, push = std::move(push_results)]() { rt->resume(co, push); });
}

void LuaRuntime::resume(lua_State* co, const PushFn& push_results) {
    if (faulted_ || !threads_.count(co))
        return;
    int n = 0;
    {
        HostWorkGuard host_work(host_work_);
        n = push_results ? push_results(co) : 0;
    }
    // Results are pushed outside any entry, so the cap is enforced here, before resuming.
    if (used_ > limits_.memory_bytes) {
        lua_settop(co, 0);
        drop(co);
        fault(memory_cap_reason(limits_.memory_bytes));
        return;
    }
    enter(co, n);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
