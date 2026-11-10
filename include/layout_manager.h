// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

class LayoutManagerTestAccess;

namespace helix {

enum class LayoutType {
    STANDARD,       // 4:3 to ~16:9
    ULTRAWIDE,      // >2.5:1 aspect ratio
    PORTRAIT,       // <0.8:1
    MICRO,          // landscape, 480x272 class
    MICRO_PORTRAIT, // portrait, 480x272 class
    TINY,           // landscape, max dim <=480
    TINY_PORTRAIT   // portrait, max dim <=480
};

/// Classify a screen geometry into a layout variant.
///
/// Free-standing because callers that run before LayoutManager::init() need the
/// same answer. Application initialises the theme (phase 6) well before the
/// layout manager (phase 8b), and the overlay-width constants computed there
/// have to know whether the nav bar is a vertical strip or a bottom bar. Sharing
/// one implementation keeps the threshold that sets ui_is_portrait and the
/// threshold that sizes overlays from ever disagreeing.
LayoutType detect_layout_type(int width, int height);

/// The ultrawide test: wider than 2.5:1. detect_layout_type() applies it after
/// the micro/tiny classes; callers that size things from raw dimensions before
/// LayoutManager exists apply it through here too, so the threshold has one home.
bool is_ultrawide_aspect(int width, int height);

/// True for every portrait class, i.e. every geometry whose navigation bar is
/// the horizontal bottom strip ui_xml/navigation_bar.xml builds when
/// ui_is_portrait is 1.
bool is_portrait_layout(LayoutType type);

/// The app's layout type: LayoutManager's answer once it is initialized, so a
/// --layout override is respected (#1255), and the physical classification of
/// @p width x @p height before then (early startup, tests).
LayoutType app_layout_type(int width, int height);

/// Whether the app is laid out in portrait: LayoutManager's answer once it is
/// initialized, so a --layout override is respected (#1255), and the physical
/// classification of @p width x @p height before then (early startup, tests).
/// The value ui_is_portrait publishes, for C++ that decides placement itself.
bool app_layout_is_portrait(int width, int height);

class LayoutManager {
  public:
    static LayoutManager& instance();

    void init(int width, int height);
    void set_override(const std::string& name);

    LayoutType type() const;
    const std::string& name() const;
    bool is_standard() const;

    /// True after init() has resolved type_ (from dims or override). Callers
    /// that run before Phase 8b (e.g. theme_manager_init) need to know whether
    /// type() holds a real answer or the default-constructed STANDARD.
    bool is_initialized() const {
        return initialized_;
    }

    /// Layout names to search for a per-layout table, most specific first. The
    /// base (the top-level "anchors" table in default_layout.json) is always the
    /// final fallback and is not included here. Portrait sub-classes inherit
    /// the shared portrait layer, so TINY_PORTRAIT yields
    /// {"tiny_portrait", "portrait"}.
    ///
    /// PanelWidgetConfig::build_default_grid() picks its anchor table this way,
    /// so a portrait panel cannot end up on anchors authored for a 6-column
    /// landscape grid (#1216).
    std::vector<std::string> variant_chain() const;

    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }

  private:
    friend class ::LayoutManagerTestAccess;
    LayoutManager() = default;
    LayoutType detect(int width, int height) const;
    static const char* type_to_name(LayoutType type);
    static LayoutType name_to_type(const std::string& name);

    LayoutType type_{LayoutType::STANDARD};
    std::string name_{"standard"};
    std::string override_name_;
    bool initialized_{false};
    int width_{0};
    int height_{0};
};

} // namespace helix
