// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "storage_report_service.h"
#include "subject_managed_panel.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace helix::settings {

/// Settings > System > Storage: what fills the printer's disk, by consumer,
/// with a cleanup per consumer and the plugin's automatic cleanup switch.
/// Reads the HelixPrint plugin's report when its storage feature is ready,
/// else sums the known directories over the file API (report_source).
class StorageSettingsOverlay : public OverlayBase {
  public:
    /// storage_state values the XML binds
    static constexpr int kStateLoading = 0;
    static constexpr int kStateReady = 1;
    static constexpr int kStateFailed = 2;

    ~StorageSettingsOverlay() override;

    const char* get_name() const override {
        return "Storage";
    }
    const char* xml_component() const override {
        return "settings_storage_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void register_callbacks() override;
    void on_activate() override;

    /// Measure again.
    void refresh();

    /// Publish @p report, read from @p source; nullopt when it could not be read.
    void show_report(storage::ReportSource source,
                     const std::optional<storage::StorageReport>& report);

    /// The Clean up button of consumer @p index: confirm, then clean.
    void confirm_cleanup(std::size_t index);

  private:
    void run_cleanup(std::size_t index);

    SubjectManager subjects_;
    lv_subject_t state_{};
    lv_subject_t used_pct_{};
    lv_subject_t volume_text_{};
    char volume_buf_[96] = {};
    lv_subject_t source_text_{};
    char source_buf_[160] = {};

    std::array<lv_subject_t, storage::kConsumerCount> value_{};
    std::array<std::array<char, 32>, storage::kConsumerCount> value_buf_{};
    std::array<lv_subject_t, storage::kConsumerCount> shown_{};
    std::array<lv_subject_t, storage::kConsumerCount> clean_{};
    /// Subject names, kept for as long as the XML registry refers to them
    std::array<std::array<std::string, 3>, storage::kConsumerCount> names_{};

    std::unique_ptr<storage::StorageReportService> service_;
    /// The client service_ talks to; a printer switch brings a new one
    IMoonrakerClient* service_client_ = nullptr;
    storage::ReportSource source_ = storage::ReportSource::FileApi;
    bool busy_ = false;
};

inline StorageSettingsOverlay& get_storage_settings_overlay() {
    return lazy_global<StorageSettingsOverlay>("StorageSettingsOverlay");
}

} // namespace helix::settings
