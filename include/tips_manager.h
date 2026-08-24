// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "spdlog/spdlog.h"

#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Printing tip data structure
 *
 * Represents a single tip from the tips database with all metadata.
 */
struct PrintingTip {
    std::string id;
    std::string title;
    std::string content;
    std::vector<std::string> tags;
    std::string difficulty; // "beginner", "intermediate", "advanced"
    std::string priority;   // "high", "medium", "low"
    std::vector<std::string> related_settings;
    std::string category; // Category name for easy reference
};

/**
 * @brief Manager for 3D printing tips database
 *
 * Thread-safe singleton that loads, searches, and retrieves printing tips
 * from a JSON database. Supports filtering by category, tags, difficulty,
 * priority, and full-text search. Also provides random tip selection for
 * "tip of the day" features.
 */
class TipsManager {
  private:
    static TipsManager* instance;
    std::string path;
    json data;
    std::vector<PrintingTip> tips_cache;
    std::vector<std::string> viewed_tip_ids_; // Session tracking for unique tips
    bool load_attempted_ = false;             // ensure_loaded() runs once, pass or fail
    // Derived from `data` at load so the DOM can be released. Categories are
    // stored rather than re-derived from tips_cache because a category whose
    // tips array is missing or empty contributes no cache entries but must
    // still appear in the list, which is what the old DOM walk returned.
    std::vector<std::string> categories_;
    std::string version_ = "unknown";
    std::mutex tips_mutex;
    std::mt19937 random_generator;

    /**
     * @brief Parse JSON and build tips cache
     *
     * Converts JSON structure to vector of PrintingTip structs for fast access.
     */
    void build_tips_cache();

    /**
     * @brief Parse the tips database on first use.
     *
     * Boot used to call init() unconditionally, parsing a 105 KB JSON file on
     * every startup for a widget many sessions never display. Idempotent, and
     * takes no lock itself -- init() does.
     */
    void ensure_loaded();

    /**
     * @brief Convert JSON tip object to PrintingTip struct
     */
    PrintingTip json_to_tip(const json& tip_json, const std::string& category);

  public:
    TipsManager();
    TipsManager(TipsManager& o) = delete;
    void operator=(const TipsManager&) = delete;

    /**
     * @brief Initialize tips manager from JSON file
     *
     * Loads and parses the tips database. Call this once at startup.
     *
     * @param tips_path Path to printing_tips.json file
     * @return true if loaded successfully, false on error
     */
    bool init(const std::string& tips_path);

    /**
     * @brief Get a random tip from the entire database
     *
     * @return Random PrintingTip, or empty tip if database is empty
     */
    PrintingTip get_random_tip();

    /**
     * @brief Get a random tip from a specific category
     *
     * @param category Category name (e.g., "klipper_features", "troubleshooting")
     * @return Random tip from category, or empty tip if category not found
     */
    PrintingTip get_random_tip_by_category(const std::string& category);

    /**
     * @brief Get a random tip for a specific difficulty level
     *
     * @param difficulty Difficulty level ("beginner", "intermediate", "advanced")
     * @return Random tip matching difficulty, or empty tip if none found
     */
    PrintingTip get_random_tip_by_difficulty(const std::string& difficulty);

    /**
     * @brief Get a random unique tip (session-aware, no repeats)
     *
     * Returns a random tip that hasn't been shown yet in this session.
     * Automatically resets viewed list when all tips have been shown.
     *
     * @return Random PrintingTip not previously viewed, or empty tip if database is empty
     */
    PrintingTip get_random_unique_tip();

    /**
     * @brief Reset viewed tips list
     *
     * Clears the session tracking so all tips can be shown again.
     */
    void reset_viewed_tips();

    /**
     * @brief Search tips by keyword (case-insensitive)
     *
     * Searches in tip title, content, and tags.
     *
     * @param keyword Keyword to search for
     * @return Vector of matching tips (empty if no matches)
     */
    std::vector<PrintingTip> search_by_keyword(const std::string& keyword);

    /**
     * @brief Get all tips in a category
     *
     * @param category Category name
     * @return Vector of tips in category (empty if category not found)
     */
    std::vector<PrintingTip> get_tips_by_category(const std::string& category);

    /**
     * @brief Get tips with a specific tag
     *
     * @param tag Tag to filter by (e.g., "calibration", "speed", "quality")
     * @return Vector of tips with tag (empty if no matches)
     */
    std::vector<PrintingTip> get_tips_by_tag(const std::string& tag);

    /**
     * @brief Get tips by difficulty level
     *
     * @param difficulty Difficulty level ("beginner", "intermediate", "advanced")
     * @return Vector of tips matching difficulty (empty if no matches)
     */
    std::vector<PrintingTip> get_tips_by_difficulty(const std::string& difficulty);

    /**
     * @brief Get tips by priority level
     *
     * @param priority Priority level ("high", "medium", "low")
     * @return Vector of tips matching priority (empty if no matches)
     */
    std::vector<PrintingTip> get_tips_by_priority(const std::string& priority);

    /**
     * @brief Get a specific tip by ID
     *
     * @param id Tip ID (e.g., "tip-001")
     * @return PrintingTip if found, or empty tip if not found
     */
    PrintingTip get_tip_by_id(const std::string& id);

    /**
     * @brief Get all category names
     *
     * @return Vector of category names
     */
    std::vector<std::string> get_all_categories();

    /**
     * @brief Get all unique tags
     *
     * @return Vector of all tags used across all tips
     */
    std::vector<std::string> get_all_tags();

    /**
     * @brief Get total number of tips
     *
     * @return Total tips in database
     */
    size_t get_total_tips();

    /**
     * @brief Get database version
     *
     * @return Version string from JSON
     */
    std::string get_version();

    /**
     * @brief Singleton accessor
     *
     * @return Pointer to TipsManager instance
     */
    static TipsManager* get_instance();
};

} // namespace helix
