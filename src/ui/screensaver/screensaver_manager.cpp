// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "config.h"
#include "display_settings_manager.h"
#include "helix_version.h"
#include "platform_capabilities.h"
#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "screensaver.h"
#include "screensaver_aqua.h"
#include "screensaver_base.h"
#include "screensaver_bounce.h"
#include "screensaver_clock_saver.h"
#include "screensaver_fireflies.h"
#include "screensaver_fireworks.h"
#include "screensaver_flip_clock.h"
#include "screensaver_highfly.h"
#include "screensaver_hyperspace.h"
#include "screensaver_level_store.h"
#include "screensaver_maze.h"
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace helix {

ScreensaverType screensaver_type_from_env(const std::string& value, ScreensaverType configured) {
    return helix::ui::resolve_screensaver_now(value, configured);
}

} // namespace helix

using helix::ui::CpuSample;
using helix::ui::GateDecision;
using helix::ui::SaverLevelEntry;

ScreensaverManager& ScreensaverManager::instance() {
    static ScreensaverManager mgr;
    return mgr;
}

ScreensaverManager::ScreensaverManager()
    : cpu_clock_(helix::ui::read_process_cpu_clock),
      random_(static_cast<std::minstd_rand::result_type>(
          std::chrono::steady_clock::now().time_since_epoch().count())) {
    screensavers_.push_back(std::make_unique<FlyingToasterScreensaver>());
    screensavers_.push_back(std::make_unique<PipesScreensaver>());
    screensavers_.push_back(std::make_unique<StarfieldScreensaver>());
    screensavers_.push_back(std::make_unique<helix::BouncingPrinterScreensaver>());
    screensavers_.push_back(std::make_unique<helix::BouncingLogoScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::FireworksScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::MazeScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::HyperspaceScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::FlipClockScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::FirefliesScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::AquaScreensaver>());
    screensavers_.push_back(std::make_unique<helix::ui::HighFlyScreensaver>());
    for (auto& saver : screensavers_) {
        if (helix::ui::ClockInputSink* sink = saver->clock_input_sink()) {
            sink->set_input_source([this] { return clock_input(); });
        }
    }

    for (const char* name : savers_missing_for_build_depth()) {
        spdlog::error("[ScreensaverManager] {} is listed as drawing at this colour depth but no "
                      "instance is registered; choosing it starts nothing",
                      name);
    }
}

std::vector<const char*> ScreensaverManager::savers_missing_for_build_depth() const {
    std::vector<const char*> missing;
    for (const helix::ui::ScreensaverInfo& row : helix::ui::kScreensavers) {
        if ((row.depths & helix::ui::kSaverBuildDepth) != 0U && find(row.type) == nullptr) {
            missing.push_back(row.name);
        }
    }
    return missing;
}

ScreensaverManager::~ScreensaverManager() = default;

void ScreensaverManager::set_host(helix::ui::SaverHost host) {
    host_ = std::move(host);
}

helix::ui::ClockInput ScreensaverManager::clock_input() const {
    helix::ui::ClockInput in = helix::ui::saver_clock_now();
    if (host_.print_status) {
        in.print = host_.print_status();
    }
    return in;
}

void ScreensaverManager::start(ScreensaverType type, bool preview) {
    if (type == ScreensaverType::OFF) {
        stop();
        return;
    }
    // A Random run keeps the saver it drew until it stops.
    if (type == ScreensaverType::RANDOM && is_active()) {
        return;
    }

    refresh_identity();
    if (type == ScreensaverType::RANDOM) {
        type = helix::ui::random_screensaver(
            [this](ScreensaverType t) { return too_heavy_for_identity(t); },
            static_cast<uint32_t>(random_()), last_random_);
        last_random_ = type;
        spdlog::info("[ScreensaverManager] Random picked screensaver type {}",
                     static_cast<int>(type));
    }
    const ScreensaverType runs = runnable_type_for_identity(type);
    const bool already_running =
        (active_ && active_->type() == runs && active_->is_active()) || black_screen_type_ == runs;
    if (already_running) {
        return;
    }

    // The screen hold stays out until the new saver has started or failed, so the panel is
    // never uncovered between the two.
    end_current(helix::SaverRunEnd::OTHER);
    counting_ = !preview;
    if (runs != type) {
        spdlog::info("[ScreensaverManager] Type {} is stored as too heavy for board {} on {}; "
                     "running type {} instead",
                     static_cast<int>(type), board_, version_, static_cast<int>(runs));
    }
    launch(runs);
}

