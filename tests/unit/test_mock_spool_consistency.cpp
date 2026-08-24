// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guards against drift between the AMS mock backends' slot filaments and the
// mock Spoolman inventory (MoonrakerSpoolmanAPIMock::init_mock_spools). A slot
// that claims spoolman_id=N must describe the same filament as spool N, or the
// slot editor looks broken in mock mode (spec §9).

#include "ams_backend_mock.h"
#include "color_utils.h"
#include "filament_display_name.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <map>

#include "../catch_amalgamated.hpp"

namespace {

std::map<int, SpoolInfo> fetch_mock_spools() {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    std::map<int, SpoolInfo> by_id;
    api.spoolman().get_spoolman_spools(
        [&](const std::vector<SpoolInfo>& spools) {
            for (const auto& s : spools) {
                by_id[s.id] = s;
            }
        },
        [](const MoonrakerError&) { FAIL("mock spool fetch must not error"); });
    return by_id;
}

/// No mocked backend reports a vendor from firmware, because none of the real
/// ones do: AFC's read_vendor() has nothing to read until upstream AFC #808
/// ships, and Happy Hare's gate map cannot carry a brand at all. The vendor
/// reaches the label through the Spoolman identity cache instead, so a slot that
/// carries its own brand is mocking something hardware never sends -- which is
/// what hid #1264. (CFS, Snapmaker and QIDI do read a vendor from firmware, but
/// none of them has a mock backend.)
void check_backend_against_spoolman(AmsBackendMock& mock, int slot_count) {
    auto spools = fetch_mock_spools();
    REQUIRE(!spools.empty());

    for (int i = 0; i < slot_count; ++i) {
        SlotInfo slot = mock.get_slot_info(i);
        if (slot.spoolman_id <= 0) {
            continue; // intentionally unlinked lane (untracked path)
        }
        INFO("slot " << i << " -> spoolman_id " << slot.spoolman_id);
        auto it = spools.find(slot.spoolman_id);
        REQUIRE(it != spools.end());
        const SpoolInfo& spool = it->second;

        CHECK(slot.material == spool.material);
        // The Spoolman record still knows the vendor; the slot must not.
        CHECK(slot.brand.empty());
        CHECK(!spool.vendor.empty());
        // Spoolman's filament.name is a filament name, not a colour word, so it
        // lands on spool_name. SlotInfo::color_name stays a colour label and is
        // left unset by apply_spool_to_slot — the label resolver derives one
        // from color_hex when nothing better exists.
        CHECK(slot.spool_name == spool.filament_name);

        uint32_t spool_rgb = 0;
        REQUIRE(helix::parse_hex_color(spool.color_hex.c_str(), spool_rgb));
        CHECK(slot.color_rgb == spool_rgb);

        CHECK(slot.remaining_weight_g == Catch::Approx(spool.remaining_weight_g).margin(0.5));
        CHECK(slot.total_weight_g == Catch::Approx(spool.initial_weight_g).margin(0.5));
    }
}

} // namespace

TEST_CASE("AFC mock slots match mock Spoolman spools (spec §9 drift)",
          "[mock][spoolman][ams_edit_overlay]") {
    AmsBackendMock mock(8);
    mock.set_afc_mode(true);
    check_backend_against_spoolman(mock, 8);
}

TEST_CASE("Happy Hare mock slots match mock Spoolman spools (spec §9 drift)",
          "[mock][spoolman][ams_edit_overlay]") {
    AmsBackendMock mock(8);
    // Happy Hare reports no vendor either: its gate map cannot carry one.
    check_backend_against_spoolman(mock, 8);
}

TEST_CASE("AFC mock keeps one unlinked lane for the untracked path",
          "[mock][spoolman][ams_edit_overlay]") {
    AmsBackendMock mock(4);
    mock.set_afc_mode(true);
    SlotInfo lane3 = mock.get_slot_info(3);
    CHECK(lane3.spoolman_id == 0);
    // Unlinked AND unbranded: no Spoolman record to name it, and AFC reports no
    // vendor, so this lane is the one that must fall back to its color name.
    CHECK(lane3.brand.empty());
    CHECK(lane3.material == "PETG");
}

TEST_CASE("AFC mock lane resolves its brand through the Spoolman identity, not the slot",
          "[mock][spoolman][ams][1264]") {
    // The realism contract this file guards, stated end to end: AFC reports no
    // vendor, so the loaded lane carries none, and the only thing that can name
    // the brand is the Spoolman identity cache. If a future change reseeds a
    // brand onto the AFC slots, the first CHECK here fails and #1264 is silently
    // untestable again.
    AmsBackendMock mock(8);
    mock.set_afc_mode(true);

    const SlotInfo lane0 = mock.get_slot_info(0);
    REQUIRE(lane0.status == SlotStatus::LOADED);
    REQUIRE(lane0.spoolman_id > 0);
    CHECK(lane0.brand.empty());

    auto spools = fetch_mock_spools();
    auto it = spools.find(lane0.spoolman_id);
    REQUIRE(it != spools.end());
    const SpoolInfo& spool = it->second;

    // Cache miss: nothing but the color is left to name the lane.
    const std::string without_identity = helix::resolve_filament_label(lane0, nullptr, "Jet Black");
    CHECK(without_identity.find(spool.vendor) == std::string::npos);

    // Cache hit: the vendor arrives, exactly as SpoolmanManager caches it.
    helix::SpoolIdentity identity;
    identity.vendor = spool.vendor;
    identity.filament_name = spool.filament_name;
    identity.material = spool.material;
    identity.color_hex = spool.color_hex;
    REQUIRE(identity.valid());

    const std::string with_identity = helix::resolve_filament_label(lane0, &identity, "Jet Black");
    CHECK(with_identity.find(spool.vendor) != std::string::npos);
    CHECK(with_identity != without_identity);
}
