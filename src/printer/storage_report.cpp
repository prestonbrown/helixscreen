// SPDX-License-Identifier: GPL-3.0-or-later

#include "storage_report.h"

#include "core/gcode_filename.h"
#include "json_utils.h"
#include "print_photos.h"

#include <algorithm>
#include <cctype>

namespace helix::storage {

namespace {

constexpr std::string_view kIndexDir = ".helix_index";
constexpr std::string_view kThumbsDir = ".thumbs";

std::size_t index_of(std::string_view key) {
    return consumer_index(key).value_or(0);
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::optional<uint64_t> byte_count(const nlohmann::json& obj, const char* key) {
    const nlohmann::json* v = json_util::find_member(obj, key);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    return json_util::as_uint64(*v);
}

} // namespace

std::optional<std::size_t> consumer_index(std::string_view key) {
    for (std::size_t i = 0; i < kConsumers.size(); ++i) {
        if (key == kConsumers[i].key) {
            return i;
        }
    }
    return std::nullopt;
}

ReportSource report_source(PluginFeatureState storage) {
    return storage == PluginFeatureState::Ready ? ReportSource::Plugin : ReportSource::FileApi;
}

std::optional<StorageReport> parse_storage_result(const nlohmann::json& result) {
    const nlohmann::json* consumers = json_util::find_member(result, "consumers");
    if (consumers == nullptr || !consumers->is_array()) {
        return std::nullopt;
    }
    StorageReport report;
    if (const nlohmann::json* volume = json_util::find_member(result, "volume");
        volume != nullptr && volume->is_object()) {
        report.total = byte_count(*volume, "total");
        report.free = byte_count(*volume, "free");
    }
    for (const nlohmann::json& entry : *consumers) {
        const auto index = consumer_index(json_util::safe_string(entry, "name"));
        if (!index) {
            continue; // a consumer a newer plugin reports and this app cannot name
        }
        ConsumerUsage& usage = report.consumers[*index];
        usage.bytes = json_util::safe_uint64(entry, "bytes");
        usage.files = json_util::safe_uint64(entry, "files");
        usage.available = json_util::safe_bool(entry, "available");
        usage.cleanable = usage.available && kConsumers[*index].cleanable &&
                          json_util::safe_bool(entry, "cleanable");
    }
    return report;
}

std::size_t classify_gcodes_path(std::string_view rel) {
    const std::size_t slash = rel.find('/');
    if (slash != std::string_view::npos) {
        const std::string top(rel.substr(0, slash));
        const std::vector<std::string> staging = gcode::staging_dirs();
        if (std::find(staging.begin(), staging.end(), top) != staging.end() ||
            gcode::is_plugin_link_dir(top)) {
            return index_of("helix_temp");
        }
        if (top == kIndexDir) {
            return index_of("analysis_sidecars");
        }
        if (top == kPhotosDir) {
            return index_of("photos");
        }
        // Any directory segment named .thumbs, at any depth
        std::size_t start = 0;
        for (std::size_t end = slash; end != std::string_view::npos; end = rel.find('/', start)) {
            if (rel.substr(start, end - start) == kThumbsDir) {
                return index_of("thumbnails");
            }
            start = end + 1;
        }
    }
    return index_of("user_gcode");
}

std::optional<DirectoryListing> parse_directory_result(const nlohmann::json& result) {
    // Moonraker's get_directory answers {dirs, files, disk_usage}; a flat
    // array of {path, size, modified} is server.files.list's shape, which some
    // servers (and the mock, for non-gcodes roots) answer instead.
    const bool flat = result.is_array();
    const nlohmann::json* files = flat ? &result : json_util::find_member(result, "files");
    if (files == nullptr || !files->is_array()) {
        return std::nullopt;
    }
    DirectoryListing listing;
    for (const nlohmann::json& f : *files) {
        ListedFile file;
        file.name = json_util::safe_string(f, flat ? "path" : "filename");
        if (file.name.empty()) {
            continue;
        }
        file.size = json_util::safe_uint64(f, "size");
        file.modified = json_util::safe_double(f, "modified");
        listing.files.push_back(std::move(file));
    }
    if (flat) {
        return listing;
    }
    if (const nlohmann::json* dirs = json_util::find_member(result, "dirs");
        dirs != nullptr && dirs->is_array()) {
        for (const nlohmann::json& d : *dirs) {
            std::string name = json_util::safe_string(d, "dirname");
            if (!name.empty() && name != "." && name != "..") {
                listing.dirs.push_back(std::move(name));
            }
        }
    }
    if (const nlohmann::json* du = json_util::find_member(result, "disk_usage");
        du != nullptr && du->is_object()) {
        listing.total = byte_count(*du, "total");
        listing.free = byte_count(*du, "free");
    }
    return listing;
}

bool fallback_cleanable(std::size_t index) {
    if (index >= kConsumers.size()) {
        return false;
    }
    return std::string_view(kConsumers[index].key) == "timelapse";
}

bool fallback_may_clean(bool plugin_installed, const PluginFeatureMap& features) {
    if (!plugin_installed) {
        return true;
    }
    const auto it = features.find(kStorageFeature);
    return it == features.end() || it->second.available;
}

namespace {

std::string trimmed_path(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

/// @p outer is @p inner or a directory above it.
bool is_or_holds(const std::string& outer, const std::string& inner) {
    return inner == outer ||
           (outer == "/" ? !inner.empty() : inner.compare(0, outer.size() + 1, outer + "/") == 0);
}

} // namespace

bool timelapse_root_safe(const nlohmann::json& roots_result) {
    if (!roots_result.is_array()) {
        return false;
    }
    std::string timelapse, gcodes, config;
    for (const nlohmann::json& root : roots_result) {
        const std::string name = json_util::safe_string(root, "name");
        const std::string path = trimmed_path(json_util::safe_string(root, "path"));
        if (name == "timelapse") {
            timelapse = path;
        } else if (name == "gcodes") {
            gcodes = path;
        } else if (name == "config") {
            config = path;
        }
    }
    if (timelapse.empty() || gcodes.empty() || config.empty()) {
        return false;
    }
    return !is_or_holds(timelapse, gcodes) && !is_or_holds(timelapse, config);
}

bool timelapse_listing_is_own(const DirectoryListing& listing) {
    if (!listing.dirs.empty()) {
        return false;
    }
    const auto timelapse = consumer_index("timelapse");
    return timelapse &&
           std::all_of(listing.files.begin(), listing.files.end(), [&](const ListedFile& f) {
               return fallback_name_allowed(*timelapse, f.name);
           });
}

const char* print_start_failure_tag(int rpc_code) {
    return is_no_space_refusal(rpc_code) ? kNoUploadSpaceMessage
                                         : TR_NOOP("Failed to start print: {}");
}

bool fallback_name_allowed(std::size_t index, std::string_view name) {
    if (index >= kConsumers.size()) {
        return false;
    }
    const std::string_view key = kConsumers[index].key;
    if (key == "timelapse") {
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::string_view ext :
             {".mp4", ".mkv", ".webm", ".avi", ".mov", ".gif", ".jpg", ".png"}) {
            if (ends_with(lower, ext)) {
                return true;
            }
        }
    }
    return false;
}

bool fallback_removes(std::size_t index, const ListedFile& file, double now, double older_than) {
    if (!fallback_cleanable(index)) {
        return false;
    }
    const std::size_t slash = file.name.rfind('/');
    const std::string_view base = slash == std::string::npos
                                      ? std::string_view(file.name)
                                      : std::string_view(file.name).substr(slash + 1);
    if (!fallback_name_allowed(index, base)) {
        return false;
    }
    return now - file.modified >= std::max(older_than, kFallbackMinAgeS);
}

CleanupMessage cleanup_message(const CleanupOutcome& outcome) {
    if (outcome.refused) {
        return CleanupMessage::NothingFreed;
    }
    if (outcome.errors > 0) {
        return outcome.freed > 0 ? CleanupMessage::FreedPartly : CleanupMessage::NothingFreed;
    }
    return outcome.freed > 0 ? CleanupMessage::Freed : CleanupMessage::NothingToFree;
}

UploadSpace upload_space(std::optional<uint64_t> free_bytes, uint64_t size) {
    if (!free_bytes) {
        return UploadSpace::Unknown;
    }
    if (*free_bytes < size || *free_bytes - size < kFreeSpaceFloorBytes) {
        return UploadSpace::TooFull;
    }
    return UploadSpace::Ok;
}

} // namespace helix::storage
