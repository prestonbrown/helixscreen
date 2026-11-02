// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_image_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "data_root_resolver.h"
#include "helix_fs.h"
#include "lvgl_image_writer.h"
#include "prerendered_images.h"
#include "printer_image_regions.h"
#include "printer_image_variants.h"
#include "printer_images.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "text_io.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <set>

// stb headers — implementations are in thumbnail_processor.cpp
#include "stb_image.h"
#include "stb_image_resize.h"

// LVGL for color format constant
#include "lvgl.h"

namespace hfs = helix::fs;

namespace helix {

// Maximum file size for import (5 MB)
static constexpr size_t kMaxFileSize = 5 * 1024 * 1024;

// Maximum image dimensions for import
static constexpr int kMaxImageDimension = 2048;

PrinterImageManager& PrinterImageManager::instance() {
    static PrinterImageManager inst;
    return inst;
}

void PrinterImageManager::init(const std::string& config_dir) {
    custom_dir_ = config_dir + "/custom_images/";
    init_subjects();

    if (!hfs::create_directories(custom_dir_)) {
        spdlog::error("[PrinterImageManager] Failed to create custom_images dir: {}",
                      std::strerror(errno));
        return;
    }
    spdlog::info("[PrinterImageManager] Initialized, custom_dir: {}", custom_dir_);
}

void PrinterImageManager::init_subjects() {
    if (subjects_initialized_)
        return;
    // Version counter for observers
    lv_subject_init_int(&image_changed_subject_, 0);
    subjects_.register_subject(&image_changed_subject_);
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(
        "PrinterImageManager", []() { PrinterImageManager::instance().deinit_subjects(); });
}

void PrinterImageManager::deinit_subjects() {
    if (!subjects_initialized_)
        return;

    // deinit_all() flips the death signal first, then frees the subject.
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

// =============================================================================
// Active image resolution
// =============================================================================

std::string PrinterImageManager::get_active_image_id() const {
    Config* config = Config::get_instance();
    return config->get<std::string>(config->df() + PRINTER_IMAGE, "");
}

void PrinterImageManager::set_active_image(const std::string& id) {
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + PRINTER_IMAGE, id);
    config->save();
    spdlog::info("[PrinterImageManager] Active image set to: '{}'",
                 id.empty() ? "(auto-detect)" : id);

    notify_image_changed();
}

std::string PrinterImageManager::get_active_image_path(int screen_width) {
    std::string id = get_active_image_id();
    if (id.empty()) {
        return ""; // Auto-detect — caller uses existing printer_type logic
    }

    int target_size = get_printer_image_size(screen_width);

    if (id.rfind("shipped:", 0) == 0) {
        // Shipped image: "shipped:voron-v2" -> look up prerendered path
        std::string name = id.substr(8);
        return get_prerendered_printer_path(name, screen_width);
    }

    if (id.rfind("custom:", 0) == 0) {
        // Custom image: "custom:my-printer" -> look in custom_dir
        std::string name = id.substr(7);
        std::string bin_path = custom_dir_ + name + "-" + std::to_string(target_size) + ".bin";

        if (hfs::exists(bin_path)) {
            return "A:" + bin_path;
        }

        // .bin missing — scan directory for a raw source image matching this name
        if (hfs::exists(custom_dir_)) {
            if (auto entries = hfs::list_dir(custom_dir_)) {
                for (const auto& e : *entries) {
                    if (!e.is_regular)
                        continue;
                    if (hfs::stem(e.name) != name)
                        continue;
                    std::string ext{hfs::extension(e.name)};
                    ext = helix::text_io::to_lower(ext);
                    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" &&
                        ext != ".gif")
                        continue;

                    std::string raw_path = e.path;
                    spdlog::info("[PrinterImageManager] Lazy-importing raw image: {}", raw_path);
                    auto result = import_image(raw_path);
                    if (result.success && hfs::exists(bin_path)) {
                        return "A:" + bin_path;
                    }
                    spdlog::warn("[PrinterImageManager] Lazy-import failed for {}: {}", raw_path,
                                 result.error);
                    return "";
                }
            }
        }

        spdlog::warn("[PrinterImageManager] Custom image not found: {}", bin_path);
        return "";
    }

