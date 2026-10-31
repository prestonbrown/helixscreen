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

#include "ui_ams_lane_settings.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"
#include "ui_resume_dispatch.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/cfs_test_access.h"
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "config.h"
#include "data_root_resolver.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_job_ref.h"
#include "printer_cache_registry.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "slot_registry.h"
#include "static_panel_registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

// Counts the restore commands the controller actually dispatches. Returns
// SUCCESS the way the real backends do (execute_gcode is fire-and-forget), so
// the default mode pins the CONTROLLER's behavior rather than a mock that
// reports failures the production code would never see. `refuse_remaps`
// returns synchronous refusals instead — the shape a slot bound produces for
// a lane on a unit the box is not reporting.
class CountingAfcBackend : public AmsBackendAfc {
  public:
    CountingAfcBackend() : AmsBackendAfc(nullptr, nullptr) {}

    AmsError can_set_tool_mapping(int, int slot_index) const override {
        if (refuse_remaps) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, 3);
        }
        if (reported_slots >= 0 && slot_index >= reported_slots) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, reported_slots - 1);
        }
        return AmsErrorHelper::success();
    }

    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override {
        calls.push_back({tool_number, slot_index});
        if (refuse_past_check) {
            return AmsErrorHelper::not_supported("write failed after the checks");
        }
        return can_set_tool_mapping(tool_number, slot_index);
    }

    // The lanes the backend reports, when a test sets reported_slots.
    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendAfc::get_system_info();
        if (reported_slots >= 0) {
            info.total_slots = reported_slots;
        }
        return info;
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
    bool refuse_remaps = false;
    int reported_slots = -1; ///< lanes past this are refused, as a detached unit's are
    /// Refuse after can_set_tool_mapping() passes, as an AD5X write_ifs_var failure does.
    bool refuse_past_check = false;
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
        AmsState::instance().init_subjects(true);
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
    // A restart builds the process-wide print-select panel around this api.
    ~Harness() {
        StaticPanelRegistry::instance().destroy_all();
    }

    /// Tick AmsState's data-revision subject the way a synced backend event
    /// does, then drain so the observer body actually runs.
    static void ams_data_tick() {
        auto* rev = AmsState::instance().get_ams_data_revision_subject();
        lv_subject_set_int(rev, lv_subject_get_int(rev) + 1);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_klippy(KlippyState state) {
        // _sync writes the subject without the async hop, but observe<int>'s
        // handler still lands on the UpdateQueue (observer_factory.h:371), so the
        // deferred-restore observer only runs on a drain. Skipping it makes the
        // retry look like it never fired.
        ps.set_klippy_state_sync(state);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Drive print_stats through the real parse the way Moonraker deltas do.
    /// Hand-writing the subjects would skip publish_lifecycle_state(), whose
    /// derivation is exactly what the #1386 tests exercise.
    void report(const std::string& job_state, const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"state", job_state}, {"filename", filename}}}};
        ps.update_from_status(status);
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
    auto state = GENERATE(KlippyState::SHUTDOWN, KlippyState::ERROR, KlippyState::STARTUP);
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(state);

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

// ============================================================================
// A refused send is not a delivered send. A slot bound refuses a lane on a
// detached unit: the box got smaller mid-print, and the snapshot still names
// the missing bay. The record must survive so the next startup's replay
// retries the refused entries and skips the ones that took — a reattached
// unit gets its routing back. Clearing here makes a temporary detach a
// permanent loss of the pre-print mapping.
// ============================================================================

TEST_CASE("remap restore: refused sends retain the record for replay", "[remap-restore][1623]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_refused"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->refuse_remaps = true;
    Harness h;
    h.set_klippy(KlippyState::READY);

    // Echoing backends take the "restores_sent == 0" branch; no-echo backends
    // (K1) take the send-as-delivery branch. Both clear through
    // finish_restore(), so both must retain on refusal.
    SECTION("echoing backend") {
        be.backend->echoes_firmware = true;
    }
    SECTION("no-echo backend") {}

    // The crash-recovery record on disk, the way persist_remap_state() writes
    // it — the file recover_pending_remap() replays on the next startup.
    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    std::filesystem::create_directories(record.parent_path());
    {
        std::ofstream ofs(record);
        ofs << "{\"backend_index\":0,\"tool_mapping\":[2,1]}";
    }

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);

    REQUIRE(be.backend->calls.size() == 2); // both differing entries attempted
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == 0);
    CHECK(std::filesystem::exists(record));
}

// A retained record retries once the backend would accept every refused entry,
// which is when a reattached unit's lanes are reported again. Klipper READY
// alone comes before that report, and a job holding the machine must not have
// its tools re-routed under it.
TEST_CASE("remap restore: a refused record retries once the backend accepts it",
          "[remap-restore][1684]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_retry_accepts"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->reported_slots = 2; // the unit carrying lane 2 is detached
    Harness h;
    h.set_klippy(KlippyState::READY);

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(be.backend->calls.size() == 2); // T0->2 refused, T1->1 sent
    REQUIRE_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    SECTION("a Klipper restart and data that leaves the lanes alone retry nothing") {
        h.set_klippy(KlippyState::STARTUP);
        h.set_klippy(KlippyState::READY);
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 2);

        be.backend->reported_slots = 4;
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 4);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    }

    SECTION("the lanes returning mid-print wait for the job to end") {
        h.report("printing", "benchy.gcode");
        be.backend->reported_slots = 4;
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 2);

        h.report("paused", "benchy.gcode");
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 2);

        h.report("complete", "benchy.gcode");
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 4);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    }
}

