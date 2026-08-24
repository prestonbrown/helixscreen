// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_asset_extractor.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace helix {

AssetExtractionResult extract_assets_if_needed(const std::string& source_dir,
                                               const std::string& target_dir,
                                               const std::string& current_version) {
    // Check if target already has matching version
    fs::path version_file = fs::path(target_dir) / "VERSION";
    if (fs::exists(version_file)) {
        std::ifstream ifs(version_file);
        std::string existing_version;
        std::getline(ifs, existing_version);
        if (existing_version == current_version) {
            spdlog::debug("Assets already at version {}, skipping extraction", current_version);
            return AssetExtractionResult::ALREADY_CURRENT;
        }
        spdlog::info("Asset version mismatch: have '{}', need '{}' - re-extracting",
                     existing_version, current_version);
    }

    // Create target directory if needed
    std::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) {
        spdlog::error("Failed to create target directory '{}': {}", target_dir, ec.message());
        return AssetExtractionResult::FAILED;
    }

    // Verify source directory exists
    if (!fs::exists(source_dir) || !fs::is_directory(source_dir)) {
        spdlog::error("Source directory '{}' does not exist or is not a directory", source_dir);
        return AssetExtractionResult::FAILED;
    }

    // Copy all files recursively from source to target
    fs::copy(source_dir, target_dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::error("Failed to copy assets from '{}' to '{}': {}", source_dir, target_dir,
                      ec.message());
        return AssetExtractionResult::FAILED;
    }

    // Write version marker
    {
        std::ofstream ofs(version_file, std::ios::trunc);
        if (!ofs) {
            spdlog::error("Failed to write version marker to '{}'", version_file.string());
            return AssetExtractionResult::FAILED;
        }
        ofs << current_version;
    }

    spdlog::info("Extracted assets to '{}' (version {})", target_dir, current_version);
    return AssetExtractionResult::EXTRACTED;
}

// ---------------------------------------------------------------------------
// Extraction-gate policy (pure, host-testable)
//
// The Android entry point further down compiles only under __ANDROID__, so the
// decisions it makes live here as free functions a host unit test can drive
// directly. The public header carries the entry points; these carry the policy.
// ---------------------------------------------------------------------------

/// Strip trailing newline/CR/space that a text asset or an on-disk marker
/// picks up from whatever wrote it.
std::string trim_build_stamp(std::string stamp) {
    while (!stamp.empty() &&
           (stamp.back() == '\n' || stamp.back() == '\r' || stamp.back() == ' ')) {
        stamp.pop_back();
    }
    return stamp;
}

/// Resolve the stamp value the extraction gate compares against.
///
/// A build that shipped no BUILD_STAMP asset — or one whose read came up short
/// or zero-length — yields an empty stamp, and an empty stamp can never equal
/// the on-disk stamp. That makes the skip branch unreachable: every cold start
/// re-extracts hundreds of files, permanently. Falling back to the release
/// version restores a stable identity, so the first launch extracts and records
/// it, later launches of the same build skip, and a version bump still forces a
/// fresh extraction. The prefix keeps the fallback from ever colliding with a
/// real (millisecond-timestamp) build stamp.
std::string resolve_asset_build_stamp(const std::string& apk_stamp,
                                      const std::string& fallback_version) {
    std::string stamp = trim_build_stamp(apk_stamp);
    if (!stamp.empty()) {
        return stamp;
    }
    return "version:" + trim_build_stamp(fallback_version);
}

/// Whether the already-extracted tree on disk can be reused as-is.
bool asset_extraction_can_skip(const std::string& resolved_stamp, const std::string& disk_stamp) {
    return !resolved_stamp.empty() && resolved_stamp == disk_stamp;
}

/// Whether an extraction pass completed well enough to record its stamp.
///
/// The stamp is the skip gate, so recording it after a partial extraction — a
/// directory that failed to open, a file that failed to read or write, with
/// ENOSPC halfway through being the realistic case — pins every later launch
/// to a truncated asset tree. Record it only when nothing failed.
bool asset_extraction_succeeded(int dirs_requested, int dirs_extracted, int file_failures) {
    return dirs_requested > 0 && dirs_extracted == dirs_requested && file_failures == 0;
}

