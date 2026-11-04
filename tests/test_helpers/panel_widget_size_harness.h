// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/tile_sizing.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix {

/// Per-widget config applied before the component name is resolved.
///
/// PanelWidgetManager's order is: construct, set_config(), get_component_name(),
/// attach(). Widgets whose component name depends on config — fan_stack picks
/// between its stack and carousel layouts — resolve to the wrong component
/// without this.
struct HarnessConfig {
    nlohmann::json value;
};

/// Every box from @p w_from x @p h_from to @p w_to x @p h_to, a pixel apart. Hold one
/// axis (from == to) to sweep the other.
inline std::vector<std::pair<int, int>> box_sweep(int w_from, int w_to, int h_from, int h_to) {
    std::vector<std::pair<int, int>> boxes;
    for (int w = w_from; w <= w_to; ++w) {
        for (int h = h_from; h <= h_to; ++h) {
            boxes.emplace_back(w, h);
        }
    }
    return boxes;
}

/// The XML-component + resize half of a harness, without the widget instance.
///
/// Split out so the registry-driven harness below reuses the create/attach and
/// resize bodies verbatim rather than growing a second copy of the ordering
/// rules documented on resize().
class PanelWidgetHarnessBase {
  public:
    /// Drive a size change and settle the layout. Does not pump timers; a test
    /// that needs those calls process_lvgl() itself.
    ///
    /// width_px/height_px are the widget's real granted cell size in production
    /// (PanelWidgetManager computes them via grid_track_extent() and the widget
    /// sits in that grid cell — panel_widget_manager.cpp). Apply them to obj_
    /// here too, or any assertion about geometry measures an unconstrained
    /// object rather than a sized one — LV_PCT()-based children resolve
    /// against whatever obj_'s last real size happened to be, not the size
    /// this call claims to represent.
    ///
    /// Order matters: obj_ must already report its new size (via an
    /// intervening layout pass) *before* on_size_changed() runs, because a
    /// widget's own on_size_changed can read that geometry — e.g.
    /// ToolSwitcherWidget::rebuild_pills() measures a child container's
    /// height, which depends on obj_'s width already having settled.
    /// lv_obj_get_width()-style getters read the last computed coord, not
    /// what was just lv_obj_set_size()'d (see tests/CLAUDE.md's "LVGL traps"
    /// section) — so set + update_layout has to happen first, not just
    /// first-in-program-order.
    void resize(int colspan, int rowspan, int width_px, int height_px) {
        lv_obj_set_size(obj_, width_px, height_px);
        lv_obj_update_layout(obj_);
        if (instance_) {
            // notify_size_changed(), not on_size_changed(): the manager calls the
            // recording entry point, and a widget that replays the last granted
            // size on rebuild only sees one if the harness records it too.
            instance_->notify_size_changed(colspan, rowspan, width_px, height_px);
        }
        lv_obj_update_layout(obj_);
    }

    lv_obj_t* child(const char* name) {
        return lv_obj_find_by_name(obj_, name);
    }
    lv_obj_t* root() {
        return obj_;
    }

    /// Each of @p boxes, as "WxH", whose drawn parts change when it is decided a second
    /// time. A verdict that reads the parts the last one left visible settles only there.
    std::vector<std::string> unsettled_boxes(const std::vector<std::pair<int, int>>& boxes) {
        std::vector<std::string> unsettled;
        for (const auto& [w, h] : boxes) {
            resize(1, 1, w, h);
            const std::string first = drawn_parts(obj_);
            resize(1, 1, w, h);
            if (drawn_parts(obj_) != first) {
                unsettled.push_back(std::to_string(w) + "x" + std::to_string(h));
            }
        }
        return unsettled;
    }

