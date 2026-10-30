// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_plr_wizard_overlay.cpp
 * @brief Guided resume after a power loss, through the HelixPrint plugin
 */

#include "ui_plr_wizard_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_nav.h"
#include "ui_position_utils.h"

#include "app_globals.h"
#include "connection_state.h"
#include "exception_policy.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "plr_plugin_rpc.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "unit_conversions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

// Static like the other guided overlays' subjects: XML bindings resolve them
// by name, and SubjectManager deinit on the singleton keeps observers
// disconnected across rebuilds.
static lv_subject_t s_plr_step;
static lv_subject_t s_plr_tier;
/// PlrZStepKind of the plan.
static lv_subject_t s_plr_z_kind;
static lv_subject_t s_plr_continue_text;
static char s_plr_continue_text_buf[48];
static lv_subject_t s_plr_file;
static char s_plr_file_buf[128];
static lv_subject_t s_plr_position;
static char s_plr_position_buf[64];
static lv_subject_t s_plr_outage;
static char s_plr_outage_buf[96];
static lv_subject_t s_plr_bed;
static char s_plr_bed_buf[64];
static lv_subject_t s_plr_bed_live;
static char s_plr_bed_live_buf[48];
static lv_subject_t s_plr_progress;
static char s_plr_progress_buf[96];
static lv_subject_t s_plr_z;
static char s_plr_z_buf[24];
static lv_subject_t s_plr_error;
static char s_plr_error_buf[256];
static lv_subject_t s_plr_warn_layer_start;
static lv_subject_t s_plr_jog_step;
/// 1 once the plugin reports the resumed print starting.
static lv_subject_t s_plr_print_starting;
/// 1 once a downward jog has landed on the tier-1 Z step.
static lv_subject_t s_plr_z_jogged;
static lv_subject_t s_plr_need_part;
static lv_subject_t s_plr_ack_part;
static lv_subject_t s_plr_need_nozzle;
static lv_subject_t s_plr_ack_nozzle;
static lv_subject_t s_plr_need_no_mesh;
static lv_subject_t s_plr_ack_no_mesh;
static lv_subject_t s_plr_need_next_layer;
static lv_subject_t s_plr_ack_next_layer;
static lv_subject_t s_plr_need_cold_bed;
static lv_subject_t s_plr_ack_cold_bed;

namespace {

bool s_wizard_open = false;

constexpr const char* kNotifyHandler = "plr_wizard";
constexpr std::array<double, 3> kJogStepsMm = {0.05, 0.1, 1.0};
constexpr int kDefaultJogStep = 1;
constexpr const char* kLayerStartWarning = "layer_start";

/// A translated format string applied to @p args. A mistranslated
/// placeholder yields the empty string rather than aborting.
template <typename... Args> std::string tr_format(const char* fmt_tr, Args&&... args) {
    std::string out;
    helix::contain_exceptions("[PLR wizard] format", [&] {
        out = fmt::format(fmt::runtime(fmt_tr), std::forward<Args>(args)...);
    });
    return out;
}

std::string outage_text(double age_s) {
    if (age_s < 0.0) {
        return {};
    }
    if (age_s < 60.0) {
        return lv_tr("Power was lost less than a minute ago.");
    }
    const int minutes = static_cast<int>(std::lround(age_s / 60.0));
    if (minutes < 120) {
        return tr_format(lv_tr("Power was lost about {} minutes ago."), minutes);
    }
    return tr_format(lv_tr("Power was lost about {} hours ago."),
                     static_cast<int>(std::lround(age_s / 3600.0)));
}

bool needs(const std::vector<PlrConfirmation>& set, PlrConfirmation c) {
    return std::find(set.begin(), set.end(), c) != set.end();
}

bool live_step(PlrWizardStep step) {
    return step == PlrWizardStep::Preparing || step == PlrWizardStep::ZStep ||
           step == PlrWizardStep::Committing;
}

} // namespace

bool plr_wizard_open() {
    return s_wizard_open;
}

