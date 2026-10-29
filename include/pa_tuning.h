// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Pressure-advance tuning by eye, during a test print.
//
// HelixScreen prints a small hollow tower whose corners show pressure-advance
// artefacts, and drives K itself: on each layer change it sends
// SET_PRESSURE_ADVANCE for that layer's band, so the file carries no PA
// commands and the user can take over at any moment. What the user finally
// picks from is the K the printer REPORTED for each layer, not the K that was
// planned, because the printer is the only witness to what was in effect.
//
// Everything here is pure: no LVGL, no network. The overlay feeds it events
// and sends the commands it asks for.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::pa {

/// Klipper accepts any non-negative K; this ceiling only stops a runaway
/// stepper from asking for a value no extruder can use.
inline constexpr float kMinK = 0.0f;
inline constexpr float kMaxK = 2.0f;

float clamp_k(float k);

/// K formatted the way tuning guides and slicers quote it: four decimals.
std::string format_k(float k);

/// The command that sets K on one extruder. smooth_time is left alone.
std::string set_pa_gcode(const std::string& extruder, float k);

/// Step sizes offered for a live nudge, smallest first. A bowden K is an
/// order of magnitude above a direct-drive one, so the large steps exist for it.
inline constexpr float kStepSizes[] = {0.001f, 0.005f, 0.01f, 0.05f};
inline constexpr int kStepCount = 4;
inline constexpr int kDefaultStepIndex = 1;

enum class ExtruderKind : int { DirectDrive = 0, Bowden = 1 };

/// The planned K sweep: band 0 prints at start_k, the last band at end_k.
struct Ramp {
    float start_k = 0.0f;
    float end_k = 0.1f;
    int bands = 10;
};

/// The sweep every tuning guide starts from for this extruder kind.
Ramp default_ramp(ExtruderKind kind);

/// The tower. A hollow square: corners are where pressure advance shows, and a
/// thin wall keeps every layer short enough that a band is a few minutes.
struct TowerGeometry {
    float first_layer_mm = 0.25f;
    float layer_mm = 0.2f;
    /// Layers below the first band, at start_k, so the first band does not sit
    /// on the squashed first layer.
    float base_mm = 1.0f;
    float band_mm = 3.0f;
    float size_mm = 40.0f;
    int perimeters = 2;
};

/// Total layers for a tower with `bands` bands.
int layer_count(const TowerGeometry& g, int bands);

/// Top-of-layer Z for 1-based `layer`.
float layer_z(const TowerGeometry& g, int layer);

/// The band 1-based `layer` belongs to: -1 for the base, else 0..bands-1.
int band_for_layer(const TowerGeometry& g, int bands, int layer);

/// The ramp's planned K for 1-based `layer`.
float ramp_k_for_layer(const TowerGeometry& g, const Ramp& r, int layer);

/// How the test print starts and ends on this printer. Its own start macro,
/// when it has one, homes, levels and primes the way every sliced file does;
/// without one the file homes and heats by itself.
struct MachineMacros {
    std::string start;              ///< empty: plain G28 and heat
    std::string start_bed_param;    ///< empty when the macro takes none we recognise
    std::string start_nozzle_param; ///< likewise
    std::string end;                ///< empty: plain cooldown
};

/// The parameters a macro takes (upper case), or nullopt when the printer has
/// no macro of that name.
using MacroParamsLookup =
    std::function<std::optional<std::vector<std::string>>(const std::string& name)>;

/// The first start and end macros the printer defines, in candidate order, and
/// the start macro's bed and nozzle temperature parameters.
MachineMacros pick_machine_macros(const std::vector<std::string>& start_candidates,
                                  const std::vector<std::string>& end_candidates,
                                  const MacroParamsLookup& params_of);

/// The start macro call with the temperatures it accepts, e.g.
/// "PRINT_START BED_TEMP=60 EXTRUDER_TEMP=210". Empty without a start macro.
std::string start_macro_call(const MachineMacros& m, int bed_temp, int nozzle_temp);

/// Where on the bed the tower and its prime line may go, gcode space.
struct PrintArea {
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
};

