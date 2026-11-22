// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_live_card.h"

#include "ui_bed_mesh.h"
#include "ui_frequency_response_chart.h"

#include "app_globals.h"
#include "bed_mesh_state.h"
#include "bed_mesh_stats.h"
#include "calibration_run.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "i_moonraker_api.h"
#include "printer_discovery.h"
#include "temp_graph_controller.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace helix::ui {

namespace {

using calibration::StepKind;
using calibration::StepLive;

constexpr const char* kBedHeater = "heater_bed";
/// The heater graph's window: a heater calibration runs a few minutes.
constexpr int kHeatGraphPoints = 120;
/// Points of the flat plane drawn under the probe when the printer has no mesh yet.
constexpr int kFlatMeshPoints = 5;

lv_obj_t* find(lv_obj_t* root, const char* name) {
    return root ? lv_obj_find_by_name(root, name) : nullptr;
}

} // namespace

LiveCardText live_card_text(StepKind kind, const StepLive& live) {
    LiveCardText t;
    switch (kind) {
    case StepKind::Heaters: {
        t.kind = LiveKind::Heat;
        const bool bed = live.heater == kBedHeater;
        if (live.mpc) {
            t.what = bed ? lv_tr("MPC: measuring how fast the bed heats and loses heat")
                         : lv_tr("MPC: measuring how fast the nozzle heats and loses heat");
        } else {
            t.what = bed ? lv_tr("PID: heating the bed in cycles to tune its control")
                         : lv_tr("PID: heating the nozzle in cycles to tune its control");
        }
        t.badge = lv_tr("HOT");
        t.badge_tone = 2;
        t.warn = lv_tr("Do not touch the nozzle or bed");
        t.hot = true;
        break;
    }
    case StepKind::BedMesh:
        t.kind = LiveKind::Mesh;
        t.what = lv_tr("Probing the bed point by point");
        t.badge = lv_tr("HOT");
        t.badge_tone = 2;
        t.warn = lv_tr("The bed is hot");
        t.hot = true;
        break;
    case StepKind::InputShaper:
        t.kind = LiveKind::Shaper;
        t.what = lv_tr("Shaking each axis through a frequency sweep");
        t.badge = lv_tr("LOUD");
        t.badge_tone = 3;
        t.warn = lv_tr("It is loud. This is normal.");
        break;
    case StepKind::BeltTension:
        t.kind = LiveKind::Belt;
        t.what = lv_tr("The tensioners pull each belt to its target");
        t.badge = lv_tr("MOTORS");
        t.badge_tone = 1;
        t.warn = lv_tr("Keep hands clear");
        break;
    default:
        break;
    }
    return t;
}

LiveCardText live_card_done_text() {
    LiveCardText t;
    t.kind = LiveKind::Done;
    t.what = lv_tr("Review the results, then save once.");
    t.badge = lv_tr("DONE");
    t.badge_tone = 4;
    return t;
}

int belt_fill_percent(float pull_fraction) {
    if (!std::isfinite(pull_fraction) || pull_fraction <= 0.0f) {
        return 0;
    }
    return static_cast<int>(std::lround(std::min(1.0f, pull_fraction) * kBeltTargetPercent));
}

int mesh_unprobed_percent(int probed, int total) {
    if (total <= 0) {
        return 0;
    }
    return std::clamp(100 - probed * 100 / total, 0, 100);
}

CalibrationLiveCard& get_calibration_live_card() {
    return helix::lazy_global_with_teardown<CalibrationLiveCard>("CalibrationLiveCard", []() {
        if (auto* self = helix::lazy_global_if_exists<CalibrationLiveCard>()) {
            self->detach();
        }
    });
}

CalibrationLiveCard::~CalibrationLiveCard() {
    stop_heat();
    shaper_chart_.destroy();
}

