// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "core/timer_guard.h"
#include "firmware_update.h"
#include "firmware_update_runner.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "update_manager_status.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;
}

namespace helix::settings {

/// Settings > System > Firmware Update: the printer's own firmware image from a USB
/// stick (when this printer's fwupdate row may apply), and every Moonraker
/// update_manager entry with Update, plus a progress view for the one running.
/// Every start goes through a confirm and a fresh print_stats check.
class FirmwareUpdateOverlay : public OverlayBase {
  public:
    static constexpr int kViewList = 0;
    static constexpr int kViewProgress = 1;
    static constexpr int kListLoading = 0;
    static constexpr int kListReady = 1;
    static constexpr int kListFailed = 2;
    static constexpr int kRunRunning = 0;
    static constexpr int kRunDone = 1;
    static constexpr int kRunFailed = 2;
    /// Ended without a verdict (the flasher vanished or went silent): check the printer
    static constexpr int kRunUnknown = 3;
    static constexpr std::size_t kMaxEntries = 12;
    static constexpr std::size_t kMaxImages = 3;

    ~FirmwareUpdateOverlay() override;

    const char* get_name() const override {
        return "Firmware Update";
    }
    const char* xml_component() const override {
        return "settings_firmware_update_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void register_callbacks() override;
    void on_activate() override;

    /// Where removable media are mounted, read at each scan for firmware images.
    using ImageRoots = std::function<std::vector<std::string>()>;
    void set_image_roots(ImageRoots roots) {
        image_roots_ = std::move(roots);
    }
    /// What launches a vendor flasher; helix::run_process unless replaced (tests
    /// hand in a fake, so no test ever runs a flasher).
    void set_process_runner(ProcessRunner runner) {
        runner_ = std::move(runner);
    }

    /// Re-read update_manager (machine.update.refresh first when @p remote) and the
    /// USB sticks.
    void refresh(bool remote);

    /// The Update button of update_manager entry @p index.
    void request_entry_update(std::size_t index);
    /// The Install button of USB image @p index.
    void request_image_install(std::size_t index);
    /// The progress view's Done button.
    void dismiss_progress();

  private:
    struct Job {
        enum class Kind { Managed, Vendor } kind = Kind::Managed;
        std::string name; ///< update_manager entry, or the image's file name
    };

    /// Which firmware row this printer has, and whether its part of the page shows.
    void update_provider();
    void show_status(const std::optional<fwupdate::UpdateStatus>& status);
    void scan_images();
    /// @p destructive: the confirm button carries the danger style.
    void confirm_and_start(Job job, std::string message, bool destructive);
    /// Re-query print_stats, then start @p job if nothing blocks.
    void check_and_start(Job job);
    void start_managed(const std::string& name);
    void start_vendor(const fwupdate::FileEntry& image, fwupdate::ApplyInputs in);
    void on_update_response(const fwupdate::UpdateResponse& r);
    void show_running(const std::string& title);
    /// Contact with Moonraker dropped mid-update: wait for update_manager to say.
    void lost_contact();
    void poll_after_lost_contact();
    enum class RunEnd { Done, Failed, Unknown };
    void finish(RunEnd end, const std::string& message);
    void set_progress(const std::string& message, std::optional<int> percent);
    void poll_vendor_log();
    void unregister_response_handler();
    void toast_blocker(fwupdate::Blocker b);
    void publish_busy();

    SubjectManager subjects_;
    helix::Subject view_{};
    helix::Subject list_state_{};
    helix::Subject vendor_shown_{};
    helix::Subject vendor_status_{};
    char vendor_status_buf_[192] = {};
    helix::Subject vendor_note_{};
    char vendor_note_buf_[256] = {};
    helix::Subject entry_count_{};
    std::array<helix::Subject, kMaxEntries> entry_name_{};
    std::array<std::array<char, 48>, kMaxEntries> entry_name_buf_{};
    std::array<helix::Subject, kMaxEntries> entry_detail_{};
    std::array<std::array<char, 96>, kMaxEntries> entry_detail_buf_{};
    std::array<helix::Subject, kMaxEntries> entry_can_update_{};
    std::array<std::array<std::string, 3>, kMaxEntries> entry_names_{};
    helix::Subject image_count_{};
    std::array<helix::Subject, kMaxImages> image_name_{};
    std::array<std::array<char, 96>, kMaxImages> image_name_buf_{};
    std::array<std::string, kMaxImages> image_subject_names_{};
    helix::Subject busy_{};
    helix::Subject run_state_{};
    helix::Subject run_title_{};
    char run_title_buf_[96] = {};
    helix::Subject run_message_{};
    char run_message_buf_[192] = {};
    helix::Subject run_pct_{};
    helix::Subject run_has_pct_{};
    helix::Subject run_no_return_{};

    ImageRoots image_roots_;
    ProcessRunner runner_ = helix::run_process;
    const fwupdate::Provider* provider_ = nullptr;
    std::vector<fwupdate::ManagedEntry> entries_;
    std::vector<fwupdate::FileEntry> images_;
    bool update_manager_busy_ = false;

    std::optional<Job> running_;
    /// A confirmed start waiting for its print_stats answer
    bool start_pending_ = false;
    /// The running update_manager job reported an error line
    bool job_failed_ = false;
    /// The client the running update_manager job's handler is registered on
    IMoonrakerClient* job_client_ = nullptr;
    std::unique_ptr<fwupdate::LogTail> vendor_log_;
    std::string vendor_pid_path_;
    bool reached_no_return_ = false;
    std::string last_vendor_line_;
    std::chrono::steady_clock::time_point last_vendor_line_at_{};
    helix::ui::LvglTimerGuard vendor_timer_;
    helix::ui::LvglTimerGuard contact_timer_;
    bool contact_query_out_ = false;
};

FirmwareUpdateOverlay& get_firmware_update_overlay();

} // namespace helix::settings
