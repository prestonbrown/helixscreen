// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_image_manager.h"

#include "config.h"
#include "lvgl_image_writer.h"
#include "prerendered_images.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

// stb headers — implementations are in thumbnail_processor.cpp
#include "stb_image.h"
#include "stb_image_resize.h"

// LVGL for color format constant
#include <lvgl/src/draw/lv_image_dsc.h>

namespace fs = std::filesystem;

namespace helix {

// Maximum file size for import (5 MB)
static constexpr size_t MAX_FILE_SIZE = 5 * 1024 * 1024;

// Maximum image dimensions for import
static constexpr int MAX_IMAGE_DIMENSION = 2048;

PrinterImageManager& PrinterImageManager::instance() {
    static PrinterImageManager inst;
    return inst;
}

void PrinterImageManager::init(const std::string& config_dir) {
    custom_dir_ = config_dir + "/custom_images/";

    // Initialize image-changed subject (version counter for observers)
    if (!subjects_initialized_) {
        lv_subject_init_int(&image_changed_subject_, 0);
        subjects_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit(
            "PrinterImageManager", []() { PrinterImageManager::instance().deinit_subjects(); });
    }

    try {
        fs::create_directories(custom_dir_);
        spdlog::info("[PrinterImageManager] Initialized, custom_dir: {}", custom_dir_);
    } catch (const fs::filesystem_error& e) {
        spdlog::error("[PrinterImageManager] Failed to create custom_images dir: {}", e.what());
    }
}

void PrinterImageManager::deinit_subjects() {
    if (!subjects_initialized_)
        return;
    lv_subject_deinit(&image_changed_subject_);
    subjects_initialized_ = false;
}

// =============================================================================
// Active image resolution
// =============================================================================

std::string PrinterImageManager::get_active_image_id() const {
    Config* config = Config::get_instance();
    if (!config)
        return "";
    return config->get<std::string>(config->df() + PRINTER_IMAGE, "");
}

void PrinterImageManager::set_active_image(const std::string& id) {
    Config* config = Config::get_instance();
    if (!config)
        return;
    config->set<std::string>(config->df() + PRINTER_IMAGE, id);
    config->save();
    spdlog::info("[PrinterImageManager] Active image set to: '{}'",
                 id.empty() ? "(auto-detect)" : id);

    // Notify observers (e.g., home panel) that the image changed
    if (subjects_initialized_) {
        int ver = lv_subject_get_int(&image_changed_subject_);
        lv_subject_set_int(&image_changed_subject_, ver + 1);
    }
}

std::string PrinterImageManager::get_active_image_path(int screen_width) {
    std::string id = get_active_image_id();
    if (id.empty()) {
        return ""; // Auto-detect — caller uses existing printer_type logic
    }

    int target_size = get_printer_image_size(screen_width);

    if (id.rfind("shipped:", 0) == 0) {
        // Shipped image: "shipped:voron-24r2" -> look up prerendered path
        std::string name = id.substr(8);
        return get_prerendered_printer_path(name, screen_width);
    }

    if (id.rfind("custom:", 0) == 0) {
        // Custom image: "custom:my-printer" -> look in custom_dir
        std::string name = id.substr(7);
        std::string bin_path = custom_dir_ + name + "-" + std::to_string(target_size) + ".bin";

        if (fs::exists(bin_path)) {
            return "A:" + bin_path;
        }

        // .bin missing — scan directory for a raw source image matching this name
        if (fs::exists(custom_dir_)) {
            for (const auto& entry : fs::directory_iterator(custom_dir_)) {
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().stem().string() != name)
                    continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" &&
                    ext != ".gif")
                    continue;

                std::string raw_path = entry.path().string();
                spdlog::info("[PrinterImageManager] Lazy-importing raw image: {}", raw_path);
                auto result = import_image(raw_path);
                if (result.success && fs::exists(bin_path)) {
                    return "A:" + bin_path;
                }
                spdlog::warn("[PrinterImageManager] Lazy-import failed for {}: {}", raw_path,
                             result.error);
                return "";
            }
        }

        spdlog::warn("[PrinterImageManager] Custom image not found: {}", bin_path);
        return "";
    }

    spdlog::warn("[PrinterImageManager] Unknown image ID format: '{}'", id);
    return "";
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

