// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_storage.h"

#include "ui_event_safety.h"
#include "ui_format_utils.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::settings {

namespace {

constexpr const char* kCleanButtonPrefix = "storage_clean_";

void on_clean_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[StorageSettingsOverlay] on_clean_clicked");
    const std::size_t prefix_len = std::strlen(kCleanButtonPrefix);
    for (auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e)); obj != nullptr;
         obj = lv_obj_get_parent(obj)) {
        const char* name = lv_obj_get_name(obj);
        if (name != nullptr && std::strncmp(name, kCleanButtonPrefix, prefix_len) == 0) {
            if (const auto index = storage::consumer_index(name + prefix_len)) {
                get_storage_settings_overlay().confirm_cleanup(*index);
            }
            return;
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

std::string usage_text(const storage::ConsumerUsage& usage) {
    if (usage.files == 0) {
        return lv_tr("Empty");
    }
    return helix::ui::format_file_size(static_cast<size_t>(usage.bytes));
}

} // namespace

StorageSettingsOverlay::~StorageSettingsOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
    }
}

void StorageSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(state_, kStateLoading, "storage_state", subjects_);
        UI_MANAGED_SUBJECT_INT(used_pct_, 0, "storage_used_pct", subjects_);
        UI_MANAGED_SUBJECT_STRING(volume_text_, volume_buf_, "", "storage_volume_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(source_text_, source_buf_, "", "storage_source_text", subjects_);
        for (std::size_t i = 0; i < storage::kConsumerCount; ++i) {
            const std::string key = storage::kConsumers[i].key;
            names_[i] = {"storage_" + key + "_value", "storage_" + key + "_shown",
                         "storage_" + key + "_clean"};
            UI_MANAGED_SUBJECT_STRING_N(value_[i], value_buf_[i].data(), value_buf_[i].size(), "",
                                        names_[i][0].c_str(), subjects_);
            UI_MANAGED_SUBJECT_INT(shown_[i], 0, names_[i][1].c_str(), subjects_);
            UI_MANAGED_SUBJECT_INT(clean_[i], 0, names_[i][2].c_str(), subjects_);
        }
    });
}

void StorageSettingsOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_storage_clean_clicked", on_clean_clicked);
}

void StorageSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    // A cleanup in flight refreshes the page when it ends, unless the client
    // it was sent through is gone and its answer with it.
    if (busy_ && service_client_ != get_moonraker_client()) {
        busy_ = false;
    }
    if (!busy_) {
        refresh();
    }
}

void StorageSettingsOverlay::refresh() {
    const PrinterPluginStatusState& plugin = get_printer_state().plugin_status_state();
    source_ = storage::report_source(
        plugin.plugin_feature(storage::kStorageFeature, storage::kStorageMinVersion));
    lv_subject_set_int(&state_, kStateLoading);
    IMoonrakerClient* client = get_moonraker_client();
    if (!service_ || service_client_ != client) {
        service_ = std::make_unique<storage::StorageReportService>(client);
        service_client_ = client;
    }
    const bool may_clean =
        storage::fallback_may_clean(plugin.service_has_helix_plugin(), plugin.features());
    service_->fetch(source_, may_clean,
                    [this, may_clean](storage::ReportSource source,
                                      std::optional<storage::StorageReport> report) {
                        if (!report && source == storage::ReportSource::Plugin) {
                            // The plugin's report failed: measure over the file API, which
                            // cleans only what is safe without the plugin.
                            source_ = storage::ReportSource::FileApi;
                            service_->fetch(source_, may_clean,
                                            [this](storage::ReportSource s,
                                                   std::optional<storage::StorageReport> r) {
                                                show_report(s, r);
                                            });
                            return;
                        }
                        show_report(source, report);
                    });
}

void StorageSettingsOverlay::show_report(storage::ReportSource source,
                                         const std::optional<storage::StorageReport>& report) {
    if (!subjects_initialized_) {
        return;
    }
    if (!report) {
        lv_subject_set_int(&state_, kStateFailed);
        return;
    }
    if (report->total && report->free && *report->total > 0) {
        const uint64_t total = *report->total;
        const uint64_t free = std::min(*report->free, total);
        const std::string text =
            fmt::format(fmt::runtime(lv_tr("{} free of {}")),
                        helix::ui::format_file_size(static_cast<size_t>(free)),
                        helix::ui::format_file_size(static_cast<size_t>(total)));
        lv_subject_copy_string(&volume_text_, text.c_str());
        lv_subject_set_int(&used_pct_, static_cast<int>((total - free) * 100 / total));
    } else {
        lv_subject_copy_string(&volume_text_, lv_tr("Free space unknown"));
        lv_subject_set_int(&used_pct_, 0);
    }
    std::string source_text = source == storage::ReportSource::Plugin
                                  ? lv_tr("Measured by the HelixPrint plugin")
                                  : lv_tr("Measured from the printer's file list");
    if (report->partial) {
        source_text += " ";
        source_text += lv_tr("(too many folders to list them all)");
    }
    lv_subject_copy_string(&source_text_, source_text.c_str());

    for (std::size_t i = 0; i < storage::kConsumerCount; ++i) {
        const storage::ConsumerUsage& usage = report->consumers[i];
        lv_subject_copy_string(&value_[i], usage_text(usage).c_str());
        lv_subject_set_int(&shown_[i], usage.available ? 1 : 0);
        lv_subject_set_int(&clean_[i], usage.cleanable && usage.files > 0 ? 1 : 0);
    }
    lv_subject_set_int(&state_, kStateReady);
    spdlog::debug("[{}] Storage report from the {}", get_name(),
                  source == storage::ReportSource::Plugin ? "plugin" : "file API");
}

void StorageSettingsOverlay::confirm_cleanup(std::size_t index) {
    if (busy_ || index >= storage::kConsumerCount || !service_) {
        return;
    }
    const storage::ConsumerInfo& info = storage::kConsumers[index];
    helix::ui::ConfirmOptions options;
    options.owner_token = lifetime_.token();
    options.destructive = true;
    helix::ui::modal_confirm(
        lv_tr("Clean Up Storage"), lv_tr(info.confirm), ModalSeverity::Warning, lv_tr("Delete"),
        [this, index]() { run_cleanup(index); }, options);
}

void StorageSettingsOverlay::run_cleanup(std::size_t index) {
    if (busy_ || !service_) {
        return;
    }
    busy_ = true;
    spdlog::info("[{}] Cleaning up {}", get_name(), storage::kConsumers[index].key);
    service_->cleanup(source_, index, [this](const storage::CleanupOutcome& outcome) {
        busy_ = false;
        const std::string freed = helix::ui::format_file_size(static_cast<size_t>(outcome.freed));
        switch (storage::cleanup_message(outcome)) {
        case storage::CleanupMessage::Freed:
            ToastManager::instance().show(
                ToastSeverity::SUCCESS,
                fmt::format(fmt::runtime(lv_tr("Freed {}")), freed).c_str());
            break;
        case storage::CleanupMessage::FreedPartly:
            ToastManager::instance().show(
                ToastSeverity::WARNING,
                fmt::format(fmt::runtime(lv_tr("Freed {}, but some files could not be deleted")),
                            freed)
                    .c_str());
            break;
        case storage::CleanupMessage::NothingFreed:
            spdlog::warn("[{}] Cleanup freed nothing ({} errors, refused {})", get_name(),
                         outcome.errors, outcome.refused);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Could not free space"));
            break;
        case storage::CleanupMessage::NothingToFree:
            ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Nothing to clean up"));
            break;
        }
        refresh();
    });
}

} // namespace helix::settings
