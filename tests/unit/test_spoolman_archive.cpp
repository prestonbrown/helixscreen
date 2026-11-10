// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spoolman_archive.cpp
 * @brief SpoolmanPanel::archive_spool — the reversible sibling of delete
 *
 * Archiving is a PATCH ({"archived": true}), not a DELETE: the spool stays on
 * the Spoolman server and the server drops it from list GETs. These tests pin
 * the panel's confirm flow end to end — dialog, PATCH payload, refetch —
 * against the mock's server-side spool list, mirroring what a user pressing
 * Archive in the context menu produces.
 */

#include "ui_modal.h"
#include "ui_panel_spoolman.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/spoolman_panel_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_types.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A Spoolman record the identity cache will accept (mirrors the helper in
/// test_spoolman_panel_commit.cpp — needs a name to be cacheable).
SpoolInfo make_spool(int id, std::string filament_name) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = "Polymaker";
    spool.filament_name = std::move(filament_name);
    spool.material = "PLA";
    spool.color_hex = "FFB6C1";
    return spool;
}

/// Archive touches no AMS/settings stores, so this is the commit fixture of
/// test_spoolman_panel_commit.cpp reduced to the mock-API wiring the confirm
/// lambda resolves through the global — plus the modal subjects the dialog
/// needs. The panel under test is the global instance, because the confirm
/// lambda's success path refreshes THAT panel.
struct SpoolmanArchiveFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;

    SpoolmanArchiveFixture() : api(client, get_printer_state()) {
        helix::ui::modal_init_subjects();

        // Controlled server-side state: two spools, so the refetch assertion
        // can tell "archived one removed" from "list emptied".
        auto& server = client.spoolman_mock().get_mock_spools();
        server.clear();
        server.push_back(make_spool(41, "Ambrosia Pink"));
        server.push_back(make_spool(42, "Jet Black"));

        client.set_mock_spoolman_enabled(true);
        set_moonraker_api(&api);
    }

    ~SpoolmanArchiveFixture() override {
        // Detach the mock before member destruction, while LVGL still runs.
        set_moonraker_api(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Answer the archive confirmation dialog by clicking one of its buttons
    /// (the modal closes itself; the queued follow-ups need the drain).
    void answer_modal(const char* button_name) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Whether the mock's LIST GET serves the spool. Goes through
/// get_spoolman_spools() (not the raw vector) because "served" is exactly the
/// list semantics real Spoolman filters archived spools out of.
bool server_serves(MoonrakerAPIMock& api, int spool_id) {
    bool found = false;
    api.spoolman().get_spoolman_spools(
        [&found, spool_id](const std::vector<SpoolInfo>& spools) {
            found = std::any_of(spools.begin(), spools.end(),
                                [spool_id](const SpoolInfo& s) { return s.id == spool_id; });
        },
        [](const MoonrakerError&) { FAIL("list GET should succeed"); });
    return found;
}

} // namespace

TEST_CASE("archive confirm sends one archived PATCH and the refetch drops the spool",
          "[spoolman][archive]") {
    SpoolmanArchiveFixture f;
    SpoolmanPanel& panel = get_global_spoolman_panel();
    SpoolmanPanelTestAccess::seed_cached_spools(
        panel, {make_spool(41, "Ambrosia Pink"), make_spool(42, "Jet Black")});

    // Precondition: the mock's server-side list serves the target spool.
    REQUIRE(server_serves(f.api, 41));

    SpoolmanPanelTestAccess::archive_spool(panel, 41);
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    f.answer_modal("btn_primary");

    const auto& updates = f.client.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 41);
    REQUIRE(updates[0].patch == nlohmann::json{{"archived", true}});

    // Refetch fidelity: the list GET no longer serves the archived spool,
    // while the bystander survives.
    std::vector<SpoolInfo> served;
    f.api.spoolman().get_spoolman_spools(
        [&served](const std::vector<SpoolInfo>& spools) { served = spools; },
        [](const MoonrakerError&) { FAIL("refetch should succeed"); });
    REQUIRE(served.size() == 1);
    REQUIRE(served[0].id == 42);
}

TEST_CASE("an archive the user cancels, or the server refuses, changes nothing",
          "[spoolman][archive]") {
    SpoolmanArchiveFixture f;
    SpoolmanPanel& panel = get_global_spoolman_panel();
    SpoolmanPanelTestAccess::seed_cached_spools(panel, {make_spool(41, "Ambrosia Pink")});
    for (const char* button : {"btn_secondary", "btn_primary"}) {
        CAPTURE(button);
        // Cancel first, while the server is up; then a confirm it fails.
        SpoolmanPanelTestAccess::archive_spool(panel, 41);
        f.answer_modal(button);
        CHECK(f.client.spoolman_mock().spool_updates.empty());
        CHECK(ModalStack::instance().stack_empty());
        f.client.set_mock_spoolman_enabled(false);
    }
}

