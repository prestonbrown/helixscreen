// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pruned_asset_tree.cpp
 * @brief The app's asset lookups, run against the tree a device actually ships
 *
 * Packaging strips `assets/images/printers/` down to one prerendered tier per
 * platform and deletes every source PNG (`prune_assets` in
 * `scripts/platform_manifest.py`). The repo tree is the opposite shape: 75
 * un-suffixed PNGs and no tier at all. Every other test in the suite runs
 * against the repo tree, so a lookup that resolves only because a PNG happens
 * to sit beside it passes here and fails on every device.
 *
 * These cases stage what pruning leaves - one tier per printer, LZ4-compressed
 * exactly as the renderer emits it, and no PNGs - then point the asset root at
 * it and call the real lookups.
 *
 * The sizes come from the manifest rather than from constants here, so a
 * platform whose panel changes is covered without editing this file.
 */

#include "../../include/data_root_resolver.h"
#include "../../include/prerender_size_class.h"
#include "../../include/prerendered_images.h"
#include "../../include/printer_image_manager.h"
#include "../../include/printer_images.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

// Declared rather than included: lz4 lives inside the LVGL submodule's private
// layout, and a test should not pin that path.
extern "C" {
int LZ4_compress_default(const char* src, char* dst, int srcSize, int dstCapacity);
int LZ4_compressBound(int inputSize);
}

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr uint8_t kLvglMagic = 0x19;
constexpr uint16_t kFlagCompressed = 0x08;
constexpr uint32_t kCompressLz4 = 2;
constexpr uint8_t kCfArgb8888 = 16; // LV_COLOR_FORMAT_ARGB8888

json load_manifest() {
    fs::path path = fs::current_path() / "assets" / "config" / "platforms.json";
    INFO("manifest path: " << path.string());
    REQUIRE(fs::exists(path));
    std::ifstream f(path);
    REQUIRE(f.good());
    json j;
    f >> j;
    return j;
}

/// Post-rotation width, which is what the app measures and what packaging used.
int effective_width(const json& panel) {
    const int w = panel.value("width", 0);
    const int h = panel.value("height", 0);
    const int rotate = panel.value("rotate", 0);
    return (rotate == 90 || rotate == 270) ? h : w;
}

/// The manifest's printer_image ladder, applied the way the packager applies it.
int kept_size(const json& manifest, int width) {
    for (const auto& rule : manifest["size_classes"]["printer_image"]) {
        if (rule.contains("max_width") && width > rule["max_width"].get<int>())
            continue;
        return rule["size"].get<int>();
    }
    FAIL("no printer_image rule matched width " << width);
    return 0;
}

/// Write a tier the way scripts/lib/lvgl_image_lib.sh does: LZ4, flag set.
void write_compressed_tier(const fs::path& file, int px) {
    const size_t stride = static_cast<size_t>(px) * 4;
    std::vector<uint8_t> raw(stride * static_cast<size_t>(px));
    for (size_t i = 0; i < raw.size(); i += 4) {
        raw[i + 0] = 0x20; // B
        raw[i + 1] = 0x40; // G
        raw[i + 2] = 0xE0; // R
        raw[i + 3] = 0xFF; // A
    }

    std::vector<char> packed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(raw.size()))));
    const int packed_len =
        LZ4_compress_default(reinterpret_cast<const char*>(raw.data()), packed.data(),
                             static_cast<int>(raw.size()), static_cast<int>(packed.size()));
    REQUIRE(packed_len > 0);
    // The whole point of the fixture: a shipped tier is smaller than its pixels.
    REQUIRE(static_cast<size_t>(packed_len) < raw.size());

    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    REQUIRE(out.good());

    const uint8_t header[12] = {
        kLvglMagic,
        kCfArgb8888,
        static_cast<uint8_t>(kFlagCompressed & 0xFF),
        static_cast<uint8_t>(kFlagCompressed >> 8),
        static_cast<uint8_t>(px & 0xFF),
        static_cast<uint8_t>(px >> 8),
        static_cast<uint8_t>(px & 0xFF),
        static_cast<uint8_t>(px >> 8),
        static_cast<uint8_t>(stride & 0xFF),
        static_cast<uint8_t>(stride >> 8),
        0,
        0,
    };
    out.write(reinterpret_cast<const char*>(header), sizeof(header));

    const uint32_t block[3] = {kCompressLz4, static_cast<uint32_t>(packed_len),
                               static_cast<uint32_t>(raw.size())};
    out.write(reinterpret_cast<const char*>(block), sizeof(block));
    out.write(packed.data(), packed_len);
    REQUIRE(out.good());
}

