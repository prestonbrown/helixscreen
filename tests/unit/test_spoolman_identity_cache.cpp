// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spoolman_identity_cache.cpp
 * @brief SpoolmanManager's transient Spoolman identity cache and its consumer
 *
 * Three layers are pinned here:
 *
 * 1. Cache semantics — populate once per id, negative caching for ids Spoolman
 *    says do not exist, invalidation on edit, clear when Spoolman disappears.
 * 2. The poll path — identity is filled from the record `refresh_spoolman_weights()`
 *    already fetches, weight polling is unaffected by a cache hit, and **nothing**
 *    Spoolman-derived is written onto the slot. That last one is the whole point of
 *    the design: the weight poll writes slots with persist=false to break a G-code
 *    feedback loop, and identity entering SlotInfo would put it back on that path.
 * 3. The label — an AFC-shaped slot renders the Spoolman name instead of the
 *    algorithmic colour word, plus the fallback ladder underneath it.
 */

#include "ui_color_picker.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_display_name.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// TestAccess — the identity cache has no public reset, and the 5s refresh
// debounce has to be defeated to poll twice inside one test (L065: friend class,
// no test-only methods on the production class).
// ============================================================================

class SpoolmanIdentityTestAccess {
  public:
    static void reset(SpoolmanManager& m) {
        // Another test file's deinit_subjects() may have latched the shutdown
        // flag; every identity entry point no-ops while it is set.
        SpoolmanManager::s_shutdown_flag.store(false, std::memory_order_release);
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.identity_cache_.clear();
        m.identity_unresolvable_.clear();
        m.api_ = nullptr;
        m.last_refresh_ms_ = 0;
        m.consecutive_failures_ = 0;
        m.cb_open_ = false;
        m.cb_tripped_at_ms_ = 0;
        m.unavailable_notified_ = false;
        if (m.poll_timer_ != nullptr && lv_is_initialized()) {
            lv_timer_delete(m.poll_timer_);
        }
        m.poll_timer_ = nullptr;
        m.poll_refcount_ = 0;
    }

    /// init_subjects() is idempotent by design, so a prior test file's call
    /// leaves the availability observer bound to a stale run. Re-arm it.
    static void rewire_subjects(SpoolmanManager& m) {
        {
            std::lock_guard<std::recursive_mutex> lock(m.mutex_);
            m.print_state_observer_.reset();
            m.spoolman_availability_observer_.reset();
            m.initialized_ = false;
        }
        m.init_subjects();
    }

    /// refresh_spoolman_weights() debounces itself for 5s; tests poll twice.
    static void clear_debounce(SpoolmanManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.last_refresh_ms_ = 0;
    }

    /// The availability observer is bound by XML name and is silently skipped
    /// when the lookup misses — assert it exists before relying on it.
    static bool observes_availability(SpoolmanManager& m) {
        return m.spoolman_availability_observer_.get() != nullptr;
    }

    static size_t cache_size(SpoolmanManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        return m.identity_cache_.size();
    }
};

using IdTA = SpoolmanIdentityTestAccess;

namespace {

SpoolInfo make_spool(int id, std::string vendor, std::string filament_name, std::string material) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = std::move(vendor);
    // parse_spool_info() puts Spoolman's filament.name in color_name — the
    // conflation the cache reads through. See identity_from_spool().
    spool.filament_name = std::move(filament_name);
    spool.material = std::move(material);
    spool.color_hex = "FFB6C1";
    spool.filament_id = 300 + id;
    spool.vendor_id = 400 + id;
    spool.remaining_weight_g = 850.0;
    spool.initial_weight_g = 1000.0;
    return spool;
}

std::string material_text() {
    return std::string(
        lv_subject_get_string(AmsState::instance().get_current_material_text_subject()));
}

} // namespace

// ============================================================================
// Fixture
// ============================================================================

class IdentityCacheFixture : public LVGLTestFixture {
  public:
    IdentityCacheFixture() {
        get_printer_state().init_subjects(false);
        IdTA::reset(SpoolmanManager::instance());
        AmsState::instance().clear_external_spool_info();
        helix::ui::UpdateQueue::instance().drain();
    }

