// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_crash_report_modal.h"

#include "ui_toast_manager.h"

#include "display_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "system/crash_reporter.h"
#include "system/debug_bundle_collector.h"
#include "system/telemetry_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

#if LV_USE_QRCODE
extern "C" {
#include "lvgl/src/libs/qrcode/qrcodegen.h"
}
#endif

// =============================================================================
// Static Members
// =============================================================================

bool CrashReportModal::callbacks_registered_ = false;
CrashReportModal* CrashReportModal::active_instance_ = nullptr;

// =============================================================================
// Constructor / Destructor
// =============================================================================

CrashReportModal::CrashReportModal() {
    spdlog::debug("[CrashReportModal] Constructed");
}

CrashReportModal::~CrashReportModal() {
    deinit_subjects();
    // clear()-path teardown frees the instance without on_hide(), which is
    // normally the one that drops the static pointer.
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
    spdlog::trace("[CrashReportModal] Destroyed");
}

// =============================================================================
// Public API
// =============================================================================

void CrashReportModal::set_report(const CrashReporter::CrashReport& report) {
    report_ = report;
}

bool CrashReportModal::show_modal(lv_obj_t* parent) {
    register_callbacks();
    init_subjects();
    // Populate details subject with crash summary
    std::string details = "Signal: " + std::to_string(report_.signal) + " (" + report_.signal_name +
                          ")\nVersion: " + report_.app_version +
                          "\nUptime: " + std::to_string(report_.uptime_sec) + "s";
    if (!report_.timestamp.empty()) {
        details += "\nTime: " + report_.timestamp;
    }
    if (!report_.exception_what.empty()) {
        details += "\nException: " + report_.exception_what;
    }

    lv_subject_copy_string(&details_subject_, details.c_str());
    lv_subject_copy_string(&status_subject_,
                           lv_tr("Send this crash report to help improve HelixScreen."));

    // Call base class show
    bool result = show(parent);
    if (result && dialog()) {
        active_instance_ = this;
    }

    return result;
}

bool CrashReportModal::show_owned(const CrashReporter::CrashReport& report) {
    auto modal = std::make_unique<CrashReportModal>();
    modal->set_report(report);
    if (!modal->show_modal(lv_screen_active())) {
        // show_modal() already registered the XML-named subjects; the global
        // subject map must not outlive the instance the unique_ptr frees here.
        modal->deinit_subjects();
        return false;
    }
    // Stack-owned one-shot: ModalStack frees the instance when its entry goes
    // (#1382).
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

// =============================================================================
// Lifecycle Hooks
// =============================================================================

void CrashReportModal::on_show() {
    spdlog::debug("[CrashReportModal] on_show");

    // Re-parent to top layer. The first-run wizard spawns on the active screen
    // immediately after us and would otherwise render on top, making Send/Dismiss
    // unreachable until wizard completion (#849). Top layer paints above all
    // screen children by design. Same pattern as AbortManager.
    if (backdrop_) {
        lv_obj_set_parent(backdrop_, lv_layer_top());
    }
}

void CrashReportModal::on_hide() {
    spdlog::debug("[CrashReportModal] on_hide");
    active_instance_ = nullptr;

    // Clear the framebuffer so stale crash dialog pixels don't bleed through
    // during app shutdown or LVGL re-init (the dialog renders before the
    // normal UI is fully up, so there's no LVGL content behind it).
    auto* dm = DisplayManager::instance();
    if (dm && dm->backend()) {
        dm->backend()->clear_framebuffer(0xFF000000);
    }
    // No self-delete: the ModalStack owns this instance and frees it when its
    // entry goes (#1382).
}

// =============================================================================
// Subject Management
// =============================================================================

void CrashReportModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    lv_subject_init_string(&details_subject_, details_buf_, nullptr, sizeof(details_buf_), "");
    lv_subject_init_string(&status_subject_, status_buf_, nullptr, sizeof(status_buf_), "");
    lv_subject_init_int(&show_qr_subject_, 0);

    lv_xml_register_subject(nullptr, "crash_report_details", &details_subject_);
    lv_xml_register_subject(nullptr, "crash_report_status", &status_subject_);
    lv_xml_register_subject(nullptr, "crash_report_show_qr", &show_qr_subject_);

    subjects_initialized_ = true;
}

void CrashReportModal::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_deinit(&details_subject_);
    lv_subject_deinit(&status_subject_);
    lv_subject_deinit(&show_qr_subject_);

    subjects_initialized_ = false;
}

// =============================================================================
// Callback Registration
// =============================================================================

void CrashReportModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    lv_xml_register_event_cb(nullptr, "on_crash_report_send", on_send_cb);
    lv_xml_register_event_cb(nullptr, "on_crash_report_dismiss", on_dismiss_cb);

    callbacks_registered_ = true;
}

// =============================================================================
// Static Event Callbacks
// =============================================================================

void CrashReportModal::on_send_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_send();
    }
}

void CrashReportModal::on_dismiss_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_dismiss();
    }
}

// =============================================================================
// Instance Event Handlers
// =============================================================================

void CrashReportModal::handle_send() {
    spdlog::info("[CrashReportModal] User clicked Send Report");
    attempt_delivery();
}