/// Whether a file under config/ must never be written out of the package.
///
/// settings.json, settings-test.json and helixconfig*.json are per-user config
/// that must not travel in an APK at all — the app synthesizes its own defaults
/// when the file is absent, and shipping one would hand every install the
/// packager's printer host and a pre-completed setup wizard. Crash dumps
/// resurrect a dismissed crash dialog; telemetry, spool state and the runtime
/// lock belong to the device. The packaging step already keeps these out; this
/// is the second line of defense for a package built from a dirty tree.
bool is_non_shippable_config_file(const std::string& filename) {
    return filename.rfind("crash", 0) == 0 || filename == ".crash_restart_count" ||
           filename == "crash_history.json" || filename == "tool_spools.json" ||
           filename == "telemetry_device.json" || filename == "telemetry_queue.json" ||
           filename == "settings.json" || filename == "settings-test.json" ||
           filename.rfind("helixconfig", 0) == 0 || filename == ".helix-screen.lock";
}

#ifdef __ANDROID__

#include "helix_version.h"

#include <SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cstdlib>
#include <jni.h>
#include <sstream>
#include <vector>

// Get AAssetManager from the Android Activity via JNI
static AAssetManager* get_asset_manager() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        spdlog::error("[AndroidAssets] Failed to get JNI env or activity");
        return nullptr;
    }

    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_assets =
        env->GetMethodID(activity_class, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject java_asset_mgr = env->CallObjectMethod(activity, get_assets);

    env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);

    if (!java_asset_mgr) {
        spdlog::error("[AndroidAssets] Failed to get AssetManager from activity");
        return nullptr;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env, java_asset_mgr);
    env->DeleteLocalRef(java_asset_mgr);
    return mgr;
}

/// Outcome of extracting one APK asset directory.
///
/// `copied` alone cannot express "the copy went wrong": a directory with zero
/// files and a directory whose every file hit ENOSPC both report 0. The caller
/// gates the BUILD_STAMP write on these, so failures have to survive the return.
struct DirExtractResult {
    int copied = 0;     ///< files written and closed cleanly
    int failed = 0;     ///< files that could not be read, opened, or written
    bool opened = true; ///< false if the directory itself could not be enumerated
};

// Extract every file from an APK asset directory to the filesystem
static DirExtractResult extract_asset_dir(AAssetManager* mgr, const std::string& asset_path,
                                          const std::string& target_path) {
    DirExtractResult result;

    // Create target directory
    std::error_code ec;
    fs::create_directories(target_path, ec);
    if (ec) {
        spdlog::error("[AndroidAssets] Failed to create dir '{}': {}", target_path, ec.message());
        result.opened = false;
        return result;
    }

    AAssetDir* dir = AAssetManager_openDir(mgr, asset_path.c_str());
    if (!dir) {
        spdlog::error("[AndroidAssets] Failed to open asset dir '{}'", asset_path);
        result.opened = false;
        return result;
    }

    // List and copy all files in this directory
    const char* filename;
    while ((filename = AAssetDir_getNextFileName(dir)) != nullptr) {
        std::string asset_file =
            asset_path.empty() ? std::string(filename) : asset_path + "/" + filename;
        std::string target_file = target_path + "/" + filename;

        // Never write out user-owned or machine-local config, whatever the
        // package happens to contain. Not a failure — these are deliberately
        // absent from a correctly built APK, so they must not count against
        // the extraction's success.
        if (asset_path == "config" && is_non_shippable_config_file(filename)) {
            spdlog::debug("[AndroidAssets] Skipping non-shippable config file {}", filename);
            continue;
        }

        AAsset* asset = AAssetManager_open(mgr, asset_file.c_str(), AASSET_MODE_STREAMING);
        if (!asset) {
            spdlog::warn("[AndroidAssets] Could not open asset '{}'", asset_file);
            result.failed++;
            continue;
        }

        off_t size = AAsset_getLength(asset);
        std::vector<char> buf(size);
        int bytes_read = AAsset_read(asset, buf.data(), size);
        AAsset_close(asset);

        if (bytes_read != size) {
            spdlog::warn("[AndroidAssets] Short read for '{}': {} of {}", asset_file, bytes_read,
                         size);
            result.failed++;
            continue;
        }

        std::ofstream ofs(target_file, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::warn("[AndroidAssets] Could not write '{}'", target_file);
            result.failed++;
            continue;
        }
        ofs.write(buf.data(), size);
        // Close explicitly and re-test the stream: a full filesystem surfaces
        // at flush time, not at write() time, so an unchecked ofstream reports
        // success for a file that never made it to disk. Drop the truncated
        // remains so nothing downstream parses a half-written asset.
        ofs.close();
        if (!ofs) {
            spdlog::warn("[AndroidAssets] Failed to flush '{}' ({} bytes)", target_file,
                         static_cast<long long>(size));
            std::error_code rm_ec;
            fs::remove(target_file, rm_ec);
            result.failed++;
            continue;
        }
        result.copied++;
    }

    AAssetDir_close(dir);
    return result;
}