    ~IdentityCacheFixture() override {
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        IdTA::reset(SpoolmanManager::instance());
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_spoolman_available(bool available) {
        get_printer_state().set_spoolman_available(available);
        helix::ui::UpdateQueue::instance().drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }
};

// ============================================================================
// 1. Cache semantics
// ============================================================================

TEST_CASE_METHOD(IdentityCacheFixture, "SpoolmanManager identity: a miss is nullopt",
                 "[spoolman][identity]") {
    CHECK_FALSE(SpoolmanManager::find_identity(7).has_value());
    CHECK_FALSE(SpoolmanManager::find_identity(0).has_value());
    CHECK_FALSE(SpoolmanManager::find_identity(-1).has_value());
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: extracted once per id, never re-extracted",
                 "[spoolman][identity]") {
    SpoolmanManager::cache_identity(make_spool(7, "Polymaker", "PolyTerra Ambrosia Pink", "PLA"));

    auto first = SpoolmanManager::find_identity(7);
    REQUIRE(first.has_value());
    CHECK(first->vendor == "Polymaker");
    CHECK(first->filament_name == "PolyTerra Ambrosia Pink");
    CHECK(first->material == "PLA");
    CHECK(first->filament_id == 307);
    CHECK(first->vendor_id == 407);

    // A second poll delivers the same id. Identity is immutable in practice, so
    // the record must be left alone — this is the half of the split cadence that
    // makes identity cost one extraction, not one per 30s poll.
    SpoolmanManager::cache_identity(make_spool(7, "eSUN", "Silk Blue", "PETG"));

    auto second = SpoolmanManager::find_identity(7);
    REQUIRE(second.has_value());
    CHECK(second->vendor == "Polymaker");
    CHECK(second->filament_name == "PolyTerra Ambrosia Pink");
    CHECK(second->material == "PLA");
    CHECK(IdTA::cache_size(SpoolmanManager::instance()) == 1);
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a record that names nothing is not cached",
                 "[spoolman][identity]") {
    // Ids and a colour hex cannot name a filament. Caching this would look like
    // a hit to the resolver and would block the next, better record.
    SpoolInfo bare;
    bare.id = 11;
    bare.color_hex = "112233";
    bare.filament_id = 5;
    bare.vendor_id = 6;

    SpoolmanManager::cache_identity(bare);
    CHECK_FALSE(SpoolmanManager::find_identity(11).has_value());

    SpoolmanManager::cache_identity(make_spool(11, "Prusament", "Galaxy Black", "PLA"));
    auto later = SpoolmanManager::find_identity(11);
    REQUIRE(later.has_value());
    CHECK(later->vendor == "Prusament");
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: an unresolvable id stays a miss until a record arrives",
                 "[spoolman][identity]") {
    SpoolmanManager::cache_identity(make_spool(9, "Polymaker", "Ambrosia Pink", "PLA"));
    REQUIRE(SpoolmanManager::find_identity(9).has_value());

    SpoolmanManager::note_identity_unresolvable(9);
    CHECK(SpoolmanManager::is_identity_unresolvable(9));
    CHECK_FALSE(SpoolmanManager::find_identity(9).has_value());

    // A real record is proof the id came back.
    SpoolmanManager::cache_identity(make_spool(9, "eSUN", "Silk Blue", "PETG"));
    CHECK_FALSE(SpoolmanManager::is_identity_unresolvable(9));
    auto id = SpoolmanManager::find_identity(9);
    REQUIRE(id.has_value());
    CHECK(id->vendor == "eSUN");
}

TEST_CASE_METHOD(IdentityCacheFixture, "SpoolmanManager identity: invalidate drops exactly one id",
                 "[spoolman][identity]") {
    SpoolmanManager::cache_identity(make_spool(1, "Polymaker", "Ambrosia Pink", "PLA"));
    SpoolmanManager::cache_identity(make_spool(2, "eSUN", "Silk Blue", "PETG"));
    SpoolmanManager::note_identity_unresolvable(3);

    SpoolmanManager::invalidate_identity(1);

    CHECK_FALSE(SpoolmanManager::find_identity(1).has_value());
    CHECK(SpoolmanManager::find_identity(2).has_value());
    CHECK(SpoolmanManager::is_identity_unresolvable(3));

    // Invalidation also lifts an unresolvable mark, so a re-created spool can
    // be polled again.
    SpoolmanManager::invalidate_identity(3);
    CHECK_FALSE(SpoolmanManager::is_identity_unresolvable(3));
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: cleared when printer_has_spoolman goes 0",
                 "[spoolman][identity]") {
    // SpoolmanManager binds its availability observer to the XML-scope subject
    // named "printer_has_spoolman". PrinterState only publishes that name under
    // init_subjects(register_xml=true), which most of the suite does not do, so
    // publish a subject under the name here — static, because XML scope outlives
    // the test — and re-init the manager so it observes this one deterministically
    // rather than whatever an earlier test file left behind.
    static lv_subject_t availability;
    static bool availability_ready = false;
    if (!availability_ready) {
        lv_subject_init_int(&availability, 1);
        lv_xml_register_subject(nullptr, "printer_has_spoolman", &availability);
        availability_ready = true;
    }
    lv_subject_set_int(&availability, 1);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_spoolman") == &availability);

    SpoolmanManager::instance().deinit_subjects();
    SpoolmanManager::instance().init_subjects();
    REQUIRE(IdTA::observes_availability(SpoolmanManager::instance()));

    SpoolmanManager::cache_identity(make_spool(1, "Polymaker", "Ambrosia Pink", "PLA"));
    SpoolmanManager::note_identity_unresolvable(2);
    REQUIRE(SpoolmanManager::find_identity(1).has_value());
    REQUIRE(SpoolmanManager::is_identity_unresolvable(2));

    // Spoolman disappeared — the same force-stop path that kills the poll timer
    // must drop the cache, or a printer switch would keep serving names from the
    // previous Spoolman.
    lv_subject_set_int(&availability, 0);
    drain(); // observe_int_sync() defers its handler through the UpdateQueue

    CHECK_FALSE(SpoolmanManager::find_identity(1).has_value());
    CHECK_FALSE(SpoolmanManager::is_identity_unresolvable(2));
    CHECK(IdTA::cache_size(SpoolmanManager::instance()) == 0);

    // Leave the shared XML-scope name reading "available" for whatever runs next.
    lv_subject_set_int(&availability, 1);
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a user edit invalidates the spool",
                 "[spoolman][identity][slot_saver]") {
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, get_printer_state());

