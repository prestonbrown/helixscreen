// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_event_journal.h"

#include "core/text_io.h"
#include "json_utils.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace helix {

namespace {

std::string lowercase(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool contains_any(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_firmware_envelope(const std::string& text) {
    if (text.empty() || text.front() != '{') {
        return false;
    }
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    return parsed.is_object() && parsed.contains("code");
}

struct ClassMarkers {
    const char* cls;
    std::initializer_list<const char*> markers;
};

// Ordered: the first class with a matching marker wins. Heater faults come
// before MCU ones because Klipper reports an ADC fault as an MCU shutdown.
const ClassMarkers kClassMarkers[] = {
    {"heater",
     {"heater", "adc out of range", "thermocouple", "thermistor", "not heating at expected rate",
      "thermal runaway"}},
    {"mcu", {"mcu", "timer too close", "missed scheduling", "rescheduled timer"}},
    {"homing", {"must home", "no trigger", "endstop", "probe triggered", "homing"}},
    {"move",
     {"move out of range", "move exceeds", "extrude below minimum", "extrude only move too long"}},
    {"filament", {"filament", "runout"}},
    {"command", {"unknown command", "error evaluating", "malformed command"}},
};

double latest_occurrence(const JournalEntry& e) {
    return e.count > 1 && e.last_time > e.time ? e.last_time : e.time;
}

bool same_event(const JournalEntry& a, const JournalEntry& b) {
    // gcode_store backfill knows no job, so an entry without one matches a
    // live entry of any job.
    const bool same_job = a.job_id == b.job_id || a.job_id.empty() || b.job_id.empty();
    // A counted entry stands for every occurrence from its time to its last.
    const bool in_span = b.time >= a.time - kJournalDedupWindowS &&
                         b.time <= latest_occurrence(a) + kJournalDedupWindowS;
    return a.kind == b.kind && a.text == b.text && a.detail == b.detail && same_job && in_span;
}

/// The number at @p pos, or nullopt when there is none.
std::optional<double> number_at(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) {
        return std::nullopt;
    }
    const char* begin = s.c_str() + pos;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

std::string journal_error_text(const std::string& line) {
    std::string text = trim(line);
    if (text.rfind("!!", 0) == 0) {
        text = text.substr(2);
    }
    return trim(text);
}

bool is_mcu_disconnect(const std::string& line) {
    return contains_any(lowercase(journal_error_text(line)),
                        {"lost communication with mcu", "unable to connect"});
}

std::string classify_journal_error(const std::string& line) {
    const std::string text = journal_error_text(line);
    if (is_firmware_envelope(text)) {
        return "firmware";
    }
    if (is_mcu_disconnect(text)) {
        return "mcu";
    }
    const std::string lowered = lowercase(text);
    for (const auto& entry : kClassMarkers) {
        if (contains_any(lowered, entry.markers)) {
            return entry.cls;
        }
    }
    return "other";
}

double wall_now_s() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

bool is_recent_cause(double cause_time, double event_time) {
    return cause_time >= event_time - kEventCauseWindowS &&
           cause_time <= event_time + kEventCauseLateS;
}

std::string journal_pause_cause(const std::vector<JournalEntry>& entries, double now) {
    bool error = false;
    for (const auto& e : entries) {
        if (!is_recent_cause(e.time, now)) {
            continue;
        }
        if (e.kind == journal_kind::kRunout) {
            return "runout";
        }
        if (e.kind == journal_kind::kGcodeError || e.kind == journal_kind::kMcuDisconnect) {
            error = true;
        }
    }
    return error ? "error" : "unknown";
}

// ---------------------------------------------------------------------------
// Source decision
// ---------------------------------------------------------------------------

JournalPlan decide_journal_source(PluginFeatureState state, bool recorder_built) {
    if (state == PluginFeatureState::Ready) {
        return {JournalSource::Plugin, false};
    }
    if (!recorder_built) {
        return {JournalSource::None, false};
    }
    return {JournalSource::Screen, true};
}

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

std::optional<std::size_t> journal_repeat_of(const std::vector<JournalEntry>& entries,
                                             const JournalEntry& e) {
    if (e.kind != journal_kind::kGcodeError) {
        return std::nullopt;
    }
    const std::size_t first =
        entries.size() > kJournalRepeatLookback ? entries.size() - kJournalRepeatLookback : 0;
    for (std::size_t i = entries.size(); i-- > first;) {
        const JournalEntry& held = entries[i];
        if (held.kind == e.kind && held.text == e.text && held.cls == e.cls &&
            held.detail == e.detail && held.job_id == e.job_id && held.filename == e.filename &&
            e.time - latest_occurrence(held) <= kJournalRepeatWindowS) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {

/// Counts @p e on the entry it repeats; false when it repeats none.
bool count_repeat(std::vector<JournalEntry>& entries, const JournalEntry& e) {
    const auto index = journal_repeat_of(entries, e);
    if (!index) {
        return false;
    }
    JournalEntry& held = entries[*index];
    held.last_time = std::max(latest_occurrence(held), e.time);
    held.count += 1;
    return true;
}

} // namespace

std::optional<JournalEntry> journal_entry_from_json(const nlohmann::json& j) {
    using namespace json_util;
    if (!j.is_object()) {
        return std::nullopt;
    }
    JournalEntry e;
    e.kind = safe_string(j, "kind");
    const nlohmann::json* time = find_member(j, "time");
    if (e.kind.empty() || time == nullptr || !time->is_number()) {
        return std::nullopt;
    }
    e.time = as_double(*time);
    e.id = safe_int64(j, "id");
    e.text = safe_string(j, "text");
    e.cls = safe_string(j, "class");
    e.detail = safe_string(j, "detail");
    e.recovered = safe_bool(j, "recovered");
    e.count = std::max(1, safe_int(j, "count", 1));
    if (e.count > 1) {
        e.last_time = safe_double(j, "last_time", e.time);
    }
    if (const nlohmann::json* job = find_member(j, "job"); job != nullptr && job->is_object()) {
        if (const nlohmann::json* id = find_member(*job, "job_id")) {
            e.job_id = as_string(*id, "", /*accept_number=*/true);
        }
        e.filename = safe_string(*job, "filename");
    }
    return e;
}

nlohmann::json journal_entry_to_json(const JournalEntry& e) {
    nlohmann::json j = {{"id", e.id},     {"time", e.time},          {"kind", e.kind},
                        {"text", e.text}, {"class", e.cls},          {"detail", e.detail},
                        {"job", nullptr}, {"recovered", e.recovered}};
    if (!e.job_id.empty() || !e.filename.empty()) {
        j["job"] = {{"job_id", e.job_id}, {"filename", e.filename}};
    }
    if (e.count > 1) {
        j["count"] = e.count;
        j["last_time"] = latest_occurrence(e);
    }
    return j;
}

std::vector<JournalEntry> parse_journal_result(const nlohmann::json& result) {
    std::vector<JournalEntry> out;
    const nlohmann::json* entries = json_util::find_member(result, "entries");
    if (entries == nullptr || !entries->is_array()) {
        return out;
    }
    for (const auto& item : *entries) {
        if (auto e = journal_entry_from_json(item)) {
            out.push_back(std::move(*e));
        }
    }
    return out;
}

nlohmann::json build_event_journal_section(JournalSource source,
                                           const std::vector<JournalEntry>& entries) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : entries) {
        list.push_back(journal_entry_to_json(e));
    }
    return {{"source", source == JournalSource::Plugin   ? "plugin"
                       : source == JournalSource::Screen ? "screen"
                                                         : "none"},
            {"entries", std::move(list)}};
}

// ---------------------------------------------------------------------------
// Backfill
// ---------------------------------------------------------------------------

std::vector<JournalEntry> backfill_from_gcode_store(const std::vector<GcodeStoreEntry>& store,
                                                    double since) {
    std::vector<JournalEntry> out;
    for (const auto& item : store) {
        if (item.type != "response" || item.time <= since || item.message.rfind("!!", 0) != 0) {
            continue;
        }
        JournalEntry e;
        e.time = item.time;
        e.kind = journal_kind::kGcodeError;
        e.text = journal_error_text(item.message);
        e.cls = classify_journal_error(item.message);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<JournalEntry> backfill_from_history(const nlohmann::json& result, double since) {
    using namespace json_util;
    std::vector<JournalEntry> out;
    const nlohmann::json* jobs = find_member(result, "jobs");
    if (jobs == nullptr || !jobs->is_array()) {
        return out;
    }
    for (const auto& job : *jobs) {
        const std::string status = safe_string(job, "status");
        const double end_time = safe_double(job, "end_time");
        if (status.empty() || status == "in_progress" || end_time <= since) {
            continue;
        }
        JournalEntry e;
        e.time = end_time;
        e.kind = journal_kind::kJobEnd;
        e.detail = status;
        if (const nlohmann::json* id = find_member(job, "job_id")) {
            e.job_id = as_string(*id, "", /*accept_number=*/true);
        }
        e.filename = safe_string(job, "filename");
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const JournalEntry& a, const JournalEntry& b) { return a.time < b.time; });
    return out;
}

std::vector<JournalEntry> backfill_from_klippy_log(const std::string& log, double since) {
    static constexpr const char* kStart = "Start printer at ";
    static constexpr const char* kStats = "Stats ";
    static constexpr const char* kShutdown = "Transition to shutdown state: ";

    std::vector<JournalEntry> out;
    std::optional<double> start_wall;
    std::optional<double> start_mono;
    std::optional<double> last_stats;

    for (std::size_t pos = 0; pos < log.size();) {
        const std::size_t eol = std::min(log.find('\n', pos), log.size());
        const std::string line = log.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.rfind(kStart, 0) == 0) {
            const auto open = line.rfind('(');
            const auto wall = number_at(line, open == std::string::npos ? line.size() : open + 1);
            std::optional<double> mono;
            if (wall) {
                const auto space = line.find(' ', open + 1);
                mono = number_at(line, space == std::string::npos ? line.size() : space + 1);
            }
            start_wall = mono ? wall : std::nullopt;
            start_mono = mono;
            last_stats.reset();
        } else if (line.rfind(kStats, 0) == 0) {
            if (auto mono = number_at(line, std::char_traits<char>::length(kStats))) {
                last_stats = mono;
            }
        } else if (line.rfind(kShutdown, 0) == 0) {
            if (!start_wall || !start_mono) {
                continue;
            }
            const double when = *start_wall + (last_stats ? *last_stats - *start_mono : 0.0);
            const std::string message =
                trim(line.substr(std::char_traits<char>::length(kShutdown)));
            if (when <= since || message.empty()) {
                continue;
            }
            JournalEntry e;
            e.time = when;
            e.kind =
                is_mcu_disconnect(message) ? journal_kind::kMcuDisconnect : journal_kind::kShutdown;
            e.text = message;
            e.cls = classify_journal_error(message);
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool backfill_needs_klippy_log(bool klippy_shutdown_now,
                               const std::vector<JournalEntry>& history_entries) {
    if (klippy_shutdown_now) {
        return true;
    }
    return std::any_of(history_entries.begin(), history_entries.end(), [](const JournalEntry& e) {
        return e.kind == journal_kind::kJobEnd && e.detail == "klippy_shutdown";
    });
}

// ---------------------------------------------------------------------------
// LocalEventJournal
// ---------------------------------------------------------------------------

LocalEventJournal& LocalEventJournal::instance() {
    static LocalEventJournal journal;
    return journal;
}

LocalEventJournal::LocalEventJournal(std::size_t max_entries, double max_age_s)
    : max_entries_(max_entries), max_age_s_(max_age_s) {}

bool LocalEventJournal::contains_duplicate_locked(const JournalEntry& e) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&e](const JournalEntry& held) { return same_event(held, e); });
}

void LocalEventJournal::insert_sorted_locked(JournalEntry e) {
    const auto pos =
        std::upper_bound(entries_.begin(), entries_.end(), e.time,
                         [](double time, const JournalEntry& held) { return time < held.time; });
    entries_.insert(pos, std::move(e));
}

void LocalEventJournal::prune_locked(double now) {
    const double cutoff = now - max_age_s_;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [cutoff](const JournalEntry& e) { return e.time < cutoff; }),
                   entries_.end());
    if (entries_.size() > max_entries_) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(
                                                                entries_.size() - max_entries_));
    }
}