/// A row widget whose user_data carries the spool id, the contract
/// on_spool_row_clicked relies on.
lv_obj_t* make_spool_row(int spool_id) {
    lv_obj_t* row = lv_obj_create(lv_screen_active());
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(spool_id)));
    return row;
}

TEST_CASE("context menu withholds Archive from the active spool only, and titles the spool",
          "[spoolman][archive]") {
    SpoolmanArchiveFixture f;
    SpoolmanPanel& panel = get_global_spoolman_panel();
    SpoolmanPanelTestAccess::seed_cached_spools(
        panel, {make_spool(41, "Ambrosia Pink"), make_spool(42, "")});
    SpoolmanPanelTestAccess::set_active_spool_id(panel, 41);

    // The menu's subjects outlive each show, so each spool must get its own answer
    // whichever was shown before it.
    struct Case {
        int id;
        bool archive_shown;
        const char* subtitle; ///< nullptr: hidden, the filament has no name
    };
    const Case cases[] = {
        {41, false, "Ambrosia Pink"}, {42, true, nullptr}, {41, false, "Ambrosia Pink"}};
    for (const Case& c : cases) {
        CAPTURE(c.id);
        lv_obj_t* row = make_spool_row(c.id);
        SpoolmanPanelTestAccess::open_context_menu(panel, row);

        lv_obj_t* archive = lv_obj_find_by_name(lv_screen_active(), "btn_archive");
        REQUIRE(archive != nullptr); // the menu actually built
        CHECK(lv_obj_has_flag(archive, LV_OBJ_FLAG_HIDDEN) == !c.archive_shown);
        lv_obj_t* header = lv_obj_find_by_name(lv_screen_active(), "spool_header");
        REQUIRE(header != nullptr);
        CHECK(std::string(lv_label_get_text(header)) == "Polymaker PLA");
        lv_obj_t* subtitle = lv_obj_find_by_name(lv_screen_active(), "spool_color_label");
        REQUIRE(subtitle != nullptr);
        CHECK(lv_obj_has_flag(subtitle, LV_OBJ_FLAG_HIDDEN) == (c.subtitle == nullptr));
        if (c.subtitle) {
            CHECK(std::string(lv_label_get_text(subtitle)) == c.subtitle);
        }

        SpoolmanPanelTestAccess::hide_context_menu(panel);
        f.process_lvgl(50);
        lv_obj_delete(row);
    }
}

TEST_CASE("the location filter shows only while a spool names a location", "[spoolman][location]") {
    SpoolmanArchiveFixture f;
    SpoolmanPanel& panel = get_global_spoolman_panel();
    panel.init_subjects();
    REQUIRE(panel.create(lv_screen_active()) != nullptr);
    lv_obj_t* filter = lv_obj_find_by_name(panel.get_panel(), "location_filter");
    REQUIRE(filter != nullptr);
    for (const char* location : {"Shelf A", ""}) {
        CAPTURE(location);
        f.client.spoolman_mock().get_mock_spools()[0].location = location;
        panel.refresh_spools();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_has_flag(filter, LV_OBJ_FLAG_HIDDEN) == (location[0] == '\0'));
    }
}

TEST_CASE("a queued refresh on a resurrected shell panel does nothing", "[spoolman][1402]") {
    SpoolmanArchiveFixture f;

    // A shell exactly like the one get_global_spoolman_panel() resurrects
    // after StaticPanelRegistry::destroy_all() freed the previous test's
    // panel: constructed, but init_subjects() never ran.
    SpoolmanPanel shell;

    // The recycled-heap bytes its uninitialized subject carries in the crash:
    // the type field reads as INT (ASCII '2' landed on it) while the
    // observer-list head is a pointer made of name-string bytes.
    SpoolmanPanelTestAccess::poison_state_subject_as_uninitialized(shell);
    const lv_subject_t before = SpoolmanPanelTestAccess::state_subject(shell);

    // The queued archive callback's refetch path: refresh_spools() on the
    // shell. Before the guard this wrote the uninitialized subject and
    // segfaulted inside lv_subject_notify (~5/16 [spoolman] runs under load);
    // it must refuse the work instead.
    SpoolmanPanelTestAccess::refresh_spools(shell);

    REQUIRE(0 == std::memcmp(&before, &SpoolmanPanelTestAccess::state_subject(shell),
                             sizeof(lv_subject_t)));
}
