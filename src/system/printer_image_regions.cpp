// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_image_regions.h"

#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

std::optional<NormPoint> read_point(const nlohmann::json& v) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
        return std::nullopt;
    return NormPoint{v[0].get<float>(), v[1].get<float>()};
}

/// One regions file, read on first use. Main-thread only.
struct RegionsTable {
    std::unordered_map<std::string, ImageRegions> entries;
    bool loaded = false;
    /// The file is there but could not be read or is not a JSON object.
    bool unreadable = false;
};

RegionsTable& shipped() {
    static RegionsTable t;
    return t;
}

RegionsTable& user() {
    static RegionsTable t;
    return t;
}

constexpr const char* kUserRegionsFile = "printer_image_regions.json";

/// `set_aside_bad`: a file that reads but does not parse is renamed to
/// `<path>.bad` and the table starts empty, so the next save writes a fresh
/// one; if the rename fails, the file stays and saving over it is refused.
void load(RegionsTable& t, const std::string& path, const char* what, bool set_aside_bad = false) {
    if (t.loaded)
        return;
    t = {};
    t.loaded = true;
    const auto text = text_io::read_file(path);
    if (!text) {
        // A missing file is the usual case; one that exists and cannot be read is not.
        t.unreadable = text_io::file_size(path).has_value();
    } else if (!nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false).is_object()) {
        const std::string bad = path + ".bad";
        bool moved = false;
        if (set_aside_bad) {
            std::remove(bad.c_str()); // rename does not replace an existing file on every VFS
            moved = std::rename(path.c_str(), bad.c_str()) == 0;
        }
        if (moved) {
            spdlog::warn("[PrinterImageRegions] {} is not a tags file; moved it to {} and "
                         "starting with no tags",
                         path, bad);
        } else {
            t.unreadable = true;
        }
    } else {
        t.entries = parse_image_regions(*text);
    }
    if (t.unreadable)
        spdlog::warn("[PrinterImageRegions] cannot read {}; its tags are ignored until it is fixed",
                     path);
    spdlog::debug("[PrinterImageRegions] {} {} tagged images from {}", t.entries.size(), what,
                  path);
}

const ImageRegions* entry_for(const RegionsTable& t, std::string_view key) {
    const auto it = t.entries.find(std::string(key));
    return it == t.entries.end() ? nullptr : &it->second;
}

nlohmann::json point_json(NormPoint p) {
    // Three decimals is a tenth of a pixel on a 300px image, and keeps the file readable.
    const auto r = [](float v) { return std::round(double(v) * 1000.0) / 1000.0; };
    return nlohmann::json::array({r(p.x), r(p.y)});
}

nlohmann::json entry_json(const ImageRegions& r) {
    nlohmann::json e = {{"size", {r.src_w, r.src_h}},
                        {"nozzle", point_json(r.nozzle)},
                        {"bed", {point_json(r.bed_left), point_json(r.bed_right)}}};
    if (r.part_fan)
        e["part_fan"] = point_json(*r.part_fan);
    if (r.chamber)
        e["chamber"] = point_json(*r.chamber);
    if (r.light)
        e["light"] = point_json(*r.light);
    return e;
}

