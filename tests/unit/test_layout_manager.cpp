// SPDX-License-Identifier: GPL-3.0-or-later

#include "layout_manager.h"

#include "../catch_amalgamated.hpp"

using helix::LayoutManager;
using helix::LayoutType;

class LayoutManagerTestAccess {
  public:
    static void reset(LayoutManager& lm) {
        lm.type_ = LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

// Reset singleton state between tests
struct LayoutFixture {
    LayoutFixture() {
        LayoutManagerTestAccess::reset(LayoutManager::instance());
    }
    ~LayoutFixture() {
        LayoutManagerTestAccess::reset(LayoutManager::instance());
    }
};

// ============================================================================
// Screen dimensions
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "LayoutManager stores screen dimensions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("800x480 standard") {
        lm.init(800, 480);
        REQUIRE(lm.width() == 800);
        REQUIRE(lm.height() == 480);
    }
    SECTION("1920x440 ultrawide") {
        lm.init(1920, 440);
        REQUIRE(lm.width() == 1920);
        REQUIRE(lm.height() == 440);
    }
    SECTION("480x1600 portrait") {
        lm.init(480, 1600);
        REQUIRE(lm.width() == 480);
        REQUIRE(lm.height() == 1600);
    }
    SECTION("uninitialized returns 0") {
        REQUIRE(lm.width() == 0);
        REQUIRE(lm.height() == 0);
    }
}

