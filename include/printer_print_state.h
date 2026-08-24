// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "async_lifetime_guard.h"
#include "print_job_ref.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif

#include <atomic>
#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "hv/json.hpp"

// Forward declaration - enums are defined in printer_state.h
namespace helix {
enum class PrintJobState;
}
namespace helix {
enum class PrintOutcome;
}
namespace helix {
enum class PrintStartPhase;
}

namespace helix {

/**
 * @brief Manages print-related subjects for printer state
 *
 * Tracks print progress, state, timing, layers, and print start phases.
 * Provides 18 subjects for reactive UI updates during printing.
 * Extracted from PrinterState as part of god class decomposition.
 *
 * @note This class manages only the subjects and their values. The enums
 *       (PrintJobState, PrintOutcome, PrintStartPhase) remain in printer_state.h
 *       as they are widely used across the codebase.
 */
class PrinterPrintState {
  public:
    PrinterPrintState();
    /// Defined in the .cpp so it cancels the preparing watchdog. CLAUDE.md
    /// threading rule 5: a raw lv_timer_t cancelled in a teardown path must also
    /// be cancelled in the destructor, or the timer stays armed on freed `this`
    /// when StaticPanelRegistry::destroy_all() runs before lv_deinit().
    ~PrinterPrintState();

    // Non-copyable
    PrinterPrintState(const PrinterPrintState&) = delete;
    PrinterPrintState& operator=(const PrinterPrintState&) = delete;

    /**
     * @brief Initialize print subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update print state from Moonraker status JSON
     * @param status JSON object containing print_stats, virtual_sdcard data
     */
    void update_from_status(const nlohmann::json& status);

    /// True if a Moonraker status object indicates an active (printing or paused) print.
    /// Pure: depends only on status["print_stats"]["state"]. Used by both the
    /// print_active subject update and the discovery-time idle gate so they agree.
    static bool status_indicates_active_print(const nlohmann::json& status);

    /**
     * @brief Reset UI state when starting a new print
     *
     * Clears progress, layers, and timing but preserves filename.
     */
    void reset_for_new_print();

    // ========================================================================
    // Subject accessors (18 subjects)
    // ========================================================================

    /// Print progress as 0-100 percent, straight from Moonraker.
    /// Drives time estimates, which key off progress being 0 before a print.
    lv_subject_t* get_print_progress_subject() {
        return &print_progress_;
    }

    /// Progress for display: tracks print_progress_ until the print reaches a
    /// terminal state, then holds its final value (100 on completion) until the
    /// next print starts. Moonraker zeroes print_progress_ in the same batch as
    /// STANDBY, so a display bound to the raw value drops to 0 the instant a
    /// print finishes.
    lv_subject_t* get_print_progress_display_subject() {
        return &print_progress_display_;
    }

    /// print_progress_display_ rendered as "N%". Written by the same call that
    /// writes the int, so a bar and its label can never disagree.
    lv_subject_t* get_print_progress_text_subject() {
        return &print_progress_text_;
    }

    /// Raw filename from Moonraker
    lv_subject_t* get_print_filename_subject() {
        return &print_filename_;
    }

    /// String state for UI display ("standby", "printing", etc.)
    lv_subject_t* get_print_state_subject() {
        return &print_state_;
    }

    /// Integer enum value for type-safe logic (PrintJobState)
    ///
    /// RAW_PRINT_STATE_OK: this IS the wire subject. Consumers asking a
    /// capability question want get_print_lifecycle_subject() instead - this one
    /// cannot express a start the app has committed to but the printer has not
    /// reported. Pair it with observe_print_state(), never the lifecycle
    /// factory: the two enums do not share numbering.
    lv_subject_t* get_print_state_enum_subject() {
        return &print_state_enum_;
    }

    /**
     * @brief Lifetime token for the "static" print subjects (e.g. print_state_enum).
     *
     * Production-wise these subjects live for the process, but tests call
     * `deinit_subjects()` / `init_subjects()` between cases. Cross-singleton
     * observers (e.g. AmsState's print-state observer) MUST pass this token to
     * `observe_int_sync(...)` — otherwise an ObserverGuard outliving a
     * `deinit_subjects()` cycle will UAF in `lv_observer_remove()` (subject
     * deinit already freed the observer node).
     */
    [[nodiscard]] SubjectLifetime get_static_subjects_lifetime() const {
        return static_subjects_lifetime_;
    }

    /// 1 when PRINTING or PAUSED, 0 otherwise
    lv_subject_t* get_print_active_subject() {
        return &print_active_;
    }

    /// Terminal outcome that persists (PrintOutcome)
    lv_subject_t* get_print_outcome_subject() {
        return &print_outcome_;
    }

    /// Combined: 1 when active AND not in start phase
    lv_subject_t* get_print_show_progress_subject() {
        return &print_show_progress_;
    }

    /// Clean display filename without path/prefix
    lv_subject_t* get_print_display_filename_subject() {
        return &print_display_filename_;
    }

