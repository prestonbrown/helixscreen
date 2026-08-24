// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_remap_restore_confirmation.cpp
 * @brief PrintStartController::restore_filament_mapping() must not treat a
 *        restore it never delivered as done (#1270).
 *
 * Run with: ./build/bin/helix-tests "[remap-restore]"
 *
 * Background: every native backend's set_tool_mapping() routes into
 * AmsSubscriptionBackend::execute_gcode(), which fires the RPC and returns
 * AmsErrorHelper::success() unconditionally — failures land in an async callback
 * that only logs. The controller branched on that return value, so a command
 * Klipper refused counted as a restore. It then cleared the in-memory snapshot
 * AND deleted pending_remap.json, destroying the record crash recovery replays.
 *
 * A halted Klipper at print end is the normal shape of a cancelled or errored
 * print, which is exactly when restore runs — so this was not a rare path.
 */

#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/cfs_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "slot_registry.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

// Counts the restore commands the controller actually dispatches. Returns
// SUCCESS the way the real backends do (execute_gcode is fire-and-forget), so
// the test pins the CONTROLLER's behavior rather than a mock that reports
// failures the production code would never see.
class CountingAfcBackend : public AmsBackendAfc {
  public:
    CountingAfcBackend() : AmsBackendAfc(nullptr, nullptr) {}

    AmsError set_tool_mapping(int tool_number, int slot_index) override {
        calls.push_back({tool_number, slot_index});
        return AmsErrorHelper::success();
    }

    std::vector<int> get_tool_mapping() const override {
        return current;
    }

    // Confirmation support is opt-in per test. Real AFC returns true, but the
    // delivery-gate tests below are about backends in general, so they run with
    // it off and pin the can't-confirm fallback. The confirmation tests turn it
    // on explicitly.
    bool reports_firmware_tool_mapping() const override {
        return echoes_firmware;
    }

    uint64_t firmware_tool_mapping_generation() const override {
        return generation;
    }

    /// Simulate a firmware report landing: the printer tells us `mapping`.
    void firmware_reports(std::vector<int> mapping) {
        current = std::move(mapping);
        ++generation;
    }

    /// Simulate an OPTIMISTIC local write — the shape that must never confirm.
    void locally_assume(std::vector<int> mapping) {
        current = std::move(mapping);
    }

    struct Call {
        int tool;
        int slot;
    };
    std::vector<Call> calls;
    std::vector<int> current;
    bool echoes_firmware = false;
    uint64_t generation = 0;
};

// Installs a counting backend at index 0 and removes it on scope exit.
struct ScopedCountingBackend {
    CountingAfcBackend* backend = nullptr;

    explicit ScopedCountingBackend(std::vector<int> current_mapping) {
        // The confirmation observer hangs off AmsState's data-revision subject.
        // Without init_subjects() that subject is never initialized, so it
        // notifies nobody and every confirmation silently fails to arrive.
        AmsState::instance().init_subjects(false);
        auto be = std::make_unique<CountingAfcBackend>();
        be->current = std::move(current_mapping);
        backend = be.get();
        AmsState::instance().set_backend(std::move(be));
    }
    ~ScopedCountingBackend() {
        AmsState::instance().set_backend(nullptr);
    }
};

// Controller + the PrinterState it observes, wired the way the panel does.
struct Harness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    MoonrakerAPIMock api{client, ps};
    PrintStartController controller{ps, &api};

    Harness() {
        ps.init_subjects(false);
    }

    /// Tick AmsState's data-revision subject the way a synced backend event
    /// does, then drain so the observer body actually runs.
    static void ams_data_tick() {
        auto* rev = AmsState::instance().get_ams_data_revision_subject();
        lv_subject_set_int(rev, lv_subject_get_int(rev) + 1);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_klippy(KlippyState state) {
        // _sync writes the subject without the async hop, but observe_int_sync's
        // handler still lands on the UpdateQueue (observer_factory.h:371), so the
        // deferred-restore observer only runs on a drain. Skipping it makes the
        // retry look like it never fired.
        ps.set_klippy_state_sync(state);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

// ============================================================================
// The #1270 failure: Klipper refuses everything, controller declares victory
// ============================================================================

TEST_CASE("remap restore: halted Klipper does not consume the saved mapping",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::SHUTDOWN);

    // Print ended with T0->slot 2, T1->slot 1 needing to go back.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    // Nothing can be delivered to a halted Klipper, so nothing should be sent...
    CHECK(be.backend->calls.empty());
    // ...and above all the snapshot must survive. Clearing it here is what
    // stranded the printer on the print's mapping with no record of the real one.
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == 0);
}

TEST_CASE("remap restore: klippy ERROR and STARTUP are also not delivery",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;

    auto state = GENERATE(KlippyState::ERROR, KlippyState::STARTUP);

    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(state);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    CHECK(be.backend->calls.empty());
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

// ============================================================================
// The gate must not break the normal path
// ============================================================================

TEST_CASE("remap restore: ready Klipper restores and consumes the snapshot",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}}; // firmware currently on the print's mapping
    Harness h;
    h.set_klippy(KlippyState::READY);

    // Saved (pre-print) mapping differs from current in both slots.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    REQUIRE(be.backend->calls.size() == 2);
    CHECK(be.backend->calls[0].tool == 0);
    CHECK(be.backend->calls[0].slot == 2);
    CHECK(be.backend->calls[1].tool == 1);
    CHECK(be.backend->calls[1].slot == 1);

    // Delivered to a ready Klipper — snapshot is spent.
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == -1);
}

