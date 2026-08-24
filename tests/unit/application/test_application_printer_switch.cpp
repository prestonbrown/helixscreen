// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_application_printer_switch.cpp
 * @brief Unit tests for Application's soft-restart entry points
 *
 * Application was structurally untestable until application.o was un-excluded from
 * TEST_APP_OBJS (mk/tests.mk) — nothing in the test binary could name the class, so
 * switch_printer(), add_printer_via_wizard() and cancel_add_printer_wizard() had zero
 * coverage despite owning app lifecycle, shutdown, teardown and printer-state init.
 *
 * ## Scope, and why it stops where it does
 *
 * All three entry points end in tear_down_printer_state() + init_printer_state(): a
 * 16-step teardown that runs StaticSubjectRegistry::deinit_all(),
 * StaticPanelRegistry::destroy_all() and update_queue_shutdown(), followed by a full
 * subject / Moonraker / XML rebuild. Driving that in a shared Catch2 process would hand
 * every later test in the shard a rebuilt set of process singletons. So these tests pin
 * the behaviour on the near side of teardown:
 *
 *   - the re-entrancy latch that makes a nested soft restart a no-op,
 *   - the config validation that must gate everything else,
 *   - the synchronous config surgery cancel_add_printer_wizard() performs before it
 *     defers its teardown.
 *
 * Everything is asserted through observable state — active printer id, Config::df(),
 * whether settings.json was written, whether the per-printer cache invalidators fired —
 * never through which internal helper was called. The invalidation probe is a
 * PrinterCacheRegistry entry precisely because the mechanism is being migrated from a
 * hardcoded PanelWidgetManager::clear_all_panel_configs() call to the registry; both
 * spellings must leave these assertions true.
 *
 * The success path of switch_printer() (df() moved, then every per-printer cache
 * dropped, then teardown) is not reachable here. It stays covered by the lint gate
 * "switch_printer invalidates cached panel widget configs before teardown" in
 * tests/shell/test_code_lint.bats.
 */

#include "ui_update_queue.h"

#include "application_test_fixture.h"
#include "config.h"
#include "printer_cache_registry.h"
#include "test_helpers/application_test_access.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

constexpr const char* PROBE_NAME = "ApplicationPrinterSwitchTestProbe";

/**
 * @brief Two-printer config + a per-printer cache-invalidation counter
 *
 * "alpha" is active; "beta" is the switch target. Config::remove_printer() refuses to
 * drop the last printer, so the add-wizard cases add a third rather than reusing beta.
 */
class PrinterSwitchFixture : public ApplicationTestFixture {
  public:
    PrinterSwitchFixture() {
        cfg_ = helix::Config::get_instance();
        REQUIRE(cfg_ != nullptr);

        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "alpha";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "alpha";
        helix::ConfigTestAccess::read_only_mode(*cfg_) = false;

        // Count per-printer cache invalidations without caring who triggers them.
        helix::PrinterCacheRegistry::instance().register_invalidator(
            PROBE_NAME, [this]() { ++invalidations_; });

        // ~Application() calls shutdown() unconditionally, which tears down
        // TelemetryManager / UpdateChecker / SoundManager / NavigationManager and the
        // UpdateQueue. This Application was never run(), so there is nothing to release.
        ApplicationTestAccess::neutralize_destructor(app_);
        ApplicationTestAccess::set_config(app_, cfg_);
    }

    ~PrinterSwitchFixture() override {
        // The registry outlives the fixture; the invalidator closes over `this`.
        helix::PrinterCacheRegistry::instance().unregister(PROBE_NAME);
        // cancel_add_printer_wizard() defers its teardown through AsyncLifetimeGuard;
        // that callback closes over app_, so drop it rather than run it.
        helix::ui::UpdateQueueTestAccess::discard_pending(helix::ui::UpdateQueue::instance());
    }

    /// Remove the persisted settings file so its later existence proves a save() ran.
    void forget_persisted_settings() const {
        std::error_code ec;
        std::filesystem::remove(helix::ConfigTestAccess::path(*cfg_), ec);
    }

    [[nodiscard]] bool settings_persisted() const {
        std::error_code ec;
        return std::filesystem::exists(helix::ConfigTestAccess::path(*cfg_), ec);
    }

    [[nodiscard]] std::vector<std::string> printer_ids() const {
        return cfg_->get_printer_ids();
    }

    [[nodiscard]] bool has_printer(const std::string& id) const {
        auto ids = printer_ids();
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    helix::Config* cfg_ = nullptr;
    Application app_;
    int invalidations_ = 0;
};

} // namespace

// ============================================================================
// switch_printer() — re-entrancy latch
// ============================================================================

TEST_CASE_METHOD(PrinterSwitchFixture,
                 "Application::switch_printer is ignored while a soft restart is running",
                 "[application][switch_printer]") {
    ApplicationTestAccess::soft_restart_in_progress(app_) = true;
    forget_persisted_settings();

    ApplicationTestAccess::switch_printer(app_, "beta");

    // Nothing moved: a nested switch during the 16-step rebuild would tear down state
    // the outer restart is still holding.
    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(cfg_->df() == "/printers/alpha/");
    CHECK(invalidations_ == 0);
    CHECK_FALSE(settings_persisted());

    // The early return happens BEFORE SoftRestartLatch is constructed, so the flag must
    // survive untouched. If the guard were dropped, the nested call's latch would clear
    // it on the way out and the outer restart would look finished while still running.
    CHECK(ApplicationTestAccess::soft_restart_in_progress(app_));
}

