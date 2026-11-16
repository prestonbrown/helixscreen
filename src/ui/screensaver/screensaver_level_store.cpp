// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_level_store.h"

#include "config.h"

#include <algorithm>

namespace helix::ui {

std::string level_store_path(const char* saver_name) {
    return std::string("/display/screensaver_levels/") + saver_name;
}

std::optional<SaverLevelEntry> parse_level_entry(const nlohmann::json* node) {
    if (node == nullptr || !node->is_object()) {
        return std::nullopt;
    }
    const auto level = node->find("level");
    const auto too_heavy = node->find("too_heavy");
    const auto key = parse_measurement_key(*node);
    if (level == node->end() || !level->is_number_unsigned() || too_heavy == node->end() ||
        !too_heavy->is_boolean() || !key) {
        return std::nullopt;
    }
    SaverLevelEntry entry;
    entry.level = level->get<size_t>();
    entry.too_heavy = too_heavy->get<bool>();
    entry.version = key->version;
    entry.board = key->board;
    return entry;
}

nlohmann::json level_entry_json(const SaverLevelEntry& entry) {
    nlohmann::json node{{"level", entry.level}, {"too_heavy", entry.too_heavy}};
    write_measurement_key(node, {entry.board, entry.version});
    return node;
}

SaverLevelEntry start_entry(const std::optional<SaverLevelEntry>& stored,
                            const std::string& version, const std::string& board,
                            size_t level_count) {
    if (stored && stored->version == version && stored->board == board) {
        SaverLevelEntry entry = *stored;
        entry.level = std::min(entry.level, level_count > 0 ? level_count - 1 : 0);
        return entry;
    }
    SaverLevelEntry fresh;
    fresh.version = version;
    fresh.board = board;
    return fresh;
}

std::optional<SaverLevelEntry> load_level_entry(const helix::Config& config,
                                                const char* saver_name) {
    return parse_level_entry(config.try_get_json(level_store_path(saver_name)));
}

void save_level_entry(helix::Config& config, const char* saver_name, const SaverLevelEntry& entry) {
    config.set<nlohmann::json>(level_store_path(saver_name), level_entry_json(entry));
    config.save();
}

} // namespace helix::ui