    spdlog::warn("[PrinterImageManager] Unknown image ID format: '{}'", id);
    return "";
}

std::vector<PrinterImageManager::DetectionInput> PrinterImageManager::detection_inputs() {
    std::vector<DetectionInput> inputs;
    AmsState& ams = AmsState::instance();
    for (lv_subject_t* s : {ams.get_backend_count_subject(), ams.get_ams_type_subject(),
                            ams.get_ams_data_revision_subject()}) {
        inputs.push_back({s, ams.get_subjects_lifetime()});
    }
    auto& caps = get_printer_state().capabilities_state();
    for (Capability cap : {Capability::EnclosureTopFitted, Capability::HasChamberHeater}) {
        inputs.push_back({caps.subject(cap), caps.get_subjects_lifetime()});
    }
    return inputs;
}

void PrinterImageManager::notify_image_changed() {
    if (subjects_initialized_) {
        int ver = lv_subject_get_int(&image_changed_subject_);
        lv_subject_set_int(&image_changed_subject_, ver + 1);
    }
}

namespace {

/// The hardware AmsState and PrinterState have detected, as the facts image
/// variants are chosen by.
printer_images::HardwareFacts detected_hardware_facts() {
    printer_images::HardwareFacts facts;
    AmsState& ams = AmsState::instance();
    for (int i = 0; i < ams.backend_count(); ++i) {
        const AmsBackend* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }
        if (backend->get_system_info().presents_filament_system()) {
            facts.filament_systems.push_back(backend->get_type());
        }
    }
    const auto& caps = get_printer_state().capabilities_state();
    facts.enclosure_top = caps.enclosure_top_fitted();
    facts.has_chamber_heater = caps.has_chamber_heater();
    return facts;
}

} // namespace

std::string PrinterImageManager::get_detected_image_path(int screen_width) {
    Config* config = Config::get_instance();
    const std::string printer_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (printer_type.empty()) {
        return printer_images::default_printer_image_path();
    }
    return printer_images::get_best_image_for_file(
        PrinterDetector::get_image_for_printer(printer_type, detected_hardware_facts()),
        screen_width);
}

std::string PrinterImageManager::get_displayed_image_path(int screen_width) {
    std::string path = get_active_image_path(screen_width);
    if (!path.empty())
        return path;
    return get_detected_image_path(screen_width);
}

// =============================================================================
// Display name formatting
// =============================================================================

std::string PrinterImageManager::format_display_name(const std::string& stem) {
    std::string name = stem;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (c == '-' || c == '_') {
            bool between_digits = (i > 0 && i + 1 < name.size() &&
                                   std::isdigit(static_cast<unsigned char>(name[i - 1])) &&
                                   std::isdigit(static_cast<unsigned char>(name[i + 1])));
            name[i] = between_digits ? '.' : ' ';
        }
    }
    return name;
}

// =============================================================================
// Browsing
// =============================================================================

