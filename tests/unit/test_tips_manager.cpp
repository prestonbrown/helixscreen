// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tips_manager.h"

#include <fstream>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test fixture for TipsManager testing
// Uses PID-unique temp paths to avoid collisions between parallel test shards
class TipsManagerTestFixture {
  protected:
    const std::string pid_suffix = std::to_string(getpid());
    const std::string test_tips_file = "/tmp/test_printing_tips_" + pid_suffix + ".json";
    const std::string invalid_tips_file = "/tmp/test_invalid_tips_" + pid_suffix + ".json";
    const std::string empty_tips_file = "/tmp/test_empty_tips_" + pid_suffix + ".json";

  public:
    // The files are removed however a case ends.
    TipsManagerTestFixture() {
        create_valid_test_tips();
    }
    ~TipsManagerTestFixture() {
        remove(test_tips_file.c_str());
        remove(invalid_tips_file.c_str());
        remove(empty_tips_file.c_str());
    }

  protected:
    /// The manager loaded from the valid test file.
    TipsManager* loaded() {
        TipsManager* mgr = TipsManager::get_instance();
        mgr->init(test_tips_file);
        return mgr;
    }

    void create_valid_test_tips() {
        std::ofstream file(test_tips_file);
        file << R"({
  "version": "1.0.0-test",
  "description": "Test tips database",
  "last_updated": "2025-10-27",
  "total_tips": 10,
  "categories": {
    "test_category_1": {
      "name": "Test Category 1",
      "description": "First test category",
      "tips": [
        {
          "id": "tip-001",
          "title": "Test Tip 1",
          "content": "This is test tip content 1",
          "tags": ["tag1", "tag2", "calibration"],
          "difficulty": "beginner",
          "priority": "high",
          "related_settings": ["setting1", "setting2"]
        },
        {
          "id": "tip-002",
          "title": "Test Tip 2",
          "content": "This is test tip content 2 with keyword speed",
          "tags": ["tag2", "tag3", "speed"],
          "difficulty": "intermediate",
          "priority": "medium",
          "related_settings": ["setting3"]
        },
        {
          "id": "tip-003",
          "title": "Test Tip 3",
          "content": "This is test tip content 3",
          "tags": ["tag1", "quality"],
          "difficulty": "advanced",
          "priority": "low",
          "related_settings": []
        }
      ]
    },
    "test_category_2": {
      "name": "Test Category 2",
      "description": "Second test category",
      "tips": [
        {
          "id": "tip-004",
          "title": "Test Tip 4",
          "content": "This is test tip content 4 with calibration keyword",
          "tags": ["tag4", "calibration"],
          "difficulty": "beginner",
          "priority": "high",
          "related_settings": ["setting4"]
        },
        {
          "id": "tip-005",
          "title": "Test Tip 5",
          "content": "This is test tip content 5",
          "tags": ["tag5"],
          "difficulty": "beginner",
          "priority": "medium",
          "related_settings": []
        }
      ]
    }
  }
})";
        file.close();
    }

    void create_invalid_json() {
        std::ofstream file(invalid_tips_file);
        file << R"({
  "invalid": "json",
  "missing": ["categories"]
})";
        file.close();
    }

    void create_empty_tips() {
        std::ofstream file(empty_tips_file);
        file << R"({
  "version": "1.0.0",
  "categories": {}
})";
        file.close();
    }
};

// ============================================================================
// Initialization and Loading
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: init() loads valid JSON file",
                 "[ui][init]") {
    TipsManager* mgr = TipsManager::get_instance();
    bool result = mgr->init(test_tips_file);

    REQUIRE(result == true);
    REQUIRE(mgr->get_total_tips() == 5);
    REQUIRE(mgr->get_version() == "1.0.0-test");
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: init() fails on missing file",
                 "[ui][init]") {
    TipsManager* mgr = TipsManager::get_instance();
    bool result = mgr->init("/tmp/nonexistent_tips.json");

    REQUIRE(result == false);
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: init() fails on invalid JSON",
                 "[ui][init]") {
    create_invalid_json();

    TipsManager* mgr = TipsManager::get_instance();
    bool result = mgr->init(invalid_tips_file);

    REQUIRE(result == false);
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: handles empty categories", "[ui][init]") {
    create_empty_tips();

    TipsManager* mgr = TipsManager::get_instance();
    bool result = mgr->init(empty_tips_file);

    REQUIRE(result == true);
    REQUIRE(mgr->get_total_tips() == 0);
}

