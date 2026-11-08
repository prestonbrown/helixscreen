# Edit Mode tile callout: design

Status: proposed 2026-11-08, awaiting Preston's answers to the open questions at the end. No code
yet. Ships on main (1.2). Deleted in the change that ships it (`docs/CLAUDE.md`).
Issue: prestonbrown/helixscreen#2025.

## Why

Selecting a tile in home Edit Mode draws two floating pills beside it: trash and, for widgets
that support it, configure. Where to put them has taken several rounds of placement rules
(`outside_pill_area`, `corner_pill_area`, `pills_go_outside`, `pill_spares_tiles`) plus a
300 ms tap lockout, and on a packed 480x272 page there is still often no good spot: the pill
lands on a corner of the tile, over a neighbour's edge, or off the screen edge, and its
position changes with what is around the tile. The issue asks for the launcher approach: the
tile's actions live in a menu anchored to the tile, drawn above the grid, so it cannot take a
tap meant for another tile and needs no placement rules.

## Goals

- One place for a tile's actions (Configure, Remove), anchored to the tile and unambiguous
  about which tile it belongs to, at every breakpoint.
- No gesture regressions: tap-to-select, press-and-move on the selected tile, hold-to-grab,
  edge resize, swipe between pages and drag across pages all behave as today.
- Delete the pill placement code and its tests rather than keep two models.
- Room in the same menu model for adding a widget or a stack at an empty cell.

## Non-goals

