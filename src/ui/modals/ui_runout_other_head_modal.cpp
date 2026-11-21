// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_runout_other_head_modal.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/static_subject_registry.h"
#include "core/timer_guard.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace helix::ui {

namespace roh = helix::runout_other_head;

namespace {

constexpr int kRows = 4;

/// Row states the XML binds.
enum RowState : int { kHidden = 0, kEligible = 1, kWarn = 2, kRefused = 3 };

struct Subjects {
    helix::Subject message;
    char message_buf[256] = {};
    helix::Subject any_eligible;
    /// 1 while a pick's confirmation is up: every row is disabled.
    helix::Subject confirming;
    std::array<helix::Subject, kRows> state;
    std::array<helix::Subject, kRows> title;
    std::array<helix::Subject, kRows> note;
    std::array<helix::Subject, kRows> color;
    std::array<std::array<char, 96>, kRows> title_buf{};
    std::array<std::array<char, 96>, kRows> note_buf{};
    SubjectManager manager;
    bool initialized = false;
};

Subjects& subjects() {
    static Subjects s;
    return s;
}

std::optional<roh::RunoutFacts> facts_for(int runout_head) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend == nullptr || !roh::supported(backend->get_type())) {
        return std::nullopt;
    }
    return backend->runout_other_head_facts(runout_head);
}

const roh::HeadFacts* head_facts(const roh::RunoutFacts& facts, int head) {
    for (const auto& h : facts.heads) {
        if (h.head == head) {
            return &h;
        }
    }
    return nullptr;
}

std::string filament_name(const roh::FilamentRecord& r) {
    return r.subtype.empty() || r.subtype == "NONE" ? r.type : r.type + " " + r.subtype;
}

/// The record's first colour; the muted theme colour when it names none.
lv_color_t first_color(const roh::FilamentRecord& r) {
    if (r.colors.empty()) {
        return theme_manager_get_color("text_muted");
    }
    return lv_color_hex(static_cast<uint32_t>(std::strtoul(r.colors.front().c_str(), nullptr, 16)));
}

/// Send @p gcode on its own; the printer's own complaint is the report if it fails.
void send_quiet(const std::string& gcode, const char* what) {
    if (gcode.empty()) {
        return;
    }
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->execute_gcode(
            gcode, nullptr,
            [what](const MoonrakerError& err) {
                spdlog::warn("[RunoutOtherHead] {} failed: {}", what, err.message);
            },
            0, false, nullptr, /*caller_surfaces_errors=*/false);
    }
}

/// The print moved to @p target_name but stayed paused: say so, and offer
/// @p resume, which resumes it there.
void show_stranded_dialog(const std::string& target_name, const std::string& resume) {
    const std::string text = fmt::format(
        fmt::runtime(lv_tr("The print moved to {} but did not resume. Check {}, then press "
                           "Resume.")),
        target_name, target_name);
    modal_confirm(lv_tr("Print Did Not Resume"), text.c_str(), ModalSeverity::Warning,
                  lv_tr("Resume"), [resume]() {
                      spdlog::info("[RunoutOtherHead] resuming on the new head: {}", resume);
                      if (IMoonrakerAPI* api = get_moonraker_api()) {
                          api->execute_gcode(resume, nullptr, nullptr, 0);
                      }
                  });
}

/// What a sent switch needs once it ends.
struct SwitchJob {
    roh::RunoutFacts before;
    roh::FilamentRecord label;
    int target = -1;
    roh::ContinuePlan plan;
    std::string runout_name;
    std::string target_name;
    RunoutOtherHeadModal::SwitchHooks hooks;
};

/// Watches one sent switch until the printer shows it or it is clearly not
/// coming. The firmware declines a switch by doing nothing, so a clean reply
/// proves nothing: only the routing moving to the target and the print
/// leaving the pause does.
class SwitchWatch {
  public:
    ~SwitchWatch() {
        stop();
    }

    void start(SwitchJob job) {
        // A watch still running is ended without its outcome; its pending
        // restore stays in the store and the next settle applies it.
        if (in_flight()) {
            stop();
            not_moved();
        }
        ++generation_;
        job_ = std::move(job);
        reply_ = roh::SendReply::None;
        sent_at_ = lv_tick_get();
        reply_at_ = sent_at_;
        timer_ = lv_timer_create([](lv_timer_t*) { watch().tick(); }, 1000, nullptr);
    }

    /// Main thread. Ignored when @p generation is not the watch in progress.
    void note_reply(uint64_t generation, roh::SendReply reply) {
        if (generation != generation_ || timer_ == nullptr) {
            return;
        }
        reply_ = reply;
        reply_at_ = lv_tick_get();
        tick();
    }