  protected:
    /// The names of @p obj's drawn descendants, depth first.
    static std::string drawn_parts(lv_obj_t* obj) {
        std::string parts;
        const uint32_t count = lv_obj_get_child_count(obj);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
                continue;
            if (const char* name = lv_obj_get_name(child))
                parts.append(name).append(" ");
            parts += drawn_parts(child);
        }
        return parts;
    }

    /// Resolve the component name from the widget, build it from XML, and
    /// attach. From the widget, not "panel_widget_" + id(): fan_stack selects
    /// between stack and carousel components, and favorite_macro's id carries
    /// an instance suffix that would produce an unregistered component name.
    ///
    /// `require` is false for the registry sweep, which must report an
    /// unbuildable component rather than abort the whole run on it.
    /// The track geometry of the panel being simulated, so a tile measures its
    /// floor against the same cell the manager would give it.
    void set_cell_metrics(const helix::CellMetrics& m) {
        cell_metrics_ = m;
        has_cell_metrics_ = true;
    }

    bool create_and_attach(lv_obj_t* screen, PanelWidget& widget, bool require = true) {
        instance_ = &widget;
        const std::string component = widget.get_component_name();
        // The same attributes PanelWidgetManager hands lv_xml_create(). Passing
        // nullptr here builds a tile whose per-instance subject names never
        // reach its XML, so every binding on them is dropped at parse time and
        // the tile cannot answer its own sizing verdict: it measures as though
        // the feature were absent.
        obj_ = static_cast<lv_obj_t*>(lv_xml_create(screen, component.c_str(), widget.xml_attrs()));
        if (require) {
            REQUIRE(obj_ != nullptr);
        }
        if (!obj_) {
            return false;
        }
        if (auto* sizing = widget.tile_sizing()) {
            sizing->set_content_root(obj_);
            if (has_cell_metrics_) {
                sizing->set_cell_metrics(cell_metrics_);
            }
        }
        lv_obj_add_flag(obj_, kPanelWidgetTileFlag);
        widget.attach_tile(obj_, screen);
        return true;
    }

    /// Build a component with no PanelWidget behind it — the pure-XML widgets
    /// in the registry, which PanelWidgetManager also creates from
    /// "panel_widget_" + id with no instance to attach.
    bool create_only(lv_obj_t* screen, const char* component) {
        obj_ = static_cast<lv_obj_t*>(lv_xml_create(screen, component, nullptr));
        return obj_ != nullptr;
    }

    PanelWidget* instance_ = nullptr;
    lv_obj_t* obj_ = nullptr;
    helix::CellMetrics cell_metrics_{};
    bool has_cell_metrics_ = false;
};

/// Creates a widget's real XML component, attaches the widget to it, and
/// drives on_size_changed().
///
/// Variadic over the constructor so one harness covers every shape in the set:
/// default-constructed, MoonrakerAPI*, PrinterState&, (string, PrinterState&),
/// and instance-id string.
template <typename W> class PanelWidgetHarness : public PanelWidgetHarnessBase {
  public:
    template <typename... Args>
    explicit PanelWidgetHarness(lv_obj_t* screen, Args&&... args)
        : widget_(std::forward<Args>(args)...) {
        create_and_attach(screen, widget_);
    }

    /// Overload for widgets whose get_component_name() depends on config
    /// (e.g. fan_stack's stack/carousel choice). Calls set_config() before
    /// the component name is resolved, matching PanelWidgetManager's real
    /// construction order.
    template <typename... Args>
    PanelWidgetHarness(lv_obj_t* screen, HarnessConfig config, Args&&... args)
        : widget_(std::forward<Args>(args)...) {
        widget_.set_config(config.value);
        create_and_attach(screen, widget_);
    }

    ~PanelWidgetHarness() {
        widget_.detach_tile();
    }

    PanelWidgetHarness(const PanelWidgetHarness&) = delete;
    PanelWidgetHarness& operator=(const PanelWidgetHarness&) = delete;

    W& widget() {
        return widget_;
    }

  private:
    W widget_;
};

/// Builds any registry entry the way PanelWidgetManager does, without naming
/// its C++ class.
///
/// PanelWidgetManager's order (panel_widget_manager.cpp) is: factory(id) ->
/// set_config() -> get_component_name() -> lv_xml_create() -> attach() ->
/// on_size_changed(). Reproducing it through PanelWidgetDef::factory is what
/// lets one loop drive all ~38 definitions; PanelWidgetHarness<W> needs the
/// concrete type and each widget's own constructor shape.
///
/// A definition with no factory is a pure-XML widget: the component is still
/// created, there is just no instance to attach or resize.
class RegistryWidgetHarness : public PanelWidgetHarnessBase {
  public:
    RegistryWidgetHarness(lv_obj_t* screen, const PanelWidgetDef& def,
                          const helix::CellMetrics* metrics = nullptr) {
        if (metrics) {
            set_cell_metrics(*metrics);
        }
        if (def.factory) {
            owned_ = def.factory(def.id);
        }
        if (owned_) {
            owned_->set_config(nlohmann::json::object());
            created_ = create_and_attach(screen, *owned_, /*require=*/false);
        } else {
            created_ = create_only(screen, (std::string("panel_widget_") + def.id).c_str());
        }
    }

