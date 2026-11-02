// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_preset_config_modal.h"

#include "ui_event_safety.h"
#include "ui_picker_option_row.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "favorite_macro_config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "printer_discovery.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {

namespace {

HELIX_PSRAM_BSS lv_subject_t s_slot_subject{};
HELIX_PSRAM_BSS lv_subject_t s_slot_text_subjects[kToolheadPresetSlotCount]{};
HELIX_PSRAM_BSS char s_slot_text_bufs[kToolheadPresetSlotCount][64];
bool s_registered = false;
SubjectManager s_subjects;

constexpr const char* kSlotTextNames[kToolheadPresetSlotCount] = {
    "toolhead_preset_cfg_slot_0_text", "toolhead_preset_cfg_slot_1_text",
    "toolhead_preset_cfg_slot_2_text"};

void add_row(lv_obj_t* list, const std::string& label, bool selected,
             std::function<void()> on_pick) {
    ui::create_picker_option_row(list, label.c_str(), selected, std::move(on_pick));
}

bool same_choice(const ToolheadPresetSlot& a, const ToolheadPresetSlot& b) {
    if (a.kind != b.kind)
        return false;
    return a.kind != ToolheadPresetSlot::Kind::Position || a.position == b.position;
}

void slot_tab_cb(lv_event_t* /*e*/, std::size_t index) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadPresetConfigModal] slot_tab_cb");
    if (auto* modal = ToolheadPresetConfigModal::active())
        modal->select_slot(index);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

ToolheadPresetConfigModal* ToolheadPresetConfigModal::s_active_ = nullptr;

void register_toolhead_preset_config() {
    if (s_registered)
        return;
    lv_subject_init_int(&s_slot_subject, 0);
    s_subjects.publish("toolhead_preset_cfg_slot", &s_slot_subject);
    for (std::size_t i = 0; i < kToolheadPresetSlotCount; ++i) {
        s_slot_text_bufs[i][0] = '\0';
        lv_subject_init_string(&s_slot_text_subjects[i], s_slot_text_bufs[i], nullptr,
                               sizeof(s_slot_text_bufs[i]), "");
        s_subjects.publish(kSlotTextNames[i], &s_slot_text_subjects[i]);
    }
    // Registered once with the widget factories, so it outlives a printer switch.
    StaticSubjectRegistry::instance().register_deinit(
        "ToolheadPresetConfigSubjects",
        []() {
            s_subjects.deinit_all();
            s_registered = false;
        },
        StaticSubjectRegistry::Lifetime::Process);

    lv_xml_register_event_cb(nullptr, "toolhead_preset_cfg_close_cb", [](lv_event_t*) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadPresetConfigModal] close_cb");
        if (auto* modal = ToolheadPresetConfigModal::active())
            modal->hide();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "toolhead_preset_cfg_slot_0_cb",
                             [](lv_event_t* e) { slot_tab_cb(e, 0); });
    lv_xml_register_event_cb(nullptr, "toolhead_preset_cfg_slot_1_cb",
                             [](lv_event_t* e) { slot_tab_cb(e, 1); });
    lv_xml_register_event_cb(nullptr, "toolhead_preset_cfg_slot_2_cb",
                             [](lv_event_t* e) { slot_tab_cb(e, 2); });
    s_registered = true;
}

ToolheadPresetConfigModal::ToolheadPresetConfigModal(ToolheadPresetSlots slots,
                                                     ChangeCallback on_change)
    : slots_(std::move(slots)), on_change_(std::move(on_change)) {
    s_active_ = this;
}

ToolheadPresetConfigModal::~ToolheadPresetConfigModal() {
    if (s_active_ == this)
        s_active_ = nullptr;
}

std::vector<ToolheadPresetSlot> ToolheadPresetConfigModal::choices() {
    std::vector<ToolheadPresetSlot> out;
    for (const auto& p : kMotionPresetTokens) {
        ToolheadPresetSlot slot;
        slot.kind = ToolheadPresetSlot::Kind::Position;
        slot.position = p.preset;
        out.push_back(slot);
    }
    for (const auto kind : {ToolheadPresetSlot::Kind::Park, ToolheadPresetSlot::Kind::ZUp,
                            ToolheadPresetSlot::Kind::ZDown, ToolheadPresetSlot::Kind::Macro,
                            ToolheadPresetSlot::Kind::Empty}) {
        ToolheadPresetSlot slot;
        slot.kind = kind;
        out.push_back(slot);
    }
    return out;
}

