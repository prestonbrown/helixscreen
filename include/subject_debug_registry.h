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

namespace helix {

/**
 * @brief Publish @p subject under @p name in the current XML scope and record it
 *        in SubjectDebugRegistry
 *
 * The body of every UI_SUBJECT_INIT_AND_REGISTER_* site, out of line: inlined,
 * the two std::string temporaries the registry takes cost each site ~100 bytes.
 */
void publish_subject(lv_subject_t* subject, const char* name, lv_subject_type_t type,
                     const char* file, int line);

} // namespace helix