    [[nodiscard]] uint64_t generation() const {
        return generation_;
    }

    [[nodiscard]] bool in_flight() const {
        return timer_ != nullptr;
    }

    static SwitchWatch& watch();

  private:
    void tick() {
        const auto now = facts_for(job_.before.runout_head);
        if (!now) {
            // The filament system went away; the pending restore waits in the store.
            stop();
            not_moved();
            return;
        }
        const roh::SwitchStep step =
            roh::judge_switch(job_.before, *now, job_.target, reply_, lv_tick_elaps(reply_at_),
                              lv_tick_elaps(sent_at_));
        if (step == roh::SwitchStep::Wait) {
            return;
        }
        const bool labelled = roh::label_written(*now, job_.label);
        const int onto = roh::moved_onto(job_.before, *now);
        stop();
        switch (step) {
        case roh::SwitchStep::Wait:
            return;
        case roh::SwitchStep::Done:
            spdlog::info("[RunoutOtherHead] the printer switched head {} to head {}",
                         job_.before.runout_head, job_.target);
            break;
        case roh::SwitchStep::Diverted: {
            spdlog::warn("[RunoutOtherHead] the printer moved head {} to head {}, not {}",
                         job_.before.runout_head, onto, job_.target);
            const std::string onto_name = active_tool_name_label(tool_label(onto), onto);
            const std::string text =
                fmt::format(fmt::runtime(lv_tr("The printer moved the print to {}, not {}.")),
                            onto_name, job_.target_name);
            ToastManager::instance().show(ToastSeverity::WARNING, text.c_str(), 10000);
            break;
        }
        case roh::SwitchStep::Stranded:
            spdlog::warn("[RunoutOtherHead] head {} moved to head {} but the print is still "
                         "paused (reply {})",
                         job_.before.runout_head, job_.target, static_cast<int>(reply_));
            show_stranded();
            break;
        case roh::SwitchStep::Fail:
            fail(*now, labelled);
            break;
        }
        if (step != roh::SwitchStep::Fail) {
            // The print left the empty head: the borrowed label must not match
            // a later runout or the next print.
            if (labelled) {
                send_quiet(job_.plan.clear_label_gcode, "clearing the borrowed label");
            }
            if (job_.hooks.on_moved) {
                job_.hooks.on_moved();
            }
        } else {
            not_moved();
        }
        RunoutOtherHeadModal::settle_pending_restore();
    }

    void show_stranded() {
        show_stranded_dialog(job_.target_name, job_.plan.resume_gcode);
    }

    void fail(const roh::RunoutFacts& now, bool labelled) {
        spdlog::warn("[RunoutOtherHead] the printer did not switch off head {} (reply {})",
                     job_.before.runout_head, static_cast<int>(reply_));
        if (labelled) {
            send_quiet(job_.plan.unlabel_gcode, "putting the empty head's record back");
        }
        const std::string text =
            now.paused
                ? fmt::format(fmt::runtime(lv_tr("{} did not switch to {}. The print is still "
                                                 "paused.")),
                              job_.runout_name, job_.target_name)
                : fmt::format(fmt::runtime(lv_tr("{} did not switch to {}. The print "
                                                 "continued without switching.")),
                              job_.runout_name, job_.target_name);
        ToastManager::instance().show(ToastSeverity::ERROR, text.c_str(), 10000);
    }

    void stop() {
        lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }

    void not_moved() {
        if (job_.hooks.on_not_moved) {
            job_.hooks.on_not_moved();
        }
    }

    lv_timer_t* timer_ = nullptr;
    uint64_t generation_ = 0;
    SwitchJob job_;
    roh::SendReply reply_ = roh::SendReply::None;
    uint32_t sent_at_ = 0;
    uint32_t reply_at_ = 0;
};

SwitchWatch& SwitchWatch::watch() {
    static SwitchWatch w;
    return w;
}

/// The facts the plan is built from. A restore still pending means the
/// setting reads as an earlier switch left it, not as the user set it: plan
/// from the user's own value, so this switch restores it too.
std::optional<roh::RunoutFacts> user_facts_for(int runout_head) {
    auto facts = facts_for(runout_head);
    if (facts && facts->auto_switch_enabled && !roh::read_pending_restore().empty()) {
        facts->auto_switch_enabled = false;
    }
    return facts;
}

/// Re-runs settle_pending_restore() when the firmware's runout switching
/// changes (a reconnect re-reports it). Installed on the first settle.
ObserverGuard& pending_restore_observer() {
    static ObserverGuard guard;
    return guard;
}

