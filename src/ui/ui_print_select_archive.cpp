// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// PrintSelectPanel's sliced .3mf support: reading an archive's plates in place
// of the metadata the printer does not have, the plate render as the card
// thumbnail, and the detail view's plate stepper.

#include "ui_filename_utils.h"
#include "ui_panel_print_select.h"
#include "ui_update_queue.h"

#include "lvgl.h"
#include "text_io.h"
#include "three_mf_reader.h"
#include "thumbnail_cache.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

using namespace helix;

namespace {

/// Where the plate @p index sits among @p summary's plates, or 0.
size_t plate_position(const three_mf::ArchiveSummary& summary, int index) {
    for (size_t p = 0; p < summary.plates.size(); ++p) {
        if (summary.plates[p].index == index) {
            return p;
        }
    }
    return 0;
}

} // namespace

void PrintSelectPanel::load_archive(size_t i, const std::string& filename, bool extract) {
    if (!api_ || i >= file_list_.size() || file_list_[i].filename != filename) {
        return;
    }
    if (!archive_reader_) {
        archive_reader_ = std::make_unique<three_mf::ThreeMfReader>(api_->transfers());
    }
    PrintFileData& file = file_list_[i];
    file.archive_requested = true;
    const std::string path = gcode::join_gcode_path(current_path_, filename);
    const uint32_t gen = nav_generation_.load();
    auto token = object_lifetime_.token();
    spdlog::debug("[{}] Reading .3mf archive {} ({})", get_name(), path,
                  extract ? "no printer metadata" : "render only");

    archive_reader_->read_summary(
        path, file.file_size_bytes, file.modified,
        [this, token, i, filename, extract, gen](const three_mf::ArchiveSummary& summary) {
            auto shared = std::make_shared<const three_mf::ArchiveSummary>(summary);
            token.defer("PrintSelectPanel::archive_summary", [this, i, filename, extract, gen,
                                                              shared]() {
                if (nav_generation_.load() != gen || i >= file_list_.size() ||
                    file_list_[i].filename != filename) {
                    return;
                }
                PrintFileData& f = file_list_[i];
                f.archive = shared;
                f.archive_extract = extract;
                spdlog::debug("[{}] {} holds {} sliced plate(s)", get_name(), filename,
                              shared->plates.size());
                if (extract) {
                    // The plate's summary stands in for the metadata the
                    // printer has none of; its render follows from there.
                    const auto* plate = three_mf::find_plate(*shared, f.archive_plate);
                    process_metadata_result(i, filename,
                                            plate ? three_mf::archive_file_metadata(*shared, *plate)
                                                  : FileMetadata{});
                } else {
                    // The printer has metadata: the archive fills
                    // only what it left out.
                    if (f.original_thumbnail_url.empty()) {
                        fetch_archive_thumbnail(i);
                    }
                    fill_missing_from_archive(i);
                }
                if (filename == selected_filename_buffer_) {
                    update_plate_stepper();
                }
            });
        },
        [this, token, i, filename, extract, gen](const std::string& error) {
            spdlog::debug("[PrintSelectPanel] .3mf {} unreadable: {}", filename, error);
            if (!extract) {
                return;
            }
            // Nothing describes the file, but it still lists, as a file whose
            // metadata never arrives does.
            token.defer("PrintSelectPanel::archive_unreadable", [this, i, filename, gen]() {
                if (nav_generation_.load() != gen || i >= file_list_.size() ||
                    file_list_[i].filename != filename) {
                    return;
                }
                file_list_[i].archive_extract = true;
                process_metadata_result(i, filename, FileMetadata{});
            });
        },
        archive_cancel_);
}

void PrintSelectPanel::fetch_archive_thumbnail(size_t i) {
    if (i >= file_list_.size() || !file_list_[i].archive || !api_) {
        return;
    }
    const PrintFileData& file = file_list_[i];
    const auto* plate = three_mf::find_plate(*file.archive, file.archive_plate);
    const zip::ZipEntry* entry = plate ? three_mf::plate_thumbnail(*plate) : nullptr;
    if (!entry) {
        return;
    }
#if defined(HELIX_PLATFORM_ESP32)
    // No disk cache: the card's decoder reads the render through its own lane
    // (fetch_esp_thumbnail), from the entry this names.
    file_list_[i].esp_thumbnail.reset();
    file_list_[i].esp_thumbnail_tried = false;
    sync_esp_thumbnails(esp_window_first_, esp_window_end_);
#else
    const std::string path = gcode::join_gcode_path(current_path_, file.filename);
    // capture(), not create(): nav_generation_ belongs to panel navigation.
    ThumbnailLoadContext ctx = ThumbnailLoadContext::capture(thumbnail_alive_, &nav_generation_);
    auto panel_tok = object_lifetime_.token();
    const std::string filename = file.filename;
    const int plate_index = plate->index;
    const char* panel_name = get_name();

    helix::fetch_thumbnail_from_archive(
        path, *entry, file.modified_timestamp, api_,
        helix::ThumbnailProcessor::get_target_for_display(), ctx,
        [this, panel_tok, i, filename, plate_index](const std::string& lvgl_path,
                                                    bool /*degraded*/) {
            panel_tok.defer("PrintSelectPanel::archive_thumbnail", [this, i, filename, plate_index,
                                                                    lvgl_path]() {
                if (i >= file_list_.size() || file_list_[i].filename != filename) {
                    return;
                }
                PrintFileData& f = file_list_[i];
                // A render for a plate the user has since stepped away from.
                const auto* shown =
                    f.archive ? three_mf::find_plate(*f.archive, f.archive_plate) : nullptr;
                if (!shown || shown->index != plate_index) {
                    return;
                }
                f.thumbnail_path = lvgl_path;
                schedule_view_refresh();
                if (filename == selected_filename_buffer_) {
                    show_file_in_detail(f);
                }
            });
        },
        [panel_name, filename](const std::string& error) {
            spdlog::debug("[{}] No .3mf render for {}: {}", panel_name, filename, error);
        });
#endif
}