const char* plr_progress_step_text(const std::string& step) {
    // prepare; the heat steps come again in commit when it re-heats. A nozzle
    // frozen into the part is heated where it stopped before any lift, then
    // drops to standby once it has let go.
    if (step == "bed_wait") {
        return lv_tr("Heating the bed");
    }
    if (step == "chamber_wait") {
        return lv_tr("Waiting for the chamber to heat");
    }
    if (step == "nozzle_in_place") {
        return lv_tr(
            "Heating the bed and nozzle so the print re-grips and the nozzle lets go of it");
    }
    if (step == "nozzle_standby") {
        return lv_tr("Cooling the nozzle to standby while you check the print");
    }
    if (step == "lift") {
        return lv_tr("Lifting the nozzle clear of the print");
    }
    if (step == "home_xy") {
        return lv_tr("Homing X and Y");
    }
    if (step == "position") {
        return lv_tr("Moving over the resume point");
    }
    // commit
    if (step == "z_reference") {
        return lv_tr("Setting the nozzle height");
    }
    if (step == "restore") {
        return lv_tr("Restoring the print settings");
    }
    if (step == "heat_nozzle") {
        return lv_tr("Heating the nozzle");
    }
    if (step == "purge") {
        return lv_tr("Purging nozzle");
    }
    if (step == "move_to_resume") {
        return lv_tr("Moving back to the print");
    }
    if (step == "start") {
        return lv_tr("Starting the print");
    }
    return nullptr;
}

PlrWizardOverlay::~PlrWizardOverlay() {
    s_wizard_open = false;
    stop_listening();
    if (subjects_initialized_) {
        subjects_.deinit_all();
    }
}

// ============================================================================
// SUBJECTS / CALLBACKS
// ============================================================================

void PlrWizardOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(s_plr_step, 0, "plr_wizard_step", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_tier, 0, "plr_wizard_tier", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_z_kind, 0, "plr_wizard_z_kind", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_continue_text, s_plr_continue_text_buf, "",
                                  "plr_wizard_continue_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_file, s_plr_file_buf, "", "plr_wizard_file", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_position, s_plr_position_buf, "", "plr_wizard_position",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_outage, s_plr_outage_buf, "", "plr_wizard_outage",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_bed, s_plr_bed_buf, "", "plr_wizard_bed", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_bed_live, s_plr_bed_live_buf, "", "plr_wizard_bed_live",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_progress, s_plr_progress_buf, "", "plr_wizard_progress",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_z, s_plr_z_buf, "", "plr_wizard_z", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_plr_error, s_plr_error_buf, "", "plr_wizard_error", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_warn_layer_start, 0, "plr_warn_layer_start", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_jog_step, kDefaultJogStep, "plr_jog_step", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_print_starting, 0, "plr_print_starting", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_z_jogged, 0, "plr_z_jogged", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_need_part, 0, "plr_need_part", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_ack_part, 0, "plr_ack_part", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_need_nozzle, 0, "plr_need_nozzle", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_ack_nozzle, 0, "plr_ack_nozzle", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_need_no_mesh, 0, "plr_need_no_mesh", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_ack_no_mesh, 0, "plr_ack_no_mesh", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_need_next_layer, 0, "plr_need_next_layer", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_ack_next_layer, 0, "plr_ack_next_layer", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_need_cold_bed, 0, "plr_need_cold_bed", subjects_);
        UI_MANAGED_SUBJECT_INT(s_plr_ack_cold_bed, 0, "plr_ack_cold_bed", subjects_);
    });
}

void PlrWizardOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_plr_wizard_continue", [](lv_event_t*) { get_plr_wizard_overlay().handle_continue(); }},
        {"on_plr_wizard_resume", [](lv_event_t*) { get_plr_wizard_overlay().handle_resume(); }},
        {"on_plr_wizard_abort", [](lv_event_t*) { get_plr_wizard_overlay().handle_abort(); }},
        // Leaving a live step aborts the sequence in on_deactivating().
        {"on_plr_wizard_close", [](lv_event_t*) { helix::nav::go_back(); }},
        // user_data carries the direction / step index as a string
        {"on_plr_wizard_jog",
         [](lv_event_t* e) {
             const char* dir = static_cast<const char*>(lv_event_get_user_data(e));
             if (dir) {
                 get_plr_wizard_overlay().handle_jog(std::atoi(dir));
             }
         }},
        {"on_plr_wizard_step",
         [](lv_event_t* e) {
             const char* index = static_cast<const char*>(lv_event_get_user_data(e));
             if (index) {
                 get_plr_wizard_overlay().handle_step_select(std::atoi(index));
             }
         }},
    });
}