/// The send's own timeout. Auto-Replenish heats, primes and cleans the new head
/// inside its RESUME, which outlasts the default.
constexpr uint32_t kSwitchSendTimeoutMs = 150000;

/// Why a plan was refused, for the toast at pick time. Never empty: a tap
/// always gets an answer.
const char* plan_refusal_text(roh::PlanRefusal refusal) {
    switch (refusal) {
    case roh::PlanRefusal::AutoSwitchUnknown:
        return TR_NOOP("The printer has not reported whether Auto-Replenish is on");
    case roh::PlanRefusal::NotPaused:
        return TR_NOOP("The print is no longer paused");
    case roh::PlanRefusal::RunoutHeadNotActive:
        return TR_NOOP("Another toolhead is active");
    case roh::PlanRefusal::PrinterBusy:
        return TR_NOOP("The printer is already resuming the print");
    case roh::PlanRefusal::UnsafeRecord:
        return TR_NOOP("Its filament record cannot be copied");
    case roh::PlanRefusal::Unsupported:
    case roh::PlanRefusal::HeadNotEligible:
    case roh::PlanRefusal::None:
        break;
    }
    return TR_NOOP("That toolhead can no longer take over the print");
}

/// "Different variant (PLA Silk vs PLA Matte)"; empty when nothing differs.
std::string variant_note(const roh::FilamentRecord& now, const roh::FilamentRecord& original) {
    const char* form = now.type == original.type ? lv_tr("Different variant ({} vs {})")
                                                 : lv_tr("Different material ({} vs {})");
    return fmt::format(fmt::runtime(form), filament_name(now), filament_name(original));
}

} // namespace

RunoutOtherHeadModal* RunoutOtherHeadModal::shown_ = nullptr;

RunoutOtherHeadModal::RunoutOtherHeadModal(int runout_head, SwitchHooks hooks)
    : runout_head_(runout_head), hooks_(std::move(hooks)) {
    init_subjects();
}

RunoutOtherHeadModal::~RunoutOtherHeadModal() {
    if (shown_ == this) {
        shown_ = nullptr;
    }
}

void RunoutOtherHeadModal::init_subjects() {
    Subjects& s = subjects();
    if (s.initialized) {
        return;
    }
    UI_MANAGED_SUBJECT_STRING(s.message, s.message_buf, "", "runout_other_head_message", s.manager);
    UI_MANAGED_SUBJECT_INT(s.any_eligible, 0, "runout_other_head_any_eligible", s.manager);
    UI_MANAGED_SUBJECT_INT(s.confirming, 0, "runout_other_head_confirming", s.manager);
    for (int i = 0; i < kRows; ++i) {
        const auto idx = static_cast<size_t>(i);
        const std::string n = std::to_string(i);
        UI_MANAGED_SUBJECT_INT(s.state[idx], kHidden, ("runout_other_head_row_state_" + n).c_str(),
                               s.manager);
        UI_MANAGED_SUBJECT_STRING_N(s.title[idx], s.title_buf[idx].data(), s.title_buf[idx].size(),
                                    "", ("runout_other_head_row_title_" + n).c_str(), s.manager);
        UI_MANAGED_SUBJECT_STRING_N(s.note[idx], s.note_buf[idx].data(), s.note_buf[idx].size(), "",
                                    ("runout_other_head_row_note_" + n).c_str(), s.manager);
        UI_MANAGED_SUBJECT_COLOR(s.color[idx], theme_manager_get_color("text_muted"),
                                 ("runout_other_head_row_color_" + n).c_str(), s.manager);
    }
    lv_xml_register_event_cb(nullptr, "runout_other_head_pick_cb", [](lv_event_t* e) {
        if (shown_ != nullptr) {
            shown_->pick(event_user_int(e).value_or(-1));
        }
    });
    s.initialized = true;
    StaticSubjectRegistry::instance().register_deinit("RunoutOtherHeadModal", [] {
        Subjects& subs = subjects();
        if (!subs.initialized) {
            return;
        }
        subs.manager.deinit_all();
        subs.initialized = false;
    });
}

bool RunoutOtherHeadModal::available(int runout_head) {
    const auto facts = facts_for(runout_head);
    return facts && roh::worth_offering(*facts);
}

bool RunoutOtherHeadModal::show_owned(int runout_head, SwitchHooks hooks) {
    if (!available(runout_head)) {
        return false;
    }
    return Modal::show_owned(std::make_unique<RunoutOtherHeadModal>(runout_head, std::move(hooks)),
                             lv_screen_active());
}

