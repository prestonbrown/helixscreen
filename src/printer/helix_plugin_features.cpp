// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_features.h"

#include "json_utils.h"
#include "lvgl.h"

#include <algorithm>
#include <iterator>

namespace helix {

PluginFeatureMap parse_plugin_features(const nlohmann::json& status_result) {
    PluginFeatureMap features;
    const nlohmann::json* map = json_util::find_member(status_result, "features");
    if (map == nullptr || !map->is_object()) {
        return features;
    }
    for (auto it = map->begin(); it != map->end(); ++it) {
        const nlohmann::json& value = it.value();
        if (!value.is_object()) {
            continue;
        }
        PluginFeatureEntry entry;
        entry.version = json_util::safe_int(value, "version", 0);
        entry.available = json_util::safe_bool(value, "available", false);
        entry.enabled = json_util::safe_bool(value, "enabled", false);
        entry.reason = json_util::safe_string(value, "reason", "");
        if (const nlohmann::json* saving = json_util::find_member(value, "saving");
            saving != nullptr && saving->is_boolean()) {
            entry.saving = saving->get<bool>();
        }
        features[it.key()] = std::move(entry);
    }
    return features;
}

bool plugin_feature_reported(bool plugin_available, const PluginFeatureMap& features,
                             const std::string& name, int min_version) {
    const auto it = features.find(name);
    return plugin_available && it != features.end() && it->second.version >= min_version;
}

PluginFeatureState plugin_feature_state(bool plugin_available, const PluginFeatureMap& features,
                                        const std::string& name, int min_version) {
    if (!plugin_available) {
        return PluginFeatureState::Absent;
    }
    const auto it = features.find(name);
    if (it == features.end()) {
        return PluginFeatureState::Absent;
    }
    const PluginFeatureEntry& entry = it->second;
    if (entry.version < min_version || !entry.available) {
        return PluginFeatureState::Absent;
    }
    return entry.enabled ? PluginFeatureState::Ready : PluginFeatureState::Disabled;
}

bool plugin_feature_opted_in(const nlohmann::json& record, const std::string& name, bool acts) {
    const nlohmann::json* value = json_util::find_member(record, name.c_str());
    if (value != nullptr && value->is_boolean()) {
        return value->get<bool>();
    }
    return !acts;
}

PluginSwitchMode plugin_switch_mode(bool plugin_available, const PluginFeatureMap& features,
                                    const std::string& name, int min_version, bool has_fallback,
                                    bool is_local) {
    switch (plugin_feature_state(plugin_available, features, name, min_version)) {
    case PluginFeatureState::Ready:
    case PluginFeatureState::Disabled:
        return PluginSwitchMode::Switch;
    case PluginFeatureState::Absent:
        break;
    }
    if (plugin_feature_reported(plugin_available, features, name, min_version) && !has_fallback) {
        // Absent with a current entry: the plugin says it cannot run here,
        // and nothing else can.
        return PluginSwitchMode::Hidden;
    }
    // A plugin that answers but lacks the feature, or cannot run it, is the
    // plugin's to fix by an update; installing it again cannot help.
    const bool outdated = plugin_available;
    if (has_fallback) {
        // While the app's fallback can act, its switch shows.
        if (is_local) {
            return PluginSwitchMode::Switch;
        }
        return outdated ? PluginSwitchMode::FallbackUpdateHint
                        : PluginSwitchMode::FallbackInstallHint;
    }
    return outdated ? PluginSwitchMode::NeedsUpdate : PluginSwitchMode::NeedsPlugin;
}

FallbackSenderRole fallback_sender_role(bool is_local, std::optional<bool> local_screen_present) {
    if (is_local) {
        return FallbackSenderRole::Sender;
    }
    return local_screen_present == false ? FallbackSenderRole::Sender : FallbackSenderRole::Standby;
}

std::optional<nlohmann::json> opt_ins_after_failed_read(const MoonrakerError& err) {
    if (err.is_not_found()) {
        return nlohmann::json::object();
    }
    return std::nullopt;
}

nlohmann::json local_screen_presence_record(int64_t now) {
    return nlohmann::json{{"seen_at", now}};
}

std::optional<bool> local_screen_present(const nlohmann::json& record, int64_t now) {
    const nlohmann::json* seen = json_util::find_member(record, "seen_at");
    if (seen == nullptr || !seen->is_number_integer()) {
        return std::nullopt;
    }
    return now - seen->get<int64_t>() <= 2 * kLocalScreenPresenceIntervalS;
}

std::string plugin_reason_text(std::string_view reason) {
    const std::string_view code = reason.substr(0, reason.find(':'));
    for (const PluginReasonText& known : kPluginReasonTexts) {
        if (code == known.code) {
            return lv_tr(known.text);
        }
    }
    return std::string(reason);
}

namespace {

bool hidden_on_overview(const KnownPluginFeature& known, const PluginFeatureEntry& entry) {
    if (entry.available || known.hidden_reasons == nullptr) {
        return false;
    }
    for (const char* const* reason = known.hidden_reasons; *reason != nullptr; ++reason) {
        if (entry.reason == *reason) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<PluginFeatureLine> plugin_feature_lines(const PluginFeatureMap& features) {
    std::vector<PluginFeatureLine> lines;
    auto line_for = [](const std::string& name, const PluginFeatureEntry& entry) {
        PluginFeatureLine line;
        line.name = name;
        line.label = name;
        line.icon = kUnknownPluginFeatureIcon;
        line.status = !entry.available ? PluginFeatureStatus::Unavailable
                      : entry.enabled  ? PluginFeatureStatus::Active
                                       : PluginFeatureStatus::Off;
        line.reason = entry.available ? "" : entry.reason;
        return line;
    };
    for (const KnownPluginFeature& known : kKnownPluginFeatures) {
        const auto it = features.find(known.name);
        if (it == features.end() || hidden_on_overview(known, it->second)) {
            continue;
        }
        PluginFeatureLine line = line_for(it->first, it->second);
        line.label = known.label;
        line.icon = known.icon;
        line.home = known.home;
        lines.push_back(std::move(line));
    }
    for (const auto& feature : features) {
        // A named reference: C++17 lambdas cannot capture a structured binding.
        const std::string& name = feature.first;
        const bool known =
            std::any_of(std::begin(kKnownPluginFeatures), std::end(kKnownPluginFeatures),
                        [&name](const KnownPluginFeature& k) { return name == k.name; });
        if (!known) {
            lines.push_back(line_for(name, feature.second));
        }
    }
    return lines;
}

} // namespace helix