void PrintSelectPanel::step_archive_plate(int step) {
    const auto it = std::find_if(file_list_.begin(), file_list_.end(), [this](const auto& f) {
        return !f.is_dir && f.filename == selected_filename_buffer_;
    });
    if (it == file_list_.end() || !it->archive || !it->archive_extract ||
        it->archive->plates.size() < 2) {
        return;
    }
    const auto& plates = it->archive->plates;
    const size_t count = plates.size();
    const size_t at =
        plate_position(*it->archive, three_mf::find_plate(*it->archive, it->archive_plate)->index);
    const size_t next = step > 0 ? (at + 1) % count : (at + count - 1) % count;
    show_archive_plate(static_cast<size_t>(it - file_list_.begin()), plates[next].index);
}

void PrintSelectPanel::show_archive_plate(size_t i, int plate_index) {
    if (i >= file_list_.size() || !file_list_[i].archive) {
        return;
    }
    PrintFileData& file = file_list_[i];
    const auto* plate = three_mf::find_plate(*file.archive, plate_index);
    if (!plate) {
        return;
    }
    file.archive_plate = plate->index;
    spdlog::debug("[{}] {}: showing plate {}", get_name(), file.filename, file.archive_plate);

    // The plate's own filaments, for the mapping card and the start's palette.
    // The card keeps the whole file's time and weight; the detail view shows
    // the plate's (apply_plate_stats_to_detail()).
    const FileMetadata plate_meta = three_mf::plate_file_metadata(*plate);
    file.filament_types.clear();
    for (std::string_view t : text_io::lines(plate_meta.filament_type, ';')) {
        file.filament_types.emplace_back(t);
    }
    file.filament_colors = plate_meta.filament_colors;
    file.filament_type = file.filament_types.empty() ? "" : file.filament_types.front();
    selected_filament_type_ = file.filament_type;
    selected_filament_colors_ = file.filament_colors;
    selected_filament_materials_ = file.filament_types;
    if (detail_view_) {
        detail_view_->set_file_palette(file.filament_colors, file.filament_types);
    }
    fetch_archive_thumbnail(i);
    show_file_in_detail(file);
}

void PrintSelectPanel::update_plate_stepper() {
    int count = 0;
    std::string text;
    std::string position;
    const auto it = std::find_if(file_list_.begin(), file_list_.end(), [this](const auto& f) {
        return !f.is_dir && f.filename == selected_filename_buffer_;
    });
    if (it != file_list_.end() && it->archive && it->archive_extract) {
        const auto* plate = three_mf::find_plate(*it->archive, it->archive_plate);
        count = static_cast<int>(it->archive->plates.size());
        // The plate's name in the project, else the slicer's own plate number,
        // so it matches the project; with several, where it sits among them.
        if (plate) {
            text = plate->name.empty() ? fmt::format(fmt::runtime(lv_tr("Plate {}")), plate->index)
                                       : plate->name;
            if (count > 1) {
                position =
                    fmt::format("{}/{}", plate_position(*it->archive, plate->index) + 1, count);
            }
        }
    }
    lv_subject_copy_string(&selected_plate_text_subject_, text.c_str());
    lv_subject_copy_string(&selected_plate_position_subject_, position.c_str());
    lv_subject_set_int(&selected_plate_count_subject_, count);
    sync_archive_scan_source();
}

void PrintSelectPanel::sync_archive_scan_source() {
    auto* prep = print_preparation_manager();
    const auto it = std::find_if(file_list_.begin(), file_list_.end(), [this](const auto& f) {
        return !f.is_dir && f.filename == selected_filename_buffer_;
    });
    if (!prep || it == file_list_.end() || !helix::gcode::is_3mf(it->filename)) {
        return;
    }
    // A .3mf the printer extracts nothing for prints the shown plate as
    // G-code: that is what the pre-print scan reads.
    std::optional<ui::PrintPreparationManager::ArchiveScanSource> source;
    const auto* plate = it->archive && it->archive_extract
                            ? three_mf::find_plate(*it->archive, it->archive_plate)
                            : nullptr;
    if (plate && plate->gcode) {
        const auto out = it->archive_outputs.find(plate->index);
        const bool converted = out != it->archive_outputs.end();
        // A plate that has to be extracted here cannot print over a transport
        // with no local copies, so nothing that prints is there to scan.
        if (converted || ui::PrintPreparationManager::keeps_local_copies(api_)) {
            source.emplace();
            source->plate_gcode = *plate->gcode;
            if (converted) {
                source->converted_output = out->second;
            }
        }
    }
    prep->set_archive_scan_source(it->filename, current_path_,
                                  FileVersion{it->file_size_bytes, it->modified_timestamp},
                                  std::move(source));
}