void CalibrationLiveCard::init_subjects() {
    UI_MANAGED_SUBJECT_INT(kind_, 0, "cal_live_kind", subjects_);
    UI_MANAGED_SUBJECT_STRING(what_, what_buf_, "", "cal_live_what", subjects_);
    UI_MANAGED_SUBJECT_STRING(badge_, badge_buf_, "", "cal_live_badge", subjects_);
    UI_MANAGED_SUBJECT_INT(badge_tone_, 0, "cal_live_badge_tone", subjects_);
    UI_MANAGED_SUBJECT_STRING(warn_, warn_buf_, "", "cal_live_warn", subjects_);
    UI_MANAGED_SUBJECT_INT(hot_, 0, "cal_live_hot", subjects_);
    UI_MANAGED_SUBJECT_INT(heat_bed_, 0, "cal_live_heat_bed", subjects_);
    UI_MANAGED_SUBJECT_INT(mesh_unprobed_, 100, "cal_live_mesh_unprobed", subjects_);
    UI_MANAGED_SUBJECT_STRING(mesh_count_, mesh_count_buf_, "", "cal_live_mesh_count", subjects_);
    UI_MANAGED_SUBJECT_INT(shaper_has_curve_, 0, "cal_live_shaper_has_curve", subjects_);
    UI_MANAGED_SUBJECT_STRING(shaper_note_, shaper_note_buf_, "", "cal_live_shaper_note",
                              subjects_);
    static const char* const kFill[kBelts] = {"cal_live_belt_0", "cal_live_belt_1"};
    static const char* const kDone[kBelts] = {"cal_live_belt_done_0", "cal_live_belt_done_1"};
    static const char* const kName[kBelts] = {"cal_live_belt_name_0", "cal_live_belt_name_1"};
    static const char* const kState[kBelts] = {"cal_live_belt_state_0", "cal_live_belt_state_1"};
    for (int i = 0; i < kBelts; ++i) {
        UI_MANAGED_SUBJECT_INT(belt_fill_[i], 0, kFill[i], subjects_);
        UI_MANAGED_SUBJECT_INT(belt_done_[i], 0, kDone[i], subjects_);
        UI_MANAGED_SUBJECT_STRING(belt_name_[i], belt_name_bufs_[i], "", kName[i], subjects_);
        UI_MANAGED_SUBJECT_STRING(belt_state_[i], belt_state_bufs_[i], "", kState[i], subjects_);
    }
    shaper_chart_.set_on_curve_changed([this](bool has_curve) {
        if (subjects_initialized_) {
            lv_subject_set_int(&shaper_has_curve_, has_curve ? 1 : 0);
        }
    });
    subjects_initialized_ = true;

    // Runs from StaticSubjectRegistry::deinit_all(), which may come after
    // StaticPanelRegistry has destroyed the card.
    StaticSubjectRegistry::instance().register_deinit("CalibrationLiveCard", []() {
        if (auto* self = helix::lazy_global_if_exists<CalibrationLiveCard>()) {
            self->deinit_subjects();
        }
    });
}