/// What the printer says about its printable area.
struct AreaSources {
    /// toolhead axis_minimum/maximum, gcode space. Required.
    bool has_axes = false;
    float axis_x_min = 0.0f, axis_x_max = 0.0f, axis_y_min = 0.0f, axis_y_max = 0.0f;
    /// [bed_mesh] mesh_min/mesh_max, gcode space: the plate itself, where a
    /// negative position_min reaches a purge bucket or brush beyond it.
    bool has_plate = false;
    float plate_x_min = 0.0f, plate_x_max = 0.0f, plate_y_min = 0.0f, plate_y_max = 0.0f;
    /// A round bed (delta): the axes are the circumscribed square.
    bool round_bed = false;
};

/// The rectangle the test print may use: the plate within the axes; on a round
/// bed the square inscribed in the axes' circle. nullopt without axes.
std::optional<PrintArea> printable_area(const AreaSources& src);

/// Everything the gcode generator needs. Coordinates are gcode space.
struct TestPrintParams {
    TowerGeometry geometry;
    int bands = 10;
    float bed_x_min = 0.0f;
    float bed_x_max = 220.0f;
    float bed_y_min = 0.0f;
    float bed_y_max = 220.0f;
    int nozzle_temp = 210;
    int bed_temp = 60;
    /// Tool to select before printing; -1 prints with whatever is mounted.
    /// Only a printer with several physical extruders gets one: an AMS lane
    /// is not a tool to mount here.
    int tool = -1;
    /// Klipper heater of the extruder the tower prints with, and the one K is
    /// tuned on.
    std::string heater = "extruder";
    /// K to put back when the tower ends, so a crash, an abandoned run or a
    /// reprint from history never leaves the last band's K behind. nullopt
    /// when the original was never reported: nothing to restore.
    std::optional<float> restore_k;
    float nozzle_diameter_mm = 0.4f;
    float filament_diameter_mm = 1.75f;
    /// Wall speed. Fast enough that the corners show the artefact.
    float wall_speed_mm_s = 100.0f;
    float first_layer_speed_mm_s = 25.0f;
    /// Part-cooling fan from the third layer on, 0-100.
    int fan_percent = 100;
    MachineMacros macros;
    std::string material;
};

/// Generate the test print: starts through the printer's start macro (or homes
/// and heats itself), primes, prints, and ends through the end macro (or cools
/// down itself); reports layers with SET_PRINT_STATS_INFO; never sets K.
std::string generate_test_gcode(const TestPrintParams& p);

/// Rough wall-clock for the print itself, heat-up excluded.
int estimate_print_seconds(const TestPrintParams& p);

/// One height range the printer reported a single K for.
struct Band {
    int first_layer = 0;
    int last_layer = 0;
    float z_low = 0.0f;  ///< bottom of first_layer
    float z_high = 0.0f; ///< top of last_layer
    float k = 0.0f;
};

/// K in effect per layer, as reported by the printer.
class BandLog {
  public:
    /// Record that `k` was in effect while 1-based `layer` (top at `z_top`,
    /// bottom at `z_bottom`) printed. The latest report for a layer wins.
    void record(int layer, float z_bottom, float z_top, float k);
    void clear();
    bool empty() const {
        return layers_.empty();
    }
    /// Consecutive layers with the same K merged, lowest first.
    std::vector<Band> bands() const;
    /// K reported for the highest recorded layer.
    std::optional<float> last_k() const;

  private:
    struct Entry {
        int layer;
        float z_bottom;
        float z_top;
        float k;
    };
    std::vector<Entry> layers_;
};

/// @p bands reduced to at most @p max rows by repeatedly merging the
/// neighbouring pair whose K differ least; the merged row keeps the K of the
/// taller part. Late nudges stay pickable, where truncating would drop them.
std::vector<Band> fit_bands(std::vector<Band> bands, size_t max);

/// The tuning run, as a state machine. The caller owns the printer; this owns
/// what K should be and what was observed.
class TuningSession {
  public:
    enum class State : int {
        Idle = 0,     ///< nothing running
        Starting = 1, ///< file uploaded / print requested, not printing yet
        Printing = 2, ///< the test print is running
        Picking = 3,  ///< print over; the user chooses a band
        Done = 4,     ///< a result exists
    };
    enum class Mode : int {
        Ramp = 0,   ///< K follows the ramp on every layer change
        Manual = 1, ///< the user nudged; K stays where they put it
    };

    /// Asks the caller to set K on the printer.
    using SendK = std::function<void(float)>;

    explicit TuningSession(SendK send) : send_(std::move(send)) {}

