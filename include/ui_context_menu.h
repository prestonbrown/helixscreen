// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_type_tag.h"

#include <cstddef>
#include <functional>
#include <lvgl.h>
#include <string>

namespace helix::ui {

/**
 * @file ui_context_menu.h
 * @brief Generic context menu component for popup menus near widgets
 *
 * Provides the common mechanics for context menus:
 * - Full-screen semi-transparent backdrop (click to dismiss)
 * - Card positioned near the triggering widget (smart left/right/vertical placement)
 * - Action callback dispatch via integer action IDs
 * - lv_obj_delete_async() for safe dismissal during event processing
 *
 * Subclasses define their own XML component, subjects, and specific actions.
 *
 * ## Usage:
 * @code
 * class MyContextMenu : public ContextMenu {
 *     HELIX_CONTEXT_MENU_KIND(MyContextMenu)
 * protected:
 *     const char* xml_component_name() const override { return "my_context_menu"; }
 *     const char* menu_card_name() const override { return "context_menu"; }
 *     void on_created(lv_obj_t* menu) override {
 *         // Configure menu-specific widgets after XML creation
 *     }
 * };
 * @endcode
 */
class ContextMenu {
  public:
    using ActionCallback = std::function<void(int action, int item_index)>;

    /** @brief Action id dispatched when the menu is dismissed without a choice */
    static constexpr int ACTION_CANCELLED = -1;

    /**
     * @brief Where the card is placed, and what it is placed relative to
     *
     * `ClickPoint` is the original behaviour: the card hangs off the captured
     * pointer position and mirrors about it horizontally when it would overflow.
     * It never flips vertically, which suits a menu raised by tapping a row.
     *
     * `BelowAnchor` places the card under a widget's rectangle and flips it above
     * when the bottom would fall off screen — what a menu raised by a button
     * needs, so the button itself stays visible.
     */
    enum class AnchorMode {
        ClickPoint,
        BelowAnchor,
    };

    /** @brief Horizontal placement of a `BelowAnchor` card against its anchor */
    enum class AnchorAlign {
        Center, ///< Card centred on the anchor's horizontal midpoint
        Left,   ///< Card's left edge aligned to the anchor's left edge
    };

    /**
     * @brief Card width as a share of the screen, clamped to a pixel range
     *
     * A zero `pct` leaves the card at whatever width its XML declared, which is
     * what a `width="content"` menu wants. Menus whose rows are `width="100%"`
     * must be sized before those children can resolve, so they state a policy.
     */
    struct CardWidth {
        int pct = 0;
        int min = 0;
        int max = 0;

        [[nodiscard]] bool is_set() const {
            return pct > 0;
        }
    };

    ContextMenu();
    virtual ~ContextMenu();

    /**
     * @brief The menu currently on screen, or nullptr
     *
     * One menu is raised at a time, so the shared XML callbacks (`backdrop_cb`,
     * `close_cb`) resolve their target through this rather than each menu keeping
     * its own `s_active_` static. Maintained by show/hide and by the card's
     * LV_EVENT_DELETE hook, so it cannot outlive the widget it points at.
     */
    [[nodiscard]] static ContextMenu* active();

    /**
     * @brief This menu's concrete type, as a `helix::type_tag<T>()` value
     *
     * The RTTI-free stand-in for `typeid(*this)`, so `active_as<T>()` can
     * downcast without `dynamic_cast` — the firmware builds `-fno-rtti`.
     * Pure rather than defaulted on purpose: a subclass that forgets it fails to
     * compile as abstract, instead of silently answering with its base's tag.
     * Declare it with HELIX_CONTEXT_MENU_KIND(Self).
     */
    [[nodiscard]] virtual std::size_t kind_tag() const = 0;

    /**
     * @brief The menu currently on screen, if it is a `T`
     *
     * The downcast every subclass callback needs: the registry holds a base
     * pointer, but a menu's own XML callbacks act on their own type. One helper
     * rather than a `dynamic_cast` open-coded at each of the ~40 callback sites.
     *
     * **Exact-type match, unlike the `dynamic_cast` this replaces.** A menu whose
     * kind tag is a *derived* type's does not answer to its base's `active_as<>`.
     * Every subclass today is a direct leaf of ContextMenu, so the two agree; if
     * an intermediate class is ever introduced, code must ask for the leaf type
     * (or the intermediate must dispatch on the tag itself).
     */
    template <typename T> [[nodiscard]] static T* active_as() {
        ContextMenu* a = active();
        return (a != nullptr && a->kind_tag() == helix::type_tag<T>()) ? static_cast<T*>(a)
                                                                       : nullptr;
    }

    /**
     * @brief Register the shared XML event callbacks
     *
     * Wires `context_menu_backdrop_cb` and `context_menu_close_cb`, which every
     * menu's XML references instead of declaring a per-menu pair. Call once at
     * startup, before any menu XML is parsed.
     *
     * Deliberately not named `register_xml_callbacks` — that is a free helper in
     * this namespace (ui_callback_helpers.h) which the subclasses call, and a
     * member of the same name would hide it inside every one of them.
     */
    static void register_shared_callbacks();

