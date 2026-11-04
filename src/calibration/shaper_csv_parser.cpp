// SPDX-License-Identifier: GPL-3.0-or-later

#include "shaper_csv_parser.h"

#include "core/text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace helix {
namespace calibration {

namespace {

/// Split a CSV line into fields (simple comma-delimited, no quoting)
std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    for (std::string_view field : text_io::lines(line, ',')) {
        fields.push_back(std::string(helix::text_io::trim(field)));
    }
    return fields;
}

/// Parse a shaper column header like "mzv(53.8)" into name and frequency
bool parse_shaper_header(const std::string& header, std::string& name, float& freq) {
    auto paren_pos = header.find('(');
    if (paren_pos == std::string::npos)
        return false;

    auto close_pos = header.find(')', paren_pos);
    if (close_pos == std::string::npos)
        return false;

    name = header.substr(0, paren_pos);
    std::string freq_str = header.substr(paren_pos + 1, close_pos - paren_pos - 1);

    char* end = nullptr;
    freq = std::strtof(freq_str.c_str(), &end);
    return end != freq_str.c_str() && freq > 0.0f;
}

} // anonymous namespace

ShaperCsvData parse_shaper_csv(const std::string& csv_path, char axis) {
    ShaperCsvData result;

    text_io::LineReader file(csv_path);
    if (!file) {
        spdlog::warn("shaper_csv_parser: cannot open file: {}", csv_path);
        return result;
    }

    // Read header line
    std::string header_line;
    if (!file.next(header_line)) {
        spdlog::warn("shaper_csv_parser: empty file: {}", csv_path);
        return result;
    }

    auto headers = split_csv_line(header_line);
    if (headers.empty()) {
        spdlog::warn("shaper_csv_parser: no columns in header: {}", csv_path);
        return result;
    }

    // Find column indices
    int freq_col = -1;
    int psd_col = -1;

    // Known non-shaper column names
    static const std::vector<std::string> known_columns = {"freq", "psd_x", "psd_y", "psd_z",
                                                           "psd_xyz"};

    // Target PSD column name based on axis
    std::string target_psd = (axis == 'Y' || axis == 'y') ? "psd_y" : "psd_x";

    // Parse shaper column headers: any column matching name(freq) pattern
    struct ShaperColumn {
        int col_index;
        std::string name;
        float frequency;
    };
    std::vector<ShaperColumn> shaper_columns;

    for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
        const auto& h = headers[i];
        if (h == "freq") {
            freq_col = i;
        } else if (h == target_psd) {
            psd_col = i;
        } else if (h == "shapers:") {
            // Legacy marker column - skip it, not an error
            continue;
        } else {
            // Try parsing as a shaper header like "mzv(53.8)"
            std::string name;
            float freq = 0.0f;
            if (parse_shaper_header(h, name, freq)) {
                shaper_columns.push_back({i, name, freq});
            }
        }
    }

    if (freq_col < 0) {
        spdlog::warn("shaper_csv_parser: no 'freq' column in: {}", csv_path);
        return result;
    }

    if (psd_col < 0) {
        spdlog::warn("shaper_csv_parser: no '{}' column in: {}", target_psd, csv_path);
        return result;
    }

    // Initialize shaper curves
    for (const auto& sc : shaper_columns) {
        ShaperResponseCurve curve;
        curve.name = sc.name;
        curve.frequency = sc.frequency;
        result.shaper_curves.push_back(std::move(curve));
    }

    // Parse data rows
    std::string line;
    while (file.next(line)) {
        const std::string trimmed(helix::text_io::trim(line));
        if (trimmed.empty())
            continue;

        auto fields = split_csv_line(line);
        if (fields.empty())
            continue;

        // Parse frequency
        if (freq_col >= static_cast<int>(fields.size()))
            continue;
        char* end = nullptr;
        float freq_val = std::strtof(fields[freq_col].c_str(), &end);
        if (end == fields[freq_col].c_str())
            continue;

        result.frequencies.push_back(freq_val);

        // Parse raw PSD
        if (psd_col < static_cast<int>(fields.size())) {
            float psd_val = std::strtof(fields[psd_col].c_str(), &end);
            result.raw_psd.push_back(psd_val);
        } else {
            result.raw_psd.push_back(0.0f);
        }

        // Parse shaper values — CSV contains transfer function coefficients (0.0-1.0),
        // multiply by raw PSD to get the shaped (attenuated) PSD for charting
        float raw_psd_val = result.raw_psd.back();
        for (size_t si = 0; si < shaper_columns.size(); ++si) {
            int col = shaper_columns[si].col_index;
            float val = 0.0f;
            if (col < static_cast<int>(fields.size()) && !fields[col].empty()) {
                val = std::strtof(fields[col].c_str(), &end);
            }
            result.shaper_curves[si].values.push_back(val * raw_psd_val);
        }
    }

    spdlog::debug("shaper_csv_parser: parsed {} frequency bins, {} shaper curves from {}",
                  result.frequencies.size(), result.shaper_curves.size(), csv_path);

    return result;
}

