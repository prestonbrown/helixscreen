// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_splash_asset_choice.cpp
 * @brief The boot splash and the app splash must choose the same artwork
 *
 * `helix-splash` and `show_splash_screen()` draw on the same display within a
 * second of each other. A disagreement between them is not a subtle bug: it is
 * the picture visibly changing under the user at handoff. Both call
 * choose_splash_asset(), so these tests are what stands behind that.
 *
 * The function is pure and takes its resolver, so every branch is reachable by
 * saying which files exist - no filesystem, no LVGL, no display.
 */

#include "../../include/prerender_size_class.h"
#include "../../include/splash_asset_choice.h"

#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A resolver over a fixed set of "files that exist". Records what was asked for
/// so a test can assert the order of the search, not just its result.
class FakeAssets {
  public:
    explicit FakeAssets(std::set<std::string> present) : present_(std::move(present)) {}

    SplashAssetResolver resolver() {
        return [this](const std::string& rel) -> std::string {
            asked_.push_back(rel);
            return present_.count(rel) ? "A:" + rel : std::string();
        };
    }

    const std::vector<std::string>& asked() const {
        return asked_;
    }

  private:
    std::set<std::string> present_;
    std::vector<std::string> asked_;
};

std::string canvas(const std::string& mode, const std::string& cls) {
    return "assets/images/prerendered/splash-3d-" + mode + "-" + cls + ".bin";
}
std::string logo(const std::string& cls) {
    return "assets/images/prerendered/splash-logo-" + cls + ".bin";
}
const char* kPng3dDark = "assets/images/helixscreen-logo-3d-dark.png";
const char* kLogoPng = "assets/images/helixscreen-logo.png";

} // namespace

TEST_CASE("a panel with its own canvas gets it", "[splash][assets]") {
    // 800x480 selects medium, and medium composites at exactly 480.
    FakeAssets fs({canvas("dark", "medium")});
    const SplashChoice c = choose_splash_asset(800, 480, true, fs.resolver());

    REQUIRE(c.kind == SplashAssetKind::FullScreen3DBin);
    REQUIRE(c.path == "A:" + canvas("dark", "medium"));
    REQUIRE(c.size_class == "medium");
    REQUIRE_FALSE(c.canvas_too_tall);
}

TEST_CASE("dark and light pick different files", "[splash][assets]") {
    FakeAssets dark({canvas("dark", "medium")});
    FakeAssets light({canvas("light", "medium")});

    REQUIRE(choose_splash_asset(800, 480, true, dark.resolver()).path ==
            "A:" + canvas("dark", "medium"));
    REQUIRE(choose_splash_asset(800, 480, false, light.resolver()).path ==
            "A:" + canvas("light", "medium"));

    SECTION("asking for the wrong mode finds nothing and falls through") {
        FakeAssets only_dark({canvas("dark", "medium"), kLogoPng});
        const SplashChoice c = choose_splash_asset(800, 480, false, only_dark.resolver());
        REQUIRE(c.kind == SplashAssetKind::LogoPng);
    }
}

TEST_CASE("a small panel falls back to the tiny canvas and moves its class with it",
          "[splash][assets]") {
    // 480x400 selects small. With no small canvas but a tiny one present, the
    // class must become tiny: the height check and the logo lookup that follow
    // both have to ask about the canvas actually in hand.
    FakeAssets fs({canvas("dark", "tiny")});
    const SplashChoice c = choose_splash_asset(480, 400, true, fs.resolver());

    REQUIRE(c.kind == SplashAssetKind::FullScreen3DBin);
    REQUIRE(c.size_class == "tiny");
    REQUIRE(c.path == "A:" + canvas("dark", "tiny"));
    // tiny composites at 320, which fits a 400px panel - so it is NOT discarded.
    REQUIRE_FALSE(c.canvas_too_tall);
}

TEST_CASE("the tiny fallback is only offered to a small panel", "[splash][assets]") {
    // A medium panel with no medium canvas must not quietly take the tiny one;
    // that would be a 480x320 image on an 800x480 screen.
    FakeAssets fs({canvas("dark", "tiny"), kLogoPng});
    const SplashChoice c = choose_splash_asset(800, 480, true, fs.resolver());

    REQUIRE(c.kind == SplashAssetKind::LogoPng);
    REQUIRE(c.size_class == "medium");
}

TEST_CASE("a canvas taller than the panel is refused", "[splash][assets]") {
    // 480x272 selects micro. Hand it the tiny canvas (320px) under micro's name
    // to force the overdraw case: the class is micro, whose composite is 272.
    SECTION("a fitting canvas is kept") {
        FakeAssets fs({canvas("dark", "micro")});
        const SplashChoice c = choose_splash_asset(480, 272, true, fs.resolver());
        REQUIRE(c.kind == SplashAssetKind::FullScreen3DBin);
        REQUIRE_FALSE(c.canvas_too_tall);
    }

    SECTION("a panel shorter than its class refuses the canvas") {
        // 480x300 still selects tiny (narrow axis 300 <= 390), which composites
        // at 320 - taller than the 300px panel.
        REQUIRE(std::string(get_splash_3d_size_name(480, 300)) == "tiny");
        REQUIRE(get_splash_3d_target_height("tiny") == 320);

        FakeAssets fs({canvas("dark", "tiny"), kPng3dDark});
        const SplashChoice c = choose_splash_asset(480, 300, true, fs.resolver());

        REQUIRE(c.canvas_too_tall);
        REQUIRE(c.kind == SplashAssetKind::Source3DPng);
        REQUIRE(c.path == std::string("A:") + kPng3dDark);
    }
}

