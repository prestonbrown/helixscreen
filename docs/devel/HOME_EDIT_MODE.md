# Home Edit Mode (Developer Guide)

How the home panel's edit mode works: the edit session, how a press becomes a selection, a
grab or a swipe, how a gesture ends, cross-page drag, drop resolution, adding a page with the
next-page slot's +, the widget catalog, and the page lifecycle. Read it before changing
`GridEditMode` or the edit-mode half of `HomePanel`.

**User-facing doc**: [Home Panel - Edit Mode](../user/guide/home-panel.md#edit-mode)

**Related**: [architecture/09-home-widgets.md](architecture/09-home-widgets.md) (widgets,
population, rebuilds), [LAYOUT_SYSTEM.md](LAYOUT_SYSTEM.md) (grid sizing, spans, snap steps),
[HELIXCTL.md](HELIXCTL.md) (driving edit mode live).

---

## Overview

Edit mode is a session over one page of the home carousel at a time. Six pieces share it, each
with one job:

| Piece | Owns |
|-------|------|
| `helix::GridEditMode` | The session: the scoped page container, the selection and its chrome, the event shield and lattice, and every gesture from press to end |
| `HomePanel` | The carousel and its pages, the grid event handlers on `carousel_host`, the swipe policy, and every change to the page set's objects |
| `helix::PanelWidgetConfig` | Pages and placement: what a page holds, the page cap, adding and removing pages, moving entries |
| `helix::cross_page_step()` | The cross-page rules of a drag, as a pure function |
| `helix::resolve_drop()` | What a released drag does, as a pure function |
| `helix::page_set_landing()` | Where the carousel and the session land after a change to the page set, as a pure function |

Rules that hold throughout:

1. **No structural change during a gesture.** Carousel tiles and page containers are created and
   destroyed only when no gesture is live, and never inside input dispatch: page-set changes run
   on the next tick.
2. **One writer per piece of state.** The carousel owns its tiles' input flags, which tiles can be
   reached, and its swipe; HomePanel states a policy. The widget config owns the page cap and page
   lifecycle.
3. **The session follows the gesture; the carousel follows the session.** One HomePanel function
   shows an edit page and re-scopes the session to it.
4. **Screen-space rules use the settled page frame**, never the live position of a page that may
   be sliding.
5. **Gesture end is event-driven.** RELEASED on the shield commits a gesture; PRESS_LOST and
   INDEV_RESET cancel it. All three reach the grid handlers by bubbling. Nothing polls.
6. **Decisions are pure functions over plain data.** LVGL code gathers the inputs and applies the
   result.

## Key files

| File | Role |
|------|------|
| `include/grid_edit_mode.h`, `src/ui/home/grid_edit_mode.cpp` | `GridEditMode`: session, selection, grab rules, drag and resize, shield, gesture end, drop commit, pruning, the catalog session |
| `include/grid_edit_cross_page.h`, `src/ui/home/grid_edit_cross_page.cpp` | `cross_page_step()` and the cross-page constants |
| `include/grid_edit_drop.h`, `src/ui/home/grid_edit_drop.cpp` | `resolve_drop()`: move, create a page, return to the origin page, or snap back |
| `include/grid_edit_page_set.h`, `src/ui/home/grid_edit_page_set.cpp` | `page_set_landing()` and `page_set_focus()`: where a page-set rebuild comes up, and the page it and the session end on |
| `include/ui_panel_home.h`, `src/ui/home/ui_panel_home.cpp` | `HomePanel`: carousel build, grid event handlers, `show_edit_page()`, swipe policy, page subjects, adding and deleting pages |
| `include/ui_carousel.h`, `src/ui/ui_carousel.cpp` | `helix::ui::carousel_set_swipe`, `carousel_set_bubble_events`, `carousel_goto_tile`, `carousel_set_trailing_tiles_reachable` |
| `include/panel_widget_config.h`, `src/system/panel_widget_config.cpp` | Page operations: `page_entries`, `place_entry`, `page_is_populated`, `can_add_page`, `add_page`, `remove_page` |
| `ui_xml/home_panel.xml` | `carousel_host` with the grid event callbacks, and the page badge |
| `ui_xml/components/home_page_container.xml` | A page's grid container |
| `ui_xml/components/home_next_page_slot.xml` | The next-page slot past the last page: the drag's drop target and the + button that adds a page |
| `include/ui_next_tick.h` | `helix::ui::run_next_tick`, how structural changes leave input dispatch |
| `src/ui/ui_utils.cpp` | `helix::ui::reset_input_within`, the scoped input reset |
| `tests/unit/test_home_edit_page_swipe.cpp` | `EditHomeFixture`: edit mode through the real carousel build, real population and real indev reads |
| `tests/test_helpers/scoped_pointer_indev.h` | `ScopedPointerIndev`, the synthetic pointer the tests drive |
| `tests/test_helpers/scoped_widget_factory.h` | `ScopedWidgetFactory`, a registry factory swapped for a scope |

## The session

### Entering

A hold on the home grid enters edit mode (`src/ui/home/ui_panel_home.cpp#on_home_grid_long_press`).
The handler does nothing when `should_suppress_edit_mode()` refuses (the Touch & Input kill
switch, the lock screen, a scroll in progress, a drag surface under the finger), when the
carousel is not resting on the page the handlers act on (`carousel_on_scoped_page()`), or when
the finger drifted from where it landed (`finger_drifted_since_press()`). Otherwise it:

1. resets the pointer, so the widget under the finger never gets its click;
2. calls `GridEditMode::enter()` on the active page's container;
3. disarms widget clicks on every other page, since a swipe can settle on any of them;
4. states the swipe policy for a session with no gesture, the policy outside edit mode: the pages
   swipe by their count and the next-page slot stays out of reach;
5. selects the widget under the finger with `handle_click()`.

A drag surface is a control the user drags for a value or a position, so a hold on it is the
start of a drag, not a request to edit. `helix::ui::is_drag_surface_hit()`
(`src/ui/ui_drag_surface.cpp#is_drag_surface_hit`) walks from the press target up to the
widget and answers true for an `lv_arc`, an `lv_slider`, or any object a widget marked with
`helix::ui::mark_drag_surface()` (`include/ui_drag_surface.h`). Marking sets
`kDragSurfaceFlag` (`LV_OBJ_FLAG_USER_1`) and the input flags a drag needs: clickable, not
scrollable, no scroll chaining (so a horizontal drag never adopts the page carousel), press
lock and no gesture bubbling. A widget opts in by marking the object from `attach()`; the
toolhead plate is the worked example (`src/ui/panel_widgets/toolhead_widget.cpp#attach`). To
make a surface inert, clear its `clickable` flag: the press then lands on the tile, and a hold
there enters edit mode as anywhere else. The fan control overlay asks the same question
(`src/ui/controls/ui_fan_control_overlay.cpp#press_consumed_by_value_widget`).

`enter()` sets `home_edit_mode` to 1, marks the entering hold inert (`gesture_inert_`, see below)
when it runs inside a pointer read, and runs `attach_to_current_page()`: it disarms the
container's widget clicks first, then creates the shield and lattice, then syncs config cell
positions from the laid-out widgets.

### Switching pages

`GridEditMode::switch_page(container, page)` re-scopes a live session, and decides itself what
travels, so no caller can choose wrong (`src/ui/home/grid_edit_mode.cpp#switch_page`):

- The scope already held is a no-op.
- While the widget catalog is open the session keeps its page: the catalog places onto the page
  it was opened from.
- A resize snap animation in flight stops, and the resized widget is laid out at the cell its
  resize committed, in place: a grid cell write that creates and deletes nothing, so no page is
  rebuilt and the shield travels as below.
- A live drag is carried: the dragged widget, the shield and its lattice, and the selection chrome
  move to the new container, the widget keeps its screen position under the pointer, and the snap
  target is recomputed on the landing page for the widget's last position, so a release before
  the next move drops there.
- Anything else is dropped, including a press that is armed but not yet dragging, whose widget is
  still laid out in its own page's grid: the selection, the chrome and the gesture state go, only
  the shield moves, and the gesture ownership change is announced, which states the swipe policy
  again.

HomePanel reaches it only through `rescope_edit_page()`: from the page subject's observer on every
change of the carousel's page to a config page, a settled swipe and an arrow button's goto alike
(`src/ui/home/ui_panel_home.cpp#on_page_changed`), and from
`show_edit_page()`, the one way edit mode shows a page, used for drag flips, a drag returned to
its origin, the catalog closing, and the focus of a rebuilt page set
(`src/ui/home/ui_panel_home.cpp#show_edit_page`). `edit_container(page)` resolves config pages and,
one past the last, the next-page slot.

### Leaving

`HomePanel::exit_grid_edit_mode()` is the one exit, used by the navbar Done button, deactivation
and page deletion (`src/ui/home/ui_panel_home.cpp#exit_grid_edit_mode`). A live gesture ends
uncommitted first. `GridEditMode::exit()` then closes the widget catalog the session opened,
clears the gesture state, cancels a resize snap animation, forgets its pointers into the
container, sets `home_edit_mode` to 0, saves the config, and schedules the rebuild that restores
the widgets. The swipe policy is stated again with no gesture: `Auto`, with the next-page slot out
of reach (see [The next-page slot](#the-next-page-slot)).

`HomePanel::on_deactivating()` leaves edit mode on every deactivation but one. Pushing the widget
catalog deactivates the panel, and the session outlives that push, since the catalog places into
it. A hot-reload rebuild (`DeactivateReason::Rebuild`) ends the session whether the catalog is
open or not: `PanelBase::rebuild()` deactivates the panel before it condemns the widget tree the
session points into (`src/ui/ui_panel_base.cpp#rebuild`). The catalog goes with the session, so
a hot reload with the catalog open closes it.

## Selection, grab and click rules

A press in a session becomes a selection, a grab or a swipe. The rules keep a packed grid
swipeable: a swipe that starts on a widget must still flip the page.

- **Press-and-move grabs only a widget selected before the press.** The gesture's first PRESSING
  cycle, which LVGL dispatches in the same read as the PRESSED, decides the grab latch from the
  selection standing at the press. A press on the selected widget, or inside its edge grab band,
  arms (`press_armed_`), and travel past `kDragThresholdPx` starts a drag, or a resize when the
  press landed on a resizable widget's edge. Any other press selects the widget under the finger
  on that first cycle, and no later cycle of the gesture selects again: a finger moving on is a
  swipe, never a new selection.
- **A hold grabs.** LONG_PRESSED in a session (`src/ui/home/grid_edit_mode.cpp#handle_long_press`)
  grabs the selected widget, or selects the widget under the finger and grabs it. A hold on empty
  grid with nothing selected opens the widget catalog at that cell. A swipe never gets here: once
  a scroll owns the gesture, LVGL sends no LONG_PRESSED.
- **An inert gesture takes no further grid action** (`gesture_inert_`): its pressing cycles
  neither arm nor select, and its holds neither grab nor open the catalog. Two gestures are
  inert. The hold that enters edit mode has made its one selection, and stays inert however long
  it lasts: the entry's `lv_indev_reset()` lets LVGL send LONG_PRESSED a second time, and that
  hold must not grab. A press that lands while a resize snap animation runs finishes the snap
  (`finish_resize_snap()`), whose rebuild replaces the objects under it on the next tick.
- **While the widget catalog is open, grid input takes no action** (see
  [The widget catalog](#the-widget-catalog)).
- **A click is a release that lands where its press did.** LVGL sends CLICKED after any release no
  scroll took, a drag's included. `on_home_grid_clicked` selects under the pointer only when
  `finger_drifted_since_press()` is false (`src/ui/home/ui_panel_home.cpp#on_home_grid_clicked`): a drag
  or a resize settles the selection at its own end, so a drop the grid rejects keeps the dragged
  widget selected. `lv_indev_get_press_moved()` is not that test: LVGL sets it only when a single
  read moves past the scroll limit, which a slow drag never does.

The latch, from `src/ui/home/grid_edit_mode.cpp#handle_pressing`:

```cpp
    if (!press_armed_) {
        if (gesture_press_seen_ && !gesture_can_arm_) {
            return;
        }
        bool owns_selected = false;
        if (selected_) {
            lv_area_t sel_area;
            lv_obj_get_coords(selected_, &sel_area);
            owns_selected = press_owns_widget(pt, sel_area);
        }
        if (!gesture_press_seen_) {
            gesture_press_seen_ = true;
            gesture_can_arm_ = owns_selected;
            if (!gesture_can_arm_) {
                handle_click(e); // select what is under the finger (or deselect)
                return;
            }
        }
        if (!owns_selected) {
            return;
        }
        press_armed_ = true;
        press_origin_ = pt;
        notify_gesture_ownership();
        return;
    }
```

`clear_gesture_state()` resets the grab latch, the inert mark, the armed, dragging and resizing
flags with the resize edge, the cross-page state and flip timers, and the drag's origin cell, span
and offsets. It touches no object beyond cancelling the flip timers, so it is safe inside input
dispatch. Three values are left to their own writers: `press_origin_`, set when a press arms or a
hold grabs; `drag_step_tick_`, set when a drag starts; and `snap_preview_col_` and
`snap_preview_row_`, reset with the snap preview. It runs on every genuine PRESSED
(`begin_press`), every release the grid handlers act on (`handle_released`), every abort (through
`tear_down_gesture()`), `exit()`, and a `switch_page()` that drops the gesture. The reset on
PRESSED matters: the grid handlers do not act on a swipe's RELEASED, because
`should_suppress_edit_mode()` refuses while a scroll owns the input, so the next press is the
first moment that gesture's latch can clear.

## Gesture ownership and the swipe policy

`GridEditMode::owns_gesture()` is true while an armed press, a drag or a resize holds the pointer.
Every change that can flip it, and the widget catalog opening or closing, calls
`notify_gesture_ownership()`, which reaches HomePanel through the `GestureOwnershipCallback` it
installs at construction (`src/ui/home/ui_panel_home.cpp#"set_gesture_ownership_callback"`) and states
the swipe policy (`src/ui/home/ui_panel_home.cpp#apply_edit_swipe_policy`):

```cpp
void HomePanel::apply_edit_swipe_policy() {
    if (!carousel_) {
        return;
    }
    using helix::ui::CarouselSwipe;
    // A gesture or the open widget catalog holds the page; otherwise the pages
    // swipe by their count, in edit mode as out of it.
    const bool edit_holds_page =
        grid_edit_mode_.owns_gesture() || grid_edit_mode_.is_catalog_open();
    helix::ui::carousel_set_swipe(carousel_,
                                  edit_holds_page ? CarouselSwipe::Disabled : CarouselSwipe::Auto);
    // The next-page slot carries the + that adds a page and stays a drag's drop
    // target, so its tile is within reach whenever the slot exists. A drop that
    // creates a page leaves the session scoped to it, and the carousel resting
    // on it, until the page-set rebuild on the next tick.
    helix::ui::carousel_set_trailing_tiles_reachable(carousel_, next_page_container_ != nullptr);
}
```

| State | Policy | Why |
|-------|--------|-----|
| Outside edit mode | `Auto` | Swipe when there is more than one page, or a next-page slot to swipe to |
| Edit mode, no gesture owns the pointer | `Auto` | As outside edit mode: the slot's tile is somewhere to swipe to even on a single-page home, since its + adds a page |
| An edit gesture owns the pointer | `Disabled` | Otherwise the carousel adopts the drag as a scroll |
| Edit mode, the widget catalog is open | `Disabled` | The session holds its page for the catalog |

The carousel owns the flags. `helix::ui::carousel_set_swipe()` stores the policy and writes the
scroll container's `LV_OBJ_FLAG_SCROLLABLE` and scroll direction from it, and the carousel applies
the stored policy again at every page-count change (`src/ui/ui_carousel.cpp#apply_input_flags`),
where a swipe counts the reachable tiles: the pages plus the slot's tile while the slot exists, so
a single-page home with a slot still swipes over to it. It writes input flags only, so it is safe
inside input dispatch. The lock has to engage on the arming cycle, before any movement, because
LVGL adopts a scroll at the first movement past the scroll limit. The same function states whether
the next-page slot can be reached, through `helix::ui::carousel_set_trailing_tiles_reachable()`:
within reach whenever the slot exists, since the slot is both a drag's drop target and the + a tap
adds a page with (see [The next-page slot](#the-next-page-slot)). Reach is derived there, from
whether the build created a slot, and never set at a call site.

`build_carousel()` states the policy for the carousel it creates, so a carousel rebuilt under a
live session holds the session's policy from the start, with the slot in reach when there is one.
The `show_edit_page()` that follows re-scopes the session with no drag to carry, so `switch_page()`
announces gesture ownership and the policy is stated again.

Focus never moves the page. LVGL scrolls every scrollable ancestor of an object that gains focus
to show it, and focus moves on by itself when the focused object is hidden or deleted: leaving
edit mode hides the navbar's Done, which the tap on it focused, and the next control in the
default input group can sit on another page. So the carousel keeps its page itself: an animated
scroll of its scroll container that no goto started and no pointer swipe drives is aimed at the
current page (`src/ui/ui_carousel.cpp#carousel_scroll_begin_cb`). That covers everything the
carousel holds, whenever it was created: the pages' widgets, the next-page slot, and whatever
population and rebuilds add. Scrollables inside a page still bring a focused control into view.

## Event routing and the shield

The grid handlers are XML callbacks on `carousel_host` (`ui_xml/home_panel.xml#carousel_host`):

```xml
    <lv_obj name="carousel_host" width="100%" height="100%" style_bg_opa="0" style_border_width="0" style_pad_all="0">
      <event_cb trigger="pressed" callback="on_home_grid_pressed"/>
      <event_cb trigger="long_pressed" callback="on_home_grid_long_press"/>
      <event_cb trigger="clicked" callback="on_home_grid_clicked"/>
      <event_cb trigger="pressing" callback="on_home_grid_pressing"/>
      <event_cb trigger="released" callback="on_home_grid_released"/>
      <event_cb trigger="press_lost" callback="on_home_grid_press_cancelled"/>
      <event_cb trigger="indev_reset" callback="on_home_grid_press_cancelled"/>
    </lv_obj>
```

Page events reach them by bubbling: shield -> page container -> tile -> scroll container ->
carousel -> `carousel_host`. On the next-page slot, the slot's own view sits between the page
container and the tile. Each link is owned in one place:

- Page containers bubble by their XML component (`ui_xml/components/home_page_container.xml`),
  and `populate_page()` flags every descendant of the container once its widgets are placed,
  widget roots included (`src/ui/home/ui_panel_home.cpp#populate_page`).
- The next-page slot's view bubbles by its XML component
  (`ui_xml/components/home_next_page_slot.xml`).
- The carousel, its scroll container and its tiles bubble because `build_carousel()` calls
  `helix::ui::carousel_set_bubble_events(carousel_, true)`
  (`src/ui/ui_carousel.cpp#carousel_set_bubble_events`). It is a carousel property, so the
  page-count changes that rewrite the tiles' input flags keep it.

Which handlers act, and when:

- `on_home_grid_long_press`, `on_home_grid_pressing` and `on_home_grid_clicked` act on what a
  press lands on, and only while `carousel_on_scoped_page()`: the carousel's page is the edit
  session's page in edit mode, the active page outside it. A slide never reads false, because
  the carousel's page is the one a goto is heading to or a swipe began from, and the handlers
  hit-test live coordinates. It reads false only while the carousel's page is one the session
  does not follow: a page the arrow buttons paged the carousel to while the widget catalog holds
  the session, where the scoped page's widgets are off screen. Long press and pressing also stop
  while `should_suppress_edit_mode()` refuses.
- `on_home_grid_pressed` records where every press lands and begins a press in a session.
- `on_home_grid_released` ends a gesture unless `should_suppress_edit_mode()` refuses, as it does
  while a scroll owns the input.
- `on_home_grid_press_cancelled` is ungated (`src/ui/home/ui_panel_home.cpp#on_home_grid_press_cancelled`):

```cpp
void HomePanel::on_home_grid_press_cancelled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_press_cancelled");
    // Ungated, unlike the handlers that classify a press: a gesture whose press
    // LVGL took away ends wherever the carousel sits and whatever suppresses
    // new input.
    get_global_home_panel().grid_edit_mode_.handle_press_cancelled(e);
    LVGL_SAFE_EVENT_CB_END();
}
```

**The shield** is a full-grid, clickable, bubbling child of the scoped container, created by
`GridEditMode::ensure_shield()`. As the topmost full-grid child it is the press target of every
edit gesture, so no widget underneath is ever pressed. Its children are the lattice dots and the
delete-page button. The selection's configure and remove buttons are its siblings, created after
it, which puts them above it. The shield carries no callback: a shield that outlives the session,
because the rebuild that would delete it never runs once the session's lifetime token expired,
holds nothing that calls into the session.

**The shield persists for the session.** LVGL glues a press to its target, and deleting that
target resets the pointer: `obj_delete_core` removes the object's callbacks, then sends it
INDEV_RESET (`lib/lvgl/src/core/lv_obj_tree.c#"/*Reset all input devices if the object to delete is used*/"`), which bubbles from the deleted
object like any other event (`event_send_core`: `lib/lvgl/src/core/lv_obj_event.c#"if(parent && event_is_bubbled(e)) {"`). A shield deleted
in place would carry that reset to the grid handlers. It stays silent only because
`helix::ui::safe_delete_deferred_raw` moves an object to `lv_layer_top()` before deleting it
(`include/ui_utils.h#safe_delete_deferred_raw`), so the reset bubbles to the top layer instead,
and a shield deleted under a live gesture ends that gesture with no event reaching the session.
Therefore:

- A selection change redraws only the lattice children (`rebuild_lattice`).
- A page switch moves the same shield with `lv_obj_set_parent`, which keeps it the press target,
  even inside that press's own dispatch (`src/ui/home/grid_edit_mode.cpp#ensure_shield`).
- The shield dies with the container's other children in a deferred rebuild on the next tick,
  and `rebuild_then_select()` creates the next shield after it. A commit schedules that rebuild
  after its release; a cancel schedules it once it ended the gesture, with the finger possibly
  still down. The rebuild moves the shield off its page before deleting it
  (`helix::ui::safe_clean_children`), so the INDEV_RESET that delete sends reaches no grid
  handler.
- A gesture that owns the pointer when that rebuild runs, such as a fresh grab made before the
  tick, loses its press target and gets no end event. `begin_press()` on the next PRESSED finds a
  gesture still owning the pointer, logs a warning and ends it uncommitted.

## Gesture end

Three events on the shield end every gesture, each bubbling to a grid handler:

| Event on the shield | When LVGL sends it | Path |
|---------------------|--------------------|------|
| `LV_EVENT_RELEASED` | The finger lifts | `on_home_grid_released` -> `handle_released()`: commit the drag or resize (`handle_drag_end`, `handle_resize_end`), then `clear_gesture_state()` |
| `LV_EVENT_PRESS_LOST` | The lift after `lv_indev_wait_release()` (`indev_proc_release`: `lib/lvgl/src/indev/lv_indev.c#"lv_obj_send_event(indev->pointer.act_obj, LV_EVENT_PRESS_LOST, indev_act);"`) | `on_home_grid_press_cancelled` -> `handle_press_cancelled()` -> `end_gesture_uncommitted()` |
| `LV_EVENT_INDEV_RESET` | `lv_indev_reset()` takes the press | `on_home_grid_press_cancelled` -> `handle_press_cancelled()` -> `end_gesture_uncommitted()` |

`end_gesture_uncommitted()` is the single cancel path, shared with `begin_press()` and
`HomePanel::exit_grid_edit_mode()` (`src/ui/home/grid_edit_mode.cpp#end_gesture_uncommitted`):

```cpp
void GridEditMode::end_gesture_uncommitted() {
    if (!active_ || !owns_gesture()) {
        return;
    }
    // A drag past the last page goes home while still live: the session must
    // not stay scoped to a page that does not exist.
    if (on_next_page_slot()) {
        return_drag_to_origin();
    }
    // No release will clear this gesture's state: the dragged widget's float
    // and both previews go as a release would take them.
    tear_down_gesture();
    cancel_snap_animation();
    // Selection goes too: an aborted gesture has no meaningful selection,
    // and dropping it here (not in the deferred rebuild) keeps the tail
    // self-sufficient when no rebuild callback is wired.
    select_widget(nullptr);
    notify_gesture_ownership();
    // Restore the world from config: widget positions (an aborted move leaves
    // the widget at its drop point), selection chrome, lattice. The rebuild
    // deletes the shield with the container's other children, so the next
    // gesture needs the one rebuild_then_select creates after it.
    rebuild_then_select("");
}
```

A drop that commits nothing ends differently: it keeps its widget selected and rebuilds nothing,
since its release left every object where it belongs (see [Drop resolution](#drop-resolution)).

**A reset has to cancel.** `lv_indev_reset()` clears the press and scroll target of every pointer
it visits, whatever object it names, and the only event it sends is INDEV_RESET: to the old press
target and to the old scroll object, each also sent as an indev-level event
(`indev_reset_core`: `lib/lvgl/src/indev/lv_indev.c#"lv_obj_send_event(act_obj, LV_EVENT_INDEV_RESET, indev);"`). With the finger still down, the next read
searches under the pointer again and sends PRESSING, then RELEASED, to whatever it finds, never
PRESSED (`indev_proc_press`: `lib/lvgl/src/indev/lv_indev.c#"if(indev->prev_state != LV_INDEV_STATE_PRESSED) {"`). A lift before that read sends no press
event: the release path hit-tests only for hover, and may send HOVER_OVER and HOVER_LEAVE, but it
has no press target to release (`indev_proc_release`: `lib/lvgl/src/indev/lv_indev.c#"lv_obj_t * hovered = pointer_search_obj(lv_display_get_default(), &indev->pointer.act_point);"`). No RELEASED
the shield could commit on follows a reset.

**So input is reset only when the press itself should end.** Navigation, a modal closing and the
display waking from sleep, dim or a screensaver interrupt a drag for real, and cancel it (the
wake through `src/application/display_manager.cpp#cancel_pointer_presses`); nothing resets input as the
display goes to sleep. The rule for new code that retires
objects while a press may be in flight on them: reset input through
`helix::ui::reset_input_within(subtree)` (`src/ui/ui_utils.cpp#reset_input_within`), which resets
only a pointer whose press or scroll target lies inside the subtree, and never call an unscoped
`lv_indev_reset()` unless the point is to end every press. Toast teardown is the model
(`src/ui/notify/ui_toast_manager.cpp#detach_from_input`): the "Not enough room" toast edit mode raises
can time out while the user rearranges, as it asks, without ending the drag.

## Cross-page drag

Every drag move (`src/ui/home/grid_edit_mode.cpp#handle_drag_move`) gathers one
`helix::CrossPageInput`, asks `helix::cross_page_step()`, and applies the step:

```cpp
    const lv_area_t frame = settled_content_area();
    helix::CrossPageInput cross;
    cross.frame_x1 = frame.x1;
    cross.frame_x2 = frame.x2;
    cross.cell_w = m.cell_w;
    cross.pointer_x = point.x;
    cross.widget_left = point.x - drag_offset_.x;
    cross.widget_width = lv_area_get_width(&widget_area);
    cross.elapsed_ms = elapsed_ms;
    cross.page_index = page_index_;
    cross.page_count = static_cast<int>(config_->page_count());
    cross.has_next_page_slot = has_next_page_slot();
    const helix::CrossPageStep step = helix::cross_page_step(cross_page_, cross);

    // Both flips wait on timers, since both are deadlines: the dwell runs out
    // while the pointer may rest, and a crossing flips only if the finger is
    // still down when kCrossPageCrossingDelayMs runs out.
    if (step.flip_dir != 0) {
        start_crossing_flip(step.flip_dir);
    }
    if (step.dwell_changed) {
        restart_dwell_flip(step.dwell_dir);
    }
```

- **The page frame.** The rules measure against where the scoped page sits with the carousel at
  rest: HomePanel's page frame callback reports the carousel viewport, and
  `settled_content_area()` insets it by the container's padding. A page sliding in mid-flip moves
  nothing the rules compare. The widget itself is positioned relative to the live container, so
  it stays under the finger while its page slides. The frame is the grid area, so in landscape
  the left edge zone sits beside the navigation bar, not at the screen edge.
- **Edge push.** While the pointer stays in an edge zone, the widget slides past the pointer at a
  speed proportional to the column track, capped `kCrossPagePushCapSlopPx` past the majority
  line, so a pinned pointer always reaches a crossing.
- **Majority crossing.** The widget's majority past a frame edge requests one flip, due
  `kCrossPageCrossingDelayMs` later; the next crossing waits until the widget is majority
  inside again.
- **Dwell.** The pointer held in an edge zone for `kCrossPageDwellMs` flips toward that edge.
  The timer restarts only when the dwell's direction changes (`dwell_changed`), so the reads a
  resting finger produces every read period do not hold it off.
- **One flip per zone stay.** A crossing stops the dwell, and a flip spends the zone for both
  triggers until the pointer leaves it, so a held push never chains two pages.

The zone width and the push speed derive from the column track (`cross_page_edge_zone_px`,
`cross_page_push_px_per_s` in `include/grid_edit_cross_page.h`), the way the resize grab band
derives from the cell size. Both flips run from one-shot LVGL timers (`crossing_flip_timer_`,
`dwell_flip_timer_`), and ask HomePanel for page `page_index_ + dir` through `ShowPageCallback`
(`src/ui/home/grid_edit_mode.cpp#request_page_flip`). HomePanel ignores a page it has no container for.
`clear_gesture_state()` cancels both timers, so no flip outlives its drag.

Whether a release creates a page is not decided per move: the drop asks
`cross_page_drop_creates_page()` with the scope the release lands in (see
[Drop resolution](#drop-resolution)).

## Drop resolution

`handle_drag_end` gathers the release into a `helix::DropInput` (`drop_input()`), asks
`helix::resolve_drop()` against the scoped page's occupancy, commits the result, prunes the page
the move emptied, and saves once (`src/ui/home/grid_edit_mode.cpp#handle_drag_end`). A commit that adds
or removes a page describes that as a `helix::PageSetChange` for `notify_pages_changed()` (see
[Page lifecycle](#page-lifecycle)):

```cpp
void GridEditMode::handle_drag_end(lv_event_t* /*e*/) {
    if (!selected_ || !container_ || !config_) {
        tear_down_gesture();
        return;
    }

    // Taken from the object before the commit, which can move the entry to
    // another page; the rebuild selects the widget again by it.
    const std::string moved_id = selected_widget_id();
    const helix::DropResolution drop =
        helix::resolve_drop(drop_input(), page_occupancy(moved_id, Occupants::OnScreen));
    const int origin_page = drag_orig_page_;
    const int pages_before = static_cast<int>(config_->page_count());
    const int landed_page = commit_drop(moved_id, drop);
    spdlog::debug("[GridEditMode] Drag end: outcome {} at ({},{}), landed on page {}",
                  static_cast<int>(drop.outcome), drop.col, drop.row, landed_page);

    if (landed_page < 0) {
        // A drop that commits nothing resolves on its origin page as a
        // cancelled move: a drag off its origin page is carried home while
        // still live, so the widget, the session and the carousel end where its
        // entry still is. A commit the config refused goes home the same way.
        if (drop.outcome != helix::DropOutcome::Cancel) {
            return_drag_to_origin();
        }
        lv_obj_t* dragged = selected_;
        tear_down_gesture();
        reselect_in_place(dragged);
        return;
    }

    tear_down_gesture();
    helix::PageSetChange change;
    change.page_count = pages_before;
    change.page_added = drop.outcome == helix::DropOutcome::CreatePage;
    change.focus_page = landed_page;
    if (landed_page != origin_page && prune_empty_page(origin_page)) {
        change.removed_page = origin_page;
    }
    // One save for the whole commit: the move, a page it created and a page it
    // emptied.
    config_->save();
    if (change.page_added || change.removed_page >= 0) {
        // The panel rebuilds the carousel on the next tick, and the session goes
        // with the entry; the grid's own deferred rebuild would run against
        // containers that rebuild replaces.
        notify_pages_changed(change);
        return;
    }
    // A move that leaves the page set alone lands on the scoped page.
    forget_container_children();
    rebuild_then_select(moved_id);
}
```

| Outcome | When `resolve_drop()` returns it | What commits |
|---------|----------------------------------|--------------|
| `Move` | On a config page, the previewed cell or the page differs from the origin and the span fits the occupancy there | `commit_drop()` -> `PanelWidgetConfig::place_entry` |
| `CreatePage` | `cross_page_drop_creates_page()` holds for the scope the release lands in: on the next-page slot, the entry lands at the previewed cell; on the last page with the widget's majority past its right border, in the rightmost columns its span allows, on the previewed row; on the first page with the widget's majority past its left border, in the leftmost column, on the previewed row, and `prepend_page` is set so the page lands before it. A landing cell that does not fit an empty page creates nothing | `commit_drop()` -> `add_page` (appends, or inserts at 0 when `prepend_page`), then `place_entry` |
| `ReturnToOrigin` | Nothing commits, and the release landed off the origin page | Nothing; `return_drag_to_origin()` carries the drag home first |
| `Cancel` | Nothing commits, on the origin page | Nothing; the widget snaps back |

A drop that commits nothing, including one the config refuses, ends with the dragged widget laid
out on its origin page and selected again (`reselect_in_place`), and rebuilds nothing.

Occupancy comes from `page_occupancy(exclude_id, occupants)`, the one builder every placement
check uses. `Occupants::OnScreen` (drag preview, drop, catalog placement) counts a placed entry
only when the scoped container holds a laid-out object named by its id. `populate_widgets()`
creates that object for every entry it places, a hardware-gated widget included, dimmed and named
like any other (`src/ui/home/panel_widget_manager.cpp#populate_widgets`), so a gated widget holds its
cell. A placed entry has no object only when its widget failed to configure or to create, and
OnScreen leaves that entry's cell free. `Occupants::AllPlaced` (resize) counts every placed entry,
so a resize never grows over a cell another entry holds in config.

## The widget catalog

The catalog opens from a hold on empty grid with nothing selected, at the cell the hold measured
(`handle_long_press` records `catalog_origin_col_` and `catalog_origin_row_`), or from the navbar
"+" (`HomePanel::open_widget_catalog`), and is placed through `GridEditMode::open_widget_catalog`
(`src/ui/home/grid_edit_mode.cpp#open_widget_catalog`). While it is open:

- `is_catalog_open()` is true, `switch_page()` keeps the session's page, and the swipe policy is
  `Disabled`.
- Grid input takes no action: `grid_input_acts()` is false, so no press, hold or click the grid
  handlers pass on, and no tap on the configure, remove or delete-page button, acts.
  Navigation's dismiss backdrop normally takes every press beside the catalog; the gate holds
  when that backdrop could not be created.
- The shield and its lattice stay drawn. The catalog is a later child of the screen than the
  panel, so the shield never covers it.
- A deactivation other than a hot-reload rebuild leaves the session live (see [Leaving](#leaving)).

When it closes, the session announces gesture ownership again and, while the session is still
active, asks HomePanel to show its page through `ShowPageCallback`, since the carousel arrow
buttons may have paged the carousel while the catalog held the session. The close `exit()` makes
shows no page. A selection runs `place_widget_from_catalog()` on the session's
page: the origin cell, then the first free cell, then smaller sizes down to the widget's minimum,
and a "Not enough room" toast when nothing fits. It moves the entry with `place_entry`, saves
once, and rebuilds with the new widget selected.

## Pages

### The carousel build

`HomePanel::build_carousel(shown)` creates one `home_page_container` per config page and, while
the config is below the page cap, a `home_next_page_slot` tile after them
(`src/ui/home/ui_panel_home.cpp#build_carousel`), states the swipe policy, goes to `shown` unanimated,
populates every page, and observes the page subject last, so the observer's first notification
re-scopes nothing into a container the population has just cleared. The indicator dots count
config pages only. `ui_carousel_goto_page()` clamps to config pages;
`helix::ui::carousel_goto_tile()` reaches the slot's tile while it is within reach.
`rebuild_carousel(shown)` makes a live session `forget_scope()` before the teardown deletes its
containers, then builds on `shown`.

### The next-page slot

Below the page cap, the carousel holds one `home_next_page_slot` tile past the last page
(`ui_xml/components/home_next_page_slot.xml`): an empty page container a drag carries a widget
onto, and beside it the **+ button** that adds a page on a tap. Both run through the one
creation path, `PanelWidgetConfig::add_page()` - the + appends a page with no widget on it
(`HomePanel::add_page_from_slot`, from the XML callback `on_add_page_clicked`), a drop appends
past the last page or inserts at index 0 before the first, with the dragged widget placed - so
the cap guard, the page ids and the landing rules are the same either way.

- The + is a plain click on the button itself, so it works with a session live or not; in edit
  mode the session follows the landing onto the added page. It sits beside
  `next_page_container`, not in it: the session disarms clicks only inside the container it
  covers, and the + must survive a drag riding onto the slot.
- A drop on the slot creates the page at commit and lands the widget at the dropped cell. A drop
  on the last page with the widget's majority past its right border appends the same way, with
  the widget in the rightmost columns its span allows; a drop on the first page with the
  widget's majority past its left border sets `prepend_page`, and the page lands at index 0 with
  everything after it shifted one page later.
- A release on the slot that creates nothing carries the drag back to its origin page first
  (`return_drag_to_origin`), where it resolves as a cancelled move.
- At the page cap there is no slot: no + tile, `has_next_page_slot()` is false, and a drop past
  either border stays on its page.

**The slot's tile is a real tile, laid out and shown from the build.** The + is a swipe
destination, which is why the swipe counts reachable tiles rather than pages
(`apply_input_flags`): a single-page home with a slot swipes over to it, and the panel's page
stays the last config page while the carousel rests on the slot. Reach therefore never toggles
with the gesture state; `carousel_set_trailing_tiles_reachable()` is false only while no slot
exists, which the cap and the carousel's own unit tests pin directly. A drag rides onto the slot
by the ordinary cross-page flip, and the session scopes to the slot's container as page
`page_count()` (`on_next_page_slot()`).

- A drop there that creates a page leaves the session scoped to the slot's container and the
  carousel resting on its tile, the dropped widget on screen, until the page-set rebuild on the
  next tick replaces the carousel.
- A cancelled drop, or an abort, carries the drag back to its origin page first
  (`return_drag_to_origin`, from `handle_drag_end` and `end_gesture_uncommitted`). The slide
  back runs on a tile that stays shown: LVGL readjusts no scroll offset along an axis that
  snaps, and the slot exists until the rebuild. Leaving edit mode mid-drag ends the gesture
  this way first.

### Page lifecycle

| Change | Where | Carousel rebuild |
|--------|-------|------------------|
| A drop creates a page | `GridEditMode::commit_drop` -> `PanelWidgetConfig::add_page` and `place_entry` | Next tick, through `PagesChangedCallback` -> `HomePanel::on_edit_pages_changed` |
| A cross-page move, or removing a page's last widget, empties the page | `GridEditMode::prune_empty_page` -> `PanelWidgetConfig::remove_page` | Next tick, the same path |
| The delete-page button is confirmed | `HomePanel::delete_edit_page`: remove the page, save, leave edit mode | Next tick, the same path, through `run_next_tick` |

A page is empty when it holds no placed entry (`page_is_populated`), so a page whose
remaining widgets are dimmed because their hardware is missing still counts as populated and
stays. `remove_page()` refuses the main page, the last remaining page and an index past the end,
and pruning relies on those guards rather than repeating them. When a commit changes the page
set, the grid skips its own deferred rebuild: HomePanel rebuilds the whole carousel and shows the
focus page with the session re-scoped (`src/ui/home/ui_panel_home.cpp#on_edit_pages_changed`).

**Where a page-set rebuild lands.** Every change to the page set from edit mode describes itself
as a `helix::PageSetChange`, numbered as the carousel was before it, with the next-page slot's
tile at `page_count` and the phantom tile a prepend creates at `-1`: how many pages there were,
whether a page was added, on the slot's tile or before the first page, which page was removed,
and the page the change asks to end on. `on_edit_pages_changed(change)`
asks `helix::page_set_landing(change, shown)` with the carousel's page before the rebuild
(`include/grid_edit_page_set.h#page_set_landing`), builds the carousel on the landing's `shown`,
and shows its `focus` with `show_edit_page()`, which re-scopes a live session there:

- The rebuilt carousel comes up on the page that was on screen, in the new numbering.
- When the focus is another page, `show_edit_page()`'s animated goto slides the carousel once,
  from there to the focus.
- It comes up on the focus directly only when the page on screen no longer exists: the change
  removed it, or it is the next-page slot and no page was added there.
- A focus the change removed lands on its own index, clamped to the last page: the page after
  it, or the new last page.

The session follows the same rule from the commit on. `GridEditMode::notify_pages_changed()`
sets the session's page to `helix::page_set_focus(change)`, the landing's focus, so while the
rebuild waits for the next tick the session names neither the main page nor an index the change
shifted. Where each change lands:

| Change | The rebuilt carousel comes up on | And ends on |
|--------|----------------------------------|-------------|
| A drop on the next-page slot | The new page, on the tile the slot held, or one tile before it when the move pruned its origin page | The same page, with no slide |
| A drop past the last page's border, released before a flip, that keeps its origin page | The origin page, the page on screen | The new page, one slide on |
| The same drop when it prunes its origin page | The new page, which takes the origin's index | The same page |
| A drop past the first page's left border | The page the drag left, one page later than before | The new page 0, one slide back |
| A tap on the slot's + | The appended page, on the tile the slot held, with no slide; the page on screen, one slide on, when the tap came from a page | The appended page |
| A move that prunes a page before its landing page | The landing page at its shifted index, the page already on screen | The same page |
| Removing a page's last widget | The page that takes the removed page's index, or the new last page: the page on screen is gone | The same page, where the session already is |
| The delete-page button | The page that takes the deleted page's index, or the new last page. Edit mode has ended | The same page |

The delete-page button is drawn on the shield on a config page other than the main page while
more than one page exists, and never on the next-page slot
(`src/ui/home/grid_edit_mode.cpp#rebuild_lattice`). Its click asks first, through the confirmation
`HomePanel::wire_grid_edit_page_callbacks()` installs, and the confirmation's action is
`delete_edit_page()`.

## Subjects

| Subject | Type | Owner | Meaning |
|---------|------|-------|---------|
| `home_edit_mode` | int | App globals (`src/app_globals.cpp#app_globals_init_subjects`) | 1 while a session is live, written only by `enter()` and `exit()` |
| `home_page_badge` | string | HomePanel | "N / M": the active page among config pages |
| `home_populated_pages` | int | HomePanel | Pages holding a placed widget (`page_is_populated`) |

`home_edit_mode` has several readers. The navbar's + and Done buttons show only while it is 1
(`ui_xml/navigation_bar.xml#nav_btn_edit_add`, `nav_btn_edit_done`), and its Home, Controls,
Filament, Settings and Advanced buttons and the printer badge are disabled while it is 1. The page
badge binds it, and `CameraWidget` observes it to cut its stream's frame rate during edit mode
(`src/ui/panel_widgets/camera_widget.cpp#update_stream_fps`).

The two HomePanel subjects are registered at construction in their own `SubjectManager`
(`src/ui/home/ui_panel_home.cpp#init_panel_subjects`), not in `subjects_`: `rebuild_carousel()` deinits
`subjects_`, which holds the per-build page subject, and these outlive every carousel rebuild.
The badge label sits outside `carousel_host`. The badge shows only in edit mode with more than one
populated page (`ui_xml/home_panel.xml`):

```xml
      <bind_flag_if cond="home_edit_mode eq 1 and home_populated_pages gt 1" flag="hidden" invert="true"/>
```

## Config contracts

- `page_entries(i)` reads a page index past the last page as an empty page. `page_entries_mut(i)`
  requires `i < page_count()`, because there is no page to write to.
- `PanelWidgetEntry::is_placed()` is `enabled && has_grid_position()`, and `page_is_populated(i)`
  means the page holds a placed entry. The badge count and pruning share it.
- `place_entry(id, page, col, row, colspan, rowspan)` leaves exactly one entry named `id` across all
  pages and carries its per-widget config along. Drag commit and catalog placement both use it.
- `can_add_page()` is the page cap (`helix::kMaxPages`); `add_page()` returns -1 at the cap. Its
  `position` parameter is the one creation seam: `-1` appends, `0` inserts before the first page
  (shifting the main page index with it), and any other position clamps into the page list. The
  slot's + and a right-border drop append; a left-border drop inserts at 0.
- Saves. Each commit saves once: a drop (`handle_drag_end`, with the page it created and the page
  it emptied), a resize (`commit_resize_with_snap`, before its snap animation starts), a removal
  (`remove_selected_widget`, with the page it emptied), and a catalog placement
  (`place_widget_from_catalog`). `exit()` saves. HomePanel saves when a page deletion is
  confirmed. `sync_config_from_screen()` saves only when a laid-out widget has drifted from its
  config cell, and runs only in `enter()`, never per page change. A page flip, a
  cancelled gesture and `prune_empty_page()` itself save nothing.

## Extending edit mode

- **A new per-gesture value**: reset it in `clear_gesture_state()`, never by hand at a call site.
- **A change that arms or ends a gesture**: call `notify_gesture_ownership()` after it.
- **A new object in the scoped container**: null its pointer in `forget_container_children()`, and
  never make it the press target; clickable chrome goes above the shield.
- **A new input on the grid** (a handler, a chrome button): act only while `grid_input_acts()`,
  and in HomePanel only while `carousel_on_scoped_page()` for input that acts on what a press
  lands on.
- **A structural change from an event handler**: run it on the next tick
  (`helix::ui::run_next_tick`, or `schedule_deferred_rebuild()` and `rebuild_then_select()` inside
  the grid), never inside input dispatch.
- **A new cross-page rule**: extend `cross_page_step()` and its table test; `handle_drag_move()`
  stays a gather-and-apply loop.
- **A new drop outcome or drop rule**: extend `resolve_drop()` and its table test;
  `handle_drag_end()` stays gather, resolve, commit.
- **A widget that refuses sizes**: the resize clamp asks the selected widget through
  `PanelWidget::fits_at()` after the registry limits, both inside
  `src/ui/home/grid_edit_mode.cpp#handle_resize_move`, walking with
  `helix::grow_span_to_fit()` (`include/grid_layout.h`). A size the widget refuses
  clamps the span and turns the pixel-following preview red exactly as a registry
  limit does. Keep `fits_at` monotonic (fits at a size means fits at every larger
  size) or the clamp's outward walk stops at the wrong span.
- **A new page operation**: a `PanelWidgetConfig` method with unit tests, and a carousel rebuild on
  the next tick through `on_edit_pages_changed()`, with the change described as a
  `helix::PageSetChange`, so it lands by `page_set_landing()` like every other page-set change.
- **Code anywhere that retires objects under a possible press**:
  `helix::ui::reset_input_within(subtree)`.

## Testing

| Layer | File | Tag |
|-------|------|-----|
| Cross-page rules | `tests/unit/test_grid_edit_cross_page.cpp` | `[cross_page]` |
| Drop resolution | `tests/unit/test_grid_edit_drop.cpp` | `[cross_page]` |
| Where a page-set rebuild lands | `tests/unit/test_grid_edit_page_set.cpp` | `[home]` |
| Page operations | `tests/unit/test_panel_widget_config.cpp` | `[widget_config]` |
| Carousel swipe, bubbling, `goto_tile`, the reach of tiles past the pages | `tests/unit/test_ui_carousel.cpp` | `[carousel]` |
| Scoped input reset | `tests/unit/test_reset_input_within.cpp` | `[indev]` |
| Drag and resize geometry | `tests/unit/test_grid_edit_drag_path.cpp`, `tests/unit/test_grid_edit_mode.cpp` | `[grid_edit_drag]`, `[grid_edit]` |
| The wiring | `tests/unit/test_home_edit_page_swipe.cpp` | `[edit-swipe]` |

`[1638]` runs the cases written for the pages work in `test_home_edit_page_swipe.cpp`,
`test_grid_edit_cross_page.cpp`, `test_grid_edit_drop.cpp`, `test_grid_edit_page_set.cpp` and
`test_panel_widget_config.cpp`.
The carousel and input-reset cases carry only their own tags.

**`EditHomeFixture`** runs HomePanel's own `build_carousel()` on a stand-in root holding a
`carousel_host` with the handlers `home_panel.xml` attaches, and installs the rebuild and
page-deletion callbacks `finalize_setup()` installs (`HomePanelTestAccess::wire_grid_edit_page_callbacks`),
so the pages, the slot, the page subject and its observer, page population and every rebuild are
production code. Pages are populated by `HomePanel::populate_page()`; only the widgets stand in:
`ScopedWidgetFactory` swaps the registry factories of the ids the config names for a plain named
tile, so a case depends on no printer state. A rebuild replaces a widget's object, so a case looks
a widget up by name after one. After a flip or a drop, `check_session_on_screen()` asserts that
the carousel shows the session's page, that page's container is the one at rest in the viewport,
and the shield and selection live in it. `watch_page_set_change()`, taken before the input that
changes the page set, and `run_page_set_rebuild()` pin where the rebuild lands: the page the
rebuilt carousel comes up on, the one slide to the focus when the focus is another page, counted
from the page strip's animated scrolls as they bubble to `carousel_host`, and the pages the badge
passes through. `ScopedWidgetCatalog` stands up the real widget catalog
for the cases that open it, and the hot-reload case creates the panel from `home_panel.xml` and
drives `NavigationManager::rebuild_active_views()`.

**`ScopedPointerIndev`** drives a real pointer one read at a time. The harness facts the suite is
built around:

- `lv_indev_active()` is non-null only inside a read, so a handler that asks for the pointer runs
  for real only from `press()`, `move()`, `release()` or `hold()`. `lv_obj_send_event()` reaches
  it with no indev.
- A read advances no time, and nothing reads on its own: `lv_timer_handler_safe()` fires one-shot
  timers only, never the indev's periodic read timer. `hold(ms)` reads every `kReadPeriodMs` and
  advances the tick and the one-shot timers between reads, the device's cadence.
- `grab(x, y)` is the two-step grab. `long_press_hold_ms()` ends a hold on the read that dispatches
  LONG_PRESSED.

The test binary links a stub `ToastManager`, so toast teardown is exercised through
`reset_input_within()` on a toast's shape (`tests/unit/test_reset_input_within.cpp`), not through
a real toast.

Thresholds, zones and times come from the code's constants (`drag_threshold_px()`,
`edge_zone_px()`, `kCrossPageDwellMs`), never literals. Run each new case alone once, and
`make test-order-dependence`, which takes no file: it re-runs every test file alone against a
full-suite report.

```bash
make test
./build/bin/helix-tests "[edit-swipe]"
./build/bin/helix-tests "[1638]"
```

**Live.** `helix-screen ctl` drives the real input pipeline on a mock instance; the Edit Mode
recipes (entering, swiping, the two-step grab, dragging onto the next-page slot) are in
[HELIXCTL.md](HELIXCTL.md). `ctl get home_edit_mode` and `home_populated_pages` read the
session's state. The log lines worth watching at `-vv`:

| Log line | Means |
|----------|-------|
| `[GridEditMode] Drag started: widget '...'` | A grab became a drag |
| `[GridEditMode] Page flip request: trigger=crossing` / `trigger=dwell` | A cross-page flip asked for a page |
| `[GridEditMode] Drop on a page border: created page N at (C,R)` | A drop created a page, appended or prepended, landing the widget at (C,R) |
| `[GridEditMode] Removed empty page N` | A page emptied and was pruned |
| `[Home Panel] Edit pages changed; rebuilding carousel on page N, focusing page M` | A page-set rebuild comes up on page N and ends on page M, one slide on when they differ |
| `PRESS_LOST on the event shield ends the gesture uncommitted` / `INDEV_RESET on ...` | LVGL took a gesture's press away and it was cancelled |
| `A press began with a gesture still owning the pointer` | A gesture outlived its press target and the next press ended it |