- Changing resize (edge bands stay), cross-page drag, the catalog, or the navbar's `+`,
  delete-page and Done buttons (#2023 moved delete-page to the navbar; it stays there).
- The removal Undo toast: unchanged, the callout's Remove goes through the same
  `remove_selected_widget()` and `offer_undo()`.

## Today, measured

Measured on the mock (`--test`, headless) with `ctl geom` and `ctl screenshot`, default
layout, with the `comments-app` worktree's build of 2026-11-07 (`2a96a27fa9`, on top of main
with #2023's navbar delete-page).

Grid: 6 x 4 cells at both sizes. The navbar is a column on the left.

| | 800x480 (Medium) | 480x272 (Micro) |
|---|---|---|
| Navbar width | 76 | 42 |
| Grid host (`carousel_host`) | x 76..800, 724 x 480 | x 42..480, 438 x 272 |
| 1x1 tile | 114 x 113 | 70 x 64 |
| Pill (`BTN_SIZE`) | 36 x 36 | 24 x 24 |
| Pill overhang past the tile corner | 9 | 6 |
| Navbar `+` / delete-page / Done | 76x52 / 76x52 / 68x52 | 42x48 / 42x48 / 37x36 |
| Drag threshold (`kDragThresholdPx`) | 12 | 12 |
| Existing context menu row (`button_height_sm`) | 40 | 24 |
| Existing picker header row (measured, fan stack picker) | 44 | 34 |

Tile rects in the default layout (x, y, w, h):

| Tile | 800x480 | 480x272 |
|---|---|---|
| printer_image | 90, 14, 220, 195 | 50, 8, 206, 107 |
| temperature | 322, 7, 114, 113 | 262, 4, 70, 64 |
| bed_temperature | 441, 7, 114, 113 | 334, 4, 70, 64 |
| fan_stack | 560, 7, 114, 113 | 262, 70, 70, 64 |
| notifications | 679, 7, 114, 113 | 406, 4, 70, 64 |
| ams | 322, 125, 233, 113 | 334, 70, 142, 64 |
| print_status | 83, 243, 472, 230 | 46, 136, 286, 132 |
| temp_graph | 560, 125, 233, 348 | 334, 136, 142, 132 |

Where the pills landed (`edit_trash_pill`, configure read off the screenshot):

| Selection | Size | Trash pill | Configure pill | What goes wrong |
|---|---|---|---|---|
| temperature | 800x480 | 409, -2, 36x36 | none | Overhangs 4 px onto bed_temperature and 2 px off the top |
| fan_stack | 800x480 | 647, -2 | ~551, -2 | On the tile's corners; fine |
| fan_stack | 480x272 | 314, 64, 24x24 | ~256, 64 | Both sit over the tile's top row, covering its "0%" readings; the configure pill straddles printer_image's right edge |
| notifications, page 1 | 480x272 | 458, -2, 24x24 | none | Runs 2 px off the right edge of the screen |
| notifications, page 2 (alone) | 480x272 | 380, 4 | none | Goes outside, left of the tile: the same tile gets a different pill spot depending on its neighbours |
| notifications, page 2 | 800x480 | 766, -2, 36x36 | none | Runs 2 px off the right edge; the red navbar delete-page trash and the grey tile trash both show |
| temp_graph | 480x272 | 458, 130 | ~328, 130 | Configure pill sits on the corner shared with print_status and ams |

Screenshots, in
`/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/685e4029-3b65-4e83-8acc-fb8b965cb144/scratchpad/plan2025/`:

- `m_temperature_selected.png`, `s_temperature_selected.png`: a 1x1 tile with only a trash pill.
- `m_fan_stack_selected.png`, `s_fan_stack_selected.png`: both pills; at 480x272 they cover the tile.
- `s_notifications_corner.png`: the top-right corner tile, pill clipped by the screen edge.
- `s_temp_graph_selected.png`: a 2x2 tile at the bottom right.
- `m_page2_two_trash_icons.png`, `s_page2_two_trash_icons.png`: page 2, navbar delete-page
  plus the tile's trash pill.
- `m_fan_stack_configure_menu.png`, `s_fan_stack_configure_menu.png`: the fan stack's own
  configure picker, an existing `ContextMenu` raised from the configure pill. At 480x272
  (240 x 176 card at 176, 6) it already covers the tile it configures.

### How input works today (the parts the callout touches)

- A press on an unselected tile selects it (brackets, edge handles, pills). It never arms.
- A press on the selected tile, or in its edge band, arms; travel past 12 px starts a drag, or
  a resize from the edge band. A release under 12 px does nothing.
- A hold (LONG_PRESSED) grabs: selects the tile under the finger if needed and starts the drag
  at once, lifting the widget to the top layer. Releasing in place resolves a drop that commits
  nothing and re-selects the widget in place.
- A hold on empty grid with nothing selected opens the widget catalog at that cell.
- The hold that enters Edit Mode is inert: it selects and nothing more.
- Pills ignore a press within `kPillLockoutMs` (300 ms) of their first frame, because the
  select tap draws the trash pill under the finger and a double tap would remove the tile.
- `ContextMenu` (`include/ui_context_menu.h`) already provides the dimmed full-screen
  backdrop (tap to dismiss), a card placed against an anchor (`BelowAnchor`, flipping above),
  row stretching, stacked column groups that go side by side when tall, and one active menu at
  a time. AMS, Spoolman, the plugin menus and every widget's configure picker use it, and it is
  in the ESP32 build.

## Options

All drawings are to scale from the measurements: 800x480 at 10 px per column and 20 px per
line, 480x272 at 6 px per column and 12 px per line. `*` is the selected tile, `#`/`=` the
callout, `:` the navbar edge. Placement is "below, else above, else right, else left, else
over the tile, clamped to the screen less the margin" (the proposed `AroundAnchor`, below).
Card sizes come from the existing tokens: Medium 180 x 138 (space_sm 7 padding and gaps, a
22 px title, a rule, two 40 px rows), Micro 120 x 84 (4 px padding and gaps, a 15 px title,
two 24 px rows). A tile with no Configure row is one row shorter (Medium 91, Micro 56).

### A. Callout card (a `ContextMenu` subclass)

A small card with the tile's name and one full-width row per action, anchored around the tile
over the shared dimmed backdrop. Opens on a tap on the selected tile or on a hold released in
place.

A1, 800x480, fan_stack: card at (527, 125), below the tile.

```
+--------------------------------------------------------------------------------+
|       : +--------------------+ +----------++----------+***********+-----------+|
|       : |printer_image       | |temperatur||bed_temp  |*fan_stack |notif      ||
|       : |                    | |          ||          |*          |           ||
|       : |                    | |          ||          |*          |           ||
|       : |                    | |          ||          |*          |           ||
|       : |                    | +----------++----------+***********+-----------+|
|       : |                    | +-------------------#=================#--------+|
|       : |                    | |ams                #Fan Stack        #        ||
|       : |                    | |                   #-----------------#        ||
|+------+ |                    | |                   #@ Configure      #        ||
||+     | +--------------------+ |                   #                 #        ||
|+------+                        +-------------------#X Remove         #        ||
|+------++-------------------------------------------#                 #        ||
||Done  ||print_status                               #=================#        ||
|+------+|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :|                                              ||                      ||
|       :+----------------------------------------------++----------------------+|
+--------------------------------------------------------------------------------+
```

A2, 800x480, temp_graph (2 x 3): no room below or above, so the card goes left of it, at
(375, 125).

```
+--------------------------------------------------------------------------------+
|       : +--------------------+ +----------++----------++----------+-----------+|
|       : |printer_image       | |temperatur||bed_temp  ||fan_stack |notif      ||
|       : |                    | |          ||          ||          |           ||
|       : |                    | |          ||          ||          |           ||
|       : |                    | |          ||          ||          |           ||
|       : |                    | +----------++----------++----------+-----------+|
|       : |                    | +----#=================#************************|
|       : |                    | |ams #Temperature Graph#*temp_graph            *|
|       : |                    | |    #-----------------#*                      *|
|+------+ |                    | |    #@ Configure      #*                      *|
||+     | +--------------------+ |    #                 #*                      *|
|+------+                        +----#X Remove         #*                      *|
|+------++----------------------------#                 #*                      *|
||Done  ||print_status                #=================#*                      *|
|+------+|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :|                                              |*                      *|
|       :+----------------------------------------------+************************|
+--------------------------------------------------------------------------------+
```

A3, 480x272, fan_stack: card at (237, 136), below.

```
+--------------------------------------------------------------------------------+
|       :+---------------------------------++-----------+-----------+-----------+|
|       :|printer_image                    ||temp       |bed        |notif      ||
|       :|                                 ||           |           |           ||
|       :|                                 ||           |           |           ||
|       :|                                 ||           |           |           ||
|       :|                                 |************+-----------------------+|
|       :|                                 |*fan_stack  |ams                    ||
|+-----+:|                                 |*           |                       ||
||+    |:|                                 |*           |                       ||
||     |:+---------------------------------+*           |                       ||
||     |:                                   *           |                       ||
|+-----++-------------------------------#===================#-------------------+|
|+-----+|print_status                   #Fan Stack          #_graph             ||
||Dn   ||                               #-------------------#                   ||
||     ||                               #@ Configure        #                   ||
|+-----+|                               #X Remove           #                   ||
|       |                               #                   #                   ||
|       |                               #                   #                   ||
|       |                               #===================#                   ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       +-----------------------------------------------+-----------------------+|
+--------------------------------------------------------------------------------+
```

A4, 480x272, notifications in the top-right corner: centred it would leave the screen, so it
clamps to x 354..474, still under the tile. No Configure row for this widget, but drawn at
the two-row height for the worst case.

```
+--------------------------------------------------------------------------------+
|       :+---------------------------------++-----------+-----------*************|
|       :|printer_image                    ||temp       |bed        *notif      *|
|       :|                                 ||           |           *           *|
|       :|                                 ||           |           *           *|
|       :|                                 ||           |           *           *|
|       :|                                 |+-----------+---#==================#+|
|       :|                                 ||fan_stack  |ams#Notifications     #||
|+-----+:|                                 ||           |   #------------------#||
||+    |:|                                 ||           |   #X Remove          #||
||     |:+---------------------------------+|           |   #                  #||
||     |:                                   |           |   #                  #||
|+-----++-----------------------------------------------+---#                  #+|
|+-----+|print_status                                   |tem#==================#||
||Dn   ||                                               |                       ||
||     ||                                               |                       ||
|+-----+|                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       +-----------------------------------------------+-----------------------+|
+--------------------------------------------------------------------------------+
```

A5, 480x272, print_status (bottom half): below has no room, above fits at (129, 50).

```
+--------------------------------------------------------------------------------+
|       :+---------------------------------++-----------+-----------+-----------+|
|       :|printer_image                    ||temp       |bed        |notif      ||
|       :|                                 ||           |           |           ||
|       :|                                 ||           |           |           ||
|       :|            #===================#||           |           |           ||
|       :|            #Print Status       #|+-----------+-----------------------+|
|       :|            #-------------------#||fan_stack  |ams                    ||
|+-----+:|            #@ Configure        #||           |                       ||
||+    |:|            #X Remove           #||           |                       ||
||     |:+------------#                   #+|           |                       ||
||     |:             #                   # |           |                       ||
|+-----+**************#===================#*************+-----------------------+|
|+-----+*print_status                                   |temp_graph             ||
||Dn   |*                                               |                       ||
||     |*                                               |                       ||
|+-----+*                                               |                       ||
|       *                                               |                       ||
|       *                                               |                       ||
|       *                                               |                       ||
|       *                                               |                       ||
|       *                                               |                       ||
|       *                                               |                       ||
|       ************************************************+-----------------------+|
+--------------------------------------------------------------------------------+
```

Every tile of the default layout gets a card that fits on screen without covering the tile at
both sizes. The only case that falls back to "over" is a tile that leaves no 84 px band (Micro)
above or below and no 120 px column beside it, which on a 6 x 4 grid means a near full-page
tile (#1931). There the card sits over the tile, which the backdrop makes safe.

- Pros: one placement function, already tested, extended by one mode. Rows are full-width
  targets: at Micro a Remove row is about 112 x 24 instead of a 24 x 24 circle. Text labels,
  so no icon-only guessing. The backdrop means no tap can reach another tile or the navbar's
  delete-page while it is open. Extends to more actions by adding rows (stacked groups
  already handle a tall card). D-pad friendly for #1871.
- Cons: one more tap than today for Remove and Configure (select, then open, then act). The
  backdrop dims the tile; a ring around the tile on the backdrop keeps it identifiable.

### B. Action strip

A horizontal strip of icon-plus-label buttons hung under or over the tile, same backdrop.

B1, 480x272, fan_stack: 96 x 40 at (249, 136).

```
+--------------------------------------------------------------------------------+
|       :+---------------------------------++-----------+-----------+-----------+|
|       :|printer_image                    ||temp       |bed        |notif      ||
|       :|                                 ||           |           |           ||
|       :|                                 ||           |           |           ||
|       :|                                 ||           |           |           ||
|       :|                                 |************+-----------------------+|
|       :|                                 |*fan_stack  |ams                    ||
|+-----+:|                                 |*           |                       ||
||+    |:|                                 |*           |                       ||
||     |:+---------------------------------+*           |                       ||
||     |:                                   *           |                       ||
|+-----++---------------------------------#===============#---------------------+|
|+-----+|print_status                     #  @     X      #mp_graph             ||
||Dn   ||                                 # Conf  Remove  #                     ||
||     ||                                 #===============#                     ||
|+-----+|                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       +-----------------------------------------------+-----------------------+|
+--------------------------------------------------------------------------------+
```

- Pros: smallest footprint (96 x 40 at Micro), stays close to the tile.
- Cons: labels at Micro are 10 px text under a 16 px icon in 44 px of width; "Configure"
  does not fit and gets abbreviated. Every extra action widens it, and at four actions it no
  longer fits beside a corner tile at Micro (4 x 44 + padding = 184 px). A new placement and
  layout component instead of reusing `ContextMenu`.

### C. Selection actions in the navbar

No floating chrome at all: while a tile is selected the navbar shows Configure and Remove
under `+`.

C1, 480x272: `+`, Configure, Remove, delete-page, Done in a 42 px column (4 x 48 + 36 = 228
of 272 px, with gaps).

```
+--------------------------------------------------------------------------------+
|+-----+:+---------------------------------++-----------+-----------+-----------+|
||+    |:|printer_image                    ||temp       |bed        |notif      ||
||     |:|                                 ||           |           |           ||
|+-----+:|                                 ||           |           |           ||
|+-----+:|                                 ||           |           |           ||
||@    |:|                                 |************+-----------------------+|
||     |:|                                 |*fan_stack  |ams                    ||
|+-----+:|                                 |*           |                       ||
|+-----+:|                                 |*           |                       ||
||X    |:+---------------------------------+*           |                       ||
||     |:                                   *           |                       ||
|+-----++-----------------------------------------------+-----------------------+|
|+-----+|print_status                                   |temp_graph             ||
||Pg   ||                                               |                       ||
||     ||                                               |                       ||
|+-----+|                                               |                       ||
|+-----+|                                               |                       ||
||Dn   ||                                               |                       ||
||     ||                                               |                       ||
|+-----+|                                               |                       ||
|       |                                               |                       ||
|       |                                               |                       ||
|       +-----------------------------------------------+-----------------------+|
+--------------------------------------------------------------------------------+
```

- Pros: no placement anywhere; no backdrop; no extra tap.
- Cons: the actions sit far from the tile, which is the "unclear which tile they belong to"
  complaint in a new place. The widget trash and page trash end up stacked in one column,
  which #2023 just untangled. At Micro the column is full, so there is no room for a third
  tile action. Portrait puts the navbar along the bottom with a different budget.

## Recommendation

**Option A at every breakpoint, replacing the pills entirely.** Keeping pills at large
breakpoints and a callout at small ones would keep both code paths, both test sets and two
models for users to learn, and the large-breakpoint pills are not trouble-free either (the
temperature tile's pill overhangs bed_temperature; the corner pill clips off the screen at 800x480
too). The placement rules, the pill lockout's draw-time plumbing and their tests go away.

### Interaction

| Gesture | Today | Proposed |
|---|---|---|
| Tap an unselected tile | Select, draw pills | Select (brackets and edge handles only) |
| Tap the selected tile (release under 12 px, press inside the tile rect) | Nothing | **Open the callout** |
| Press the selected tile and move past 12 px | Drag, or resize from the edge band | Same |
| Hold a tile, then move | Grab and drag | Same |
| Hold a tile, release in place (under 12 px total travel, drop commits nothing) | Re-select | Re-select, **then open the callout** |
| Hold on empty grid, nothing selected | Open the catalog at that cell | Same (see open question 3) |
| Tap empty grid | Deselect | Same |
| The hold that enters Edit Mode | Select only (inert) | Same (see open question 2) |
| Swipe on empty grid or from an unselected tile | Flip page | Same |

So a long press opens the menu from any state in one gesture, as on Android launchers, and the
same long press turns into a drag the moment the finger moves: the drag already started at
LONG_PRESSED, and the callout only opens at a release that travelled under the threshold and
committed nothing. There is no window where the menu is open while a drag is live, so the
callout needs no "dismiss when the finger moves" logic.

While the callout is open:

- The shared backdrop takes every press, including over the navbar. A tap anywhere outside the
  card closes it and does nothing else: the tile stays selected, no other tile is selected, no
  page flips, Done does not fire. One tap to close, one to act, as in every other menu.
- The tile is outlined on the backdrop (a non-clickable ring at the tile's rect, accent border)
  so the dim does not hide which tile the menu is for.
- `grid_input_acts()` is false, as while the catalog is open, so remote-control clicks and
  stray events take no grid action.
- Rows: **Configure** (only when `PanelWidget::has_edit_configure()`), **Remove**. Configure
  closes the callout, then runs `configure_selected_widget()`, whose widget opens its own
  picker (one `ContextMenu` at a time, so the callout must be hidden first, which
  `dispatch_action()` already does). Remove closes it and runs `remove_selected_widget()`,
  which shows the Undo toast as today.
- The rows keep a 300 ms tap lockout from the card's first frame. The card is placed off the
  tile in every measured case, so a triple tap's third press hits the backdrop, but the "over"
  fallback puts rows under the finger, and the lockout costs nothing.
- It closes, without acting, on: `exit()`, `switch_page()`, `forget_scope()`, a selection
  change, `end_gesture_uncommitted()`, a deferred rebuild, the catalog opening, panel
  deactivation, and XML hot reload. An action whose tile is no longer the selection (by config
  id) does nothing.

Small screens: at Micro the card is 120 x 84 with two rows, and every tile in the default
layout keeps a band above or below for it (A3 to A5). The 24 px row height is the same height
as today's pill but five times wider. If Preston wants taller rows at Micro, a callout row token of 32 px at Micro
instead of `#button_height_sm` makes the card 100 px tall, which still fits above or below every
default tile.

## Interfaces

### `ContextMenu`: one new anchor mode (`include/ui_context_menu.h`)

```cpp
enum class AnchorMode {
    ClickPoint,
    BelowAnchor,
    /// Below the anchor, else above, else right of it, else left of it: the first
    /// side where the whole card fits inside the bounds less the margin. Below and
    /// above centre the card on the anchor; right and left top-align it. When no
    /// side fits the card is centred on the anchor, then clamped like the others.
    AroundAnchor,
};

/// The AroundAnchor counterpart to show_below_widget().
bool show_around_widget(lv_obj_t* parent, int item_index, lv_obj_t* anchor);

/// Which side compute_card_pos() put an AroundAnchor card on; Over when none fit.
enum class AnchorSide { Below, Above, Right, Left, Over };

/// compute_card_pos() also reports the side, for the callout's ring and for tests.
static lv_point_t compute_card_pos(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                                   int32_t margin, int32_t gap, AnchorMode mode,
                                   AnchorAlign align, AnchorSide* side = nullptr);
```

`compute_card_pos` stays one pure function with the existing clamps applied last; the new mode
only chooses the starting point.

### Tap lockout, lifted out of `GridEditMode` (`include/tap_lockout.h`)

The pill lockout's logic, unchanged, as a value type the callout owns. `GridEditMode` loses
`kPillLockoutMs`, `pill_release_acts()`, `pills_drawn_tick_`, `pills_shown_`,
`pill_press_tick_` and `pill_press_latched_`.

```cpp
namespace helix::ui {
class TapLockout {
  public:
    static constexpr uint32_t kDefaultMs = 300;
    explicit TapLockout(uint32_t ms = kDefaultMs);
    /// The target was created at @p tick; the lockout runs from its first frame.
    void reset(uint32_t tick);
    /// LV_EVENT_DRAW_POST: the first call fixes the start of the lockout.
    void on_drawn(uint32_t tick);
    void on_press(uint32_t tick);
    void on_press_lost();
    /// Whether a release acts: over the target, and its press (or @p now, for a
    /// click with no press behind it) at least the lockout after the first frame.
    /// Consumes the press.
    [[nodiscard]] bool release_acts(bool released_over_target, uint32_t now);
};
}
```

### The callout (`include/ui_tile_edit_callout.h`, `src/ui/home/ui_tile_edit_callout.cpp`, `ui_xml/tile_edit_callout.xml`)

```cpp
namespace helix::ui {

enum class TileCalloutAction : int { Configure = 0, Remove = 1 };

/// What the callout offers for one tile. Pure, so each row's rule is testable
/// without a screen.
struct TileCalloutRows {
    bool configure = false;
    bool remove = true;
};
[[nodiscard]] TileCalloutRows tile_callout_rows(bool has_edit_configure);

class TileEditCallout : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(TileEditCallout)
  public:
    /// Raise the callout for @p tile, titled @p title, around it on @p screen.
    /// @p on_action gets the chosen action; a backdrop tap sends nothing.
    bool show_for(lv_obj_t* screen, lv_obj_t* tile, std::string title, TileCalloutRows rows,
                  std::function<void(TileCalloutAction)> on_action);
    static void register_callbacks();   // tile_callout_configure_cb, tile_callout_remove_cb

  protected:
    const char* xml_component_name() const override { return "tile_edit_callout"; }
    void on_created(lv_obj_t* backdrop) override;  // title, row visibility, tile ring
    CardWidth card_width() const override;          // fit_content, min 112, pct 40
};
}
```

XML: `context_menu_backdrop` + `context_menu_card` with `hide_close="true"` and the tile's
title, then two `ui_button` rows (`variant="ghost"`, `height="#button_height_sm"`, icons
`settings` and `delete`), named `tile_callout_configure` and `tile_callout_remove` so `ctl click`
reaches them. The ring is a named, non-clickable `lv_obj` in the backdrop that C++ positions
at the tile rect (`DECLARATIVE_OK: measured anchor`).

Translations: "Configure" and "Remove" strings with `translation_tag`, plus the widget's
existing display name for the title.

### `GridEditMode` (`include/grid_edit_mode.h`)

Removed: `remove_btn_`, `configure_btn_`, `PillCorner`, `pills_go_outside()`,
`outside_pill_area()`, `corner_pill_area()`, `kPillLockoutMs`, `pill_release_acts()`, the
pill state fields, and in the .cpp `pill_spares_tiles()`, `make_chrome_pill` and the
`BTN_SIZE` table.

Added:

```cpp
/// The facts of one gesture's release that decide whether it opens the callout.
struct CalloutReleaseFacts {
    bool press_on_selected = false;  ///< the press landed inside the tile selected before it
    bool resized = false;            ///< the gesture ran a resize
    bool committed = false;          ///< a drop or resize changed the layout
    int max_travel_px = 0;           ///< farthest the pointer got from the press origin
    bool inert = false;              ///< gesture_inert_
    bool input_acts = false;         ///< grid_input_acts() at the release
};

/// True for a tap on the selected tile and for a hold released in place: a press inside
/// the selected tile, no resize, nothing committed, at most kDragThresholdPx of travel,
/// not inert, while grid input acts.
[[nodiscard]] static bool release_opens_callout(const CalloutReleaseFacts& f);

/// Whether the tile callout is on screen.
bool is_callout_open() const;

/// Raise the callout for the selected tile. A no-op without a selection or while
/// grid input does not act.
void open_tile_callout();

/// Close it without acting. Safe when it is not open.
void close_tile_callout();
```

State: `std::unique_ptr<helix::ui::TileEditCallout> callout_`, `std::string callout_widget_id_`
(the config id the callout was raised for), `int gesture_max_travel_px_` (reset in
`clear_gesture_state()`, raised in `handle_pressing()` and `handle_drag_move()`).
`grid_input_acts()` becomes `active_ && !catalog_open_ && !is_callout_open()`.

Flow: `handle_released()` fills `CalloutReleaseFacts` before the end handlers clear state.
When `release_opens_callout()` is true it opens the callout on the next tick
(`helix::ui::run_next_tick`, under the session's `AsyncLifetimeGuard`), after
`reselect_in_place()` has put a held widget back in its cell, anchored to the selected object
it finds by `callout_widget_id_`. The action callback re-checks that the selection's id still
equals `callout_widget_id_` before calling `configure_selected_widget()` or
`remove_selected_widget()`.

Firmware: the new .cpp goes in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`
next to `ui_context_menu.cpp`. gnu++17 rules apply. Net flash should drop: `ContextMenu` is
already linked and the pill placement code goes.

### Docs to update in the shipping change

- `docs/devel/HOME_EDIT_MODE.md` § "Selection, grab and click rules" (the pill lockout bullet
  becomes the callout's rules), § "Event routing and the shield" (configure and remove
  buttons are no longer shield siblings), § "Undoing a removal" (triggered from the callout).
- `docs/devel/HELIXCTL.md` § "Home-grid Edit Mode": tap the selected tile, then
  `ctl click tile_callout_remove`.

## Tests

Tags: `[grid_edit]`, `[edit-callout]`, `[context-menu][position]`. Each new test goes red
once by breaking the line it pins.

| # | Test | File | Kind | Pins |
|---|---|---|---|---|
| 1 | AroundAnchor: below when it fits (Medium fan_stack: card 180x138, anchor 560,7..673,119, bounds 800x480, margin 10, gap 5 -> 527,125, Below) | `test_context_menu_position.cpp` | pure | below first, centring |
| 2 | AroundAnchor: above when below overflows (Micro print_status -> 129,50, Above) | same | pure | flip order |
| 3 | AroundAnchor: right, then left, when neither vertical side fits (Medium temp_graph -> 375,125, Left; mirrored layout -> Right) | same | pure | side order, top-align |
| 4 | AroundAnchor: centred on the anchor then clamped when no side fits (full-screen anchor) -> Over, card inside bounds | same | pure | fallback |
| 5 | AroundAnchor: corner tile clamps horizontally but stays Below (Micro notifications -> 354,70) | same | pure | clamp after side choice |
| 6 | `release_opens_callout`: true for a tap on the selected tile and for a hold released in place; false for each single fact flipped (press not on selected, resized, committed, travel 13 px, inert, input not acting) | `test_grid_edit_mode.cpp` | pure | every condition |
| 7 | `tile_callout_rows`: Configure only with `has_edit_configure` | `test_tile_edit_callout.cpp` (new) | pure | row rule |
| 8 | `TapLockout`: double tap (press 240 ms, lift 310 ms) refused; press after 300 ms acts; press before first frame refused; press lost clears the latch; a click with no press is timed at the click | `test_tap_lockout.cpp` (new) | pure | migrated pill lockout cases |
| 9 | Tap a tile, tap it again: callout open, anchored around it, no object named `edit_trash_pill` anywhere | `test_home_edit_page_swipe.cpp` | fixture | open path, pills gone |
| 10 | Hold a tile, release in place: callout opens after the widget is re-seated | same | fixture | hold path, next-tick ordering |
| 11 | Hold a tile, move 60 px, release: drag commits, callout never opens | same | fixture | drag unchanged |
| 12 | Press in the selected tile's edge band and drag: resize commits, no callout | same | fixture | resize unchanged |
| 13 | The entering hold does not open the callout | same | fixture | inert |
| 14 | Remove row removes the tile and offers Undo; Undo restores it (existing Undo cases driven through the callout) | `test_grid_edit_mode.cpp` | fixture | Remove wiring |
| 15 | Configure row closes the callout and the widget's picker opens as the active menu | `test_home_edit_page_swipe.cpp` | fixture | Configure wiring, one menu at a time |
| 16 | Backdrop tap closes, tile stays selected, the tile under the tap is not selected, the page does not flip | same | fixture | dismiss-only |
| 17 | Closes without acting on exit, page switch, forget_scope, catalog open; a late action after exit does nothing | same | fixture | lifetime |
| 18 | Action after the selection changed under it does nothing (id check) | same | fixture | stale target |
| 19 | Micro 480x272 default layout: for every tile, the card lies inside the screen and does not overlap the tile | `test_tile_edit_callout.cpp` | fixture | small-screen fit |

Deleted: the two "selection pills" placement cases in `test_grid_edit_mode.cpp`, the pill
cases in `test_grid_edit_chrome_contrast.cpp` (contrast moves to the theme's ghost button,
already covered), the pill lockout cases (moved to #8), the pill parent assertions in the
page-swipe test (replaced by #17's page-switch close), and the `edit_trash_pill` helper in
`test_home_edit_page_swipe.cpp` (replaced by a callout helper).

Verification at the finish, per CLAUDE.md: `make t F='[grid_edit],[edit-callout],[context-menu]'`
while building, one review, screenshots of A1, A3, A4 and A5 at their sizes, one
`make land-gate`, then `mutate-diff` after landing.

## Open questions for Preston

1. **First tap or second tap?** The plan opens the callout on a tap on the *selected* tile
   (so two taps from nothing), keeping the first tap for selection and press-and-move. Opening
   on the first tap would put a backdrop over the grid after every select and add a dismiss
   tap before every move. OK with two taps, plus hold-and-release as the one-gesture way?
2. **The hold that enters Edit Mode**: today it selects and stops. Should releasing it in
   place also open the callout (launcher behaviour), or stay select-only?
3. **What is a "stack"?** Nothing in the code is called one besides the fan and temperature
   stack widgets. If it means a new tile that holds several widgets, the empty-cell hold would
   open a second callout ("Add widget here", "Add stack here") with the same class and
   placement. Until a second add action exists, the plan keeps the hold on empty grid opening
   the catalog directly, since a one-row menu is only an extra tap.
4. **More tile actions now?** Candidates the callout makes cheap: "Move to page N" (cross-page
   moves are drag-to-edge only today, fiddly at 480x272) and size presets. In scope for this
   change, or later?
5. **Backdrop**: keep the shared 50% dim with a ring around the tile, or a lighter dim for this
   menu?
6. **Row height at Micro**: `#button_height_sm` (24 px, matches every other context menu) or a
   callout-specific 32 px row (card 100 px tall)?