    // Non-copyable
    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;

    // Movable
    ContextMenu(ContextMenu&& other) noexcept;
    ContextMenu& operator=(ContextMenu&& other) noexcept;

    /**
     * @brief Show context menu near a widget
     * @param parent Parent screen for the menu
     * @param item_index Index of the item this menu is for
     * @param near_widget Widget to position menu near
     * @return true if menu was shown successfully
     */
    bool show_near_widget(lv_obj_t* parent, int item_index, lv_obj_t* near_widget);

    /**
     * @brief Show the menu under a widget, flipping above it when it would overflow
     *
     * The `AnchorMode::BelowAnchor` counterpart to show_near_widget(). Use it when
     * the menu is raised by a button or a card whose rectangle — not the exact
     * pixel tapped — is what the menu should hang off.
     *
     * @param parent      Parent screen for the backdrop
     * @param item_index  Index of the item this menu is for, or -1
     * @param anchor      Widget whose rectangle the card is placed against
     * @param align       Horizontal placement against that rectangle
     */
    bool show_below_widget(lv_obj_t* parent, int item_index, lv_obj_t* anchor,
                           AnchorAlign align = AnchorAlign::Center);

    /** @brief show_below_widget() for a menu that is not about an indexed item */
    bool show_below_widget(lv_obj_t* parent, lv_obj_t* anchor,
                           AnchorAlign align = AnchorAlign::Center) {
        return show_below_widget(parent, -1, anchor, align);
    }

    /**
     * @brief Set the click point for positioning (call before show)
     * Captures the display-coordinate click point from the triggering event.
     */
    void set_click_point(lv_point_t point) {
        click_point_ = point;
    }

    /**
     * @brief Hide the context menu
     *
     * The widget is deleted asynchronously, but `menu_` is cleared synchronously —
     * `safe_delete_deferred()` takes its pointer by reference. So is_visible() is
     * false the moment this returns, which is what the `show_*()` guards rely on.
     */
    void hide();

    /**
     * @brief Check if menu is currently visible
     */
    [[nodiscard]] bool is_visible() const {
        return menu_ != nullptr;
    }

    /**
     * @brief Get item index the menu is currently shown for
     */
    [[nodiscard]] int get_item_index() const {
        return item_index_;
    }

    /**
     * @brief Set callback for menu actions
     */
    void set_action_callback(ActionCallback callback);

  protected:
    /**
     * @brief Get the XML component name for this menu
     * Subclasses must override to provide their menu's XML component.
     */
    virtual const char* xml_component_name() const = 0;

    /**
     * @brief Get the name of the card widget inside the XML for positioning
     * Default: "context_menu"
     */
    virtual const char* menu_card_name() const {
        return "context_menu";
    }

    /**
     * @brief Called after the menu XML is created, before positioning
     *
     * Subclasses override to configure menu-specific widgets (dropdowns, headers,
     * generated rows). The argument is the **backdrop**, not the card — reach the
     * card and everything under it with `lv_obj_find_by_name()`.
     *
     * The backdrop has been laid out by the time this runs, so measuring against it
     * is safe. The card has not been positioned yet, and its final height is not
     * known until the rows added here have been measured.
     */
    virtual void on_created(lv_obj_t* backdrop) {
        (void)backdrop;
    }

    /**
     * @brief Card width policy, applied before the card's children resolve
     * Default: leave the width the XML declared.
     */
    virtual CardWidth card_width() const {
        return {};
    }

    /**
     * @brief Called when the backdrop is clicked (before hide)
     *
     * This is where a menu states what a tap outside it means. The default is
     * cancel — dispatch `ACTION_CANCELLED` and take no action — which is right
     * for a menu whose whole job is choosing one item.
     *
     * A *configure* menu whose controls already apply live has nothing to cancel,
     * so it overrides this to commit and hide instead. Getting that wrong is not
     * cosmetic: the two multi-select pickers used to disagree, and tapping outside
     * one of them silently discarded the user's changes.
     */
    virtual void on_backdrop_clicked();

    /**
     * @brief Called when the card's own close/Done control is clicked
     * Default: same as a backdrop click, so a menu need only override one of them.
     */
    virtual void on_close_clicked();

    /**
     * @brief Dispatch an action and hide the menu
     * Captures callback, hides, then invokes with action + item_index.
     */
    void dispatch_action(int action);

    // Accessors for subclass use
    /** @brief The backdrop — the whole menu, card included */
    [[nodiscard]] lv_obj_t* menu() const {
        return menu_;
    }
    /** @brief The card inside the backdrop, or nullptr if the XML has no such name */
    [[nodiscard]] lv_obj_t* card() const;
    [[nodiscard]] lv_obj_t* parent() const {
        return parent_;
    }

    /**
     * @brief `pct` percent of the height of the screen this menu is on
     *
     * The measurement every picker needs to cap a scrolling list, and one that is
     * easy to get wrong: the backdrop's own height reads 0 until it has been laid
     * out, so the fraction must come off the screen.
     */
    [[nodiscard]] int32_t screen_height_pct(int pct) const;

