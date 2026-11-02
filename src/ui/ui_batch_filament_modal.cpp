// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_batch_filament_modal.h"

#include "ui_error_reporting.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::ui {

namespace {

// The opening direction's copy, published before the picker's XML is created
// so the freshly bound title and primary button read the right strings.
// Process-wide rather than a member (same shape as the sidebar's gating
// subjects): the XML tree bound to these outlives each one-shot instance's
// exit animation, and an instance-owned subject would be deinit'd out from
// under a live binding.
lv_subject_t s_title_text{};
lv_subject_t s_action_text{};
std::string s_title_owned;
std::string s_action_owned;
bool s_direction_subjects_initialized = false;
SubjectManager s_subjects;

void init_direction_subjects() {
    if (s_direction_subjects_initialized) {
        return;
    }
    lv_subject_init_pointer(&s_title_text, nullptr);
    lv_subject_init_pointer(&s_action_text, nullptr);
    s_subjects.publish("batch_filament_title_text", &s_title_text);
    s_subjects.publish("batch_filament_action_text", &s_action_text);
    s_direction_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("BatchFilamentModalDirection", []() {
        if (s_direction_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_direction_subjects_initialized = false;
        }
    });
}

} // namespace

// filament_op_eligibility_reason() returns runtime-chosen strings, so the
// extractor cannot see them at the lv_tr() call site in dispatch(). This never
// runs; it lists every reason as a literal key.
// clang-format off
[[maybe_unused]] static void eligibility_reason_translation_hints_() {
    (void)lv_tr("empty"); (void)lv_tr("already loaded"); (void)lv_tr("not loaded");
    (void)lv_tr("feeder not in automatic mode"); (void)lv_tr("filament sensor disabled");
    (void)lv_tr("busy"); (void)lv_tr("feeder error");
}
// clang-format on