bool LocalEventJournal::append(JournalEntry entry, double now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A live line is seen once, so a match is a repeat before it is a duplicate.
    if (count_repeat(entries_, entry)) {
        ++version_;
        return true;
    }
    if (contains_duplicate_locked(entry)) {
        return false;
    }
    insert_sorted_locked(std::move(entry));
    prune_locked(now);
    ++version_;
    return true;
}

std::size_t LocalEventJournal::merge_backfill(std::vector<JournalEntry> entries, double now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t added = 0;
    for (auto& e : entries) {
        e.recovered = true;
        if (contains_duplicate_locked(e)) {
            continue;
        }
        if (!count_repeat(entries_, e)) {
            insert_sorted_locked(std::move(e));
        }
        ++added;
    }
    if (added > 0) {
        prune_locked(now);
        ++version_;
    }
    return added;
}

std::vector<JournalEntry> LocalEventJournal::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

double LocalEventJournal::latest_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty() ? 0.0 : entries_.back().time;
}

std::uint32_t LocalEventJournal::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

nlohmann::json LocalEventJournal::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : entries_) {
        list.push_back(journal_entry_to_json(e));
    }
    return {{"entries", std::move(list)}};
}

bool LocalEventJournal::load_json(const nlohmann::json& j, double now) {
    const nlohmann::json* list = json_util::find_member(j, "entries");
    if (list == nullptr || !list->is_array()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    for (const auto& item : *list) {
        if (auto e = journal_entry_from_json(item)) {
            insert_sorted_locked(std::move(*e));
        }
    }
    prune_locked(now);
    ++version_;
    return true;
}

bool LocalEventJournal::load_file(const std::string& path, double now) {
    const auto body = text_io::read_file(path, 4 * 1024 * 1024);
    if (!body) {
        return false;
    }
    const auto parsed = nlohmann::json::parse(*body, nullptr, false);
    if (parsed.is_discarded()) {
        spdlog::warn("[EventJournal] {} is not JSON, starting empty", path);
        return false;
    }
    return load_json(parsed, now);
}

bool LocalEventJournal::save_file(const std::string& path) const {
    const std::string body = json_util::safe_dump(to_json());
    if (!text_io::write_file_atomic(path, body)) {
        spdlog::warn("[EventJournal] Could not write {}", path);
        return false;
    }
    return true;
}

void LocalEventJournal::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    ++version_;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

JournalRow describe_journal_entry(const JournalEntry& e) {
    using namespace journal_kind;
    JournalRow row;
    const std::string& k = e.kind;
    if (k == kShutdown) {
        row = {JournalSeverity::Error, TR_NOOP("Klipper shut down"), e.text};
    } else if (k == kMcuDisconnect) {
        row = {JournalSeverity::Error, TR_NOOP("MCU disconnected"), e.text};
    } else if (k == kDisconnect) {
        row = {JournalSeverity::Warning, TR_NOOP("Klipper disconnected"), {}};
    } else if (k == kReady) {
        row = {JournalSeverity::Success, TR_NOOP("Klipper ready"), {}};
    } else if (k == kFirmwareRestart) {
        row = {JournalSeverity::Info, TR_NOOP("Firmware restart"), {}};
    } else if (k == kGcodeError) {
        row = {JournalSeverity::Error, TR_NOOP("Printer error"), e.text};
    } else if (k == kRunout) {
        row = {JournalSeverity::Warning, TR_NOOP("Filament runout"), e.detail};
    } else if (k == kPause) {
        row = {JournalSeverity::Warning, TR_NOOP("Print paused"), {}};
        if (e.detail == "runout") {
            row.message_tag = TR_NOOP("Filament runout");
        } else if (e.detail == "error") {
            row.message_tag = TR_NOOP("After an error");
        } else if (e.detail == "command") {
            row.message_tag = TR_NOOP("Paused by a command");
        }
    } else if (k == kJobEnd) {
        if (e.detail == "completed") {
            row = {JournalSeverity::Success, TR_NOOP("Print completed"), {}};
        } else if (e.detail == "cancelled") {
            row = {JournalSeverity::Info, TR_NOOP("Print canceled"), {}};
        } else {
            row = {JournalSeverity::Error, TR_NOOP("Print failed"), e.detail};
            if (e.detail == "klippy_shutdown") {
                row.message_tag = TR_NOOP("Klipper shut down");
            } else if (e.detail == "klippy_disconnect") {
                row.message_tag = TR_NOOP("Klipper disconnected");
            } else if (e.detail == "interrupted" || e.detail == "server_exit") {
                row.message_tag = TR_NOOP("Interrupted");
            }
        }
    } else {
        row = {JournalSeverity::Info, TR_NOOP("Printer event"), e.text};
    }
    row.repeat_count = std::max(1, e.count);
    return row;
}

std::vector<JournalEntry> journal_view_order(std::vector<JournalEntry> entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const JournalEntry& a, const JournalEntry& b) { return a.time > b.time; });
    return entries;
}

} // namespace helix