// Read MANIFEST.txt from the APK root. The manifest is produced at build time
// by scripts/gen-packaging-manifest.sh and lists every subdirectory (one per
// line, project-relative) that ships in the APK. AAssetDir can only list
// files, not subdirs — the manifest is how the extractor learns the tree.
//
// Returns empty vector on failure; caller must treat that as fatal since
// there is no safe fallback (a hardcoded list is exactly the drift we are
// trying to eliminate).
static std::vector<std::string> read_manifest(AAssetManager* mgr) {
    std::vector<std::string> dirs;
    AAsset* asset = AAssetManager_open(mgr, "MANIFEST.txt", AASSET_MODE_BUFFER);
    if (!asset) {
        spdlog::error("[AndroidAssets] BUILD ERROR: MANIFEST.txt missing from APK. "
                      "Check android/app/build.gradle — the genManifest task must run "
                      "before copyAssets/mergeAssets.");
        return dirs;
    }

    off_t size = AAsset_getLength(asset);
    std::string content(static_cast<size_t>(size), '\0');
    int bytes_read = AAsset_read(asset, content.data(), size);
    AAsset_close(asset);
    if (bytes_read != size) {
        spdlog::error("[AndroidAssets] Short read on MANIFEST.txt: {} of {}", bytes_read, size);
        return dirs;
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        dirs.push_back(std::move(line));
    }
    return dirs;
}

// Read a small text asset from the APK root (BUILD_STAMP, etc.). Returns an
// empty string if the asset is absent or short-reads; callers treat that as
// "not present" and fall through to extraction.
static std::string read_asset_text(AAssetManager* mgr, const char* name) {
    AAsset* asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!asset)
        return {};
    off_t size = AAsset_getLength(asset);
    std::string content(static_cast<size_t>(size), '\0');
    int bytes_read = AAsset_read(asset, content.data(), size);
    AAsset_close(asset);
    if (bytes_read != size)
        return {};
    return content;
}

