// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file subject_managed_panel.h
 * @brief RAII helper for automatic subject deinitialization in panels
 *
 * SubjectManager provides automatic cleanup for LVGL subjects registered with panels.
 * Panels register their subjects during init_subjects(), and the manager automatically
 * calls lv_subject_deinit() on all registered subjects when destroyed.
 *
 * @pattern RAII (Resource Acquisition Is Initialization)
 * @threading Main thread only (LVGL is not thread-safe)
 *
 * ## Usage Pattern:
 *
 * @code
 * class MyPanel : public PanelBase {
 * public:
 *     void init_subjects() override {
 *         if (subjects_initialized_) return;
 *
 *         // Initialize and register subjects with the manager
 *         lv_subject_init_int(&my_count_, 0);
 *         subjects_.register_subject(&my_count_);
 *
 *         lv_subject_init_string(&my_label_, label_buf_, nullptr, sizeof(label_buf_), "");
 *         subjects_.register_subject(&my_label_);
 *
 *         subjects_initialized_ = true;
 *     }
 *
 *     ~MyPanel() {
 *         // subjects_.deinit_all() called automatically, OR:
 *         subjects_.deinit_all();  // Explicit call if destructor does other work first
 *     }
 *
 * private:
 *     SubjectManager subjects_;  // RAII - auto-deinits on destruction
 *     lv_subject_t my_count_;
 *     lv_subject_t my_label_;
 *     char label_buf_[64];
 * };
 * @endcode
 *
 * ## Integration with UI_SUBJECT_INIT_AND_REGISTER_* Macros:
 *
 * The existing macros can be combined with SubjectManager:
 *
 * @code
 * UI_SUBJECT_INIT_AND_REGISTER_INT(my_count_, 0, "my_count");
 * subjects_.register_subject(&my_count_);
 * @endcode
 *
 * ## Thread Safety:
 *
 * SubjectManager is NOT thread-safe. All calls must happen on the main (LVGL) thread.
 * This matches LVGL's threading model.
 *
 * @see PanelBase for base class with subjects_initialized_ flag
 * @see OverlayBase for overlay base class with same pattern
 */

#pragma once

#include "helix/xml/scoped_subject_registry.h"
#include "lvgl/lvgl.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

/**
 * @class SubjectManager
 * @brief RAII container for automatic LVGL subject cleanup
 *
 * Tracks registered lv_subject_t pointers and deinitializes them all in destructor.
 * Guards against double-deinit by clearing the list after deinitialization.
 *
 * The manager tracks whether subjects were properly deinitialized during application
 * shutdown (when LVGL is still running). If so, any remaining subjects during static
 * destruction are silently cleared without warning, as they represent benign re-use
 * of the manager rather than a cleanup failure.
 */
class SubjectManager {
  public:
    /**
     * @brief Default constructor
     */
    SubjectManager() = default;

    /**
     * @brief Destructor - automatically deinitializes all registered subjects
     */
    ~SubjectManager() {
        deinit_all();
    }

    // Non-copyable (subject ownership is unique)
    SubjectManager(const SubjectManager&) = delete;
    SubjectManager& operator=(const SubjectManager&) = delete;

    // Movable (transfers subject ownership)
    SubjectManager(SubjectManager&& other) noexcept
        : subjects_(std::move(other.subjects_)), subject_names_(std::move(other.subject_names_)) {
        other.subjects_.clear();
        other.subject_names_.clear();
    }

    SubjectManager& operator=(SubjectManager&& other) noexcept {
        if (this != &other) {
            deinit_all();
            subjects_ = std::move(other.subjects_);
            subject_names_ = std::move(other.subject_names_);
            other.subjects_.clear();
            other.subject_names_.clear();
        }
        return *this;
    }