TEST_CASE("remap restore: a new print keeps the retained snapshot and disarms its retry",
          "[remap-restore][1684][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_retry_new_print"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->reported_slots = 2;
    Harness h;
    h.set_klippy(KlippyState::READY);

    // Print A's restore is refused, and its retry is armed.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(be.backend->calls.size() == 2);

    // Print B starts with an explicit remap while the lanes are back, still at
    // Idle: apply_filament_remaps runs before the print is prepared.
    be.backend->reported_slots = 4;
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    helix::ToolMapping remap;
    remap.tool_index = 0;
    remap.mapped_slot = 3;
    remap.mapped_backend = 0;
    remap.user_picked = true;
    view.set_filament_mappings({remap});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));
    REQUIRE(be.backend->calls.size() == 3);
    // The live table still holds A's map, so A's original stays the snapshot.
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});

    // B's remap lands and bumps the data revision. A's retry is gone: nothing
    // may be sent over B's mapping while B is about to print.
    be.backend->firmware_reports({3, 2});
    Harness::ams_data_tick();
    CHECK(be.backend->calls.size() == 3);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});
    h.controller.set_detail_view(nullptr);
}

// ============================================================================
// An unrestored map is still the printer's own (prestonbrown/helixscreen#1859)
// ============================================================================

namespace {
helix::ToolMapping
row(int tool, int slot, bool picked,
    helix::ToolMapping::MatchReason reason = helix::ToolMapping::MatchReason::FIRMWARE_MAPPING) {
    helix::ToolMapping m;
    m.tool_index = tool;
    m.mapped_slot = slot;
    m.mapped_backend = 0;
    m.user_picked = picked;
    m.reason = reason;
    return m;
}
} // namespace

TEST_CASE("remap restore: a deferred restore is the next print's snapshot and seed",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_deferred_next_print"};
    // Print A ran on {1, 2}; the printer's own map is {2, 1}. The table has
    // been reported by firmware, so it is the seed's routing.
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    be.backend->firmware_reports({1, 2});
    Harness h;
    h.set_klippy(KlippyState::SHUTDOWN);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.empty());

    // The seed routes by the printer's own map, not A's.
    const auto routing = AmsState::instance().collect_firmware_routing();
    CHECK(routing.head(0) == 2);
    CHECK(routing.head(1) == 1);

    // Print B: the user picks T1 onto lane 3; T0 is the seed's, on its own lane.
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({row(0, 2, false), row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));

    REQUIRE(be.backend->calls.size() == 2);
    CHECK(be.backend->calls[0].tool == 0);
    CHECK(be.backend->calls[0].slot == 2); // T0 back on the lane the seed showed
    CHECK(be.backend->calls[1].tool == 1);
    CHECK(be.backend->calls[1].slot == 3);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});

    // The deferral's retry belonged to A: Klipper coming back sends nothing.
    h.set_klippy(KlippyState::READY);
    CHECK(be.backend->calls.size() == 2);
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: another controller's print start stands down a deferred restore",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_two_controllers"};
    ScopedCountingBackend be{{1, 2}};
    Harness h; // controller A, whose restore is waiting on Klipper
    h.set_klippy(KlippyState::SHUTDOWN);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.empty());

    // Controller B (a queued-job start) remaps T1 for its own print.
    PrintStartController b{h.ps, &h.api};
    helix::ui::PrintSelectDetailView view;
    b.set_detail_view(&view);
    view.set_filament_mappings({row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(b));
    REQUIRE(be.backend->calls.size() == 1);

    // A's deferral fires on READY, but B's print owns the record now.
    h.set_klippy(KlippyState::READY);
    CHECK(be.backend->calls.size() == 1);
    CHECK(PrintStartControllerTestAccess::saved_mapping(b) == std::vector<int>{2, 1});
    b.set_detail_view(nullptr);
}

