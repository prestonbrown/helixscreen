# Page Scroll Buttons

**Version:** 1.0
**Last Updated:** 2026-08-14
**Status:** Shipped

A chevron column that pages a long screen up and down without a swipe. Off by
default on desktop and embedded Linux, **on by default on ESP32**, where
finger-drag scrolling on the panel is slow enough that the buttons are the
primary way to move a list.

Nothing opts in. A tree walk finds containers that qualify and injects the
gutter into them, which makes the attach policy the most important thing in this
document: the walk decides where chevrons appear, and getting it wrong puts them
on top of somebody's content.

---

## Key files

| Path | Role |
|------|------|
| `include/page_scroll_math.h` | Pure paging math. No LVGL state, trivially testable |
| `include/page_scroll_controller.h` + `src/ui/page_scroll_controller.cpp` | One controller per managed container. Owns the gutter, the reserved padding, and the scroll wiring |
| `include/page_scroll_auto_inject.h` + `src/ui/page_scroll_auto_inject.cpp` | The policy. Walks a shown root and decides what gets a controller |
| `ui_xml/components/page_scroll_gutter.xml` | The chevron column itself. Created by the controller, never placed in screen XML |
| `src/system/display_settings_manager.cpp:234-248` | The setting and its per-platform default |

Registration is at `src/xml_registration.cpp:319` (the component) and `:782`
(`PageScrollAutoInject::init()`). Teardown is
`src/application/application.cpp:4700`.

---

## The setting

Config key `/display/page_scroll_buttons`, subject
`settings_page_scroll_buttons`, accessors
`DisplaySettingsManager::get_page_scroll_buttons()` / `set_page_scroll_buttons()`.

```cpp
// src/system/display_settings_manager.cpp:234-248
#if defined(ESP_PLATFORM)
    constexpr bool page_scroll_default = true;
#else
    constexpr bool page_scroll_default = false;
#endif
```

That split is why an ESP32-only layout bug in this feature can sit unnoticed on
every other platform. If you change anything here, reason about ESP32 first.

The user toggle lives in Display settings and drives
`PageScrollAutoInject::on_setting_toggled()` directly from its callback. It is
deliberately **not** a subject observer, and the rationale is in
`PageScrollAutoInject::init()`: the settings subject is re-initialised lazily
when the Settings panel is first built, which is after `init()` runs, and that
re-init clears the observer list. An observer registered there would never fire.

---

## Attach policy

This is the part to read before you change anything.

`on_root_shown(root)` is called from four places in `NavigationManager`, all of
them "a root just became visible": `src/ui/ui_nav_manager.cpp:1402` (panel
activate), `:1744` (initial panel), `:1979` and `:2085` (overlay push). It reads
the setting live, prunes dead controllers, forces a layout pass so overflow is
measurable, then walks.

A container qualifies when all three hold (`page_scroll_auto_inject.cpp:47-55`):

| Condition | Why |
|---|---|
| has `LV_OBJ_FLAG_SCROLLABLE` | it is a scroll region at all |
| `lv_obj_get_scroll_dir() & LV_DIR_VER` | horizontal-only regions (the home carousel) are not paged |
| `scroll_bottom > 0 \|\| scroll_y > 0` | content actually overflows right now |

The walk then cuts on three things, in this order:

1. **`LV_OBJ_FLAG_HIDDEN`** - do not inject into a stacked or hidden panel.
2. **`helix::PANEL_WIDGET_TILE_FLAG`** - do not descend into a home widget tile.
3. **An already-claimed ancestor** - a qualifying container underneath a managed
   one does not get its own gutter, so gutters never nest. The claim propagates
   across repeated walks over a persistent tree, which matters when you navigate
   away from a cached panel and back.

### Why tiles are cut

Page scrolling is a **page-level** affordance. A home widget tile is sized by
the home grid and scrolled by dragging it, so a gutter inside one is not a
smaller version of the right thing, it is the wrong thing at the wrong scale.

The numbers make it concrete. The gutter is two `#button_height_lg` chevrons
separated by a `#space_2xl` gap, in a `#button_height`-wide column:

| Tier | Chevron | Gap | Total gutter |
|------|---------|-----|--------------|
| medium (800x480, the K-Touch) | 70px | 32px | **52 x 172px** |

172px of chevrons inside a tile on a 480px-tall screen is most of the tile.

The cut is a **subtree** cut, not a "is this tile scrollable" test, and that
distinction is load-bearing. 37 of the 40 `panel_widget_*.xml` roots already set
`scrollable="false"`, and two more inherit the clear from `ui_card`
(`src/ui/ui_card.cpp:60`). So testing the tile root would find almost nothing -
the walk was sailing straight through those innocent-looking roots and
attaching to whatever was scrollable inside. Marking the root and returning
there is what actually stops it.

