// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_maze.h"

#include "ui_panel_print_select.h"

#include "app_globals.h"
#include "core/gcode_filename.h"
#include "helix_fs.h"
#include "lv_draw_buf_guard.h"
#include "moonraker_types.h"
#include "print_history_manager.h"
#include "screensaver_image.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"
#include "thumbnail_rules.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_set>

namespace helix::ui {

std::vector<MazeArtCandidate> history_art_candidates(const std::vector<PrintHistoryJob>& jobs,
                                                     size_t max_jobs) {
    std::vector<MazeArtCandidate> out;
    std::unordered_set<std::string> files;
    std::vector<const ThumbnailInfo*> thumbs;
    for (const PrintHistoryJob& job : jobs) {
        if (out.size() >= max_jobs) {
            break;
        }
        if (job.thumbnails.empty() || !files.insert(job.filename).second) {
            continue;
        }
        thumbs.clear();
        for (const ThumbnailInfo& t : job.thumbnails) {
            thumbs.push_back(&t);
        }
        auto fits = [](const ThumbnailInfo* t) {
            return std::max(t->width, t->height) <= kMazeArtMaxSourceSide;
        };
        std::stable_sort(thumbs.begin(), thumbs.end(),
                         [&fits](const ThumbnailInfo* a, const ThumbnailInfo* b) {
                             if (fits(a) != fits(b)) {
                                 return fits(a);
                             }
                             return fits(a) ? a->pixel_count() > b->pixel_count()
                                            : a->pixel_count() < b->pixel_count();
                         });
        const std::string dir = helix::gcode_dir_of(job.filename);
        MazeArtCandidate keys;
        for (const ThumbnailInfo* t : thumbs) {
            keys.push_back(resolve_thumbnail_path(t->relative_path, dir));
        }
        out.push_back(std::move(keys));
    }
    return out;
}

std::vector<std::string> cached_art_paths(const std::vector<MazeArtCandidate>& candidates,
                                          const MazeArtLookup& lookup, size_t max,
                                          size_t max_probes,
                                          const std::vector<std::string>& taken) {
    std::vector<std::string> out;
    size_t probes = 0;
    auto used = [&](const std::string& path) {
        return std::find(out.begin(), out.end(), path) != out.end() ||
               std::find(taken.begin(), taken.end(), path) != taken.end();
    };
    for (const MazeArtCandidate& keys : candidates) {
        for (const std::string& key : keys) {
            if (out.size() >= max || probes >= max_probes) {
                return out;
            }
            probes++;
            std::string path = lookup(key);
            if (path.empty()) {
                continue;
            }
            if (!used(path)) {
                out.push_back(std::move(path));
            }
            break;
        }
    }
    return out;
}

std::vector<std::string> mix_maze_art(const MazeArtSources& sources, size_t max) {
    std::vector<std::string> out;
    auto add = [&](const std::string& path) {
        if (out.size() < max && !path.empty() &&
            std::find(out.begin(), out.end(), path) == out.end()) {
            out.push_back(path);
        }
    };
    const size_t rounds = std::max(sources.recent.size(), sources.files.size());
    for (size_t i = 0; i < std::max<size_t>(rounds, 1); i++) {
        if (i < sources.recent.size()) {
            add(sources.recent[i]);
        }
        if (i < sources.files.size()) {
            add(sources.files[i]);
        }
        if (i == 0) {
            add(sources.printer);
        }
    }
    return out;
}

namespace {

constexpr const char* kLogoPath = "A:assets/images/helix-icon-128.png";

/// The pixels of a decoded image as 0xAARRGGBB, for the formats LVGL decodes art into.
bool to_argb(const lv_draw_buf_t* buf, MazeImage& out) {
    const auto w = static_cast<int>(buf->header.w);
    const auto h = static_cast<int>(buf->header.h);
    const uint32_t stride = buf->header.stride;
    const auto cf = static_cast<lv_color_format_t>(buf->header.cf);
    if (w <= 0 || h <= 0) {
        return false;
    }
    out.w = w;
    out.h = h;
    out.argb.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = buf->data + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t a = 0xFF, r = 0, g = 0, b = 0;
            switch (cf) {
            case LV_COLOR_FORMAT_ARGB8888:
            case LV_COLOR_FORMAT_XRGB8888:
                b = row[x * 4];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = cf == LV_COLOR_FORMAT_ARGB8888 ? row[x * 4 + 3] : 0xFF;
                break;
            case LV_COLOR_FORMAT_RGB888:
                b = row[x * 3];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                break;
            case LV_COLOR_FORMAT_RGB565:
            case LV_COLOR_FORMAT_RGB565A8: {
                const Rgb c =
                    PixelWriter::unpack565(static_cast<uint16_t>(row[x * 2] | row[x * 2 + 1] << 8));
                r = c.r;
                g = c.g;
                b = c.b;
                if (cf == LV_COLOR_FORMAT_RGB565A8) {
                    // The alpha plane follows the colour plane, one byte per pixel.
                    const uint8_t* alpha = buf->data + static_cast<size_t>(stride) * h;
                    a = alpha[static_cast<size_t>(y) * (stride / 2) + x];
                }
                break;
            }
            default:
                return false;
            }
            out.argb[static_cast<size_t>(y) * w + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return true;
}

/// Decodes `path` into `out`, reduced to fit a `reduce_to` square when that is above 0;
/// leaves it empty when the image cannot be read or is larger than `max_side` a side (0 for
/// any size).
void load_image(const std::string& path, MazeImage& out, const char* what, int max_side = 0,
                int reduce_to = 0) {
    out = MazeImage{};
    if (path.empty()) {
        return;
    }
    if (max_side > 0) {
        lv_image_header_t header{};
        if (!saver_image_info(path.c_str(), header)) {
            spdlog::debug("[Screensaver] Maze {} '{}' did not decode", what, path);
            return;
        }
        const auto w = static_cast<int>(header.w);
        const auto h = static_cast<int>(header.h);
        if (w > max_side || h > max_side) {
            spdlog::debug("[Screensaver] Maze {} '{}' is {}x{}, over {} a side, not used", what,
                          path, w, h, max_side);
            return;
        }
    }
    lv_draw_buf_t* decoded = saver_decode_image(path.c_str(), what, spdlog::level::debug);
    if (decoded == nullptr) {
        return;
    }
    if (!to_argb(decoded, out)) {
        spdlog::debug("[Screensaver] Maze {} '{}' has colour format {}, not used", what, path,
                      static_cast<int>(decoded->header.cf));
        out = MazeImage{};
    }
    helix::safe_draw_buf_destroy(decoded, "maze_art");
    if (reduce_to > 0 && !out.empty()) {
        out = reduce_maze_image(out, reduce_to);
    }
}

} // namespace

std::string maze_cached_thumbnail(const std::string& key) {
    if (key.empty()) {
        return {};
    }
    // peek(), never get_if_cached() (which deletes a stale entry and bumps its age) or fetch().
    // The cache id carries the connected printer, so another printer's entries never match.
    // A file's card image is cached under the source its listing came from: Moonraker, or a
    // local file when the listing does (mock metadata).
    const auto target = helix::ThumbnailProcessor::get_target_for_display();
    for (const auto source :
         {helix::ThumbnailSource::Moonraker, helix::ThumbnailSource::LocalFile}) {
        std::string path =
            get_thumbnail_cache().peek(helix::thumbnail_cache_id(source, key), target);
        if (!path.empty()) {
            return path;
        }
    }
    return {};
}

MazeArtSources maze_art_sources_from_app(int screen_w, std::minstd_rand& rng) {
    MazeArtSources sources;
    if (PrintHistoryManager* history = get_print_history_manager()) {
        sources.recent = cached_art_paths(history_art_candidates(history->get_jobs(), 20),
                                          maze_cached_thumbnail, kMazeArtRecent, 20);
    }
    if (PrintSelectPanel* panel = find_print_select_panel()) {
        std::vector<MazeArtCandidate> candidates;
        for (std::string& key : panel->printer_thumbnail_keys()) {
            candidates.push_back({std::move(key)});
        }
        screensaver::shuffle_in_place(rng, candidates.begin(), candidates.end());
        sources.files = cached_art_paths(candidates, maze_cached_thumbnail, kMazeArtFiles,
                                         kMazeArtFileProbes, sources.recent);
    }
    sources.printer = saver_printer_image_path(screen_w);
    return sources;
}

bool MazeScreensaver::on_start() {
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&started]() {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
    };
    MazeArt art;
    load_image(kLogoPath, art.logo, "logo", 0, kMazeArtPictureSide);

    const MazeArtSources sources =
        art_source_ ? art_source_() : maze_art_sources_from_app(screen_w(), rng());
    // Each decode is reduced before the next starts, so at most one full-size image is held.
    for (const std::string& path : mix_maze_art(sources, kMazeMaxPictures)) {
        if (elapsed_ms() >= art_decode_budget_ms_) {
            break;
        }
        // The printer picture too: its prerendered variants are 150 or 300 px, and only a raw
        // full-size asset (no variant for that printer) is left out.
        MazeImage picture;
        load_image(path, picture, path == sources.printer ? "printer picture" : "thumbnail",
                   kMazeArtMaxSourceSide, kMazeArtPictureSide);
        if (!picture.empty()) {
            art.pictures.push_back(std::move(picture));
        }
    }
    spdlog::debug("[Screensaver] Maze art: {} recent, {} files, printer {}, {} decoded in {} ms",
                  sources.recent.size(), sources.files.size(),
                  sources.printer.empty() ? "none" : "yes", art.pictures.size(), elapsed_ms());

    start_sim(art);
    spdlog::debug("[Screensaver] Maze logo {}, {} pictures", art.logo.empty() ? "drawn" : "decoded",
                  sim_.renderer().picture_count());
    return true;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
