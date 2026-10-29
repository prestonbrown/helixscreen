// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_tuning.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace helix::pa {

namespace {

/// Two reported K values closer than this are the same setting: Klipper
/// reports what it was sent, and the overlay never sends finer than 0.0001.
constexpr float kKEpsilon = 0.00005f;

bool same_k(float a, float b) {
    return std::fabs(a - b) < kKEpsilon;
}

/// Base layers above the first layer, before band 0.
int base_layers(const TowerGeometry& g) {
    return std::max(0, static_cast<int>(std::lround(g.base_mm / g.layer_mm)));
}

int layers_per_band(const TowerGeometry& g) {
    return std::max(1, static_cast<int>(std::lround(g.band_mm / g.layer_mm)));
}

/// Extrusion length per mm of travel for a line of width w and height h.
double e_per_mm(double w, double h, double filament_diameter) {
    const double r = filament_diameter / 2.0;
    return (w * h) / (M_PI * r * r);
}

/// Line width every loop prints at: the usual slicer default for a nozzle.
double line_width(const TestPrintParams& p) {
    return p.nozzle_diameter_mm * 1.125;
}

/// Path length of one square loop with half-width `half`.
double loop_length(double half) {
    return 8.0 * half;
}

double layer_height_of(const TowerGeometry& g, int layer) {
    return layer <= 1 ? g.first_layer_mm : g.layer_mm;
}

/// Brim loops outside the tower on the first layer, for adhesion.
constexpr int kBrimLoops = 4;

/// Total extrusion path for one layer, and the loops that make it.
struct LayerPlan {
    std::vector<double> halves; ///< loop half-widths, printed in this order
};

LayerPlan plan_layer(const TestPrintParams& p, int layer) {
    const double w = line_width(p);
    const double outer = p.geometry.size_mm / 2.0 - w / 2.0;
    LayerPlan plan;
    // Inner loops first, so the outer wall is laid against the inner one and
    // its corners are the ones the user judges.
    for (int i = p.geometry.perimeters - 1; i >= 0; --i) {
        plan.halves.push_back(outer - i * w);
    }
    if (layer == 1) {
        for (int i = 1; i <= kBrimLoops; ++i) {
            plan.halves.push_back(outer + i * w);
        }
    }
    return plan;
}

} // namespace

float clamp_k(float k) {
    if (!std::isfinite(k)) {
        return kMinK;
    }
    return std::clamp(k, kMinK, kMaxK);
}

std::string format_k(float k) {
    return fmt::format("{:.4f}", k);
}

std::string set_pa_gcode(const std::string& extruder, float k) {
    return fmt::format("SET_PRESSURE_ADVANCE EXTRUDER={} ADVANCE={}", extruder,
                       format_k(clamp_k(k)));
}

Ramp default_ramp(ExtruderKind kind) {
    // The sweeps Klipper's own tuning guide implies for its tower: about 0.1 of
    // K across a direct-drive tower, ten times that on a bowden one.
    if (kind == ExtruderKind::Bowden) {
        return Ramp{0.0f, 1.0f, 10};
    }
    return Ramp{0.0f, 0.1f, 10};
}

int layer_count(const TowerGeometry& g, int bands) {
    return 1 + base_layers(g) + std::max(1, bands) * layers_per_band(g);
}

float layer_z(const TowerGeometry& g, int layer) {
    if (layer <= 1) {
        return g.first_layer_mm;
    }
    return g.first_layer_mm + static_cast<float>(layer - 1) * g.layer_mm;
}

int band_for_layer(const TowerGeometry& g, int bands, int layer) {
    const int first_band_layer = 2 + base_layers(g);
    if (layer < first_band_layer) {
        return -1;
    }
    const int band = (layer - first_band_layer) / layers_per_band(g);
    return std::min(band, std::max(1, bands) - 1);
}

float ramp_k_for_layer(const TowerGeometry& g, const Ramp& r, int layer) {
    const int band = band_for_layer(g, r.bands, layer);
    if (band <= 0 || r.bands <= 1) {
        return clamp_k(r.start_k);
    }
    const float step = (r.end_k - r.start_k) / static_cast<float>(r.bands - 1);
    return clamp_k(r.start_k + step * static_cast<float>(band));
}