    /// LVGL path to current print thumbnail
    lv_subject_t* get_print_thumbnail_path_subject() {
        return &print_thumbnail_path_;
    }

#if defined(HELIX_PLATFORM_ESP32)
    /// Bumped every time the PSRAM thumbnail below is replaced. ESP32 has no
    /// disk thumbnail cache, so print_thumbnail_path_ stays empty there and
    /// consumers observe this counter instead, then pull the shared_ptr.
    lv_subject_t* get_print_psram_thumb_gen_subject() {
        return &print_psram_thumb_gen_;
    }

    /// Current print's PSRAM-resident thumbnail, or nullptr when none is
    /// loaded. UI thread only — the returned shared_ptr keeps the buffer alive
    /// for as long as a widget's image src points at its descriptor.
    [[nodiscard]] std::shared_ptr<helix::ui::EspPsramThumbnail> get_print_psram_thumbnail() const {
        return print_psram_thumbnail_;
    }
#endif

    /**
     * @brief Image the print-thumbnail subject carries when there is no thumbnail
     *
     * The subject is NEVER the empty string — not at init, and not for a file
     * with no thumbnail of its own. `""` reaches lv_image_set_src with a first
     * byte of 0x00, which lv_image_src_get_type() classifies as
     * LV_IMAGE_SRC_VARIABLE (lv_draw_image.c:211), so LVGL then dereferences the
     * one-byte literal as an lv_image_dsc_t. Every consumer used to carry its
     * own guard against that; publishing an explicit placeholder removes the
     * input instead of the guards' need to disagree about it.
     *
     * Re-exported as ActivePrintMediaManager::no_thumbnail_placeholder(), which
     * is the name the sole writer of this subject publishes it under.
     *
     * Resolved through helix::asset_component_uri() rather than spelled as a
     * literal: on firmware the bundle mounts at a configured root, so a raw
     * "A:assets/images/..." misses the mount and lv_image_set_src fails to open
     * it — LVGL then clears the widget. Identity on desktop (asset root ".").
     * The resolved string is cached on first call, which is safe because every
     * caller runs after helix::set_asset_root().
     */
    static const char* no_thumbnail_placeholder();

    /**
     * @brief Gcode filename the current thumbnail path was produced for
     *
     * Written by set_print_thumbnail() BEFORE the path subject is published, so an
     * observer of get_print_thumbnail_path_subject() can trust this describes the
     * path it is holding. Empty when no thumbnail identity has been set.
     */
    [[nodiscard]] const std::string& get_print_thumbnail_file() const {
        return print_thumbnail_file_;
    }

    /// Current layer number (0-based)
    lv_subject_t* get_print_layer_current_subject() {
        return &print_layer_current_;
    }

    /// Total layers from file metadata
    lv_subject_t* get_print_layer_total_subject() {
        return &print_layer_total_;
    }

    /// Elapsed print time in seconds (extrusion time only, from Moonraker print_duration)
    lv_subject_t* get_print_duration_subject() {
        return &print_duration_;
    }

    /// Wall-clock elapsed time in seconds (from Moonraker total_duration, includes prep)
    lv_subject_t* get_print_elapsed_subject() {
        return &print_elapsed_;
    }

    /// Estimated remaining time in seconds
    lv_subject_t* get_print_time_left_subject() {
        return &print_time_left_;
    }

    /// Filament used during current print (in mm, from Moonraker print_stats.filament_used)
    lv_subject_t* get_print_filament_used_subject() {
        return &print_filament_used_;
    }

    /**
     * @brief Per-extruder filament_used (mm, integer), 0-based.
     *
     * Populated from Klipper's per-object `extruder`/`extruder1`/`extruder2`/...
     * `filament_used` fields during status updates. Callers use the returned
     * subject to observe one tool's consumption independently of the aggregate
     * `print_stats.filament_used` stream.
     *
     * Map entries are **pre-populated** in `init_subjects()` for all indices
     * `0 .. MAX_EXTRUDER_SCAN-1`, so the map structure is frozen after init.
     * This eliminates the WebSocket-BG-thread vs UI-thread rehash race that
     * lazy emplace would expose (only subject values change during status
     * updates, and `lv_subject_set_int` is atomic for the int value).
     *
     * These subjects are still **dynamic** ([L077]): they are re-created on
     * `deinit_subjects()` / `init_subjects()` cycles. Observers MUST pass a
     * SubjectLifetime token and subscribe via `observe_int_sync(..., lifetime)`
     * — otherwise ObserverGuard dangles on reconnect.
     *
     * @param extruder_idx 0-based extruder index (0 = "extruder", 1 = "extruder1", ...)
     * @param[out] lifetime Token whose expiration signals subject death
     * @return Non-null subject pointer for `0 <= idx < MAX_EXTRUDER_SCAN` once
     *         `init_subjects()` has run; `nullptr` otherwise (lifetime untouched).
     */
    lv_subject_t* get_extruder_filament_used_subject(int extruder_idx, SubjectLifetime& lifetime);

