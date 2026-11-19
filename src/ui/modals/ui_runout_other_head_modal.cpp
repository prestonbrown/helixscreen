// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_runout_other_head_modal.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
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
#include <memory>
#include <string>

namespace helix::ui {

namespace roh = helix::runout_other_head;

namespace {

constexpr int kRows = 4;

/// Row states the XML binds.
enum RowState : int { kHidden = 0, kEligible = 1, kColorDiffers = 2, kRefused = 3 };

struct Subjects {
    helix::Subject message;
    char message_buf[256] = {};
    helix::Subject any_eligible;
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

/// Watches one sent switch until the printer shows it or it is clearly not
/// coming. The firmware declines a switch by doing nothing, so a clean reply
/// proves nothing: only the pause ending or the routing changing does.
class SwitchWatch {
  public:
    ~SwitchWatch() {
        stop();
    }

    void start(roh::RunoutFacts before, std::string restore_gcode, std::string runout_name,
               std::string chosen_name) {
        stop();
        ++generation_;
        before_ = std::move(before);
        restore_gcode_ = std::move(restore_gcode);
        runout_name_ = std::move(runout_name);
        chosen_name_ = std::move(chosen_name);
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

    static SwitchWatch& watch();

  private:
    void tick() {
        const auto now = facts_for(before_.runout_head);
        if (!now) {
            stop();
            return;
        }
        switch (roh::judge_switch(before_, *now, reply_, lv_tick_elaps(reply_at_),
                                  lv_tick_elaps(sent_at_))) {
        case roh::SwitchVerdict::Pending:
            return;
        case roh::SwitchVerdict::Switched:
            spdlog::info("[RunoutOtherHead] the printer switched off head {}", before_.runout_head);
            stop();
            return;
        case roh::SwitchVerdict::Failed:
            fail();
            return;
        }
    }

    void fail() {
        spdlog::warn("[RunoutOtherHead] the printer did not switch off head {} (reply {})",
                     before_.runout_head, static_cast<int>(reply_));
        // The script stops at its first error, so a setting it changed for
        // the switch goes back on its own.
        if (!restore_gcode_.empty()) {
            if (IMoonrakerAPI* api = get_moonraker_api()) {
                api->execute_gcode(restore_gcode_, nullptr, [](const MoonrakerError& err) {
                    spdlog::warn("[RunoutOtherHead] restoring Auto-Replenish failed: {}",
                                 err.message);
                });
            }
        }
        const std::string text = fmt::format(
            fmt::runtime(lv_tr("{} did not switch to {}. The print is still paused, and {} now "
                               "carries the filament label of {}.")),
            runout_name_, chosen_name_, runout_name_, chosen_name_);
        ToastManager::instance().show(ToastSeverity::ERROR, text.c_str(), 10000);
        stop();
    }

    void stop() {
        lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }

    lv_timer_t* timer_ = nullptr;
    uint64_t generation_ = 0;
    roh::RunoutFacts before_;
    std::string restore_gcode_;
    std::string runout_name_;
    std::string chosen_name_;
    roh::SendReply reply_ = roh::SendReply::None;
    uint32_t sent_at_ = 0;
    uint32_t reply_at_ = 0;
};

SwitchWatch& SwitchWatch::watch() {
    static SwitchWatch w;
    return w;
}

/// The send's own timeout. Auto-Replenish heats, primes and cleans the new head
/// inside its RESUME, which outlasts the default.
constexpr uint32_t kSwitchSendTimeoutMs = 150000;

} // namespace

RunoutOtherHeadModal* RunoutOtherHeadModal::shown_ = nullptr;

RunoutOtherHeadModal::RunoutOtherHeadModal(int runout_head) : runout_head_(runout_head) {
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
    return facts_for(runout_head).has_value();
}

bool RunoutOtherHeadModal::show_owned(int runout_head) {
    if (!available(runout_head)) {
        return false;
    }
    return Modal::show_owned(std::make_unique<RunoutOtherHeadModal>(runout_head),
                             lv_screen_active());
}

void RunoutOtherHeadModal::on_show() {
    shown_ = this;
    wire_ok_button("btn_primary");
    publish();
}

void RunoutOtherHeadModal::publish() {
    Subjects& s = subjects();
    const auto facts = facts_for(runout_head_);
    verdicts_ = facts ? roh::assess(*facts) : std::vector<roh::HeadVerdict>{};

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
            } else if (v.color_differs) {
                state = kColorDiffers;
                note = lv_tr("Different color");
            } else {
                state = kEligible;
                note = lv_tr("Same filament");
            }
        }
        any = any || state == kEligible || state == kColorDiffers;
        lv_subject_set_string(&s.title[idx], title.c_str());
        lv_subject_set_string(&s.note[idx], note.c_str());
        lv_subject_set_color(&s.color[idx], color);
        lv_subject_set_int(&s.state[idx], state);
    }
    lv_subject_set_int(&s.any_eligible, any ? 1 : 0);
}

