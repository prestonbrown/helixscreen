// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lua_include.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace helix::plugin {

/// Counts errors in a sliding window. `record` returns true when this error reaches the
/// threshold.
class ErrorWindow {
  public:
    using Clock = std::chrono::steady_clock;
    ErrorWindow(size_t threshold, std::chrono::seconds window);
    bool record(Clock::time_point now);

  private:
    size_t threshold_;
    std::chrono::seconds window_;
    std::deque<Clock::time_point> times_;
};

/// `<dir>/<path>.lua` then `<dir>/lib/<path>.lua` for a `require` name of [A-Za-z0-9_]
/// segments joined by single dots. Empty for any other name.
std::vector<std::string> require_candidates(const std::string& plugin_dir, const std::string& name);

/// A path inside a plugin: '/'-separated segments of [A-Za-z0-9_.-], where no segment is
/// empty, "." or "..". No leading '/', ends in ".lua", at most 128 bytes.
bool is_plugin_relative_path(std::string_view path);

/// One plugin's Lua state. Main thread only, except `Pending::resolve`.
class LuaRuntime {
  public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        size_t memory_bytes = 2 * 1024 * 1024;
        std::chrono::milliseconds time_budget{50};
    };

    /// Called once, when the runtime faults. The runtime refuses further entries; the owner
    /// destroys it from a later main-loop turn, never from inside this call.
    using FaultHandler = std::function<void(const std::string& reason)>;

    /// Pushes values onto a coroutine and returns how many.
    using PushFn = std::function<int(lua_State*)>;

    /// A suspended async call. Copyable; `resolve` is thread-safe and one-shot.
    class Pending {
      public:
        void resolve(PushFn push_results) const;

      private:
        friend class LuaRuntime;
        Pending(LuaRuntime* rt, lua_State* co, LifetimeToken token);
        LuaRuntime* rt_;
        lua_State* co_;
        LifetimeToken token_;
        std::shared_ptr<std::atomic<bool>> done_;
    };

    LuaRuntime(std::string plugin_id, std::string plugin_dir, Limits limits, FaultHandler on_fault);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    /// The runtime that owns `L` or any of its coroutines.
    static LuaRuntime& from(lua_State* L);

    const std::string& plugin_id() const {
        return plugin_id_;
    }
    const std::string& plugin_dir() const {
        return plugin_dir_;
    }
    lua_State* state() const {
        return L_;
    }
    size_t memory_used() const {
        return used_;
    }
    size_t memory_cap() const {
        return limits_.memory_bytes;
    }
    bool faulted() const {
        return faulted_;
    }
    const std::string& fault_reason() const {
        return fault_reason_;
    }
    LifetimeToken token() const {
        return guard_.token();
    }

    /// Registers one subject observer or printer watch; false when that would exceed
    /// `limit`. The ui and printer bindings share this quota so neither can exhaust it
    /// alone. Counts only grow: both kinds live until the runtime closes.
    bool add_observer_watch(size_t limit) {
        if (observer_watches_ >= limit)
            return false;
        ++observer_watches_;
        return true;
    }

    /// Charges non-Lua memory (a canvas display list) against the same cap alloc()
    /// enforces; false when it does not fit. Pair every reserve with a release.
    bool reserve_external(size_t bytes);
    /// Hands reserved bytes back. Over-release clamps to zero rather than underflowing.
    void release_external(size_t bytes);

    /// Loads text and runs it as a new entry. False if it failed to load or raised.
    bool run_string(const std::string& code, const std::string& chunk_name);
    /// `run_string` on `<plugin_dir>/<relative_path>`.
    bool run_file(const std::string& relative_path);

    /// Registry reference to a copy of the value at `index` of `L`.
    int ref_value(lua_State* L, int index);
    void unref(int ref);

    /// Calls the function behind `fn_ref` as a new entry. No-op once faulted.
    void invoke(int fn_ref, const PushFn& push_args = {});

    /// For a binding: runs `start` with a Pending for the running coroutine `co`, then
    /// yields it. Raises a Lua error when `co` is not an entry coroutine or cannot yield.
    /// Use as `return rt.await_async(L, ...);`.
    int await_async(lua_State* co, const std::function<void(Pending)>& start);

    /// Work that runs before lua_close, in reverse order of registration.
    void on_close(std::function<void()> fn);

    /// Logs a plugin error; the third within 60 s faults the plugin.
    void report_error(const std::string& message);

  private:
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void budget_hook(lua_State* L, lua_Debug* ar);
    static int lua_require(lua_State* L);
    static int lua_print(lua_State* L);

    void install_sandbox();
    bool spawn(const PushFn& push_args);
    bool enter(lua_State* co, int nargs);
    void resume(lua_State* co, const PushFn& push_results);
    void drop(lua_State* co);
    void fault(const std::string& reason);

    static constexpr int kMaxEntryDepth = 8;
    static constexpr int kHookInterval = 10000;

    std::string plugin_id_;
    std::string plugin_dir_;
    Limits limits_;
    FaultHandler on_fault_;
    lua_State* L_ = nullptr;

    size_t used_ = 0;
    int depth_ = 0;     ///< active entries; the memory cap applies only while > 0
    int host_work_ = 0; ///< host-side bookkeeping in progress; the cap is not enforced on it
    Clock::time_point deadline_{};
    bool yielded_for_async_ = false;
    bool killed_ = false;
    bool faulted_ = false;
    std::string fault_reason_;
    ErrorWindow errors_{3, std::chrono::seconds(60)};

    std::unordered_map<lua_State*, int> threads_; ///< live entry coroutine -> registry ref
    size_t observer_watches_ = 0;
    std::vector<std::function<void()>> closers_;
    AsyncLifetimeGuard guard_;
};

} // namespace helix::plugin
