// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_batch_filament_modal.h"

#include "ui_error_reporting.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "filament_op_slot_resolver.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_subject_registry.h"

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
lv_subject_t s_title_text;
lv_subject_t s_action_text;
std::string s_title_owned;
std::string s_action_owned;
bool s_direction_subjects_initialized = false;

void init_direction_subjects() {
    if (s_direction_subjects_initialized) {
        return;
    }
    lv_subject_init_pointer(&s_title_text, nullptr);
    lv_subject_init_pointer(&s_action_text, nullptr);
    lv_xml_register_subject(nullptr, "batch_filament_title_text", &s_title_text);
    lv_xml_register_subject(nullptr, "batch_filament_action_text", &s_action_text);
    s_direction_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("BatchFilamentModalDirection", []() {
        if (s_direction_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_title_text);
            lv_subject_deinit(&s_action_text);
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

bool BatchFilamentModal::show_owned(bool for_load) {
    init_direction_subjects();

    // One lv_tr() per literal: the extractor cannot see keys chosen inside a
    // ternary, and every key already exists in translations.
    const char* title = for_load ? lv_tr("Load filament") : lv_tr("Unload filament");
    const char* action = for_load ? lv_tr("Load") : lv_tr("Unload");
    // Copy first, publish second: set_pointer fires observers synchronously,
    // and they read the buffer, so the owned strings must already hold the new
    // text when they do.
    s_title_owned = title;
    s_action_owned = action;
    lv_subject_set_pointer(&s_title_text, s_title_owned.data());
    lv_subject_set_pointer(&s_action_text, s_action_owned.data());

    auto modal = std::make_unique<BatchFilamentModal>();
    modal->for_load_ = for_load;
    if (!modal->show(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    ModalStack::instance().assume_ownership(modal->backdrop(), std::move(modal));
    return true;
}

bool BatchFilamentModal::head_can_act(std::optional<bool> at_toolhead,
                                      std::optional<bool> lane_presence, bool for_load) {
    const bool loaded_here = at_toolhead.value_or(false);
    if (!for_load) {
        return loaded_here;
    }
    return !loaded_here && lane_presence.value_or(true);
}

std::vector<bool>
BatchFilamentModal::prefill_selection(const std::vector<std::optional<bool>>& at_toolhead,
                                      const std::vector<std::optional<bool>>& lane_presence,
                                      bool for_load) {
    std::vector<bool> ticked;
    ticked.reserve(at_toolhead.size());
    for (size_t i = 0; i < at_toolhead.size(); ++i) {
        ticked.push_back(head_can_act(
            at_toolhead[i], i < lane_presence.size() ? lane_presence[i] : std::nullopt, for_load));
    }
    return ticked;
}

bool BatchFilamentModal::any_head_for_direction(
    const std::vector<std::optional<bool>>& at_toolhead,
    const std::vector<std::optional<bool>>& lane_presence, bool for_load) {
    const std::vector<bool> ticked = prefill_selection(at_toolhead, lane_presence, for_load);
    return std::any_of(ticked.begin(), ticked.end(), [](bool tick) { return tick; });
}

std::vector<int> BatchFilamentModal::selected_slots(const std::vector<std::string>& keys) {
    std::vector<int> slots;
    slots.reserve(keys.size());
    for (const auto& key : keys) {
        slots.push_back(std::stoi(key));
    }
    return slots;
}

std::string BatchFilamentModal::row_label(LaneNoun noun, int slot, const SlotInfo& info,
                                          std::optional<bool> present, bool at_toolhead) {
    const std::string lane = lane_label(noun, slot);
    if (!info.material.empty()) {
        const char* where = at_toolhead ? lv_tr("loaded") : lv_tr("ready to load");
        return lane + " (" + info.material + " - " + where + ")"; // material: no i18n
    }
    if (present && !*present) {
        return lane + " (" + lv_tr("Empty") + ")";
    }
    return lane;
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

BatchFilamentModal::BatchRowSource BatchFilamentModal::collect_rows(const AmsBackend& backend) {
    BatchRowSource rows;
    const int total = backend.get_system_info().total_slots;
    rows.slots.reserve(static_cast<size_t>(total));
    rows.lane_presence.reserve(static_cast<size_t>(total));
    rows.at_toolhead.reserve(static_cast<size_t>(total));
    for (int slot = 0; slot < total; ++slot) {
        rows.slots.push_back(backend.get_slot_info(slot));
        rows.lane_presence.push_back(slot_presence(rows.slots.back()));
        rows.at_toolhead.push_back(backend.can_unload_from_toolhead(slot));
    }
    return rows;
}

void BatchFilamentModal::on_show() {
    // btn_primary runs the direction this picker was opened in; btn_secondary
    // is a real Cancel: Modal::on_cancel() just hides.
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    AmsBackend* backend = AmsState::instance().get_backend();
    lv_obj_t* container = find_widget("batch_multiselect");
    if (!backend || !container) {
        spdlog::warn("[BatchFilamentModal] {} — picker left empty",
                     backend ? "batch_multiselect not found" : "no backend");
        return;
    }

    const BatchRowSource rows = collect_rows(*backend);
    const std::vector<bool> ticked =
        prefill_selection(rows.at_toolhead, rows.lane_presence, for_load_);

    std::vector<MultiSelectItem> items;
    items.reserve(rows.slots.size());
    for (size_t slot = 0; slot < rows.slots.size(); ++slot) {
        items.push_back(
            {std::to_string(slot),
             row_label(backend->lane_noun(), static_cast<int>(slot), rows.slots[slot],
                       rows.lane_presence[slot], rows.at_toolhead[slot].value_or(false)),
             ticked[slot]});
    }
    multiselect_.attach(container);
    multiselect_.set_items(items);
    spdlog::debug("[BatchFilamentModal] {} row(s) populated", items.size());
}

void BatchFilamentModal::on_ok() {
    dispatch(for_load_);
}

void BatchFilamentModal::dispatch(bool load) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("Multi-Filament System not available"));
        hide();
        return;
    }

    const std::vector<int> slots = selected_slots(multiselect_.get_selected_keys());
    if (slots.empty()) {
        NOTIFY_WARNING("{}", lv_tr("Select at least one"));
        return; // keep the picker open — nothing was dispatched
    }

    std::vector<AmsBackend::FilamentOpEligibility> per_slot;
    const AmsSystemInfo sys = backend->get_system_info();
    per_slot.reserve(static_cast<size_t>(sys.total_slots));
    for (int slot = 0; slot < sys.total_slots; ++slot) {
        per_slot.push_back(backend->slot_op_eligibility(slot, load));
    }
    const EligibilitySift sift = sift_eligible(slots, per_slot);
    if (sift.dropped >= 0) {
        // Name the first head we are dropping and why; a batch that silently
        // shrinks is worse than one that explains itself.
        NOTIFY_WARNING("{} {}: {}", lv_tr("Skipped"),
                       lane_label(backend->lane_noun(), sift.dropped),
                       lv_tr(filament_op_eligibility_reason(sift.drop_reason)));
    }
    if (sift.eligible.empty()) {
        return; // keep the picker open — nothing was dispatched
    }

    spdlog::info("[BatchFilamentModal] {} batch on {} slot(s)", load ? "Load" : "Unload",
                 sift.eligible.size());
    AmsError error = load ? backend->load_filament_batch(sift.eligible)
                          : backend->unload_filament_batch(sift.eligible);
    if (!error.success()) {
        helix::ui::notify_ams_error(error, load ? lv_tr("Batch load failed")
                                                : lv_tr("Batch unload failed"));
    }
    hide();
}

} // namespace helix::ui
