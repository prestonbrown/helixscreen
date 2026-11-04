// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A lane whose tag names a Spoolman spool, through AmsState: the lookup, the
// link committed as a pick commits it, and the toasts.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tag_reading.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kTaggedSpool = 4242;
constexpr int kOtherSpool = 4243;
constexpr int kMissingSpool = 98765;

/// A mock whose slots' tags name the spools a test sets.
class TaggedMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    [[nodiscard]] std::optional<ams::TagReading> tag_reading(int slot_index) const override {
        auto it = tag_ids.find(slot_index);
        if (it == tag_ids.end()) {
            return std::nullopt;
        }
        ams::TagReading reading;
        reading.spoolman_id = it->second;
        return reading;
    }

    [[nodiscard]] bool tags_name_spoolman_spools() const override {
        return true;
    }

    /// The unit as a box off the bus reports it: absent, its bays placeholders
    /// that read empty.
    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (unit_absent && !info.units.empty()) {
            info.units[0].absent = true;
            for (auto& slot : info.units[0].slots) {
                slot.status = SlotStatus::EMPTY;
            }
        }
        return info;
    }

    std::map<int, int> tag_ids;
    bool unit_absent = false;
};

SpoolInfo make_spool(int id, std::string vendor) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = std::move(vendor);
    spool.filament_name = "Galaxy Black";
    spool.material = "PETG";
    spool.color_hex = "1A1A1A";
    spool.filament_id = 300 + id;
    spool.vendor_id = 400 + id;
    spool.remaining_weight_g = 850.0;
    spool.initial_weight_g = 1000.0;
    return spool;
}