void CalibrationLiveCard::deinit_subjects() {
    detach();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void CalibrationLiveCard::publish_text(const LiveCardText& t) {
    lv_subject_set_int(&kind_, static_cast<int>(t.kind));
    lv_subject_set_string(&what_, t.what.c_str());
    lv_subject_set_string(&badge_, t.badge.c_str());
    lv_subject_set_int(&badge_tone_, t.badge_tone);
    lv_subject_set_string(&warn_, t.warn.c_str());
    lv_subject_set_int(&hot_, t.hot ? 1 : 0);
}

void CalibrationLiveCard::update(const calibration::CalibrationRun* run, lv_obj_t* root) {
    if (!subjects_initialized_) {
        init_subjects();
    }
    lv_obj_t* body = find(root, "cal_live_body");
    if (body != body_) {
        // A new widget tree: whatever the old one held went with it.
        detach();
        body_ = body;
        watch_deletion(body);
    }

    const int current = run ? run->current() : -1;
    if (current < 0) {
        const bool over = run && run->phase() != calibration::RunPhase::Ready &&
                          run->phase() != calibration::RunPhase::Running;
        stop_heat();
        stop_shaper();
        publish_text(over ? live_card_done_text() : LiveCardText{});
        return;
    }
    const auto& entry = run->entries()[static_cast<size_t>(current)];
    const StepKind kind = entry.step->info().kind;
    const LiveCardText text = live_card_text(kind, entry.progress.live);
    publish_text(text);

    if (text.kind != LiveKind::Heat) {
        stop_heat();
    }
    if (text.kind != LiveKind::Shaper) {
        stop_shaper();
    }
    switch (text.kind) {
    case LiveKind::Heat:
        show_heat(root, entry.progress.live);
        break;
    case LiveKind::Mesh:
        show_mesh(root, entry.progress.live);
        break;
    case LiveKind::Shaper:
        show_shaper(root, entry.progress.live);
        break;
    case LiveKind::Belt:
        show_belt(entry.progress);
        break;
    default:
        break;
    }
}

void CalibrationLiveCard::watch_deletion(lv_obj_t* body) {
    if (!body) {
        return;
    }
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup; the charts live inside this tree.
    lv_obj_add_event_cb(
        body,
        [](lv_event_t* e) {
            auto* self = static_cast<CalibrationLiveCard*>(lv_event_get_user_data(e));
            if (self->body_ == lv_event_get_target_obj(e)) {
                self->detach();
                self->body_ = nullptr;
            }
        },
        LV_EVENT_DELETE, this);
}

void CalibrationLiveCard::detach() {
    stop_heat();
    stop_shaper();
    shaper_chart_.destroy();
    mesh_fed_version_.reset();
    body_ = nullptr;
}

// ============================================================================
// Heaters: the heater's temperature graph against its calibration target
// ============================================================================

void CalibrationLiveCard::show_heat(lv_obj_t* root, const StepLive& live) {
    const bool bed = live.heater == kBedHeater;
    lv_subject_set_int(&heat_bed_, bed ? 1 : 0);
    if (live.heater.empty() ||
        (heat_graph_ && heat_heater_ == live.heater && heat_graph_->is_valid())) {
        return;
    }
    lv_obj_t* host = find(root, "cal_live_heat_graph");
    if (!host) {
        return;
    }
    stop_heat();
    TempGraphControllerConfig cfg;
    cfg.point_count = kHeatGraphPoints;
    cfg.axis_size = "xs";
    cfg.initial_features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                           TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_GRADIENTS;
    // The axis reaches past the calibration target in whole steps.
    const float step = bed ? 50.0f : 100.0f;
    cfg.scale_params.step = step;
    cfg.scale_params.floor =
        std::max(step * 2.0f, (std::floor(static_cast<float>(live.target_c) / step) + 1.0f) * step);
    cfg.series.push_back({live.heater, kTempGraphSeriesColors[bed ? 1 : 0], true,
                          bed ? lv_tr("Bed") : lv_tr("Nozzle")});
    heat_graph_ = std::make_unique<TempGraphController>(host, cfg);
    heat_heater_ = live.heater;
    spdlog::debug("[CalibrationLiveCard] Heater graph for {}", live.heater);
}

void CalibrationLiveCard::stop_heat() {
    heat_graph_.reset();
    heat_heater_.clear();
}

// ============================================================================
// Bed mesh: the printer's mesh, dimmed where the probe has not been yet
// ============================================================================

void CalibrationLiveCard::show_mesh(lv_obj_t* root, const StepLive& live) {
    lv_subject_set_int(&mesh_unprobed_, live.probe_total > 0
                                            ? mesh_unprobed_percent(live.probed, live.probe_total)
                                            : 100);
    const std::string count =
        live.probe_total > 0
            ? fmt::format(fmt::runtime(lv_tr("{} of {} points")), live.probed, live.probe_total)
            : std::string();
    lv_subject_set_string(&mesh_count_, count.c_str());
    if (lv_obj_t* map = find(root, "cal_live_mesh_map")) {
        feed_mesh_map(map);
    }
}

void CalibrationLiveCard::feed_mesh_map(lv_obj_t* map) {
    auto& mesh_state = BedMeshState::instance();
    const int version = lv_subject_get_int(mesh_state.get_version_subject());
    if (mesh_fed_version_ == version) {
        return;
    }
    ui_bed_mesh_set_static_mode(map, true);
    ui_bed_mesh_set_zero_plane_visible(map, false);
    ui_bed_mesh_set_render_mode(map, BedMeshRenderMode::Force3D);

    const MeshStats& stats = mesh_state.stats();
    const BedMeshProfile& profile = mesh_state.profile();
    std::vector<std::vector<float>> matrix;
    int rows = kFlatMeshPoints;
    int cols = kFlatMeshPoints;
    if (stats.loaded) {
        matrix = mean_normalized_matrix(profile, stats);
        rows = stats.rows;
        cols = stats.cols;
    } else {
        // No mesh yet: the probe fills in a flat bed.
        matrix.assign(static_cast<size_t>(rows),
                      std::vector<float>(static_cast<size_t>(cols), 0.0f));
    }
    std::vector<const float*> row_ptrs;
    row_ptrs.reserve(matrix.size());
    for (const auto& row : matrix) {
        row_ptrs.push_back(row.data());
    }
    if (!ui_bed_mesh_set_data(map, row_ptrs.data(), rows, cols)) {
        return;
    }
    double bed_x0 = profile.mesh_min[0], bed_x1 = profile.mesh_max[0];
    double bed_y0 = profile.mesh_min[1], bed_y1 = profile.mesh_max[1];
    double mesh_x0 = bed_x0, mesh_x1 = bed_x1, mesh_y0 = bed_y0, mesh_y1 = bed_y1;
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        const auto& bed = api->hardware().build_volume();
        if (bed.x_max > bed.x_min && bed.y_max > bed.y_min) {
            bed_x0 = bed.x_min;
            bed_x1 = bed.x_max;
            bed_y0 = bed.y_min;
            bed_y1 = bed.y_max;
            if (!stats.loaded) {
                mesh_x0 = bed_x0;
                mesh_x1 = bed_x1;
                mesh_y0 = bed_y0;
                mesh_y1 = bed_y1;
            }
        }
    }
    if (bed_x1 > bed_x0 && bed_y1 > bed_y0) {
        ui_bed_mesh_set_bounds(map, bed_x0, bed_x1, bed_y0, bed_y1, mesh_x0, mesh_x1, mesh_y0,
                               mesh_y1);
    }
    ui_bed_mesh_set_z_display_offset(map, static_cast<double>(stats.mean_z));
    mesh_fed_version_ = version;
}

