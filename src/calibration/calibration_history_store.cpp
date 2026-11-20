// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_history_store.h"

#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix::calibration {

using json = nlohmann::json;

std::optional<std::tm> parse_iso_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int parts[3] = {0, 0, 0};
    const size_t starts[3] = {0, 5, 8};
    const size_t lengths[3] = {4, 2, 2};
    for (int p = 0; p < 3; ++p) {
        for (size_t k = 0; k < lengths[p]; ++k) {
            const char c = text[starts[p] + k];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            parts[p] = parts[p] * 10 + (c - '0');
        }
    }
    const int year = parts[0];
    const int month = parts[1];
    const int day = parts[2];
    static constexpr int kDaysInMonth[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1 || day > kDaysInMonth[month - 1]) {
        return std::nullopt;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && day == 29 && !leap) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    return tm;
}

std::string iso_date(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

json history_to_json(const History& h) {
    json steps = json::object();
    for (const auto& [id, e] : h) {
        steps[id] = {{"date", e.date}, {"summary", e.summary}, {"values", e.values}};
    }
    return {{"version", kHistorySchemaVersion}, {"steps", steps}};
}

std::optional<History> history_from_json(const json& j) {
    if (!j.is_object() || json_util::read_integer_field(j, "version") != kHistorySchemaVersion) {
        return std::nullopt;
    }
    const json* steps = json_util::find_member(j, "steps");
    if (!steps || !steps->is_object()) {
        return std::nullopt;
    }
    History out;
    for (const auto& [id, e] : steps->items()) {
        if (!e.is_object()) {
            return std::nullopt;
        }
        const json* date = json_util::find_member(e, "date");
        const json* summary = json_util::find_member(e, "summary");
        const json* values = json_util::find_member(e, "values");
        if (!date || !date->is_string() || !parse_iso_date(date->get<std::string>()) || !summary ||
            !summary->is_string() || !values || !values->is_object()) {
            return std::nullopt;
        }
        out[id] = HistoryEntry{date->get<std::string>(), summary->get<std::string>(), *values};
    }
    return out;
}

CalibrationHistory& CalibrationHistory::instance() {
    static CalibrationHistory history;
    return history;
}

void CalibrationHistory::load(IMoonrakerAPI& api) {
    const uint64_t gen = ++generation_;
    loading_ = true;
    IMoonrakerAPI* apip = &api;
    api.database_get_item(
        kHistoryDbNamespace, kHistoryDbKey,
        lifetime_.bg_cb("CalibrationHistory::load",
                        [this, gen, apip](const json& value) {
                            if (gen != generation_) {
                                return;
                            }
                            merge_loaded(*apip, history_from_json(value));
                        }),
        lifetime_.bg_cb("CalibrationHistory::load_none",
                        [this, gen, apip](const MoonrakerError& err) {
                            if (gen != generation_) {
                                return;
                            }
                            spdlog::debug("[CalibrationHistory] No saved history: {}", err.message);
                            merge_loaded(*apip, std::nullopt);
                        }));
}

void CalibrationHistory::merge_loaded(IMoonrakerAPI& api, std::optional<History> loaded) {
    loading_ = false;
    history_ = loaded ? std::move(*loaded) : History{};
    spdlog::debug("[CalibrationHistory] Loaded {} step result(s)", history_.size());
    if (pending_.empty()) {
        return;
    }
    for (auto& [id, e] : pending_) {
        history_[id] = std::move(e);
    }
    pending_.clear();
    save(api);
}

void CalibrationHistory::record(IMoonrakerAPI& api, const std::string& step_id,
                                const std::string& summary, const json& values) {
    HistoryEntry e{iso_date(std::time(nullptr)), summary,
                   values.is_object() ? values : json::object()};
    history_[step_id] = e;
    if (loading_) {
        // Writing now would replace the stored history with this one entry.
        pending_[step_id] = std::move(e);
        return;
    }
    save(api);
}

void CalibrationHistory::save(IMoonrakerAPI& api) {
    api.database_post_item(kHistoryDbNamespace, kHistoryDbKey, history_to_json(history_), nullptr,
                           [](const MoonrakerError& err) {
                               spdlog::warn("[CalibrationHistory] Could not save: {}", err.message);
                           });
}

void CalibrationHistory::clear() {
    ++generation_;
    loading_ = false;
    history_.clear();
    pending_.clear();
}

const HistoryEntry* CalibrationHistory::entry(const std::string& step_id) const {
    const auto it = history_.find(step_id);
    return it == history_.end() ? nullptr : &it->second;
}

} // namespace helix::calibration