TEST_CASE("remap restore: a print that sends nothing keeps the pending record",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_pending_nothing_sent"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->reported_slots = 2; // T0's own lane 2 is on a detached unit
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.size() == 2); // T0 refused, T1 sent

    // Print B's pick matches the live table, so nothing goes out, but B's end
    // still owes the printer its map: start arms the restore on this answer.
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({row(1, 2, true)});
    const bool restore_owed = PrintStartControllerTestAccess::apply_remaps(h.controller);
    CHECK(restore_owed);
    CHECK(be.backend->calls.size() == 2);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});
    if (restore_owed) {
        PrintStartControllerTestAccess::observe_for_restore(h.controller);
    }
    h.report("printing", "b.gcode");

    SECTION("the unit back by B's end gets T0's lane restored") {
        be.backend->reported_slots = 4;
        h.report("complete", "b.gcode");
        REQUIRE(be.backend->calls.size() == 4);
        CHECK(be.backend->calls[2].tool == 0);
        CHECK(be.backend->calls[2].slot == 2);
    }

    SECTION("a unit still gone at B's end lets T0's entry go") {
        h.report("complete", "b.gcode");
        REQUIRE(be.backend->calls.size() == 3);
        CHECK(be.backend->calls[2].tool == 1);
        CHECK(be.backend->calls[2].slot == 1);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

        be.backend->reported_slots = 4;
        Harness::ams_data_tick();
        CHECK(be.backend->calls.size() == 3);
    }
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: two controllers armed on one print restore it once",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_two_lifecycle"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true; // the record outlives the first send
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    helix::ui::PrintSelectDetailView view_a;
    h.controller.set_detail_view(&view_a);
    view_a.set_filament_mappings({row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));
    PrintStartControllerTestAccess::observe_for_restore(h.controller);

    PrintStartController b{h.ps, &h.api};
    helix::ui::PrintSelectDetailView view_b;
    b.set_detail_view(&view_b);
    view_b.set_filament_mappings({row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(b));
    PrintStartControllerTestAccess::observe_for_restore(b);
    const size_t before_end = be.backend->calls.size();

    h.report("printing", "b.gcode");
    h.report("complete", "b.gcode");
    CHECK(be.backend->calls.size() == before_end + 2); // T0->2 and T1->1, once
    b.set_detail_view(nullptr);
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: a tool mapped by hand outside a print is not reverted",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_hand_edit"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->reported_slots = 2;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    SECTION("the edit becomes the tool's entry") {
        be.backend->locally_assume({3, 2}); // the user put T0 on lane 3
        helix::ui::adopt_hand_tool_mapping(0, 0, 3);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) ==
              std::vector<int>{3, 1});
        be.backend->reported_slots = 4;
        PrintStartControllerTestAccess::restore(h.controller);
        REQUIRE(be.backend->calls.size() == 1);
        CHECK(be.backend->calls[0].tool == 1);
    }

    SECTION("an edit that leaves nothing to restore ends the record") {
        be.backend->locally_assume({2, 1});
        helix::ui::adopt_hand_tool_mapping(0, 1, 1);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    }

    SECTION("an edit on another backend leaves the record alone") {
        helix::ui::adopt_hand_tool_mapping(1, 0, 3);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) ==
              std::vector<int>{2, 1});
    }
}

TEST_CASE("remap restore: each printer keeps its own pending record on disk",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_per_printer_file"};
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    auto* cfg = Config::get_instance();
    const std::string printer_a = "remap-printer-a";
    cfg->add_printer(printer_a, nlohmann::json{{"printer_name", "A"}});
    cfg->add_printer("remap-printer-b", nlohmann::json{{"printer_name", "B"}});
    REQUIRE(cfg->set_active_printer(printer_a));
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);

    // Printer A: a print remapped T1, and its restore is still pending.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    view.set_filament_mappings({row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));

    // Switch to B, which prints with a remap of its own.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    REQUIRE(cfg->set_active_printer("remap-printer-b"));
    view.set_filament_mappings({row(0, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));

    // Back to A: startup replays A's record, not B's.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    REQUIRE(cfg->set_active_printer(printer_a));
    be.backend->calls.clear();
    h.controller.recover_pending_remap();
    REQUIRE(be.backend->calls.size() == 2);
    CHECK(be.backend->calls[0].tool == 0);
    CHECK(be.backend->calls[0].slot == 2);
    CHECK(be.backend->calls[1].tool == 1);
    CHECK(be.backend->calls[1].slot == 1);

    // A's restore landing clears A's entry only.
    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    REQUIRE(std::filesystem::exists(record));
    std::ifstream ifs(record);
    const std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false);
    CHECK_FALSE(j["printers"].contains(printer_a));
    CHECK(j["printers"].contains("remap-printer-b"));
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: a refused restore carried through a print clears once confirmed",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_refused_then_confirmed"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    be.backend->reported_slots = 2;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.size() == 2);
    be.backend->firmware_reports({1, 1}); // T1's restore landed, T0's was refused

    // Print B remaps T1 and runs to completion.
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({row(1, 0, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));
    REQUIRE(be.backend->calls.size() == 3);
    be.backend->firmware_reports({1, 0});
    PrintStartControllerTestAccess::observe_for_restore(h.controller);
    h.report("printing", "b.gcode");

    // The unit is back by B's end, so the whole original map goes out.
    be.backend->reported_slots = 4;
    h.report("complete", "b.gcode");
    REQUIRE(be.backend->calls.size() == 5);
    CHECK(be.backend->calls[3].tool == 0);
    CHECK(be.backend->calls[3].slot == 2);
    CHECK(be.backend->calls[4].tool == 1);
    CHECK(be.backend->calls[4].slot == 1);
    REQUIRE(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});

    be.backend->firmware_reports({2, 1});
    Harness::ams_data_tick();
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: a record from another filament system is not reused",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_layout_changed"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    be.backend->firmware_reports({1, 2});
    Harness h;
    h.set_klippy(KlippyState::READY);
    AmsState::instance().set_pending_tool_map_restore(0, AmsType::HAPPY_HARE, {2, 1});

    SECTION("print start snapshots the live table instead") {
        CHECK(AmsState::instance().collect_firmware_routing().head(0) == 1);
        helix::ui::PrintSelectDetailView view;
        h.controller.set_detail_view(&view);
        // T0's row names the dropped record's lane; it is a seed, not a pick.
        view.set_filament_mappings({row(0, 2, false), row(1, 3, true)});
        REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        REQUIRE(be.backend->calls.size() == 1);
        CHECK(be.backend->calls[0].tool == 1);
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) ==
              std::vector<int>{1, 2});
        h.controller.set_detail_view(nullptr);
    }

    SECTION("a restore drops it without sending") {
        PrintStartControllerTestAccess::restore(h.controller);
        CHECK(be.backend->calls.empty());
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    }
}