void ToolheadPresetConfigModal::on_show() {
    s_active_ = this;
    list_ = find_widget("toolhead_preset_cfg_list");
    selected_ = 0;
    showing_macros_ = false;
    lv_subject_set_int(&s_slot_subject, 0);
    publish_slot_texts();
    populate_list();
}

void ToolheadPresetConfigModal::on_hide() {
    list_ = nullptr;
}

void ToolheadPresetConfigModal::select_slot(std::size_t index) {
    if (index >= slots_.size())
        return;
    selected_ = index;
    showing_macros_ = false;
    lv_subject_set_int(&s_slot_subject, static_cast<int32_t>(index));
    populate_list();
}

void ToolheadPresetConfigModal::pick_choice(std::size_t choice) {
    const auto all = choices();
    if (choice >= all.size())
        return;
    if (all[choice].kind == ToolheadPresetSlot::Kind::Macro) {
        showing_macros_ = true;
        populate_list();
        return;
    }
    apply(all[choice]);
}

void ToolheadPresetConfigModal::pick_macro(const std::string& name) {
    ToolheadPresetSlot slot;
    slot.kind = ToolheadPresetSlot::Kind::Macro;
    slot.macro = name;
    apply(slot);
}

void ToolheadPresetConfigModal::apply(const ToolheadPresetSlot& slot) {
    slots_[selected_] = slot;
    showing_macros_ = false;
    spdlog::info("[ToolheadPresetConfigModal] Slot {} -> {}", selected_ + 1,
                 slot.kind == ToolheadPresetSlot::Kind::Empty ? std::string("empty")
                                                              : toolhead_preset_slot_label(slot));
    publish_slot_texts();
    populate_list();
    if (on_change_)
        on_change_(slots_);
}

void ToolheadPresetConfigModal::publish_slot_texts() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        std::string text = toolhead_preset_slot_label(slots_[i]);
        if (text.empty())
            text = lv_tr("Empty");
        lv_subject_copy_string(&s_slot_text_subjects[i], text.c_str());
    }
}

void ToolheadPresetConfigModal::populate_list() {
    if (!list_)
        return;
    // Async: a pick fires from a row in this list.
    ui::safe_clean_children(list_);
    const ToolheadPresetSlot& current = slots_[selected_];

    if (showing_macros_) {
        add_row(list_, lv_tr("Back"), false, [] {
            if (auto* modal = active()) {
                modal->showing_macros_ = false;
                modal->populate_list();
            }
        });
        IMoonrakerAPI* api = get_moonraker_api();
        if (!api) {
            spdlog::warn("[ToolheadPresetConfigModal] No API available for macro list");
            return;
        }
        for (const auto& macro : pickable_macro_names(api->hardware().macros())) {
            const bool selected =
                current.kind == ToolheadPresetSlot::Kind::Macro && current.macro == macro;
            add_row(list_, get_display_name(macro, DeviceType::MACRO), selected, [macro] {
                if (auto* modal = active())
                    modal->pick_macro(macro);
            });
        }
        return;
    }

    const auto all = choices();
    for (std::size_t i = 0; i < all.size(); ++i) {
        std::string label;
        switch (all[i].kind) {
        case ToolheadPresetSlot::Kind::Macro:
            label = current.kind == ToolheadPresetSlot::Kind::Macro
                        ? fmt::format("{}: {}", lv_tr("Macro"), toolhead_preset_slot_label(current))
                        : std::string(lv_tr("Macro"));
            break;
        case ToolheadPresetSlot::Kind::Empty:
            label = lv_tr("Empty");
            break;
        default:
            label = toolhead_preset_slot_label(all[i]);
            break;
        }
        add_row(list_, label, same_choice(all[i], current), [i] {
            if (auto* modal = active())
                modal->pick_choice(i);
        });
    }
}

} // namespace helix