void ScreensaverManager::launch(ScreensaverType type) {
    helix::ui::SaverBase* saver = find(type);
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver(type);
    if (!saver || !info) {
        spdlog::warn("[ScreensaverManager] No screensaver registered for type {}",
                     static_cast<int>(type));
        release_screen();
        release_refresh_period();
        return;
    }

    const StartPlan plan = plan_start(*saver, *info);
    if (plan.too_heavy) {
        spdlog::warn("[ScreensaverManager] Every screensaver is stored as too heavy for board {} "
                     "on {}; showing a black screen",
                     board_, version_);
        run_black_screen(type);
        return;
    }

    // The saver moves the held display refresh to its level's period as it starts.
    hold_refresh_period();
    saver->set_start_level(plan.level);
    saver->start();
    if (!saver->is_active()) {
        spdlog::warn("[ScreensaverManager] Screensaver type {} did not start",
                     static_cast<int>(type));
        release_screen();
        release_refresh_period();
        return;
    }

    active_ = saver;
    active_info_ = info;
    begin_gate(plan);
    count_run_start(info->name);
    hold_screen();
    spdlog::info("[ScreensaverManager] Started screensaver type {}", static_cast<int>(type));
}

ScreensaverType ScreensaverManager::runnable_type(ScreensaverType configured) {
    refresh_identity();
    return runnable_type_for_identity(configured);
}

ScreensaverType ScreensaverManager::runnable_type_for_identity(ScreensaverType configured) const {
    return helix::ui::runnable_screensaver(
        configured, [this](ScreensaverType t) { return too_heavy_for_identity(t); });
}

bool ScreensaverManager::too_heavy_here(ScreensaverType type) {
    refresh_identity();
    return too_heavy_for_identity(type);
}

std::vector<ScreensaverType> ScreensaverManager::choices(ScreensaverType configured) {
    refresh_identity();
    return helix::ui::screensaver_choices(
        [this](ScreensaverType t) { return too_heavy_for_identity(t); }, configured);
}

ScreensaverManager::SettingsDropdown
ScreensaverManager::settings_dropdown(ScreensaverType configured) {
    ScreensaverManager& mgr = instance();
    mgr.refresh_identity();
    SettingsDropdown out;
    const auto too_heavy = [&mgr](ScreensaverType t) { return mgr.too_heavy_for_identity(t); };
    out.choices = helix::ui::screensaver_choices(too_heavy, configured);
    out.runs = helix::ui::runnable_screensaver(configured, too_heavy);
    out.runs_too_heavy = too_heavy(out.runs);
    return out;
}

bool ScreensaverManager::too_heavy_for_identity(ScreensaverType type) const {
    if (type == ScreensaverType::OFF) {
        return false;
    }
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver(type);
    if (!info) {
        // Not a saver at all: launch() refuses it rather than running a fallback.
        return false;
    }
    const helix::ui::SaverBase* saver = find(type);
    if (!saver) {
        return true;
    }
    const helix::ui::SaverEnvOverrides env = helix::ui::saver_env_overrides();
    if (env.level && *env.level < saver->level_count()) {
        return false;
    }
    const std::optional<SaverLevelEntry> stored =
        helix::ui::load_level_entry(*helix::Config::get_instance(), info->name);
    return helix::ui::start_entry(stored, version_, board_, saver->level_count()).too_heavy;
}

void ScreensaverManager::refresh_identity() {
    const helix::PlatformCapabilities caps = helix::PlatformCapabilities::detect();
    cores_ = caps.cpu_cores;
    efficiency_cores_only_ = caps.efficiency_cores_only;
    version_ = helix_version_full();
    board_ = helix::ui::board_fingerprint(board_facts(caps));
}

void ScreensaverManager::stop(helix::SaverRunEnd how) {
    end_current(how);
    // The idle stretch the next run subtracts starts now.
    baseline_.reset();
    sampled_ = false;
    release_screen();
    release_refresh_period();
}

void ScreensaverManager::count_run_start(const char* name) {
    if (counting_) {
        usage_.begin(name, std::chrono::steady_clock::now());
    }
}

