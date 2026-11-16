// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_history_photos.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // not in the lvgl.h umbrella
#include "print_photos.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

#if HELIX_HAS_CAMERA
#include "camera_frame.h"
#include "camera_stream.h"
#include "printer_state.h"
#include "webcam_selection.h"
#endif

namespace helix::ui {

namespace {

HistoryPhotoViewer* s_viewer = nullptr;

void on_view_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HistoryPhotos] on_view_clicked");
    const int view = event_user_int(e).value_or(0);
    if (s_viewer != nullptr && view >= 0 && view <= static_cast<int>(HistoryPhotoView::End)) {
        s_viewer->select(static_cast<HistoryPhotoView>(view));
    }
    LVGL_SAFE_EVENT_CB_END();
}

const char* kind_of(HistoryPhotoView view) {
    switch (view) {
    case HistoryPhotoView::Layer:
        return kPhotoKindLayer;
    case HistoryPhotoView::End:
        return kPhotoKindEnd;
    case HistoryPhotoView::Preview:
        break;
    }
    return nullptr;
}

} // namespace

HistoryPhotoView default_photo_view(const std::vector<PrintPhoto>& photos) {
    if (photo_for_view(photos, HistoryPhotoView::End) != nullptr) {
        return HistoryPhotoView::End;
    }
    if (photo_for_view(photos, HistoryPhotoView::Layer) != nullptr) {
        return HistoryPhotoView::Layer;
    }
    return HistoryPhotoView::Preview;
}

const PrintPhoto* photo_for_view(const std::vector<PrintPhoto>& photos, HistoryPhotoView view) {
    const char* kind = kind_of(view);
    if (kind == nullptr) {
        return nullptr;
    }
    for (const auto& photo : photos) {
        if (photo.kind == kind) {
            return &photo;
        }
    }
    return nullptr;
}

HistoryPhotoViewer::~HistoryPhotoViewer() {
    if (s_viewer == this) {
        s_viewer = nullptr;
    }
    if (buf_ != nullptr && lv_is_initialized()) {
        lv_image_cache_drop(buf_);
        lv_draw_buf_destroy(buf_);
    }
}

void HistoryPhotoViewer::init_subjects(SubjectManager& subjects) {
    UI_MANAGED_SUBJECT_INT(has_layer_, 0, "history_detail_has_layer_photo", subjects);
    UI_MANAGED_SUBJECT_INT(has_end_, 0, "history_detail_has_end_photo", subjects);
    UI_MANAGED_SUBJECT_INT(view_, 0, "history_detail_photo_view", subjects);
    UI_MANAGED_SUBJECT_INT(state_, 0, "history_detail_photo_state", subjects);
    register_xml_callbacks({{"history_detail_photo_view", on_view_clicked}});
    s_viewer = this;
}

lv_obj_t* HistoryPhotoViewer::image() const {
    return overlay_ != nullptr ? lv_obj_find_by_name(overlay_, "photo_image") : nullptr;
}

void HistoryPhotoViewer::set_image(lv_draw_buf_t* buf) {
    if (lv_obj_t* img = image()) {
        lv_image_set_src(img, buf);
    }
    if (buf_ != nullptr) {
        lv_image_cache_drop(buf_);
        lv_draw_buf_destroy(buf_);
    }
    buf_ = buf;
}

void HistoryPhotoViewer::show_job(const PrintHistoryJob& job, lv_obj_t* overlay) {
    ++generation_;
    overlay_ = overlay;
    job_id_ = job.job_id;
    start_time_ = job.start_time;
    apply(job.photos);
#if HELIX_HAS_CAMERA
    if (job.job_id.empty()) {
        return;
    }
    // The record is where every photo is listed; auxiliary_data only links
    // what the plugin had when the job finished, and a Moonraker whose
    // history has none links nothing. Both are shown.
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    const uint32_t generation = generation_;
    const double start_time = start_time_;
    api->database_get_item(
        kPhotosNamespace, job.job_id,
        [this, token = lifetime_.token(), generation, start_time,
         linked = job.photos](const nlohmann::json& record) {
            if (!photo_record_matches(record, start_time)) {
                return;
            }
            std::vector<PrintPhoto> photos = merge_photo_lists(linked, parse_photo_record(record));
            if (same_photos(photos, linked)) {
                return;
            }
            token.defer("HistoryPhotos::record", [this, generation, photos = std::move(photos)] {
                if (generation == generation_) {
                    apply(photos);
                }
            });
        },
        [](const MoonrakerError& err) {
            if (!err.is_not_found()) {
                spdlog::debug("[HistoryPhotos] Reading the photo record failed: {}", err.message);
            }
        });
#endif
}