TEST_CASE("remap restore: a backend with no mapping table drops the record unsent",
          "[remap-restore][tool_mapping]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_no_table"};
    ScopedCountingBackend be{{1, 2}};
    // An AFC shim over a firmware that maps tools per print has no table.
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"AFC", "AFC_unit IFS", "zmod_afclite"}));
    be.backend->set_discovery(hw);
    REQUIRE(be.backend->get_remap_strategy() == AmsBackend::RemapStrategy::None);
    Harness h;
    h.set_klippy(KlippyState::READY);

    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    std::filesystem::create_directories(record.parent_path());
    {
        std::ofstream ofs(record);
        ofs << "{\"backend_index\":0,\"tool_mapping\":[2,1]}";
    }
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    PrintStartControllerTestAccess::restore(h.controller);

    CHECK(be.backend->calls.empty());
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK_FALSE(std::filesystem::exists(record));
}

TEST_CASE("remap restore: a printer switch drops the record", "[remap-restore][1859]") {
    LVGLTestFixture fx;
    // A re-init registers AmsState's invalidator; a test that cleared the registry
    // left an initialized AmsState without one.
    helix::AmsState::instance().deinit_subjects();
    helix::AmsState::instance().init_subjects(true);
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    helix::PrinterCacheRegistry::instance().invalidate_all();
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    PrintStartControllerTestAccess::restore(h.controller);
    CHECK(be.backend->calls.empty());
}

TEST_CASE("remap restore: an unparsable record file is moved aside", "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_torn_file"};
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    const auto aside = std::filesystem::path(record.string() + ".corrupt");
    std::filesystem::create_directories(record.parent_path());
    const std::string torn = "{\"printers\":{\"other\":{\"backend_ind";
    {
        std::ofstream ofs(record);
        ofs << torn;
    }
    const auto contents = [](const std::filesystem::path& p) {
        std::ifstream ifs(p);
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };

    // A print start with a remap persists over an empty file, the torn bytes kept aside.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({row(1, 3, true)});
    REQUIRE(PrintStartControllerTestAccess::apply_remaps(h.controller));
    CHECK(contents(aside) == torn);
    const auto j = nlohmann::json::parse(contents(record), nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j["printers"].contains(Config::get_instance()->get_active_printer_id()));
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: a tool unmapped before the print does not hold the record open",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_unmapped_confirm"};
    ScopedCountingBackend be{{1, 0}}; // the print mapped T1, which had no lane
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, -1}, 0);

    SECTION("the firmware confirming T0 ends it") {
        PrintStartControllerTestAccess::restore(h.controller);
        REQUIRE(be.backend->calls.size() == 1);
        be.backend->firmware_reports({2, 0});
        Harness::ams_data_tick();
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    }

    SECTION("a print start finding T0 already back ends it") {
        be.backend->firmware_reports({2, 0});
        helix::ui::PrintSelectDetailView view;
        h.controller.set_detail_view(&view);
        view.set_filament_mappings({row(0, 2, true)});
        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
        h.controller.set_detail_view(nullptr);
    }
}

TEST_CASE("remap restore: assigning a tool in lane settings updates the pending record",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_lane_settings_edit"};
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);

    REQUIRE(helix::ui::lane_settings::commit_assigned_tool(*be.backend, 3, 0));
    REQUIRE(be.backend->calls.size() == 1);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{3, 1});
}

TEST_CASE("remap restore: a hand edit that takes a saved lane releases it",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_hand_takes_lane"};
    ScopedCountingBackend be{{0, 3, 1}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    // The printer's own map had T1 on lane 2 and T2 on lane 1.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {0, 2, 1}, 0);

    // The user puts T0 on lane 2 by hand; a lane-keyed table unmaps T1 there.
    be.backend->locally_assume({2, -1, 0});
    helix::ui::adopt_hand_tool_mapping(0, 0, 2);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) ==
          std::vector<int>{2, -1, 1});

    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE(be.backend->calls.size() == 1); // T2 back on lane 1; nothing takes lane 2
    CHECK(be.backend->calls[0].tool == 2);
    CHECK(be.backend->calls[0].slot == 1);
}

TEST_CASE("remap restore: a print that names no backend keeps another backend's record",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_other_backend"};
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 1);

    helix::ToolMapping auto_row;
    auto_row.tool_index = 0;
    auto_row.is_auto = true;
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({auto_row});
    PrintStartControllerTestAccess::apply_remaps(h.controller);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller) == std::vector<int>{2, 1});
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == 1);
    h.controller.set_detail_view(nullptr);
}