    /// Maximum number of per-extruder filament subjects pre-populated at init.
    /// Klipper toolchanger setups max out well below this.
    static constexpr int MAX_EXTRUDER_SCAN = 16;

    /// Current PrintStartPhase enum value
    /**
     * @brief Begin preparing a job, on the main thread
     *
     * The commit path: the user has chosen this job and work has started, but
     * the printer has not been handed it yet. Clears the previous job's
     * terminal state and raises the first pre-print phase.
     *
     * Synchronous by design. set_print_start_state() defers because it is
     * called from WebSocket callbacks; a button press is already on the main
     * thread, so the clear lands before anything can render a Preparing state
     * next to the finished job's numbers.
     */
    void begin_preparing(const PrintJobRef& job);

    /// Stop preparing. Every reason routes through here; see PreparingExit.
    void retire_preparing(PreparingExit reason);

    [[nodiscard]] bool has_preparing_job() const {
        return !preparing_job_.empty();
    }
    [[nodiscard]] const PrintJobRef& preparing_job() const {
        return preparing_job_;
    }

    /**
     * @brief Bumped each time a job starts preparing; 0 when none is
     *
     * Consumers that must adopt the new job's identity - the media manager
     * above all - observe this instead of relying on every start path
     * remembering to tell them. An epoch rather than a flag, so two
     * back-to-back prints of the SAME file are still distinguishable.
     */
    lv_subject_t* get_preparing_epoch_subject() {
        return &preparing_epoch_;
    }

    /**
     * @brief The UI-level print state as it was before the current one
     *
     * Published from the same place that computes the transition, so a consumer
     * can ask "what just happened?" without keeping a private previous-state
     * variable. Eight of those existed, and they disagreed at the edges.
     *
     * Only rewritten when the state actually changes - otherwise it collapses
     * to equal the current state and every consumer sees a self-transition.
     */
    lv_subject_t* get_print_lifecycle_prev_subject() {
        return &print_lifecycle_prev_;
    }

    /// Why the last preparing job ended. Meaningful once the epoch reads 0.
    [[nodiscard]] PreparingExit last_preparing_exit() const {
        return last_preparing_exit_;
    }

    /**
     * @brief The authoritative UI-level print state (PrintState enum)
     *
     * Derived from print_stats.state and the pre-print phase by
     * derive_print_state(). Consumers observe this instead of re-deriving
     * their own answer from the raw job-state enum.
     */
    lv_subject_t* get_print_lifecycle_subject() {
        return &print_lifecycle_;
    }

    /**
     * @brief Boolean form of job_holds_machine(), for XML bindings
     *
     * 1 whenever the lifecycle is Preparing, Printing or Paused. This is what
     * motion, jog and extrude controls bind to; `print_active` is the same
     * question asked of the wire, so it reads 0 for the whole of a host-side
     * pre-print block while the toolhead is homing and probing.
     *
     * C++ callers should prefer `job_holds_machine(lifecycle)` on the value they
     * already hold. The subject exists because XML cannot call a predicate.
     */
    lv_subject_t* get_job_holds_machine_subject() {
        return &job_holds_machine_;
    }

    lv_subject_t* get_print_start_phase_subject() {
        return &print_start_phase_;
    }

    /// Human-readable phase message
    lv_subject_t* get_print_start_message_subject() {
        return &print_start_message_;
    }

    /// Print start progress 0-100%
    lv_subject_t* get_print_start_progress_subject() {
        return &print_start_progress_;
    }

    /// 1 while print workflow executing, 0 otherwise
    lv_subject_t* get_print_in_progress_subject() {
        return &print_in_progress_;
    }

    /// Predicted pre-print time remaining (formatted string, e.g. "~2 min left")
    lv_subject_t* get_print_start_time_left_subject() {
        return &print_start_time_left_;
    }

    /// Predicted pre-print time remaining in seconds (for augmenting total remaining)
    lv_subject_t* get_preprint_remaining_subject() {
        return &preprint_remaining_;
    }

    /// Pre-print elapsed seconds (time since preparation started)
    lv_subject_t* get_preprint_elapsed_subject() {
        return &preprint_elapsed_;
    }

    /// Klipper display message (from M117 / display_status.message)
    lv_subject_t* get_display_message_subject() {
        return &display_message_;
    }

    /// 1 when display_message is non-empty, 0 when empty (for XML visibility binding)
    lv_subject_t* get_display_message_visible_subject() {
        return &display_message_visible_;
    }

    /// Klipper print_stats.message — typically populated on pause/error to describe
    /// the reason (e.g. "Filament Sensor filament_sensor: Runout Detected" or error
    /// strings). Empty for normal user-initiated PAUSE without a configured message.
    lv_subject_t* get_print_message_subject() {
        return &print_message_;
    }

