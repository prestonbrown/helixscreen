// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_printer_image_tagger.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_nav.h"
#include "ui_panel_common.h"

#include "callout_layout.h"
#include "lvgl.h"
#include "panel_widgets/callout_chip.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <type_traits>

namespace helix::settings {

using helix::ui::find_required;

std::optional<ImageTagTarget> displayed_image_tag_target() {
    ImageTagTarget t;
    t.path = PrinterImageManager::instance().get_displayed_image_path(
        printer_images::current_screen_width());
    t.key = printer_image_region_key(t.path);
    lv_image_header_t hdr;
    if (t.key.empty() || lv_image_decoder_get_info(t.path.c_str(), &hdr) != LV_RESULT_OK)
        return std::nullopt;
    t.natural_w = static_cast<int>(hdr.w);
    t.natural_h = static_cast<int>(hdr.h);
    return t;
}

namespace {

const char* prompt_text(TagPrompt p) {
    switch (p) {
    case TagPrompt::Nozzle:
        return lv_tr("Tap the nozzle tip");
    case TagPrompt::PartFan:
        return lv_tr("Tap the part cooling fan");
    case TagPrompt::BedLeft:
        return lv_tr("Tap the bed's front-left corner");
    case TagPrompt::BedRight:
        return lv_tr("Tap the bed's front-right corner");
    case TagPrompt::Chamber:
        return lv_tr("Tap an empty spot inside the enclosure");
    case TagPrompt::Light:
        return lv_tr("Tap the light");
    case TagPrompt::Done:
        break;
    }
    return lv_tr("Check the chips, then save");
}

/// Each review chip's part, in CalloutKind order.
constexpr const char* kChipParts[] = {"nozzle", "bed", "chamber", "fan", "light", "toolhead"};
constexpr size_t kChipCount = std::size(kChipParts);

} // namespace

std::string review_chip_name(CalloutKind k) {
    return std::string("tagger_chip_") + kChipParts[static_cast<size_t>(k)];
}

std::string review_chip_shown_subject(CalloutKind k) {
    return std::string("printer_image_tagger_") + kChipParts[static_cast<size_t>(k)] + "_shown";
}

std::optional<NormPoint> tagger_tap_point(const lv_area_t& image_box, lv_point_t tap, int natural_w,
                                          int natural_h) {
    const CalloutRect fit = fit_image(lv_area_get_width(&image_box), lv_area_get_height(&image_box),
                                      natural_w, natural_h);
    return image_point_at(fit, tap.x - image_box.x1, tap.y - image_box.y1);
}

PrinterImageTaggerOverlay::~PrinterImageTaggerOverlay() {
    deinit_subjects_base(subjects_);
}

void PrinterImageTaggerOverlay::init_subjects() {
    UI_MANAGED_SUBJECT_STRING(prompt_subject_, prompt_buf_, "", "printer_image_tagger_prompt",
                              subjects_);
    UI_MANAGED_SUBJECT_POINTER(image_src_subject_, image_src_buf_, "printer_image_tagger_src",
                               subjects_);
    UI_MANAGED_SUBJECT_INT(reviewing_subject_, 0, "printer_image_tagger_reviewing", subjects_);
    UI_MANAGED_SUBJECT_INT(can_skip_subject_, 0, "printer_image_tagger_can_skip", subjects_);
    UI_MANAGED_SUBJECT_INT(can_undo_subject_, 0, "printer_image_tagger_can_undo", subjects_);
    static_assert(std::extent_v<decltype(chip_shown_)> == kChipCount);
    for (size_t k = 0; k < kChipCount; ++k) {
        UI_MANAGED_SUBJECT_INT(chip_shown_[k], 0,
                               review_chip_shown_subject(static_cast<CalloutKind>(k)).c_str(),
                               subjects_);
    }
}

void PrinterImageTaggerOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_printer_image_tagger_tap",
         [](lv_event_t*) { get_printer_image_tagger_overlay().handle_tap(); }},
        {"on_printer_image_tagger_skip",
         [](lv_event_t*) {
             auto& self = get_printer_image_tagger_overlay();
             if (self.session_.skip()) {
                 self.refresh();
             }
         }},
        {"on_printer_image_tagger_undo",
         [](lv_event_t*) {
             auto& self = get_printer_image_tagger_overlay();
             self.session_.undo();
             self.refresh();
         }},
        {"on_printer_image_tagger_cancel", [](lv_event_t*) { helix::nav::go_back(); }},
        {"on_printer_image_tagger_save",
         [](lv_event_t*) { get_printer_image_tagger_overlay().handle_save(); }},
    });
}

lv_obj_t* PrinterImageTaggerOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }
    overlay_root_ = helix::ui::create_xml_hidden(parent, xml_component());
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    // Full screen: every pixel of image is room to tap.
    helix::nav::set_overlay_width_unmanaged(overlay_root_);
    return overlay_root_;
}