bool RunoutOtherHeadModal::show_stranded(int onto) {
    AmsBackend* backend = AmsState::instance().get_backend();
    const std::string resume =
        backend != nullptr ? roh::resume_on_gcode(backend->get_type(), onto) : std::string{};
    if (resume.empty()) {
        return false;
    }
    show_stranded_dialog(active_tool_name_label(tool_label(onto), onto), resume);
    return true;
}

void RunoutOtherHeadModal::set_confirming(bool confirming) {
    lv_subject_set_int(&subjects().confirming, confirming ? 1 : 0);
}

void RunoutOtherHeadModal::settle_pending_restore() {
    if (!pending_restore_observer() && AmsState::instance().get_endless_spool_on_subject()) {
        pending_restore_observer() = helix::ui::observe<int>(
            AmsState::instance().get_endless_spool_on_subject(), &pending_restore_observer(),
            [](ObserverGuard*, int) { settle_pending_restore(); },
            AmsState::instance().get_subjects_lifetime());
        StaticSubjectRegistry::instance().register_deinit(
            "RunoutOtherHeadPendingRestore", [] { pending_restore_observer().reset(); });
    }
    const std::string pending = roh::read_pending_restore();
    const auto facts = facts_for(0);
    switch (roh::pending_restore_step(!pending.empty(), SwitchWatch::watch().in_flight(),
                                      facts ? facts->auto_switch_enabled : std::nullopt)) {
    case roh::PendingRestoreStep::Keep:
        return;
    case roh::PendingRestoreStep::Send:
        spdlog::info("[RunoutOtherHead] putting back a setting a switch changed: {}", pending);
        send_quiet(pending, "restoring Auto-Replenish");
        return;
    case roh::PendingRestoreStep::Clear:
        spdlog::info("[RunoutOtherHead] the setting a switch changed is back");
        roh::write_pending_restore({});
        return;
    }
}

void RunoutOtherHeadModal::on_show() {
    shown_ = this;
    wire_ok_button("btn_primary");
    set_confirming(false);
    publish();
}

void RunoutOtherHeadModal::publish() {
    Subjects& s = subjects();
    const auto facts = facts_for(runout_head_);
    AmsBackend* backend = AmsState::instance().get_backend();
    verdicts_ = facts && backend != nullptr ? roh::assess(backend->get_type(), *facts)
                                            : std::vector<roh::HeadVerdict>{};

    const std::string runout_name = active_tool_name_label(tool_label(runout_head_), runout_head_);
    const std::string message =
        facts && facts->original
            ? fmt::format(fmt::runtime(lv_tr("{} ran out of {}. Pick a loaded toolhead to finish "
                                             "the print. It keeps the temperatures sliced for the "
                                             "original filament.")),
                          runout_name, filament_name(*facts->original))
            : fmt::format(fmt::runtime(lv_tr("{} ran out, and the printer has not said what it "
                                             "was printing.")),
                          runout_name);
    lv_subject_set_string(&s.message, message.c_str());

    bool any = false;
    for (int i = 0; i < kRows; ++i) {
        const auto idx = static_cast<size_t>(i);
        int state = kHidden;
        std::string title;
        std::string note;
        lv_color_t color = theme_manager_get_color("text_muted");
        for (const auto& v : verdicts_) {
            if (v.head != i || v.refusal == roh::HeadRefusal::IsRunoutHead) {
                continue;
            }
            const roh::HeadFacts* h = head_facts(*facts, i);
            title = active_tool_name_label(tool_label(i), i);
            if (h != nullptr && h->record) {
                title += fmt::format(" {} {}", kUnitSeparator, filament_name(*h->record));
                color = first_color(*h->record);
            }
            if (!v.eligible()) {
                state = kRefused;
                note = lv_tr(roh::refusal_text(v.refusal));
            } else if (v.variant_differs && h != nullptr && h->record && facts->original) {
                state = kWarn;
                note = variant_note(*h->record, *facts->original);
            } else if (v.color_differs) {
                state = kWarn;
                note = lv_tr("Different color");
            } else {
                state = kEligible;
                note = lv_tr("Same filament");
            }
        }
        any = any || state == kEligible || state == kWarn;
        lv_subject_set_string(&s.title[idx], title.c_str());
        lv_subject_set_string(&s.note[idx], note.c_str());
        lv_subject_set_color(&s.color[idx], color);
        lv_subject_set_int(&s.state[idx], state);
    }
    lv_subject_set_int(&s.any_eligible, any ? 1 : 0);
}

