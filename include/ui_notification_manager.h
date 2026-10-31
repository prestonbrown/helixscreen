// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "lvgl.h"
#include "subject_managed_panel.h"

/**
 * @brief Active notification status for badge coloring
 */
enum class NotificationStatus {
    NONE,    ///< No active notifications
    INFO,    ///< Info notification active
    WARNING, ///< Warning notification active
    ERROR    ///< Error notification active
};

/**
 * @brief Singleton manager for notification badge and history panel
 *
 * Manages the notification badge in the navbar showing:
 * - Unread notification count
 * - Notification severity color
 * - Badge pulse animation on new notifications
 *
 * Uses LVGL subjects for reactive XML bindings.
 *
 * Usage:
 *   NotificationManager::instance().register_callbacks();  // Before XML creation
 *   NotificationManager::instance().init_subjects();       // Before XML creation
 *   // Create XML...
 *   NotificationManager::instance().init();                // After XML creation
 */
class NotificationManager {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to the NotificationManager singleton
     */
    static NotificationManager& instance();

    /**
     * @brief Death signal for the subjects this NotificationManager owns.
     *
     * Pass to observe_*() from anything that can outlive this object's
     * deinit_subjects(): that path frees every observer node without bumping
     * the ObserverGuard invalidation epoch, so a guard without the token
     * dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    // Non-copyable, non-movable (singleton)
    NotificationManager(const NotificationManager&) = delete;
    NotificationManager& operator=(const NotificationManager&) = delete;
    NotificationManager(NotificationManager&&) = delete;
    NotificationManager& operator=(NotificationManager&&) = delete;

    /**
     * @brief Register notification event callbacks
     *
     * Must be called BEFORE app_layout XML is created so LVGL can find the callbacks.
     */
    void register_callbacks();

    /**
     * @brief Initialize notification subjects for XML reactive bindings
     *
     * Must be called BEFORE app_layout XML is created so XML bindings can find subjects.
     * Registers the following subjects:
     * - notification_count (int: badge count, 0=hidden)
     * - notification_count_text (string: formatted count)
     * - notification_severity (int: 0=info, 1=warning, 2=error)
     * - notification_history_version (int: history revision, bumped on add/clear)
     */
    void init_subjects();

    /**
     * @brief Initialize the notification system
     *
     * Should be called after XML is created.
     */
    void init();

    /**
     * @brief Update notification severity (badge color)
     * @param status New notification status (NONE defaults to INFO color)
     */
    void update_notification(NotificationStatus status);

    /**
     * @brief Update notification unread count badge
     * @param count Number of unread notifications (0 hides badge)
     */
    void update_notification_count(size_t count);

    /**
     * @brief Publish the history revision to the version subject
     *
     * Compares NotificationHistory::version() against the last published value
     * and sets notification_history_version only on a change. Must be called
     * on the LVGL main thread; notification_refresh_from_history() does that.
     */
    void publish_history_version();

    /**
     * @brief Get the history version subject for observer attachment
     *
     * @return Subject pointer, or nullptr before init_subjects()
     */
    lv_subject_t* history_version_subject();

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     */
    void deinit_subjects();

  private:
    /**
     * @brief Animate notification badge with attention pulse
     *
     * Finds the notification_badge widget on active screen and
     * triggers scale pulse animation to draw attention.
     */
    void animate_notification_badge();

    // Private constructor for singleton
    NotificationManager() = default;
    ~NotificationManager() = default;

    // Event callback for notification history button (static to work with LVGL XML API)
    static void notification_history_clicked(lv_event_t* e);

    // ============================================================================
    // Notification State Subjects (drive XML reactive bindings)
    // ============================================================================

    // RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    // Notification badge: count (0 = hidden), text for display, severity for badge color
    lv_subject_t notification_count_subject_{};
    lv_subject_t notification_count_text_subject_{};
    lv_subject_t notification_severity_subject_{}; // 0=info, 1=warning, 2=error
    lv_subject_t notification_history_version_subject_{};

    // Notification count text buffer (for string subject)
    char notification_count_text_buf_[8] = "0";

    // Track notification panel to prevent multiple instances
    helix::ui::WidgetRef notification_panel_obj_;

    // Track previous notification count for pulse animation (only pulse on increase)
    size_t previous_notification_count_ = 0;

    // Last history revision published to notification_history_version
    uint64_t last_published_history_version_ = 0;

    bool subjects_initialized_ = false;
    bool callbacks_registered_ = false;
    bool initialized_ = false;
};

// ============================================================================
// FREE FUNCTIONS (in helix::ui namespace)
// ============================================================================

namespace helix::ui {

/**
 * @brief Register notification event callbacks
 */
void notification_register_callbacks();

/**
 * @brief Initialize notification subjects for XML reactive bindings
 */
void notification_init_subjects();

/**
 * @brief Deinitialize notification subjects for clean shutdown
 */
void notification_deinit_subjects();

/**
 * @brief Initialize the notification system
 */
void notification_manager_init();

/**
 * @brief Update notification severity
 */
void notification_update(NotificationStatus status);

/**
 * @brief Update notification unread count badge
 */
void notification_update_count(size_t count);

/**
 * @brief Recompute both the unread-count badge and its severity color from the
 *        current NotificationHistory in a single pass.
 *
 * Call this AFTER appending an entry to history. It guarantees count and color
 * are derived from the same snapshot, avoiding the off-by-one where severity is
 * computed before the triggering entry exists. Maps the worst unread severity
 * (ERROR > WARNING > INFO; NONE when nothing is unread) to the badge color.
 */
void notification_refresh_from_history();

/**
 * @brief Get the subject that fires when the history revision changes
 *
 * @return Subject pointer, or nullptr before notification_init_subjects()
 */
lv_subject_t* notification_history_version_subject();

} // namespace helix::ui