// ============================================================================
// Random Tip Selection
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_random_tip() returns valid tip",
                 "[ui][random]") {
    TipsManager* mgr = loaded();

    PrintingTip tip = mgr->get_random_tip();

    REQUIRE(!tip.id.empty());
    REQUIRE(!tip.title.empty());
    REQUIRE(!tip.content.empty());
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_random_tip() returns empty on empty database", "[ui][random]") {
    create_empty_tips();

    TipsManager* mgr = TipsManager::get_instance();
    mgr->init(empty_tips_file);

    PrintingTip tip = mgr->get_random_tip();

    REQUIRE(tip.id.empty());
    REQUIRE(tip.title.empty());
}

// ============================================================================
// Unique Tip Selection (Session Tracking)
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_random_unique_tip() returns different tips", "[ui][unique]") {
    TipsManager* mgr = loaded();

    PrintingTip tip1 = mgr->get_random_unique_tip();
    PrintingTip tip2 = mgr->get_random_unique_tip();

    REQUIRE(!tip1.id.empty());
    REQUIRE(!tip2.id.empty());
    REQUIRE(tip1.id != tip2.id);
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_random_unique_tip() resets after exhaustion", "[ui][unique]") {
    TipsManager* mgr = loaded();

    // Get all 5 tips
    std::vector<std::string> tip_ids;
    for (int i = 0; i < 5; i++) {
        PrintingTip tip = mgr->get_random_unique_tip();
        REQUIRE(!tip.id.empty());
        tip_ids.push_back(tip.id);
    }

    // Verify all tips are unique
    std::sort(tip_ids.begin(), tip_ids.end());
    auto it = std::unique(tip_ids.begin(), tip_ids.end());
    REQUIRE(it == tip_ids.end());

    // Next call should reset and return a valid tip
    PrintingTip next_tip = mgr->get_random_unique_tip();
    REQUIRE(!next_tip.id.empty());
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: reset_viewed_tips() clears session",
                 "[ui][unique]") {
    TipsManager* mgr = loaded();

    // Exhaust all 5 tips
    for (int i = 0; i < 5; i++) {
        PrintingTip tip = mgr->get_random_unique_tip();
        REQUIRE(!tip.id.empty());
    }

    // Reset viewed tips (should allow getting all 5 tips again)
    mgr->reset_viewed_tips();

    // Verify we can get 5 more unique tips (proves reset worked)
    std::vector<std::string> tip_ids_after_reset;
    for (int i = 0; i < 5; i++) {
        PrintingTip tip = mgr->get_random_unique_tip();
        REQUIRE(!tip.id.empty());
        tip_ids_after_reset.push_back(tip.id);
    }

    // Verify all 5 tips after reset are unique
    std::sort(tip_ids_after_reset.begin(), tip_ids_after_reset.end());
    auto it = std::unique(tip_ids_after_reset.begin(), tip_ids_after_reset.end());
    REQUIRE(it == tip_ids_after_reset.end());
}

// ============================================================================
// Category Filtering
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tips_by_category() returns correct tips",
                 "[ui][category]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->get_tips_by_category("test_category_1");

    REQUIRE(tips.size() == 3);
    REQUIRE(tips[0].category == "test_category_1");
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_tips_by_category() returns empty for invalid category",
                 "[ui][category]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->get_tips_by_category("nonexistent_category");

    REQUIRE(tips.size() == 0);
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_all_categories() returns correct count",
                 "[ui][category]") {
    TipsManager* mgr = loaded();

    auto categories = mgr->get_all_categories();

    REQUIRE(categories.size() == 2);
}

// ============================================================================
// Tag Filtering
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tips_by_tag() returns matching tips",
                 "[ui][tag]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->get_tips_by_tag("calibration");

    REQUIRE(tips.size() == 2); // tip-001 and tip-004 have "calibration" tag
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tips_by_tag() is case-insensitive",
                 "[ui][tag]") {
    TipsManager* mgr = loaded();

    auto tips_lower = mgr->get_tips_by_tag("calibration");
    auto tips_upper = mgr->get_tips_by_tag("CALIBRATION");
    auto tips_mixed = mgr->get_tips_by_tag("CaLiBrAtIoN");

    REQUIRE(tips_lower.size() == tips_upper.size());
    REQUIRE(tips_lower.size() == tips_mixed.size());
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_all_tags() returns unique tags",
                 "[ui][tag]") {
    TipsManager* mgr = loaded();

    auto tags = mgr->get_all_tags();

    // Should have: tag1, tag2, tag3, tag4, tag5, calibration, speed, quality
    REQUIRE(tags.size() >= 8);

    // Verify sorted
    REQUIRE(std::is_sorted(tags.begin(), tags.end()));
}