ResonanceCsvData parse_resonance_csv(const std::string& csv_path) {
    ResonanceCsvData result;

    text_io::LineReader file(csv_path);
    if (!file) {
        spdlog::warn("[ShaperCSV] cannot open resonance file: {}", csv_path);
        result.error = ResonanceCsvError::MISSING;
        return result;
    }

    std::string header_line;
    file.next(header_line);
    auto headers = split_csv_line(header_line);

    int freq_col = -1;
    int psd_col = -1;
    int psd_x_col = -1;
    int psd_y_col = -1;
    bool has_axis_psd = false;
    int other_cols = 0;
    for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
        const auto& h = headers[i];
        if (h == "freq") {
            freq_col = i;
        } else if (h == "psd_xyz") {
            psd_col = i;
        } else if (h == "psd_x" || h == "psd_y" || h == "psd_z") {
            has_axis_psd = true;
            if (h == "psd_x") {
                psd_x_col = i;
            } else if (h == "psd_y") {
                psd_y_col = i;
            }
        } else {
            ++other_cols;
        }
    }

    if (freq_col < 0 || psd_col < 0) {
        // Per-chip output (one column per accelerometer) has no summed psd_xyz;
        // which chip to use is a caller decision, not a guess this parser makes.
        if (freq_col >= 0 && !has_axis_psd && other_cols > 1) {
            spdlog::warn("[ShaperCSV] per-chip columns, no psd_xyz: {}", csv_path);
            result.error = ResonanceCsvError::MULTI_CHIP;
        } else {
            spdlog::warn("[ShaperCSV] no freq/psd_xyz column: {}", csv_path);
            result.error = ResonanceCsvError::NO_PSD_COLUMN;
        }
        return result;
    }

    std::string line;
    while (file.next(line)) {
        if (helix::text_io::trim(line).empty())
            continue;
        auto fields = split_csv_line(line);
        if (fields.size() < headers.size())
            continue;

        bool numeric = true;
        for (const auto& field : fields) {
            char* end = nullptr;
            std::strtof(field.c_str(), &end);
            if (field.empty() || end == field.c_str()) {
                numeric = false;
                break;
            }
        }
        if (!numeric)
            continue;

        char* end = nullptr;
        float freq_val = std::strtof(fields[freq_col].c_str(), &end);
        float psd_val = std::strtof(fields[psd_col].c_str(), &end);
        result.curve.emplace_back(freq_val, psd_val);
        if (psd_x_col >= 0 && psd_y_col >= 0) {
            const float x = std::strtof(fields[psd_x_col].c_str(), &end);
            const float y = std::strtof(fields[psd_y_col].c_str(), &end);
            result.in_plane.emplace_back(freq_val, x + y);
        }
    }

    if (result.curve.empty()) {
        spdlog::warn("[ShaperCSV] no data rows in resonance file: {}", csv_path);
        result.error = ResonanceCsvError::EMPTY;
        return result;
    }

    spdlog::debug("[ShaperCSV] parsed {} resonance bins from {}", result.curve.size(), csv_path);
    return result;
}

} // namespace calibration
} // namespace helix
