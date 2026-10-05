// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Registry for per-printer cache invalidation when the active printer changes
 *
 * HelixScreen keeps per-printer settings under `/printers/<active_id>/…`, reached by
 * prefixing a key with `Config::df()`. Switching the active printer repoints `df()` at a
 * different subtree, so any component that *memoized* a `df()`-derived value — a member
 * loaded once behind a `loaded_` guard, a map keyed by panel/page, a lazily-built list —
 * keeps serving the PREVIOUS printer's data until something explicitly drops it.
 *
 * That is what #804 was: `PanelWidgetManager` cached one `PanelWidgetConfig` per panel and
 * `load()` became a no-op once loaded, so Home rendered the old printer's widget layout
 * after a switch. The fix was a single hardcoded `clear_all_panel_configs()` call in
 * `PrinterSession::switch_printer()` — which fixed that one component and left every future
 * one to be remembered by hand.
 *
 * This registry makes the invalidation structural instead. A component that caches
 * `df()`-derived state self-registers a callback that drops it; every active-printer change
 * fires all of them.
 *
 * ## Relationship to StaticSubjectRegistry
 *
 * Deliberately the same shape (`instance()` / `register_*` / `*_all()` / `*_one()` /
 * `clear()` / `count()` / `is_destroyed()`) so the two read as siblings. Three behaviours
 * differ, and each difference is forced by what the registry is for:
 *
 * 1. **Entries persist across `invalidate_all()`.** `StaticSubjectRegistry::deinit_all()`
 *    detaches its entries because the subject sources it tore down re-register when they
 *    are re-created. Here nothing is destroyed — the caches' owners are process-lifetime
 *    singletons — and a user may switch printers any number of times, so every switch must
 *    fire every invalidator.
 * 2. **Registration is idempotent by name.** Because entries persist, a component whose
 *    `init_subjects()` runs again after a soft restart would otherwise accumulate a
 *    duplicate on every switch. `register_invalidator()` replaces an existing entry with
 *    the same name (keeping its original position).
 * 3. **Callbacks fire in registration order, not reverse.** `deinit_all()` is LIFO because
 *    destruction order matters. Dropping cached state has no such dependency — invalidators
 *    must not care about each other — so FIFO is used for the more obvious semantics.
 *
 * A throwing invalidator is caught and logged rather than allowed to skip the rest of the
 * list: one component's bad cleanup silently leaving another component stale is exactly the
 * failure mode this registry exists to prevent.
 *
 * ## Self-Registration Pattern (MANDATORY)
 *
 * Register from the component's own init, next to the code that populates the cache, so the
 * two cannot drift apart:
 *
 * ```cpp
 * void MyManager::init() {
 *     if (initialized_) return;
 *     // ... build caches from Config::df() ...
 *     initialized_ = true;
 *
 *     PrinterCacheRegistry::instance().register_invalidator(
 *         "MyManager", []() { MyManager::instance().clear_cached_config(); });
 * }
 * ```
 *
 * A component with a bounded lifetime (a non-singleton that can be destroyed while the
 * registry lives on — e.g. a test fixture's instance) must call `unregister()` from its
 * teardown so the registry never holds a callback over freed memory.
 *
 * Main-thread only. `PrinterSession::switch_printer()` and the add-printer wizard paths call
 * `invalidate_all()` after `Config::set_active_printer()` and before
 * `tear_down_printer_state()`, i.e. while `df()` already points at the NEW printer.
 */
class PrinterCacheRegistry {
  public:
    /**
     * @brief Get the singleton instance
     */
    static PrinterCacheRegistry& instance();

    /**
     * @brief Check if registry has been destroyed (for static destruction guards)
     */
    static bool is_destroyed();

    /**
     * @brief Register a per-printer cache invalidation callback
     *
     * Idempotent by name: registering a name that is already present replaces its callback
     * in place, keeping the original ordering. This is what makes it safe to register from
     * an init path that runs again after every soft restart.
     *
     * @param name Component name, used for logging, replacement and unregister()
     * @param invalidate_fn Function that drops the component's df()-derived cache
     */
    void register_invalidator(const char* name, std::function<void()> invalidate_fn);

    /**
     * @brief Remove a registration without running it
     *
     * For components that can be destroyed while the registry outlives them. Call from the
     * same teardown that tears down whatever the callback touches.
     *
     * @param name Name used at registration
     * @return true if an entry was found and removed
     */
    bool unregister(const char* name);

    /**
     * @brief Fire every registered invalidator, in registration order
     *
     * Registrations are retained — the next printer switch fires them again. Safe to call
     * with no registrations. An invalidator that throws is logged and skipped; the
     * remaining invalidators still run.
     */
    void invalidate_all();

    /**
     * @brief Run a single registered invalidator by name, keeping it registered
     *
     * Test hook, and useful for targeted invalidation. Unlike
     * StaticSubjectRegistry::deinit_one() the entry is NOT consumed — invalidation is
     * repeatable by design.
     *
     * @param name Name used at registration
     * @return true if an entry was found and run
     */
    bool invalidate_one(const char* name);

    /**
     * @brief Drop all registrations without running them (test isolation)
     */
    void clear();

    /**
     * @brief Get count of registered invalidators (for testing/debugging)
     */
    [[nodiscard]] size_t count() const {
        return invalidators_.size();
    }

    /**
     * @brief Registered names in firing order (for testing/debugging)
     */
    [[nodiscard]] std::vector<std::string> names() const;

  private:
    PrinterCacheRegistry() = default;
    ~PrinterCacheRegistry();

    // Non-copyable
    PrinterCacheRegistry(const PrinterCacheRegistry&) = delete;
    PrinterCacheRegistry& operator=(const PrinterCacheRegistry&) = delete;

    struct InvalidateEntry {
        std::string name;
        std::function<void()> invalidate_fn;
    };

    std::vector<InvalidateEntry> invalidators_;
};

} // namespace helix