    SpoolmanManager::cache_identity(make_spool(42, "Polymaker", "Ambrosia Pink", "PLA"));
    SpoolmanManager::cache_identity(make_spool(43, "eSUN", "Silk Blue", "PETG"));
    REQUIRE(SpoolmanManager::find_identity(42).has_value());

    SlotInfo original;
    original.slot_index = 0;
    original.spoolman_id = 42;
    original.brand = "Polymaker";
    original.material = "PLA";
    original.color_rgb = 0xFFB6C1;

    SlotInfo edited = original;
    edited.brand = "Prusament";

    bool completed = false;
    SpoolmanSlotSaver saver(&api);
    // UnlinkLocalOnly completes synchronously without touching Spoolman, which
    // is the cheapest way to drive save()'s single completion funnel.
    saver.save(original, edited, SpoolmanSlotSaver::LinkIntent::UnlinkLocalOnly,
               [&completed](const SaveResult& r) { completed = r.success; });

    CHECK(completed);
    CHECK_FALSE(SpoolmanManager::find_identity(42).has_value());
    // Untouched spools keep their cached identity.
    CHECK(SpoolmanManager::find_identity(43).has_value());
}

// ============================================================================
// 2. The poll path — identity from the fetch that already happens
// ============================================================================

namespace {

/// Wire a mock backend + mock API into the singletons the poll reaches through.
struct PollHarness {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    PollHarness() : api(client, get_printer_state()) {
        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        AmsState::instance().set_backend(std::move(owned));
        SpoolmanManager::instance().set_api(&api);
    }