void CrashReportModal::handle_dismiss() {
    spdlog::info("[CrashReportModal] User dismissed crash report");

    // Always consume the crash file so we don't nag on every launch
    CrashReporter::instance().consume_crash_file();

    hide();
}

// =============================================================================
// Delivery Logic
// =============================================================================

void CrashReportModal::attempt_delivery() {
    if (sending_) {
        spdlog::debug("[CrashReportModal] Send already in flight, ignoring double-tap");
        return;
    }
    sending_ = true;

    // Phase 1: collect + upload a debug bundle first. The bundle captures the
    // pre-crash log tail, sanitized settings, crash history, and Moonraker
    // state — context the bare crash report doesn't carry. We pass the share
    // code through to the report so the GitHub issue links to it.
    //
    // Bundle upload runs on a detached thread inside upload_async; the
    // callback is marshaled back to the UI thread. If the bundle fails (no
    // network, worker down, etc.) the report still goes out — just without a
    // share code.
    lv_subject_copy_string(&status_subject_, lv_tr("Collecting debug info..."));

    helix::BundleOptions options;
    // Don't pull Klipper/Moonraker logs in the crash-boot path — Moonraker
    // may not be connected yet and the bundle already has our syslog tail.

    auto token = lifetime_.token();
    // upload_async fires its callback on a detached worker thread —
    // send_with_bundle calls hide() and creates an lv_qrcode widget, both of
    // which are LVGL operations that must run on the main thread. Marshal via
    // token.defer (same anti-pattern that fed the L081 cluster, see
    // feedback_token_defer_required.md).
    helix::DebugBundleCollector::upload_async(
        options, [this, token](const helix::BundleResult& result) {
            std::string share = result.success ? result.share_code : "";
            if (result.success) {
                spdlog::info("[CrashReportModal] Debug bundle attached: {}", share);
            } else {
                spdlog::warn("[CrashReportModal] Debug bundle upload failed "
                             "(continuing without share_code): {}",
                             result.error_message);
            }
            token.defer("CrashReportModal::send_with_bundle",
                        [this, share]() { send_with_bundle(share); });
        });
}

void CrashReportModal::send_with_bundle(const std::string& share_code) {
    auto& cr = CrashReporter::instance();

    lv_subject_copy_string(&status_subject_, lv_tr("Sending..."));

    // Copy so we don't mutate the original until we know the send happened.
    CrashReporter::CrashReport report = report_;
    report.debug_bundle_share_code = share_code;

    if (cr.try_auto_send(report)) {
        spdlog::info("[CrashReportModal] Crash report sent via worker");
        // The user consented to share this crash by sending the report, so
        // honor that consent for telemetry too: emit a lightweight crash event
        // even when the telemetry opt-in is off, keeping fleet crash rates
        // visible. Must run before consume_crash_file() rotates crash.txt away.
        auto& tm = TelemetryManager::instance();
        if (tm.enqueue_crash_event_unconditional()) {
            tm.try_send(/*force=*/true);
        }
        cr.save_to_file(report);
        cr.consume_crash_file();
        hide();
        ToastManager::instance().show(ToastSeverity::SUCCESS,
                                      lv_tr("Crash report sent — thank you!"), 4000);
        return;
    }

    // Auto-send failed — fall back to QR code for phone-based submission.
    std::string url = cr.generate_github_url(report);
    if (!url.empty()) {
        show_qr_code(url);
        lv_subject_copy_string(&status_subject_,
                               lv_tr("No network. Scan QR code to report on your phone."));
    } else {
        lv_subject_copy_string(&status_subject_, lv_tr("Report saved to crash_report.txt"));
    }

    cr.save_to_file(report);
    cr.consume_crash_file();
}

void CrashReportModal::show_qr_code(const std::string& url) {
    // Show the QR container
    lv_subject_set_int(&show_qr_subject_, 1);

    // Find the QR container and create QR code widget
    if (!dialog()) {
        return;
    }

    lv_obj_t* qr_container = lv_obj_find_by_name(dialog(), "qr_container");
    if (!qr_container) {
        spdlog::warn("[CrashReportModal] QR container not found");
        return;
    }

#if LV_USE_QRCODE
    lv_obj_t* qr = lv_qrcode_create(qr_container);
    if (qr) {
        // Compute exact canvas size to eliminate white margin:
        // Get QR version for this data, then size canvas to exact multiple of modules
        int qr_version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, url.size());
        int qr_modules = (qr_version > 0) ? qrcodegen_version2size(qr_version) : 0;
        int target_px = 180;
        int canvas_size = target_px;
        if (qr_modules > 0) {
            int scale = target_px / qr_modules;
            if (scale < 1)
                scale = 1;
            canvas_size = qr_modules * scale;
        }
        lv_qrcode_set_size(qr, canvas_size);
        lv_qrcode_set_quiet_zone(qr, false);
        lv_qrcode_update(qr, url.c_str(), static_cast<uint32_t>(url.size()));
        lv_obj_center(qr);
        spdlog::debug(
            "[CrashReportModal] QR code created: {} chars, version={}, modules={}, canvas={}px",
            url.size(), qr_version, qr_modules, canvas_size);
    }
#else
    spdlog::warn("[CrashReportModal] QR code support not compiled in (LV_USE_QRCODE=0)");
    lv_subject_copy_string(&status_subject_, lv_tr("Saved to crash_report.txt (QR not available)"));
    (void)url;
#endif
}