namespace {

/// Spellings slicers' start gcode passes temperatures to a start macro under,
/// most explicit first.
constexpr const char* kBedParams[] = {"BED_TEMP", "BED", "BED_TEMPERATURE", "BEDTEMP"};
constexpr const char* kNozzleParams[] = {
    "EXTRUDER_TEMP", "EXTRUDER", "HOTEND_TEMP",         "HOTEND",
    "NOZZLE_TEMP",   "NOZZLE",   "EXTRUDER_TEMPERATURE"};

template <size_t N>
std::string first_known(const std::vector<std::string>& params, const char* const (&names)[N]) {
    for (const char* name : names) {
        if (std::find(params.begin(), params.end(), name) != params.end()) {
            return name;
        }
    }
    return {};
}

} // namespace

MachineMacros pick_machine_macros(const std::vector<std::string>& start_candidates,
                                  const std::vector<std::string>& end_candidates,
                                  const MacroParamsLookup& params_of) {
    MachineMacros m;
    if (!params_of) {
        return m;
    }
    for (const auto& name : start_candidates) {
        if (auto params = params_of(name)) {
            m.start = name;
            m.start_bed_param = first_known(*params, kBedParams);
            m.start_nozzle_param = first_known(*params, kNozzleParams);
            break;
        }
    }
    for (const auto& name : end_candidates) {
        if (params_of(name)) {
            m.end = name;
            break;
        }
    }
    return m;
}

std::string start_macro_call(const MachineMacros& m, int bed_temp, int nozzle_temp) {
    if (m.start.empty()) {
        return {};
    }
    std::string call = m.start;
    if (!m.start_bed_param.empty()) {
        call += fmt::format(" {}={}", m.start_bed_param, bed_temp);
    }
    if (!m.start_nozzle_param.empty()) {
        call += fmt::format(" {}={}", m.start_nozzle_param, nozzle_temp);
    }
    return call;
}

std::optional<PrintArea> printable_area(const AreaSources& src) {
    if (!src.has_axes) {
        return std::nullopt;
    }
    PrintArea a{src.axis_x_min, src.axis_x_max, src.axis_y_min, src.axis_y_max};
    if (src.round_bed) {
        // The axes bound the reachable circle; the square inscribed in it is
        // reachable everywhere, corners included.
        const float cx = (a.x_min + a.x_max) / 2.0f;
        const float cy = (a.y_min + a.y_max) / 2.0f;
        const float r = std::min(a.x_max - a.x_min, a.y_max - a.y_min) / 2.0f;
        const float half = r / std::sqrt(2.0f);
        return PrintArea{cx - half, cx + half, cy - half, cy + half};
    }
    if (src.has_plate) {
        a.x_min = std::max(a.x_min, src.plate_x_min);
        a.x_max = std::min(a.x_max, src.plate_x_max);
        a.y_min = std::max(a.y_min, src.plate_y_min);
        a.y_max = std::min(a.y_max, src.plate_y_max);
    }
    if (a.x_max <= a.x_min || a.y_max <= a.y_min) {
        return std::nullopt;
    }
    return a;
}

int estimate_print_seconds(const TestPrintParams& p) {
    const int layers = layer_count(p.geometry, p.bands);
    double seconds = 0.0;
    for (int layer = 1; layer <= layers; ++layer) {
        const double speed = layer == 1 ? p.first_layer_speed_mm_s : p.wall_speed_mm_s;
        double length = 0.0;
        for (double half : plan_layer(p, layer).halves) {
            length += loop_length(half);
        }
        // A layer change, a travel and the corner decelerations are about a
        // second on top of the extrusion itself.
        seconds += length / std::max(1.0, speed) + 1.0;
    }
    return static_cast<int>(std::lround(seconds));
}

