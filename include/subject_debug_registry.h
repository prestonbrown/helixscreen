// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file subject_debug_registry.h
 * @brief Debug registry for LVGL subjects
 *
 * Maps subject pointers to metadata (name, type, file, line) for debugging.
 * Useful for tracing subject updates and diagnosing binding issues.
 *
 * Usage:
 *   SubjectDebugRegistry::instance().register_subject(&subject, "name", type, __FILE__, __LINE__);
 *   auto* info = SubjectDebugRegistry::instance().lookup(&subject);
 */

#pragma once

#include <lvgl.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h" // CONFIG_HELIX_SUBJECT_DEBUG_STRIP
#endif

/// 1 when the subject macros record every subject in SubjectDebugRegistry.
/// Only an ESP-IDF build can turn it off: the knob is a Kconfig option, read
/// behind ESP_PLATFORM, so no desktop or Linux build can see it set.
#if defined(ESP_PLATFORM) && CONFIG_HELIX_SUBJECT_DEBUG_STRIP
#define HELIX_SUBJECT_DEBUG_REGISTRY_ENABLED 0
#else
#define HELIX_SUBJECT_DEBUG_REGISTRY_ENABLED 1
#endif

/// Records a subject in SubjectDebugRegistry from a registration macro, with
/// the macro's call site as the file and line. Expands to nothing when the
/// registry is compiled out, so the name and __FILE__ never become strings.
#if HELIX_SUBJECT_DEBUG_REGISTRY_ENABLED
#define HELIX_SUBJECT_DEBUG_REGISTER(subject_ptr, name, type)                                      \
    SubjectDebugRegistry::instance().register_subject((subject_ptr), (name), (type), __FILE__,     \
                                                      __LINE__)
#else
#define HELIX_SUBJECT_DEBUG_REGISTER(subject_ptr, name, type) ((void)0)
#endif

/**
 * @brief Debug information for a registered subject
 */
struct SubjectDebugInfo {
    std::string name;       ///< Human-readable name for the subject
    lv_subject_type_t type; ///< LVGL subject type
    std::string file;       ///< Source file where subject was registered
    int line;               ///< Line number where subject was registered
};

/**
 * @brief Registry mapping LVGL subject pointers to debug metadata
 *
 * Singleton that stores debug information for subjects. Useful for debugging
 * subject binding issues and tracing value updates.
 */
class SubjectDebugRegistry {
  public:
    /**
     * @brief Get the singleton instance
     * @return Reference to the global registry
     */
    static SubjectDebugRegistry& instance();

    /**
     * @brief Register a subject with debug metadata
     * @param subject Pointer to the LVGL subject
     * @param name Human-readable name for the subject
     * @param type LVGL subject type
     * @param file Source file (__FILE__)
     * @param line Line number (__LINE__)
     *
     * Re-registering the same pointer updates the existing entry.
     */
    void register_subject(lv_subject_t* subject, const std::string& name, lv_subject_type_t type,
                          const std::string& file, int line);

    /**
     * @brief Drop a subject's debug entry before its storage is freed
     *
     * Without this the registry keeps a key pointing at freed memory, and
     * lookup_by_name() can hand back a dead subject when a later instance
     * republishes the same name from a different address.
     *
     * @param subject Pointer to the subject being torn down (null is ignored)
     */
    void unregister_subject(lv_subject_t* subject);

    /**
     * @brief Look up debug info for a subject
     * @param subject Pointer to the LVGL subject
     * @return Pointer to debug info, or nullptr if not registered
     */
    const SubjectDebugInfo* lookup(lv_subject_t* subject);

    /**
     * @brief Get human-readable name for a subject type
     * @param type LVGL subject type
     * @return String representation (e.g., "INT", "STRING", "POINTER")
     */
    static std::string type_name(lv_subject_type_t type);

    /**
     * @brief Log all registered subjects
     *
     * Dumps all registered subjects to the log at DEBUG level.
     */
    void dump_all_subjects();

    /**
     * @brief Look up a subject by its registered name
     * @param name The human-readable name used during registration
     * @return Pointer to the subject, or nullptr if not found
     */
    lv_subject_t* lookup_by_name(const std::string& name);

    /**
     * @brief List all registered subjects with their debug info
     * @return Vector of (name, info) pairs
     */
    std::vector<std::pair<std::string, SubjectDebugInfo>> list_all();

    /**
     * @brief Clear all registrations
     *
     * Primarily for testing. Removes all registered subjects.
     */
    void clear();

  private:
    // Defined out-of-line: they maintain the liveness flag that makes a late
    // unregister_subject() during static destruction a no-op rather than an abort.
    SubjectDebugRegistry();
    ~SubjectDebugRegistry();

    SubjectDebugRegistry(const SubjectDebugRegistry&) = delete;
    SubjectDebugRegistry& operator=(const SubjectDebugRegistry&) = delete;

    std::unordered_map<lv_subject_t*, SubjectDebugInfo> subjects_;
    std::unordered_map<std::string, lv_subject_t*> name_to_subject_; ///< Reverse lookup by name
    mutable std::mutex mutex_;                                       ///< Protects both maps
};