bool BatchFilamentModal::show_owned(HeadPickOp op, Confirm on_confirm) {
    init_direction_subjects();

    // One lv_tr() per literal: the extractor cannot see keys chosen inside a
    // ternary.
    const char* title = lv_tr("Load into which tool?");
    const char* action = lv_tr("Load");
    switch (op) {
    case HeadPickOp::Load:
        break;
    case HeadPickOp::Unload:
        title = lv_tr("Unload which tool?");
        action = lv_tr("Unload");
        break;
    case HeadPickOp::Mount:
        title = lv_tr("Mount which tool?");
        action = lv_tr("Mount");
        break;
    case HeadPickOp::SeatByHand:
        title = lv_tr("Seat which tool by hand?");
        action = lv_tr("Seat");
        break;
    }
    // Copy first, publish second: set_pointer fires observers synchronously,
    // and they read the buffer, so the owned strings must already hold the new
    // text when they do.
    s_title_owned = title;
    s_action_owned = action;
    lv_subject_set_pointer(&s_title_text, s_title_owned.data());
    lv_subject_set_pointer(&s_action_text, s_action_owned.data());

    auto modal = std::make_unique<BatchFilamentModal>();
    modal->op_ = op;
    modal->on_confirm_ = std::move(on_confirm);
    if (!modal->show(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    ModalStack::instance().assume_ownership(modal->backdrop(), std::move(modal));
    return true;
}

std::vector<int> BatchFilamentModal::selected_slots(const std::vector<std::string>& keys) {
    std::vector<int> slots;
    slots.reserve(keys.size());
    for (const auto& key : keys) {
        const auto slot = helix::text_io::parse_leading<int>(key);
        if (!slot) {
            spdlog::warn("[BatchFilamentModal] non-numeric row key '{}' - ignoring selection", key);
            return {};
        }
        slots.push_back(*slot);
    }
    return slots;
}

namespace {

/// What is in the lane and where it stands, or empty when nothing can be said.
std::string contents_suffix(const SlotInfo& info, std::optional<bool> present, bool at_toolhead) {
    if (!info.material.empty()) {
        const char* where = at_toolhead ? lv_tr("loaded") : lv_tr("ready to load");
        return " (" + info.material + " - " + where + ")"; // material: no i18n
    }
    if (present && !*present) {
        return std::string(" (") + lv_tr("Empty") + ")";
    }
    return {};
}

} // namespace

std::string BatchFilamentModal::row_label(LaneNoun noun, int slot, const SlotInfo& info,
                                          std::optional<bool> present, bool at_toolhead) {
    return lane_label(noun, slot) + contents_suffix(info, present, at_toolhead);
}

bool BatchFilamentModal::row_at_toolhead(const AmsBackend& backend, int head, int slot) {
    if (slot < 0) {
        return false;
    }
    // One slot per head: a head known to hold filament holds this slot's.
    return backend.can_unload_from_toolhead(slot) ||
           (backend.get_topology() == PathTopology::PARALLEL &&
            backend.head_has_filament(head).value_or(false));
}

std::string BatchFilamentModal::head_row_label(const HeadRowText& text) {
    std::string label = lane_label(text.head_noun, text.head);
    if (text.show_slot && text.slot >= 0) {
        label += " \u00b7 " + lane_label(text.slot_noun, text.slot);
    }
    if (!text.reason.empty()) {
        return label + " (" + text.reason + ")";
    }
    if (text.slot < 0) {
        return label;
    }
    return label + contents_suffix(text.info, text.present, text.at_toolhead);
}

BatchFilamentModal::EligibilitySift
BatchFilamentModal::sift_eligible(const std::vector<int>& selected,
                                  const std::vector<AmsBackend::FilamentOpEligibility>& per_slot) {
    EligibilitySift out;
    out.eligible.reserve(selected.size());
    for (int slot : selected) {
        const auto idx = static_cast<size_t>(slot);
        // total_slots can shrink between the picker opening and the press;
        // a slot past the eligibility table has no value to read and no
        // refusal to name.
        if (idx >= per_slot.size()) {
            continue;
        }
        if (per_slot[idx] == AmsBackend::FilamentOpEligibility::Eligible) {
            out.eligible.push_back(slot);
        } else if (out.dropped < 0) {
            out.dropped = slot;
            out.drop_reason = per_slot[idx];
        }
    }
    return out;
}

void BatchFilamentModal::on_show() {
    // btn_primary hands the chosen rows over; btn_secondary is a real Cancel:
    // Modal::on_cancel() just hides.
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    AmsBackend* backend = AmsState::instance().get_backend();
    lv_obj_t* container = find_widget("batch_multiselect");
    if (!backend || !container) {
        spdlog::warn("[HeadPicker] {} — picker left empty",
                     backend ? "batch_multiselect not found" : "no backend");
        return;
    }

    const FilamentHeadView view = read_filament_head_view(backend, get_printer_state());
    rows_ = decide_head_picker_rows(view, op_);

    HeadRowText text;
    text.head_noun = backend->tool_noun();
    text.slot_noun = backend->lane_noun();
    std::vector<MultiSelectItem> items;
    items.reserve(rows_.size());
    for (const HeadPickRow& row : rows_) {
        text.head = row.head;
        text.slot = row.slot;
        // A slot is worth naming where several can feed the head, or where
        // the machine has one head and the slot is the whole answer.
        text.show_slot = !single_select(op_) && (view.heads.size() == 1 || row.slot != row.head);
        text.reason = row.enabled ? std::string() : row.reason;
        text.info = row.slot >= 0 ? backend->get_slot_info(row.slot) : SlotInfo{};
        text.present = row.slot >= 0 ? slot_presence(text.info) : std::nullopt;
        text.at_toolhead = row_at_toolhead(*backend, row.head, row.slot);
        items.push_back(
            {std::to_string(row.head), head_row_label(text), row.preselected, row.enabled});
    }
    multiselect_.attach(container);
    multiselect_.set_max_selected(
        single_select(op_) ? 1 : static_cast<size_t>(std::max(view.max_heads_per_op, 1)));
    multiselect_.set_items(items);
    spdlog::debug("[HeadPicker] {} row(s) populated", items.size());
}

void BatchFilamentModal::on_ok() {
    const std::vector<int> heads = selected_slots(multiselect_.get_selected_keys());
    std::vector<HeadPickRow> chosen;
    for (int head : heads) {
        for (const HeadPickRow& row : rows_) {
            if (row.head == head && row.enabled) {
                chosen.push_back(row);
            }
        }
    }
    if (chosen.empty()) {
        NOTIFY_WARNING("{}", lv_tr("Select at least one"));
        return; // keep the picker open: nothing was chosen
    }
    // Moved out before hide(): hiding can free this instance.
    Confirm confirm = std::move(on_confirm_);
    hide();
    if (confirm) {
        confirm(chosen);
    }
}

bool BatchFilamentModal::dispatch_batch(AmsBackend& backend, const std::vector<int>& slots,
                                        bool load) {
    std::vector<AmsBackend::FilamentOpEligibility> per_slot;
    const AmsSystemInfo sys = backend.get_system_info();
    per_slot.reserve(static_cast<size_t>(sys.total_slots));
    for (int slot = 0; slot < sys.total_slots; ++slot) {
        per_slot.push_back(backend.slot_op_eligibility(slot, load));
    }
    const EligibilitySift sift = sift_eligible(slots, per_slot);
    if (sift.dropped >= 0) {
        // Name the first head we are dropping and why; a batch that silently
        // shrinks is worse than one that explains itself.
        NOTIFY_WARNING("{} {}: {}", lv_tr("Skipped"), lane_label(backend.lane_noun(), sift.dropped),
                       lv_tr(filament_op_eligibility_reason(sift.drop_reason)));
    }
    if (sift.eligible.empty()) {
        return false;
    }

    spdlog::info("[HeadPicker] {} batch on {} slot(s)", load ? "Load" : "Unload",
                 sift.eligible.size());
    AmsError error = load ? backend.load_filament_batch(sift.eligible)
                          : backend.unload_filament_batch(sift.eligible);
    if (!error.success()) {
        helix::ui::notify_ams_error(error, load ? lv_tr("Batch load failed")
                                                : lv_tr("Batch unload failed"));
        return false;
    }
    return true;
}

} // namespace helix::ui