std::string generate_test_gcode(const TestPrintParams& p) {
    const TowerGeometry& g = p.geometry;
    const int layers = layer_count(g, p.bands);
    const double w = line_width(p);
    const double cx = (p.bed_x_min + p.bed_x_max) / 2.0;
    const double cy = (p.bed_y_min + p.bed_y_max) / 2.0;
    // Filament total, for the header: the metadata parsers read it.
    double filament_mm = 0.0;
    for (int layer = 1; layer <= layers; ++layer) {
        const double epm = e_per_mm(w, layer_height_of(g, layer), p.filament_diameter_mm);
        for (double half : plan_layer(p, layer).halves) {
            filament_mm += loop_length(half) * epm;
        }
    }

    const int est = estimate_print_seconds(p);
    std::string out;
    out.reserve(static_cast<size_t>(layers) * 400 + 2048);
    auto line = [&out](const std::string& s) {
        out += s;
        out += '\n';
    };

    line("; generated by HelixScreen pressure advance tuner");
    line("; Pressure advance is set live by HelixScreen on each layer change.");
    line(fmt::format("; estimated printing time (normal mode) = {}m {}s", est / 60, est % 60));
    line(fmt::format("; total layer number: {}", layers));
    line(fmt::format("; layer_height = {:.2f}", g.layer_mm));
    line(fmt::format("; first_layer_height = {:.2f}", g.first_layer_mm));
    line(fmt::format("; max_z_height = {:.2f}", layer_z(g, layers)));
    line(fmt::format("; first_layer_temperature = {}", p.nozzle_temp));
    line(fmt::format("; first_layer_bed_temperature = {}", p.bed_temp));
    line(fmt::format("; filament used [mm] = {:.1f}", filament_mm));
    if (!p.material.empty()) {
        line(fmt::format("; filament_type = {}", p.material));
    }
    const std::string start_call = start_macro_call(p.macros, p.bed_temp, p.nozzle_temp);
    const bool macro_heats =
        !p.macros.start_bed_param.empty() && !p.macros.start_nozzle_param.empty();
    // The tower's extruder is heated by name: a T parameter on M104/M109 is
    // an error where tools are lanes, and on a changer it targets a tool, not
    // a heater.
    const std::string heat_nozzle =
        fmt::format("SET_HEATER_TEMPERATURE HEATER={} TARGET={}", p.heater, p.nozzle_temp);
    line("G90");
    if (start_call.empty() || !macro_heats) {
        // Nothing will heat the machine for us, or the macro takes no
        // temperatures: start heating now. A macro that does take them is left
        // to its own order, which often probes with a cool nozzle.
        line(fmt::format("M140 S{}", p.bed_temp));
        line(heat_nozzle);
    }
    if (start_call.empty()) {
        line("G28");
    } else {
        line(start_call);
        line("G90");
    }
    line("M83");
    line(fmt::format("M190 S{}", p.bed_temp));
    line(heat_nozzle);
    // Hot before any tool change: a toolchange purges or wipes.
    line(fmt::format("TEMPERATURE_WAIT SENSOR={} MINIMUM={}", p.heater, p.nozzle_temp - 2));
    if (p.tool >= 0) {
        line(fmt::format("T{}", p.tool)); // DISPLAY_NUMBERING_OK: gcode tool change, not a label
        line(fmt::format("TEMPERATURE_WAIT SENSOR={} MINIMUM={}", p.heater, p.nozzle_temp - 2));
    }
    line("M107");
    line(fmt::format("SET_PRINT_STATS_INFO TOTAL_LAYER={}", layers));

    // Prime line along the front edge, clear of the tower and its brim.
    const double prime_x0 = std::max<double>(p.bed_x_min + 10.0, cx - 40.0);
    const double prime_y = p.bed_y_min + 5.0;
    const double prime_len = 60.0;
    const double prime_epm = e_per_mm(w * 1.5, g.first_layer_mm, p.filament_diameter_mm);
    const double travel_f = 150.0 * 60.0;
    line("G1 Z5 F600");
    line(fmt::format("G1 X{:.3f} Y{:.3f} F{:.0f}", prime_x0, prime_y, travel_f));
    line(fmt::format("G1 Z{:.3f} F600", g.first_layer_mm));
    line(fmt::format("G1 X{:.3f} E{:.5f} F1200", prime_x0 + prime_len, prime_len * prime_epm));
    line("G1 E-0.8 F2100");
    line(fmt::format("G1 Z{:.3f} F600", g.first_layer_mm + 0.4));

    for (int layer = 1; layer <= layers; ++layer) {
        const double h = layer_height_of(g, layer);
        const double z = layer_z(g, layer);
        const double speed = layer == 1 ? p.first_layer_speed_mm_s : p.wall_speed_mm_s;
        const double epm = e_per_mm(w, h, p.filament_diameter_mm);
        const LayerPlan plan = plan_layer(p, layer);

        line(fmt::format("SET_PRINT_STATS_INFO CURRENT_LAYER={}", layer));
        line(fmt::format(";LAYER:{} Z:{:.3f}", layer, z));
        if (layer == 3 && p.fan_percent > 0) {
            line(fmt::format("M106 S{}", std::clamp(p.fan_percent, 0, 100) * 255 / 100));
        }
        const double first_half = plan.halves.front();
        line(fmt::format("G1 X{:.3f} Y{:.3f} F{:.0f}", cx - first_half, cy - first_half, travel_f));
        line(fmt::format("G1 Z{:.3f} F600", z));
        if (layer == 1) {
            line("G1 E0.8 F2100");
        }
        bool first_loop = true;
        for (double half : plan.halves) {
            const double x0 = cx - half;
            const double y0 = cy - half;
            const double x1 = cx + half;
            const double y1 = cy + half;
            const double side = 2.0 * half;
            const double e = side * epm;
            if (!first_loop) {
                // Step to the next loop's corner; adjacent loops, so no retract.
                line(fmt::format("G1 X{:.3f} Y{:.3f} F{:.0f}", x0, y0, travel_f));
            }
            first_loop = false;
            const double f = speed * 60.0;
            line(fmt::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}", x1, y0, e, f));
            line(fmt::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}", x1, y1, e, f));
            line(fmt::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}", x0, y1, e, f));
            line(fmt::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}", x0, y0, e, f));
        }
    }

    line("G1 E-1 F2100");
    // A layer past the last tells the tuner the tower is done, so the restore
    // below is not logged against the top band.
    line(fmt::format("SET_PRINT_STATS_INFO CURRENT_LAYER={}", layers + 1));
    if (p.restore_k) {
        line(set_pa_gcode(p.heater, *p.restore_k));
    }
    if (!p.macros.end.empty()) {
        line(p.macros.end);
    } else {
        line("G91");
        line("G1 Z10 F600");
        line("G90");
    }
    // Cool down even after an end macro: not every one turns everything off.
    line(fmt::format("SET_HEATER_TEMPERATURE HEATER={} TARGET=0", p.heater));
    line("M140 S0");
    line("M107");
    line("M84");
    return out;
}

