// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_screws_tilt_share_modal.h"

#include "screws_tilt_share_text.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <lvgl.h>

namespace helix::ui {

namespace {

constexpr size_t kNameBufSize = 40;
constexpr size_t kZBufSize = 24;
constexpr size_t kAdjBufSize = 24;

/**
 * @brief Row subjects backing the modal's <repeat> expansion
 *
 * Deliberately process-wide rather than per-instance. Modal teardown deletes
 * the widget tree asynchronously (exit animation + deferred delete) while the
 * owning instance's deletion is itself deferred a tick - so instance-owned
 * subjects would be deinit'd while bound labels still exist.
 * Registering once and tearing down via StaticSubjectRegistry (which runs
 * before lv_deinit()) removes that ordering hazard entirely.
 */
struct ShareSubjects {
    lv_subject_t count{};
    std::array<lv_subject_t, ScrewsTiltShareModal::kMaxRows> name{};
    std::array<lv_subject_t, ScrewsTiltShareModal::kMaxRows> z{};
    std::array<lv_subject_t, ScrewsTiltShareModal::kMaxRows> adj{};

    char name_bufs[ScrewsTiltShareModal::kMaxRows][kNameBufSize] = {};
    char z_bufs[ScrewsTiltShareModal::kMaxRows][kZBufSize] = {};
    char adj_bufs[ScrewsTiltShareModal::kMaxRows][kAdjBufSize] = {};

    SubjectManager manager;
    bool initialized = false;
};

ShareSubjects& share_subjects() {
    static ShareSubjects s;
    return s;
}

void deinit_share_subjects() {
    auto& s = share_subjects();
    if (!s.initialized) {
        return;
    }
    s.manager.deinit_all();
    s.initialized = false;
    spdlog::debug("[ScrewsTiltShare] Subjects deinitialized");
}

void init_share_subjects() {
    auto& s = share_subjects();
    if (s.initialized) {
        return;
    }

    UI_MANAGED_SUBJECT_INT(s.count, 0, "screws_share_count", s.manager);

    for (size_t i = 0; i < ScrewsTiltShareModal::kMaxRows; i++) {
        char name_key[40];
        char z_key[40];
        char adj_key[40];
        std::snprintf(name_key, sizeof(name_key), "screws_share_%zu_name", i);
        std::snprintf(z_key, sizeof(z_key), "screws_share_%zu_z", i);
        std::snprintf(adj_key, sizeof(adj_key), "screws_share_%zu_adj", i);

        UI_MANAGED_SUBJECT_STRING_N(s.name[i], s.name_bufs[i], kNameBufSize, "", name_key,
                                    s.manager);
        UI_MANAGED_SUBJECT_STRING_N(s.z[i], s.z_bufs[i], kZBufSize, "", z_key, s.manager);
        UI_MANAGED_SUBJECT_STRING_N(s.adj[i], s.adj_bufs[i], kAdjBufSize, "", adj_key, s.manager);
    }

    s.initialized = true;

    // Self-register cleanup so subjects die before lv_deinit() (mandatory
    // pattern — see StaticSubjectRegistry docs).
    StaticSubjectRegistry::instance().register_deinit("ScrewsTiltShareModal",
                                                      []() { deinit_share_subjects(); });

    spdlog::debug("[ScrewsTiltShare] Subjects initialized ({} rows)",
                  ScrewsTiltShareModal::kMaxRows);
}

/// Push one result set into the row subjects; returns the row count rendered.
int publish_rows(const std::vector<ScrewTiltResult>& results) {
    auto& s = share_subjects();
    const size_t rows = std::min(results.size(), ScrewsTiltShareModal::kMaxRows);

    for (size_t i = 0; i < ScrewsTiltShareModal::kMaxRows; i++) {
        if (i >= rows) {
            lv_subject_copy_string(&s.name[i], "");
            lv_subject_copy_string(&s.z[i], "");
            lv_subject_copy_string(&s.adj[i], "");
            continue;
        }

        const auto& screw = results[i];
        char z_text[kZBufSize];
        std::snprintf(z_text, sizeof(z_text), "Z %s", format_screw_share_z(screw).c_str());

        lv_subject_copy_string(&s.name[i], screw.display_name().c_str());
        lv_subject_copy_string(&s.z[i], z_text);
        lv_subject_copy_string(&s.adj[i],
                               format_screw_share_adjustment(screw, lv_tr("Base")).c_str());
    }

    // Set the count LAST: it drives the <repeat> expansion, and the rebuilt
    // rows read the row subjects as they are created.
    lv_subject_set_int(&s.count, static_cast<int>(rows));
    return static_cast<int>(rows);
}

} // namespace

ScrewsTiltShareModal::ScrewsTiltShareModal(std::vector<ScrewTiltResult> results)
    : results_(std::move(results)), share_text_(build_screws_tilt_share_text(results_)) {}

bool ScrewsTiltShareModal::show_modal(lv_obj_t* parent) {
    init_share_subjects();
    const int rows = publish_rows(results_);
    spdlog::debug("[ScrewsTiltShare] Showing {} of {} screws, payload {} bytes", rows,
                  results_.size(), share_text_.size());
    return show(parent);
}

void ScrewsTiltShareModal::on_show() {
    wire_ok_button("btn_ok");
    create_qr_code();
}

void ScrewsTiltShareModal::create_qr_code() {
#if LV_USE_QRCODE
    auto* container = find_widget("qr_container");
    if (!container) {
        spdlog::warn("[ScrewsTiltShare] qr_container not found");
        return;
    }

    // Adaptive QR size: proportion of dialog width, clamped to a scannable
    // range (same approach as InfoQrModal).
    lv_obj_update_layout(dialog());
    const int32_t dialog_w = lv_obj_get_content_width(dialog());
    const int32_t qr_size = LV_CLAMP(110, dialog_w / 4, 160);

    lv_obj_set_size(container, qr_size, qr_size);

    lv_obj_t* qr = lv_qrcode_create(container);
    if (!qr) {
        spdlog::warn("[ScrewsTiltShare] lv_qrcode_create failed");
        return;
    }

    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, share_text_.c_str(), static_cast<uint32_t>(share_text_.size()));
    lv_obj_center(qr);
    spdlog::debug("[ScrewsTiltShare] QR code created: {}px", qr_size);
#else
    spdlog::warn("[ScrewsTiltShare] QR code support not compiled (LV_USE_QRCODE=0)");
#endif
}

} // namespace helix::ui