// ============================================================================
// UI CREATION / LIFECYCLE
// ============================================================================

lv_obj_t* PlrWizardOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    auto& ps = get_printer_state();
    conn_observer_ = observe<int>(
        ps.network_state().get_printer_connection_state_subject(), this,
        [](PlrWizardOverlay* self, int state) { self->on_connection_changed(state); },
        ps.get_subjects_lifetime());
    z_observer_ = observe<int>(
        ps.motion_state().get_position_z_subject(), this,
        [](PlrWizardOverlay* self, int centimm) { self->update_z_display(centimm); },
        ps.get_subjects_lifetime());
    bed_temp_observer_ = observe<int>(
        ps.temperature_state().get_bed_temp_subject(), this,
        [](PlrWizardOverlay* self, int /*deci*/) { self->update_bed_live(); },
        ps.get_subjects_lifetime());
    bed_target_observer_ = observe<int>(
        ps.temperature_state().get_bed_target_subject(), this,
        [](PlrWizardOverlay* self, int /*deci*/) { self->update_bed_live(); },
        ps.get_subjects_lifetime());
    return overlay_root_;
}

void PlrWizardOverlay::open(IMoonrakerAPI* api) {
    if (!api) {
        spdlog::warn("[PLR wizard] open: no IMoonrakerAPI");
        return;
    }
    api_ = api;
    ++session_;
    plan_.reset();
    plan_reply_.reset();
    plan_ready_seen_ = false;
    print_starting_ = false;
    z_given_.reset();
    status_ = PlrPluginStatus{};
    connected_ = true;
    error_text_.clear();
    jog_.reset();
    if (!show(lv_screen_active())) {
        return;
    }
    awake_.acquire();
    s_wizard_open = true;
    for (lv_subject_t* s : {&s_plr_ack_part, &s_plr_ack_nozzle, &s_plr_ack_no_mesh,
                            &s_plr_ack_next_layer, &s_plr_ack_cold_bed, &s_plr_need_part,
                            &s_plr_need_nozzle, &s_plr_need_no_mesh, &s_plr_need_next_layer,
                            &s_plr_need_cold_bed, &s_plr_warn_layer_start, &s_plr_print_starting}) {
        lv_subject_set_int(s, 0);
    }
    lv_subject_set_int(&s_plr_jog_step, kDefaultJogStep);
    lv_subject_copy_string(&s_plr_progress, "");
    set_step(PlrWizardStep::Offer);
    listen_for_notifications();
    request_plan();
}

void PlrWizardOverlay::on_deactivating(DeactivateReason reason) {
    // Nothing here is cancelled: a screensaver or a view pushed on top (Print
    // Status opens as the resumed print starts) must not abort a bed wait the
    // user is waiting out. Leaving the wizard is on_dismissed().
    (void)reason;
}

void PlrWizardOverlay::on_dismissed() {
    // Leaving mid-sequence is an abort: the plugin stops, turns the heaters
    // off and keeps the snapshot for another try.
    if (step_ != PlrWizardStep::Done && step_ != PlrWizardStep::Failed &&
        step_ != PlrWizardStep::Aborted && !print_starting_) {
        advance(Event{false, false, true});
    }
    stop_listening();
    awake_.reset();
    s_wizard_open = false;
    // Replies still in flight belong to this opening, not the next.
    ++session_;
    jog_.reset();
}

// ============================================================================
// STEP MACHINE
// ============================================================================

void PlrWizardOverlay::set_step(PlrWizardStep step) {
    step_ = step;
    lv_subject_set_int(&s_plr_step, static_cast<int>(step));
}