TEST_CASE("a refused canvas still prefers the 3D source over the flat logo", "[splash][assets]") {
    // Both fallbacks present: the scalable 3D source wins, because the flat logo
    // is a different picture entirely and the two binaries would then disagree
    // about which one the user sees.
    FakeAssets fs({canvas("dark", "tiny"), kPng3dDark, logo("tiny"), kLogoPng});
    const SplashChoice c = choose_splash_asset(480, 300, true, fs.resolver());

    REQUIRE(c.canvas_too_tall);
    REQUIRE(c.kind == SplashAssetKind::Source3DPng);
}

TEST_CASE("with no canvas at all the ladder walks down", "[splash][assets]") {
    SECTION("3D source next") {
        FakeAssets fs({kPng3dDark, logo("medium"), kLogoPng});
        REQUIRE(choose_splash_asset(800, 480, true, fs.resolver()).kind ==
                SplashAssetKind::Source3DPng);
    }
    SECTION("then the pre-rendered logo") {
        FakeAssets fs({logo("medium"), kLogoPng});
        const SplashChoice c = choose_splash_asset(800, 480, true, fs.resolver());
        REQUIRE(c.kind == SplashAssetKind::LogoBin);
        REQUIRE(c.path == "A:" + logo("medium"));
    }
    SECTION("then the logo source") {
        FakeAssets fs({kLogoPng});
        REQUIRE(choose_splash_asset(800, 480, true, fs.resolver()).kind ==
                SplashAssetKind::LogoPng);
    }
    SECTION("and an empty tree yields nothing rather than a bad path") {
        FakeAssets fs({});
        const SplashChoice c = choose_splash_asset(800, 480, true, fs.resolver());
        REQUIRE(c.kind == SplashAssetKind::Nothing);
        REQUIRE(c.path.empty());
    }
}

TEST_CASE("the logo lookup uses the class the canvas search settled on", "[splash][assets]") {
    SECTION("with no canvas at all the class does not move") {
        // The class only moves when a tiny CANVAS is actually taken. A 480x400
        // panel with no canvas stays small, and there is no splash-logo-small in
        // the generated set, so it drops to the PNG.
        FakeAssets fs({logo("tiny"), kLogoPng});
        const SplashChoice c = choose_splash_asset(480, 400, true, fs.resolver());
        REQUIRE(c.size_class == "small");
        REQUIRE(c.kind == SplashAssetKind::LogoPng);
    }

    SECTION("a refused canvas still hands its class to the logo lookup") {
        // 480x310 is tiny class; the tiny canvas is 320px and so is refused, and
        // the ladder reaches the logo under the class it settled on.
        //
        // Note this does NOT exercise a class that MOVED: the move at step 2 only
        // happens when the tiny canvas is taken, and a taken canvas returns
        // immediately. A small-class panel is 391px or taller on its narrow axis,
        // so the 320px tiny canvas always fits it. The moved-class-then-logo path
        // is unreachable, which is why reverting that lookup to a freshly computed
        // class is equivalent code rather than a gap.
        FakeAssets fs({canvas("dark", "tiny"), logo("tiny"), kLogoPng});
        const SplashChoice c = choose_splash_asset(480, 310, true, fs.resolver());
        REQUIRE(c.canvas_too_tall);
        REQUIRE(c.size_class == "tiny");
        REQUIRE(c.kind == SplashAssetKind::LogoBin);
        REQUIRE(c.path == "A:" + logo("tiny"));
    }
}

TEST_CASE("the search order is canvas, 3D source, logo bin, logo source", "[splash][assets]") {
    // Order matters as much as the result: a caller that found the logo first
    // would show the flat picture on a device that had the canvas.
    FakeAssets fs({kLogoPng});
    choose_splash_asset(800, 480, true, fs.resolver());

    const std::vector<std::string>& asked = fs.asked();
    REQUIRE(asked.size() == 4);
    CHECK(asked[0] == canvas("dark", "medium"));
    CHECK(asked[1] == kPng3dDark);
    CHECK(asked[2] == logo("medium"));
    CHECK(asked[3] == kLogoPng);
}

TEST_CASE("nothing is asked for once a choice is made", "[splash][assets]") {
    // The canvas is present, so exactly one lookup should happen. A caller that
    // kept probing would stat files on every boot for no reason.
    FakeAssets fs({canvas("dark", "medium"), kPng3dDark, logo("medium"), kLogoPng});
    choose_splash_asset(800, 480, true, fs.resolver());
    REQUIRE(fs.asked().size() == 1);
}

TEST_CASE("every class a real panel selects resolves to a canvas name", "[splash][assets]") {
    // Walk the panels the fleet actually has, post-rotation.
    struct Panel {
        const char* name;
        int w, h;
        const char* cls;
    };
    const Panel panels[] = {
        {"CC1", 480, 272, "micro"},      {"Snapmaker U1", 480, 320, "tiny"},
        {"K2 Plus", 800, 480, "medium"}, {"K1C", 800, 480, "medium"},
        {"AD5X", 800, 480, "medium"},    {"AD5M", 800, 480, "medium"},
    };

    for (const Panel& p : panels) {
        INFO(p.name << " " << p.w << "x" << p.h);
        FakeAssets fs({canvas("light", p.cls)});
        const SplashChoice c = choose_splash_asset(p.w, p.h, false, fs.resolver());
        REQUIRE(c.size_class == p.cls);
        REQUIRE(c.kind == SplashAssetKind::FullScreen3DBin);
        // Every shipped panel must take its own canvas without overdraw.
        REQUIRE_FALSE(c.canvas_too_tall);
    }
}