TEST_CASE("remap restore: startup replays only the active printer's record",
          "[remap-restore][1859]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_recover_printer_id"};
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);
    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    std::filesystem::create_directories(record.parent_path());
    const std::string active = Config::get_instance()->get_active_printer_id();
    const std::string entry = "{\"backend_index\":0,\"tool_mapping\":[2,1]}";

    SECTION("another printer's record is left alone") {
        {
            std::ofstream ofs(record);
            ofs << "{\"printers\":{\"" << active << "-other\":" << entry << "}}";
        }
        h.controller.recover_pending_remap();
        CHECK(be.backend->calls.empty());
        CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
        CHECK(std::filesystem::exists(record));
    }

    SECTION("the active printer's record is restored") {
        {
            std::ofstream ofs(record);
            ofs << "{\"printers\":{\"" << active << "\":" << entry << "}}";
        }
        h.controller.recover_pending_remap();
        CHECK(be.backend->calls.size() == 2);
    }

    SECTION("a record naming no printer is the active printer's") {
        {
            std::ofstream ofs(record);
            ofs << entry;
        }
        h.controller.recover_pending_remap();
        CHECK(be.backend->calls.size() == 2);
    }
}

TEST_CASE("remap restore: a refusal past the backend's checks is not retried per tick",
          "[remap-restore][1684]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_retry_past_check"};
    ScopedCountingBackend be{{1, 2}};
    be.backend->reported_slots = 4;
    be.backend->refuse_past_check = true;
    Harness h;
    h.set_klippy(KlippyState::READY);

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 2}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(be.backend->calls.size() == 1);

    // can_set_tool_mapping() passes, so a retry would fire and fail on every
    // tick. The record stays for startup instead.
    Harness::ams_data_tick();
    Harness::ams_data_tick();
    CHECK(be.backend->calls.size() == 1);
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

TEST_CASE("remap restore: a tool unmapped before the print is left unmapped",
          "[remap-restore][1684]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_retry_unmapped"};
    ScopedCountingBackend be{{0, 1}};
    be.backend->reported_slots = 2;
    Harness h;
    h.set_klippy(KlippyState::READY);

    // T0 had no lane before the print; T1's lane is on the detached unit.
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {-1, 3}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(be.backend->calls.size() == 1);
    CHECK(be.backend->calls[0].tool == 1);

    // The unmapped entry must not hold back the lane that can come back.
    be.backend->reported_slots = 4;
    Harness::ams_data_tick();
    REQUIRE(be.backend->calls.size() == 2);
    CHECK(be.backend->calls[1].tool == 1);
    CHECK(be.backend->calls[1].slot == 3);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

namespace {

/// A CFS backend whose gcode is captured instead of sent.
class CapturingCfsBackend : public helix::printer::AmsBackendCfs {
  public:
    CapturingCfsBackend() : AmsBackendCfs(nullptr, nullptr) {}
    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    std::vector<std::string> sent;
};

nlohmann::json cfs_unit() {
    return {{"state", "connect"},   {"filament", "None"},    {"vender", {"none"}},
            {"remain_len", {"-1"}}, {"color_value", {"-1"}}, {"material_type", {"-1"}}};
}

/// A stock box frame reporting units T1..T<units>, T0 routed to T1A.
nlohmann::json cfs_box_frame(int units) {
    nlohmann::json box = {{"state", "connect"},
                          {"filament", 0},
                          {"enable", 1},
                          {"filament_useup", 0},
                          {"map", {{"T1A", "T1A"}}}};
    for (int u = 1; u <= units; ++u) {
        box["T" + std::to_string(u)] = cfs_unit();
    }
    return {{"params", nlohmann::json::array({nlohmann::json{{"box", box}}, 0})}};
}

} // namespace

// CFS refuses a bay past the attached units. A restore aimed at the second
// unit's T2B is refused while the box reports one unit and goes out on the
// frame that reports the second.
TEST_CASE("remap restore: CFS retries when the box reports the refused bay's unit",
          "[remap-restore][1684]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_retry_cfs"};
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<CapturingCfsBackend>();
    CapturingCfsBackend* cfs = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    Harness h;
    h.set_klippy(KlippyState::READY);

    CfsTestAccess::handle_status(*cfs, cfs_box_frame(1));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cfs->get_system_info().total_slots == 4);
    REQUIRE_FALSE(cfs->can_set_tool_mapping(0, 5).success());

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {5}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cfs->sent.empty());
    REQUIRE_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    CfsTestAccess::handle_status(*cfs, cfs_box_frame(1));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(cfs->sent.empty());

    SECTION("the box reporting the second unit lands the replay") {
        CfsTestAccess::handle_status(*cfs, cfs_box_frame(2));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(cfs->sent == std::vector<std::string>{"BOX_MODIFY_TN T1A=T2B"});
    }

    SECTION("a backend with no box frame yet replays nothing") {
        // A rebuilt backend reads total_slots 0, where CFS accepts any
        // encodable bay: T2B passes the predicate, but nothing has said the
        // second unit exists.
        auto fresh_owned = std::make_unique<CapturingCfsBackend>();
        CapturingCfsBackend* fresh = fresh_owned.get();
        AmsState::instance().set_backend(std::move(fresh_owned));
        REQUIRE(fresh->get_system_info().total_slots == 0);
        REQUIRE(fresh->can_set_tool_mapping(0, 5).success());
        Harness::ams_data_tick();
        CHECK(fresh->sent.empty());

        // The first frame shows one unit: still refused, still nothing sent.
        CfsTestAccess::handle_status(*fresh, cfs_box_frame(1));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fresh->sent.empty());

        CfsTestAccess::handle_status(*fresh, cfs_box_frame(2));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fresh->sent == std::vector<std::string>{"BOX_MODIFY_TN T1A=T2B"});
    }

    AmsState::instance().set_backend(nullptr);
}