std::vector<PlrConfirmation> PlrWizardOverlay::given() const {
    std::vector<PlrConfirmation> out;
    const std::pair<lv_subject_t*, PlrConfirmation> acks[] = {
        {&s_plr_ack_part, PlrConfirmation::PartAttached},
        {&s_plr_ack_nozzle, PlrConfirmation::NozzleClear},
        {&s_plr_ack_no_mesh, PlrConfirmation::AcceptNoMesh},
        {&s_plr_ack_next_layer, PlrConfirmation::AcceptNextLayer},
        {&s_plr_ack_cold_bed, PlrConfirmation::AcceptColdBed},
    };
    for (const auto& [subject, c] : acks) {
        if (lv_subject_get_int(subject) != 0) {
            out.push_back(c);
        }
    }
    if (z_given_) {
        out.push_back(*z_given_);
    }
    return out;
}

void PlrWizardOverlay::advance(const Event& event) {
    PlrWizardInputs in;
    in.step = step_;
    in.rpc_ok = event.rpc_ok;
    in.rpc_failed = event.rpc_failed;
    in.user_abort = event.user_abort;
    in.connected = connected_;
    in.plan = plan_ ? &*plan_ : nullptr;
    in.given = given();

    const PlrWizardStep prev = step_;
    const PlrWizardStep next = plr_next_wizard_step(in);
    if (next == prev) {
        return;
    }
    spdlog::info("[PLR wizard] step {} -> {}", static_cast<int>(prev), static_cast<int>(next));

    // Once the plugin has started moving or heating, every way out of the
    // sequence stops it: heaters off, snapshot kept.
    if (live_step(prev) && (next == PlrWizardStep::Aborted || next == PlrWizardStep::Failed) &&
        connected_ && api_ && plan_) {
        plr_rpc::abort(
            *api_, plan_->plan_id, nullptr,
            lifetime_.bg_cb("PlrWizardOverlay::abort_error", [](const MoonrakerError& err) {
                spdlog::warn("[PLR wizard] abort failed: {}", err.message);
            }));
    }
    set_step(next);
    enter_step(next);
}

void PlrWizardOverlay::enter_step(PlrWizardStep step) {
    // A jog still queued would land after the step it belonged to.
    jog_.reset();
    const int session = session_;
    auto on_ok = lifetime_.bg_cb("PlrWizardOverlay::rpc_ok", [this, session]() {
        if (session == session_) {
            advance(Event{true, false, false});
        }
    });
    auto on_error =
        lifetime_.bg_cb("PlrWizardOverlay::rpc_error", [this, session](const MoonrakerError& err) {
            if (session == session_) {
                on_rpc_error(err);
            }
        });

    switch (step) {
    case PlrWizardStep::Checks:
        publish_plan();
        break;
    case PlrWizardStep::Preparing:
        lv_subject_copy_string(&s_plr_progress, lv_tr("Waiting for the printer"));
        update_bed_live();
        plr_rpc::prepare(*api_, plan_->plan_id, false, std::move(on_ok), std::move(on_error));
        break;
    case PlrWizardStep::ZStep:
        lv_subject_set_int(&s_plr_jog_step, kDefaultJogStep);
        lv_subject_set_int(&s_plr_z_jogged, 0);
        break;
    case PlrWizardStep::Committing:
        lv_subject_copy_string(&s_plr_progress, lv_tr("Waiting for the printer"));
        plr_rpc::commit(*api_, plan_->plan_id, given(), plan_->tier, std::move(on_ok),
                        std::move(on_error));
        break;
    case PlrWizardStep::Failed:
        lv_subject_copy_string(&s_plr_error, error_text_.c_str());
        stop_listening();
        // Nothing is held for the user to watch once the sequence has ended.
        awake_.reset();
        break;
    case PlrWizardStep::Done:
    case PlrWizardStep::Aborted:
        stop_listening();
        awake_.reset();
        break;
    default:
        break;
    }
}

