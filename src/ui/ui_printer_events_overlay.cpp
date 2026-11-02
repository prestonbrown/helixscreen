// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_events_overlay.h"

#include "ui_format_utils.h"

#include "app_globals.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <ctime>

namespace helix::ui {

namespace {

std::string row_message(const JournalEntry& e, const JournalRow& row) {
    std::string message = row.message_tag ? lv_tr(row.message_tag) : row.message;
    if (e.filename.empty()) {
        return message;
    }
    return message.empty() ? e.filename : message + " (" + e.filename + ")";
}

std::string row_time(const JournalEntry& e, const JournalRow& row) {
    std::string text = format_modified_date(static_cast<time_t>(e.time));
    if (row.repeat_count > 1) {
        text += " ";
        text += fmt::format(fmt::runtime(lv_tr("(repeated {} times)")), row.repeat_count);
    }
    if (e.recovered) {
        text += " ";
        text += lv_tr("(recovered)");
    }
    return text;
}

} // namespace

PrinterEventsOverlay::~PrinterEventsOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
    }
}

void PrinterEventsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(count_subject_, 0, "printer_events_count", subjects_);
        UI_MANAGED_SUBJECT_STRING(source_subject_, source_buf_, "", "printer_events_source",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(unavailable_subject_, 0, "printer_events_unavailable", subjects_);
    });
}

void PrinterEventsOverlay::before_show() {
    ui_alive_ = true;
}

void PrinterEventsOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh();
}

void PrinterEventsOverlay::refresh() {
    const JournalPlan plan =
        decide_journal_source(get_printer_state().plugin_status_state().plugin_feature(
            kJournalFeatureName, kJournalFeatureMinVersion));
    IMoonrakerClient* client = get_moonraker_client();
    if (plan.source == JournalSource::None) {
        show_entries(JournalSource::None, {});
        return;
    }
    if (plan.source == JournalSource::Screen || client == nullptr) {
        show_entries(JournalSource::Screen, LocalEventJournal::instance().snapshot());
        return;
    }
    const nlohmann::json params = {{"limit", kMaxRows}};
    client->send_jsonrpc(
        "server.helix.journal", params,
        lifetime_.bg_cb("PrinterEventsOverlay::journal",
                        [this](const nlohmann::json& response) {
                            const nlohmann::json* result =
                                json_util::find_member(response, "result");
                            show_entries(JournalSource::Plugin, result
                                                                    ? parse_journal_result(*result)
                                                                    : std::vector<JournalEntry>{});
                        }),
        lifetime_.bg_cb("PrinterEventsOverlay::journal_error",
                        [this](const MoonrakerError& err) {
                            spdlog::warn("[{}] Plugin journal unavailable ({}), showing the "
                                         "screen's own",
                                         get_name(), err.message);
                            show_entries(JournalSource::Screen,
                                         LocalEventJournal::instance().snapshot());
                        }),
        0, /*silent=*/true);
}

void PrinterEventsOverlay::show_entries(JournalSource source, std::vector<JournalEntry> entries) {
    if (!ui_alive_) {
        return;
    }
    entries = journal_view_order(std::move(entries));
    if (entries.size() > kMaxRows) {
        entries.resize(kMaxRows);
    }
    const std::size_t n = entries.size();
    severity_pool_.ensure_size(n);
    title_pool_.ensure_size(n);
    message_pool_.ensure_size(n);
    has_message_pool_.ensure_size(n);
    time_pool_.ensure_size(n);

    for (std::size_t i = 0; i < n; ++i) {
        const JournalEntry& e = entries[i];
        const JournalRow row = describe_journal_entry(e);
        const std::string message = row_message(e, row);
        severity_pool_.set_int(i, static_cast<int>(row.severity));
        title_pool_.set_string(i, lv_tr(row.title_tag));
        message_pool_.set_string(i, message);
        has_message_pool_.set_int(i, message.empty() ? 0 : 1);
        time_pool_.set_string(i, row_time(e, row));
    }

    const char* source_text =
        source == JournalSource::Plugin   ? lv_tr("Recorded by the HelixPrint plugin")
        : source == JournalSource::Screen ? lv_tr("Recorded by this screen while connected")
                                          : "";
    lv_subject_copy_string(&source_subject_, source_text);
    lv_subject_set_int(&unavailable_subject_, source == JournalSource::None ? 1 : 0);
    lv_subject_set_int(&count_subject_, static_cast<int>(n));
    spdlog::debug("[{}] Showing {} event(s)", get_name(), n);
}

void PrinterEventsOverlay::on_ui_destroyed() {
    ui_alive_ = false;
    // The next open's repeat must not bind rows to the slots reclaimed below.
    if (subjects_initialized_) {
        lv_subject_set_int(&count_subject_, 0);
    }
    severity_pool_.reclaim();
    title_pool_.reclaim();
    message_pool_.reclaim();
    has_message_pool_.reclaim();
    time_pool_.reclaim();
}

} // namespace helix::ui
