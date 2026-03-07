// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief Registry for static panel/overlay instances to ensure proper destruction order
 *
 * Static global panels (g_xxx_panel) are destroyed during exit() -> __cxa_finalize_ranges,
 * which happens AFTER Application::shutdown() returns. By that time, spdlog and other
 * infrastructure may already be destroyed, causing crashes in panel destructors.
 *
 * This registry allows panels to self-register their destruction callbacks during
 * creation. Application::shutdown() calls destroy_all() to destroy panels in reverse
 * creation order while infrastructure (spdlog, LVGL) is still alive.
 *
 * Usage:
 * ```cpp
 * // In get_global_xxx_panel():
 * if (!g_xxx_panel) {
 *     g_xxx_panel = std::make_unique<XxxPanel>();
 *     StaticPanelRegistry::instance().register_destroy("XxxPanel", []() {
 *         spdlog::debug("[XxxPanel] Destroying static instance");
 *         g_xxx_panel.reset();
 *     });
 * }
 * ```
 */
class StaticPanelRegistry {
  public:
    /**
     * @brief Get the singleton instance
     */
    static StaticPanelRegistry& instance();

    /**
     * @brief Check if registry has been destroyed (for static destruction guards)
     */
    static bool is_destroyed();

    /**
     * @brief Check if destroy_all() is currently executing
     *
     * During destroy_all(), callers should skip lv_obj_delete() calls
     * because lv_deinit() (called after destroy_all()) cleans up all
     * LVGL objects. Attempting to delete risks use-after-free.
     */
    static bool is_destroying_all();

    /**
     * @brief Register a destruction callback for a panel
     * @param name Panel name for logging
     * @param destroy_fn Function to call during destroy_all()
     */
    void register_destroy(const char* name, std::function<void()> destroy_fn);

    /**
     * @brief Destroy all registered panels in reverse registration order
     *
     * Called from Application::shutdown() before LVGL deinit.
     * After this call, the registry is cleared but remains usable.
     */
    void destroy_all();

    /**
     * @brief Clear all registered entries without running callbacks
     *
     * Used during soft restart (printer switching) after destroy_all() has run,
     * to ensure no stale entries remain before re-initialization registers new ones.
     */
    void clear();

    /**
     * @brief Get count of registered panels (for testing/debugging)
     */
    size_t count() const {
        return destroyers_.size();
    }

  private:
    StaticPanelRegistry() = default;
    ~StaticPanelRegistry();

    // Non-copyable
    StaticPanelRegistry(const StaticPanelRegistry&) = delete;
    StaticPanelRegistry& operator=(const StaticPanelRegistry&) = delete;

    struct DestroyEntry {
        std::string name;
        std::function<void()> destroy_fn;
    };

    std::vector<DestroyEntry> destroyers_;
    static std::atomic<bool> s_destroying_all_;
};
