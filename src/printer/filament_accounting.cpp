// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_accounting.h"

#include "ui_format_utils.h"

#include "display_numbering.h"
#include "format_utils.h"
#include "json_utils.h"
#include "lvgl.h"
#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace helix {

using nlohmann::json;

ActiveSpoolOwner active_spool_owner(bool spoolman, bool manages_active_spool,
                                    bool printer_switches_active_spool,
                                    bool macros_set_active_spool) {
    if (!spoolman) {
        return ActiveSpoolOwner::NoSpoolman;
    }
    if (manages_active_spool) {
        return ActiveSpoolOwner::Backend;
    }
    if (printer_switches_active_spool) {
        return ActiveSpoolOwner::Printer;
    }
    if (macros_set_active_spool) {
        return ActiveSpoolOwner::Macros;
    }
    return ActiveSpoolOwner::Helix;
}

bool app_sets_spool_on_edit(ActiveSpoolOwner owner, bool backend_sets_on_link) {
    switch (owner) {
    case ActiveSpoolOwner::Helix:
    case ActiveSpoolOwner::Macros:
        return true;
    case ActiveSpoolOwner::Backend:
        return !backend_sets_on_link;
    case ActiveSpoolOwner::Printer:
    case ActiveSpoolOwner::NoSpoolman:
        return false;
    }
    return false;
}

const char* active_spool_owner_name(ActiveSpoolOwner owner) {
    switch (owner) {
    case ActiveSpoolOwner::NoSpoolman:
        return "no_spoolman";
    case ActiveSpoolOwner::Backend:
        return "backend";
    case ActiveSpoolOwner::Printer:
        return "printer";
    case ActiveSpoolOwner::Macros:
        return "macros";
    case ActiveSpoolOwner::Helix:
        return "helix";
    }
    return "helix";
}

std::string feed_signal_key(const json& value) {
    if (value.is_number_integer()) {
        return std::to_string(json_util::as_int64(value));
    }
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (!std::isfinite(d) || d != std::trunc(d)) {
            return {};
        }
        return std::to_string(static_cast<long long>(d));
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return {};
}

namespace {

json feed_json(const FeedEntry& feed) {
    json j = {{"tool", feed.tool},
              {"lane", feed.lane},
              {"spool_id", feed.spool_id > 0 ? json(feed.spool_id) : json(nullptr)}};
    if (feed.grams_per_mm > 0.0f) {
        j["grams_per_mm"] = feed.grams_per_mm;
    }
    return j;
}

} // namespace

bool printer_switches_active_spool(const PrinterDiscovery& hw) {
    return hw.has_spoollink();
}

json filament_spools_record(bool manages_active_spool, bool printer_switches_active_spool,
                            bool macros_set_active_spool, const std::vector<FeedSignal>& signals) {
    json list = json::array();
    for (const FeedSignal& sig : signals) {
        json values = json::object();
        for (const auto& [key, feed] : sig.values) {
            values[key] = feed_json(feed);
        }
        list.push_back({{"object", sig.object}, {"path", sig.path}, {"values", values}});
    }
    return {{"version", kFilamentSpoolsVersion},
            {"manages_active_spool", manages_active_spool},
            {"printer_switches_active_spool", printer_switches_active_spool},
            {"macros_set_active_spool", macros_set_active_spool},
            {"signals", list}};
}

std::optional<FeedSignal> extruder_feed_signal(const std::vector<std::string>& tool_extruders,
                                               const std::vector<FeedEntry>& feeds) {
    std::map<std::string, int> uses;
    for (const std::string& name : tool_extruders) {
        if (!name.empty()) {
            ++uses[name];
        }
    }
    FeedSignal sig{"toolhead", {"extruder"}, {}};
    for (size_t i = 0; i < tool_extruders.size() && i < feeds.size(); ++i) {
        const std::string& name = tool_extruders[i];
        if (!name.empty() && uses[name] == 1) {
            sig.values.emplace_back(name, feeds[i]);
        }
    }
    if (sig.values.empty()) {
        return std::nullopt;
    }
    return sig;
}