void ScreensaverManager::count_run_end(helix::SaverRunEnd how, std::optional<size_t> level) {
    usage_.end(how, level, std::chrono::steady_clock::now());
}

void ScreensaverManager::end_current(helix::SaverRunEnd how) {
    if (active_) {
        count_run_end(how, active_->level());
        active_->stop();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {}",
                     static_cast<int>(active_->type()));
        active_ = nullptr;
        active_info_ = nullptr;
    }
    if (black_screen_type_ != ScreensaverType::OFF) {
        count_run_end(how, std::nullopt);
        black_screen_.destroy();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {} (black screen)",
                     static_cast<int>(black_screen_type_));
        black_screen_type_ = ScreensaverType::OFF;
    }
    gate_enabled_ = false;
}

ScreensaverType ScreensaverManager::running_type() const {
    if (active_ && active_->is_active()) {
        return active_->type();
    }
    return black_screen_type_;
}

bool ScreensaverManager::is_active() const {
    return (active_ && active_->is_active()) || black_screen_type_ != ScreensaverType::OFF;
}

ScreensaverType ScreensaverManager::configured_type() {
    const int type_int = helix::DisplaySettingsManager::instance().get_screensaver_type();
    return static_cast<ScreensaverType>(helix::ui::clamp_screensaver_type(type_int));
}

helix::ui::SaverBase* ScreensaverManager::find(ScreensaverType type) const {
    for (const auto& saver : screensavers_) {
        if (saver->type() == type) {
            return saver.get();
        }
    }
    return nullptr;
}

helix::ui::BoardFacts
ScreensaverManager::board_facts(const helix::PlatformCapabilities& caps) const {
    return helix::ui::current_board_facts(caps, host_.display_backend);
}

ScreensaverManager::StartPlan
ScreensaverManager::plan_start(const helix::ui::SaverBase& saver,
                               const helix::ui::ScreensaverInfo& info) {
    const helix::ui::SaverEnvOverrides env = helix::ui::saver_env_overrides();
    budget_override_.reset();
    if (env.budget_pct) {
        budget_override_ = static_cast<double>(*env.budget_pct) / 100.0;
    }
    if (env.level) {
        if (*env.level < saver.level_count()) {
            spdlog::info("[ScreensaverManager] HELIX_SCREENSAVER_LEVEL={} runs {} at that level "
                         "with the gate off",
                         *env.level, info.name);
            return {*env.level, false, false};
        }
        spdlog::warn("[ScreensaverManager] Ignoring HELIX_SCREENSAVER_LEVEL={}: {} has levels 0 "
                     "to {}",
                     *env.level, info.name, saver.level_count() - 1);
    }

    std::optional<SaverLevelEntry> stored;
    const helix::Config* config = helix::Config::get_instance();
    stored = helix::ui::load_level_entry(*config, info.name);

    const SaverLevelEntry entry =
        helix::ui::start_entry(stored, version_, board_, saver.level_count());
    return {entry.level, entry.too_heavy, true};
}

void ScreensaverManager::begin_gate(const StartPlan& plan) {
    gate_enabled_ = plan.gated;
    gated_level_ = active_->level();
    requested_level_ = gated_level_;
    level_ceiling_ = 0;
    comfort_.reset();
    const CpuSample now = cpu_clock_();
    session_.begin(now, baseline_.rate());
    last_sample_ns_ = now.wall_ns;
    sampled_ = true;
}

