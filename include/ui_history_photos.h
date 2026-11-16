// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The camera photos (print_photos.h) on the print history detail overlay:
// which of them a job has, and the one the preview card shows instead of the
// slicer preview. A build without a JPEG decoder (HELIX_HAS_CAMERA=0) reports
// no photos, so the card keeps the preview.

#include "async_lifetime_guard.h"
#include "core/subject.h"
#include "lvgl.h"
#include "print_history_data.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix::ui {

/// What the detail overlay's preview card shows; history_detail_photo_view.
enum class HistoryPhotoView {
    Preview = 0, ///< the slicer preview
    Layer = 1,   ///< the photo after layer 2
    End = 2,     ///< the photo at the end of the job
};

/// Loading state of the shown photo; history_detail_photo_state.
enum class HistoryPhotoState {
    Loading = 0,
    Shown = 1,
    Failed = 2,
};

/// The view the card opens on for a job with @p photos: the end photo, else
/// the layer photo, else the preview.
HistoryPhotoView default_photo_view(const std::vector<PrintPhoto>& photos);

/// The photo of @p photos that @p view shows, or nullptr (Preview, or none of
/// that kind).
const PrintPhoto* photo_for_view(const std::vector<PrintPhoto>& photos, HistoryPhotoView view);

class HistoryPhotoViewer {
  public:
    HistoryPhotoViewer() = default;
    ~HistoryPhotoViewer();

    HistoryPhotoViewer(const HistoryPhotoViewer&) = delete;
    HistoryPhotoViewer& operator=(const HistoryPhotoViewer&) = delete;

    /// Register the overlay's photo subjects and the view callback.
    void init_subjects(SubjectManager& subjects);

    /// The detail overlay rooted at @p overlay now shows @p job. Uses the
    /// photos its auxiliary_data links, else asks for its photo record.
    void show_job(const PrintHistoryJob& job, lv_obj_t* overlay);

    /// Show @p view on the card. Main thread.
    void select(HistoryPhotoView view);

    /// Delete @p job's photos and record, for a history entry deleted here.
    static void delete_photos(const PrintHistoryJob& job);

  private:
    void apply(std::vector<PrintPhoto> photos);
    void load(const PrintPhoto& photo);
    void set_image(lv_draw_buf_t* buf);
    lv_obj_t* image() const;

    helix::Subject has_layer_{};
    helix::Subject has_end_{};
    helix::Subject view_{};
    helix::Subject state_{};

    lv_obj_t* overlay_ = nullptr;
    std::string job_id_;
    double start_time_ = 0.0;
    std::vector<PrintPhoto> photos_;
    /// The pixels the image widget points at; owned here.
    lv_draw_buf_t* buf_ = nullptr;
    /// Bumped per job and per load, so a late reply for an earlier one drops.
    uint32_t generation_ = 0;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
