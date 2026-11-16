// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The thumbnail decisions every backend shares (thumbnail_rules.h), and the
// writers that must apply them: nothing the cache names `.png` may hold
// anything but PNG bytes.

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/thumbnail_processor_test_access.h"
#include "async_lifetime_guard.h"
#include "core/update_queue.h"
#include "http_request_epoch.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/unique_temp_dir.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"
#include "thumbnail_rules.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ImageFormat;

namespace {

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const char* kPngAsset = "assets/images/benchy_thumbnail_white.png";
const char* kJpegAsset = "assets/test_timelapse/benchy_timelapse_20260310.thumb.jpg";

std::vector<uint8_t> qoi_bytes() {
    // "qoif", 1x1, RGBA, sRGB, one QOI_OP_RGBA pixel, then the end marker.
    return {'q',  'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0,
            0xFF, 1,   2,   3,   4, 0, 0, 0, 0, 0, 0, 0, 1};
}

/// A scratch directory removed on scope exit.
struct ScratchDir {
    explicit ScratchDir(const std::string& prefix) : path(helix::test::unique_temp_dir(prefix)) {
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string path;
};

} // namespace

TEST_CASE("sniff_image_format reads the magic bytes, not the name", "[thumbnail][rules]") {
    CHECK(helix::sniff_image_format(read_bytes(kPngAsset)) == ImageFormat::Png);
    CHECK(helix::sniff_image_format(read_bytes(kJpegAsset)) == ImageFormat::Jpeg);
    CHECK(helix::sniff_image_format(qoi_bytes()) == ImageFormat::Qoi);

    CHECK(helix::sniff_image_format(std::vector<uint8_t>{}) == ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(nullptr, 64) == ImageFormat::Unknown);
    // A signature cut short is not that format.
    CHECK(helix::sniff_image_format(std::vector<uint8_t>{0x89, 'P', 'N', 'G'}) ==
          ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(std::vector<uint8_t>{0xFF, 0xD8}) == ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(std::string_view("<html>404</html>")) == ImageFormat::Unknown);
}

TEST_CASE("is_complete_image wants the format's end marker", "[thumbnail][rules]") {
    const auto png = read_bytes(kPngAsset);
    const auto jpeg = read_bytes(kJpegAsset);
    REQUIRE(png.size() > 64);
    REQUIRE(jpeg.size() > 64);

    CHECK(helix::is_complete_image(png));
    CHECK(helix::is_complete_image(jpeg));

    CHECK_FALSE(helix::is_complete_image({png.begin(), png.begin() + png.size() / 2}));
    CHECK_FALSE(helix::is_complete_image({jpeg.begin(), jpeg.begin() + jpeg.size() / 2}));
    // QOI is never decoded here, complete or not.
    CHECK_FALSE(helix::is_complete_image(qoi_bytes()));
}

TEST_CASE("ensure_png passes PNG, re-encodes JPEG, refuses the rest", "[thumbnail][rules]") {
    const auto png = read_bytes(kPngAsset);
    CHECK(helix::ensure_png(png) == png);

    const auto from_jpeg = helix::ensure_png(read_bytes(kJpegAsset));
    REQUIRE_FALSE(from_jpeg.empty());
    CHECK(helix::sniff_image_format(from_jpeg) == ImageFormat::Png);
    CHECK(helix::is_complete_image(from_jpeg));

    // A cut JPEG never reaches the decoder, even one cut so late (just the
    // EOI marker missing) that the decoder would make something of it.
    const auto jpeg = read_bytes(kJpegAsset);
    CHECK(helix::ensure_png({jpeg.begin(), jpeg.begin() + jpeg.size() / 2}).empty());
    CHECK(helix::ensure_png({jpeg.begin(), jpeg.end() - 2}).empty());

    CHECK(helix::ensure_png(qoi_bytes()).empty());
    CHECK(helix::ensure_png({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46}).empty());
    CHECK(helix::ensure_png({}).empty());
}

TEST_CASE("save_raw_png stores a JPEG thumbnail as a real PNG", "[thumbnail][rules][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();
    const std::string id = "rules_jpeg_" + helix::test::unique_suffix();

    const std::string saved =
        cache.save_raw_png(helix::ThumbnailSource::Moonraker, id, read_bytes(kJpegAsset));
    REQUIRE_FALSE(saved.empty());
    CHECK(helix::sniff_image_format(read_bytes(saved.substr(2))) == ImageFormat::Png);
    cache.invalidate(id);

    const std::string qoi_id = "rules_qoi_" + helix::test::unique_suffix();
    CHECK(cache.save_raw_png(helix::ThumbnailSource::Moonraker, qoi_id, qoi_bytes()).empty());
    CHECK_FALSE(std::filesystem::exists(cache.get_cache_path(qoi_id)));
}

TEST_CASE_METHOD(LVGLTestFixture, "Mock thumbnail download writes PNG bytes for a JPEG source",
                 "[thumbnail][rules][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    ScratchDir dir("rules_mock_dl");
    const std::string cache_path = dir.path + "/thumb.png";

    bool ok = false;
    api.transfers().download_thumbnail(
        kJpegAsset, cache_path, [&](const std::string&) { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    CHECK(helix::sniff_image_format(read_bytes(cache_path)) == ImageFormat::Png);
}

TEST_CASE_METHOD(HelixTestFixture, "ThumbnailProcessor pre-scales a JPEG thumbnail",
                 "[thumbnail][rules][processor]") {
    ScratchDir dir("rules_proc_jpeg");
    auto* proc = ThumbnailProcessorTestAccess::make();
    proc->set_cache_dir(dir.path);
    helix::ThumbnailTarget target;
    target.width = 120;
    target.height = 120;

    const auto jpeg = read_bytes(kJpegAsset);
    auto ok = proc->process_sync(jpeg, "timelapse.mp4", target);
    CHECK(ok.success);
    CHECK(ok.error.empty());

    // A cut JPEG never reaches the decoder.
    auto cut =
        proc->process_sync({jpeg.begin(), jpeg.begin() + jpeg.size() / 2}, "cut.mp4", target);
    CHECK_FALSE(cut.success);

    ThumbnailProcessorTestAccess::destroy(proc);
}

// ============================================================================
// Cache keys and freshness
// ============================================================================

namespace {

// The key formula and the per-source string namespaces caches on disk were
// written with. thumbnail_key must reproduce them exactly or every existing
// cache is orphaned on upgrade.
std::string on_disk_hash(const std::string& namespaced) {
    const std::string scoped = std::to_string(helix::http_epoch::printer_key()) + '\n' + namespaced;
    return std::to_string(std::hash<std::string>{}(scoped));
}

} // namespace

TEST_CASE("thumbnail_key names the files existing caches already hold", "[thumbnail][rules][key]") {
    using helix::ThumbnailSource;
    helix::http_epoch::set_base_url("http://10.0.0.7:7125", true);
    const std::string id = "sub dir/.thumbs/Benchy-300x300.png";

    CHECK(helix::thumbnail_key(ThumbnailSource::Moonraker, id) == on_disk_hash(id) + ".png");
    CHECK(helix::thumbnail_key(ThumbnailSource::LocalFile, id) ==
          on_disk_hash(id + "_local") + ".png");
    CHECK(helix::thumbnail_key(ThumbnailSource::GcodeExtract, id) ==
          on_disk_hash(id + "_extracted") + ".png");
    CHECK(helix::thumbnail_key(ThumbnailSource::Usb, id) == on_disk_hash("usb:" + id) + ".png");
    const std::string video = "benchy_20260310.mp4";
    CHECK(helix::thumbnail_key(ThumbnailSource::Timelapse, video) ==
          on_disk_hash("tl_" + std::to_string(std::hash<std::string>{}(video))) + ".png");

    helix::ThumbnailTarget target;
    target.width = 160;
    target.height = 120;
    CHECK(helix::thumbnail_key(ThumbnailSource::Moonraker, id, &target) ==
          on_disk_hash(id) + "_160x120_ARGB8888.bin");
    CHECK(helix::thumbnail_key(ThumbnailSource::Usb, id, &target) ==
          on_disk_hash("usb:" + id) + "_160x120_ARGB8888.bin");
}

TEST_CASE("is_fresh: a cache older than its source is stale", "[thumbnail][rules][key]") {
    CHECK(helix::is_fresh(1000, 0)); // source time unknown: no check
    CHECK(helix::is_fresh(1000, 999));
    CHECK(helix::is_fresh(1000, 1000));
    CHECK_FALSE(helix::is_fresh(999, 1000));
}

TEST_CASE("A cached thumbnail is found only under the source it was saved as",
          "[thumbnail][rules][key][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();
    const std::string id = "/media/usb0/rules_" + helix::test::unique_suffix() + ".gcode";

    const std::string saved =
        cache.save_raw_png(helix::ThumbnailSource::Usb, id, read_bytes(kPngAsset));
    REQUIRE_FALSE(saved.empty());

    ThumbnailRequest req;
    req.key = id;
    req.format = ThumbnailRequest::ThumbnailFormat::FullPng;
    req.source = helix::ThumbnailSource::Usb;
    CHECK(cache.get_if_cached(req) == saved);
    req.source = helix::ThumbnailSource::GcodeExtract;
    CHECK(cache.get_if_cached(req).empty());

    cache.invalidate(helix::thumbnail_cache_id(helix::ThumbnailSource::Usb, id));
}

// ============================================================================
// Cache behaviour the shared rules drive
// ============================================================================

namespace {

void set_mtime(const std::string& path, time_t epoch) {
    const utimbuf times{epoch, epoch};
    REQUIRE(::utime(path.c_str(), &times) == 0);
}

time_t mtime_of(const std::string& path) {
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    return st.st_mtime;
}

void settle_fetches(const std::function<bool()>& done) {
    for (int i = 0; i < 20 && !done(); ++i) {
        helix::ThumbnailProcessor::instance().wait_for_completion();
        helix::ui::UpdateQueue::instance().drain();
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "save_prescaled: a fresh .bin makes a rescan a no-op",
                 "[thumbnail][rules][usb]") {
    ThumbnailCache& cache = get_thumbnail_cache();
    const std::string id = "/media/usb0/rescan_" + helix::test::unique_suffix() + ".gcode";
    helix::ThumbnailTarget target;
    target.width = 128;
    target.height = 128;
    const auto png = read_bytes(kPngAsset);

    const std::string first =
        cache.save_prescaled(helix::ThumbnailSource::Usb, id, png, target, 1000);
    REQUIRE(first.size() > 4);
    REQUIRE(first.compare(first.size() - 4, 4, ".bin") == 0);
    const std::string bin = first.substr(2);
    const std::string png_path =
        cache.get_cache_path(helix::thumbnail_cache_id(helix::ThumbnailSource::Usb, id));
    set_mtime(bin, 2000);
    set_mtime(png_path, 2000);

    // Unchanged stick file: neither the PNG nor the .bin is written again.
    CHECK(cache.save_prescaled(helix::ThumbnailSource::Usb, id, png, target, 1000) == first);
    CHECK(mtime_of(bin) == 2000);
    CHECK(mtime_of(png_path) == 2000);

    // A file edited since: both are rebuilt.
    CHECK(cache.save_prescaled(helix::ThumbnailSource::Usb, id, png, target, 3000) == first);
    CHECK(mtime_of(bin) != 2000);

    cache.invalidate(helix::thumbnail_cache_id(helix::ThumbnailSource::Usb, id));
}

TEST_CASE_METHOD(LVGLTestFixture, "A full-PNG fetch honours source_modified",
                 "[thumbnail][rules][freshness]") {
    ThumbnailCache cache;
    const std::string key = ".thumbs/fullpng_fresh_" + helix::test::unique_suffix() + ".png";
    const std::string saved =
        cache.save_raw_png(helix::ThumbnailSource::Moonraker, key, read_bytes(kPngAsset));
    REQUIRE_FALSE(saved.empty());
    set_mtime(saved.substr(2), 1000);

    ThumbnailRequest req;
    req.key = key;
    req.format = ThumbnailRequest::ThumbnailFormat::FullPng;
    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;

    std::string delivered, error;
    auto fetch = [&](time_t source_modified) {
        delivered.clear();
        error.clear();
        req.source_modified = source_modified;
        cache.fetch(
            req, ThumbnailLoadContext::create(guard, &gen),
            [&](const std::string& path, bool) { delivered = path; },
            [&](const std::string& e) { error = e; });
        helix::ui::UpdateQueue::instance().drain();
    };

    fetch(500); // cache newer than the source
    CHECK(delivered == saved);

    fetch(2000); // re-sliced since: the stale PNG is not served (no API to refetch)
    CHECK(delivered.empty());
    CHECK_FALSE(error.empty());
    CHECK_FALSE(std::filesystem::exists(saved.substr(2)));
}

TEST_CASE_METHOD(LVGLTestFixture, "A refused thumbnail format is not downloaded again",
                 "[thumbnail][rules][unsupported]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    ThumbnailCache cache;

    ScratchDir dir("rules_unsupported");
    const std::string source = dir.path + "/thumb.qoi";
    {
        std::ofstream out(source, std::ios::binary);
        const auto qoi = qoi_bytes();
        out.write(reinterpret_cast<const char*>(qoi.data()),
                  static_cast<std::streamsize>(qoi.size()));
    }

    ThumbnailRequest req;
    req.key = source;
    req.target.width = 96;
    req.target.height = 96;
    req.api = &api;
    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;
    bool done = false;
    std::string delivered, error;
    auto fetch = [&](time_t source_modified) {
        done = false;
        delivered.clear();
        error.clear();
        req.source_modified = source_modified;
        cache.fetch(
            req, ThumbnailLoadContext::create(guard, &gen),
            [&](const std::string& path, bool) { delivered = path, done = true; },
            [&](const std::string& e) { error = e, done = true; });
        settle_fetches([&] { return done; });
    };

    fetch(0);
    REQUIRE_FALSE(error.empty());

    // The source turns decodable, but nothing has said the cached verdict is
    // stale: the refusal stands and no download goes out.
    std::filesystem::copy_file(kPngAsset, source,
                               std::filesystem::copy_options::overwrite_existing);
    fetch(0);
    CHECK(delivered.empty());
    CHECK_FALSE(error.empty());

    // A source newer than the verdict is fetched again.
    fetch(std::time(nullptr) + 3600);
    CHECK(error.empty());
    CHECK_FALSE(delivered.empty());

    cache.invalidate(source);
}