// ============================================================================
// Detection via init()
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "Standard landscape resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("800x480") {
        lm.init(800, 480);
        REQUIRE(lm.type() == LayoutType::STANDARD);
    }
    SECTION("1024x600") {
        lm.init(1024, 600);
        REQUIRE(lm.type() == LayoutType::STANDARD);
    }
    SECTION("1920x1080") {
        lm.init(1920, 1080);
        REQUIRE(lm.type() == LayoutType::STANDARD);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Ultrawide resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("1920x480 ratio=4.0") {
        lm.init(1920, 480);
        REQUIRE(lm.type() == LayoutType::ULTRAWIDE);
    }
    SECTION("2560x600 ratio=4.27") {
        lm.init(2560, 600);
        REQUIRE(lm.type() == LayoutType::ULTRAWIDE);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Portrait resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("480x800") {
        lm.init(480, 800);
        REQUIRE(lm.type() == LayoutType::PORTRAIT);
    }
    SECTION("600x1024") {
        lm.init(600, 1024);
        REQUIRE(lm.type() == LayoutType::PORTRAIT);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Micro landscape resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("480x272 — Ender 3 V3 KE") {
        lm.init(480, 272);
        REQUIRE(lm.type() == LayoutType::MICRO);
        REQUIRE(lm.name() == "micro");
    }
    SECTION("320x240 — min dim ≤272 so micro, not tiny") {
        lm.init(320, 240);
        REQUIRE(lm.type() == LayoutType::MICRO);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Micro portrait resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("272x480") {
        lm.init(272, 480);
        REQUIRE(lm.type() == LayoutType::MICRO_PORTRAIT);
        REQUIRE(lm.name() == "micro_portrait");
    }
    SECTION("240x320 — min dim ≤272 so micro portrait, not tiny portrait") {
        lm.init(240, 320);
        REQUIRE(lm.type() == LayoutType::MICRO_PORTRAIT);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Tiny landscape resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("480x320 — min dim >272 so stays tiny") {
        lm.init(480, 320);
        REQUIRE(lm.type() == LayoutType::TINY);
    }
    SECTION("480x400") {
        lm.init(480, 400);
        REQUIRE(lm.type() == LayoutType::TINY);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Tiny portrait resolutions", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("320x480 — min dim >272 so stays tiny portrait") {
        lm.init(320, 480);
        REQUIRE(lm.type() == LayoutType::TINY_PORTRAIT);
    }
    SECTION("400x480") {
        lm.init(400, 480);
        REQUIRE(lm.type() == LayoutType::TINY_PORTRAIT);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Micro/Tiny boundary at min_dim=272", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("480x272 is micro (boundary inclusive)") {
        lm.init(480, 272);
        REQUIRE(lm.type() == LayoutType::MICRO);
    }
    SECTION("480x273 is tiny (one above boundary)") {
        lm.init(480, 273);
        REQUIRE(lm.type() == LayoutType::TINY);
    }
}

TEST_CASE_METHOD(LayoutFixture, "Square resolution is STANDARD", "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(500, 500);
    REQUIRE(lm.type() == LayoutType::STANDARD);
}

// ============================================================================
// Override
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "Override forces layout type", "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.set_override("ultrawide");
    lm.init(800, 480);
    REQUIRE(lm.type() == LayoutType::ULTRAWIDE);
    REQUIRE(lm.name() == "ultrawide");
}

TEST_CASE_METHOD(LayoutFixture, "Override normalizes hyphens to underscores", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("tiny-portrait") {
        lm.set_override("tiny-portrait");
        lm.init(800, 480);
        REQUIRE(lm.type() == LayoutType::TINY_PORTRAIT);
        REQUIRE(lm.name() == "tiny_portrait");
    }
    SECTION("micro-portrait") {
        lm.set_override("micro-portrait");
        lm.init(800, 480);
        REQUIRE(lm.type() == LayoutType::MICRO_PORTRAIT);
        REQUIRE(lm.name() == "micro_portrait");
    }
}

TEST_CASE_METHOD(LayoutFixture, "Unknown override name defaults to standard", "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.set_override("bogus");
    lm.init(800, 480);
    REQUIRE(lm.type() == LayoutType::STANDARD);
    REQUIRE(lm.name() == "standard");
}

TEST_CASE_METHOD(LayoutFixture, "Empty override clears override", "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.set_override("ultrawide");
    lm.set_override("");
    lm.init(800, 480);
    REQUIRE(lm.type() == LayoutType::STANDARD);
}

// ============================================================================
// is_standard()
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "is_standard returns true only for STANDARD", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("STANDARD") {
        lm.init(1024, 600);
        REQUIRE(lm.is_standard() == true);
    }
    SECTION("ULTRAWIDE") {
        lm.init(1920, 480);
        REQUIRE(lm.is_standard() == false);
    }
    SECTION("PORTRAIT") {
        lm.init(480, 800);
        REQUIRE(lm.is_standard() == false);
    }
    SECTION("MICRO") {
        lm.init(480, 272);
        REQUIRE(lm.is_standard() == false);
    }
    SECTION("MICRO_PORTRAIT") {
        lm.init(272, 480);
        REQUIRE(lm.is_standard() == false);
    }
    SECTION("TINY") {
        lm.init(480, 320);
        REQUIRE(lm.is_standard() == false);
    }
    SECTION("TINY_PORTRAIT") {
        lm.init(320, 480);
        REQUIRE(lm.is_standard() == false);
    }
}

// ============================================================================
// name()
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "name returns correct string for each type", "[layout-manager]") {
    auto& lm = LayoutManager::instance();

    SECTION("standard") {
        lm.init(1024, 600);
        REQUIRE(lm.name() == "standard");
    }
    SECTION("ultrawide") {
        lm.init(1920, 480);
        REQUIRE(lm.name() == "ultrawide");
    }
    SECTION("portrait") {
        lm.init(480, 800);
        REQUIRE(lm.name() == "portrait");
    }
    SECTION("micro") {
        lm.init(480, 272);
        REQUIRE(lm.name() == "micro");
    }
    SECTION("micro_portrait") {
        lm.init(272, 480);
        REQUIRE(lm.name() == "micro_portrait");
    }
    SECTION("tiny") {
        lm.init(480, 320);
        REQUIRE(lm.name() == "tiny");
    }
    SECTION("tiny_portrait") {
        lm.init(320, 480);
        REQUIRE(lm.name() == "tiny_portrait");
    }
}

// ============================================================================
// resolve_xml_path()
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "resolve_xml_path returns base path for standard",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(1024, 600);
    REQUIRE(lm.resolve_xml_path("home_panel.xml") == "ui_xml/home_panel.xml");
}

TEST_CASE_METHOD(LayoutFixture, "resolve_xml_path falls back to base for non-standard",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(1920, 480);
    // No variant file exists on disk, so should fall back
    REQUIRE(lm.resolve_xml_path("nonexistent_panel.xml") == "ui_xml/nonexistent_panel.xml");
}