void PlrWizardOverlay::request_plan() {
    if (!api_) {
        return;
    }
    const int session = session_;
    auto on_error =
        lifetime_.bg_cb("PlrWizardOverlay::plan_error", [this, session](const MoonrakerError& err) {
            if (session == session_) {
                on_rpc_error(err);
            }
        });
    if (!plan_reply_ && status_.backend == PlrPluginBackend::None) {
        plr_rpc::request_status(*api_,
                                lifetime_.bg_cb("PlrWizardOverlay::status",
                                                [this, session](const PlrPluginStatus& s) {
                                                    if (session == session_) {
                                                        on_status(s);
                                                    }
                                                }),
                                on_error);
        return;
    }
    plr_rpc::request_plan(*api_,
                          lifetime_.bg_cb("PlrWizardOverlay::plan",
                                          [this, session](const PlrPlanReply& plan) {
                                              if (session == session_) {
                                                  on_plan(plan);
                                              }
                                          }),
                          on_error);
}

void PlrWizardOverlay::on_status(const PlrPluginStatus& status) {
    status_ = status;
    if (!status.offer_available || status.backend == PlrPluginBackend::None) {
        error_text_ = lv_tr("The printer no longer has a print to resume.");
        advance(Event{false, true, false});
        return;
    }
    request_plan();
}

void PlrWizardOverlay::on_plan(const PlrPlanReply& plan) {
    if (plan.pending) {
        // A long scan: the plan follows with helix:plr_plan_ready. Pending
        // again after that event means the scan finished without a plan, and
        // asking again would only start it over.
        if (plan_ready_seen_) {
            error_text_ = lv_tr("The printer could not work out where to resume.");
            advance(Event{false, true, false});
            return;
        }
        spdlog::info("[PLR wizard] plan pending, waiting for the scan");
        return;
    }
    plan_reply_ = plan;
    plan_ = plr_plan_view(plan, status_.tier, status_.backend);
    if (plan_->unsatisfiable) {
        error_text_ = lv_tr("This resume needs a newer version of HelixScreen.");
        spdlog::warn("[PLR wizard] plan requires confirmations this app does not know");
    } else if (plan_->tier != 1 && plan_->tier != 2) {
        error_text_ = lv_tr("Resuming is not set up for this printer.");
    }
    advance(Event{true, false, false});
}

void PlrWizardOverlay::on_rpc_error(const MoonrakerError& err) {
    spdlog::error("[PLR wizard] {} failed: {}", err.method, err.message);
    error_text_ = err.localized_message();
    advance(Event{false, true, false});
}

void PlrWizardOverlay::on_progress(const PlrProgressEvent& event) {
    if (!plan_ || event.plan_id != plan_->plan_id) {
        return;
    }
    if (step_ != PlrWizardStep::Preparing && step_ != PlrWizardStep::Committing) {
        return;
    }
    // done and failed say nothing new: the next step's running line, or the
    // RPC's own reply, follows.
    if (event.state != "running") {
        return;
    }
    if (step_ == PlrWizardStep::Committing && event.step == "start") {
        print_starting_ = true;
        lv_subject_set_int(&s_plr_print_starting, 1);
        // The running print decides dim and sleep from here, as any print does.
        awake_.reset();
    }
    if (const char* text = plr_progress_step_text(event.step)) {
        lv_subject_copy_string(&s_plr_progress, text);
    }
}

void PlrWizardOverlay::on_plan_ready(const PlrPlanReady& ready) {
    if (step_ != PlrWizardStep::Offer || plan_reply_) {
        return;
    }
    if (!ready.error.empty()) {
        spdlog::warn("[PLR wizard] plan failed in the background: {}", ready.error);
        error_text_ = ready.error;
        advance(Event{false, true, false});
        return;
    }
    spdlog::info("[PLR wizard] plan {} ready", ready.plan_id);
    plan_ready_seen_ = true;
    request_plan();
}

void PlrWizardOverlay::on_connection_changed(int state) {
    const bool connected = state == static_cast<int>(ConnectionState::CONNECTED);
    if (connected == connected_) {
        return;
    }
    connected_ = connected;
    if (!connected_) {
        // Nothing a disconnected screen confirmed can be trusted to apply.
        error_text_ = lv_tr("Lost the connection to the printer.");
        jog_.reset();
        advance(Event{});
    }
}

// ============================================================================
// USER ACTIONS
// ============================================================================

void PlrWizardOverlay::handle_continue() {
    if (step_ == PlrWizardStep::Checks) {
        advance(Event{});
    }
}

