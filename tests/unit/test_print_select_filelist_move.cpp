// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_filelist_move.cpp
 * @brief The panel's notify_filelist_changed handler must parse source_item.
 *
 * Moonraker builds the notification's `item` from a move's destination and
 * attaches the origin as `source_item`, so a gcode moved out of the gcodes
 * root reports item.root == "config" with source_item.root == "gcodes". The
 * root filter predicate is pinned as a pure function in
 * test_print_select_filelist_filter.cpp; these cases pin the wiring - the
 * registered callback must parse source_item and refresh, while a change
 * confined to foreign roots must not re-fetch. They also pin the parse against
 * payload shapes Moonraker is free to send: a null field or a payload that is
 * not an object must still reach the filter.
 */

#include "ui_panel_print_select.h"

#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The real notification path over the real panel. The fixture's API handoff
/// registers the handler; it parses the frame on the calling thread and
/// marshals the refresh to the main thread via async_call, so firing the
/// callbacks from the test thread mirrors a WebSocket delivery.
class PrintSelectFilelistFixture : public PrintSelectPanelFixture {
  public:
    PrintSelectFilelistFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Registered) {}

    /// One payload wrapped in the JSON-RPC envelope the WebSocket dispatch
    /// hands to a method callback.
    static json frame(json payload) {
        return json{{"jsonrpc", "2.0"},
                    {"method", "notify_filelist_changed"},
                    {"params", json::array({std::move(payload)})}};
    }

    /// A move_file payload with caller-chosen item and source_item roots. A
    /// null source_root omits source_item entirely, the way an upload, create
    /// or delete arrives.
    static json move_frame(const std::string& item_root, const char* source_root) {
        json payload = {{"action", "move_file"},
                        {"item", {{"root", item_root}, {"path", "spare.cfg"}}}};
        if (source_root != nullptr) {
            payload["source_item"] = {{"root", source_root}, {"path", "origin.gcode"}};
        }
        return frame(std::move(payload));
    }

    void fire(const json& msg) {
        MoonrakerClientTestAccess::fire_method_callbacks(mock_client_, "notify_filelist_changed",
                                                         msg);
        drain();
    }
};

} // namespace

// ============================================================================
// The regression: a move OUT of gcodes reports a foreign item root
// ============================================================================

TEST_CASE_METHOD(PrintSelectFilelistFixture, "A move out of the gcodes root refreshes the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_out.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    // The move already happened on the printer's storage: only the
    // notification can tell the panel to look again. item describes the
    // destination; the gcodes origin lives in source_item.
    REQUIRE(file.remove_from_disk());
    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));

    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(PrintSelectFilelistFixture,
                 "A move confined to foreign roots does not re-fetch the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_foreign.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    fire(PrintSelectFilelistFixture::move_frame("config", "config"));
    // No refresh: the cached list still names a file the directory no longer
    // has, which is the price of filtering — one the config-root storm makes
    // worth paying.
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    // The callback is live, not silently dead: the same panel, with no
    // further refresh_files(true), still refreshes when a frame names gcodes.
    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(PrintSelectFilelistFixture,
                 "A foreign-root frame without source_item does not re-fetch the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_nosrc.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    // Uploads, creates and deletes carry no source_item at all; one of those
    // shaped like a foreign-root item must not refresh.
    fire(PrintSelectFilelistFixture::move_frame("config", nullptr));
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

// ============================================================================
// Payload shapes the parse must survive before the filter can run
// ============================================================================

TEST_CASE_METHOD(PrintSelectFilelistFixture, "A filelist frame with null fields reaches the filter",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_null_fields.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    // Moonraker declares these fields as strings, but a JSON null is legal on
    // the wire and nlohmann's value(key, default) returns the default only for
    // an ABSENT key - a present null goes through get<std::string>() and
    // throws. The client catches per callback, so the throw would cost the
    // panel the whole frame and leave the listing stale.
    fire(frame(json{{"action", nullptr},
                    {"item", {{"root", nullptr}, {"path", nullptr}}},
                    {"source_item", {{"root", nullptr}, {"path", nullptr}}}}));

    // An item root that parses to empty is the fail-safe case: refresh rather
    // than trust a payload shape we do not recognise.
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(PrintSelectFilelistFixture,
                 "A filelist frame whose payload is not an object reaches the filter",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_scalar_payload.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    // params[0] is an object on every frame Moonraker documents; reading
    // fields off one that is not throws before the filter ever runs.
    fire(frame(json(nullptr)));

    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}