void HistoryPhotoViewer::apply(std::vector<PrintPhoto> photos) {
#if !HELIX_HAS_CAMERA
    // Nothing here decodes a JPEG.
    photos.clear();
#endif
    photos_ = std::move(photos);
    lv_subject_set_int(&has_layer_,
                       photo_for_view(photos_, HistoryPhotoView::Layer) != nullptr ? 1 : 0);
    lv_subject_set_int(&has_end_,
                       photo_for_view(photos_, HistoryPhotoView::End) != nullptr ? 1 : 0);
    select(default_photo_view(photos_));
}

void HistoryPhotoViewer::select(HistoryPhotoView view) {
    lv_subject_set_int(&view_, static_cast<int>(view));
    ++generation_;
    set_image(nullptr);
    const PrintPhoto* photo = photo_for_view(photos_, view);
    if (photo == nullptr) {
        return;
    }
    lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Loading));
    load(*photo);
}

void HistoryPhotoViewer::load(const PrintPhoto& photo) {
#if HELIX_HAS_CAMERA
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Failed));
        return;
    }
    // Decoded to the box it is shown in, so a large photo never sits in
    // memory at full size. The image itself is hidden while it loads, so the
    // card's photo layer is measured.
    int max_w = 0;
    int max_h = 0;
    if (lv_obj_t* box =
            overlay_ != nullptr ? lv_obj_find_by_name(overlay_, "photo_layer") : nullptr) {
        lv_obj_update_layout(box);
        max_w = lv_obj_get_width(box);
        max_h = lv_obj_get_height(box);
    }
    if (max_w <= 0 || max_h <= 0) {
        lv_display_t* disp = lv_display_get_default();
        max_w = lv_display_get_horizontal_resolution(disp) / 2;
        max_h = lv_display_get_vertical_resolution(disp) / 2;
    }
    // Shown the way the camera view shows that camera: an upside-down camera
    // is turned upright here too. The bytes on the printer stay as taken.
    const std::optional<WebcamInfo> cam =
        webcam::photo_pick(get_printer_state().capabilities_state().get_webcams());
    const CameraStream::Transform transform =
        cam ? camera_view_transform(*cam) : CameraStream::Transform{};
    const uint32_t generation = generation_;
    const std::string path = photo.path;
    api->transfers().download_file(
        "gcodes", path,
        [this, token = lifetime_.token(), generation, max_w, max_h, path,
         transform](const std::string& jpeg) {
            // Worker thread: decode here, hand pixels to the main thread.
            auto frame = std::make_shared<CameraFrame>(
                CameraStream::decode_snapshot(jpeg, max_w, max_h, transform));
            token.defer("HistoryPhotos::decoded", [this, generation, frame, path] {
                if (generation != generation_) {
                    return;
                }
                lv_draw_buf_t* buf = frame->empty() ? nullptr : to_draw_buf(*frame);
                if (buf == nullptr) {
                    spdlog::warn("[HistoryPhotos] {} did not decode", path);
                    lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Failed));
                    return;
                }
                set_image(buf);
                lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Shown));
            });
        },
        [this, token = lifetime_.token(), generation, path](const MoonrakerError& err) {
            spdlog::warn("[HistoryPhotos] Fetching {} failed: {}", path, err.message);
            token.defer("HistoryPhotos::failed", [this, generation] {
                if (generation == generation_) {
                    lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Failed));
                }
            });
        });
#else
    (void)photo;
    lv_subject_set_int(&state_, static_cast<int>(HistoryPhotoState::Failed));
#endif
}

void HistoryPhotoViewer::delete_photos(const PrintHistoryJob& job) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || job.job_id.empty()) {
        return;
    }
    const std::string job_id = job.job_id;
    const double start_time = job.start_time;
    api->database_get_item(
        kPhotosNamespace, job_id,
        [job_id, start_time](const nlohmann::json& record) {
            // History reuses ids: a record of a later job keeps its photos.
            if (!photo_record_matches(record, start_time)) {
                return;
            }
            queue_update("HistoryPhotos::delete", [job_id, paths = photo_record_paths(record)] {
                IMoonrakerAPI* api = get_moonraker_api();
                if (api == nullptr) {
                    return;
                }
                for (const auto& path : paths) {
                    api->files().delete_file(
                        "gcodes/" + path, [] {},
                        [path](const MoonrakerError& err) {
                            spdlog::debug("[HistoryPhotos] Deleting {} failed: {}", path,
                                          err.message);
                        });
                }
                api->database_delete_item(kPhotosNamespace, job_id);
                spdlog::info("[HistoryPhotos] Deleted the photos of job {}", job_id);
            });
        },
        [](const MoonrakerError& err) {
            if (!err.is_not_found()) {
                spdlog::debug("[HistoryPhotos] Reading the photo record failed: {}", err.message);
            }
        });
}

} // namespace helix::ui
