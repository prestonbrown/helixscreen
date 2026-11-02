// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_self_heal.h"

#include "ui_toast_manager.h"

#include "config.h"
#include "http_executor.h"
#include "lvgl.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {
// install.sh --relink walks /proc and greps the config; it never waits on Moonraker.
constexpr int kRelinkTimeoutSeconds = 30;
// install.sh --restart-moonraker: a 60s wait for Moonraker to come back plus
// the capped HTTP probes around it. moonraker_restart in helix_print_conf.sh
// counts its worst case, which stays well inside this.
constexpr int kRestartTimeoutSeconds = 180;
constexpr uint32_t kRestartToastMs = 4000;
// How often a restart waiting for the screen looks again.
constexpr uint32_t kRestartRecheckMs = 1000;
// PluginSelfHealMemory, kept across starts.
constexpr const char* kPrefRestartedForBundled = "/plugin_self_heal/restarted_for_bundled";
constexpr const char* kPrefRestartedForReported = "/plugin_self_heal/restarted_for_reported";
constexpr const char* kPrefRelinkedFor = "/plugin_self_heal/relinked_for";
} // namespace

bool PluginSelfHeal::ran_this_start_ = false;

PluginSelfHeal::PluginSelfHeal(PrinterState& printer_state, HelixPluginInstaller& installer,
                               ScreenProbe screen)
    : printer_state_(printer_state), installer_(installer), screen_(std::move(screen)) {}

void PluginSelfHeal::on_plugin_reported() {
    if (ran_this_start_ || printer_state_.plugin_status_state().helix_plugin_state() < 0) {
        return;
    }
    ran_this_start_ = true;

    // Switched off by the user, it reads as not installed: the install prompt
    // offers to turn it on, so its link is no sign it should be left alone.
    if (printer_state_.plugin_status_state().disabled()) {
        spdlog::info("[PluginSelfHeal] The HelixPrint plugin is set to enabled: False; the link "
                     "is not checked and an install turns it back on");
        return;
    }
    // A mock Moonraker is not the one install.sh would find on this host.
    if (get_runtime_config()->should_mock_moonraker()) {
        return;
    }
    if (!installer_.is_local_moonraker()) {
        act(false, PluginLinkCheck::NotOurs);
        return;
    }
    const std::string script = installer_.get_install_script_path();
    if (script.empty()) {
        spdlog::info("[PluginSelfHeal] No bundled install.sh; the plugin link is not checked");
        return;
    }
    // The script blocks while it walks /proc and the config, so it runs off
    // this thread; its outcome comes back here.
    checking_ = true;
    auto tok = lifetime_.token();
    http::HttpExecutor::slow().submit([this, tok, script]() {
        const PluginLinkCheck link = relink_plugin(script, kRelinkTimeoutSeconds);
        tok.defer("PluginSelfHeal::relinked", [this, link]() { act(true, link); });
    });
}

void PluginSelfHeal::act(bool local, PluginLinkCheck link) {
    checking_ = false;
    // A link of ours means the user installed the plugin: one that reads as
    // absent failed to load, which this check repairs, so offering an install
    // would be wrong.
    if (link == PluginLinkCheck::Current || link == PluginLinkCheck::Relinked) {
        HelixPluginInstaller::settle_prompt_for_session();
    }
    PluginSelfHealFacts facts;
    facts.local = local;
    facts.link = link;
    facts.reported_version = printer_state_.plugin_status_state().version();
    facts.bundled_version = installer_.bundled_plugin_version();
    facts.plugin_file = installer_.bundled_plugin_file();
    facts.job_holds = job_holds_machine(printer_state_.print_state().get_print_lifecycle());
    facts.memory = load_memory();

    const PluginSelfHealStep step = decide_plugin_self_heal(facts);
    restart_facts_ = facts;
    remember(facts, false);

    switch (step) {
    case PluginSelfHealStep::GiveUp:
        if (link == PluginLinkCheck::Relinked) {
            spdlog::warn("[PluginSelfHeal] The plugin link was re-pointed at {} again after a "
                         "restart for it; not restarting until a start finds it current. Check "
                         "how Moonraker's components directory reaches that file",
                         facts.plugin_file);
            return;
        }
        spdlog::warn("[PluginSelfHeal] Moonraker still runs HelixPrint plugin {} after a restart "
                     "for this app's {}; not restarting again until either version changes. "
                     "Check which helix_print.py Moonraker loads",
                     facts.reported_version, facts.bundled_version);
        return;
    case PluginSelfHealStep::Nothing:
        if (link == PluginLinkCheck::Failed) {
            spdlog::warn("[PluginSelfHeal] Could not check the plugin link (install.sh's "
                         "warnings above name the path); the next start tries again");
        }
        return;
    case PluginSelfHealStep::ReportOnly:
        spdlog::info("[PluginSelfHeal] Moonraker runs HelixPrint plugin {} and this app ships {}, "
                     "and {}; run the install command from Advanced on the printer to match them",
                     facts.reported_version, facts.bundled_version,
                     local ? "install.sh --relink found no plugin link of ours (no symlink, a "
                             "copied file, or no [helix_print] section)"
                           : "Moonraker runs on another host");
        return;
    case PluginSelfHealStep::WaitForJob:
        spdlog::info("[PluginSelfHeal] The HelixPrint plugin needs a Moonraker restart (link {}, "
                     "running {}, bundled {}); waiting for the job to end",
                     link == PluginLinkCheck::Relinked ? "re-pointed" : "current",
                     facts.reported_version, facts.bundled_version);
        restart_wait_ = PluginTurnStep::WaitForJob;
        return;
    case PluginSelfHealStep::RestartNow:
        spdlog::info("[PluginSelfHeal] Restarting Moonraker to load the HelixPrint plugin this "
                     "app ships (link {}, running {}, bundled {})",
                     link == PluginLinkCheck::Relinked ? "re-pointed" : "current",
                     facts.reported_version, facts.bundled_version);
        restart_moonraker();
        return;
    }
}