std::vector<PrinterImageManager::ImageInfo> PrinterImageManager::get_shipped_images() const {
    std::vector<ImageInfo> results;

    const std::string printer_dir = "assets/images/printers/";
    auto paths = scan_for_images(printer_dir);

    for (const auto& path : paths) {
        std::string stem = fs::path(path).stem().string();

        ImageInfo info;
        info.id = "shipped:" + stem;
        info.display_name = format_display_name(stem);
        // Preview uses 150px prerendered variant
        info.preview_path = get_prerendered_printer_path(stem, 480); // 480 -> 150px
        results.push_back(std::move(info));
    }

    // Sort by id for consistent ordering
    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

std::vector<PrinterImageManager::ImageInfo> PrinterImageManager::get_custom_images() const {
    std::vector<ImageInfo> results;

    if (custom_dir_.empty() || !fs::exists(custom_dir_)) {
        return results;
    }

    for (const auto& entry : fs::directory_iterator(custom_dir_)) {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
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
        if (fs::exists(preview_bin)) {
            info.preview_path = "A:" + preview_bin;
        } else {
            info.preview_path = "A:" + entry.path().string();
        }
        results.push_back(std::move(info));
    }

    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

std::vector<PrinterImageManager::ImageInfo> PrinterImageManager::get_invalid_custom_images() const {
    std::vector<ImageInfo> results;

    if (custom_dir_.empty() || !fs::exists(custom_dir_)) {
        return results;
    }

    // Collect stems that have a successful .bin conversion
    std::set<std::string> valid_stems;
    for (const auto& entry : fs::directory_iterator(custom_dir_)) {
        if (!entry.is_regular_file())
            continue;
        std::string filename = entry.path().filename().string();
        if (filename.size() >= 8 && filename.substr(filename.size() - 8) == "-300.bin") {
            valid_stems.insert(filename.substr(0, filename.size() - 8));
        }
    }

    // Find raw files that don't have a corresponding .bin
    for (const auto& entry : fs::directory_iterator(custom_dir_)) {
        if (!entry.is_regular_file())
            continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Only consider image-like extensions (skip .bin, .DS_Store, .tmp, etc.)
        static const std::set<std::string> image_exts = {".png", ".jpg", ".jpeg",
                                                         ".bmp", ".gif", ".webp"};
        if (image_exts.find(ext) == image_exts.end())
            continue;

        std::string stem = entry.path().stem().string();
        if (valid_stems.count(stem))
            continue;

        ImageInfo info;
        info.id = "invalid:" + stem;
        info.display_name = format_display_name(stem);
        results.push_back(std::move(info));
    }

    std::sort(results.begin(), results.end(),
              [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });

    return results;
}

int PrinterImageManager::auto_import_raw_images() {
    if (custom_dir_.empty() || !fs::exists(custom_dir_)) {
        return 0;
    }

    int imported = 0;
    auto raw_files = scan_for_images(custom_dir_);

    for (const auto& path : raw_files) {
        std::string stem = fs::path(path).stem().string();
        std::string bin_path = custom_dir_ + stem + "-300.bin";

        // Skip if already converted
        if (fs::exists(bin_path)) {
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

    if (!fs::exists(dir))
        return results;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;

        std::string ext = entry.path().extension().string();
        // Case-insensitive extension check
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif") {
            results.push_back(entry.path().string());
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
    if (!fs::exists(path)) {
        result.error = "File not found: " + path;
        return result;
    }

    // Check file size
    auto file_size = fs::file_size(path);
    if (file_size > MAX_FILE_SIZE) {
        result.error =
            "File too large (" + std::to_string(file_size / 1024 / 1024) + "MB, max 5MB)";
        return result;
    }

    // Check image dimensions using stbi_info (no decode needed)
    int w = 0, h = 0, channels = 0;
    if (!stbi_info(path.c_str(), &w, &h, &channels)) {
        result.error = "Not a valid image file";
        return result;
    }

    if (w > MAX_IMAGE_DIMENSION || h > MAX_IMAGE_DIMENSION) {
        result.error = "Image too large (" + std::to_string(w) + "x" + std::to_string(h) +
                       ", max " + std::to_string(MAX_IMAGE_DIMENSION) + "x" +
                       std::to_string(MAX_IMAGE_DIMENSION) + ")";
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
    std::string stem = fs::path(source_path).stem().string();

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

    // Create 300px variant
    std::string path_300 = custom_dir_ + stem + "-300.bin";
    if (!convert_to_bin(pixels, w, h, path_300, 300)) {
        stbi_image_free(pixels);
        result.error = "Failed to create 300px variant";
        return result;
    }

    // Create 150px variant
    std::string path_150 = custom_dir_ + stem + "-150.bin";
    if (!convert_to_bin(pixels, w, h, path_150, 150)) {
        stbi_image_free(pixels);
        // Clean up the 300px variant
        fs::remove(path_300);
        result.error = "Failed to create 150px variant";
        return result;
    }

    stbi_image_free(pixels);

    result.success = true;
    result.id = "custom:" + stem;
    spdlog::info("[PrinterImageManager] Imported '{}' as '{}'", source_path, result.id);
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
        if (fs::exists(path)) {
            fs::remove(path);
            any_removed = true;
        }
    }

    if (any_removed) {
        spdlog::info("[PrinterImageManager] Deleted custom image: '{}'", name);
    } else {
        spdlog::warn("[PrinterImageManager] No files found to delete for: '{}'", name);
    }

    return any_removed;
}

} // namespace helix