    ~RegistryWidgetHarness() {
        if (owned_) {
            owned_->detach_tile();
        }
        // Deleted here rather than left to the fixture's screen teardown: the
        // sweep builds every definition once per geometry, and a live tree of
        // several hundred widgets changes what the next one measures.
        if (obj_) {
            lv_obj_delete(obj_);
        }
    }

    RegistryWidgetHarness(const RegistryWidgetHarness&) = delete;
    RegistryWidgetHarness& operator=(const RegistryWidgetHarness&) = delete;

    /// False when the XML component could not be built at all.
    bool created() const {
        return created_;
    }

    /// What the grid does once the tile is on screen; a no-op for a pure-XML widget.
    void activate() {
        if (owned_) {
            owned_->on_activate();
        }
    }

  private:
    std::unique_ptr<PanelWidget> owned_;
    bool created_ = false;
};

/// Fails when font tokens are not resolving.
///
/// theme_manager_get_font() falls back to lv_font_get_default() on any miss, so
/// with an uninitialized theme every token returns the same pointer and both
/// branches of a font assertion compare equal while proving nothing. Call this
/// before any font assertion.
inline void require_font_tokens_distinct() {
    REQUIRE(theme_manager_get_font("font_xs") != theme_manager_get_font("font_body"));
}

// ---------------------------------------------------------------------------
// Content-fit detection
// ---------------------------------------------------------------------------

/// Which of the three independent overflow conditions fired.
///
/// They are not restatements of each other. Geometry catches a child drawn
/// outside its parent's usable area; Scroll catches content the layout
/// repositioned *inside* the box while still needing more room than the box
/// has; Text catches a string that fits its label's box only because the label
/// renderer shortened it.
enum class OverflowKind { Geometry, Scroll, Text };

inline const char* overflow_kind_name(OverflowKind k) {
    switch (k) {
    case OverflowKind::Geometry:
        return "geometry";
    case OverflowKind::Scroll:
        return "scroll";
    case OverflowKind::Text:
        return "text";
    }
    return "?";
}

/// One place where laid-out content did not fit the box it was given.
struct OverflowFinding {
    OverflowKind kind;
    std::string path;   ///< Slash-joined object names from the widget root
    std::string detail; ///< Measured numbers, in px
};

/// Subtrees whose overflow is the design, listed by object name.
///
/// A name, not a flag test, because LVGL sets LV_OBJ_FLAG_SCROLLABLE on every
/// plain lv_obj at construction (lv_obj.c) — which is why this tree writes
/// scrollable="false" in 1600+ places to opt back out. "The flag is set" is
/// therefore indistinguishable from "nobody thought about it", and skipping on
/// the flag alone would silence the gate everywhere it matters. Naming each
/// exempt object keeps the list countable and reviewable.
struct OverflowExceptions {
    std::vector<std::string> scrollable_names;

    bool exempts(const char* name) const {
        if (!name) {
            return false;
        }
        return std::find(scrollable_names.begin(), scrollable_names.end(), std::string(name)) !=
               scrollable_names.end();
    }
};

/// Everything one sweep of a widget tree saw: the failures, the subtrees that
/// were deliberately not walked, and how close the tightest passing box came
/// to overflowing.
struct OverflowReport {
    std::vector<OverflowFinding> findings;
    std::vector<std::string> skipped;  ///< path + reason, one per skipped subtree
    int min_slack_px = INT32_MAX;      ///< Smallest surviving geometric gap, either axis
    int min_text_slack_px = INT32_MAX; ///< Smallest surviving gap around a rendered string

    bool clean() const {
        return findings.empty();
    }
};

