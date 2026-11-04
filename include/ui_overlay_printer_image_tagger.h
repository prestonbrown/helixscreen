// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "callout_layout.h"
#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "printer_image_regions.h"

#include <optional>
#include <string>

namespace helix::settings {

/// The image the home printer widget shows now, as the tagger sees it.
struct ImageTagTarget {
    std::string path; ///< LVGL path of the displayed image
    std::string key;  ///< printer_image_region_key(path)
    int natural_w = 0;
    int natural_h = 0;
};

/// The displayed image, or nullopt when it has no regions key or its size
/// cannot be read. The picker and the tagger both decide from this.
std::optional<ImageTagTarget> displayed_image_tag_target();

/// Where a tap at screen point `tap` lands on an image drawn contain-fit in
/// `image_box` (screen coordinates), or nullopt for a tap in the letterbox.
std::optional<NormPoint> tagger_tap_point(const lv_area_t& image_box, lv_point_t tap, int natural_w,
                                          int natural_h);

/// The review chip for `k`'s part in printer_image_tagger_overlay.xml, and the
/// subject that shows it.
std::string review_chip_name(CalloutKind k);
std::string review_chip_shown_subject(CalloutKind k);

/**
 * @brief Full-screen overlay for tapping the parts of the displayed printer image
 *
 * One tap per ImageTagSession prompt, then a review step with chips at the
 * tapped points. Save stores the tags in the user regions file and redraws the
 * home widget; Cancel (or back) leaves without saving.
 */
class PrinterImageTaggerOverlay : public OverlayBase {
  public:
    ~PrinterImageTaggerOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Printer Image Tagger";
    }
    const char* xml_component() const override {
        return "printer_image_tagger_overlay";
    }

    void on_activate() override;

    /// Start tagging `target`.
    bool show(lv_obj_t* parent_screen, const ImageTagTarget& target);

  protected:
    void before_show() override;

  private:
    void handle_tap();
    void handle_save();
    /// Session state -> subjects; in review, lays the chips out as the home widget pins them.
    void refresh();
    void place_review_chips();

    ImageTagTarget target_;
    ImageTagSession session_;

    SubjectManager subjects_;
    lv_subject_t prompt_subject_{};
    char prompt_buf_[160] = {};
    lv_subject_t image_src_subject_{};
    char image_src_buf_[512] = {};
    lv_subject_t reviewing_subject_{};
    lv_subject_t can_skip_subject_{};
    lv_subject_t can_undo_subject_{};
    /// One per review chip, indexed by CalloutKind: 1 while the layout places it.
    lv_subject_t chip_shown_[6] = {};
};

inline PrinterImageTaggerOverlay& get_printer_image_tagger_overlay() {
    return lazy_global<PrinterImageTaggerOverlay>("PrinterImageTaggerOverlay");
}

} // namespace helix::settings