// ============================================================================
// switch_printer() — unknown printer id
// ============================================================================

TEST_CASE_METHOD(PrinterSwitchFixture,
                 "Application::switch_printer rejects an unknown printer without mutating state",
                 "[application][switch_printer]") {
    forget_persisted_settings();

    ApplicationTestAccess::switch_printer(app_, "no-such-printer");

    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(cfg_->df() == "/printers/alpha/");
    CHECK(has_printer("alpha"));
    CHECK(has_printer("beta"));

    // A rejected switch must not persist anything — the config it would write is the
    // config already on disk, and writing it hides the failure from a later reader.
    CHECK_FALSE(settings_persisted());

    // Cache invalidation must sit AFTER the set_active_printer() validation. Firing it
    // on a rejected switch drops every per-printer cache for a switch that never
    // happened, and does it while df() still points at the old printer — the exact
    // ordering PrinterCacheRegistry's contract depends on.
    CHECK(invalidations_ == 0);

    // SoftRestartLatch is RAII, so a rejected switch has to leave the flag clear. A
    // stuck flag makes every later switch or add a silent no-op for the whole process.
    CHECK_FALSE(ApplicationTestAccess::soft_restart_in_progress(app_));
}

// ============================================================================
// cancel_add_printer_wizard() — recovery surgery
// ============================================================================

TEST_CASE_METHOD(PrinterSwitchFixture,
                 "Application::cancel_add_printer_wizard removes the failed printer and restores "
                 "the previous one",
                 "[application][switch_printer]") {
    // Mirror add_printer_via_wizard(): a new empty entry is created and made active,
    // and the previous id is stashed for recovery.
    cfg_->add_printer("printer-3", nlohmann::json{{"wizard_completed", false}});
    REQUIRE(cfg_->set_active_printer("printer-3"));
    ApplicationTestAccess::wizard_previous_printer_id(app_) = "alpha";
    forget_persisted_settings();

    ApplicationTestAccess::cancel_add_printer_wizard(app_);

    // The half-configured printer is gone and the previous one is active again.
    CHECK_FALSE(has_printer("printer-3"));
    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(cfg_->df() == "/printers/alpha/");
    CHECK(has_printer("alpha"));
    CHECK(has_printer("beta"));

    // The recovery is persisted immediately — a crash before the deferred teardown must
    // not leave the abandoned printer entry active on the next boot.
    CHECK(settings_persisted());

    // Recovery state is single-shot: a second cancel must find nothing to undo.
    CHECK(ApplicationTestAccess::wizard_previous_printer_id(app_).empty());

    // Teardown (and the cache invalidation that goes with it) is DEFERRED — this runs
    // from a wizard button handler, so the wizard container has to outlive the callback.
    // Doing it synchronously deletes the widget tree under the event that called us.
    CHECK(invalidations_ == 0);
}

TEST_CASE_METHOD(PrinterSwitchFixture,
                 "Application::cancel_add_printer_wizard is inert without recovery state",
                 "[application][switch_printer]") {
    cfg_->add_printer("printer-3", nlohmann::json{{"wizard_completed", false}});
    REQUIRE(cfg_->set_active_printer("printer-3"));
    ApplicationTestAccess::wizard_previous_printer_id(app_).clear();
    forget_persisted_settings();

    ApplicationTestAccess::cancel_add_printer_wizard(app_);

    // No stashed previous id means this cancel does not belong to an add-printer flow.
    // Acting anyway would remove whatever printer happens to be active.
    CHECK(has_printer("printer-3"));
    CHECK(cfg_->get_active_printer_id() == "printer-3");
    CHECK_FALSE(settings_persisted());
    CHECK(invalidations_ == 0);
}

TEST_CASE_METHOD(
    PrinterSwitchFixture,
    "Application::cancel_add_printer_wizard is ignored while a soft restart is running",
    "[application][switch_printer]") {
    cfg_->add_printer("printer-3", nlohmann::json{{"wizard_completed", false}});
    REQUIRE(cfg_->set_active_printer("printer-3"));
    ApplicationTestAccess::wizard_previous_printer_id(app_) = "alpha";
    ApplicationTestAccess::soft_restart_in_progress(app_) = true;
    forget_persisted_settings();

    ApplicationTestAccess::cancel_add_printer_wizard(app_);

    // Recovery state must survive so the cancel can still be honoured once the in-flight
    // restart finishes; consuming it here would strand the abandoned printer as active.
    CHECK(ApplicationTestAccess::wizard_previous_printer_id(app_) == "alpha");
    CHECK(has_printer("printer-3"));
    CHECK(cfg_->get_active_printer_id() == "printer-3");
    CHECK_FALSE(settings_persisted());
    CHECK(invalidations_ == 0);
}