void RunoutOtherHeadModal::pick(int row) {
    const auto facts = facts_for(runout_head_);
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!facts || backend == nullptr) {
        return;
    }
    const roh::ContinuePlan plan = roh::build_plan(backend->get_type(), *facts, row);
    if (!plan.ok()) {
        spdlog::info("[RunoutOtherHead] head {} refused ({})", row, static_cast<int>(plan.refusal));
        if (plan.refusal == roh::PlanRefusal::AutoSwitchUnknown) {
            ToastManager::instance().show(
                ToastSeverity::WARNING,
                lv_tr("The printer has not reported whether Auto-Replenish is on"));
        }
        publish(); // the printer moved on; show why
        return;
    }
    bool color_differs = false;
    for (const auto& v : roh::assess(*facts)) {
        if (v.head == row) {
            color_differs = v.color_differs;
        }
    }
    std::string message =
        fmt::format(fmt::runtime(lv_tr("Continue the print on {}? The printer moves the rest of "
                                       "the print to it, heats it and resumes.")),
                    active_tool_name_label(tool_label(row), row));
    if (color_differs) {
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
    ConfirmOptions options;
    options.owner_token = lifetime_.token();
    modal_confirm(
        lv_tr("Continue on Another Toolhead"), message.c_str(),
        color_differs ? ModalSeverity::Warning : ModalSeverity::Info, lv_tr("Continue"),
        [this, row]() { start(row); }, options);
}

void RunoutOtherHeadModal::start(int head) {
    // Re-plan at the moment of sending: the pause or the heads may have moved
    // while the confirmation was up.
    const auto facts = facts_for(runout_head_);
    AmsBackend* backend = AmsState::instance().get_backend();
    IMoonrakerAPI* api = get_moonraker_api();
    if (!facts || backend == nullptr || api == nullptr) {
        return;
    }
    const roh::ContinuePlan plan = roh::build_plan(backend->get_type(), *facts, head);
    if (!plan.ok()) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("That toolhead can no longer take over the print"));
        publish();
        return;
    }
    std::string script;
    for (const auto& line : plan.gcode) {
        script += script.empty() ? line : "\n" + line;
    }
    spdlog::info("[RunoutOtherHead] continuing the print: runout head {} -> head {}", runout_head_,
                 plan.continues_on);
    SwitchWatch& watch = SwitchWatch::watch();
    watch.start(*facts, plan.restore_gcode,
                active_tool_name_label(tool_label(runout_head_), runout_head_),
                active_tool_name_label(tool_label(plan.continues_on), plan.continues_on));
    const uint64_t generation = watch.generation();
    const auto reply = [generation](roh::SendReply r) {
        queue_update("RunoutOtherHead::reply",
                     [generation, r] { SwitchWatch::watch().note_reply(generation, r); });
    };
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