    /// print_stats.exception (Snapmaker U1 structured pause descriptor) id, or
    /// -1 when no exception is latched. Hardware-verified ids: 523 = recoverable
    /// runout, 532 = terminal dirty-bed (#991). Main-thread only.
    [[nodiscard]] int get_print_exception_id() const {
        return print_exception_id_;
    }

    /// print_stats.exception code, or -1 when absent. (Runout = 0, dirty-bed = 1.)
    [[nodiscard]] int get_print_exception_code() const {
        return print_exception_code_;
    }

    /// print_stats.exception message — the human-readable pause reason. On these
    /// firmware pauses print_stats.message is EMPTY and the text lives here.
    [[nodiscard]] const std::string& get_print_exception_message() const {
        return print_exception_message_;
    }

    /// virtual_sdcard.pl_env_valid — Snapmaker-fork Power-Loss-Recovery flag.
    /// True only after the firmware validates a coherent power-loss snapshot
    /// against MCU flash on boot; default 0, and stays 0 on mainline Klipper
    /// (field absent). Main-thread only (updated by update_from_status).
    lv_subject_t* get_pl_env_valid_subject() {
        return &pl_env_valid_;
    }

    /// True when virtual_sdcard.pl_env_valid is set. See get_pl_env_valid_subject().
    [[nodiscard]] bool is_pl_env_valid() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&pl_env_valid_)) != 0;
    }

    /// virtual_sdcard.file_path — the file a Power-Loss-Recovery restore would
    /// resume. Only meaningful when is_pl_env_valid() is true. Main-thread only.
    [[nodiscard]] const std::string& pl_recovery_file() const {
        return pl_recovery_file_;
    }

    /// Clear the cached Power-Loss-Recovery file path. Used on the disconnect
    /// edge alongside forcing pl_env_valid back to 0, so a reconnect starts from
    /// a clean slate and re-derives both from the fresh status. Main-thread only.
    void clear_pl_recovery_file() {
        pl_recovery_file_.clear();
    }

    /// print_stats.power_loss PRESENCE — the Creality-Klipper-fork capability
    /// marker for Power-Loss-Recovery. 1 once a status payload has carried the
    /// key as a JSON number; mainline Klipper never emits it, and Moonraker's
    /// explicit null for a subscribed-but-unpopulated field does not count.
    /// Latches UP only (status arrives as deltas) and is reset by the offer
    /// controller on the disconnect edge. See docs/devel/POWER_LOSS_RECOVERY.md.
    lv_subject_t* get_creality_plr_capable_subject() {
        return &creality_plr_capable_;
    }

    /// True when print_stats.power_loss has been seen. See
    /// get_creality_plr_capable_subject().
    [[nodiscard]] bool is_creality_plr_capable() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&creality_plr_capable_)) != 0;
    }

    // ========================================================================
    // Setters
    // ========================================================================

    /**
     * @brief Set print outcome for UI badge display
     * @param outcome The print outcome value to set
     */
    void set_print_outcome(PrintOutcome outcome);

    /**
     * @brief Set the current print's thumbnail, tagged with the file it is for
     *
     * Main thread only — publishing the path fires observers synchronously.
     *
     * @param for_file Gcode filename this path was produced for ("" to clear identity)
     * @param path LVGL-compatible path (e.g., "A:/tmp/thumbnail_xxx.bin"), "" to clear
     */
    void set_print_thumbnail(const std::string& for_file, const std::string& path);

#if defined(HELIX_PLATFORM_ESP32)
    /**
     * @brief Install the current print's PSRAM-resident thumbnail
     *
     * MAIN THREAD ONLY. Two reasons, both hard: the subject bump notifies
     * observers that call LVGL widget APIs, and replacing the shared_ptr can
     * drop the last reference to the previous thumbnail, whose destructor
     * calls lv_image_cache_drop(). Background callers must marshal via
     * tok.defer()/ui_queue_update() first.
     *
     * @param thumb New thumbnail (may be nullptr to clear)
     */
    void set_print_psram_thumbnail(std::shared_ptr<helix::ui::EspPsramThumbnail> thumb);