    void poll() {
        IdTA::clear_debounce(SpoolmanManager::instance());
        SpoolmanManager::instance().refresh_spoolman_weights();
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(
    IdentityCacheFixture,
    "SpoolmanManager identity: the weight poll fills identity and leaves the slot alone",
    "[spoolman][identity]") {
    set_spoolman_available(true);
    PollHarness h;

    SlotInfo before = h.backend->get_slot_info(0);
    before.spoolman_id = 1; // mock spool 1: Polymaker / "Jet Black" / PLA
    before.remaining_weight_g = 111.0f;
    before.total_weight_g = 111.0f;
    h.backend->set_slot_info(0, before);
    before = h.backend->get_slot_info(0);

    REQUIRE_FALSE(SpoolmanManager::find_identity(1).has_value());

    h.poll();

    // Identity landed, from a record the poll was already fetching.
    auto id = SpoolmanManager::find_identity(1);
    REQUIRE(id.has_value());
    CHECK(id->vendor == "Polymaker");
    CHECK(id->filament_name == "Jet Black");
    CHECK(id->material == "PLA");

    // Weight — the thing the poll is actually for — still updated.
    SlotInfo after = h.backend->get_slot_info(0);
    CHECK(after.remaining_weight_g == Catch::Approx(850.0f));
    CHECK(after.total_weight_g == Catch::Approx(1000.0f));

    // ...and NOTHING Spoolman-derived reached the slot. Identity in SlotInfo
    // would re-enter the persist=false quarantine the weight poll depends on.
    CHECK(after.brand == before.brand);
    CHECK(after.spool_name == before.spool_name);
    CHECK(after.color_name == before.color_name);
    CHECK(after.material == before.material);
    CHECK(after.color_rgb == before.color_rgb);
    CHECK(after.spoolman_filament_id == before.spoolman_filament_id);
    CHECK(after.spoolman_vendor_id == before.spoolman_vendor_id);
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a cache hit does not stop weight polling",
                 "[spoolman][identity]") {
    set_spoolman_available(true);
    PollHarness h;

    SlotInfo slot = h.backend->get_slot_info(0);
    slot.spoolman_id = 1;
    slot.remaining_weight_g = 111.0f;
    h.backend->set_slot_info(0, slot);

    h.poll();
    REQUIRE(SpoolmanManager::find_identity(1).has_value());
    REQUIRE(h.backend->get_slot_info(0).remaining_weight_g == Catch::Approx(850.0f));

    // Spoolman reports a lower remaining weight on the next cycle. The cached
    // identity must not short-circuit that: the two have separate cadences.
    for (auto& spool : h.api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 1) {
            spool.remaining_weight_g = 610.0;
        }
    }

    h.poll();

    CHECK(h.backend->get_slot_info(0).remaining_weight_g == Catch::Approx(610.0f));
    auto id = SpoolmanManager::find_identity(1);
    REQUIRE(id.has_value());
    CHECK(id->vendor == "Polymaker"); // still the first extraction
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a spool Spoolman does not have is not polled again",
                 "[spoolman][identity]") {
    set_spoolman_available(true);
    PollHarness h;

    SlotInfo slot = h.backend->get_slot_info(0);
    slot.spoolman_id = 900; // no such spool in the mock inventory
    slot.remaining_weight_g = 111.0f;
    slot.total_weight_g = 111.0f;
    h.backend->set_slot_info(0, slot);

    h.poll();

    CHECK(SpoolmanManager::is_identity_unresolvable(900));
    CHECK_FALSE(SpoolmanManager::find_identity(900).has_value());

    // Make the id resolvable behind the manager's back. If the second poll still
    // issued a request, the weight below would change — it must not, because a
    // known-dead id is skipped before the request is made.
    h.api.spoolman_mock().get_mock_spools().push_back(
        make_spool(900, "Polymaker", "Ambrosia Pink", "PLA"));

    h.poll();

    CHECK(h.backend->get_slot_info(0).remaining_weight_g == Catch::Approx(111.0f));
    CHECK_FALSE(SpoolmanManager::find_identity(900).has_value());

    // The escape hatch: an explicit invalidation lets it be polled again.
    SpoolmanManager::invalidate_identity(900);
    h.poll();

    CHECK(h.backend->get_slot_info(0).remaining_weight_g == Catch::Approx(850.0f));
    CHECK(SpoolmanManager::find_identity(900).has_value());
}

// ============================================================================
// 3. The label, end to end through AmsState
// ============================================================================

namespace {

/// An AFC-shaped slot: firmware gives a filament name and a material, never a
/// brand and never a colour name (ams_backend_afc.cpp:2249).
void make_afc_slot(AmsBackendMock* backend, int spoolman_id) {
    SlotInfo slot = backend->get_slot_info(0);
    slot.spoolman_id = spoolman_id;
    slot.spool_name = "Ambrosia Pink";
    slot.color_name.clear();
    slot.brand.clear();
    slot.material = "PLA";
    slot.color_rgb = 0xFFB6C1; // the hex whose algorithmic name is the bug
    backend->set_slot_info(0, slot);
}

} // namespace

TEST_CASE_METHOD(IdentityCacheFixture,
                 "AmsState label: an AFC slot reads the Spoolman name, not the colour algorithm",
                 "[spoolman][identity][ams]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<AmsBackendMock>(4);
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    REQUIRE(backend->is_filament_loaded());
    REQUIRE(backend->get_current_slot() == 0);

    make_afc_slot(backend, 77);

    // Derive the thing we must NOT see rather than hardcoding it.
    const std::string algorithmic = helix::get_color_name_from_hex(0xFFB6C1);
    REQUIRE_FALSE(algorithmic.empty());

    SECTION("cold cache — firmware name, still not the colour word") {
        ams.sync_current_loaded_from_backend();
        CHECK(material_text() == "Ambrosia Pink PLA");
        CHECK(material_text() != algorithmic + " PLA");
    }

    SECTION("cache hit — Spoolman supplies the brand AFC cannot") {
        SpoolmanManager::cache_identity(make_spool(77, "Polymaker", "Ambrosia Pink", "PLA"));

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "Polymaker Ambrosia Pink PLA");
        CHECK(material_text() != algorithmic + " PLA");
    }