TEST_CASE("remap restore: ready Klipper with nothing to change still clears",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{2, 1}}; // firmware already matches the saved mapping
    Harness h;
    h.set_klippy(KlippyState::READY);

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    CHECK(be.backend->calls.empty()); // no diff, nothing to send
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

// ============================================================================
// Deferral has to actually resolve, or the gate just leaks the snapshot
// ============================================================================

TEST_CASE("remap restore: deferred restore fires when Klipper becomes ready",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::SHUTDOWN);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.empty());

    // Klipper comes back (FIRMWARE_RESTART, or the user clears the shutdown).
    // The pending restore must go out on its own — otherwise the snapshot is
    // retained forever and only a full app restart would replay it.
    h.set_klippy(KlippyState::READY);

    CHECK(be.backend->calls.size() == 2);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

// ============================================================================
// Firmware confirmation: the send is not the proof
// ============================================================================

TEST_CASE("remap restore: echoing backend waits for firmware before clearing",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    // Commands went out to a ready Klipper...
    REQUIRE(be.backend->calls.size() == 2);
    // ...but a send is not proof the firmware applied them, so the record stays.
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

TEST_CASE("remap restore: our own optimistic write never counts as confirmation",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);

    // THE trap this whole seam exists for: every backend updates its registry
    // optimistically inside set_tool_mapping(), so the mapping reads correct
    // immediately even when Klipper refused the command. Matching values with an
    // unmoved generation must NOT be accepted as proof.
    be.backend->locally_assume({2, 1});
    Harness::ams_data_tick();

    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

TEST_CASE("remap restore: firmware report matching the snapshot confirms it",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    be.backend->firmware_reports({2, 1});
    Harness::ams_data_tick();

    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == -1);
}

TEST_CASE("remap restore: firmware reporting the wrong mapping keeps waiting",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);

    // A multi-lane restore lands one delta at a time, so a partial match is a
    // normal intermediate state — keep the record rather than declaring failure.
    be.backend->firmware_reports({2, 2});
    Harness::ams_data_tick();
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    // The rest arrives.
    be.backend->firmware_reports({2, 1});
    Harness::ams_data_tick();
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

// ============================================================================
// Per-backend echo capability (#1270). Which backends can confirm at all was
// established by reading firmware sources and probing hardware, not assumed:
//   AFC  — per-lane `map` over the subscription
//   HH   — whole ttg_map in get_status() (mmu.py)
//   CFS  — box.map, measured on a live K2 (single-key delta, ~0.7s) — EXCEPT K1,
//          where BOX_MODIFY_TN no-ops (#968) so no confirming frame ever arrives
//   AD5X — zmod_ifs.py has no get_status at all; nothing to confirm against
// A backend that wrongly claims support waits forever and strands the record.
// ============================================================================

TEST_CASE("remap restore: echoing backends advertise confirmation support",
          "[remap-restore][1270]") {
    AmsBackendAfc afc{nullptr, nullptr};
    CHECK(afc.reports_firmware_tool_mapping());

    AmsBackendHappyHare hh{nullptr, nullptr};
    CHECK(hh.reports_firmware_tool_mapping());

    // AD5X IFS cannot: ZMOD publishes no status object for the mapping.
    AmsBackendAd5xIfs ifs{nullptr, nullptr};
    CHECK_FALSE(ifs.reports_firmware_tool_mapping());
}

TEST_CASE("remap restore: CFS confirms except on K1 where BOX_MODIFY_TN no-ops",
          "[remap-restore][1270]") {
    helix::printer::AmsBackendCfs cfs{nullptr, nullptr};
    CHECK(cfs.reports_firmware_tool_mapping()); // K2 default

    CfsTestAccess::set_macro_variant_k1(cfs);
    CHECK_FALSE(cfs.reports_firmware_tool_mapping());
}

// ============================================================================
// The registry seam itself: only firmware writes move the generation.
// ============================================================================

TEST_CASE("remap restore: only firmware-sourced registry writes bump the generation",
          "[remap-restore][1270]") {
    using helix::printer::SlotRegistry;
    SlotRegistry reg;
    reg.initialize("unit", {"a", "b", "c"});

    const uint64_t start = reg.firmware_mapping_generation();

    // Per-slot optimistic write (what set_tool_mapping() does before sending).
    reg.set_tool_mapping(0, 0);
    CHECK(reg.firmware_mapping_generation() == start);

    // Per-slot firmware write (AFC's subscription parser).
    reg.set_tool_mapping(1, 1, SlotRegistry::MappingSource::Firmware);
    CHECK(reg.firmware_mapping_generation() == start + 1);

    // Bulk optimistic write.
    reg.set_tool_map({0, 1, 2});
    CHECK(reg.firmware_mapping_generation() == start + 1);

    // Bulk firmware write (Happy Hare's whole ttg_map).
    reg.set_tool_map({2, 1, 0}, SlotRegistry::MappingSource::Firmware);
    CHECK(reg.firmware_mapping_generation() == start + 2);

    // A rejected write must not count as confirmation.
    reg.set_tool_mapping(99, 0, SlotRegistry::MappingSource::Firmware);
    CHECK(reg.firmware_mapping_generation() == start + 2);
}
