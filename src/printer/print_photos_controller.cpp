// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_photos_controller.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "fallback_sender.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "printer_state.h"
#include "thumbnail_rules.h"
#include "webcam_selection.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <memory>
#include <utility>

namespace helix {

using ui::observe;

namespace {
constexpr const char* kHandlerName = "PrintPhotos";
} // namespace

PrintPhotosController::PrintPhotosController(SnapshotFetch fetch) : fetch_(std::move(fetch)) {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PrintPhotosController* self, int seq) {
            if (seq > 0) {
                self->on_plugin_probed();
            }
        },
        ps.get_subjects_lifetime());
    // print_stats.info's layer is stored after the subject fires, so it is
    // read on the next tick.
    layer_observer_ = observe<int>(
        ps.print_state().get_print_layer_current_subject(), this,
        [](PrintPhotosController* self, int) {
            self->lifetime_.defer("PrintPhotos::layer", [self] {
                const auto& print = get_printer_state().print_state();
                if (print.slicer_reports_layers()) {
                    self->on_slicer_layer(print.slicer_current_layer());
                }
            });
        },
        ps.get_subjects_lifetime());
}

void PrintPhotosController::on_plugin_probed() {
    subscribe_history();
}

void PrintPhotosController::subscribe_history() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    history_events_.subscribe(*api, kHandlerName, lifetime_.token(),
                              [this](const HistoryJobEvent& event) {
                                  const Job job{event.job_id, event.filename, event.start_time};
                                  if (event.action == "added") {
                                      on_job_added(job);
                                  } else {
                                      on_job_finished(job, event.status);
                                  }
                              });
}

void PrintPhotosController::on_job_added(const Job& job) {
    job_ = job;
    layer_.reset();
    layer_photo_due_ = true;
}

void PrintPhotosController::on_job_finished(const Job& job, const std::string& status) {
    if (job_ && job_->job_id == job.job_id) {
        job_.reset();
    }
    layer_photo_due_ = false;
    layer_.reset();
    if (ends_with_photo(status)) {
        capture(job, kPhotoKindEnd);
    }
}

void PrintPhotosController::on_slicer_layer(int layer) {
    const std::optional<int> previous = layer_;
    layer_ = layer;
    if (!job_ || !layer_photo_due_ || !layer_two_done(previous, layer)) {
        return;
    }
    layer_photo_due_ = false;
    capture(*job_, kPhotoKindLayer);
}

void PrintPhotosController::capture(const Job& job, const std::string& kind) {
    auto& ps = get_printer_state();
    auto& status = ps.plugin_status_state();
    const PluginFeatureState photos = status.plugin_feature(kPhotosFeature, kPhotosMinVersion);
    const bool opted_in =
        plugin_feature_opted_in(status.stored_opt_ins(), kPhotosFeature, /*acts=*/false);
    std::optional<WebcamInfo> cam = webcam::photo_pick(ps.capabilities_state().get_webcams());
    const bool can_capture = fetch_ != nullptr && cam.has_value();
    // Only a photo this instance would take needs to know who else is here.
    if (photo_taker(photos, opted_in, can_capture, FallbackSenderRole::Sender) != PhotoTaker::App) {
        spdlog::debug("[PrintPhotos] Job {} {} photo: {}", job.job_id, kind,
                      photos == PluginFeatureState::Ready ? "the plugin takes it"
                      : !can_capture                      ? "no camera to take it with"
                                                          : "switched off");
        return;
    }
    std::string url = cam->snapshot_url;
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->resolve_webcam_url(url);
    }
    resolve_fallback_sender_role(
        lifetime_.token(), [this, job, kind, url, photos, opted_in](FallbackSenderRole role) {
            if (photo_taker(photos, opted_in, true, role) != PhotoTaker::App) {
                spdlog::debug("[PrintPhotos] Job {} {} photo: a local screen takes it", job.job_id,
                              kind);
                return;
            }
            fetch_and_store(job, kind, url);
        });
}

void PrintPhotosController::fetch_and_store(const Job& job, const std::string& kind,
                                            const std::string& url) {
    spdlog::info("[PrintPhotos] Taking the {} photo of job {} from {}", kind, job.job_id, url);
    fetch_(url, [this, token = lifetime_.token(), job, kind](std::string body) {
        // Worker thread: check the bytes, hand them to the main thread.
        if (sniff_image_format(body) != ImageFormat::Jpeg || body.size() > kMaxPhotoBytes) {
            spdlog::info("[PrintPhotos] No {} photo for job {}: the snapshot is {}", kind,
                         job.job_id, body.empty() ? "unavailable" : "not a usable JPEG");
            return;
        }
        auto bytes = std::make_shared<std::string>(std::move(body));
        token.defer("PrintPhotos::fetched",
                    [this, job, kind, bytes] { upload(job, kind, std::move(*bytes)); });
    });
}