// ============================================================================
// The gate must not break the normal path
// ============================================================================

TEST_CASE("remap restore: ready Klipper consumes the snapshot, sending only what differs",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    const bool already_matches = GENERATE(false, true);
    CAPTURE(already_matches);
    ScopedCountingBackend be{already_matches ? std::vector<int>{2, 1} : std::vector<int>{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);

    if (already_matches) {
        CHECK(be.backend->calls.empty());
    } else {
        REQUIRE(be.backend->calls.size() == 2);
        CHECK(be.backend->calls[0].tool == 0);
        CHECK(be.backend->calls[0].slot == 2);
        CHECK(be.backend->calls[1].tool == 1);
        CHECK(be.backend->calls[1].slot == 1);
    }
    // Delivered to a ready Klipper: the snapshot is spent.
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == -1);
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
// #1386: registration must not read the PREVIOUS job's terminal state.
//
// execute_print_start() registers the restore observer BEFORE start_now calls
// begin_preparing(), and Moonraker leaves print_stats.state on the finished
// job for the whole preparing window. The observer fires on attach, so the
// old code saw the stale terminal value and reverted the remaps ~150ms after
// sending them.
// ============================================================================

TEST_CASE("remap restore: previous job's terminal state does not revert fresh remaps",
          "[remap-restore][1386]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);

    // The previous job finished; print_stats still says so, and this job's
    // preparing window has not opened, which is exactly what the controller
    // sees between apply_filament_remaps() and start_now.
    h.report("complete", "previous.gcode");
    REQUIRE(lv_subject_get_int(h.ps.print_state().get_print_lifecycle_subject()) ==
            static_cast<int>(PrintState::Complete));

    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::observe_for_restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();

    // The remaps just sent must survive the observer's attach-fire.
    CHECK(be.backend->calls.empty());
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

TEST_CASE("remap restore: latch arms when this job goes live, fires at its end",
          "[remap-restore][1386]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    Harness h;
    h.set_klippy(KlippyState::READY);

    // Same stale-terminal registration as above: nothing may fire yet.
    h.report("complete", "previous.gcode");
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::observe_for_restore(h.controller);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(be.backend->calls.empty());

    // start_now runs: the preparing window opens and the derived lifecycle
    // becomes Preparing, which must ARM the latch rather than fire on the
    // stale terminal wire state still sitting underneath it.
    h.ps.print_state().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(h.ps.print_state().get_print_lifecycle_subject()) ==
            static_cast<int>(PrintState::Preparing));
    CHECK(be.backend->calls.empty());

    // The printer takes the job. The name lands while the wire is still
    // terminal (Moonraker deltas), so the state flip below retires the
    // preparing claim as Confirmed, exactly as a real start does.
    h.report("complete", "next.gcode");
    h.report("printing", "next.gcode");
    CHECK(be.backend->calls.empty());

    // ...and when THIS job ends, the restore finally goes out.
    h.report("complete", "next.gcode");

    REQUIRE(be.backend->calls.size() == 2);
    CHECK(be.backend->calls[0].tool == 0);
    CHECK(be.backend->calls[0].slot == 2);
    CHECK(be.backend->calls[1].tool == 1);
    CHECK(be.backend->calls[1].slot == 1);
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

// ============================================================================
// Firmware confirmation: the send is not the proof
// ============================================================================

TEST_CASE(
    "remap restore: an echoing backend waits for firmware; its own optimistic write is no proof",
    "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);

    // Commands went out to a ready Klipper, but a send is not proof, so the record stays.
    REQUIRE(be.backend->calls.size() == 2);
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    // THE trap this whole seam exists for: every backend updates its registry
    // optimistically inside set_tool_mapping(), so the mapping reads correct
    // immediately even when Klipper refused the command. Matching values with an
    // unmoved generation must NOT be accepted as proof.
    be.backend->locally_assume({2, 1});
    Harness::ams_data_tick();

    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
}

TEST_CASE("remap restore: firmware confirms only the mapping that matches the snapshot",
          "[remap-restore][1270]") {
    LVGLTestFixture fx;
    ScopedCountingBackend be{{1, 2}};
    be.backend->echoes_firmware = true;
    Harness h;
    h.set_klippy(KlippyState::READY);
    PrintStartControllerTestAccess::seed_saved_mapping(h.controller, {2, 1}, 0);
    PrintStartControllerTestAccess::restore(h.controller);
    REQUIRE_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    // A multi-lane restore lands one delta at a time, so a partial match is a
    // normal intermediate state — keep the record rather than declaring failure.
    be.backend->firmware_reports({2, 2});
    Harness::ams_data_tick();
    CHECK_FALSE(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());

    // The rest arrives.
    be.backend->firmware_reports({2, 1});
    Harness::ams_data_tick();
    CHECK(PrintStartControllerTestAccess::saved_mapping(h.controller).empty());
    CHECK(PrintStartControllerTestAccess::saved_backend_index(h.controller) == -1);
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

// Spools latched on the bed refuse every print start in the API layer, so a
// reprint stops before it prepares anything and the drying prompt explains why.
TEST_CASE("reprint with spools latched on the bed never begins preparing",
          "[print-start][spool-latch]") {
    LVGLTestFixture fx;
    Harness h;
    h.ps.print_state().set_spool_latch(true);

    bool errored = false;
    bool started = false;
    h.controller.initiate_reprint(
        "part.gcode", "part.gcode", {}, [&] { started = true; }, [&] { errored = true; });

    CHECK(errored);
    CHECK_FALSE(started);
    CHECK_FALSE(h.ps.print_state().has_preparing_job());
    h.ps.print_state().set_spool_latch(false);
}

namespace {

/// A revision 4 Fork box with boxes 1-3 on the bus and box 2 not answering.
nlohmann::json fork_print_map_frame() {
    nlohmann::json slots = nlohmann::json::array();
    for (int i = 0; i < 12; ++i) {
        slots.push_back({{"index", i},
                         {"external", false},
                         {"present", i < 4 || i >= 8},
                         {"loaded", false},
                         {"material", "PLA"},
                         {"color", "#F2F2F2"}});
    }
    nlohmann::json box = {{"api_version", 1},
                          {"api_revision", 4},
                          {"driver_ready", true},
                          {"loaded_slot", -1},
                          {"slots", slots},
                          {"boxes",
                           {{{"address", 1}, {"online", true}},
                            {{"address", 2}, {"online", false}},
                            {{"address", 3}, {"online", true}}}}};
    return {{"params", nlohmann::json::array({nlohmann::json{{"box", box}}, 0})}};
}

} // namespace

TEST_CASE("pre-print send: a remap to a box that is not answering stops the print",
          "[print-start][print_map]") {
    LVGLTestFixture fx;
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<CapturingCfsBackend>();
    auto* cfs = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    CfsTestAccess::handle_status(*cfs, fork_print_map_frame());
    REQUIRE(cfs->requires_preprint_send());

    Harness h;
    h.client.clear_gcode_script_history();
    std::vector<std::string> errors;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& m) { errors.push_back(m); });

    bool done = false;
    bool aborted = false;
    const auto send = [&](std::map<int, int> remap) {
        done = aborted = false;
        errors.clear();
        PrintStartControllerTestAccess::send_preprint(
            h.controller, {0, 1}, remap, [&] { done = true; }, [&] { aborted = true; });
        helix::ui::UpdateQueue::instance().drain();
    };
    const auto print_map_sent = [&] {
        for (const auto& script : h.client.gcode_script_history()) {
            if (script.find("BOX_PRINT_MAP") != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    SECTION("a slot on the offline box") {
        send({{0, 5}});
        CHECK(aborted);
        CHECK_FALSE(done);
        CHECK_FALSE(print_map_sent());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].find("Disconnected") != std::string::npos);
    }

    SECTION("a slot on a box that is answering") {
        send({{0, 9}});
        CHECK(done);
        CHECK_FALSE(aborted);
        CHECK(errors.empty());
        CHECK(print_map_sent());
    }

    SECTION("a file whose tools are unknown starts on a cleared map") {
        PrintStartControllerTestAccess::send_preprint(
            h.controller, {}, {}, [&] { done = true; }, [&] { aborted = true; });
        helix::ui::UpdateQueue::instance().drain();
        CHECK(done);
        REQUIRE(h.client.gcode_script_history().size() == 1);
        CHECK(h.client.gcode_script_history()[0] == "BOX_PRINT_MAP MAP=");
    }

    helix::ui::set_test_notification_error_hook(nullptr);
    helix::ui::UpdateQueue::instance().drain();
    AmsState::instance().set_backend(nullptr);
}

// A Fork box keeps a print map for the next file it loads, so a reprint with no
// recorded routing still sends the file's tools at their own slots: that is the
// firmware default, and it overwrites a map a failed remapped start left pending.
TEST_CASE("reprint: a Fork print-map box sends the identity map for the file's tools",
          "[print-start][print_map][reprint]") {
    LVGLTestFixture fx;
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<CapturingCfsBackend>();
    auto* cfs = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    const auto print_map_lines = [](const Harness& h) {
        std::vector<std::string> lines;
        for (const auto& script : h.client.gcode_script_history()) {
            if (script.find("BOX_PRINT_MAP") != std::string::npos) {
                lines.push_back(script);
            }
        }
        return lines;
    };
    const auto reprint = [](Harness& h) {
        h.client.clear_gcode_script_history();
        h.controller.initiate_reprint("part.gcode", "part.gcode", {0, 1}, [] {}, [] {});
        helix::ui::UpdateQueue::instance().drain();
    };

    SECTION("revision 4 sends every used tool at its own slot") {
        CfsTestAccess::handle_status(*cfs, fork_print_map_frame());
        REQUIRE(cfs->requires_preprint_send());
        REQUIRE(cfs->last_print_tool_mapping().empty());
        Harness h;
        reprint(h);
        const auto lines = print_map_lines(h);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "BOX_PRINT_MAP MAP=0:0,1:1");
    }

    SECTION("revision 4 with the file's tools unknown clears the pending map") {
        CfsTestAccess::handle_status(*cfs, fork_print_map_frame());
        Harness h;
        h.client.clear_gcode_script_history();
        h.controller.initiate_reprint("part.gcode", "part.gcode", {}, [] {}, [] {});
        // With no tools in hand the reprint reads the file first; this one is
        // not on the printer, so the read answers nothing.
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
        const auto lines = print_map_lines(h);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "BOX_PRINT_MAP MAP=");
    }

    SECTION("revision 3 sends no print map") {
        auto frame = fork_print_map_frame();
        frame["params"][0]["box"]["api_revision"] = 3;
        CfsTestAccess::handle_status(*cfs, frame);
        REQUIRE_FALSE(cfs->requires_preprint_send());
        Harness h;
        reprint(h);
        CHECK(print_map_lines(h).empty());
    }

    SECTION("revision 2 has no print map to send") {
        auto frame = fork_print_map_frame();
        frame["params"][0]["box"]["api_revision"] = 2;
        CfsTestAccess::handle_status(*cfs, frame);
        REQUIRE_FALSE(cfs->requires_preprint_send());
        Harness h;
        reprint(h);
        CHECK(print_map_lines(h).empty());
    }

    SECTION("a stock box sends no print map") {
        CfsTestAccess::handle_status(*cfs, cfs_box_frame(1));
        REQUIRE_FALSE(cfs->default_print_routing_is_identity());
        Harness h;
        reprint(h);
        CHECK(print_map_lines(h).empty());
    }

    helix::ui::UpdateQueue::instance().drain();
    AmsState::instance().set_backend(nullptr);
}