// ============================================================================
// Difficulty Filtering
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tips_by_difficulty() filters correctly",
                 "[ui][difficulty]") {
    TipsManager* mgr = loaded();

    auto beginner_tips = mgr->get_tips_by_difficulty("beginner");
    auto intermediate_tips = mgr->get_tips_by_difficulty("intermediate");
    auto advanced_tips = mgr->get_tips_by_difficulty("advanced");

    REQUIRE(beginner_tips.size() == 3);     // tip-001, tip-004, tip-005
    REQUIRE(intermediate_tips.size() == 1); // tip-002
    REQUIRE(advanced_tips.size() == 1);     // tip-003
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_tips_by_difficulty() is case-insensitive", "[ui][difficulty]") {
    TipsManager* mgr = loaded();

    auto tips_lower = mgr->get_tips_by_difficulty("beginner");
    auto tips_upper = mgr->get_tips_by_difficulty("BEGINNER");

    REQUIRE(tips_lower.size() == tips_upper.size());
}

// ============================================================================
// Priority Filtering
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tips_by_priority() filters correctly",
                 "[ui][priority]") {
    TipsManager* mgr = loaded();

    auto high_tips = mgr->get_tips_by_priority("high");
    auto medium_tips = mgr->get_tips_by_priority("medium");
    auto low_tips = mgr->get_tips_by_priority("low");

    REQUIRE(high_tips.size() == 2);   // tip-001, tip-004
    REQUIRE(medium_tips.size() == 2); // tip-002, tip-005
    REQUIRE(low_tips.size() == 1);    // tip-003
}

// ============================================================================
// Keyword Search
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: search_by_keyword() finds in title",
                 "[ui][search]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->search_by_keyword("Test Tip 1");

    REQUIRE(tips.size() >= 1);
    REQUIRE(tips[0].id == "tip-001");
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: search_by_keyword() finds in content",
                 "[ui][search]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->search_by_keyword("speed");

    REQUIRE(tips.size() >= 1);
    bool found_tip_002 = false;
    for (const auto& tip : tips) {
        if (tip.id == "tip-002") {
            found_tip_002 = true;
            break;
        }
    }
    REQUIRE(found_tip_002 == true);
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: search_by_keyword() finds in tags",
                 "[ui][search]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->search_by_keyword("calibration");

    REQUIRE(tips.size() == 2); // tip-001 and tip-004 have "calibration" in tags OR content
}

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: search_by_keyword() is case-insensitive",
                 "[ui][search]") {
    TipsManager* mgr = loaded();

    auto tips_lower = mgr->search_by_keyword("speed");
    auto tips_upper = mgr->search_by_keyword("SPEED");

    REQUIRE(tips_lower.size() == tips_upper.size());
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: search_by_keyword() returns empty for no matches", "[ui][search]") {
    TipsManager* mgr = loaded();

    auto tips = mgr->search_by_keyword("nonexistent_keyword_xyz");

    REQUIRE(tips.size() == 0);
}

// ============================================================================
// Specific Tip Lookup
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: get_tip_by_id() returns correct tip",
                 "[ui][lookup]") {
    TipsManager* mgr = loaded();

    PrintingTip tip = mgr->get_tip_by_id("tip-003");

    REQUIRE(tip.id == "tip-003");
    REQUIRE(tip.title == "Test Tip 3");
    REQUIRE(tip.difficulty == "advanced");
}

TEST_CASE_METHOD(TipsManagerTestFixture,
                 "TipsManager: get_tip_by_id() returns empty for invalid ID", "[ui][lookup]") {
    TipsManager* mgr = loaded();

    PrintingTip tip = mgr->get_tip_by_id("tip-999");

    REQUIRE(tip.id.empty());
}

// ============================================================================
// Thread Safety (Basic Test)
// ============================================================================

TEST_CASE_METHOD(TipsManagerTestFixture, "TipsManager: concurrent access is thread-safe",
                 "[ui][thread_safety][slow]") {
    TipsManager* mgr = loaded();

    std::vector<std::thread> threads;
    std::atomic<int> successful_reads{0};

    // Spawn 10 threads that each read 100 tips
    for (int t = 0; t < 10; t++) {
        threads.emplace_back([mgr, &successful_reads]() {
            for (int i = 0; i < 100; i++) {
                PrintingTip tip = mgr->get_random_tip();
                if (!tip.id.empty()) {
                    successful_reads++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All reads should succeed
    REQUIRE(successful_reads == 1000);
}