    /**
     * @brief Register a subject for automatic cleanup
     *
     * Call this after lv_subject_init_*() to ensure the subject is
     * deinitialized when this SubjectManager is destroyed.
     *
     * @param subject Pointer to initialized lv_subject_t (must not be null)
     *
     * @note Null pointers are safely ignored with a warning log
     * @note Duplicate registrations are ignored (no double-deinit)
     */
    /// @param xml_name The name this subject was published under in the XML scope,
    ///        so deinit_all() can withdraw it. Kept here rather than looked up from
    ///        SubjectDebugRegistry: deinit_all() can run during static destruction,
    ///        where touching that singleton's std::mutex aborts the process.
    void register_subject(lv_subject_t* subject, const char* xml_name = nullptr) {
        if (!subject) {
            spdlog::warn("[SubjectManager] Attempted to register null subject");
            return;
        }

        // Check for duplicates
        for (const auto* s : subjects_) {
            if (s == subject) {
                spdlog::warn("[SubjectManager] Subject already registered, ignoring duplicate");
                return;
            }
        }

        subjects_.push_back(subject);
        subject_names_.emplace_back(xml_name ? xml_name : "");
    }

    /**
     * @brief Deinitialize all registered subjects
     *
     * Called automatically by destructor. Can also be called manually
     * for explicit cleanup ordering (e.g., before lv_deinit()).
     *
     * Safe to call multiple times - subsequent calls are no-ops.
     *
     * @note Checks lv_is_initialized() to handle static destruction order safely
     * @note Never logs. A SubjectManager owned by a static reaches this through
     *       the C++ atexit chain, and spdlog's registry - a lazily built
     *       function-local static - is torn down before any object created
     *       during dynamic initialization, so a log call here reads a freed
     *       logger. An app that ran Application::shutdown() arrives with an
     *       empty subjects_ and returns above; a binary that never does (every
     *       unit test) takes the full path.
     * @note Each subject's XML-scope name is withdrawn before the subject is freed,
     *       so a name can never outlive the storage it resolves to. Owners that do
     *       not live for the whole process (a panel held by a stack-allocated test
     *       fixture, a torn-down panel) depend on this: without it the registry
     *       keeps handing out a pointer to freed memory and the next lv_xml_create()
     *       binding that name reads it. Panels are still destroyed via
     *       StaticPanelRegistry BEFORE lv_deinit(); do not destroy them after.
     */
    void deinit_all() {
        if (subjects_.empty()) {
            return;
        }

        // LVGL is gone, so the subjects cannot be deinitialized. Drop the
        // bookkeeping and let their storage die with the manager.
        if (!lv_is_initialized()) {
            subjects_.clear();
            subject_names_.clear();
            return;
        }

        for (size_t i = 0; i < subjects_.size(); ++i) {
            auto* subject = subjects_[i];
            if (!subject) {
                continue;
            }
            // Drop the XML-scope name BEFORE freeing the subject. The managed
            // macros publish every subject into the process-wide XML registry,
            // which keeps resolving the name after lv_subject_deinit() — so an
            // owner that does not outlive the process (a panel held by a
            // stack-allocated test fixture, a torn-down panel) leaves the name
            // pointing at dead memory, and the next lv_xml_create() that binds
            // it reads freed storage. Unregistering turns that into a clean
            // "subject not found" instead.
            if (i < subject_names_.size() && !subject_names_[i].empty()) {
                helix::xml::unregister_subject_in_current_scope(subject_names_[i].c_str());
            }
            // Same reasoning for the debug registry, which is also keyed by a
            // pointer that is about to dangle. Safe during static destruction:
            // unregister_subject() checks a liveness flag that outlives the
            // singleton, so a late call is a no-op rather than a locked-destroyed-
            // mutex abort.
            SubjectDebugRegistry::instance().unregister_subject(subject);
            lv_subject_deinit(subject);
        }

        subjects_.clear();
        subject_names_.clear();
    }

    /**
     * @brief Get count of registered subjects
     * @return Number of registered subjects
     */
    size_t count() const {
        return subjects_.size();
    }

    /**
     * @brief Check if any subjects are registered
     * @return true if at least one subject is registered
     */
    bool has_subjects() const {
        return !subjects_.empty();
    }

  private:
    std::vector<lv_subject_t*> subjects_;
    /// XML-scope name per entry in subjects_, same index. Empty when a subject was
    /// registered without one (register_subject() called directly, not via a macro).
    std::vector<std::string> subject_names_;
};

/**
 * @brief Helper macro to init, register with XML system, AND register with SubjectManager
 *
 * Combines UI_SUBJECT_INIT_AND_REGISTER_INT with SubjectManager registration.
 *
 * @param subject lv_subject_t member variable
 * @param initial_value Initial integer value
 * @param xml_name String name for XML binding
 * @param manager SubjectManager instance
 */