bool PrinterImageTaggerOverlay::show(lv_obj_t* parent_screen, const ImageTagTarget& target) {
    target_ = target;
    session_ = {};
    return OverlayBase::show(parent_screen);
}

void PrinterImageTaggerOverlay::before_show() {
    std::strncpy(image_src_buf_, target_.path.c_str(), sizeof(image_src_buf_) - 1);
    lv_subject_set_pointer(&image_src_subject_, image_src_buf_);
    refresh();
    spdlog::info("[{}] Tagging '{}' ({}x{})", get_name(), target_.key, target_.natural_w,
                 target_.natural_h);
}

void PrinterImageTaggerOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh();
}

void PrinterImageTaggerOverlay::refresh() {
    const TagPrompt p = session_.prompt();
    const std::string text = session_.done()
                                 ? prompt_text(p)
                                 : fmt::format("{} ({}/{})", prompt_text(p), session_.step() + 1,
                                               ImageTagSession::kPromptCount);
    lv_subject_copy_string(&prompt_subject_, text.c_str());
    lv_subject_set_int(&reviewing_subject_, session_.done() ? 1 : 0);
    lv_subject_set_int(&can_skip_subject_, session_.can_skip() ? 1 : 0);
    lv_subject_set_int(&can_undo_subject_, session_.can_undo() ? 1 : 0);
    if (session_.done()) {
        place_review_chips();
    }
}

void PrinterImageTaggerOverlay::place_review_chips() {
    lv_obj_t* img = find_required(overlay_root_, "tagger_image", get_name());
    lv_obj_t* layer = find_required(overlay_root_, "tagger_review_layer", get_name());
    if (!img || !layer) {
        return;
    }
    // Sized like the home widget's pinned chips; the light chip is its icon.
    lv_obj_update_layout(layer);
    CalloutChipWidths widths{};
    lv_obj_t* probe = nullptr;
    for (size_t k = 0; k < kChipCount; ++k) {
        const std::string name = review_chip_name(static_cast<CalloutKind>(k));
        lv_obj_t* chip = lv_obj_find_by_name(layer, name.c_str());
        if (!chip)
            continue;
        probe = chip;
        lv_obj_t* label = lv_obj_find_by_name(chip, (name + "_text").c_str());
        widths[k] = label ? helix::ui::compact_callout_chip_w(chip, lv_label_get_text(label), 0)
                          : static_cast<int>(lv_obj_get_width(chip));
    }
    if (!probe)
        return;
    const ImageRegions regions = session_.regions(target_.natural_w, target_.natural_h);
    const int chip_h = helix::ui::compact_callout_chip_h(probe, regions.light.has_value());
    const CalloutLayout out =
        review_callout_layout(regions, lv_obj_get_width(img), lv_obj_get_height(img), widths,
                              chip_h, theme_manager_get_spacing("space_xs"));

    bool shown[kChipCount] = {};
    for (const CalloutChipOut& c : out.chips) {
        const auto k = static_cast<size_t>(c.kind);
        if (lv_obj_t* chip = lv_obj_find_by_name(layer, review_chip_name(c.kind).c_str())) {
            // DECLARATIVE_OK: measured callout layout
            lv_obj_set_pos(chip, c.rect.x, c.rect.y);
            // DECLARATIVE_OK: measured callout layout
            lv_obj_set_width(chip, c.rect.w);
            shown[k] = true;
        }
    }
    for (size_t k = 0; k < kChipCount; ++k) {
        lv_subject_set_int(&chip_shown_[k], shown[k] ? 1 : 0);
    }
}

void PrinterImageTaggerOverlay::handle_tap() {
    lv_indev_t* indev = lv_indev_active();
    lv_obj_t* img = find_required(overlay_root_, "tagger_image", get_name());
    if (!indev || !img || session_.done()) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(img, &a);
    const auto pt = tagger_tap_point(a, p, target_.natural_w, target_.natural_h);
    if (!pt) {
        return; // a tap in the letterbox, off the image
    }
    spdlog::debug("[{}] prompt {} -> ({:.3f}, {:.3f})", get_name(), session_.step(), pt->x, pt->y);
    session_.tap(*pt);
    refresh();
}

void PrinterImageTaggerOverlay::handle_save() {
    if (!session_.done()) {
        return;
    }
    if (!save_user_image_regions(target_.key,
                                 session_.regions(target_.natural_w, target_.natural_h))) {
        NOTIFY_ERROR(lv_tr("Could not save the printer image tags"));
        return;
    }
    spdlog::info("[{}] Saved tags for '{}'", get_name(), target_.key);
    PrinterImageManager::instance().notify_image_changed();
    helix::nav::go_back();
}

} // namespace helix::settings