namespace detail {

/// Slash path to an object. Unnamed objects fall back to their child index —
/// lv_obj_class_t::name lives in a private LVGL header, and an index is enough
/// to walk back to the offending node in the XML.
inline std::string obj_path(const std::string& parent, lv_obj_t* o, int index) {
    const char* n = lv_obj_get_name(o);
    std::string leaf = n ? n : ("#" + std::to_string(index));
    if (lv_obj_check_type(o, &lv_label_class)) {
        leaf += "(label)";
    }
    return parent.empty() ? leaf : parent + "/" + leaf;
}

/// True when the label itself declares that its text may be shortened.
///
/// DOTS ellipsizes on purpose, SCROLL and SCROLL_CIRCULAR are marquees whose
/// whole point is that the string is longer than the box. CLIP is *not* on this
/// list: it silently cuts with no affordance, which is the failure this check
/// exists to name.
inline bool label_declares_truncation(lv_obj_t* o) {
    lv_label_long_mode_t m = lv_label_get_long_mode(o);
    return m == LV_LABEL_LONG_MODE_DOTS || m == LV_LABEL_LONG_MODE_SCROLL ||
           m == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
}

inline void note_slack(OverflowReport& r, int slack) {
    r.min_slack_px = std::min(r.min_slack_px, slack);
}

inline void note_text_slack(OverflowReport& r, int slack) {
    r.min_text_slack_px = std::min(r.min_text_slack_px, slack);
}

inline void check_text(lv_obj_t* o, const std::string& path, OverflowReport& r) {
    const char* txt = lv_label_get_text(o);
    if (!txt || txt[0] == '\0') {
        return;
    }

    // The label's OWN resolved font and spacing. Reading a token here instead
    // would compare the string against a face it is not drawn in, and the check
    // would pass or fail for reasons unrelated to the widget — the same vacuity
    // require_font_tokens_distinct() guards against.
    const lv_font_t* font = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    if (!font) {
        return;
    }
    const int32_t letter_space = lv_obj_get_style_text_letter_space(o, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(o, LV_PART_MAIN);
    const int32_t box_w = lv_obj_get_content_width(o);
    const int32_t box_h = lv_obj_get_content_height(o);
    if (box_w <= 0 || box_h <= 0) {
        return;
    }

    // WRAP reflows within the box, so it is measured against the box width and
    // can only fail on height. Every other mode keeps one line, so it is
    // measured unconstrained and fails on width.
    const bool wraps = lv_label_get_long_mode(o) == LV_LABEL_LONG_MODE_WRAP;
    lv_point_t size{};
    lv_text_get_size(&size, txt, font, letter_space, line_space, wraps ? box_w : LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);

    // Slack is only meaningful where the box is imposed on the string. A
    // content-sized label is exactly as wide as its own text, so it reports
    // zero slack on every icon glyph in the tree and would drown the "passes,
    // but only just" signal it exists to carry.
    const bool box_is_imposed = lv_obj_get_style_width(o, LV_PART_MAIN) != LV_SIZE_CONTENT;

    if (size.x > box_w) {
        r.findings.push_back({OverflowKind::Text, path,
                              "text is " + std::to_string(size.x) + "px wide in a " +
                                  std::to_string(box_w) + "px box: \"" + txt + "\""});
    } else if (box_is_imposed) {
        note_text_slack(r, box_w - size.x);
    }
    if (size.y > box_h) {
        r.findings.push_back({OverflowKind::Text, path,
                              "text is " + std::to_string(size.y) + "px tall in a " +
                                  std::to_string(box_h) + "px box: \"" + txt + "\""});
    } else if (box_is_imposed) {
        note_text_slack(r, box_h - size.y);
    }
}

inline void walk(lv_obj_t* o, const std::string& path, const OverflowExceptions& ex,
                 OverflowReport& r) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return; // Not rendered, so nothing to clip.
    }

    const char* name = lv_obj_get_name(o);
    const bool scroll_exempt = lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
                               lv_obj_get_scroll_dir(o) != LV_DIR_NONE && ex.exempts(name);
    if (scroll_exempt) {
        r.skipped.push_back(path + " (named scrollable exception)");
        return;
    }

