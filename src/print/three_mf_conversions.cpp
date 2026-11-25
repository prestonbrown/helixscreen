// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "three_mf_conversions.h"

#include "json_utils.h"

#include <cmath>
#include <map>

namespace helix::three_mf {

namespace {

/// Moonraker reports mtimes as float seconds, and a converter writing them
/// back through JSON may round the fraction.
constexpr double kMtimeToleranceS = 0.01;

} // namespace

std::vector<Conversion> parse_conversions(const nlohmann::json& result) {
    std::vector<Conversion> out;
    if (!result.is_object()) {
        return out;
    }
    auto* it = helix::json_util::find_array(result, "conversions");
    if (it == nullptr) {
        return out;
    }
    for (const auto& e : *it) {
        if (!e.is_object()) {
            continue;
        }
        Conversion c;
        c.source = json_util::safe_string(e, "source");
        c.source_mtime = json_util::safe_double(e, "source_mtime");
        c.source_size = json_util::safe_uint64(e, "source_size");
        c.plate = json_util::safe_int(e, "plate");
        c.plate_md5 = json_util::safe_string(e, "plate_md5");
        c.output = json_util::safe_string(e, "output");
        c.output_md5 = json_util::safe_string(e, "output_md5");
        c.state = json_util::safe_string(e, "state");
        c.error = json_util::safe_string(e, "error");
        if (c.source.empty()) {
            continue;
        }
        out.push_back(std::move(c));
    }
    return out;
}

namespace {

/// Whether @p c is a finished conversion of exactly the archive listed now,
/// whose output is listed too.
bool current_and_listed(const Conversion& c,
                        const std::map<std::string, const ListedFile*>& by_path) {
    if (c.state != "done" || c.output.empty()) {
        return false;
    }
    const auto source = by_path.find(c.source);
    if (source == by_path.end() || by_path.find(c.output) == by_path.end()) {
        return false;
    }
    return source->second->size == c.source_size &&
           std::fabs(source->second->modified - c.source_mtime) <= kMtimeToleranceS;
}

std::map<std::string, const ListedFile*> index_listing(const std::vector<ListedFile>& listed) {
    std::map<std::string, const ListedFile*> by_path;
    for (const auto& f : listed) {
        by_path[f.path] = &f;
    }
    return by_path;
}

} // namespace

std::set<std::string> converted_archives(const std::vector<Conversion>& conversions,
                                         const std::vector<ListedFile>& listed) {
    const auto by_path = index_listing(listed);
    // One entry per sliced plate: the archive hides only when every plate's is current.
    std::map<std::string, bool> all_current;
    for (const auto& c : conversions) {
        const bool ok = current_and_listed(c, by_path);
        auto [it, inserted] = all_current.emplace(c.source, ok);
        if (!inserted) {
            it->second = it->second && ok;
        }
    }
    std::set<std::string> hidden;
    for (const auto& [source, ok] : all_current) {
        if (ok) {
            hidden.insert(source);
        }
    }
    return hidden;
}

std::map<int, std::string> current_outputs(const std::vector<Conversion>& conversions,
                                           const std::vector<ListedFile>& listed,
                                           const std::string& source) {
    const auto by_path = index_listing(listed);
    std::map<int, std::string> outputs;
    for (const auto& c : conversions) {
        if (c.source == source && c.plate > 0 && current_and_listed(c, by_path)) {
            outputs[c.plate] = c.output;
        }
    }
    return outputs;
}

} // namespace helix::three_mf