#define UI_MANAGED_SUBJECT_INT(subject, initial_value, xml_name, manager)                          \
    do {                                                                                           \
        lv_subject_init_int(&(subject), (initial_value));                                          \
        helix::xml::register_subject_in_current_scope((xml_name), &(subject));                     \
        (manager).register_subject(&(subject), (xml_name));                                        \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (xml_name), LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);                      \
    } while (0)

/**
 * @brief Helper macro to init, register with XML system, AND register with SubjectManager
 *
 * Combines UI_SUBJECT_INIT_AND_REGISTER_STRING with SubjectManager registration.
 *
 * @param subject lv_subject_t member variable
 * @param buffer Character buffer for string storage
 * @param initial_value Initial string value
 * @param xml_name String name for XML binding
 * @param manager SubjectManager instance
 */
#define UI_MANAGED_SUBJECT_STRING(subject, buffer, initial_value, xml_name, manager)               \
    do {                                                                                           \
        lv_subject_init_string(&(subject), (buffer), nullptr, sizeof(buffer), (initial_value));    \
        helix::xml::register_subject_in_current_scope((xml_name), &(subject));                     \
        (manager).register_subject(&(subject), (xml_name));                                        \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (xml_name), LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);                   \
    } while (0)

/**
 * @brief Helper macro to init, register with XML system, AND register with SubjectManager
 *
 * Variant with explicit size for use with std::array<char, N>::data() where sizeof
 * would return pointer size instead of buffer size.
 *
 * @param subject lv_subject_t member variable
 * @param buffer Character buffer pointer (e.g., buf.data())
 * @param size Buffer size in bytes (e.g., buf.size())
 * @param initial_value Initial string value
 * @param xml_name String name for XML binding
 * @param manager SubjectManager instance
 */
#define UI_MANAGED_SUBJECT_STRING_N(subject, buffer, size, initial_value, xml_name, manager)       \
    do {                                                                                           \
        snprintf((buffer), (size), "%s", (initial_value));                                         \
        lv_subject_init_string(&(subject), (buffer), nullptr, (size), (buffer));                   \
        helix::xml::register_subject_in_current_scope((xml_name), &(subject));                     \
        (manager).register_subject(&(subject), (xml_name));                                        \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (xml_name), LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);                   \
    } while (0)

/**
 * @brief Helper macro to init, register with XML system, AND register with SubjectManager
 *
 * Combines UI_SUBJECT_INIT_AND_REGISTER_POINTER with SubjectManager registration.
 *
 * @param subject lv_subject_t member variable
 * @param initial_value Initial pointer value (can be nullptr)
 * @param xml_name String name for XML binding
 * @param manager SubjectManager instance
 */
#define UI_MANAGED_SUBJECT_POINTER(subject, initial_value, xml_name, manager)                      \
    do {                                                                                           \
        lv_subject_init_pointer(&(subject), (initial_value));                                      \
        helix::xml::register_subject_in_current_scope((xml_name), &(subject));                     \
        (manager).register_subject(&(subject), (xml_name));                                        \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (xml_name), LV_SUBJECT_TYPE_POINTER, __FILE__, __LINE__);                  \
    } while (0)

/**
 * @brief Helper macro to init, register with XML system, AND register with SubjectManager
 *
 * Combines UI_SUBJECT_INIT_AND_REGISTER_COLOR with SubjectManager registration.
 *
 * @param subject lv_subject_t member variable
 * @param initial_value Initial lv_color_t value
 * @param xml_name String name for XML binding
 * @param manager SubjectManager instance
 */
#define UI_MANAGED_SUBJECT_COLOR(subject, initial_value, xml_name, manager)                        \
    do {                                                                                           \
        lv_subject_init_color(&(subject), (initial_value));                                        \
        helix::xml::register_subject_in_current_scope((xml_name), &(subject));                     \
        (manager).register_subject(&(subject), (xml_name));                                        \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (xml_name), LV_SUBJECT_TYPE_COLOR, __FILE__, __LINE__);                    \
    } while (0)
