// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "resonance_console.h"

#include "core/text_io.h"
#include "helix_regex.h"
#include "i_moonraker_client.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace helix::calibration {

float ResonanceTesterConfig::sweep_seconds() const {
    return (max_freq - min_freq) / hz_per_sec;
}

void query_resonance_tester_config(IMoonrakerClient& client,
                                   std::function<void(ResonanceTesterConfig)> on_done) {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};
    client.send_jsonrpc(
        "printer.objects.query", params,
        [on_done](const json& response) {
            if (!response.contains("result") || !response["result"].contains("status") ||
                !response["result"]["status"].contains("configfile") ||
                !response["result"]["status"]["configfile"].contains("settings")) {
                on_done(ResonanceTesterConfig{});
                return;
            }
            on_done(parse_resonance_tester_config(
                response["result"]["status"]["configfile"]["settings"]));
        },
        [on_done](const MoonrakerError&) { on_done(ResonanceTesterConfig{}); });
}

namespace {

/// "x, y, z" from the first non-empty line of a probe_points string.
std::optional<std::array<float, 3>> parse_probe_point_text(std::string_view text) {
    std::string_view line;
    for (const std::string_view candidate : text_io::lines(text)) {
        line = text_io::trim(candidate);
        if (!line.empty()) {
            break;
        }
    }
    std::array<float, 3> point{};
    size_t filled = 0;
    for (const std::string_view field : text_io::lines(line, ',')) {
        if (filled == point.size()) {
            break;
        }
        const auto value = text_io::parse_leading<float>(field);
        if (!value) {
            return std::nullopt;
        }
        point[filled++] = *value;
    }
    if (filled < point.size()) {
        return std::nullopt;
    }
    return point;
}

/// The first point of configfile's probe_points: [[x, y, z], ...] or a string.
std::optional<std::array<float, 3>> parse_probe_point(const nlohmann::json& value) {
    if (value.is_string()) {
        return parse_probe_point_text(value.get_ref<const std::string&>());
    }
    if (!value.is_array() || value.empty()) {
        return std::nullopt;
    }
    const auto& first = value[0];
    if (first.is_string()) {
        return parse_probe_point_text(first.get_ref<const std::string&>());
    }
    if (!first.is_array() || first.size() != 3) {
        return std::nullopt;
    }
    std::array<float, 3> point{};
    for (size_t i = 0; i < point.size(); ++i) {
        if (!first[i].is_number()) {
            return std::nullopt;
        }
        point[i] = first[i].get<float>();
    }
    return point;
}

} // namespace

ResonanceTesterConfig parse_resonance_tester_config(const nlohmann::json& settings) {
    ResonanceTesterConfig cfg;
    const auto rt = settings.find("resonance_tester");
    if (rt == settings.end() || !rt->is_object()) {
        return cfg;
    }
    // configfile reports numbers, but some forks echo strings — accept both
    // rather than silently keeping defaults. A value that is present but
    // unparseable rejects the whole config: a range half-read from a
    // malformed config would arm the sweep-end check against a guessed
    // ceiling, so the caller gets clean defaults instead.
    auto read = [&rt](const char* key, float fallback) -> std::optional<float> {
        const auto it = rt->find(key);
        if (it == rt->end()) {
            return fallback;
        }
        if (it->is_number()) {
            return it->get<float>();
        }
        if (it->is_string()) {
            return text_io::parse_leading<float>(it->get_ref<const std::string&>());
        }
        return fallback;
    };
    const auto min_freq = read("min_freq", cfg.min_freq);
    const auto max_freq = read("max_freq", cfg.max_freq);
    const auto hz_per_sec = read("hz_per_sec", cfg.hz_per_sec);
    if (!min_freq || !max_freq || !hz_per_sec) {
        return {};
    }
    cfg.min_freq = *min_freq;
    cfg.max_freq = *max_freq;
    cfg.hz_per_sec = *hz_per_sec;
    cfg.from_printer = true;
    if (const auto pp = rt->find("probe_points"); pp != rt->end()) {
        cfg.probe_point = parse_probe_point(*pp);
    }
    auto read_chip = [&rt](const char* key) -> std::string {
        const auto it = rt->find(key);
        if (it == rt->end() || !it->is_string()) {
            return {};
        }
        return std::string(text_io::trim(it->get_ref<const std::string&>()));
    };
    cfg.accel_chip = read_chip("accel_chip");
    cfg.accel_chip_x = read_chip("accel_chip_x");
    cfg.accel_chip_y = read_chip("accel_chip_y");
    return cfg;
}

std::optional<float> parse_testing_frequency(const std::string& line) {
    static const helix::Regex freq_regex(R"(Testing frequency ([\d.]+) Hz)");
    helix::RegexMatch match;
    if (helix::regex_search(line, match, freq_regex) && match.size() == 2) {
        return text_io::parse_leading<float>(match[1].str());
    }
    return std::nullopt;
}

std::optional<std::string> parse_written_csv_path(const std::string& line) {
    static const helix::Regex csv_regex(R"(data written to (\S+\.csv))");
    helix::RegexMatch match;
    if (helix::regex_search(line, match, csv_regex) && match.size() == 2) {
        return match[1].str();
    }
    return std::nullopt;
}

int sweep_percent(float freq, const ResonanceTesterConfig& cfg) {
    const float range = cfg.max_freq - cfg.min_freq;
    const float progress_frac = (range > 0) ? (freq - cfg.min_freq) / range : 0.0f;
    const int percent = static_cast<int>(std::lround(progress_frac * 100.0f));
    return std::clamp(percent, 0, 100);
}

} // namespace helix::calibration