void PlrWizardOverlay::handle_resume() {
    if (step_ != PlrWizardStep::ZStep || !plan_) {
        return;
    }
    // A jog still moving or queued would land after the plugin reads the part
    // top, so the resume waits for the head to stop.
    if (jog_.in_flight()) {
        NOTIFY_INFO(lv_tr("Wait for the nozzle to stop moving"));
        return;
    }
    // The part top is wherever the user lowered the nozzle to; with no jog
    // down the lifted height would be declared as the part top.
    if (plan_->tier == 1 && lv_subject_get_int(&s_plr_z_jogged) == 0) {
        return;
    }
    z_given_ = plan_->tier == 1 ? PlrConfirmation::ZJogged : PlrConfirmation::ZConfirmed;
    advance(Event{});
    if (step_ == PlrWizardStep::ZStep) {
        z_given_.reset();
    }
}

void PlrWizardOverlay::handle_abort() {
    // An abort turns the heaters off, which would land in the running print.
    if (print_starting_) {
        return;
    }
    advance(Event{false, false, true});
}

void PlrWizardOverlay::handle_step_select(int index) {
    if (index >= 0 && index < static_cast<int>(kJogStepsMm.size())) {
        lv_subject_set_int(&s_plr_jog_step, index);
    }
}

void PlrWizardOverlay::handle_jog(int direction) {
    if (step_ != PlrWizardStep::ZStep || !plan_ || plan_->tier != 1 || !api_ || direction == 0) {
        return;
    }
    const int index = lv_subject_get_int(&s_plr_jog_step);
    const double step_mm = kJogStepsMm[static_cast<size_t>(
        std::clamp(index, 0, static_cast<int>(kJogStepsMm.size()) - 1))];
    AxisMove move;
    move.dz = direction > 0 ? step_mm : -step_mm;

    // The same relative move the motion panel jogs with, one in flight and
    // further taps folded into the next.
    if (auto immediate = jog_.on_tap(move)) {
        send_jog(*immediate);
    }
}