void ScreensaverManager::on_idle_check_tick() {
    const CpuSample now = cpu_clock_();
    if (sampled_ && now.wall_ns - last_sample_ns_ < kSampleIntervalNs) {
        return;
    }
    last_sample_ns_ = now.wall_ns;
    sampled_ = true;

    if (!active_ || !active_->is_active()) {
        if (black_screen_type_ == ScreensaverType::OFF) {
            baseline_.add(now);
        }
        return;
    }
    if (!gate_enabled_) {
        return;
    }
    if (active_->level() != gated_level_) {
        // The saver applied a different level: measure that level on its own.
        gated_level_ = active_->level();
        session_.restart_window(now);
        comfort_.reset();
        return;
    }

    const std::optional<double> share = session_.add(now);
    // While a step-down waits for the saver's next natural break, windows only keep closing.
    if (!share || requested_level_ != gated_level_) {
        return;
    }

    const bool printing = host_.is_printing && host_.is_printing();
    const double budget = budget_override_.value_or(
        helix::ui::saver_budget_share(cores_, efficiency_cores_only_, printing));
    switch (helix::ui::decide_saver_level(*share, budget, gated_level_, active_->level_count())) {
    case GateDecision::KEEP:
        // Comfort at this rung accumulates toward raising the level again: a
        // stored low level was earned under whatever load ran then, and a
        // sustained quiet run says the board affords more now.
        comfort_.add(*share, budget);
        if (helix::ui::decide_saver_level_up(gated_level_, comfort_.streak(), level_ceiling_) ==
            GateDecision::STEP_UP) {
            requested_level_ = gated_level_ - 1;
            spdlog::info("[ScreensaverManager] {} steady at {:.1f}% of a {:.1f}% budget{}; "
                         "stepping up to level {}",
                         active_info_->name, *share * 100.0, budget * 100.0,
                         printing ? " while printing" : "", requested_level_);
            // A raise measured while printing says nothing about what the board
            // affords idle, so only an idle raise is recorded.
            if (!printing) {
                record_level(requested_level_, false);
            }
            active_->request_level(requested_level_);
        }
        return;
    case GateDecision::STEP_UP:
        // decide_saver_level never raises; a raise comes from the streak above.
        return;
    case GateDecision::STEP_DOWN:
        requested_level_ = gated_level_ + 1;
        level_ceiling_ = std::max(level_ceiling_, requested_level_);
        comfort_.reset();
        spdlog::info("[ScreensaverManager] {} used {:.1f}% of a core against a {:.1f}% budget{}; "
                     "stepping down to level {}",
                     active_info_->name, *share * 100.0, budget * 100.0,
                     printing ? " while printing" : "", requested_level_);
        // The print's budget says nothing about what the board affords idle.
        if (!printing) {
            record_level(requested_level_, false);
        }
        active_->request_level(requested_level_);
        return;
    case GateDecision::TOO_HEAVY: {
        if (printing) {
            spdlog::warn("[ScreensaverManager] {} used {:.1f}% of a core at its lowest level "
                         "against a {:.1f}% budget while printing; blanking the screen for this "
                         "run",
                         active_info_->name, *share * 100.0, budget * 100.0);
        } else {
            spdlog::warn("[ScreensaverManager] {} used {:.1f}% of a core at its lowest level "
                         "against a {:.1f}% budget; it is too heavy for this board until the app "
                         "version or board changes",
                         active_info_->name, *share * 100.0, budget * 100.0);
            record_level(gated_level_, true);
        }
        count_run_end(helix::SaverRunEnd::TOO_HEAVY, gated_level_);
        const ScreensaverType type = active_->type();
        active_->stop();
        active_ = nullptr;
        active_info_ = nullptr;
        gate_enabled_ = false;
        // Unmarked, the saver would resolve to itself again: a print blanks the rest of the run.
        if (printing) {
            run_black_screen(type);
        } else {
            launch(runnable_type_for_identity(type));
        }
        return;
    }
    }
}

void ScreensaverManager::run_black_screen(ScreensaverType type) {
    release_refresh_period();
    show_black_screen(type);
    count_run_start(helix::kSaverUsageBlackScreen);
    hold_screen();
}

void ScreensaverManager::show_black_screen(ScreensaverType type) {
    black_screen_.create();
    black_screen_type_ = type;
    gate_enabled_ = false;
}

void ScreensaverManager::record_level(size_t level, bool too_heavy) {
    helix::Config* config = helix::Config::get_instance();
    if (!active_info_) {
        return;
    }
    helix::ui::save_level_entry(*config, active_info_->name, {level, too_heavy, version_, board_});
}

void ScreensaverManager::hold_screen() {
    if (!holds_screen_) {
        helix::active_screen_hide_hold().acquire(lv_screen_active());
        holds_screen_ = true;
    }
}

void ScreensaverManager::release_screen() {
    if (holds_screen_) {
        holds_screen_ = false;
        helix::active_screen_hide_hold().release();
    }
}

void ScreensaverManager::hold_refresh_period() {
    if (!holds_refresh_period_) {
        helix::active_refresh_period_hold().acquire();
        holds_refresh_period_ = true;
    }
}

void ScreensaverManager::release_refresh_period() {
    if (holds_refresh_period_) {
        holds_refresh_period_ = false;
        helix::active_refresh_period_hold().release();
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