bool write_user_table(const std::unordered_map<std::string, ImageRegions>& entries) {
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [key, r] : entries)
        doc[key] = entry_json(r);
    // One entry per line, sorted, as regions.json is laid out.
    std::string text = "{";
    const char* sep = "\n";
    for (const auto& [key, e] : doc.items()) {
        text += sep;
        text += "  " + json_util::safe_dump(nlohmann::json(key)) + ": " + json_util::safe_dump(e);
        sep = ",\n";
    }
    text += "\n}\n";
    const std::string path = writable_path(kUserRegionsFile);
    if (!text_io::write_file_atomic(path, text, text_io::Durability::Fsync)) {
        spdlog::error("[PrinterImageRegions] cannot write {}: {}", path, std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace

std::unordered_map<std::string, ImageRegions> parse_image_regions(const std::string& json_text) {
    std::unordered_map<std::string, ImageRegions> out;
    const auto doc = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        spdlog::warn("[PrinterImageRegions] regions document is not a JSON object");
        return out;
    }
    for (const auto& [name, e] : doc.items()) {
        const auto size =
            e.is_object() && e.contains("size") ? read_point(e["size"]) : std::nullopt;
        const auto nozzle =
            e.is_object() && e.contains("nozzle") ? read_point(e["nozzle"]) : std::nullopt;
        const auto& bed = e.is_object() && e.contains("bed") ? e["bed"] : nlohmann::json();
        const auto bl = bed.is_array() && bed.size() == 2 ? read_point(bed[0]) : std::nullopt;
        const auto br = bed.is_array() && bed.size() == 2 ? read_point(bed[1]) : std::nullopt;
        if (!size || !nozzle || !bl || !br) {
            spdlog::warn("[PrinterImageRegions] skipping '{}': needs size, nozzle and bed", name);
            continue;
        }
        ImageRegions r;
        r.src_w = static_cast<int>(size->x);
        r.src_h = static_cast<int>(size->y);
        r.nozzle = *nozzle;
        r.bed_left = *bl;
        r.bed_right = *br;
        if (e.contains("part_fan"))
            r.part_fan = read_point(e["part_fan"]);
        if (e.contains("chamber"))
            r.chamber = read_point(e["chamber"]);
        if (e.contains("light"))
            r.light = read_point(e["light"]);
        out.emplace(name, r);
    }
    return out;
}

const ImageRegions* lookup_user_image_regions(std::string_view key, int natural_w, int natural_h) {
    load(user(), writable_path(kUserRegionsFile), "user", /*set_aside_bad=*/true);
    const ImageRegions* r = entry_for(user(), key);
    return r && r->src_w == natural_w && r->src_h == natural_h ? r : nullptr;
}

bool has_shipped_image_regions(std::string_view key) {
    load(shipped(), asset_path("assets/images/printers/regions.json"), "shipped");
    return entry_for(shipped(), key) != nullptr;
}

const ImageRegions* lookup_image_regions(std::string_view key, int natural_w, int natural_h) {
    if (const ImageRegions* r = lookup_user_image_regions(key, natural_w, natural_h))
        return r;
    return has_shipped_image_regions(key) ? entry_for(shipped(), key) : nullptr;
}

namespace {

/// Writes the user table with `change` applied, and keeps the change in memory
/// only once it is on disk. Refuses to replace a file it could not read, which
/// would drop every tag in it.
template <typename Change> bool commit_user_change(Change change) {
    load(user(), writable_path(kUserRegionsFile), "user", /*set_aside_bad=*/true);
    if (user().unreadable) {
        spdlog::error("[PrinterImageRegions] not saving over unreadable {}",
                      writable_path(kUserRegionsFile));
        return false;
    }
    auto next = user().entries;
    change(next);
    if (!write_user_table(next))
        return false;
    user().entries = std::move(next);
    return true;
}

} // namespace

bool save_user_image_regions(const std::string& key, const ImageRegions& regions) {
    return commit_user_change([&](auto& entries) { entries[key] = regions; });
}

bool reset_user_image_regions(const std::string& key) {
    load(user(), writable_path(kUserRegionsFile), "user", /*set_aside_bad=*/true);
    if (user().entries.count(key) == 0)
        return true;
    return commit_user_change([&](auto& entries) { entries.erase(key); });
}

// Declared in tests/test_helpers/printer_image_regions_test_access.h only.
void replace_image_regions(std::unordered_map<std::string, ImageRegions> regions) {
    shipped() = {};
    shipped().entries = std::move(regions);
    shipped().loaded = true;
    user() = {};
    user().loaded = true;
}

void reload_user_image_regions() {
    user() = {};
}

void unload_image_regions() {
    shipped() = {};
    user() = {};
}

std::string printer_image_region_key(std::string_view path) {
    const bool is_shipped = path.find("/images/printers/") != std::string_view::npos;
    // Custom images live under the config dir's custom_images/.
    if (!is_shipped && path.find("/custom_images/") == std::string_view::npos)
        return {};
    const auto slash = path.find_last_of('/');
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    std::string stem(file.substr(0, dot));
    // Prerendered and imported tiers are "<name>-<size>.bin".
    if (dot != std::string_view::npos && file.substr(dot) == ".bin") {
        const auto dash = stem.find_last_of('-');
        if (dash != std::string::npos && dash + 1 < stem.size() &&
            std::all_of(stem.begin() + dash + 1, stem.end(),
                        [](unsigned char c) { return std::isdigit(c); }))
            stem.resize(dash);
    }
    return is_shipped ? stem : "custom:" + stem;
}

bool ImageTagSession::can_skip() const {
    const auto p = prompt();
    return p == TagPrompt::PartFan || p == TagPrompt::Chamber || p == TagPrompt::Light;
}

void ImageTagSession::tap(NormPoint p) {
    if (done())
        return;
    points_[step_++] = p;
}

bool ImageTagSession::skip() {
    if (!can_skip())
        return false;
    points_[step_++].reset();
    return true;
}

void ImageTagSession::undo() {
    if (step_ > 0)
        points_[--step_].reset();
}

ImageRegions ImageTagSession::regions(int natural_w, int natural_h) const {
    const auto at = [this](TagPrompt p) { return points_[static_cast<int>(p)]; };
    ImageRegions r;
    r.src_w = natural_w;
    r.src_h = natural_h;
    r.nozzle = at(TagPrompt::Nozzle).value_or(NormPoint{});
    r.bed_left = at(TagPrompt::BedLeft).value_or(NormPoint{});
    r.bed_right = at(TagPrompt::BedRight).value_or(NormPoint{});
    r.part_fan = at(TagPrompt::PartFan);
    r.chamber = at(TagPrompt::Chamber);
    r.light = at(TagPrompt::Light);
    return r;
}

} // namespace helix