void PlrWizardOverlay::send_jog(const AxisMove& move) {
    const SafetyLimits& limits = api_->get_safety_limits();
    const auto& settings = SettingsManager::instance();
    const double xy_feedrate = static_cast<double>(effective_jog_speed_mm_min(
        settings.get_jog_speed_xy(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));
    const double z_feedrate = static_cast<double>(effective_jog_speed_mm_min(
        settings.get_jog_speed_z(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));
    const int session = session_;
    api_->motion().move_relative(
        move.dx, move.dy, move.dz, xy_feedrate, z_feedrate,
        lifetime_.bg_cb("PlrWizardOverlay::jog_ack",
                        [this, session, down = move.dz < 0.0]() {
                            if (session != session_ || step_ != PlrWizardStep::ZStep) {
                                return;
                            }
                            if (down) {
                                lv_subject_set_int(&s_plr_z_jogged, 1);
                            }
                            if (auto flush = jog_.on_ack()) {
                                if (const auto* delta = std::get_if<AxisMove>(&*flush)) {
                                    send_jog(*delta);
                                }
                            }
                        }),
        lifetime_.bg_cb("PlrWizardOverlay::jog_error", [this, session](const MoonrakerError& err) {
            if (session != session_ || step_ != PlrWizardStep::ZStep) {
                return;
            }
            jog_.on_error();
            NOTIFY_ERROR(lv_tr("Jog failed: {}"), err.localized_message());
        }));
}

// ============================================================================
// PRESENTATION
// ============================================================================

void PlrWizardOverlay::publish_plan() {
    if (!plan_ || !plan_reply_) {
        return;
    }
    const PlrPlanReply& reply = *plan_reply_;
    lv_subject_set_int(&s_plr_tier, plan_->tier);
    lv_subject_set_int(&s_plr_z_kind, static_cast<int>(plr_z_step_kind(*plan_)));
    lv_subject_copy_string(&s_plr_continue_text, plr_prepare_moves_head(*plan_)
                                                     ? lv_tr("Heat and Home")
                                                     : lv_tr("Start Heating"));

    const std::string& path =
        reply.filename.empty() && status_.snapshot ? status_.snapshot->filename : reply.filename;
    const std::string file =
        helix::gcode::get_display_filename(helix::gcode::resolve_gcode_filename(path));
    lv_subject_copy_string(&s_plr_file, file.c_str());
    const std::string position =
        reply.layer ? tr_format(lv_tr("Layer {}, Z {:.2f} mm"), *reply.layer, reply.z_gcode)
                    : tr_format(lv_tr("Z {:.2f} mm"), reply.z_gcode);
    lv_subject_copy_string(&s_plr_position, position.c_str());
    const std::string outage = outage_text(status_.snapshot ? status_.snapshot->age_s : -1.0);
    lv_subject_copy_string(&s_plr_outage, outage.c_str());
    const std::string bed = reply.bed_target > 0.0f
                                ? tr_format(lv_tr("Bed is at {:.0f}°C, the print needs {:.0f}°C."),
                                            reply.bed_current, reply.bed_target)
                                : std::string();
    lv_subject_copy_string(&s_plr_bed, bed.c_str());
    lv_subject_set_int(&s_plr_warn_layer_start,
                       std::find(reply.warnings.begin(), reply.warnings.end(),
                                 kLayerStartWarning) != reply.warnings.end() ||
                               reply.method == kLayerStartWarning
                           ? 1
                           : 0);

    const std::vector<PlrConfirmation> need =
        plr_check_confirmations(*plan_, plr_plan_bed_state(*plan_));
    lv_subject_set_int(&s_plr_need_part, needs(need, PlrConfirmation::PartAttached) ? 1 : 0);
    lv_subject_set_int(&s_plr_need_nozzle, needs(need, PlrConfirmation::NozzleClear) ? 1 : 0);
    lv_subject_set_int(&s_plr_need_no_mesh, needs(need, PlrConfirmation::AcceptNoMesh) ? 1 : 0);
    lv_subject_set_int(&s_plr_need_next_layer,
                       needs(need, PlrConfirmation::AcceptNextLayer) ? 1 : 0);
    lv_subject_set_int(&s_plr_need_cold_bed, needs(need, PlrConfirmation::AcceptColdBed) ? 1 : 0);
}

void PlrWizardOverlay::update_bed_live() {
    auto& temps = get_printer_state().temperature_state();
    const double current =
        units::from_decidegrees(lv_subject_get_int(temps.get_bed_temp_subject()));
    const double target =
        units::from_decidegrees(lv_subject_get_int(temps.get_bed_target_subject()));
    const std::string text = target > 0.0
                                 ? tr_format(lv_tr("Bed {:.0f} / {:.0f}°C"), current, target)
                                 : tr_format(lv_tr("Bed {:.0f}°C"), current);
    lv_subject_copy_string(&s_plr_bed_live, text.c_str());
}

void PlrWizardOverlay::update_z_display(int centimm) {
    position::format_position(centimm, s_plr_z_buf, sizeof(s_plr_z_buf));
    lv_subject_copy_string(&s_plr_z, s_plr_z_buf);
}

// ============================================================================
// NOTIFICATIONS
// ============================================================================

void PlrWizardOverlay::listen_for_notifications() {
    if (!api_ || listening_) {
        return;
    }
    listening_ = true;
    const int session = session_;
    api_->register_method_callback(
        kPlrProgressNotify, kNotifyHandler,
        lifetime_.bg_cb("PlrWizardOverlay::progress", [this, session](const nlohmann::json& msg) {
            if (session != session_) {
                return;
            }
            if (auto event = plr_parse_progress_event(msg)) {
                on_progress(*event);
            }
        }));
    api_->register_method_callback(
        kPlrPlanReadyNotify, kNotifyHandler,
        lifetime_.bg_cb("PlrWizardOverlay::plan_ready", [this, session](const nlohmann::json& msg) {
            if (session == session_) {
                on_plan_ready(plr_parse_plan_ready(msg));
            }
        }));
}

void PlrWizardOverlay::stop_listening() {
    if (!api_ || !listening_) {
        return;
    }
    listening_ = false;
    api_->unregister_method_callback(kPlrProgressNotify, kNotifyHandler);
    api_->unregister_method_callback(kPlrPlanReadyNotify, kNotifyHandler);
}

} // namespace helix::ui