    SECTION("Spoolman down — the label degrades one step, never blanks") {
        SpoolmanManager::cache_identity(make_spool(77, "Polymaker", "Ambrosia Pink", "PLA"));
        ams.sync_current_loaded_from_backend();
        REQUIRE(material_text() == "Polymaker Ambrosia Pink PLA");

        SpoolmanManager::clear_identity_cache();
        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "Ambrosia Pink PLA");
        CHECK_FALSE(material_text().empty());
    }

    SECTION("spool deleted — unresolvable id is a miss, not a blank") {
        SpoolmanManager::note_identity_unresolvable(77);
        ams.sync_current_loaded_from_backend();
        CHECK(material_text() == "Ambrosia Pink PLA");
    }

    SECTION("no spool id — unchanged from firmware-only behaviour") {
        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = 0;
        backend->set_slot_info(0, slot);

        // A cached identity for the id the slot no longer carries must not leak in.
        SpoolmanManager::cache_identity(make_spool(77, "Polymaker", "Ambrosia Pink", "PLA"));
        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "Ambrosia Pink PLA");
    }

    SECTION("no name anywhere — the algorithmic colour name is the last naming layer") {
        SlotInfo slot = backend->get_slot_info(0);
        slot.spool_name.clear();
        slot.color_name.clear();
        slot.brand.clear();
        backend->set_slot_info(0, slot);

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == algorithmic + " PLA");
    }

    SECTION("nothing at all — never blank") {
        SlotInfo slot = backend->get_slot_info(0);
        slot.spool_name.clear();
        slot.color_name.clear();
        slot.brand.clear();
        slot.material.clear();
        slot.color_rgb = 0;
        backend->set_slot_info(0, slot);

        ams.sync_current_loaded_from_backend();

        CHECK_FALSE(material_text().empty());
    }

    SECTION("a user override outranks the cached Spoolman identity") {
        // apply_overrides() has already merged the override onto the slot by the
        // time the label is built, so the override arrives as slot fields.
        SlotInfo slot = backend->get_slot_info(0);
        slot.spool_name = "Shop Reload";
        slot.brand = "House Brand";
        backend->set_slot_info(0, slot);

        SpoolmanManager::cache_identity(make_spool(77, "Polymaker", "Ambrosia Pink", "PLA"));
        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "House Brand Shop Reload PLA");
    }

    ams.clear_backends();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "AmsState label: the bypass card uses the same resolver and keeps its wording",
                 "[spoolman][identity][ams]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<AmsBackendMock>(4);
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    // enable_bypass() needs a started backend; the default "idle" scenario
    // spawns no thread, so this stays a synchronous test.
    REQUIRE(backend->start().success());
    REQUIRE(backend->enable_bypass().success());
    REQUIRE(backend->is_bypass_active());

    SECTION("an external spool with nothing but a colour falls back to the colour name") {
        SlotInfo ext;
        ext.slot_index = -2;
        ext.global_index = -2;
        ext.color_rgb = 0xFFB6C1;
        ext.material = "PLA";
        ams.set_external_spool_info_in_memory(ext);

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == helix::get_color_name_from_hex(0xFFB6C1) + " PLA");
    }

    SECTION("bypass with no external spool keeps the card's own wording") {
        // The no-spool branch is separate code that must survive the resolver
        // swap; the resolver's own last_resort is the same translated string, so
        // either way the card never goes blank.
        ams.clear_external_spool_info();

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == std::string(lv_tr("External")));
    }

    SECTION("the cached identity reaches the bypass card too") {
        SlotInfo ext;
        ext.slot_index = -2;
        ext.global_index = -2;
        ext.spoolman_id = 55;
        ext.color_rgb = 0xFFB6C1;
        ext.material = "PLA";
        ams.set_external_spool_info_in_memory(ext);

        SpoolmanManager::cache_identity(make_spool(55, "Polymaker", "Ambrosia Pink", "PLA"));
        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "Polymaker Ambrosia Pink PLA");
    }

    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// 4. Arming the poll, and telling the UI when an identity lands