// ============================================================================
// Ultrawide fallback integration
//
// The ultrawide layout has no XML overrides — the former bespoke
// ui_xml/ultrawide/home_panel.xml predated the responsive widget grid and was
// removed so ultrawide inherits the standard carousel+grid (which already widens
// the column count dynamically). Every panel therefore falls back to standard.
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "resolve_xml_path falls back to standard for ultrawide panels",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(1920, 480);
    REQUIRE(lm.type() == LayoutType::ULTRAWIDE);

    // No ultrawide overrides exist -> home and controls both resolve to standard.
    REQUIRE(lm.resolve_xml_path("home_panel.xml") == "ui_xml/home_panel.xml");
    REQUIRE(lm.resolve_xml_path("controls_panel.xml") == "ui_xml/controls_panel.xml");
}

TEST_CASE_METHOD(LayoutFixture, "has_override returns false for all ultrawide panels",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(1920, 480);

    REQUIRE(lm.has_override("home_panel.xml") == false);
    REQUIRE(lm.has_override("controls_panel.xml") == false);
}

// ============================================================================
// Micro override integration (requires ui_xml/micro/ files on disk)
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "resolve_xml_path returns micro override when file exists on disk",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(480, 272);
    REQUIRE(lm.type() == LayoutType::MICRO);

    // controls_panel.xml has a micro override -> should resolve to micro path
    REQUIRE(lm.resolve_xml_path("controls_panel.xml") == "ui_xml/micro/controls_panel.xml");
}

TEST_CASE_METHOD(LayoutFixture, "resolve_xml_path falls back for panels without micro override",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(480, 272);
    REQUIRE(lm.type() == LayoutType::MICRO);

    // home_panel.xml has no micro override -> should fall back to standard
    REQUIRE(lm.resolve_xml_path("home_panel.xml") == "ui_xml/home_panel.xml");
}

TEST_CASE_METHOD(LayoutFixture, "has_override returns true for micro controls_panel",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(480, 272);

    REQUIRE(lm.has_override("controls_panel.xml") == true);
    REQUIRE(lm.has_override("home_panel.xml") == false);
}

// ============================================================================
// Portrait family fallback chain (requires ui_xml/portrait/ files on disk)
//
// tiny_portrait and micro_portrait carry no app_layout.xml of their own; they
// inherit the shared ui_xml/portrait/ layer before falling back to base
// (landscape). This lets the small-portrait classes hold only files that differ
// from portrait/ instead of duplicating the whole shell.
// ============================================================================

TEST_CASE_METHOD(LayoutFixture, "tiny_portrait inherits app_layout from portrait layer",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(320, 480);
    REQUIRE(lm.type() == LayoutType::TINY_PORTRAIT);

    // No tiny_portrait/app_layout.xml -> resolves up the chain to portrait/.
    REQUIRE(lm.resolve_xml_path("app_layout.xml") == "ui_xml/portrait/app_layout.xml");
    REQUIRE(lm.has_override("app_layout.xml") == true);
}

TEST_CASE_METHOD(LayoutFixture, "micro_portrait inherits app_layout from portrait layer",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(272, 480);
    REQUIRE(lm.type() == LayoutType::MICRO_PORTRAIT);

    REQUIRE(lm.resolve_xml_path("app_layout.xml") == "ui_xml/portrait/app_layout.xml");
}

TEST_CASE_METHOD(LayoutFixture, "portrait resolves its own app_layout override",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(480, 800);
    REQUIRE(lm.type() == LayoutType::PORTRAIT);

    REQUIRE(lm.resolve_xml_path("app_layout.xml") == "ui_xml/portrait/app_layout.xml");
}

TEST_CASE_METHOD(LayoutFixture, "portrait family still falls back to base for non-overridden files",
                 "[layout-manager]") {
    auto& lm = LayoutManager::instance();
    lm.init(320, 480); // tiny_portrait

    // home_panel.xml has no portrait-family override -> base (landscape).
    REQUIRE(lm.resolve_xml_path("home_panel.xml") == "ui_xml/home_panel.xml");
    REQUIRE(lm.has_override("home_panel.xml") == false);
}