  private:
    lv_obj_t* menu_ = nullptr;
    lv_obj_t* parent_ = nullptr;
    int item_index_ = -1;
    lv_point_t click_point_ = {0, 0};
    AnchorMode anchor_mode_ = AnchorMode::ClickPoint;
    AnchorAlign anchor_align_ = AnchorAlign::Center;
    lv_obj_t* anchor_widget_ = nullptr;
    ActionCallback action_callback_;

    /** @brief The menu currently on screen — see active() */
    static inline ContextMenu* s_active_ = nullptr;

    /** @brief Shared body behind show_near_widget() and show_below_widget() */
    bool show_impl(lv_obj_t* parent, int item_index, lv_obj_t* anchor, AnchorMode mode,
                   AnchorAlign align);

    /** @brief Take over another menu's widget and delete hook (move ctor/assign) */
    void adopt_from(ContextMenu& other);

    /**
     * @brief Keep menu_/s_active_ from outliving the backdrop widget
     *
     * The backdrop can be destroyed without going through hide() — a screen swap
     * takes its parent with it. Without this the pointers go stale and the next
     * backdrop tap dispatches through freed memory. Re-pointed on move, since the
     * callback carries `this`.
     */
    void install_delete_hook();
    void uninstall_delete_hook();
    static void on_menu_deleted(lv_event_t* e);

    /** @brief The two XML callbacks every menu shares; both route through active() */
    static void backdrop_cb(lv_event_t* e);
    static void close_cb(lv_event_t* e);

    /** @brief Apply card_width() to the card, before its children resolve */
    void apply_card_width(lv_obj_t* menu_card);

    /**
     * @brief Name of the optional column-group container inside a menu card
     *
     * A card that wants the side-by-side fallback wraps its action groups in a
     * container with this name, one child per group. Cards without it are simply
     * never reflowed.
     */
    static constexpr const char* COLUMNS_NAME = "menu_columns";

    /** @brief Name of a column group's heading block (label + rule), if it has one */
    static constexpr const char* COLUMN_HEADING_NAME = "col_heading";

    /** @brief Shortest a row can be and still plausibly be a tap target, in px */
    static constexpr int32_t MIN_TAPPABLE_H = 8;

    /**
     * @brief Widen every tappable row to the width of the container holding it
     * Makes a row clickable across the full menu width rather than only across the
     * width of its own text. Runs after on_created() so it sees the final row set.
     */
    static void stretch_rows_to_card(lv_obj_t* menu_card);

    /** @brief stretch_rows_to_card() for one container's direct children */
    static void stretch_rows_in(lv_obj_t* container);

    /**
     * @brief Keep the card inside the screen, going side-by-side if it must
     *
     * Stacked column groups are the default. If the stacked card would not fit the
     * backdrop (less a margin top and bottom), the group container flips to a row so
     * the groups sit side by side; if even that overflows, the card is capped and
     * made scrollable.
     */
    static void fit_card_to_screen(lv_obj_t* menu_card);

    /**
     * @brief Hide column groups left with no visible action, and lone headings
     *
     * A group whose actions were all hidden becomes a heading over nothing. And a
     * group heading only earns its space when a sibling group is showing too —
     * alone it just restates the card header.
     */
    static void tidy_column_groups(lv_obj_t* columns);

    /**
     * @brief Place the card against whichever anchor show_impl() was given
     */
    void position_card(lv_obj_t* menu_card);

  public:
    /**
     * @brief Geometry of position_card(), factored out so it can be tested
     *
     * Pure arithmetic in backdrop-local coordinates — no LVGL state — so the
     * placement rules can be exercised without building a screen. All four
     * previous implementations of this got their edge cases subtly different;
     * one function with one set of tests is the point of the exercise.
     *
     * @param card    Card size
     * @param anchor  Anchor rect (BelowAnchor) or a zero-size rect at the click
     *                point (ClickPoint), both backdrop-local
     * @param bounds  Backdrop size
     * @param margin  Smallest gap between card and backdrop edge
     * @param gap     Gap between card and anchor in BelowAnchor mode
     */
    static lv_point_t compute_card_pos(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                                       int32_t margin, int32_t gap, AnchorMode mode,
                                       AnchorAlign align);
};

/**
 * @brief Declare a context menu's concrete kind, for ContextMenu::active_as()
 *
 * One line in every concrete subclass body: `HELIX_CONTEXT_MENU_KIND(MyMenu)`,
 * where the argument is the class being declared. Emits no access specifier, so
 * it can sit anywhere in the body without moving the section it lands in. The
 * override may therefore be private — harmless, because every call goes through
 * ContextMenu's own public declaration, which is what active_as<>() holds.
 */
#define HELIX_CONTEXT_MENU_KIND(T)                                                                 \
    std::size_t kind_tag() const override {                                                        \
        return helix::type_tag<T>();                                                               \
    }

} // namespace helix::ui