int count_containing(const std::vector<std::string>& msgs, const std::string& needle) {
    int n = 0;
    for (const auto& m : msgs) {
        if (m.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

struct TagLinkFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    TaggedMock* backend = nullptr;
    std::vector<std::string> warnings;
    std::vector<std::string> infos;

    TagLinkFixture() : api(client, get_printer_state()) {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_ams_tag_links_spoolman(true);
        set_spoolman_available(true);

        client.spoolman_mock().get_mock_spools().push_back(make_spool(kTaggedSpool, "Prusament"));
        client.spoolman_mock().get_mock_spools().push_back(make_spool(kOtherSpool, "Sunlu"));

        auto owned = std::make_unique<TaggedMock>(4);
        backend = owned.get();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);

        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = 0;
        backend->sync_external_identity(0, slot);
        backend->force_slot_status(0, SlotStatus::AVAILABLE);

        ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
        ui::set_test_notification_info_hook(
            [this](const std::string& msg) { infos.push_back(msg); });
    }

    ~TagLinkFixture() override {
        ui::set_test_notification_warning_hook(nullptr);
        ui::set_test_notification_info_hook(nullptr);
        SettingsManager::instance().set_ams_tag_links_spoolman(true);
        set_spoolman_available(false);
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        ui::UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    /// The capability lands through the update queue.
    static void set_spoolman_available(bool available) {
        get_printer_state().capabilities_state().set_spoolman_available(available);
        ui::UpdateQueue::instance().drain();
    }

    /// One status frame, and whatever it queued.
    void frame() {
        AmsStateTestAccess::backend_frame(AmsState::instance());
        ui::UpdateQueue::instance().drain();
    }

    /// The spool leaves slot 0, and one comes back whose tag names @p tag.
    void insert(std::optional<int> tag) {
        backend->tag_ids.erase(0);
        backend->force_slot_status(0, SlotStatus::EMPTY);
        frame();
        if (tag) {
            backend->tag_ids[0] = *tag;
        }
        backend->force_slot_status(0, SlotStatus::AVAILABLE);
        frame();
    }

    [[nodiscard]] int linked() const {
        return backend->get_slot_info(0).spoolman_id;
    }
};

} // namespace

TEST_CASE_METHOD(TagLinkFixture, "a tag naming a Spoolman spool links its lane like a pick",
                 "[ams][tag_link]") {
    REQUIRE(linked() == 0);
    insert(kTaggedSpool);

    const SlotInfo slot = backend->get_slot_info(0);
    CHECK(slot.spoolman_id == kTaggedSpool);
    CHECK(slot.brand == "Prusament");
    CHECK(slot.material == "PETG");
    CHECK(count_containing(infos, "#" + std::to_string(kTaggedSpool)) == 1);
    CHECK(warnings.empty());
}

TEST_CASE_METHOD(TagLinkFixture, "a new insert's tag replaces the lane's link", "[ams][tag_link]") {
    insert(kTaggedSpool);
    REQUIRE(linked() == kTaggedSpool);
    insert(kOtherSpool);
    CHECK(linked() == kOtherSpool);
}

TEST_CASE_METHOD(TagLinkFixture, "a tag naming no spool warns once and leaves the lane",
                 "[ams][tag_link]") {
    insert(kMissingSpool);
    frame();
    frame();
    CHECK(linked() == 0);
    CHECK(count_containing(warnings, "#" + std::to_string(kMissingSpool)) == 1);
    CHECK(infos.empty());
}

TEST_CASE_METHOD(TagLinkFixture, "a spool picked while the tag stays seated is kept",
                 "[ams][tag_link]") {
    insert(kTaggedSpool);
    REQUIRE(linked() == kTaggedSpool);

    const auto pick = AmsState::instance().link_spool_to_slot(0, make_spool(kOtherSpool, "Sunlu"));
    REQUIRE(pick.error.success());
    frame();
    frame();
    CHECK(linked() == kOtherSpool);
    CHECK(count_containing(infos, "#" + std::to_string(kTaggedSpool)) == 1);
}

TEST_CASE_METHOD(TagLinkFixture,
                 "the setting turned off links nothing, and turning it on "
                 "later does not link a spool already seated",
                 "[ams][tag_link]") {
    SettingsManager::instance().set_ams_tag_links_spoolman(false);
    insert(kTaggedSpool);
    CHECK(linked() == 0);

    SettingsManager::instance().set_ams_tag_links_spoolman(true);
    frame();
    CHECK(linked() == 0);
    CHECK(infos.empty());
}

TEST_CASE_METHOD(TagLinkFixture, "a spool seated while Spoolman is away links when it returns",
                 "[ams][tag_link]") {
    set_spoolman_available(false);
    insert(kTaggedSpool);
    CHECK(linked() == 0);

    set_spoolman_available(true);
    frame();
    CHECK(linked() == kTaggedSpool);
}

TEST_CASE_METHOD(TagLinkFixture, "a tag naming no spool leaves the lane alone", "[ams][tag_link]") {
    insert(std::nullopt);
    CHECK(linked() == 0);
    CHECK(infos.empty());
    CHECK(warnings.empty());
}

TEST_CASE_METHOD(TagLinkFixture, "a box dropping off the bus and back is not a new insert",
                 "[ams][tag_link]") {
    SECTION("a pick made while the tag was seated survives") {
        insert(kTaggedSpool);
        REQUIRE(linked() == kTaggedSpool);
        REQUIRE(AmsState::instance()
                    .link_spool_to_slot(0, make_spool(kOtherSpool, "Sunlu"))
                    .error.success());
        frame();
        REQUIRE(linked() == kOtherSpool);

        backend->unit_absent = true;
        frame();
        backend->unit_absent = false;
        frame();
        CHECK(linked() == kOtherSpool);
        CHECK(count_containing(infos, "#" + std::to_string(kTaggedSpool)) == 1);
    }

    SECTION("a tag naming no spool warns only once") {
        insert(kMissingSpool);
        REQUIRE(count_containing(warnings, "#" + std::to_string(kMissingSpool)) == 1);

        backend->unit_absent = true;
        frame();
        backend->unit_absent = false;
        frame();
        CHECK(count_containing(warnings, "#" + std::to_string(kMissingSpool)) == 1);
    }
}

TEST_CASE_METHOD(TagLinkFixture, "a missing spool seated across a restart does not warn again",
                 "[ams][tag_link]") {
    backend->tag_ids[0] = kMissingSpool;
    frame();
    frame();
    CHECK(linked() == 0);
    CHECK(warnings.empty());
}

TEST_CASE_METHOD(TagLinkFixture, "a new backend starts with no tag link left over from the last",
                 "[ams][tag_link]") {
    // A want for the old backend's lane 0, waiting on Spoolman.
    set_spoolman_available(false);
    insert(kTaggedSpool);
    REQUIRE(linked() == 0);

    // The new backend's lane 0 holds the same tag but is linked elsewhere: a
    // first sight there links nothing, where the old want would.
    auto owned = std::make_unique<TaggedMock>(4);
    TaggedMock* next = owned.get();
    next->tag_ids[0] = kTaggedSpool;
    SlotInfo slot = next->get_slot_info(0);
    slot.spoolman_id = kOtherSpool;
    next->sync_external_identity(0, slot);
    next->force_slot_status(0, SlotStatus::AVAILABLE);
    AmsState::instance().set_backend(std::move(owned));
    backend = next;

    set_spoolman_available(true);
    frame();
    frame();
    CHECK(linked() == kOtherSpool);
}

TEST_CASE_METHOD(TagLinkFixture, "a tag arriving in a single-slot update links its lane",
                 "[ams][tag_link]") {
    backend->force_slot_status(0, SlotStatus::EMPTY);
    frame();
    backend->tag_ids[0] = kTaggedSpool;
    backend->force_slot_status(0, SlotStatus::AVAILABLE);
    AmsState::instance().update_slot_for_backend(0, 0);
    ui::UpdateQueue::instance().drain();
    CHECK(linked() == kTaggedSpool);
}

TEST_CASE_METHOD(TagLinkFixture, "a single-slot update from a box off the bus is not a removal",
                 "[ams][tag_link]") {
    insert(kTaggedSpool);
    REQUIRE(linked() == kTaggedSpool);
    REQUIRE(AmsState::instance()
                .link_spool_to_slot(0, make_spool(kOtherSpool, "Sunlu"))
                .error.success());
    frame();
    REQUIRE(linked() == kOtherSpool);

    backend->unit_absent = true;
    backend->force_slot_status(0, SlotStatus::EMPTY);
    frame();
    AmsState::instance().update_slot_for_backend(0, 0);
    ui::UpdateQueue::instance().drain();

    backend->unit_absent = false;
    backend->force_slot_status(0, SlotStatus::AVAILABLE);
    frame();
    CHECK(linked() == kOtherSpool);
}