bool is_tool_select_macro(const std::string& name) {
    if (name.size() < 2 || (name[0] != 'T' && name[0] != 't')) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

SpoolSwitch decide_spool_switch(const FeedEntry* feed, int active_spool,
                                const std::vector<int>& mapped) {
    if (feed == nullptr) {
        return {};
    }
    if (feed->spool_id > 0) {
        if (feed->spool_id == active_spool) {
            return {};
        }
        return {SpoolSwitch::Action::Set, feed->spool_id};
    }
    if (active_spool > 0 && std::find(mapped.begin(), mapped.end(), active_spool) != mapped.end()) {
        return {SpoolSwitch::Action::Clear, 0};
    }
    return {};
}

FilamentAccountingSender filament_accounting_sender(PluginFeatureState accounting, bool opted_in,
                                                    ActiveSpoolOwner owner,
                                                    FallbackSenderRole role) {
    if (owner != ActiveSpoolOwner::Helix) {
        return FilamentAccountingSender::None;
    }
    switch (accounting) {
    case PluginFeatureState::Ready:
        return FilamentAccountingSender::Plugin;
    case PluginFeatureState::Disabled:
        return FilamentAccountingSender::None;
    case PluginFeatureState::Absent:
        break;
    }
    if (opted_in && role == FallbackSenderRole::Sender) {
        return FilamentAccountingSender::App;
    }
    return FilamentAccountingSender::None;
}

bool writes_filament_spools_record(FallbackSenderRole role,
                                   const std::vector<FeedSignal>& signals) {
    return role == FallbackSenderRole::Sender && !signals.empty();
}

bool filament_usage_available(PluginFeatureState accounting) {
    return accounting != PluginFeatureState::Absent;
}

std::vector<SpoolUsage> parse_filament_usage(const json& reply) {
    std::vector<SpoolUsage> out;
    const json* record = json_util::find_member(reply, "record");
    if (record == nullptr || !record->is_object()) {
        return out;
    }
    const json* spools = json_util::find_member(*record, "spools");
    if (spools == nullptr || !spools->is_array()) {
        return out;
    }
    for (const json& s : *spools) {
        if (!s.is_object()) {
            continue;
        }
        SpoolUsage u;
        u.tool = json_util::safe_int(s, "tool", -1);
        u.lane = json_util::safe_int(s, "lane", -1);
        u.spool_id = std::max(0, json_util::safe_int(s, "spool_id", 0));
        u.mm = json_util::safe_double(s, "mm", 0.0);
        if (const json* g = json_util::find_member(s, "grams"); g != nullptr && g->is_number()) {
            u.grams = g->get<double>();
        }
        out.push_back(u);
    }
    return out;
}

std::string describe_spool_usage(const std::vector<SpoolUsage>& usage,
                                 const std::function<std::string(int)>& spool_name,
                                 ui::LaneNoun noun) {
    const std::string separator = std::string(" ") + ui::kUnitSeparator + " ";
    std::string text;
    for (const SpoolUsage& u : usage) {
        const std::string who = u.tool >= 0 ? ui::tool_label(u.tool) : ui::lane_label(noun, u.lane);
        std::string spool;
        if (u.spool_id > 0) {
            spool = spool_name ? spool_name(u.spool_id) : std::string();
            if (spool.empty()) {
                spool = fmt::format(fmt::runtime(lv_tr("Spool #{}")), u.spool_id);
            }
        } else {
            spool = lv_tr("No spool linked");
        }
        const std::string amount = u.grams
                                       ? ui::format_filament_weight(static_cast<float>(*u.grams))
                                       : format::format_filament_length(u.mm);
        if (!text.empty()) {
            text += '\n';
        }
        if (!who.empty()) {
            text += who + separator;
        }
        text += spool + separator + amount;
    }
    return text;
}

} // namespace helix
