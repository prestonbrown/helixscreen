// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_registry.h"

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_clock_reading.h"
#include "screensaver_cpu_clock.h"
#include "screensaver_gate.h"
#include "screensaver_overlay.h"
#include "screensaver_usage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Row pitch in bytes lv_canvas_set_buffer() uses for a w-wide canvas in format cf
 *
 * lv_canvas_set_buffer() does not take the caller's word for packing: it
 * derives an LV_DRAW_BUF_STRIDE_ALIGN-rounded stride and sizes the canvas
 * extent from it, so a buffer allocated at w * h * bytes-per-pixel under-runs
 * the extent whenever the stride exceeds w * bytes-per-pixel
 * (prestonbrown/helixscreen#1591). The canvas screensavers size their
 * allocations and direct pixel writes from this pitch so they cannot disagree
 * with what LVGL steps rows by.
 */
inline uint32_t screensaver_canvas_stride_bytes(int32_t w, lv_color_format_t cf) {
    return lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf);
}

class SaverBase;
class ClockInputSink;
struct BoardFacts;

} // namespace helix::ui

namespace helix {
struct PlatformCapabilities;
class ScreensaverManagerTestAccess;
} // namespace helix

/**
 * @brief Interface the manager drives every screensaver through
 */
class Screensaver {
  public:
    virtual ~Screensaver() = default;

    /** @brief Create overlay and begin rendering */
    virtual void start() = 0;

    /** @brief Stop rendering and destroy overlay */
    virtual void stop() = 0;

    /** @brief Check if this screensaver is currently running */
    virtual bool is_active() const = 0;

    /** @brief Return the type identifier for this screensaver */
    virtual ScreensaverType type() const = 0;

    /** @brief Where a clock saver reads the time and the print; nullptr for one without a clock */
    virtual helix::ui::ClockInputSink* clock_input_sink() {
        return nullptr;
    }
};

namespace helix::ui {

/// What the screensaver gate needs from the app.
struct SaverHost {
    /// True while a print job holds the machine; unset reads as not printing.
    std::function<bool()> is_printing;
    /// Running display path for the board fingerprint: "sdl", "fbdev", "drm" or "egl".
    std::string display_backend;
    /// The print's phase, time left and progress; unset reads as no print.
    std::function<ClockPrint()> print_status;
};

} // namespace helix::ui

/**
 * @brief Map a HELIX_SCREENSAVER_NOW value onto a screensaver type
 *
 * A name selects that screensaver. Anything else — "1" included — means the
 * configured one, falling back to flying toasters when nothing is configured.
 *
 * @param value      The environment variable's value
 * @param configured The type from settings, used when @p value names none
 */
namespace helix {
ScreensaverType screensaver_type_from_env(const std::string& value, ScreensaverType configured);
} // namespace helix

/**
 * @brief Owns the screensavers, starts the configured one, and keeps it within its CPU budget
 */
class ScreensaverManager {
  public:
    /** @brief Must not be called before LVGL initialization */
    static ScreensaverManager& instance();

    ScreensaverManager(const ScreensaverManager&) = delete;
    ScreensaverManager& operator=(const ScreensaverManager&) = delete;

    /// Samples closer together than this are skipped by on_idle_check_tick().
    static constexpr uint64_t kSampleIntervalNs = 250000000ULL;

    /**
     * @brief Start the specified screensaver type
     *
     * Stops any currently active screensaver first. OFF stops and starts nothing.
     *
     * The saver starts at the level stored for it on this board and app version, or at level 0
     * (HELIX_SCREENSAVER_LEVEL forces a level and turns the gate off for the run). A saver
     * stored as too heavy gives way to runnable_type(type); with every saver too heavy, a
     * static black screen shows instead.
     *
     * While a saver runs, the active screen is hidden beneath its overlay
     * (helix::ScreenHideHold), and it stays hidden across a switch between types.
     * For the same span the display refreshes at the running saver's frame period,
     * following its level; level 0 runs at HELIX_SCREENSAVER_REFR_PERIOD_MS when one is
     * configured (helix::RefreshPeriodHold). The black screen does not take that hold.
     * A saver that fails to start leaves the manager inactive, and the screen and
     * refresh period as they were before any saver ran.
     *
     * A run counts in usage() unless `preview` says it is the settings Test button's.
     */
    void start(ScreensaverType type, bool preview = false);

    /**
     * @brief Registry savers this build can draw but has no instance for
     *
     * Which savers a colour depth can draw is stated in four places that agree only by
     * convention: SCREENSAVERS::depths, the Makefile's source filter, and this file's include
     * and registration guards. When they disagree the type is selectable and starts nothing,
     * which looks to a user like the screen simply never blanking. Empty in a correct build.
     */
    std::vector<const char*> savers_missing_for_build_depth() const;

    /**
     * @brief The saver start(configured) runs on this board and app version
     *
     * `configured`, or when it is too heavy here the fallback helix::ui::runnable_screensaver
     * picks. The stored setting is left alone.
     */
    ScreensaverType runnable_type(ScreensaverType configured);

    /**
     * @brief Whether this board cannot run `type`
     *
     * True when its stored entry for this board and app version is marked too heavy, or the
     * build has no instance of a registered saver. False for OFF, for a value the registry does
     * not know, and for a saver HELIX_SCREENSAVER_LEVEL runs at a forced level with the gate off.
     */
    bool too_heavy_here(ScreensaverType type);

    /// The settings dropdown's options on this board with `configured` stored
    /// (helix::ui::screensaver_choices).
    std::vector<ScreensaverType> choices(ScreensaverType configured);