#endif

    /**
     * @brief Set display-ready print filename for UI binding
     * @param name Clean display name
     */
    void set_print_display_filename(const std::string& name);

    /**
     * @brief Set total layer count from file metadata
     * @param total Total number of layers
     */
    void set_print_layer_total(int total);

    /**
     * @brief Set slice layer heights from file metadata (for Z-height derivation)
     *
     * Thread-safe: marshals via helix::ui::queue_update() because the metadata
     * callback runs on a background/HttpExecutor thread, while update_from_status
     * reads these members on the main thread. When first_layer_height <= 0 it
     * falls back to layer_height.
     *
     * @param layer_height Slice per-layer height in mm
     * @param first_layer_height Slice first-layer height in mm (may differ)
     */
    void set_print_layer_heights(double layer_height, double first_layer_height);

    /**
     * @brief Set current layer number (gcode response fallback)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     * Called from gcode response parser when print_stats.info doesn't fire.
     *
     * @param layer Current layer number
     */
    void set_print_layer_current(int layer);

    /**
     * @brief Check if real layer data has been received from slicer/Moonraker.
     * When false, layer count is estimated from print progress.
     */
    bool has_real_layer_data() const {
        return has_real_layer_data_;
    }

    /**
     * @brief Is the displayed current layer trustworthy (not a progress guess)?
     *
     * True when the layer came from a real slicer/Moonraker field
     * (has_real_layer_data_) OR from the Z-height derivation, which tracks actual
     * commanded geometry and matches Mainsail. False only when the value is the
     * byte/time progress-fraction estimate (which drifts high early in a print).
     * The print-status label uses this to decide whether to prefix "~".
     * Distinct from has_real_layer_data() on purpose: Z-derived layers are
     * accurate for display but must NOT satisfy has_real_layer_data() (that flag
     * gates pre-print completion — see printer_reports_layers_).
     */
    bool layer_is_accurate() const {
        return has_real_layer_data_ || layer_z_derived_;
    }

    /**
     * @brief Sticky: has this printer EVER reported a real layer field this session?
     *
     * True once any of print_stats.info.current_layer, print_stats.info.total_layer,
     * or virtual_sdcard.layer has been observed at least once. NOT reset between
     * prints (printer capability, not per-print state). Used by the pre-print
     * completion gate to choose the real-first-layer path vs the print_duration
     * fallback without racing reset_for_new_print(). See printer_reports_layers_.
     */
    bool printer_reports_layers() const {
        return printer_reports_layers_;
    }

    /**
     * @brief True when Klipper's virtual_sdcard is actively playing back gcode.
     *
     * Distinct from PrintJobState — `state=="paused"` can coexist with
     * `is_active==false` (e.g. Snapmaker U1 dirty-bed exception, level-2
     * aborts, or anything that calls SDCARD_RESET_FILE without first
     * cancelling). In that "paused but inactive" state a plain RESUME is a
     * no-op: pause_resume.resume() runs the macro chain but has no SD
     * context to resume, so the print is effectively terminated.
     *
     * Backends use this to detect dirty-bed/abort scenarios in
     * prepare_for_resume and surface a "Restart from beginning?" UX instead
     * of firing a useless RESUME.
     *
     * Main-thread only (updated by update_from_status from the notification
     * pump). Default false until the first status update.
     */
    bool is_sdcard_active() const {
        return sdcard_active_;
    }

    /**
     * @brief Set print start phase and update message/progress
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param phase Current PrintStartPhase
     * @param message Human-readable message (e.g., "Heating Nozzle...")
     * @param progress Estimated progress 0-100%
     */
    void set_print_start_state(PrintStartPhase phase, const char* message, int progress);

    /**
     * @brief Reset print start to IDLE
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     */
    void reset_print_start_state();

    /**
     * @brief Set the print-in-progress flag (UI workflow state)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     */
    void set_print_in_progress(bool in_progress);

    /**
     * @brief Set predicted pre-print time remaining string
     *
     * Main-thread only (called from LVGL timer).
     * @param text Formatted string (e.g., "~2 min left") or empty to clear
     */
    void set_print_start_time_left(const char* text);

    /**
     * @brief Clear predicted pre-print time remaining
     */
    void clear_print_start_time_left();

    /**
     * @brief Set pre-print remaining seconds (for total remaining augmentation)
     *
     * Main-thread only (called from LVGL timer).
     */
    void set_preprint_remaining_seconds(int seconds);

    /**
     * @brief Set pre-print elapsed seconds (for elapsed display during preparation)
     *
     * Main-thread only (called from LVGL timer).
     */
    void set_preprint_elapsed_seconds(int seconds);

    /**
     * @brief Set slicer's estimated total print time (from file metadata)
     *
     * Used as a fallback for remaining time when print_duration is still 0.
     * @param seconds Slicer's estimated total print time in seconds
     */
    void set_estimated_print_time(int seconds);

    /**
     * @brief Get slicer's estimated total print time
     * @return Estimated print time in seconds, or 0 if not set
     */
    int get_estimated_print_time() const;

    // ========================================================================
    // State queries
    // ========================================================================

    /**
     * @brief Get current print job state as enum
     * @return Current PrintJobState
     *
     * RAW_PRINT_STATE_OK: this IS the wire accessor; get_print_lifecycle() is
     * the derived one.
     */
    PrintJobState get_print_job_state() const;

    /**
     * @brief Get the derived print lifecycle
     *
     * The typed counterpart to get_print_job_state(), and the ONLY way call
     * sites should read `print_lifecycle`. Reading the subject by hand means
     * hand-casting the subject's int, and because
     * lv_subject_get_int() returns int that cast compiles against whichever
     * subject you happened to name - so pairing it with the raw
     * print_state_enum subject is silent, and the two enums do NOT share
     * numbering past index 0:
     *
     *     PrintJobState  STANDBY=0 PRINTING=1 PAUSED=2 COMPLETE=3 ...
     *     PrintState     Idle=0    Preparing=1 Printing=2 Paused=3 ...
     *
     * so a COMPLETE job reads back as Paused and a PRINTING one as Preparing.
     * That mistake was made twice while migrating guards onto the lifecycle
     * (ams_backend_ad5x_ifs, power_device_state) and was invisible both times
     * until a test caught it. This accessor removes the cast, and with it the
     * opportunity.
     */
    [[nodiscard]] PrintState get_print_lifecycle() const;

    /**
     * @brief Check if a new print can be started
     * @return true if printer is in a state that allows starting a new print
     */
    [[nodiscard]] bool can_start_new_print() const;

    /**
     * @brief Check if a print workflow is currently in progress
     * @return true during print preparation
     */
    [[nodiscard]] bool is_print_in_progress() const;

    /**
     * @brief Check if currently in print start phase
     * @return true if phase is not IDLE
     */
    [[nodiscard]] bool is_in_print_start() const;

    /// Recompute and publish print_lifecycle from job state + pre-print phase.
    void publish_lifecycle_state();

    /**
     * @brief Settle a live preparing job against what the printer reports
     *
     * Idempotent, and called from both the job-state and filename parse points
     * because either can arrive first. Only a PRINTING report settles anything:
     * the previous job going terminal while ours prepares is the whole reason
     * this exists, so it must leave the claim intact.
     */
    void reconcile_preparing();

  private:
    friend class PrinterPrintStateTestAccess;

    /**
     * @brief Update print_show_progress_ combined subject
     *
     * Sets print_show_progress_ to 1 only when print_active==1 AND print_start_phase==IDLE.
     */
    void update_print_show_progress();

    /**
     * @brief Update display_message_visible_ derived subject
     *
     * Visible whenever display_message is non-empty, including during pre-print:
     * PRINT_START macros are where most M117 traffic originates, and the
     * collector's phase label lives in a separate subject (print_start_message),
     * so there is nothing here to duplicate.
     */
    void update_display_message_visible();

    /// Write print_progress_display_ and print_progress_text_ together. The sole
    /// writer of both, so the int and its rendered string cannot drift apart.
    /// No-ops while progress_frozen_ is set.
    void publish_progress_display(int percent);

    /// Hold the current display progress until the next print starts. Called on
    /// the transition into COMPLETE/CANCELLED/ERROR; completion pins 100 first.
    void freeze_progress_display(bool complete);

    /// Resume tracking print_progress_ and reset the display to 0.
    void unfreeze_progress_display();

    /**
     * @brief Internal setter for print-in-progress flag
     *
     * Called via helix::async::invoke from set_print_in_progress().
     */
    void set_print_in_progress_internal(bool in_progress);

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    /// Lifetime for the "static" subjects below. Reset (to false then released)
    /// in `deinit_subjects()` so cross-singleton observers can detect subject
    /// death and skip `lv_observer_remove()` on freed observer nodes.
    SubjectLifetime static_subjects_lifetime_;

    /// Generation guard for the setters that defer their subject writes to the
    /// main thread. Invalidated by `deinit_subjects()` and by destruction, so a
    /// callback still sitting in the UpdateQueue when the subjects go away is
    /// dropped instead of notifying a freed observer list (#1165, #1146).
    /// Distinct from `static_subjects_lifetime_`, which is a `shared_ptr<bool>`
    /// read by observers and carries no deferral machinery.
    AsyncLifetimeGuard async_lifetime_;

    // Print progress subjects
    lv_subject_t print_progress_{};         // Integer 0-100
    lv_subject_t print_progress_display_{}; // Integer 0-100, frozen after completion
    lv_subject_t print_progress_text_{};    // String: print_progress_display_ as "N%"
    lv_subject_t print_filename_{};         // String buffer
    lv_subject_t print_state_{};            // String buffer (for UI display)
    lv_subject_t print_state_enum_{};       // Integer: PrintJobState enum
    lv_subject_t print_active_{};           // Integer: 1 when PRINTING/PAUSED
    lv_subject_t print_outcome_{};          // Integer: PrintOutcome enum
    lv_subject_t print_show_progress_{};    // Integer: 1 when active AND not starting
    lv_subject_t print_display_filename_{}; // String: clean filename
    lv_subject_t print_thumbnail_path_{};   // String: LVGL thumbnail path