    if (lv_obj_check_type(o, &lv_label_class)) {
        // A label that declares truncation is exempt from BOTH text-side
        // checks, not just the measured one. LVGL derives a label's scroll
        // extent from its text self-size, so an ellipsized or marquee string
        // reports scroll_right/scroll_bottom for exactly the reason its
        // long_mode already accepts — leaving that half unexempted would let
        // the same intentional string fail under a different heading.
        if (label_declares_truncation(o)) {
            r.skipped.push_back(path + " (label long_mode declares truncation)");
            return;
        }
        check_text(o, path, r);
    }

    // Content that the layout kept inside the box by shifting it: the children
    // are all in bounds, and the box still cannot show them all at once.
    // An image reports its source's natural size as its self size whatever its box
    // draws, so a thumbnail scaled to fit (inner_align contain) still shows as scrollable. Its
    // box is still held to the parent by the geometry check below.
    const bool is_image = lv_obj_check_type(o, &lv_image_class) &&
                          lv_image_get_inner_align(o) == LV_IMAGE_ALIGN_CONTAIN;
    const int32_t sb = is_image ? 0 : lv_obj_get_scroll_bottom(o);
    const int32_t sr = is_image ? 0 : lv_obj_get_scroll_right(o);
    if (sb > 0) {
        r.findings.push_back(
            {OverflowKind::Scroll, path, "scroll_bottom " + std::to_string(sb) + "px"});
    }
    if (sr > 0) {
        r.findings.push_back(
            {OverflowKind::Scroll, path, "scroll_right " + std::to_string(sr) + "px"});
    }

    // The parent's CONTENT box, not its outer coords: padding and border are
    // not usable space, and a child sitting in the padding is already clipped
    // on any container that clips.
    lv_area_t content;
    lv_obj_get_content_coords(o, &content);

    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const std::string cpath = obj_path(path, c, static_cast<int>(i));

        lv_area_t ca;
        lv_obj_get_coords(c, &ca);
        const int left = content.x1 - ca.x1;
        const int right = ca.x2 - content.x2;
        const int top = content.y1 - ca.y1;
        const int bottom = ca.y2 - content.y2;
        const int worst = std::max(std::max(left, right), std::max(top, bottom));
        if (worst > 0) {
            r.findings.push_back({OverflowKind::Geometry, cpath,
                                  "outside parent content box by " + std::to_string(worst) +
                                      "px (l/r/t/b " + std::to_string(left) + "/" +
                                      std::to_string(right) + "/" + std::to_string(top) + "/" +
                                      std::to_string(bottom) + ")"});
        } else {
            note_slack(r, -worst);
        }

        walk(c, cpath, ex, r);
    }
}

} // namespace detail

/// Walk `root` and report every place its content does not fit.
///
/// Runs all three checks — see OverflowKind. `root` is expected to have been
/// laid out already (PanelWidgetHarnessBase::resize() does that); an unsettled
/// tree reports whatever stale coords it happens to hold.
inline OverflowReport collect_overflow(lv_obj_t* root, const OverflowExceptions& ex = {}) {
    OverflowReport r;
    if (!root) {
        return r;
    }
    lv_obj_update_layout(root);
    detail::walk(root, detail::obj_path("", root, 0), ex, r);
    return r;
}

/// Fail unless every descendant of `root` fits the box it was given.
///
/// The message names the failing object, which of the three checks fired, and
/// the measured pixels, plus every subtree the exception list caused to be
/// skipped — a skip that is not visible in the failure is a skip nobody
/// reviews.
inline void require_no_overflow(lv_obj_t* root, const OverflowExceptions& ex = {},
                                const std::string& context = {}) {
    OverflowReport r = collect_overflow(root, ex);
    if (r.clean()) {
        return;
    }
    std::string msg =
        context.empty() ? std::string("content does not fit") : context + ": content does not fit";
    for (const auto& f : r.findings) {
        msg += "\n  [" + std::string(overflow_kind_name(f.kind)) + "] " + f.path + ": " + f.detail;
    }
    for (const auto& s : r.skipped) {
        msg += "\n  [skipped] " + s;
    }
    FAIL_CHECK(msg);
}

} // namespace helix
