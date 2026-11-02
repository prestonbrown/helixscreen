// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file widget_resolution.h
 * @brief Deciding which widget a `helix-screen ctl` click/set_value acts on.
 *
 * Split out of remote_control_server.cpp so the rule can be unit-tested: the
 * server itself drags in the transports and the toast manager, but the
 * resolution rule is where the interesting mistakes live and it needs nothing
 * but LVGL.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_event_t;
typedef struct _lv_event_t lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t* e);

namespace helix {

/**
 * @brief Split an indexed name token like "toggle[3]" into name and ordinal.
 *
 * The ordinal disambiguates same-named siblings, which LVGL permits: name
 * resolution only auto-indexes names ending in '#' (lv_obj_tree.c), so two
 * children both named "toggle" both resolve to "toggle".
 *
 * @param token     The token as typed.
 * @param out_name  Receives the name with the bracket suffix stripped.
 * @param out_index Receives the 0-based ordinal.
 * @return false when @p token carries no well-formed "[<digits>]" suffix, in
 *         which case the outputs are untouched and the token is a plain name.
 *         A malformed suffix ("t[", "t[]", "t[x]", "t[-1]") is not an error
 *         here — no widget name in the tree contains a bracket, so the caller
 *         simply fails to find it, which is the same outcome as any typo.
 */
bool parse_indexed_name(const std::string& token, std::string& out_name, int& out_index);

/**
 * @brief Emit a locator for @p o that a human can read, retype, and reuse.
 *
 * Each segment is, in order of preference: the widget's resolved name when it
 * is unique among its siblings; "name[k]" when it is not; the bare child index
 * when the widget has no name of its own. So a locator reads
 * "s/main_content/settings_list/toggle[1]" rather than "s/15/1/1/2".
 *
 * Name segments also survive layout churn that index segments do not: inserting
 * a row above the target shifts every index after it, but not a name.
 *
 * @param o    The widget to describe.
 * @param base Emit relative to this ancestor, with no root prefix. nullptr
 *             emits an absolute locator rooted at "s" (active screen) or "t"
 *             (top layer). When @p base is not an ancestor of @p o, the result
 *             falls back to absolute.
 */
std::string path_of(lv_obj_t* o, lv_obj_t* base = nullptr);

/**
 * @brief How one widget is addressed within its parent — one path segment.
 *
 * The building block path_of() concatenates. Exposed because describe_screen
 * walks the tree top-down and appends a segment per level, rather than paying
 * path_of()'s walk-to-the-root for every widget it lists.
 */
std::string path_segment_for(lv_obj_t* o);

/// A widget whose content reaches past its content area, with the excess in px
/// on each side. LVGL scrolls a scrollable widget by exactly these amounts and
/// clips a non-scrollable one.
struct Overflow {
    lv_obj_t* obj = nullptr;
    int32_t top = 0, bottom = 0, left = 0, right = 0;
};

/// Every widget in @p root's visible subtree, @p root included, that overflows.
std::vector<Overflow> find_overflow(lv_obj_t* root);

/**
 * @brief Resolve a locator emitted by path_of() back to a live widget.
 *
 * Segment dispatch mirrors what path_of() emits: all-digits addresses a child
 * index, "name[k]" the k-th same-named child, anything else the uniquely-named
 * one. An all-numeric locator therefore still resolves exactly as it did before
 * names were introduced, so anything holding an older path keeps working.
 *
 * @param path      "s/..." or "t/..." for an absolute locator; otherwise
 *                  resolved relative to @p base.
 * @param base      Starting point for a relative locator; nullptr means the
 *                  active screen. Ignored for an absolute locator.
 * @param ambiguous Optional; filled with every candidate when a name segment
 *                  matches more than one sibling and no ordinal was given, so
 *                  the caller can report instead of silently taking the first.
 * @return The addressed widget, or nullptr if any segment fails to resolve.
 */
lv_obj_t* resolve_path(const std::string& path, lv_obj_t* base = nullptr,
                       std::vector<lv_obj_t*>* ambiguous = nullptr);

/**
 * @brief A widget carrying a value the user manipulates directly.
 *
 * As opposed to a container that merely happens to be clickable. Composite
 * rows (a settings toggle row, a dropdown row) are shells wrapping one of
 * these.
 */
bool is_value_control(lv_obj_t* o);

/**
 * @brief Whether @p o carries a tap handler somebody explicitly attached.
 *
 * Class-level handlers are deliberately not counted: every lv_obj has one, so
 * counting them would make the predicate universally true. Only an added
 * callback whose filter admits LV_EVENT_CLICKED or LV_EVENT_SHORT_CLICKED
 * means "tapping this does something". SHORT_CLICKED counts because a real
 * quick tap delivers it too.
 *
 * @param cb When set, only a handler that calls this function counts.
 */
bool has_own_click_handler(lv_obj_t* o, lv_event_cb_t cb = nullptr);

/**
 * @brief Whether a tap on @p o reaches any tap handler: its own, or one on an
 * ancestor its events bubble to.
 *
 * False means a tap there does nothing a registered callback can see, which is
 * the case for a widget whose only action is a hold.
 */
bool tap_reaches_handler(lv_obj_t* o);

/**
 * @brief Resolve the widget a `click`/`set_value` should actually land on.
 *
 * A composite settings row is a shell wrapping the control the caller meant,
 * so addressing the row descends to that control. The descent is bounded: a
 * target that is already a click target in its own right is acted on
 * literally, and the search never tunnels past a descendant that is itself a
 * click target. Without those bounds a full-screen context-menu backdrop
 * resolves into whatever control the menu happens to contain and dispatches
 * its event — real G-code from a click whose only intent was to dismiss the
 * menu (#1179).
 *
 * @param target       The addressed widget.
 * @param descended_to Set to the resolved child when it differs from target,
 *                     nullptr otherwise. Required.
 * @param ambiguous    Optional; filled with every candidate when more than one
 *                     exists, so the caller can report instead of guessing.
 * @return The widget to act on — null only when @p target is null.
 */
lv_obj_t* resolve_actionable(lv_obj_t* target, lv_obj_t** descended_to,
                             std::vector<lv_obj_t*>* ambiguous);

/**
 * @brief Why a real tap on this widget would never arrive, or nullptr.
 *
 * `lv_obj_send_event(o, LV_EVENT_CLICKED)` reaches any object at all, so a
 * synthetic click reports success on widgets a finger cannot activate. LVGL's
 * input device is stricter: it hit-tests on LV_OBJ_FLAG_CLICKABLE and gates
 * PRESSED, PRESSING and CLICKED on !lv_obj_has_state(o, LV_STATE_DISABLED)
 * (lv_indev.c). Anything the indev would refuse is named here so a remote click
 * can refuse it too, instead of handing back a success a real panel would not
 * reproduce.
 *
 * Checked in the indev's own order, so the reported reason is the first one a
 * finger would hit. The returned string is a literal, valid for the caller's
 * lifetime.
 *
 * @return Literal reason ("hidden", "not clickable", "disabled"), or nullptr
 *         when a tap would land.
 */
const char* click_blocker(lv_obj_t* obj);

/**
 * @brief The nearest ancestor that clips @p obj out of sight, or nullptr when it shows.
 *
 * Every ancestor without LV_OBJ_FLAG_OVERFLOW_VISIBLE clips its children to its
 * own box, so what shows is the intersection of those boxes. Per axis, a widget
 * that fits in that area must lie wholly inside it; one larger than it must at
 * least overlap it. A hidden ancestor (@p obj included) or a screen that is
 * not loaded is returned as the clipper too.
 */
lv_obj_t* view_clipper(lv_obj_t* obj);

/**
 * @brief Scroll every scrollable ancestor of @p obj until it shows, then check it does.
 *
 * The widget's own parent is often a plain column that does not scroll, while
 * the container that does sits further up, so each ancestor is scrolled in turn.
 *
 * @return The ancestor still clipping @p obj (see view_clipper()), or nullptr.
 */
lv_obj_t* scroll_into_view(lv_obj_t* obj);

/**
 * @brief Append every widget below @p root whose name satisfies @p pred, depth first.
 *
 * A widget the author never named is not nameless: lv_obj_get_name_resolved()
 * crafts "<class>_<index>" for it ("lv_label_0") and lv_obj_find_by_name()
 * already matches that form, so the predicate sees the crafted name. The name
 * is built on demand and never stored, which matters on the ESP32 target where
 * naming every widget for real would be paid in heap. Explicit names are still
 * the better thing for a test to drive: a crafted index counts siblings, so
 * inserting a widget renumbers the ones after it.
 *
 * Hidden subtrees are skipped unless @p include_hidden: a widget the user
 * cannot see is never what `click <name>` meant. @p root itself is not tested.
 *
 * @param pred Called with the resolved name (never null); true keeps the widget.
 */
void collect_widgets(lv_obj_t* root, const std::function<bool(const char* name)>& pred,
                     std::vector<lv_obj_t*>& out, bool include_hidden = false);

/**
 * @brief collect_widgets() over the search roots a `ctl` command addresses.
 *
 * @p scope when given (the caller's working directory), otherwise the active
 * screen followed by the top layer, where modals live.
 */
[[nodiscard]] std::vector<lv_obj_t*>
search_widgets(lv_obj_t* scope, const std::function<bool(const char* name)>& pred,
               bool include_hidden = false);

/**
 * @brief Rank one candidate of a by-name search; the highest key wins.
 *
 * Packs three fields into one comparable value, most significant first: the
 * layer @p o sits on (the top layer, where modals live, outranks the screen),
 * the child index of its top-level ancestor (overlays stack as increasing
 * indices under the screen), and @p discovery_order as the final tie-break.
 *
 * The return type is 64-bit for a reason that is invisible on the build host:
 * the layer rank occupies bit 40, so a 32-bit accumulator cannot hold it. On
 * every 32-bit device target (armv7 K1/K2/AD5M/CC1, armhf Pi) `long` is 32
 * bits, the shift is undefined, and the rank and index fields collapse into
 * each other - `ctl click <name>` then picks a widget under the panel instead
 * of the modal on top of it.
 *
 * @param o               The candidate widget.
 * @param discovery_order Its position in the caller's match list.
 */
[[nodiscard]] int64_t widget_pick_key(lv_obj_t* o, size_t discovery_order);

/**
 * @brief Pick the match the user is actually looking at.
 *
 * The highest widget_pick_key() among @p matches. Without this, a name that
 * exists in both a background overlay and the visible one resolves to the
 * background copy and the click becomes a silent no-op.
 *
 * @return The winning widget, or nullptr when @p matches is empty.
 */
[[nodiscard]] lv_obj_t* topmost_visible(const std::vector<lv_obj_t*>& matches);

} // namespace helix
