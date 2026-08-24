// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tips_manager.h"

#include "ui_error_reporting.h"

#include "data_root_resolver.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sys/stat.h>

using namespace helix;

TipsManager* TipsManager::instance{nullptr};

TipsManager::TipsManager() {
    // Seed random generator with high-quality entropy
    std::random_device rd;
    random_generator.seed(rd());
}

TipsManager* TipsManager::get_instance() {
    if (instance == nullptr) {
        instance = new TipsManager();
    }
    instance->ensure_loaded();
    return instance;
}

void TipsManager::ensure_loaded() {
    {
        std::lock_guard<std::mutex> lock(tips_mutex);
        if (load_attempted_) {
            return;
        }
        load_attempted_ = true;
    }
    // Deliberately outside the lock above: init() takes tips_mutex itself.
    init(helix::find_readable("printing_tips.json"));
}

bool TipsManager::init(const std::string& tips_path) {
    std::lock_guard<std::mutex> lock(tips_mutex);

    // Reset viewed tips session when reinitializing
    viewed_tip_ids_.clear();

    path = tips_path;
    struct stat buffer;

    if (stat(tips_path.c_str(), &buffer) != 0) {
        NOTIFY_WARNING(lv_tr("Tips database not found"));
        LOG_ERROR_INTERNAL("[TipsManager] Tips file not found: {}", tips_path);
        return false;
    }

    try {
        spdlog::debug("[TipsManager] Loading tips from {}", tips_path);
        std::ifstream file(tips_path);
        data = json::parse(file);

        // Validate required fields
        if (!data.contains("categories") || !data["categories"].is_object()) {
            NOTIFY_WARNING(lv_tr("Tips database format error"));
            LOG_ERROR_INTERNAL(
                "[TipsManager] Invalid tips file: missing or invalid 'categories' field");
            return false;
        }

        // Build cache for fast access
        // Cache the two things the DOM was still being kept alive for, then
        // release it. `data` and tips_cache held the same 105 KB twice for the
        // whole session; only get_version() and get_all_categories() still read
        // the DOM, and both are satisfied by these.
        version_ = helix::json_util::safe_string(data, "version", "unknown");
        categories_.clear();
        for (auto& [category_key, _] : data["categories"].items()) {
            categories_.push_back(category_key);
        }

        build_tips_cache();
        data = json();

        // Reads the cached members, not `data` — it has just been released.
        spdlog::trace("[TipsManager] Loaded {} tips from {} categories (version: {})",
                      tips_cache.size(), categories_.size(), version_);

        return true;
    } catch (const json::parse_error& e) {
        NOTIFY_WARNING(lv_tr("Could not parse tips database"));
        LOG_ERROR_INTERNAL("[TipsManager] JSON parse error: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        NOTIFY_WARNING(lv_tr("Error loading printing tips"));
        LOG_ERROR_INTERNAL("[TipsManager] Error loading tips: {}", e.what());
        return false;
    }
}

void TipsManager::build_tips_cache() {
    tips_cache.clear();

    if (!data.contains("categories") || !data["categories"].is_object()) {
        spdlog::warn("[TipsManager] No categories found in tips database");
        return;
    }

    // Iterate through all categories
    for (auto& [category_key, category_obj] : data["categories"].items()) {
        if (!category_obj.contains("tips") || !category_obj["tips"].is_array()) {
            spdlog::warn("[TipsManager] Category '{}' has no tips array", category_key);
            continue;
        }

        // Iterate through tips in this category
        for (const auto& tip_json : category_obj["tips"]) {
            // A non-object entry would resolve to an all-empty tip that still
            // occupies a slot in the random rotation — drop it instead.
            if (!tip_json.is_object()) {
                spdlog::warn("[TipsManager] Category '{}' has a non-object tip entry; skipping",
                             category_key);
                continue;
            }
            PrintingTip tip = json_to_tip(tip_json, category_key);
            tips_cache.push_back(tip);
        }
    }

    spdlog::trace("[TipsManager] Built cache with {} tips", tips_cache.size());
}

PrintingTip TipsManager::json_to_tip(const json& tip_json, const std::string& category) {
    PrintingTip tip;

    // safe_string, not .value(): .value() throws type_error.302 on a key that is
    // PRESENT with a null value (a missing key is fine). init()'s try/catch does
    // span this call, so the old behaviour was not a crash — it was worse in a
    // quieter way: one null field anywhere in the file aborted init() and the
    // user lost the ENTIRE tips database. Per-field defaults keep the cost to
    // the field.
    tip.id = helix::json_util::safe_string(tip_json, "id");
    tip.title = helix::json_util::safe_string(tip_json, "title");
    tip.content = helix::json_util::safe_string(tip_json, "content");
    tip.difficulty = helix::json_util::safe_string(tip_json, "difficulty");
    tip.priority = helix::json_util::safe_string(tip_json, "priority");
    tip.category = category;

    // Parse tags array
    if (tip_json.contains("tags") && tip_json["tags"].is_array()) {
        for (const auto& tag : tip_json["tags"]) {
            if (tag.is_string()) {
                tip.tags.push_back(tag.get<std::string>());
            }
        }
    }

    // Parse related_settings array
    if (tip_json.contains("related_settings") && tip_json["related_settings"].is_array()) {
        for (const auto& setting : tip_json["related_settings"]) {
            if (setting.is_string()) {
                tip.related_settings.push_back(setting.get<std::string>());
            }
        }
    }

    return tip;
}

PrintingTip TipsManager::get_random_tip() {
    std::lock_guard<std::mutex> lock(tips_mutex);

    if (tips_cache.empty()) {
        spdlog::warn("[TipsManager] No tips available for random selection");
        return PrintingTip{};
    }

    std::uniform_int_distribution<size_t> dist(0, tips_cache.size() - 1);
    return tips_cache[dist(random_generator)];
}

PrintingTip TipsManager::get_random_tip_by_category(const std::string& category) {
    auto category_tips = get_tips_by_category(category);

    if (category_tips.empty()) {
        spdlog::warn("[TipsManager] No tips found in category '{}'", category);
        return PrintingTip{};
    }

    std::uniform_int_distribution<size_t> dist(0, category_tips.size() - 1);
    return category_tips[dist(random_generator)];
}

PrintingTip TipsManager::get_random_tip_by_difficulty(const std::string& difficulty) {
    auto difficulty_tips = get_tips_by_difficulty(difficulty);

    if (difficulty_tips.empty()) {
        spdlog::warn("[TipsManager] No tips found with difficulty '{}'", difficulty);
        return PrintingTip{};
    }

    std::uniform_int_distribution<size_t> dist(0, difficulty_tips.size() - 1);
    return difficulty_tips[dist(random_generator)];
}

PrintingTip TipsManager::get_random_unique_tip() {
    std::lock_guard<std::mutex> lock(tips_mutex);

    if (tips_cache.empty()) {
        spdlog::warn("[TipsManager] No tips available for unique selection");
        return PrintingTip{};
    }

    // Check if all tips have been viewed
    if (viewed_tip_ids_.size() >= tips_cache.size()) {
        spdlog::info("[TipsManager] All {} tips viewed - resetting session", tips_cache.size());
        viewed_tip_ids_.clear();
    }

    // Build list of unviewed tip *indices* — not full PrintingTip copies. Copying
    // the whole cache into a temporary vector every 60s allocated 4 strings + 2
    // string-vectors per tip, fragmenting the heap on memory-constrained ARM
    // devices and eventually tripping glibc's heap-consistency detector (#771).
    std::vector<size_t> unviewed_indices;
    unviewed_indices.reserve(tips_cache.size());
    for (size_t i = 0; i < tips_cache.size(); ++i) {
        const auto& tip = tips_cache[i];
        if (std::find(viewed_tip_ids_.begin(), viewed_tip_ids_.end(), tip.id) ==
            viewed_tip_ids_.end()) {
            unviewed_indices.push_back(i);
        }
    }

    if (unviewed_indices.empty()) {
        spdlog::error("[TipsManager] Logic error: no unviewed tips found but viewed count < total");
        return PrintingTip{};
    }

    // Select random unviewed tip
    std::uniform_int_distribution<size_t> dist(0, unviewed_indices.size() - 1);
    const PrintingTip& selected = tips_cache[unviewed_indices[dist(random_generator)]];

    // Mark as viewed
    viewed_tip_ids_.push_back(selected.id);

    spdlog::trace("[TipsManager] Selected unique tip '{}' ({}/{})", selected.id,
                  viewed_tip_ids_.size(), tips_cache.size());

    return selected; // single copy on return (NRVO when possible)
}

void TipsManager::reset_viewed_tips() {
    std::lock_guard<std::mutex> lock(tips_mutex);
    spdlog::info("[TipsManager] Manually resetting viewed tips ({} tips)", viewed_tip_ids_.size());
    viewed_tip_ids_.clear();
}

std::vector<PrintingTip> TipsManager::search_by_keyword(const std::string& keyword) {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<PrintingTip> results;

    if (keyword.empty()) {
        return results;
    }

    // Convert keyword to lowercase for case-insensitive search
    std::string keyword_lower = keyword;
    std::transform(keyword_lower.begin(), keyword_lower.end(), keyword_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& tip : tips_cache) {
        // Search in title
        std::string title_lower = tip.title;
        std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Search in content
        std::string content_lower = tip.content;
        std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Check title, content, and tags
        bool found = false;

        if (title_lower.find(keyword_lower) != std::string::npos ||
            content_lower.find(keyword_lower) != std::string::npos) {
            found = true;
        } else {
            // Search in tags
            for (const auto& tag : tip.tags) {
                std::string tag_lower = tag;
                std::transform(tag_lower.begin(), tag_lower.end(), tag_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (tag_lower.find(keyword_lower) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            results.push_back(tip);
        }
    }

    spdlog::debug("[TipsManager] Keyword search '{}' found {} tips", keyword, results.size());
    return results;
}

std::vector<PrintingTip> TipsManager::get_tips_by_category(const std::string& category) {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<PrintingTip> results;

    for (const auto& tip : tips_cache) {
        if (tip.category == category) {
            results.push_back(tip);
        }
    }

    return results;
}

std::vector<PrintingTip> TipsManager::get_tips_by_tag(const std::string& tag) {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<PrintingTip> results;

    // Convert tag to lowercase for case-insensitive comparison
    std::string tag_lower = tag;
    std::transform(tag_lower.begin(), tag_lower.end(), tag_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& tip : tips_cache) {
        for (const auto& tip_tag : tip.tags) {
            std::string tip_tag_lower = tip_tag;
            std::transform(tip_tag_lower.begin(), tip_tag_lower.end(), tip_tag_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (tip_tag_lower == tag_lower) {
                results.push_back(tip);
                break;
            }
        }
    }

    return results;
}

std::vector<PrintingTip> TipsManager::get_tips_by_difficulty(const std::string& difficulty) {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<PrintingTip> results;

    // Convert to lowercase for case-insensitive comparison
    std::string difficulty_lower = difficulty;
    std::transform(difficulty_lower.begin(), difficulty_lower.end(), difficulty_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& tip : tips_cache) {
        std::string tip_difficulty_lower = tip.difficulty;
        std::transform(tip_difficulty_lower.begin(), tip_difficulty_lower.end(),
                       tip_difficulty_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (tip_difficulty_lower == difficulty_lower) {
            results.push_back(tip);
        }
    }

    return results;
}

std::vector<PrintingTip> TipsManager::get_tips_by_priority(const std::string& priority) {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<PrintingTip> results;

    // Convert to lowercase for case-insensitive comparison
    std::string priority_lower = priority;
    std::transform(priority_lower.begin(), priority_lower.end(), priority_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& tip : tips_cache) {
        std::string tip_priority_lower = tip.priority;
        std::transform(tip_priority_lower.begin(), tip_priority_lower.end(),
                       tip_priority_lower.begin(), [](unsigned char c) { return std::tolower(c); });

        if (tip_priority_lower == priority_lower) {
            results.push_back(tip);
        }
    }

    return results;
}

PrintingTip TipsManager::get_tip_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(tips_mutex);

    for (const auto& tip : tips_cache) {
        if (tip.id == id) {
            return tip;
        }
    }

    spdlog::warn("[TipsManager] Tip ID '{}' not found", id);
    return PrintingTip{};
}

std::vector<std::string> TipsManager::get_all_categories() {
    std::lock_guard<std::mutex> lock(tips_mutex);
    return categories_;
}

std::vector<std::string> TipsManager::get_all_tags() {
    std::lock_guard<std::mutex> lock(tips_mutex);
    std::vector<std::string> all_tags;

    // Collect all unique tags
    for (const auto& tip : tips_cache) {
        for (const auto& tag : tip.tags) {
            // Check if tag already exists (simple linear search, OK for small dataset)
            if (std::find(all_tags.begin(), all_tags.end(), tag) == all_tags.end()) {
                all_tags.push_back(tag);
            }
        }
    }

    // Sort alphabetically
    std::sort(all_tags.begin(), all_tags.end());

    return all_tags;
}

size_t TipsManager::get_total_tips() {
    std::lock_guard<std::mutex> lock(tips_mutex);
    return tips_cache.size();
}

std::string TipsManager::get_version() {
    std::lock_guard<std::mutex> lock(tips_mutex);
    // Captured at load; `data` is released once the cache is built.
    return version_;
}