// ============================================================================
// Input shaper: the resonance curve streamed during each axis's sweep
// ============================================================================

void CalibrationLiveCard::show_shaper(lv_obj_t* root, const StepLive& live) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!shaper_facts_asked_ && api) {
        shaper_facts_asked_ = true;
        calibration::query_live_sweep_facts(
            *api, lifetime_.bg_cb("CalibrationLiveCard.sweep_facts",
                                  [this](calibration::LiveSweepFacts facts) {
                                      shaper_facts_ = std::move(facts);
                                  }));
    }
    lv_subject_set_string(&shaper_note_, live.axis == 0
                                             ? lv_tr("Checking the accelerometer")
                                             : lv_tr("The curve draws in as the axis sweeps"));
    if (live.axis == 0) {
        return;
    }
    if (live.axis != shaper_axis_) {
        shaper_axis_ = live.axis;
        shaper_armed_ = false;
    }
    if (!shaper_armed_ && !live.analyzing && shaper_facts_) {
        const auto target = calibration::live_sweep_target(
            *shaper_facts_,
            live.axis == 'Y' ? calibration::SweepAxis::Y : calibration::SweepAxis::X);
        shaper_armed_ = shaper_chart_.begin(find(root, "cal_live_shaper_chart"), live.axis, target);
        if (shaper_armed_) {
            shaper_chart_.set_marks_peak(true);
            if (auto* chart = shaper_chart_.chart()) {
                ui_frequency_response_chart_set_y_labels_visible(chart, false);
            }
        } else {
            lv_subject_set_string(&shaper_note_,
                                  lv_tr("This printer cannot stream the sweep to the screen"));
        }
    }
    if (!shaper_armed_) {
        return;
    }
    shaper_chart_.on_progress(live.analyzing ? ShaperCalibrationPhase::Analyzing
                                             : ShaperCalibrationPhase::Sweeping,
                              live.sweep_hz);
}

void CalibrationLiveCard::stop_shaper() {
    shaper_chart_.stop();
    shaper_axis_ = 0;
    shaper_armed_ = false;
    shaper_facts_asked_ = false;
    shaper_facts_.reset();
}

// ============================================================================
// Belt tension: each belt's pull up to its target mark
// ============================================================================

void CalibrationLiveCard::show_belt(const calibration::StepProgress& progress) {
    const char* const names[kBelts] = {lv_tr("X belt"), lv_tr("Y belt")};
    for (int i = 0; i < kBelts; ++i) {
        // A step that does not measure each belt shows them pulled together.
        float pull = progress.live.belt_pull[i];
        if (pull < 0.0f) {
            pull = progress.percent < 0 ? 0.0f : static_cast<float>(progress.percent) / 100.0f;
        }
        const bool done = pull >= 1.0f;
        lv_subject_set_int(&belt_fill_[i], belt_fill_percent(pull));
        lv_subject_set_int(&belt_done_[i], done ? 1 : 0);
        lv_subject_set_string(&belt_name_[i], names[i]);
        lv_subject_set_string(&belt_state_[i], done ? lv_tr("at target") : lv_tr("pulling..."));
    }
}

} // namespace helix::ui
