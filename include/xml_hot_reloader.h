// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <lvgl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class XmlHotReloaderTestAccess;

namespace helix {

/// A subject a component scope only *borrows*: C++ owns the storage (a
/// `static inline lv_subject_t` member, a field of a long-lived manager, …) and
/// handed the scope a pointer via `lv_xml_register_subject()`.
using BorrowedSubject = std::pair<std::string, lv_subject_t*>;

/**
 * @brief Snapshot every borrowed subject registered in a component's scope
 *
 * `lv_xml_component_unregister()` destroys the scope, taking these registrations
 * with it. Re-registering the component from XML alone cannot bring them back —
 * only the C++ that owns the storage knows about them, and it ran once at
 * startup. Without a snapshot/restore the component reloads live but *inert*:
 * every `bind_*` naming a borrowed subject silently resolves to nothing.
 *
 * @param component_name registered component name (e.g. "runout_guidance_modal")
 * @return (name, subject) for each `owned == false` record; empty if no scope
 */
std::vector<BorrowedSubject> snapshot_borrowed_subjects(const char* component_name);

/**
 * @brief Re-register snapshotted borrowed subjects into a component's new scope
 * @param component_name registered component name (scope must already exist)
 * @param borrowed       result of a prior snapshot_borrowed_subjects() call
 * @return number of subjects successfully re-registered
 */
size_t restore_borrowed_subjects(const char* component_name,
                                 const std::vector<BorrowedSubject>& borrowed);

/**
 * @brief Hot-reloads XML components when files change on disk
 *
 * Polls ui_xml/ directories for mtime changes and re-registers modified
 * components via lv_xml_component_unregister + lv_xml_register_component_from_file.
 * Only active when HELIX_HOT_RELOAD=1 environment variable is set.
 *
 * @note Only tracks files present at start() time. Newly created XML files
 *       are not detected — restart the app to pick them up.
 *
 * @threading Polling runs on a dedicated background thread; actual LVGL
 *            operations are marshalled to the main thread via queue_update().
 */
class XmlHotReloader {
  public:
    XmlHotReloader() = default;
    ~XmlHotReloader();

    // Non-copyable
    XmlHotReloader(const XmlHotReloader&) = delete;
    XmlHotReloader& operator=(const XmlHotReloader&) = delete;

    /**
     * @brief Start watching the given directories for XML changes
     * @param xml_dirs Directories to watch (e.g., {"ui_xml", "ui_xml/components"})
     * @param poll_interval_ms How often to check for changes (default 500ms)
     */
    void start(const std::vector<std::string>& xml_dirs, int poll_interval_ms = 500);

    /**
     * @brief Stop the polling thread (blocks until joined)
     */
    void stop();

    /**
     * @brief Check all tracked files for changes and reload any that changed
     *
     * Normally called by the polling thread, but can be called manually for testing.
     */
    void scan_and_reload();

    /// Number of XML files currently being tracked
    /// @note Only safe to call when polling is not running (before start() or after stop())
    size_t tracked_file_count() const {
        return file_mtimes_.size();
    }

    /// Check if a specific absolute path is being tracked
    /// @note Only safe to call when polling is not running (before start() or after stop())
    bool is_tracking(const std::string& abs_path) const {
        return file_mtimes_.count(abs_path) > 0;
    }

    /// Check if the polling thread is running
    bool is_running() const {
        return running_.load();
    }

    /// Callback type for reload events: (component_name, lvgl_path)
    using ReloadCallback = std::function<void(const std::string&, const std::string&)>;

    /// Set a custom reload callback (for testing). If set, bypasses LVGL queue_update.
    void set_reload_callback(ReloadCallback cb) {
        reload_callback_ = std::move(cb);
    }

    /// Callback type for post-reload notifications: (component_name)
    using AfterReloadCallback = std::function<void(const std::string&)>;

    /// Set a callback invoked after successful re-registration (or directly after the test
    /// reload_callback fires). Runs on whatever thread handled the reload — in production
    /// that's the LVGL main thread via queue_update; in tests it's the caller thread.
    void set_after_reload_callback(AfterReloadCallback cb) {
        after_reload_callback_ = std::move(cb);
    }

  private:
    friend class ::XmlHotReloaderTestAccess;
    void poll_loop();
    void initial_scan(const std::vector<std::string>& xml_dirs);

    /// Derive LVGL component name from filename (strip .xml extension)
    static std::string component_name_from_path(const std::filesystem::path& path);

    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    int poll_interval_ms_{500};

    /// The poll thread parks here between scans instead of sleeping, so stop()
    /// wakes it immediately rather than waiting out a whole interval. Every
    /// caller assumes that: the destructor stops the thread during shutdown,
    /// and the tests start with a deliberately long interval to mean "do not
    /// poll on your own, I will drive scan_and_reload() myself".
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    /// Map: absolute file path -> last modification time
    std::unordered_map<std::string, std::filesystem::file_time_type> file_mtimes_;

    /// Map: absolute file path -> LVGL registration path ("A:ui_xml/...")
    std::unordered_map<std::string, std::string> file_to_lvgl_path_;

    /// Map: absolute file path -> the path LayoutManager resolves against, i.e.
    /// the file's location under the watch root with any leading layout-variant
    /// directory stripped ("home_panel.xml", "components/progress_bar.xml").
    std::unordered_map<std::string, std::string> file_to_logical_path_;

    /// Map: absolute file path -> layout-variant directory the file lives in
    /// ("micro", "portrait", …), or "" for the base copy. A file only reloads
    /// while its variant is the one the active layout resolves to.
    std::unordered_map<std::string, std::string> file_to_variant_;

    /// Optional test callback — if set, called instead of LVGL unregister/register
    ReloadCallback reload_callback_;

    AfterReloadCallback after_reload_callback_;
};

} // namespace helix
