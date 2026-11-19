// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_runout_other_head_modal.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fmt/format.h>
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
        message += lv_tr("Auto-Replenish is turned on for this switch.");
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
    api->execute_gcode(script, nullptr, [](const MoonrakerError& err) {
        spdlog::warn("[RunoutOtherHead] switch refused: {}", err.message);
        queue_update("RunoutOtherHead::error", [] {
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("The printer did not switch toolheads"));
        });
    });
    hide();
}

} // namespace helix::ui
