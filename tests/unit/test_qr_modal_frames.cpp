// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The three modals that show a QR code: what each one draws, measured on the
// lv_qrcode widget itself (payload, size, colors, quiet zone, visibility), so
// the cases hold however the frame around the code is built.

#include "ui_crash_report_modal.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#include "ui_screws_tilt_share_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "calibration_types.h"
#include "lvgl/src/libs/qrcode/qrcodegen.h"
#include "lvgl/src/widgets/qrcode/lv_qrcode_private.h"
#include "screws_tilt_share_text.h"
#include "system/crash_reporter.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

namespace fs = std::filesystem;

void collect_qrcodes(lv_obj_t* obj, std::vector<lv_obj_t*>& out) {
    if (lv_obj_check_type(obj, &lv_qrcode_class)) {
        out.push_back(obj);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_qrcodes(lv_obj_get_child(obj, i), out);
    }
}

/// The dialog's only QR code; fails the case when there is none or several.
lv_obj_t* only_qrcode(lv_obj_t* dialog) {
    std::vector<lv_obj_t*> found;
    collect_qrcodes(dialog, found);
    REQUIRE(found.size() == 1);
    return found.front();
}

size_t qrcode_count(lv_obj_t* dialog) {
    std::vector<lv_obj_t*> found;
    collect_qrcodes(dialog, found);
    return found.size();
}

/// No object between the code and the dialog is hidden.
bool shown_within(lv_obj_t* obj, lv_obj_t* dialog) {
    for (lv_obj_t* o = obj; o && o != dialog; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o)) {
            return false;
        }
    }
    return true;
}

std::string payload(lv_obj_t* qr) {
    auto* q = reinterpret_cast<lv_qrcode_t*>(qr);
    return std::string(reinterpret_cast<const char*>(q->data), q->data_len);
}

/// What every QR in the app draws: black on white, edge to edge, centered
/// inside the object that holds it.
void check_common_look(lv_obj_t* qr) {
    auto* q = reinterpret_cast<lv_qrcode_t*>(qr);
    CHECK(lv_color_eq(q->dark_color, lv_color_black()));
    CHECK(lv_color_eq(q->light_color, lv_color_white()));
    CHECK(q->quiet_zone == 0);

    lv_obj_t* host = lv_obj_get_parent(qr);
    lv_area_t host_area;
    lv_obj_get_content_coords(host, &host_area);
    lv_area_t qr_area;
    lv_obj_get_coords(qr, &qr_area);
    CHECK(qr_area.x1 - host_area.x1 == host_area.x2 - qr_area.x2);
    CHECK(qr_area.y1 - host_area.y1 == host_area.y2 - qr_area.y2);
}

class ScopedEnv {
  public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        if (const char* v = std::getenv(key)) {
            had_ = true;
            saved_ = v;
        }
        ::setenv(key, value, 1);
    }
    ~ScopedEnv() {
        if (had_) {
            ::setenv(key_.c_str(), saved_.c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    std::string key_;
    std::string saved_;
    bool had_ = false;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "InfoQrModal draws its URL as a QR sized to two fifths of the dialog",
                 "[qr_modal][info_qr_modal]") {
    helix::ui::InfoQrModal modal({"help_circle", "Help", "Scan for help",
                                  "https://helixscreen.org/help", "helixscreen.org/help"});
    REQUIRE(modal.show_modal(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    lv_obj_update_layout(dialog);

    lv_obj_t* qr = only_qrcode(dialog);
    CHECK(payload(qr) == "https://helixscreen.org/help");
    const int32_t expected = LV_CLAMP(120, lv_obj_get_content_width(dialog) * 2 / 5, 200);
    CHECK(lv_obj_get_width(qr) == expected);
    CHECK(lv_obj_get_height(qr) == expected);
    CHECK(lv_obj_get_width(lv_obj_get_parent(qr)) == expected);
    CHECK(lv_obj_get_height(lv_obj_get_parent(qr)) == expected);
    CHECK(shown_within(qr, dialog));
    check_common_look(qr);
    modal.hide();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ScrewsTiltShareModal draws the share text as a QR sized to a quarter of the "
                 "dialog",
                 "[qr_modal][screws_tilt]") {
    std::vector<ScrewTiltResult> results(2);
    results[0].screw_name = "front_left";
    results[0].z_height = 2.5f;
    results[0].is_reference = true;
    results[1].screw_name = "front_right";
    results[1].z_height = 2.45f;
    results[1].adjustment = "CW 00:05";
    const std::string text = build_screws_tilt_share_text(results);

    helix::ui::ScrewsTiltShareModal modal(results);
    REQUIRE(modal.show_modal(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    lv_obj_update_layout(dialog);

    lv_obj_t* qr = only_qrcode(dialog);
    CHECK(payload(qr) == text);
    const int32_t expected = LV_CLAMP(110, lv_obj_get_content_width(dialog) / 4, 160);
    CHECK(lv_obj_get_width(qr) == expected);
    CHECK(lv_obj_get_height(qr) == expected);
    CHECK(lv_obj_get_width(lv_obj_get_parent(qr)) == expected);
    CHECK(lv_obj_get_height(lv_obj_get_parent(qr)) == expected);
    CHECK(shown_within(qr, dialog));
    check_common_look(qr);
    modal.hide();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CrashReportModal shows the issue URL as a QR, snapped to whole modules, only "
                 "once the report falls back to the phone",
                 "[qr_modal][crash_report]") {
    ScopedEnv no_uploads("HELIX_DIAGNOSTIC_UPLOADS", "0");
    const fs::path dir = fs::temp_directory_path() / "helix_test_qr_modal_frames";
    fs::create_directories(dir);

    CrashReporter reporter;
    reporter.init(dir.string());
    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "1.2.3";
    report.timestamp = "2026-11-20T12:00:00Z";
    const std::string url = reporter.generate_github_url(report);
    REQUIRE_FALSE(url.empty());

    CrashReportModal modal(reporter);
    modal.set_report(report);
    REQUIRE(modal.show_modal(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    CHECK(qrcode_count(dialog) == 0);

    lv_obj_t* send = lv_obj_find_by_name(dialog, "modal_send_btn");
    REQUIRE(send != nullptr);
    lv_obj_send_event(send, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(dialog);

    lv_obj_t* qr = only_qrcode(dialog);
    CHECK(payload(qr) == url);
    const int version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, url.size());
    REQUIRE(version > 0);
    const int modules = qrcodegen_version2size(version);
    const int expected = modules * std::max(1, 180 / modules);
    CHECK(lv_obj_get_width(qr) == expected);
    CHECK(lv_obj_get_height(qr) == expected);
    CHECK(shown_within(qr, dialog));
    check_common_look(qr);

    modal.hide();
    reporter.shutdown();
    fs::remove_all(dir);
}