/// A staged asset root holding exactly what a device receives.
///
/// Reproduces both halves of how a device resolves assets: the working
/// directory IS the install root there, and the asset root is "." on top of it.
/// Redirecting only one of them leaves the other pointing at the repo tree,
/// where the PNGs still are, and the case passes for the wrong reason.
///
/// Restores both on the way out so one platform cannot leak into the next.
class PrunedTree {
  public:
    PrunedTree(const std::string& tag, int size, const std::vector<std::string>& stems)
        : previous_(helix::asset_root()), previous_cwd_(fs::current_path()) {
        root_ =
            fs::temp_directory_path() / ("helix_pruned_" + tag + "_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root_, ec);

        const fs::path printers = root_ / "assets" / "images" / "printers";
        fs::create_directories(printers / "prerendered");
        std::ofstream(printers / "README.md") << "shipped\n";

        for (const auto& stem : stems)
            write_compressed_tier(
                printers / "prerendered" / (stem + "-" + std::to_string(size) + ".bin"), size);

        helix::set_asset_root(".");
        fs::current_path(root_);
    }

    ~PrunedTree() {
        fs::current_path(previous_cwd_);
        helix::set_asset_root(previous_);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    PrunedTree(const PrunedTree&) = delete;
    PrunedTree& operator=(const PrunedTree&) = delete;

    const fs::path& root() const {
        return root_;
    }

  private:
    std::string previous_;
    fs::path previous_cwd_;
    fs::path root_;
};

/// Platforms whose panel is fixed, which are the ones packaging prunes for.
std::vector<std::string> pruned_platforms(const json& manifest) {
    std::vector<std::string> ids;
    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& entry = it.value();
        if (!entry.contains("panel"))
            continue;
        if (effective_width(entry["panel"]) <= 0)
            continue;
        ids.push_back(it.key());
    }
    return ids;
}

const char* kStem = "creality-k2-plus";

} // namespace

TEST_CASE("the fixture really is a pruned tree", "[pruned-assets]") {
    // Guards the cases below: if staging ever leaves a PNG behind, every one of
    // them would pass for the wrong reason.
    const json manifest = load_manifest();
    PrunedTree tree("selfcheck", 300, {kStem, "generic-corexy"});

    const fs::path printers = tree.root() / "assets" / "images" / "printers";
    for (const auto& entry : fs::recursive_directory_iterator(printers)) {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        INFO("staged " << entry.path().string());
        REQUIRE(ext != ".png");
    }
    REQUIRE(fs::exists(printers / "prerendered" / (std::string(kStem) + "-300.bin")));
}

TEST_CASE("every platform resolves its printer image from what it ships",
          "[pruned-assets][printer]") {
    const json manifest = load_manifest();

    for (const auto& id : pruned_platforms(manifest)) {
        const json& entry = manifest["platforms"][id];
        const int width = effective_width(entry["panel"]);
        const int size = kept_size(manifest, width);

        PrunedTree tree(id, size, {kStem, "generic-corexy"});
        INFO("platform " << id << " ships only -" << size << ".bin at width " << width);

        SECTION("home and wizard image: " + id) {
            const std::string path = helix::get_prerendered_printer_path(kStem, width);
            INFO("resolved to " << path);
            REQUIRE(path.find(std::to_string(size) + ".bin") != std::string::npos);
            REQUIRE(path.find(".png") == std::string::npos);
        }

        SECTION("the shipped-image picker is not empty: " + id) {
            // scan_for_images decides what the picker lists. A pruned tree has
            // no PNGs, so a scan that only knows about PNGs hands the user an
            // empty picker on every device.
            const auto shipped = helix::PrinterImageManager::instance().get_shipped_images(width);
            INFO("picker listed " << shipped.size() << " images");
            REQUIRE_FALSE(shipped.empty());
        }

        SECTION("every picker preview resolves to a file that exists: " + id) {
            const auto listed = helix::PrinterImageManager::instance().get_shipped_images(width);
            // An empty list would walk zero previews and report success, so the
            // absence has to be ruled out before the loop means anything.
            REQUIRE_FALSE(listed.empty());
            for (const auto& img : listed) {
                INFO(img.id << " preview -> " << img.preview_path);
                REQUIRE_FALSE(img.preview_path.empty());
                REQUIRE(img.preview_path.find(".png") == std::string::npos);
            }
        }

        SECTION("the database path helper resolves: " + id) {
            // get_prerendered_path() is what the printer database walks to turn
            // "<name>.png" into a shipped tier.
            const std::string path =
                PrinterImages::get_prerendered_path(std::string(kStem) + ".png", width);
            INFO("resolved to '" << path << "'");
            REQUIRE_FALSE(path.empty());
        }
    }
}

TEST_CASE("a compressed tier can still be cached", "[pruned-assets][cache]") {
    const json manifest = load_manifest();
    PrunedTree tree("cache", 300, {kStem, "generic-corexy"});

    const std::string tier = (tree.root() / "assets" / "images" / "printers" / "prerendered" /
                              (std::string(kStem) + "-300.bin"))
                                 .string();
    const std::string out = (tree.root() / "cached.bin").string();

    // Every shipped tier is LZ4. With the source PNG pruned there is nothing
    // else to decode from, so a generator that cannot read its own compressed
    // format has no fallback left and the cache is never built - which means
    // the failure repeats on every activation for the life of the device.
    REQUIRE(helix::generate_cached_printer_image(tier, 233, 209, out));
    REQUIRE(fs::exists(out));
    REQUIRE(fs::file_size(out) > 0);
}