// A restart from the beginning carries no routing, so on a Fork print-map box it
// clears whatever map is pending rather than let it apply to the restarted file.
TEST_CASE("restart from the beginning: a Fork print-map box clears the pending map",
          "[print-start][print_map][reprint]") {
    LVGLTestFixture fx;
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<CapturingCfsBackend>();
    auto* cfs = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    Harness h;
    h.client.clear_gcode_script_history();
    const auto sent = [&h] {
        std::string out;
        for (const auto& script : h.client.gcode_script_history()) {
            out += script + "\n";
        }
        return out;
    };

    SECTION("revision 4 sends the clear after the cancel") {
        CfsTestAccess::handle_status(*cfs, fork_print_map_frame());
        helix::ui::restart_print_from_beginning(&h.api, "part.gcode", "[test]", nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(sent().find("CANCEL_PRINT_BASE\nBOX_PRINT_MAP MAP=\n") != std::string::npos);
    }

    SECTION("revision 3 sends no print map") {
        auto frame = fork_print_map_frame();
        frame["params"][0]["box"]["api_revision"] = 3;
        CfsTestAccess::handle_status(*cfs, frame);
        helix::ui::restart_print_from_beginning(&h.api, "part.gcode", "[test]", nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(sent().find("CANCEL_PRINT_BASE") != std::string::npos);
        CHECK(sent().find("BOX_PRINT_MAP") == std::string::npos);
    }

    helix::ui::UpdateQueue::instance().drain();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE("remap send: a pick on a second backend is never sent to the first",
          "[print_start][remap-restore][multi_backend]") {
    LVGLTestFixture fx;
    ConfigDirGuard config_dir{"remap_two_backends"};
    ScopedCountingBackend be{{0, 1, 2, 3}};
    auto second_owned = std::make_unique<CountingAfcBackend>();
    second_owned->current = {0, 1, 2, 3};
    CountingAfcBackend* const second = second_owned.get();
    REQUIRE(AmsState::instance().add_backend(std::move(second_owned)) == 1);
    Harness h;
    h.set_klippy(KlippyState::READY);

    // T0 on backend 0 lane 2 decides the backend the send writes; T1's pick is
    // backend 1's slot 2, which shares only its number with backend 0's.
    helix::ToolMapping t0;
    t0.tool_index = 0;
    t0.mapped_slot = 1;
    t0.mapped_backend = 0;
    t0.user_picked = true;
    helix::ToolMapping t1;
    t1.tool_index = 1;
    t1.mapped_slot = 2;
    t1.mapped_backend = 1;
    t1.user_picked = true;
    helix::ui::PrintSelectDetailView view;
    h.controller.set_detail_view(&view);
    view.set_filament_mappings({t0, t1});
    CHECK(PrintStartControllerTestAccess::apply_remaps(h.controller));

    REQUIRE(be.backend->calls.size() == 1);
    CHECK(be.backend->calls[0].tool == 0);
    CHECK(be.backend->calls[0].slot == 1);
    const bool t1_sent_to_backend_0 =
        std::any_of(be.backend->calls.begin(), be.backend->calls.end(),
                    [](const CountingAfcBackend::Call& c) { return c.tool == 1 && c.slot == 2; });
    CHECK_FALSE(t1_sent_to_backend_0);
    CHECK(second->calls.empty());
    h.controller.set_detail_view(nullptr);
}