void RunoutOtherHeadModal::pick(int row) {
    const auto facts = user_facts_for(runout_head_);
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!facts || backend == nullptr) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("The printer is not reporting its toolheads"));
        return;
    }
    const roh::ContinuePlan plan = roh::build_plan(backend->get_type(), *facts, row);
    if (!plan.ok()) {
        spdlog::info("[RunoutOtherHead] head {} refused ({})", row, static_cast<int>(plan.refusal));
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr(plan_refusal_text(plan.refusal)));
        publish(); // the printer moved on; show why
        return;
    }
    roh::HeadVerdict verdict;
    for (const auto& v : roh::assess(backend->get_type(), *facts)) {
        if (v.head == row) {
            verdict = v;
        }
    }
    std::string message =
        fmt::format(fmt::runtime(lv_tr("Continue the print on {}? The printer moves the rest of "
                                       "the print to it, heats it and resumes.")),
                    active_tool_name_label(tool_label(row), row));
    const roh::HeadFacts* chosen = head_facts(*facts, row);
    if (verdict.variant_differs && chosen != nullptr && chosen->record && facts->original) {
        message += " ";
        message += variant_note(*chosen->record, *facts->original);
        message += lv_tr(": the rest prints at the original's temperature.");
    }
    if (verdict.color_differs) {
        message += " ";
        message += lv_tr("Its filament is a different color.");
    }
    if (plan.continues_on != row) {
        message += " ";
        message += fmt::format(
            fmt::runtime(lv_tr("{} holds the same filament, and the printer uses that one.")),
            active_tool_name_label(tool_label(plan.continues_on), plan.continues_on));
    }
    if (plan.switches_auto_switch_on) {
        message += " ";
        message += lv_tr("Auto-Replenish is turned on for this switch and off again after.");
    }
    const bool warn = verdict.color_differs || verdict.variant_differs;
    ConfirmOptions options;
    options.owner_token = lifetime_.token();
    // Every row waits on this answer; one that is not Continue frees them.
    options.on_cancel = [] { set_confirming(false); };
    options.on_dismiss = [] { set_confirming(false); };
    set_confirming(true);
    modal_confirm(
        lv_tr("Continue on Another Toolhead"), message.c_str(),
        warn ? ModalSeverity::Warning : ModalSeverity::Info, lv_tr("Continue"),
        [this, row]() { start(row); }, options);
}

void RunoutOtherHeadModal::start(int head) {
    set_confirming(false);
    // Re-plan at the moment of sending: the pause or the heads may have moved
    // while the confirmation was up.
    const auto facts = user_facts_for(runout_head_);
    AmsBackend* backend = AmsState::instance().get_backend();
    IMoonrakerAPI* api = get_moonraker_api();
    if (!facts || backend == nullptr || api == nullptr) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("The printer is not reporting its toolheads"));
        return;
    }
    const roh::ContinuePlan plan = roh::build_plan(backend->get_type(), *facts, head);
    if (!plan.ok()) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr(plan_refusal_text(plan.refusal)));
        publish();
        return;
    }
    std::string script;
    for (const auto& line : plan.gcode) {
        script += script.empty() ? line : "\n" + line;
    }
    spdlog::info("[RunoutOtherHead] continuing the print: runout head {} -> head {}", runout_head_,
                 plan.continues_on);
    // Stored before sending: a restart or a lost link while the switch runs
    // must not leave the setting changed.
    if (!plan.restore_gcode.empty()) {
        roh::write_pending_restore(plan.restore_gcode);
    }
    const std::string target_name =
        active_tool_name_label(tool_label(plan.continues_on), plan.continues_on);
    SwitchWatch& watch = SwitchWatch::watch();
    watch.start(SwitchJob{
        .before = *facts,
        .label = *head_facts(*facts, head)->record,
        .target = plan.continues_on,
        .plan = plan,
        .runout_name = active_tool_name_label(tool_label(runout_head_), runout_head_),
        .target_name = target_name,
        .hooks = hooks_,
    });
    if (hooks_.on_sent) {
        hooks_.on_sent(target_name);
    }
    const uint64_t generation = watch.generation();
    const auto reply = [generation](roh::SendReply r) {
        queue_update("RunoutOtherHead::reply",
                     [generation, r] { SwitchWatch::watch().note_reply(generation, r); });
    };
    // ERROR_OWNERSHIP_OK: SwitchWatch reports the outcome, and a timed-out
    // send may still switch, so no failure is reported from the reply alone.
    api->execute_gcode(
        script, [reply]() { reply(roh::SendReply::Ok); },
        [reply](const MoonrakerError& err) {
            spdlog::warn("[RunoutOtherHead] switch send failed: {}", err.message);
            reply(err.is_transport_loss() ? roh::SendReply::TransportLost
                                          : roh::SendReply::Refused);
        },
        kSwitchSendTimeoutMs);
    hide();
}

} // namespace helix::ui