#if defined(HELIX_PLATFORM_ESP32)
    // ESP32 thumbnail route (Task 11 R2): no disk cache on this platform, so
    // the image lives in PSRAM behind a shared_ptr instead of at a path. The
    // counter subject is what UI code observes; the pointer is what it reads.
    lv_subject_t print_psram_thumb_gen_{}; // Integer: bumped on every install
    std::shared_ptr<helix::ui::EspPsramThumbnail> print_psram_thumbnail_;
#endif

    // Layer tracking subjects
    lv_subject_t print_layer_current_{}; // Current layer (0-based)
    lv_subject_t print_layer_total_{};   // Total layers

    // Slice geometry from file metadata, used for Z-height layer derivation when
    // the slicer never reports a layer number (no print_stats.info.current_layer,
    // no virtual_sdcard.layer). These belong to the FILE, not the session: like
    // print_layer_total_ they survive reset_for_new_print() so same-file reprints
    // (whose metadata callback won't re-fire) keep working.
    double layer_height_ = 0.0;       // slice layer height (mm)
    double first_layer_height_ = 0.0; // slice first-layer height (mm)
    // Last commanded Z (mm) from gcode_move.gcode_position, cached each status
    // update so the Z-derivation can run on any update (even one that only
    // carries virtual_sdcard). Reset per-print (motion belongs to the print).
    double last_gcode_z_mm_ = 0.0;
    bool have_gcode_z_ = false;

    // Print time tracking subjects (in seconds)
    lv_subject_t print_duration_{};      // Extrusion-only elapsed time (Moonraker print_duration)
    lv_subject_t print_elapsed_{};       // Wall-clock elapsed time (Moonraker total_duration)
    lv_subject_t print_time_left_{};     // Estimated remaining
    lv_subject_t print_filament_used_{}; // Filament used in mm (from Moonraker print_stats)

    // Per-extruder filament_used (mm) — heap-allocated for stable pointers.
    // Map entries are pre-populated by init_subjects() for indices 0..MAX_EXTRUDER_SCAN-1,
    // freezing the map structure so the WebSocket background thread cannot trigger
    // a rehash while a UI-thread caller is reading. Only the int value inside
    // each subject is mutated during status updates (atomic via lv_subject_set_int).
    // See [L077] for the lifetime-token discipline required when observing
    // these dynamic subjects.
    struct ExtruderFilamentInfo {
        std::unique_ptr<lv_subject_t> subject; ///< int: mm consumed on this extruder
        SubjectLifetime lifetime;              ///< shared_ptr<bool>: true while subject alive
    };
    std::unordered_map<int, ExtruderFilamentInfo> extruder_filament_used_;

    /// Create the per-extruder filament_used entry for idx. Called only from
    /// init_subjects() to pre-populate the map — NEVER from update_from_status
    /// or the accessor, because emplace from the WebSocket background thread
    /// could race with UI-thread reads via rehash invalidation.
    void create_extruder_filament_entry(int extruder_idx);

    // Print start progress subjects
    lv_subject_t print_start_phase_{};    // Integer: PrintStartPhase enum
    lv_subject_t print_lifecycle_{};      // Integer: PrintState enum (derived)
    lv_subject_t print_lifecycle_prev_{}; // Integer: PrintState enum, value before the current
    lv_subject_t job_holds_machine_{};    // Integer 0/1: job_holds_machine(print_lifecycle)

    /// The job we are preparing; empty when none. See begin_preparing().
    PrintJobRef preparing_job_{};
    lv_subject_t preparing_epoch_{}; // Integer: bumped per preparing job, 0 when none
    int preparing_epoch_counter_ = 0;
    PreparingExit last_preparing_exit_ = PreparingExit::Confirmed;
    lv_subject_t print_start_message_{};  // String: phase message
    lv_subject_t print_start_progress_{}; // Integer: 0-100%

    // Print workflow in-progress subject
    lv_subject_t print_in_progress_{};

    /// Backstop for a preparing job the printer never acknowledges.
    ///
    /// `print_in_progress` is published from the preparing job, and
    /// `can_start_new_print()` refuses while it is set - so without a bound, a
    /// job that never confirms would lock out every later print for the rest of
    /// the session. Armed by begin_preparing(), disarmed by retire_preparing().
    lv_timer_t* preparing_watchdog_ = nullptr;

    /// Half an hour. Must sit above the slowest legitimate pre-print: the K2
    /// Plus runs ~1140s with a forced mesh, and PrintStartCollector uses the
    /// same 1800s as its own "definitely stuck" ceiling.
    static constexpr uint32_t PREPARING_WATCHDOG_MS = 1800U * 1000U;

    void arm_preparing_watchdog();
    void cancel_preparing_watchdog();
    static void preparing_watchdog_cb(lv_timer_t* timer);

    // Pre-print duration prediction subjects
    lv_subject_t print_start_time_left_{};
    lv_subject_t preprint_remaining_{}; // int: seconds remaining for pre-print
    lv_subject_t preprint_elapsed_{};   // int: seconds elapsed since pre-print started

    // Slicer estimated total print time (not a subject - no XML binding needed)
    int estimated_print_time_ = 0;

    // Exponential moving average for time remaining estimate.
    // Smooths out wild jumps at low progress where the extrapolation is noisy.
    double smoothed_remaining_ = 0.0;
    bool has_smoothed_remaining_ = false;

    // Layer tracking: true when real layer data received from print_stats.info or gcode fallback.
    // When false, current_layer is estimated from progress * total_layers.
    // Atomic: written from background thread (gcode fallback), read from main thread (UI).
    // NOTE: this is PER-PRINT — reset_for_new_print() clears it to false, and it
    // is transiently false at the start of each print until info.current_layer is
    // re-observed. Do NOT use it to decide whether a printer reports layers at
    // all; use printer_reports_layers_ for that (see below).
    std::atomic<bool> has_real_layer_data_{false};

    // True when the current layer value came from the Z-height derivation (tier
    // 3) rather than the progress-fraction estimate (tier 4). Drives the display
    // "accurate" decision (layer_is_accurate()) WITHOUT satisfying
    // has_real_layer_data_ — Z-derived layers are display-accurate but must never
    // flip the pre-print completion gate. Per-print (cleared by
    // reset_for_new_print). Atomic: written on the status-update path, read from
    // the main thread by the label formatter.
    std::atomic<bool> layer_z_derived_{false};

    // STICKY printer capability: true once ANY real layer field
    // (print_stats.info.current_layer, print_stats.info.total_layer, or
    // virtual_sdcard.layer) has been observed at least once this SESSION. Unlike
    // has_real_layer_data_, this is a printer property, NOT per-print — it is
    // never cleared by reset_for_new_print(). The pre-print → printing hand-off
    // (PrintStartCollector via MoonrakerManager::should_complete_preprint) uses
    // this to choose between the real-first-layer gate and the print_duration
    // fallback. Using the per-print flag there caused premature completion on the
    // Snapmaker U1: reset_for_new_print() clears has_real_layer_data_ AFTER the
    // collector starts, and the U1 doesn't continuously re-emit current_layer
    // during pre-print, so the flag stayed false through the purge and the
    // print_duration fallback fired mid-pre-print. total_layer is present from
    // print start on the U1, so this sticky flag latches immediately and stays
    // true. Atomic: written from the status-update thread, read from main thread.
    std::atomic<bool> printer_reports_layers_{false};

    // virtual_sdcard.is_active — true while Klipper is actively playing back
    // gcode from the SD/file. Distinct from PrintJobState (a print can be
    // paused with is_active=false; see is_sdcard_active() docs). Main-thread
    // only, updated from update_from_status.
    bool sdcard_active_ = false;

    /// True between a terminal print state and the start of the next print,
    /// while print_progress_display_ holds its final value.
    bool progress_frozen_ = false;

    // virtual_sdcard.pl_env_valid — Snapmaker-fork Power-Loss-Recovery flag.
    lv_subject_t pl_env_valid_{}; // Integer: 1 when a validated PLR snapshot exists
    // virtual_sdcard.file_path — companion to pl_env_valid_. Not a subject:
    // no XML binding needed, read on the main thread by the resume dispatcher.
    std::string pl_recovery_file_;

    // print_stats.power_loss — Creality-fork PLR capability marker. Integer:
    // 1 once the key has been seen as a JSON number (presence, not value).
    lv_subject_t creality_plr_capable_{};

    // Slicer progress from display_status (M73 gcode command)
    // When active, preferred over virtual_sdcard file-position progress
    double slicer_progress_ = 0.0;        // Raw 0.0-1.0 from display_status
    bool slicer_progress_active_ = false; // True once non-zero value seen during print

    // Display message from Klipper (M117 gcode / display_status.message)
    lv_subject_t display_message_{};         // String subject for UI binding
    lv_subject_t display_message_visible_{}; // Integer: 1 when non-empty, 0 when empty
    char display_message_buf_[128]{};        // Buffer for message storage

    // print_stats.message from Klipper (set by firmware on pause/error to describe reason)
    lv_subject_t print_message_{};  // String: e.g. "Filament Sensor: Runout Detected"
    char print_message_buf_[256]{}; // Buffer for print_stats.message storage

    // print_stats.exception — structured pause descriptor {id,index,code,message,
    // level} sent by Snapmaker U1 firmware. On these pauses print_stats.message
    // is empty and the reason text lives in exception.message. Plain members
    // (not subjects): no XML binding, read on the main thread by the resume
    // classifier. Default -1/"" means "no exception latched". (#991)
    int print_exception_id_ = -1;
    int print_exception_code_ = -1;
    std::string print_exception_message_;

    // String buffers for subject storage
    char print_filename_buf_[256]{};
    char print_display_filename_buf_[128]{};
    char print_thumbnail_path_buf_[512]{};
    // Identity for print_thumbnail_path_: the gcode filename that path was
    // produced for. Plain member (no XML binding), written before the subject.
    std::string print_thumbnail_file_;
    char print_progress_text_buf_[16]{};
    char print_state_buf_[32]{};
    char print_start_message_buf_[64]{};
    char print_start_time_left_buf_[32]{};
};

} // namespace helix
