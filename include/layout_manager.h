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
/// one implementation keeps the threshold that selects ui_xml/portrait/ and the
/// threshold that sizes overlays from ever disagreeing.
LayoutType detect_layout_type(int width, int height);

/// True for every portrait class, i.e. every geometry whose navigation bar is
/// the horizontal bottom strip built by ui_xml/portrait/navigation_bar.xml.
bool is_portrait_layout(LayoutType type);

class LayoutManager {
  public:
    static LayoutManager& instance();

    void init(int width, int height);
    void set_override(const std::string& name);

    LayoutType type() const;
    const std::string& name() const;
    std::string resolve_xml_path(const std::string& filename) const;
    bool has_override(const std::string& filename) const;
    /// Variant directory currently supplying `filename`, or "" when the base
    /// ui_xml/ copy is in effect. `filename` takes the same form as
    /// resolve_xml_path() ("home_panel.xml", "components/progress_bar.xml").
    std::string active_variant_dir(const std::string& filename) const;
    /// True when `dir` names a layout-variant directory under ui_xml/, as
    /// opposed to a content subdirectory such as components/.
    static bool is_variant_dir(const std::string& dir);
    bool is_standard() const;

    /// True after init() has resolved type_ (from dims or override). Callers
    /// that run before Phase 8b (e.g. theme_manager_init) need to know whether
    /// type() holds a real answer or the default-constructed STANDARD.
    bool is_initialized() const {
        return initialized_;
    }

    /// Variant directories to search for a layout override, most specific
    /// first. The base (ui_xml/, and the top-level "anchors" table in
    /// default_layout.json) is always the final fallback and is not included
    /// here. Portrait sub-classes inherit the shared portrait/ layer, so
    /// TINY_PORTRAIT yields {"tiny_portrait", "portrait"}.
    ///
    /// Public because the same search order keys more than XML files:
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