// ============================================================================
// BandLog
// ============================================================================

void BandLog::record(int layer, float z_bottom, float z_top, float k) {
    if (layer <= 0) {
        return;
    }
    auto it = std::lower_bound(layers_.begin(), layers_.end(), layer,
                               [](const Entry& e, int l) { return e.layer < l; });
    if (it != layers_.end() && it->layer == layer) {
        it->k = k;
        return;
    }
    layers_.insert(it, Entry{layer, z_bottom, z_top, k});
}

void BandLog::clear() {
    layers_.clear();
}

std::vector<Band> BandLog::bands() const {
    std::vector<Band> out;
    for (const Entry& e : layers_) {
        if (!out.empty() && same_k(out.back().k, e.k)) {
            out.back().last_layer = e.layer;
            out.back().z_high = e.z_top;
            continue;
        }
        out.push_back(Band{e.layer, e.layer, e.z_bottom, e.z_top, e.k});
    }
    return out;
}

std::optional<float> BandLog::last_k() const {
    if (layers_.empty()) {
        return std::nullopt;
    }
    return layers_.back().k;
}

std::vector<Band> fit_bands(std::vector<Band> bands, size_t max) {
    if (max == 0) {
        return {};
    }
    while (bands.size() > max) {
        size_t best = 0;
        float best_diff = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < bands.size(); ++i) {
            const float diff = std::fabs(bands[i].k - bands[i + 1].k);
            if (diff < best_diff) {
                best_diff = diff;
                best = i;
            }
        }
        Band& lo = bands[best];
        const Band& hi = bands[best + 1];
        const int lo_layers = lo.last_layer - lo.first_layer + 1;
        const int hi_layers = hi.last_layer - hi.first_layer + 1;
        if (hi_layers > lo_layers) {
            lo.k = hi.k;
        }
        lo.last_layer = hi.last_layer;
        lo.z_high = hi.z_high;
        bands.erase(bands.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    return bands;
}

// ============================================================================
// TuningSession
// ============================================================================

void TuningSession::start(const TowerGeometry& g, const Ramp& r, std::optional<float> original_k) {
    geometry_ = g;
    ramp_ = r;
    log_.clear();
    state_ = State::Starting;
    mode_ = Mode::Ramp;
    layer_ = 0;
    current_k_ = original_k.value_or(0.0f);
    original_k_ = original_k;
    last_sent_.reset();
    changed_k_ = false;
    tower_done_ = false;
    result_.reset();
}

void TuningSession::send(float k) {
    const float clamped = clamp_k(k);
    last_sent_ = clamped;
    changed_k_ = true;
    if (send_) {
        send_(clamped);
    }
}

void TuningSession::on_print_started() {
    if (state_ != State::Starting) {
        return;
    }
    state_ = State::Printing;
    if (mode_ == Mode::Ramp) {
        send(ramp_k_for_layer(geometry_, ramp_, std::max(1, layer_)));
    }
}

void TuningSession::on_layer(int layer) {
    if (state_ == State::Starting && layer > 0) {
        on_print_started();
    }
    if (state_ != State::Printing || layer <= 0 || layer == layer_) {
        return;
    }
    if (layer > layer_count(geometry_, ramp_.bands)) {
        // The end gcode: the tower is done, and what it sets is not a band.
        tower_done_ = true;
        return;
    }
    layer_ = layer;
    if (mode_ == Mode::Ramp) {
        const float want = ramp_k_for_layer(geometry_, ramp_, layer);
        if (!last_sent_ || !same_k(*last_sent_, want)) {
            send(want);
        }
    }
    const float bottom = layer <= 1 ? 0.0f : layer_z(geometry_, layer - 1);
    log_.record(layer, bottom, layer_z(geometry_, layer), current_k_);
}

void TuningSession::on_k_reported(float k) {
    if (tower_done_) {
        return;
    }
    const bool on_top_layer = layer_ >= layer_count(geometry_, ramp_.bands);
    const bool asked_for = last_sent_ && same_k(*last_sent_, k);
    if (state_ == State::Printing && on_top_layer && original_k_ && same_k(*original_k_, k) &&
        !asked_for) {
        tower_done_ = true;
        return;
    }
    current_k_ = k;
    if (state_ == State::Printing && layer_ > 0) {
        const float bottom = layer_ <= 1 ? 0.0f : layer_z(geometry_, layer_ - 1);
        log_.record(layer_, bottom, layer_z(geometry_, layer_), k);
    }
}

void TuningSession::on_print_ended() {
    if (state_ == State::Printing && !log_.empty()) {
        state_ = State::Picking;
        return;
    }
    if (state_ == State::Starting || state_ == State::Printing) {
        abandon();
    }
}

bool TuningSession::nudge(float delta) {
    if (state_ != State::Printing || tower_done_) {
        return false;
    }
    // The base is what was last asked for, so taps faster than the printer's
    // report accumulate instead of all starting from the same stale value.
    const float base = last_sent_.value_or(current_k_);
    mode_ = Mode::Manual;
    send(base + delta);
    return true;
}

bool TuningSession::pick_band(int index, size_t max) {
    if (state_ != State::Picking) {
        return false;
    }
    const auto all = bands(max);
    if (index < 0 || index >= static_cast<int>(all.size())) {
        return false;
    }
    result_ = all[static_cast<size_t>(index)].k;
    send(*result_);
    state_ = State::Done;
    return true;
}

float TuningSession::ending_k() const {
    return log_.last_k().value_or(current_k_);
}

void TuningSession::keep_current() {
    if (state_ != State::Picking) {
        return;
    }
    result_ = ending_k();
    send(*result_);
    state_ = State::Done;
}

void TuningSession::abandon() {
    if (changed_k_ && !result_ && state_ != State::Done && original_k_) {
        send(*original_k_);
    }
    state_ = State::Idle;
}

std::string measured_source(const std::string& provider_id) {
    return "measured:" + provider_id;
}

nlohmann::json merge_moonraker_record(const nlohmann::json& existing, const SavedK& saved) {
    nlohmann::json out = existing.is_object() ? existing : nlohmann::json::object();
    nlohmann::json entry = {{"k", std::round(saved.k * 10000.0f) / 10000.0},
                            {"extruder", saved.extruder},
                            {"material", saved.material},
                            {"nozzle_temp", saved.nozzle_temp},
                            {"spool_id", saved.spool_id},
                            {"filament_id", saved.filament_id},
                            {"filament_name", saved.filament_name},
                            {"updated", saved.updated},
                            {"source", saved.source}};
    out["version"] = 1;
    out["last"] = entry;
    if (!out.contains("extruders") || !out["extruders"].is_object()) {
        out["extruders"] = nlohmann::json::object();
    }
    if (!saved.extruder.empty()) {
        out["extruders"][saved.extruder] = entry;
    }
    if (!out.contains("filaments") || !out["filaments"].is_object()) {
        out["filaments"] = nlohmann::json::object();
    }
    if (saved.filament_id > 0) {
        out["filaments"][std::to_string(saved.filament_id)] = entry;
    }
    return out;
}

std::string extruder_for_spool(int spool_id, const std::vector<SpoolOnExtruder>& mounted,
                               const std::string& active_extruder) {
    for (const auto& m : mounted) {
        if (spool_id > 0 && m.spool_id == spool_id && !m.extruder.empty()) {
            return m.extruder;
        }
    }
    return active_extruder;
}

bool should_auto_apply_on_activation(bool printing, std::optional<float> saved_k) {
    return !printing && saved_k.has_value();
}

} // namespace helix::pa