std::vector<PrinterImageManager::ImageInfo>
PrinterImageManager::get_shipped_images(int screen_width) const {
    std::vector<ImageInfo> results;
    std::set<std::string> stems;

    // Two shapes hold the shipped set, and the picker has to read both. A
    // checkout has the source PNGs and no tiers; a device has one tier per
    // printer and no PNGs at all, because packaging deletes them. Reading only
    // the PNGs hands every device an empty picker.
    const std::string printer_dir = "assets/images/printers/";
    for (const auto& path : scan_for_images(helix::asset_path(printer_dir)))
        stems.insert(std::string(hfs::stem(path)));

    const std::string tier_dir = helix::asset_path(printer_dir + "prerendered/");
    if (hfs::exists(tier_dir)) {
        if (auto entries = hfs::list_dir(tier_dir)) {
            for (const auto& e : *entries) {
                if (!e.is_regular || hfs::extension(e.name) != ".bin")
                    continue;
                // "<stem>-<size>.bin" -> "<stem>"; anything else is not a tier.
                const std::string name{hfs::stem(e.name)};
                const size_t dash = name.rfind('-');
                if (dash == std::string::npos || dash == 0)
                    continue;
                const std::string suffix = name.substr(dash + 1);
                if (suffix.empty() ||
                    !std::all_of(suffix.begin(), suffix.end(),
                                 [](unsigned char c) { return std::isdigit(c) != 0; }))
                    continue;
                stems.insert(name.substr(0, dash));
            }
        }
    }

    for (const auto& stem : stems) {
        ImageInfo info;
        info.id = "shipped:" + stem;
        info.display_name = format_display_name(stem);
        // Sized off the real display: packaging keeps only the tier this width
        // selects, so any fixed size names a file half the fleet does not have.
        info.preview_path = get_prerendered_printer_path(stem, screen_width);
        results.push_back(std::move(info));
    }

    // Sort by id for consistent ordering
    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

std::vector<PrinterImageManager::ImageInfo> PrinterImageManager::get_custom_images() const {
    std::vector<ImageInfo> results;

    if (custom_dir_.empty() || !hfs::exists(custom_dir_)) {
        return results;
    }

    if (auto entries = hfs::list_dir(custom_dir_)) {
        for (const auto& e : *entries) {
            if (!e.is_regular)
                continue;

            std::string filename = e.name;
            // Look for the 300px variant as the canonical marker
            if (filename.size() < 8 || filename.substr(filename.size() - 8) != "-300.bin")
                continue;

            // Extract base name: "my-printer-300.bin" -> "my-printer"
            std::string name = filename.substr(0, filename.size() - 8);

            ImageInfo info;
            info.id = "custom:" + name;
            info.display_name = format_display_name(name);
            // Preview uses the 150px variant
            std::string preview_bin = custom_dir_ + name + "-150.bin";
            if (hfs::exists(preview_bin)) {
                info.preview_path = "A:" + preview_bin;
            } else {
                info.preview_path = "A:" + e.path;
            }
            results.push_back(std::move(info));
        }
    }

    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

std::vector<PrinterImageManager::ImageInfo> PrinterImageManager::get_invalid_custom_images() const {
    std::vector<ImageInfo> results;

    if (custom_dir_.empty() || !hfs::exists(custom_dir_)) {
        return results;
    }

    // Collect stems that have a successful .bin conversion
    std::set<std::string> valid_stems;
    if (auto entries = hfs::list_dir(custom_dir_)) {
        for (const auto& e : *entries) {
            if (!e.is_regular)
                continue;
            if (e.name.size() >= 8 && e.name.substr(e.name.size() - 8) == "-300.bin") {
                valid_stems.insert(e.name.substr(0, e.name.size() - 8));
            }
        }
    }

    // Find raw files that don't have a corresponding .bin
    if (auto entries = hfs::list_dir(custom_dir_)) {
        for (const auto& e : *entries) {
            if (!e.is_regular)
                continue;

            std::string ext{hfs::extension(e.name)};
            ext = helix::text_io::to_lower(ext);

            // Only consider image-like extensions (skip .bin, .DS_Store, .tmp, etc.)
            static const std::set<std::string> image_exts = {".png", ".jpg", ".jpeg",
                                                             ".bmp", ".gif", ".webp"};
            if (image_exts.find(ext) == image_exts.end())
                continue;

            std::string stem{hfs::stem(e.name)};
            if (valid_stems.count(stem))
                continue;

            ImageInfo info;
            info.id = "invalid:" + stem;
            info.display_name = format_display_name(stem);
            results.push_back(std::move(info));
        }
    }

    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

int PrinterImageManager::auto_import_raw_images() {
    if (custom_dir_.empty() || !hfs::exists(custom_dir_)) {
        return 0;
    }

    int imported = 0;
    auto raw_files = scan_for_images(custom_dir_);

    for (const auto& path : raw_files) {
        std::string stem{hfs::stem(path)};
        std::string bin_path = custom_dir_ + stem + "-300.bin";

        // Skip if already converted
        if (hfs::exists(bin_path)) {
            continue;
        }

        spdlog::info("[PrinterImageManager] Auto-importing raw image: {}", path);
        auto result = import_image(path);
        if (result.success) {
            imported++;
        } else {
            spdlog::warn("[PrinterImageManager] Auto-import failed for {}: {}", path, result.error);
        }
    }

    if (imported > 0) {
        spdlog::info("[PrinterImageManager] Auto-imported {} raw image(s)", imported);
    }
    return imported;
}

std::vector<std::string> PrinterImageManager::scan_for_images(const std::string& dir) const {
    std::vector<std::string> results;

    if (!hfs::exists(dir))
        return results;

    if (auto entries = hfs::list_dir(dir)) {
        for (const auto& e : *entries) {
            if (!e.is_regular)
                continue;

            std::string ext{hfs::extension(e.name)};
            // Case-insensitive extension check
            ext = helix::text_io::to_lower(ext);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
                ext == ".gif") {
                results.push_back(e.path);
            }
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

// =============================================================================
// Validation
// =============================================================================

PrinterImageManager::ValidationResult
PrinterImageManager::validate_image(const std::string& path) const {
    ValidationResult result;

    // Check file exists
    if (!hfs::exists(path)) {
        result.error = "File not found: " + path;
        return result;
    }

    // Check file size
    auto file_size = helix::text_io::file_size(path);
    if (!file_size) {
        result.error = "File not found: " + path;
        return result;
    }
    if (*file_size > kMaxFileSize) {
        result.error =
            "File too large (" + std::to_string(*file_size / 1024 / 1024) + "MB, max 5MB)";
        return result;
    }

    // Check image dimensions using stbi_info (no decode needed)
    int w = 0, h = 0, channels = 0;
    if (!stbi_info(path.c_str(), &w, &h, &channels)) {
        result.error = "Not a valid image file";
        return result;
    }

    if (w > kMaxImageDimension || h > kMaxImageDimension) {
        result.error = "Image too large (" + std::to_string(w) + "x" + std::to_string(h) +
                       ", max " + std::to_string(kMaxImageDimension) + "x" +
                       std::to_string(kMaxImageDimension) + ")";
        return result;
    }

    result.valid = true;
    result.width = w;
    result.height = h;
    return result;
}

// =============================================================================
// Import + conversion
// =============================================================================

int PrinterImageManager::invalidate_printer_image_cache(const std::string& source_image_path) {
    int removed = 0;

    // An absent cache dir leaves nothing to invalidate.
    if (const auto listing = helix::fs::list_dir(get_printer_image_cache_dir())) {
        for (const auto& entry : *listing) {
            if (!entry.is_regular)
                continue;
            if (printer_cache_entry_matches(entry.name, source_image_path)) {
                if (helix::fs::remove(entry.path) || errno == ENOENT) {
                    ++removed;
                    spdlog::debug("[PrinterCache] Invalidated cache: {}", entry.name);
                }
            }
        }
    }

    if (removed > 0) {
        spdlog::info("[PrinterCache] Invalidated {} cache entries for '{}'", removed,
                     source_image_path);
    }
    return removed;
}

bool PrinterImageManager::convert_to_bin(const uint8_t* pixels, int w, int h,
                                         const std::string& output_path, int target_size) {
    // Calculate target dimensions maintaining aspect ratio
    int target_w, target_h;
    if (w >= h) {
        target_w = target_size;
        target_h = static_cast<int>(static_cast<float>(h) / w * target_size);
    } else {
        target_h = target_size;
        target_w = static_cast<int>(static_cast<float>(w) / h * target_size);
    }

    // Ensure minimum dimensions
    if (target_w < 1)
        target_w = 1;
    if (target_h < 1)
        target_h = 1;

    // Resize
    std::vector<uint8_t> resized(target_w * target_h * 4);
    int resize_ok = stbir_resize_uint8(pixels, w, h, 0, resized.data(), target_w, target_h, 0,
                                       4); // RGBA channels
    if (!resize_ok) {
        spdlog::error("[PrinterImageManager] Resize failed for {}", output_path);
        return false;
    }

    // Convert RGBA (stb) → BGRA (LVGL ARGB8888 in little-endian memory)
    for (size_t i = 0; i < resized.size(); i += 4) {
        std::swap(resized[i], resized[i + 2]); // R ↔ B
    }

    // Write as LVGL binary (ARGB8888)
    return write_lvgl_bin(output_path, target_w, target_h,
                          static_cast<uint8_t>(LV_COLOR_FORMAT_ARGB8888), resized.data(),
                          resized.size());
}

PrinterImageManager::ImportResult
PrinterImageManager::import_image(const std::string& source_path) {
    ImportResult result;

    // Validate
    auto validation = validate_image(source_path);
    if (!validation.valid) {
        result.error = validation.error;
        spdlog::warn("[PrinterImageManager] Import validation failed: {}", result.error);
        return result;
    }

    // Extract base name from source
    std::string stem{hfs::stem(source_path)};

    // Load with stbi — force 4 channels (RGBA)
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load(source_path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        result.error = "Failed to decode image: " + std::string(stbi_failure_reason());
        spdlog::error("[PrinterImageManager] {}", result.error);
        return result;
    }

    // Ensure custom dir exists
    if (custom_dir_.empty()) {
        stbi_image_free(pixels);
        result.error = "PrinterImageManager not initialized (no custom dir)";
        return result;
    }

    // Invalidate dimension-specific disk caches for the old image (re-import case).
    // Only filesystem ops here — LVGL cache drops happen on the main/UI thread
    // (in PrinterImageWidget and PrinterManagerOverlay) when the image subject fires.
    std::string path_300 = custom_dir_ + stem + "-300.bin";
    std::string path_150 = custom_dir_ + stem + "-150.bin";
    invalidate_printer_image_cache("A:" + path_300);
    invalidate_printer_image_cache("A:" + path_150);

    // Create 300px variant
    if (!convert_to_bin(pixels, w, h, path_300, 300)) {
        stbi_image_free(pixels);
        result.error = "Failed to create 300px variant";
        return result;
    }

    // Create 150px variant
    if (!convert_to_bin(pixels, w, h, path_150, 150)) {
        stbi_image_free(pixels);
        // Clean up the 300px variant
        hfs::remove(path_300);
        result.error = "Failed to create 150px variant";
        return result;
    }

    stbi_image_free(pixels);

    result.success = true;
    result.id = "custom:" + stem;
    spdlog::info("[PrinterImageManager] Imported '{}' as '{}'", source_path, result.id);
    // Tags made on the old picture would pin chips to the wrong places on this one.
    reset_user_image_regions(printer_image_region_key(path_300));
    return result;
}

void PrinterImageManager::import_image_async(const std::string& source_path,
                                             std::function<void(ImportResult)> callback) {
    // For now, run synchronously. Phase 4 adds proper async via thread pool.
    auto result = import_image(source_path);
    if (callback) {
        callback(std::move(result));
    }
}

// =============================================================================
// Cleanup
// =============================================================================

bool PrinterImageManager::delete_custom_image(const std::string& name) {
    if (custom_dir_.empty())
        return false;

    bool any_removed = false;

    // Remove both size variants
    for (const char* suffix : {"-300.bin", "-150.bin"}) {
        std::string path = custom_dir_ + name + suffix;
        if (hfs::exists(path)) {
            hfs::remove(path);
            any_removed = true;
        }
    }

    if (any_removed) {
        spdlog::info("[PrinterImageManager] Deleted custom image: '{}'", name);
        reset_user_image_regions(printer_image_region_key(custom_dir_ + name + "-300.bin"));
    } else {
        spdlog::warn("[PrinterImageManager] No files found to delete for: '{}'", name);
    }

    return any_removed;
}

} // namespace helix
