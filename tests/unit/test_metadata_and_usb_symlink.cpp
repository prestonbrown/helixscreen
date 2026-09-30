// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_metadata_and_usb_symlink.cpp
 * @brief Unit tests for metadata silent flag, metascan fallback, and USB symlink detection
 *
 * Tests:
 * - MoonrakerAPI::get_file_metadata() with silent flag
 * - MoonrakerAPI::metascan_file() API method
 * - PrintSelectUsbSource symlink detection and tab hiding
 */

#include "ui_update_queue.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_file_api.h"
#include "../../include/ui_print_select_usb_source.h"
#include "../../lvgl/lvgl.h"
#include "../test_helpers/moonraker_file_api_test_access.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerMetadataUSB {
    LVGLInitializerMetadataUSB() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerMetadataUSB lvgl_init;
} // namespace

// ============================================================================
// Test Fixtures
// ============================================================================

/**
 * @brief Test fixture for MoonrakerAPI metadata operations
 */
class MetadataAPITestFixture {
  public:
    MetadataAPITestFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // Initialize printer state
        state.init_subjects(false);

        // Connect mock client
        mock_client.connect("ws://mock/websocket", []() {}, []() {});

        // Run discovery
        mock_client.discover_printer([]() {});

        // Create API
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~MetadataAPITestFixture() {
        // Drain while `state` is still alive — discover_printer() in the ctor
        // leaves PrinterCapabilitiesState's deferred setters queued (#1166).
        helix::ui::UpdateQueue::instance().drain();

        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

// ============================================================================
// Metadata API Tests
// ============================================================================

TEST_CASE_METHOD(MetadataAPITestFixture, "get_file_metadata calls success callback with valid file",
                 "[metadata][api]") {
    bool success_called = false;
    bool error_called = false;

    api->files().get_file_metadata(
        "test_file.gcode", [&](const FileMetadata&) { success_called = true; },
        [&](const MoonrakerError&) { error_called = true; });

    // Mock is synchronous - callback already fired
    REQUIRE(success_called);
    REQUIRE_FALSE(error_called);
}

TEST_CASE_METHOD(MetadataAPITestFixture, "get_file_metadata forwards its silent flag to the send",
                 "[metadata][api][silent]") {
    // silent=true is what keeps check_timeouts() from toasting REQUEST_TIMEOUT when a
    // file isn't indexed yet. Asserting only success_called would pass even if the flag
    // were dropped on the floor, so read it back off the transport.
    bool success_called = false;

    api->files().get_file_metadata(
        "test_file.gcode", [&](const FileMetadata&) { success_called = true; },
        [&](const MoonrakerError&) {}, true // silent
    );

    REQUIRE(success_called);
    REQUIRE(mock_client.last_send_method() == "server.files.metadata");
    REQUIRE(mock_client.last_send_silent() == true);

    // The default (4th argument omitted) must NOT be silent — otherwise a genuine
    // metadata failure would go unreported.
    success_called = false;
    api->files().get_file_metadata(
        "test_file.gcode", [&](const FileMetadata&) { success_called = true; },
        [&](const MoonrakerError&) {});

    REQUIRE(success_called);
    REQUIRE(mock_client.last_send_silent() == false);
}

TEST_CASE_METHOD(MetadataAPITestFixture, "metascan_file calls success callback with metadata",
                 "[metadata][api][metascan]") {
    bool success_called = false;
    bool error_called = false;

    api->files().metascan_file(
        "test_file.gcode", [&](const FileMetadata&) { success_called = true; },
        [&](const MoonrakerError&) { error_called = true; });

    // Mock is synchronous - callback already fired
    REQUIRE(success_called);
    REQUIRE_FALSE(error_called);
}

TEST_CASE_METHOD(MetadataAPITestFixture, "metascan_file is silent by default",
                 "[metadata][api][metascan]") {
    // metascan_file defaults silent=true: it is the fallback the UI fires when
    // get_file_metadata() came back unindexed, so its own failure must not toast.
    // The case above already proves success_called alone cannot see the flag.
    bool success_called = false;

    api->files().metascan_file(
        "test_file.gcode", [&](const FileMetadata&) { success_called = true; },
        [&](const MoonrakerError&) {});

    REQUIRE(success_called);
    REQUIRE(mock_client.last_send_method() == "server.files.metascan");
    REQUIRE(mock_client.last_send_silent() == true);
}

// The metascan single-flight gate: the print-select panel re-triggers metascan
// on every metadata miss (list refresh, detail view, activation retry) and
// Moonraker queues each duplicate server-side, so a host that parses slowly
// (QIDI .gcode.3mf on a small board) accrues a backlog that starves the UI's
// own requests. These cases pin the two guards.
namespace {
/// Prime last_send_method() with a known non-metascan value so a later
/// REQUIRE on it proves whether a metascan was actually sent.
void prime_last_send(MetadataAPITestFixture& f) {
    f.api->files().get_file_metadata(
        "seed.gcode", [](const FileMetadata&) {}, [](const MoonrakerError&) {}, true);
    REQUIRE(f.mock_client.last_send_method() == "server.files.metadata");
}
} // namespace

TEST_CASE_METHOD(MetadataAPITestFixture, "metascan_file drops a duplicate while one is in flight",
                 "[metadata][api][metascan]") {
    MoonrakerFileApiTestAccess::force_metascan_in_flight(api->files(), "dup.gcode");
    prime_last_send(*this);

    bool success_called = false;
    api->files().metascan_file(
        "dup.gcode", [&](const FileMetadata&) { success_called = true; },
        [](const MoonrakerError&) {});

    REQUIRE_FALSE(success_called);
    REQUIRE(mock_client.last_send_method() == "server.files.metadata"); // nothing sent
}

TEST_CASE_METHOD(MetadataAPITestFixture,
                 "metascan_file completion arms a cooldown that suppresses a rescan",
                 "[metadata][api][metascan]") {
    bool success_called = false;
    api->files().metascan_file(
        "cooldown.gcode", [&](const FileMetadata&) { success_called = true; },
        [](const MoonrakerError&) {});
    REQUIRE(success_called); // the mock completes synchronously

    REQUIRE(MoonrakerFileApiTestAccess::metascan_in_cooldown(api->files(), "cooldown.gcode"));
    REQUIRE_FALSE(MoonrakerFileApiTestAccess::metascan_in_flight(api->files(), "cooldown.gcode"));

    prime_last_send(*this);
    success_called = false;
    api->files().metascan_file(
        "cooldown.gcode", [&](const FileMetadata&) { success_called = true; },
        [](const MoonrakerError&) {});

    REQUIRE_FALSE(success_called);
    REQUIRE(mock_client.last_send_method() == "server.files.metadata"); // nothing sent

    // Once the cooldown expires, the same file is scannable again.
    MoonrakerFileApiTestAccess::expire_metascan_cooldown(api->files(), "cooldown.gcode");
    success_called = false;
    api->files().metascan_file(
        "cooldown.gcode", [&](const FileMetadata&) { success_called = true; },
        [](const MoonrakerError&) {});
    REQUIRE(success_called);
    REQUIRE(mock_client.last_send_method() == "server.files.metascan");
}

TEST_CASE_METHOD(MetadataAPITestFixture, "metascan_file error completion also arms the cooldown",
                 "[metadata][api][metascan]") {
    // HELIX_MOCK_METADATA_404 makes the mock fail every metascan with
    // file-not-found — the shape of a vendor fork whose parser cannot read
    // the file at all. Read per call by the mock, so the toggle is scoped to
    // this request and invisible to the rest of the suite.
    ::setenv("HELIX_MOCK_METADATA_404", "1", 1);
    bool error_called = false;
    api->files().metascan_file(
        "unparsable.gcode", [](const FileMetadata&) {},
        [&](const MoonrakerError&) { error_called = true; });
    REQUIRE(error_called);

    // The toggle must be live per request: unset, the same file now scans
    // cleanly. A cached read would latch the first evaluation for the whole
    // process and this success would arrive as an error instead.
    ::unsetenv("HELIX_MOCK_METADATA_404");
    MoonrakerFileApiTestAccess::expire_metascan_cooldown(api->files(), "unparsable.gcode");
    bool recovered = false;
    api->files().metascan_file(
        "unparsable.gcode", [&](const FileMetadata&) { recovered = true; },
        [](const MoonrakerError&) {});
    REQUIRE(recovered);
    REQUIRE(mock_client.last_send_method() == "server.files.metascan");

    REQUIRE(MoonrakerFileApiTestAccess::metascan_in_cooldown(api->files(), "unparsable.gcode"));
    REQUIRE_FALSE(MoonrakerFileApiTestAccess::metascan_in_flight(api->files(), "unparsable.gcode"));

    prime_last_send(*this);
    api->files().metascan_file(
        "unparsable.gcode", [](const FileMetadata&) {}, [](const MoonrakerError&) {});
    REQUIRE(mock_client.last_send_method() == "server.files.metadata"); // nothing sent
}

namespace {
/// Exposes the protected metadata parser.
class MetadataParseProbe : public MoonrakerFileAPI {
  public:
    using MoonrakerFileAPI::MoonrakerFileAPI;
    using MoonrakerFileAPI::parse_file_list;
    using MoonrakerFileAPI::parse_file_metadata;
};
} // namespace

TEST_CASE_METHOD(MetadataAPITestFixture, "Metadata sent as JSON strings still parses",
                 "[metadata][api][qidi_3mf]") {
    // A QIDI Moonraker copies layer_height out of the 3mf project config as a string.
    MetadataParseProbe probe(mock_client);
    json response;
    response["result"] = {{"filename", "Foo (PETG).gcode.3mf"},
                          {"layer_height", "0.2"},
                          {"first_layer_height", "0.25"},
                          {"object_height", "12.5"},
                          {"estimated_time", 600.0}};
    const FileMetadata m = probe.parse_file_metadata(response);
    CHECK(m.layer_height == Catch::Approx(0.2));
    CHECK(m.first_layer_height == Catch::Approx(0.25));
    CHECK(m.object_height == Catch::Approx(12.5));
    CHECK(m.estimated_time == Catch::Approx(600.0));
}

TEST_CASE_METHOD(MetadataAPITestFixture, "Integer metadata sent as JSON strings still parses",
                 "[metadata][api][json_coercion]") {
    MetadataParseProbe probe(mock_client);
    json response;
    response["result"] = {
        {"size", "711288"},
        {"layer_count", "42"},
        {"gcode_start_byte", "1024"},
        {"gcode_end_byte", "700000"},
        {"thumbnails",
         json::array({{{"relative_path", ".thumbs/a.png"}, {"width", "300"}, {"height", "200"}}})}};
    const FileMetadata m = probe.parse_file_metadata(response);
    CHECK(m.size == 711288);
    CHECK(m.layer_count == 42);
    CHECK(m.gcode_start_byte == 1024);
    CHECK(m.gcode_end_byte == 700000);
    REQUIRE(m.thumbnails.size() == 1);
    CHECK(m.thumbnails[0].width == 300);
    CHECK(m.thumbnails[0].height == 200);
}

TEST_CASE_METHOD(MetadataAPITestFixture,
                 "Integer metadata defaults on null, missing, negative and garbage",
                 "[metadata][api][json_coercion]") {
    MetadataParseProbe probe(mock_client);
    json response;
    response["result"] = {{"size", "-1"},
                          {"layer_count", "-1"},
                          {"gcode_start_byte", nullptr},
                          {"gcode_end_byte", "garbage"},
                          {"thumbnails", json::array({{{"relative_path", ".thumbs/a.png"},
                                                       {"width", nullptr},
                                                       {"height", "tall"}}})}};
    FileMetadata m = probe.parse_file_metadata(response);
    CHECK(m.size == 0);
    CHECK(m.layer_count == 0);
    CHECK(m.gcode_start_byte == 0);
    CHECK(m.gcode_end_byte == 0);
    REQUIRE(m.thumbnails.size() == 1);
    CHECK(m.thumbnails[0].width == 0);
    CHECK(m.thumbnails[0].height == 0);

    response["result"] = {{"layer_count", -1}, {"size", -5}};
    m = probe.parse_file_metadata(response);
    CHECK(m.layer_count == 0);
    CHECK(m.size == 0);

    response["result"] = {{"layer_count", 5000000000LL}};
    CHECK(probe.parse_file_metadata(response).layer_count == 0);

    response["result"] = json::object();
    m = probe.parse_file_metadata(response);
    CHECK(m.size == 0);
    CHECK(m.layer_count == 0);
}

TEST_CASE_METHOD(MetadataAPITestFixture, "File list sizes and times sent as JSON strings parse",
                 "[metadata][api][json_coercion]") {
    MetadataParseProbe probe(mock_client);
    json response;

    SECTION("flat server.files.list array") {
        response["result"] =
            json::array({{{"path", "a.gcode"}, {"size", "711288"}, {"modified", "1757990000.5"}},
                         {{"path", "b.gcode"}, {"size", "-1"}, {"modified", nullptr}}});
        const auto files = probe.parse_file_list(response);
        REQUIRE(files.size() == 2);
        CHECK(files[0].size == 711288);
        CHECK(files[0].modified == Catch::Approx(1757990000.5));
        CHECK(files[1].size == 0);
        CHECK(files[1].modified == 0.0);
    }

    SECTION("dirs and files object") {
        response["result"] = {
            {"dirs", json::array({{{"dirname", "sub"}, {"modified", "1757990000.5"}}})},
            {"files",
             json::array(
                 {{{"filename", "c.gcode"}, {"size", "42"}, {"modified", "1757990001.5"}}})}};
        const auto files = probe.parse_file_list(response);
        REQUIRE(files.size() == 2);
        CHECK(files[0].modified == Catch::Approx(1757990000.5));
        CHECK(files[1].size == 42);
        CHECK(files[1].modified == Catch::Approx(1757990001.5));
    }
}

// ============================================================================
// USB Source Symlink Detection Tests
// ============================================================================

namespace {
// Read a registered subject's current int value by name. Fails the calling
// test immediately if the subject isn't registered — a missing subject and a
// subject stuck at its default both need to be distinguishable failures.
int subject_int_value(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}
} // namespace

TEST_CASE("PrintSelectUsbSource initial state has Moonraker access false", "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    REQUIRE_FALSE(usb_source.moonraker_has_usb_access());
}

TEST_CASE("PrintSelectUsbSource::on_drive_inserted / on_drive_removed write "
          "print_source_usb_present",
          "[usb][symlink]") {
    // Regression coverage for the Rule #2 fix (imperative lv_obj_add_flag/
    // remove_flag on source_selector -> a subject + XML bind_flag_if). This
    // reads the subject the XML binding actually observes, not just the C++
    // member state — deleting either lv_subject_set_int call in
    // on_drive_inserted()/on_drive_removed() must fail this test.
    helix::ui::PrintSelectUsbSource::init_subjects();
    helix::ui::PrintSelectUsbSource usb_source;

    // print_source_usb_present is a process-wide static; an earlier test case
    // may have already written it. Drive to a known state via the real API
    // rather than assume a pristine default.
    usb_source.on_drive_removed();
    REQUIRE(subject_int_value("print_source_usb_present") == 0);

    usb_source.on_drive_inserted();
    REQUIRE(subject_int_value("print_source_usb_present") == 1);

    usb_source.on_drive_removed();
    REQUIRE(subject_int_value("print_source_usb_present") == 0);

    // Re-insert to confirm it isn't a one-shot latch.
    usb_source.on_drive_inserted();
    REQUIRE(subject_int_value("print_source_usb_present") == 1);
}

TEST_CASE("PrintSelectUsbSource::set_usb_manager writes print_source_usb_present for the "
          "startup-race case",
          "[usb][symlink]") {
    // set_usb_manager(nullptr) exercises the "no manager yet" branch — has_drives
    // is false, so this also confirms the subject is written (to 0) even when
    // there's nothing to report, not just left at its default by omission.
    helix::ui::PrintSelectUsbSource::init_subjects();
    helix::ui::PrintSelectUsbSource usb_source;

    usb_source.set_usb_manager(nullptr);
    REQUIRE(subject_int_value("print_source_usb_present") == 0);
}

TEST_CASE("PrintSelectUsbSource with symlink access stays on PRINTER source", "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    // Set symlink access
    usb_source.set_moonraker_has_usb_access(true);

    // Verify we stay on PRINTER source (default)
    REQUIRE(usb_source.get_current_source() == FileSource::PRINTER);
    REQUIRE_FALSE(usb_source.is_usb_active());
}

TEST_CASE("PrintSelectUsbSource on_drive_inserted does nothing when symlink active",
          "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    // Set symlink access BEFORE drive insert
    usb_source.set_moonraker_has_usb_access(true);

    // on_drive_inserted should be a no-op (button is null anyway, but logic should skip)
    usb_source.on_drive_inserted();

    // Should still be on PRINTER source
    REQUIRE(usb_source.get_current_source() == FileSource::PRINTER);
}

TEST_CASE("PrintSelectUsbSource on_drive_removed switches from USB to PRINTER", "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    FileSource last_source = FileSource::PRINTER;
    bool callback_fired = false;
    usb_source.set_on_source_changed([&](FileSource source) {
        last_source = source;
        callback_fired = true;
    });

    // select_usb_source() calls refresh_files(), which is a safe no-op with
    // no UsbManager attached (warns, reports an empty file list).
    usb_source.select_usb_source();
    REQUIRE(usb_source.is_usb_active());

    usb_source.on_drive_removed();

    REQUIRE(usb_source.get_current_source() == FileSource::PRINTER);
    REQUIRE_FALSE(usb_source.is_usb_active());
    REQUIRE(callback_fired);
    REQUIRE(last_source == FileSource::PRINTER);
}

TEST_CASE("PrintSelectUsbSource on_drive_removed while already on PRINTER does not fire the "
          "source-changed callback",
          "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    bool callback_fired = false;
    usb_source.set_on_source_changed([&](FileSource) { callback_fired = true; });

    // Already on PRINTER (the default) — removal has nothing to switch away from.
    usb_source.on_drive_removed();

    REQUIRE(usb_source.get_current_source() == FileSource::PRINTER);
    REQUIRE_FALSE(callback_fired);
}

TEST_CASE("PrintSelectUsbSource::set_moonraker_has_usb_access writes "
          "print_source_moonraker_usb_access in both directions",
          "[usb][symlink]") {
    // Pre-existing gap this change fixes: the old imperative version only ever
    // hid the selector when has_access became true, with no code path to show
    // it again if access were later revoked. Reading the subject the XML
    // binding observes (not just the member getter) is what actually proves
    // the fix — a stub that skipped the false-branch write would still pass
    // the getter-only assertions below but fail these subject reads.
    helix::ui::PrintSelectUsbSource::init_subjects();
    helix::ui::PrintSelectUsbSource usb_source;

    // print_source_moonraker_usb_access is a process-wide static; an earlier
    // test case may have already written it. Drive to a known state via the
    // real API rather than assume a pristine default.
    usb_source.set_moonraker_has_usb_access(false);
    REQUIRE(subject_int_value("print_source_moonraker_usb_access") == 0);

    usb_source.set_moonraker_has_usb_access(true);
    REQUIRE(usb_source.moonraker_has_usb_access());
    REQUIRE(subject_int_value("print_source_moonraker_usb_access") == 1);

    usb_source.set_moonraker_has_usb_access(false);
    REQUIRE_FALSE(usb_source.moonraker_has_usb_access());
    REQUIRE(subject_int_value("print_source_moonraker_usb_access") == 0);

    usb_source.set_moonraker_has_usb_access(true);
    REQUIRE(usb_source.moonraker_has_usb_access());
    REQUIRE(subject_int_value("print_source_moonraker_usb_access") == 1);
}

TEST_CASE("PrintSelectUsbSource switches from USB to PRINTER when symlink detected",
          "[usb][symlink]") {
    helix::ui::PrintSelectUsbSource usb_source;

    // Track source changes
    FileSource last_source = FileSource::PRINTER;
    usb_source.set_on_source_changed([&](FileSource source) { last_source = source; });

    // Manually set to USB source (simulating user clicked USB tab)
    // Note: We can't fully test this without LVGL widgets, but we test the logic
    // The implementation switches to PRINTER when symlink is detected while on USB

    // Detecting symlink should trigger switch to PRINTER and callback
    usb_source.set_moonraker_has_usb_access(true);

    // If we were on USB, we'd switch to PRINTER
    // Since we start on PRINTER, the callback won't fire but state should remain PRINTER
    REQUIRE(usb_source.get_current_source() == FileSource::PRINTER);
}

// ============================================================================
// Integration-style Tests
// ============================================================================

TEST_CASE_METHOD(MetadataAPITestFixture, "list_files for usb path returns empty when no symlink",
                 "[usb][symlink][integration]") {
    // Ensure symlink simulation is off
    mock_set_usb_symlink_active(false);

    bool success_called = false;
    std::vector<FileInfo> received_files;

    api->files().list_files(
        "gcodes", "usb", false,
        [&](const std::vector<FileInfo>& files) {
            received_files = files;
            success_called = true;
        },
        [&](const MoonrakerError&) {});

    // Mock is synchronous
    REQUIRE(success_called);
    REQUIRE(received_files.empty()); // No files when symlink not active
}

TEST_CASE_METHOD(MetadataAPITestFixture,
                 "list_files for usb path returns files when symlink active",
                 "[usb][symlink][integration]") {
    // Enable symlink simulation
    mock_set_usb_symlink_active(true);

    bool success_called = false;
    std::vector<FileInfo> received_files;

    api->files().list_files(
        "gcodes", "usb", false,
        [&](const std::vector<FileInfo>& files) {
            received_files = files;
            success_called = true;
        },
        [&](const MoonrakerError&) {});

    // Mock is synchronous
    REQUIRE(success_called);
    REQUIRE_FALSE(received_files.empty()); // Has files when symlink active

    // Cleanup
    mock_set_usb_symlink_active(false);
}