void PluginSelfHeal::on_job_state_changed() {
    if (restart_wait_ == PluginTurnStep::WaitForJob) {
        recheck_restart();
    }
}

void PluginSelfHeal::recheck_restart() {
    if (restart_wait_ == PluginTurnStep::Stop) {
        return;
    }
    const PluginTurnStep step = decide_plugin_turn(
        true, job_holds_machine(printer_state_.print_state().get_print_lifecycle()),
        restart_wait_ == PluginTurnStep::WaitForJob, screen_ ? screen_() : PluginScreenState{});
    switch (step) {
    case PluginTurnStep::Stop:
    case PluginTurnStep::WaitForJob:
        restart_wait_ = step;
        restart_timer_.reset();
        return;
    case PluginTurnStep::WaitForScreen:
        restart_wait_ = step;
        if (!restart_timer_) {
            restart_timer_.reset(lv_timer_create(
                [](lv_timer_t* timer) {
                    static_cast<PluginSelfHeal*>(lv_timer_get_user_data(timer))->recheck_restart();
                },
                kRestartRecheckMs, this));
        }
        return;
    case PluginTurnStep::Go:
        restart_wait_ = PluginTurnStep::Stop;
        restart_timer_.reset();
        spdlog::info("[PluginSelfHeal] The job and its completion flow are done; restarting "
                     "Moonraker for the HelixPrint plugin");
        restart_moonraker();
        return;
    }
}

void PluginSelfHeal::restart_moonraker() {
    const std::string script = installer_.get_install_script_path();
    // The script blocks while Moonraker restarts, so it runs off this thread.
    // It may start long after this call, behind other work on the lane, so
    // it reads the print state from Moonraker itself right before the
    // restart rather than taking the one seen here.
    auto tok = lifetime_.token();
    http::HttpExecutor::slow().submit([this, tok, script]() {
        const PluginRestartOutcome outcome =
            restart_moonraker_for_plugin(script, kRestartTimeoutSeconds);
        tok.defer("PluginSelfHeal::restarted", [this, outcome]() { on_restarted(outcome); });
    });
}

PluginSelfHealMemory PluginSelfHeal::load_memory() {
    Config* config = Config::get_instance();
    PluginSelfHealMemory memory;
    memory.restarted_for_bundled = config->get<std::string>(kPrefRestartedForBundled, "");
    memory.restarted_for_reported = config->get<std::string>(kPrefRestartedForReported, "");
    memory.relinked_for = config->get<std::string>(kPrefRelinkedFor, "");
    return memory;
}

void PluginSelfHeal::remember(const PluginSelfHealFacts& facts, bool restart_finished) {
    Config* config = Config::get_instance();
    const PluginSelfHealMemory memory = remember_plugin_self_heal(facts, restart_finished);
    if (!(memory == facts.memory)) {
        config->set<std::string>(kPrefRestartedForBundled, memory.restarted_for_bundled);
        config->set<std::string>(kPrefRestartedForReported, memory.restarted_for_reported);
        config->set<std::string>(kPrefRelinkedFor, memory.relinked_for);
        config->save();
    }
}

void PluginSelfHeal::on_restarted(PluginRestartOutcome outcome) {
    if (plugin_restart_finished(outcome, !restart_facts_.reported_version.empty())) {
        restart_facts_.memory = load_memory();
        remember(restart_facts_, true);
    }
    switch (outcome) {
    case PluginRestartOutcome::Restarted:
        spdlog::info("[PluginSelfHeal] Moonraker restarted for the HelixPrint plugin");
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("HelixPrint plugin updated."),
                                      kRestartToastMs);
        return;
    case PluginRestartOutcome::TimedOut:
        spdlog::error("[PluginSelfHeal] The Moonraker restart for the HelixPrint plugin ran past "
                      "{}s and was stopped; Moonraker may still be restarting. The next start "
                      "tries again",
                      kRestartTimeoutSeconds);
        return;
    case PluginRestartOutcome::HeldForPrint:
        spdlog::info("[PluginSelfHeal] Moonraker reports a print running; restarting it for the "
                     "HelixPrint plugin once the job ends");
        restart_wait_ = PluginTurnStep::WaitForJob;
        return;
    case PluginRestartOutcome::NotAnswering:
        spdlog::error("[PluginSelfHeal] Moonraker does not answer, so it was not restarted for the "
                      "HelixPrint plugin; start the moonraker service to load {}",
                      restart_facts_.plugin_file);
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Restart Moonraker to finish the plugin change"));
        return;
    case PluginRestartOutcome::NeedsManual:
    case PluginRestartOutcome::JobUnknown:
    case PluginRestartOutcome::Failed:
        spdlog::warn("[PluginSelfHeal] Moonraker was not restarted for the HelixPrint plugin; it "
                     "runs the old plugin code until Moonraker restarts");
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Restart Moonraker to finish the plugin change"));
        return;
    }
}

} // namespace helix