    /// @p original_k is nullopt when the printer never reported one: the run
    /// then has nothing to restore.
    void start(const TowerGeometry& g, const Ramp& r, std::optional<float> original_k);
    /// The print reached "printing".
    void on_print_started();
    /// 1-based layer now printing. 0 means not started yet; a layer past the
    /// tower's last is the end gcode, and records nothing.
    void on_layer(int layer);
    /// The printer reported K for the extruder being tuned. On the top layer
    /// the original K coming back unasked is the end gcode's restore, which can
    /// arrive in the same status frame as the end marker and ahead of it: it
    /// ends logging instead of overwriting the top band.
    void on_k_reported(float k);
    /// The print ended, by any route. With nothing observed the run is void.
    void on_print_ended();
    /// Nudge K by `delta`. Stops the ramp for the rest of the print. False
    /// (nothing sent) outside the print or once the tower is done.
    bool nudge(float delta);
    /// Pick one of bands(max). Returns false for an index out of range.
    bool pick_band(int index, size_t max = SIZE_MAX);
    /// Keep the K the tower ended on (the end gcode restores the original, so
    /// it is sent again).
    void keep_current();
    /// The K keep_current() would apply.
    float ending_k() const;
    /// Abandon the run. Asks for the original K back when the run changed it
    /// and produced no result.
    void abandon();

    State state() const {
        return state_;
    }
    Mode mode() const {
        return mode_;
    }
    int layer() const {
        return layer_;
    }
    /// Last K the printer reported, or the original before any report.
    float current_k() const {
        return current_k_;
    }
    std::optional<float> original_k() const {
        return original_k_;
    }
    std::optional<float> result() const {
        return result_;
    }
    const TowerGeometry& geometry() const {
        return geometry_;
    }
    const Ramp& ramp() const {
        return ramp_;
    }
    std::vector<Band> bands(size_t max = SIZE_MAX) const {
        return fit_bands(log_.bands(), max);
    }

  private:
    void send(float k);

    SendK send_;
    State state_ = State::Idle;
    Mode mode_ = Mode::Ramp;
    TowerGeometry geometry_;
    Ramp ramp_;
    BandLog log_;
    int layer_ = 0;
    float current_k_ = 0.0f;
    std::optional<float> original_k_;
    std::optional<float> last_sent_;
    bool changed_k_ = false;
    /// The end gcode has started: nothing more is a band, and nudges are moot.
    bool tower_done_ = false;
    std::optional<float> result_;
};

/// Where a tuned K is published in Moonraker's database, for other clients
/// (a slicer plugin) to read. Schema: docs/devel/PRESSURE_ADVANCE_TUNING.md.
inline constexpr const char* kMoonrakerNamespace = "helix-screen";
inline constexpr const char* kMoonrakerKey = "pressure_advance";

inline constexpr const char* kSourceTuned = "tuned";

/// The `source` of a K the printer measured with `provider_id`'s procedure.
std::string measured_source(const std::string& provider_id);

/// One result, as published.
struct SavedK {
    float k = 0.0f;
    std::string extruder;
    std::string material;
    int nozzle_temp = 0;
    /// Spoolman ids, 0 when no spool was active.
    int spool_id = 0;
    int filament_id = 0;
    std::string filament_name;
    /// ISO 8601 UTC.
    std::string updated;
    /// How the value was found: kSourceTuned for a tower the user judged,
    /// measured_source(provider id) for a printer that measured it.
    std::string source = kSourceTuned;
};

/// `existing` (the key's current value, or null) with `saved` recorded as the
/// latest result, as the extruder's latest, and as its filament's when it has
/// one. Other entries are kept.
nlohmann::json merge_moonraker_record(const nlohmann::json& existing, const SavedK& saved);

/// A spool mounted on a specific extruder (a tool changer's per-tool assignment).
struct SpoolOnExtruder {
    int spool_id = 0;
    std::string extruder;
};

/// The extruder a newly active spool feeds: the one whose tool holds it, else
/// the active extruder (a single hotend, or lanes into one).
std::string extruder_for_spool(int spool_id, const std::vector<SpoolOnExtruder>& mounted,
                               const std::string& active_extruder);

/// Whether a spool activation should apply its saved K. Never mid-print: a
/// slicer that sets PA in its start gcode has already spoken, and an AMS swap
/// during a print must not override it.
bool should_auto_apply_on_activation(bool printing, std::optional<float> saved_k);

} // namespace helix::pa