    /// What the settings dropdown lists and selects with `configured` stored.
    struct SettingsDropdown {
        std::vector<ScreensaverType> choices;
        /// The saver that runs, which the dropdown selects.
        ScreensaverType runs = ScreensaverType::OFF;
        /// Only on a board where every saver is too heavy: `runs` is the stored saver, listed
        /// so that choosing Off is a change.
        bool runs_too_heavy = false;
    };
    static SettingsDropdown settings_dropdown(ScreensaverType configured);

    /** @brief Stop whatever screensaver or black screen is active; `how` is what usage() records */
    void stop(helix::SaverRunEnd how = helix::SaverRunEnd::OTHER);

    /// Seconds shown, starts and run endings per saver for telemetry; previews are left out.
    helix::SaverUsage& usage() {
        return usage_;
    }

    /** @brief True while a screensaver or its black screen is up */
    bool is_active() const;

    /// The saver running now, the type its black screen stands in for, or OFF. Never RANDOM:
    /// a Random run reports the saver it drew.
    ScreensaverType running_type() const;

    /** @brief Read configured screensaver type from DisplaySettingsManager, clamped */
    static ScreensaverType configured_type();

    /// Gives the gate the app's print state and display path.
    void set_host(helix::ui::SaverHost host);

    /**
     * @brief Samples the process CPU clock for the gate
     *
     * Call on the display manager's idle-check tick. While no saver runs, samples feed the idle
     * baseline. While one runs they close the gate's windows: over budget steps the saver down
     * one level and stores that level; over budget at the lowest level stores the board as too
     * heavy and replaces the saver with its fallback (runnable_type), or a static black screen
     * when every saver is too heavy.
     */
    void on_idle_check_tick();

  private:
    friend class helix::ScreensaverManagerTestAccess;

    struct StartPlan {
        size_t level = 0;
        bool too_heavy = false;
        bool gated = true;
    };

    ScreensaverManager();
    ~ScreensaverManager();

    /** @brief Find screensaver instance by type, or nullptr */
    helix::ui::SaverBase* find(ScreensaverType type) const;

    /// Level, too-heavy mark and gating a run of `saver` starts with, for the identity
    /// refresh_identity() last read.
    StartPlan plan_start(const helix::ui::SaverBase& saver, const helix::ui::ScreensaverInfo& info);

    /// Reads the core count, app version and board fingerprint the level store is keyed by.
    void refresh_identity();

    /// too_heavy_here() against the identity refresh_identity() last read.
    bool too_heavy_for_identity(ScreensaverType type) const;

    /// runnable_type() against the identity refresh_identity() last read.
    ScreensaverType runnable_type_for_identity(ScreensaverType configured) const;

    /// Starts `type`, already resolved by runnable_type(), keeping the screen hold out. A saver
    /// stored as too heavy shows the black screen.
    void launch(ScreensaverType type);

    helix::ui::BoardFacts board_facts(const helix::PlatformCapabilities& caps) const;

    /// Starts measuring the saver that just started.
    void begin_gate(const StartPlan& plan);

    /// Stops the running saver or black screen without giving back the holds.
    void end_current(helix::SaverRunEnd how);

    /// Records in usage_ that a run of `name` started, unless this one is a preview.
    void count_run_start(const char* name);

    /// Records in usage_ that the counted run ended, at `level` (none for the black screen).
    void count_run_end(helix::SaverRunEnd how, std::optional<size_t> level);

    /// Shows the static black overlay in place of `type`.
    void show_black_screen(ScreensaverType type);

    /// Runs the black screen in place of `type`: no held refresh period, counted, screen held.
    void run_black_screen(ScreensaverType type);

    /// Stores `level` and `too_heavy` for the running saver on this board and version.
    void record_level(size_t level, bool too_heavy);

    /** @brief Take the active-screen hide hold, once however many savers run in turn */
    void hold_screen();

    /** @brief Give back the hold taken by hold_screen(), if one is out */
    void release_screen();

    /** @brief Take the refresh period hold, once however many savers run in turn */
    void hold_refresh_period();

    /** @brief Give back the hold taken by hold_refresh_period(), if one is out */
    void release_refresh_period();

    /// The local time, date and the host's print, for the clock savers.
    helix::ui::ClockInput clock_input() const;

    std::vector<std::unique_ptr<helix::ui::SaverBase>> screensavers_;
    helix::ui::SaverBase* active_ = nullptr;
    const helix::ui::ScreensaverInfo* active_info_ = nullptr;
    helix::ui::SaverOverlay black_screen_;
    ScreensaverType black_screen_type_ = ScreensaverType::OFF;
    /// Draws a Random run's saver; last_random_ is the one it drew before.
    std::minstd_rand random_;
    ScreensaverType last_random_ = ScreensaverType::OFF;
    bool holds_screen_ = false;
    bool holds_refresh_period_ = false;

    helix::ui::SaverHost host_;
    helix::ui::CpuClockFn cpu_clock_;
    helix::ui::IdleBaseline baseline_;
    helix::ui::SaverGateSession session_;
    helix::ui::SaverComfortStreak comfort_;
    bool gate_enabled_ = false;
    std::optional<double> budget_override_;
    int cores_ = 0;
    float bogomips_ = 0.0f;
    size_t gated_level_ = 0;
    size_t requested_level_ = 0;
    /// The highest rung a raise may reach this run: a rung that stepped down stays closed.
    size_t level_ceiling_ = 0;
    uint64_t last_sample_ns_ = 0;
    bool sampled_ = false;
    std::string version_;
    std::string board_;

    helix::SaverUsage usage_;
    /// False while the running saver is a preview, which usage_ leaves out.
    bool counting_ = false;
};

#endif // HELIX_ENABLE_SCREENSAVER