void PrintPhotosController::upload(const Job& job, const std::string& kind, std::string bytes) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !api->is_connected()) {
        return;
    }
    const std::string path = photo_path(job.job_id, job.start_time, kind);
    const size_t size = bytes.size();
    api->transfers().upload_file_with_name(
        "gcodes", path, photo_file_name(job.job_id, job.start_time, kind), bytes,
        [this, token = lifetime_.token(), job, kind, path, size] {
            token.defer("PrintPhotos::uploaded",
                        [this, job, kind, path, size] { write_record(job, kind, path, size); });
        },
        [path](const MoonrakerError& err) {
            spdlog::warn("[PrintPhotos] Uploading {} failed: {}", path, err.message);
        },
        ITransfersAPI::UploadLane::Bulk);
}

void PrintPhotosController::write_record(const Job& job, const std::string& kind,
                                         const std::string& path, size_t size) {
    pending_records_.push_back({job, kind, path, size});
    next_record();
}

void PrintPhotosController::next_record() {
    if (record_busy_ || pending_records_.empty()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        pending_records_.clear();
        return;
    }
    // One read-add-write at a time: two photos of one job written at once
    // would both read the record without the other.
    record_busy_ = true;
    PendingRecord pending = std::move(pending_records_.front());
    pending_records_.pop_front();
    auto done = [this, token = lifetime_.token()](bool written) {
        token.defer("PrintPhotos::record_done", [this, written] {
            record_busy_ = false;
            if (written && pending_records_.empty()) {
                prune();
            }
            next_record();
        });
    };
    auto store = [this, token = lifetime_.token(), pending, done](const nlohmann::json& existing) {
        if (photo_record_is_newer(existing)) {
            spdlog::info("[PrintPhotos] The record of job {} is a newer version, left as it is",
                         pending.job.job_id);
            done(false);
            return;
        }
        nlohmann::json record = photo_record_with(
            existing, pending.job.job_id, pending.job.filename, pending.job.start_time,
            pending.kind, pending.path, pending.size, static_cast<int64_t>(std::time(nullptr)));
        token.defer("PrintPhotos::store", [job = pending.job, record = std::move(record), done] {
            IMoonrakerAPI* api = get_moonraker_api();
            if (api == nullptr) {
                done(false);
                return;
            }
            api->database_post_item(
                kPhotosNamespace, job.job_id, record,
                [job, done] {
                    spdlog::info("[PrintPhotos] Photo record of job {} written", job.job_id);
                    done(true);
                },
                [job, done](const MoonrakerError& err) {
                    spdlog::warn("[PrintPhotos] Writing the record of job {} failed: {}",
                                 job.job_id, err.message);
                    done(false);
                });
        });
    };
    api->database_get_item(kPhotosNamespace, pending.job.job_id, store,
                           [store, done, job_id = pending.job.job_id](const MoonrakerError& err) {
                               if (!err.is_not_found()) {
                                   spdlog::warn("[PrintPhotos] Reading the record of job {} "
                                                "failed: {}",
                                                job_id, err.message);
                                   done(false);
                                   return;
                               }
                               store(nlohmann::json());
                           });
}

void PrintPhotosController::prune() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    api->database_get_namespace(
        kPhotosNamespace,
        [token = lifetime_.token()](const nlohmann::json& records) {
            std::vector<std::string> drop = photo_records_to_prune(records, kMaxPhotoJobs);
            if (drop.empty()) {
                return;
            }
            std::vector<std::pair<std::string, std::vector<std::string>>> doomed;
            for (const auto& key : drop) {
                const nlohmann::json* record = json_util::find_member(records, key.c_str());
                doomed.emplace_back(key, record != nullptr ? photo_record_paths(*record)
                                                           : std::vector<std::string>{});
            }
            token.defer("PrintPhotos::prune_apply", [doomed = std::move(doomed)] {
                IMoonrakerAPI* api = get_moonraker_api();
                if (api == nullptr) {
                    return;
                }
                spdlog::info("[PrintPhotos] Dropping the photos of {} old jobs", doomed.size());
                for (const auto& [key, paths] : doomed) {
                    for (const auto& path : paths) {
                        api->files().delete_file(
                            "gcodes/" + path, [] {},
                            [path](const MoonrakerError& err) {
                                spdlog::debug("[PrintPhotos] Deleting {} failed: {}", path,
                                              err.message);
                            });
                    }
                    api->database_delete_item(kPhotosNamespace, key);
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[PrintPhotos] Reading the photo records failed: {}", err.message);
        });
}

} // namespace helix