// ============================================================================

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager polling: a request made before Spoolman is available still arms",
                 "[spoolman][identity][polling]") {
    // At boot the Home panel activates synchronously inside init_ui(), while
    // printer_has_spoolman is still 0 because set_spoolman_available() defers
    // through the UpdateQueue. The wish to poll therefore always arrives before
    // the ability to serve it, and discarding it left the Home panel polling
    // nothing for the rest of the session.
    set_spoolman_available(false);
    IdTA::rewire_subjects(SpoolmanManager::instance());
    REQUIRE(IdTA::observes_availability(SpoolmanManager::instance()));

    PollHarness h;
    SlotInfo slot = h.backend->get_slot_info(0);
    slot.spoolman_id = 1;
    slot.remaining_weight_g = 111.0f;
    h.backend->set_slot_info(0, slot);

    SpoolmanManager::instance().start_spoolman_polling();
    drain();
    CHECK_FALSE(SpoolmanManager::find_identity(1).has_value()); // nothing to poll yet

    set_spoolman_available(true);
    // The poll runs inside the availability drain and re-queues its API
    // callback, so the identity lands on the following pass.
    drain();
    drain();

    // The deferred request is honoured the moment Spoolman shows up.
    CHECK(SpoolmanManager::find_identity(1).has_value());

    SpoolmanManager::instance().stop_spoolman_polling();
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a new identity refreshes labels even when the weight "
                 "never moves",
                 "[spoolman][identity][1264]") {
    // cache_identity() deliberately runs before the weights-unchanged early
    // return, so the name lands. Nothing then told the UI to recompose, so the
    // Active Spool widget kept rendering the pre-identity label.
    set_spoolman_available(true);
    PollHarness h;

    SlotInfo slot = h.backend->get_slot_info(0);
    slot.spoolman_id = 1;
    // Exactly what mock spool 1 reports, so the early return fires.
    slot.remaining_weight_g = 850.0f;
    slot.total_weight_g = 1000.0f;
    h.backend->set_slot_info(0, slot);

    const int before = lv_subject_get_int(AmsState::instance().get_slots_version_subject());
    REQUIRE_FALSE(SpoolmanManager::find_identity(1).has_value());

    h.poll();

    REQUIRE(SpoolmanManager::find_identity(1).has_value());
    CHECK(lv_subject_get_int(AmsState::instance().get_slots_version_subject()) > before);
}

TEST_CASE_METHOD(IdentityCacheFixture,
                 "SpoolmanManager identity: a poll that learns nothing new does not churn the UI",
                 "[spoolman][identity][1264]") {
    // The other half of the contract. Bumping on every poll would re-enter the
    // refresh cascade the weights-unchanged early return exists to prevent.
    set_spoolman_available(true);
    PollHarness h;

    SlotInfo slot = h.backend->get_slot_info(0);
    slot.spoolman_id = 1;
    slot.remaining_weight_g = 850.0f;
    slot.total_weight_g = 1000.0f;
    h.backend->set_slot_info(0, slot);

    h.poll();
    REQUIRE(SpoolmanManager::find_identity(1).has_value());

    const int settled = lv_subject_get_int(AmsState::instance().get_slots_version_subject());
    h.poll(); // identity already cached, weights still unchanged
    CHECK(lv_subject_get_int(AmsState::instance().get_slots_version_subject()) == settled);
}