void android_extract_assets_if_needed() {
    const char* internal_path = SDL_AndroidGetInternalStoragePath();
    if (!internal_path) {
        spdlog::error("[AndroidAssets] Could not get internal storage path from SDL");
        return;
    }

    std::string target_dir = std::string(internal_path) + "/data";
    spdlog::info("[AndroidAssets] Target directory: {}", target_dir);

    AAssetManager* mgr = get_asset_manager();
    if (!mgr) {
        spdlog::error("[AndroidAssets] Could not get AAssetManager, app will lack UI resources");
        setenv("HELIX_DATA_DIR", target_dir.c_str(), 1);
        return;
    }

    // Per-build stamp gate: Gradle writes BUILD_STAMP into the APK assets on
    // every build (scripts/gen-git-hash.sh cannot help here — its header is
    // regenerated at CMake *configure* time, not build time, so the hash goes
    // stale across same-version dev rebuilds). Comparing the APK stamp to the
    // one written at the end of the last extraction makes every install
    // re-extract, so iterating on XML/assets on a connected device always
    // picks up edits, while a stable install still skips the copy at launch.
    // The version-number gate this replaces silently served stale XML for
    // same-version dev rebuilds.
    const std::string raw_stamp = read_asset_text(mgr, "BUILD_STAMP");
    const std::string apk_stamp = resolve_asset_build_stamp(raw_stamp, helix_version());
    if (trim_build_stamp(raw_stamp).empty()) {
        spdlog::warn("[AndroidAssets] BUILD_STAMP missing or empty in the APK — gating on the "
                     "release version instead ('{}'). Check that genBuildStamp ran.",
                     apk_stamp);
    }

    fs::path stamp_file = fs::path(target_dir) / "BUILD_STAMP";
    std::string disk_stamp;
    if (fs::exists(stamp_file)) {
        std::ifstream ifs(stamp_file);
        std::getline(ifs, disk_stamp);
        disk_stamp = trim_build_stamp(disk_stamp);
    }

    if (asset_extraction_can_skip(apk_stamp, disk_stamp)) {
        spdlog::info("[AndroidAssets] Build stamp matches ({}), skipping extraction", disk_stamp);
        setenv("HELIX_DATA_DIR", target_dir.c_str(), 1);
        return;
    }
    spdlog::info("[AndroidAssets] Build stamp differs: apk='{}' disk='{}' - re-extracting",
                 apk_stamp, disk_stamp);

    // Remove pre-split stale seeds under {target_dir}/config/. Before bfeba7c26
    // these paths held the shipped RO seeds; after the split they moved to
    // assets/config/, but an upgrade-in-place leaves the old copies on disk
    // where find_readable() would return them first and shadow the new seeds.
    // Safe to delete: all entries here are RO shipped content, not user state.
    {
        std::error_code ec;
        const std::string cfg = target_dir + "/config";
        for (const char* stale : {"printer_database.json", "printing_tips.json",
                                  "default_layout.json", "helix_macros.cfg"}) {
            fs::remove(cfg + "/" + stale, ec);
        }
        for (const char* stale_dir :
             {"presets", "print_start_profiles", "platform", "sounds", "themes/defaults"}) {
            fs::remove_all(cfg + "/" + stale_dir, ec);
        }
    }

    // Walk MANIFEST.txt — every directory in the APK, one per line, in a stable
    // sort order. For each entry, extract the files it contains (AAssetDir can
    // only enumerate files, not subdirs, which is why the manifest exists).
    // Adding a new source-tree directory doesn't require touching this code:
    // the build-time script regenerates the manifest automatically.
    std::vector<std::string> manifest = read_manifest(mgr);
    if (manifest.empty()) {
        spdlog::error("[AndroidAssets] No manifest — aborting extraction. App will lack "
                      "UI resources (printer database, themes, presets, etc.).");
        setenv("HELIX_DATA_DIR", target_dir.c_str(), 1);
        return;
    }

    int total = 0;
    int dirs_extracted = 0;
    int file_failures = 0;
    for (const std::string& rel : manifest) {
        DirExtractResult r = extract_asset_dir(mgr, rel, target_dir + "/" + rel);
        total += r.copied;
        file_failures += r.failed;
        if (r.opened)
            dirs_extracted++;
    }
    spdlog::info("[AndroidAssets] Total: {} files extracted across {} of {} dirs to '{}' "
                 "({} file failures)",
                 total, dirs_extracted, manifest.size(), target_dir, file_failures);

    // Write markers: VERSION (informational) and BUILD_STAMP (the gate).
    //
    // BUILD_STAMP is what makes the next launch skip extraction, so a partial
    // pass must not record one — otherwise a single ENOSPC leaves a half-copied
    // asset tree that every subsequent launch treats as current. Leaving the
    // stamp unwritten means the next launch sees a mismatch and retries.
    if (!asset_extraction_succeeded(static_cast<int>(manifest.size()), dirs_extracted,
                                    file_failures)) {
        spdlog::error("[AndroidAssets] Extraction incomplete ({} of {} dirs, {} file failures) — "
                      "not recording BUILD_STAMP so the next launch retries",
                      dirs_extracted, manifest.size(), file_failures);
        std::error_code ec;
        fs::remove(stamp_file, ec);
    } else {
        std::error_code ec;
        fs::create_directories(target_dir, ec);
        std::ofstream vofs(fs::path(target_dir) / "VERSION", std::ios::trunc);
        if (vofs)
            vofs << helix_version();
        std::ofstream sofs(stamp_file, std::ios::trunc);
        sofs << apk_stamp;
        sofs.close();
        if (!sofs) {
            spdlog::error("[AndroidAssets] Failed to write BUILD_STAMP to '{}' — the next launch "
                          "will re-extract",
                          stamp_file.string());
            fs::remove(stamp_file, ec);
        }
    }

    // Set HELIX_DATA_DIR so ensure_project_root_cwd() chdir's here
    setenv("HELIX_DATA_DIR", target_dir.c_str(), 1);
    spdlog::info("[AndroidAssets] Set HELIX_DATA_DIR={}", target_dir);
}

#endif // __ANDROID__

} // namespace helix