Tiles are marked at the single place one is created,
`src/ui/panel_widget_manager.cpp`, right after `lv_obj_set_name()`. There is no
second path: the widget pool recycles `PanelWidget` C++ instances, not LVGL
objects, and the object tree is always cleaned and rebuilt.

`panel_widget_nozzle_temps` is the one tile whose root sets `scrollable="true"`
on purpose. It drags to scroll and gets no chevrons, which is the intended
outcome: a tile is small enough that the arrows would cover most of what it is
showing, and a short drag scrolls it anyway. If some future widget genuinely
needs paging, add a narrow opt-back-in rather than removing the cut.

---

## How a gutter behaves once attached

The controller (`src/ui/page_scroll_controller.cpp`) creates the gutter as a
child of the container it manages (`:35`) and positions it with
`floating="true" align="right_mid"`, so it neither participates in the parent's
flex or grid layout nor scrolls with the content.

To keep the chevrons off the content, `apply_reserved_padding()` (`:79-88`)
grows the container's `pad_right` by the gutter width and translates the gutter
back out over the strip it just freed. The original `pad_right` and scrollbar
mode are saved at attach (`:31`, `:61`) and restored on detach (`:94`).

`refresh_reach_state()` (`:98-121`) runs on scroll and resize: it hides the
gutter and drops the padding when content fits, and otherwise disables the up or
down chevron at each end. Paging is
`lv_obj_scroll_by_bounded(container_, 0, -direction * step, anim)` (`:141`),
where `step` is `page_scroll_step()` - 90% of the viewport, so a row overlaps
between pages - and `anim` follows the Animations setting.

> **Known limitation.** The reserved-padding trick only moves children that
> respect padding. A child positioned with `align`, `ignore_layout`, or
> `floating` ignores it and will sit under the chevrons. If you are debugging an
> overlap and the container legitimately qualifies, check how the overlapped
> child is positioned before assuming the walk is at fault.

Z-order is implicit from child index. The gutter is appended at attach time, so
anything added to the container afterwards draws over it. Nothing calls
`lv_obj_move_foreground()` anywhere in the feature.

---

## Lifetime

The controller watches `LV_EVENT_DELETE` on both the container and the gutter.
The gutter watch exists because `lv_obj_clean()` on a surviving container
destroys the gutter without firing the container's own delete, which dangled
`gutter_` into `refresh_reach_state()` (#1123).

`walk_and_attach()` erases a controller from the map synchronously inside its
own deleted-callback. That is safe only because the controller nulls
`container_` before invoking the callback and touches no member afterward. It is
a `delete this`-adjacent pattern, and the invariant is called out at both ends.
Do not add code that touches the controller after the erase.

---

## Debugging

```bash
./build/bin/helix-screen --test -s 800x480 -vv --remote-socket /tmp/s.sock
./build/bin/helix-screen ctl -s /tmp/s.sock geom <container_name>
```

`ctl geom` reports the `scrollable` flag and the scroll extents, which is how
you confirm whether a container qualifies. The walk logs its result per root:

```
[PageScroll] on_root_shown root=0x... -> managed=2
```

`managed=0` means nothing on that root qualified. Two gotchas that will waste
your time otherwise:

- **`--test` reads settings-test.json, not `settings.json`.** Setting
  `display.page_scroll_buttons` in the wrong file silently does nothing.
- **The desktop default is off.** You have to enable it explicitly to see any of
  this on a dev machine, which is the whole reason an ESP32-only regression is
  easy to miss.

---

## Testing

| File | Covers |
|------|--------|
| `tests/unit/test_page_scroll_math.cpp` | the pure paging step and reach computation |
| `tests/unit/test_page_scroll_setting.cpp` | the setting round-trips |
| `tests/unit/test_page_scroll_gutter_component.cpp` | the XML component builds with named up/down buttons |
| `tests/unit/test_page_scroll_controller.cpp` | attach, padding restore, end-disabling, last-page clamp, container deletion |
| `tests/unit/test_page_scroll_auto_inject.cpp` | the policy: what qualifies, nesting, the tile cut |

```bash
./build/bin/helix-tests "[page_scroll_buttons]"
```

If you change the attach policy, the test to extend is
`test_page_scroll_auto_inject.cpp`, and the assertion that keeps a policy change
honest is the one asserting a page-level container **still** gets its gutter. A
cut that suppresses everything passes a test that only checks the thing you
wanted gone.

---

## Related Documentation

- [chapter 09 — Home panel widgets](architecture/09-home-widgets.md) - Panel Widget System, including the
  `LV_OBJ_FLAG_USER_*` ledger that `PANEL_WIDGET_TILE_FLAG` is part of
- [LAYOUT_SYSTEM.md](LAYOUT_SYSTEM.md) - the home widget grid that sizes tiles
- [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md) - `lv_obj` defaults, and why
  `scrollable` is the one this project's theme does not override
- [HELIXCTL.md](HELIXCTL.md) - `ctl geom`
- `docs/user/guide/settings/display-sound.md` - the user-facing description
