# Writing a Lua Plugin

A HelixScreen plugin is a folder of Lua and XML that runs on the printer's screen. It can
add home-panel tiles, overlays and settings without compiling anything: an author needs a
text editor and the plugin folder, nothing else. This guide is the contract; every rule in
it is enforced by code, cited so it can be verified against the source.

Six complete plugins ship in the repository:

- `examples/plugins/temp-spark` - a heater sparkline tile. No permissions.
- `examples/plugins/led-effects` - an LED effect toggle. The `gcode` permission.
- `examples/plugins/maintenance-meter` - consumable hour meters as an adaptive
  tile. The `storage` permission; the step-by-step build of it is
  `docs/devel/PLUGIN_TUTORIAL.md`.
- `examples/plugins/print-watcher` - print-event notifications to ntfy,
  Discord, Telegram or a webhook. The `http` permission; the worked example
  for `helix.settings.set` as a control surface and for `i18n/` translation
  packs.
- `examples/plugins/ambient-tile` - room temperature and humidity from Home
  Assistant or any JSON endpoint. The `http` reading example: `http.get`
  against untrusted JSON, pointer resolution, a poll cadence, and a canvas
  sparkline from a plugin-side window.
- `examples/plugins/calibration-bridge` - slicer-side filament calibration at
  the printer, over Moonraker agent events. A proof of concept: no companion
  ships yet, the plugin implements the proposed contract alone. The first
  **overlay-only** example (`widgets: []`): the companion event is the front
  door, a settings action row the manual one; also the `moonraker_write`
  example (`printer.print.start`) and the worked use of an event-fed dropdown
  with `bind_value`.

## 1. What a plugin is

A folder named after the plugin id:

```
my-plugin/
  manifest.json     describes the plugin: id, name, widgets, settings, permissions
  main.lua          runs when the plugin loads
  ui/               XML components, one file per component
    my-plugin__tile.xml
```

The plugin runs in its own sandboxed Lua 5.4 state (`src/plugin/lua_runtime.cpp#LuaRuntime`).
It cannot freeze or crash the screen: each Lua entry gets a time budget, the state has a
memory cap, and repeated errors disable the plugin while the app keeps running
(§10 Limits and faults).

Deliberately out of scope: plugin-implemented capability backends (the binding design
leaves room; none is built), multi-instance plugin widgets, plugin-to-plugin calls, raw
`lv_obj` access, nav bar panels, and a plugin catalog, signing or install-from-URL (§3 covers sharing).

## 2. Quick start

Run HelixScreen from a checkout and point it at a local plugin folder:

```sh
cp -r examples/plugins/temp-spark examples/plugins/my-plugin
cd examples/plugins/my-plugin
# rename the id everywhere: the folder, manifest.json's "id", and every
# temp-spark__ name in manifest.json, main.lua and the ui/ file names
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

The `--test` flag runs a mock printer, so nothing touches a real machine. Then:

1. Settings > Plugins > Temperature Sparkline (your plugin's `name`) > enable. A plugin
   with no permissions asks nothing; one with permissions shows a consent dialog first.
2. Add the tile from the home panel's widget catalog, under the Plugins category. A tile
   needs free cells: remove or shrink a stock widget first if the page is full.

Saving any file of the plugin reloads it within about a second in native dev builds (the
same `HELIX_HOT_RELOAD` gate as XML hot reload; `HELIX_PLUGIN_DIR` documents it in
`ENVIRONMENT_VARIABLES.md`). Cross-compiled release builds do not poll. The one exception
is `i18n/*.xml`: a pack registers once per path, so an edited pack needs an app restart
(§13 Translations).

`HELIX_PLUGIN_DIR` replaces the printer's plugin source with your directory while it is
set, so the fastest loop is: edit, save, watch the log line naming your plugin reload.

## 3. Installing on a printer

Copy the plugin folder into the printer's Moonraker config:

```
printer_data/config/helixscreen/plugins/<id>/
```

The Mainsail or Fluidd file manager works; no restart is needed. The screen mirrors the
folder into a local cache on connect and whenever the files change, shows a toast when a
new plugin arrives, and never enables one on its own - the user enables it in
Settings > Plugins (`src/application/printer_session.cpp#on_plugin_sync`).

A plugin over a sync limit is skipped whole and keeps its previous installed version
(`include/plugin_source.h`): at most 128 files, 8 MB per plugin, 4 MB per file, and 32
plugins in one listing.

To share a plugin, or to find ones other people wrote, use the `#plugin-showcase` channel
on the [HelixScreen Discord](https://discord.gg/RZCT2StKhr). Plugins posted there come from
the community and are not reviewed by the HelixScreen project: read a plugin's
`manifest.json` permissions and its Lua before you enable it. There is no in-app catalog
(prestonbrown/helixscreen#1771).

## 4. Naming

A plugin id matches `^[a-z][a-z0-9-]{1,31}$`: lowercase, 2 to 32 characters, digits and
hyphens, never an underscore (`src/plugin/plugin_manifest.cpp#is_valid_plugin_id`). The
folder name must equal the id.

Everything a plugin registers is named `<id>__<rest>` with a double underscore
(`kPluginNameSeparator`, `include/plugin_manifest.h#kPluginNameSeparator`):

- XML component files: `ui/<id>__<name>.xml`, and the component's name inside
- widget ids and their components in the manifest
- subjects (Lua hands only the short `<name>`; see §8)
- object `name=` attributes in XML
- `plugin_event` handler names in `user_data`

One global XML and subject scope is shared by the app and every plugin. The app's own
names use single underscores (`extruder_temp`, `settings_*`); only the double underscore
proves a name belongs to a plugin, and a plugin can reach only its own names.

## 5. manifest.json

Parsed by `src/plugin/plugin_manifest.cpp#parse_manifest`. Any error in one field rejects
the whole manifest; Settings > Plugins shows the reasons.

Every user-facing string in the manifest - `name`, settings `label`s, widget
`name`/`description` - is English source: the file stays
English and the render sites translate it through the plugin's `i18n/` pack
(§13 Translations), falling back to the literal text.

| Field | Type | Default | Rules |
|---|---|---|---|
| `id` | string | required | the id pattern above; must match the folder name |
| `name` | string | required | single line, at most 48 bytes; shown in the UI |
| `version` | string | required | single line, at most 32 bytes |
| `author` | string | empty | single line, at most 64 bytes |
| `description` | string | empty | parsed and kept, but no screen displays it; the Plugins screen shows `name` and the plugin's status |
| `helix_version` | string | empty | a HelixScreen version constraint; a plugin whose constraint fails the running version shows as *incompatible* |
| `permissions` | array | `[]` | zero or more of `gcode`, `moonraker_write`, `http`, `storage` (§9) |
| `memory_mb` | integer | `2` | 1 to 64; the plugin's share of the plugin memory budget |
| `icons` | object | `{}` | up to 16 custom icons drawn from PNGs in the plugin folder (Custom icons, below) |
| `widgets` | array | `[]` | up to 32 home tiles |
| `settings` | array | `[]` | the settings schema (§6) |
| `settings_overlay` | string | empty | one of the plugin's own components, opened instead of the generated settings screen |
| `menu_items` | array | `[]` | actions the plugin adds to app menus (§8, Menu items) |

Each `widgets` entry:

| Field | Type | Default | Rules |
|---|---|---|---|
| `id` | string | required | `<id>__<name>`, unique in the array |
| `name` | string | required | the catalog entry's display name |
| `component` | string | required | `<id>__<name>`, a file in `ui/` |
| `icon` | string | `puzzle_outline` | an icon name from `include/ui_icon_codepoints.h`, or one of the plugin's `icons` (a `<id>__` name that is not declared rejects the manifest) |
| `description` | string | empty | catalog help text |
| `colspan`, `rowspan` | number | `1` | 1 to 8 grid cells; whole cells unless `half_cells` |
| `max_colspan`, `max_rowspan` | number | `0` | `0` = not resizable on that axis; otherwise `colspan` to 8 |
| `half_cells` | boolean | `false` | resizes in half cells, and the spans above may be written as halves (`1.5`) |

Widget tiles appear in the home panel's widget catalog under the Plugins category, never
enabled by default, one instance each. A tile's saved placement survives a disabled or
absent plugin and returns when it loads again.

### Custom icons

The `icons` object registers PNGs from the plugin folder as icons, so a widget's catalog
entry and the plugin's XML can show the same artwork:

```json
"icons": {
  "maintenance__lubrication": {"src": "icons/lubrication.png", "fallback": "water"}
},
"widgets": [{"id": "maintenance__home", "name": "Maintenance",
             "component": "maintenance__tile", "icon": "maintenance__lubrication"}]
```

```xml
<icon name="maintenance__glyph" src="maintenance__lubrication" size="lg" variant="primary"/>
```

| Field | Rules |
|---|---|
| key | the icon's name, `<id>__<name>` with the name 1 to 48 of `[a-z0-9_-]` |
| `src` | required: a `.png` path relative to the plugin folder, `/`-separated segments of `[A-Za-z0-9_.-]`, no `..`, at most 128 bytes |
| `fallback` | an app icon name from `include/ui_icon_codepoints.h`; default `puzzle_outline` |

Draw a single-color silhouette on a transparent background, square, for example 64 x 64:
the icon uses only the PNG's alpha channel, as a mask. It is scaled to fit the icon size's
square (aspect kept, centered) and painted with the icon's `variant` or `color`, exactly
like a built-in icon, so it follows theme and dark-mode changes. A PNG's own colors are
ignored.

A PNG that is missing, unreadable, over 64 KB or over 256 x 256 pixels, or that a symlink
places outside the plugin folder, draws the `fallback` icon instead and logs a warning
naming the icon; the plugin still loads. Icons are registered when the plugin loads and
withdrawn when it unloads, reloads or is disabled: anything still showing one then draws
its fallback (`src/ui/components/ui_icon_image.cpp#register_image_icon`). An XML `icon` whose `src`
carries the `__` separator must name one of the plugin's own icons (§7); `menu_items`
icons are app icon names only. That check reads literal values only: a `src` filled
through a component prop is not checked, so it keeps names tidy and is not a security
boundary (another plugin's icon is just artwork).

## 6. Settings

Each `settings` entry has `key` (matching `[a-z0-9_]{1,64}`), `label` and `type`, and
becomes one row of the plugin's settings screen
(`src/plugin/plugin_settings_overlay.cpp#setting_row_spec`):

| Type | Extra fields | Row |
|---|---|---|
| `bool` | `default` | toggle |
| `int` | `min` < `max`, `default` in range | slider, whole numbers |
| `float` | `min` < `max`, `default` in range | slider, value scaled by 100 |
| `enum` | `options`: non-empty array of strings without newlines; `default` one of them | dropdown |
| `string` | `default`, a string | text input; a stored value over 1024 bytes reads as the default |
| `action` | `callback`: `<id>__<name>` of a `helix.ui.on` handler | button that fires the handler |
| `info` | `subject`: `<id>__<name>` of a plugin subject | read-only row bound to the subject |

In Lua, `helix.settings.get(key)` returns the stored value if it still fits the
declaration, else the default (`src/plugin/lua_bind_io.cpp#effective_setting`).
`helix.settings.set(key, value)` writes one of the plugin's own declared settings
through the same validation, persistence and `on_change` notification the generated
settings screen uses, so a plugin's own overlay can be the control surface; an
undeclared key, a read-only `action`/`info` row, or a value the declaration rejects
(min/max, enum membership, string size) raises.
Setting the value a key already holds is no change: nothing is saved and no
`on_change` handler runs, so a handler that sets its own key back does not recurse.
An accepted change reaches its handlers before `set` returns, but the disk write is
deferred: about 0.5 s later, one write per burst, and on unload or shutdown, the same
schedule as `helix.storage.set` (`src/plugin/lua_bind_io.cpp#set_plugin_setting`).
`helix.settings.on_change(key, fn)` calls `fn(value)` after each accepted change, from
either path; several handlers per key are allowed. An `action` row reaches its handler
through the same path as a `plugin_event`.

Enable state and settings are shared across printers: enabling a plugin once enables it
on every printer that ships it, under the same permission check.

**Validate any setting that reaches G-code.** A stored string is whatever was typed. The
led-effects plugin anchors and bounds its effect name before building a command line
(§12): anchor the match (`^...$`), bound the length, and refuse rather than send.

## 7. XML

Each `ui/<id>__<name>.xml` file defines one component and is registered under its file
stem. The whole file passes `src/plugin/plugin_xml_policy.cpp#check_plugin_xml` before
anything is registered; a rejection fails the load and Settings > Plugins shows the
reason with the file name.

Allowed elements (`src/plugin/plugin_xml_policy.cpp#is_allowed_element`):

- `lv_*` widgets (`lv_label`, `lv_bar`, `lv_obj`, and so on)
- the structural `component`, `view`, `api`, `prop`
- `style`, `remove_style*`, `play_timeline_event`, `bind_*` elements
- `event_cb`
- the app chrome widgets `overlay_panel`, `ui_card`, `icon` (its `src` an app icon name or
  one of the plugin's own `icons`, §5), `text_heading`,
  `text_body`, `text_muted`, `text_small`, `text_xs`, `text_tiny`
- the app dialog chrome `ui_dialog`, `modal_header` and `modal_button_row`, for a
  component opened with `helix.ui.dialog` (rules below)
- `consts` and, inside it only, the value elements `px`, `int`, `percentage`, `color`
  and `string` (see Responsive sizing)
- `plugin_canvas`, the plugin drawing surface (below)
- `plugin_table`, a data table filled from Lua (below and §8, helix.ui.table)
- the plugin's own components, by name

A `view` may `extends` only another allowed name. `screen_load_event` and
`screen_create_event` are never available. The `subject_*_event` elements are rejected
outright: they hold a raw subject pointer with no observer, and `plugin_event` plus
`s:set()` in Lua reaches the same effect by name.

Attribute rules (`src/plugin/plugin_xml_policy.cpp#check_plugin_attr`):

- The only callback is `plugin_event`: any `callback`, `event_cb`, `*_callback` or `*_cb`
  attribute must read `plugin_event`.
- `user_data` carries the target: `<id>__<name>` optionally followed by `:arg`. The host
  runs the `helix.ui.on` handler `name` with `arg` as a string, or `nil` without a colon.
  On `modal_button_row` and `modal_header`, a prop ending in `_user_data`
  (`primary_user_data`) is a target too and follows the same rule.
- Every subject reference (`subject`, `*_subject`, and any `bind_*` attribute) must name a
  subject this plugin owns, spelled `<id>__<name>`.
- Object `name=` attributes must be `<id>__<name>`, so a screen-wide lookup never
  resolves a plugin object. On `modal_button_row` and `modal_header`, so must every
  prop ending in `_name`, which is how they name their objects (`primary_name`). On
  your own components, `_name` and `_user_data` props are ordinary props.
- `modal_button_row` and `modal_header` name their objects with app names by default
  (`btn_primary`, `header_title`), so a plugin must pass every one of their name props,
  owned: `primary_name`, `secondary_name`, `tertiary_name`, `quaternary_name`,
  `tertiary_divider_name` and `quaternary_divider_name` on the button row; `icon_name`,
  `title_name` and `close_name` on the header. A missing one fails the load with the prop
  named.
- A `$`-prop indirection on any of these attributes is refused.
- `ui_breakpoint`, `ui_breakpoint_v` and `ui_is_portrait` are readable as `subject=` on
  a `bind_flag_if_*` or `bind_style_if_*`, and as identifiers in `cond`; nowhere else
  (not in `bind_value`, `bind_text` or any other binding).
- `cond` and `*_cond` expressions may name only the plugin's own subjects and the three
  layout subjects above. Identifiers in
  the expression grammar are `[A-Za-z_][A-Za-z0-9_]*`, and every owned name starts with
  the id, so **a plugin whose id contains a hyphen cannot use `cond` at all**. Use
  `bind_flag_if_eq` (and the other `bind_flag_*` elements) instead; led-effects does.

Because every `bind_*` attribute value must be an owned subject name, `bind_text-fmt` is
rejected: a plugin-supplied printf format bound to a subject is a format-string hazard.
Format in Lua and set a string subject.

A component prop must be declared inside an `<api>` block to exist, and
`helix.ui.overlay(component, attrs)` fills those props at open time - the way
data rides into an overlay that XML cannot build dynamically (the options of
a dropdown, for instance: `examples/plugins/calibration-bridge`). A
dropdown's `bind_value` is the way a selection comes back: it is a two-way
subject binding, so the pick lands on a plugin-owned int subject without any
widget access.

Styling uses the app's design tokens, for example `style_pad_all="#space_sm"`,
`style_text_color="#text"`, `style_text_font="#font_heading"`
(`docs/devel/UI_CONTRIBUTOR_GUIDE.md` has the full token list).

**Font tokens are base tokens only**: `font_xs`, `font_small`, `font_body`,
`font_body_bold`, `font_heading`, `font_xl`, `font_display`, `font_mono`. A size-suffixed
variant (`font_heading_medium`, `font_body_large`) names a face the theme registers only
from its breakpoint tier upward, so on most screens the XML engine silently substitutes the
default font - a trap an author cannot see on an oversized dev display. The policy rejects
a suffixed token on `style_text_font`, and `helix.canvas` text enforces the same rule
(`src/ui/theme/theme_fonts.cpp#theme_manager_font_token_is_base`).

### Responsive sizing

A plugin component sizes itself the way an app component does.

- **App tokens** (`#space_md`, `#font_heading`, the `#dialog_content_*` height ladder,
  `UI_CONTRIBUTOR_GUIDE.md` §3) already resolve to the screen's breakpoint.
- **Your own constants** go in a `<consts>` block in any of your `ui/` files, named
  `<id>__<name>`, and every one of your components can use them as `#<id>__<name>`:

  ```xml
  <consts>
    <px name="my-plugin__card_w_small" value="200"/>
    <px name="my-plugin__card_w_medium" value="260"/>
    <px name="my-plugin__card_w_large" value="320"/>
    <px name="my-plugin__gap" value="6"/>
  </consts>
  ```

  A `px` constant with a `_small`/`_medium`/`_large` set resolves to the screen's tier on
  the narrow axis, exactly as an app token does, with the same optional tiers and
  fallbacks (`_micro`, `_tiny`, `_xlarge`, `_xxlarge`) and the same DPI scaling; reference
  it by the base name (`#my-plugin__card_w`). A partial set (`_small` and `_large` without
  `_medium`) fails the load with the constant and the missing tier named. `int`,
  `percentage`, `color` and `string` constants, and `px` without a tier suffix, register
  as written. The resolver is the app's own
  (`helix::theme_manager_resolve_px_tables` in `src/ui/theme/theme_responsive.cpp`), and it runs
  again when the screen changes size, so a component created afterwards gets the new tier.
- **Layout switches** read `ui_breakpoint` (narrow axis), `ui_breakpoint_v` (vertical) or
  `ui_is_portrait` in a conditional bind, for example
  `<bind_flag_if_lt subject="ui_breakpoint" flag="hidden" ref_value="2"/>` hides an
  object below the small tier.

### plugin_canvas

`<plugin_canvas name="<id>__<name>" .../>` is the plugin drawing surface
(`src/plugin/plugin_canvas.cpp#canvas_xml_create`): an empty object that replays whatever
display list Lua last committed under that name (§8). Its `name=` follows the owned-name
rule and is the registry key the drawing publishes under. It defaults to 100% width and
100% height of its parent, is not scrollable, and is not clickable, so a canvas inside a
clickable tile does not swallow the tile's tap; set `width`/`flex_grow` to shape it like
any other child. A canvas with no committed list draws nothing.

### plugin_table

`<plugin_table name="<id>__<name>" .../>` is a scrollable data table
(`src/plugin/plugin_table.cpp#table_xml_create`) whose columns and rows come from
`helix.ui.table` (§8). Its `name=` follows the owned-name rule and is the key its data
publishes under: every live `plugin_table` with that name shows the same data, and one
created later shows it at once. It defaults to 100% width and height of its parent; set
`width`, `height` or `flex_grow` like any other child. Cells are padded by `#space_sm`.
The first row is a header of column titles.
Cells use the theme's body font. Colors come from the theme at draw time (text, muted
header text, separators, and the selected row in the primary color), so a theme switch is
just the next repaint.
A table with no data shows an empty header.

## 8. The Lua API

Every entry - the top level of `main.lua`, each handler, each timer callback - runs as
its own coroutine. Async calls yield and come back on the main thread, returning `value`
or `nil, err`; synchronous errors raise Lua errors, which count toward the error limit.
An async call is legal at the top level of `main.lua` and inside any function the runtime
calls, and nowhere else: not in a metamethod, a table iterator, a sort comparator, the
top level of a `require`d module, or a coroutine the plugin creates itself
(`src/plugin/lua_runtime.cpp#await_async`).

### helix.log (`src/plugin/lua_bind_core.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.log.debug(msg)` | | also `info`, `warn`, `error` |
| `print(...)` | | the sandbox's `print` logs at info with the plugin tag, one line capped at 4096 bytes |

### helix.subject (`src/plugin/lua_bind_ui.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.subject.int(name[, init])` | handle | `name` is 1-48 chars of `[a-z0-9_-]`; registered as `<id>__<name>`; at most 128 subjects per plugin |
| `helix.subject.string(name[, init])` | handle | strings are at most 1023 bytes |
| `s:get()` | number or string | |
| `s:set(v)` | | int range is int32; a string over 1023 bytes is an error |
| `s:observe(fn)` | | `fn(value)` on each change, not for the current value; observers and printer watches share a quota of 256 per plugin |

Subjects are the plugin's whole output: XML binds to them, Lua sets them.

### helix.ui (`src/plugin/lua_bind_ui.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.ui.on(name, fn)` | | registers the handler a `plugin_event` or an `action` setting row fires; `name` is 1-48 chars of `[a-z0-9_-]`; `fn(arg)` receives the `user_data` argument string or `nil` |
| `helix.ui.toast(msg[, severity])` | | `severity` is `"info"` (default), `"success"`, `"warning"` or `"error"` |
| `helix.ui.confirm(title, msg[, opts])` | | `opts.severity` (`"info"`, `"warning"`, `"error"`), `opts.confirm_text` (default `"OK"`), `opts.on_confirm`, `opts.on_cancel`; a dismissed dialog runs `on_cancel` too; at most one shown or waiting per plugin; waits its turn like every plugin surface (below); not during `on_unload` |
| `helix.ui.overlay(component[, attrs])` | handle | opens one of the plugin's own components full-screen; requesting a component the plugin already has shown or waiting returns that handle and changes nothing (it keeps its attrs and callbacks); `attrs` is a table of string attributes passed to the component plus optional `on_open` and `on_close` functions; not during `on_unload` |
| `helix.ui.dialog(component[, attrs])` | handle | opens one of the plugin's own components as a dialog: a centred card over a dimmed backdrop, dismissed like any app dialog. Build it from `ui_dialog`, `modal_header` and `modal_button_row` (§7) so it looks like one. `attrs` as for `overlay`; at most one dialog shown or waiting per plugin; not during `on_unload` |
| `helix.ui.edit_number(opts)` | | opens the app's numeric keypad for one value (Editors, below) |
| `helix.ui.edit_text(opts)` | | opens a text dialog with the app's keyboard for one line of text (Editors, below) |

A handle from `overlay` or `dialog` has `close()` and `is_open()` (both also callable as
`h:close()`). `close()` closes a shown surface or withdraws one still waiting.
`is_open()` is true only while it is on screen.

`on_open()` runs when the surface actually appears. `on_close(how)` runs exactly once,
however it ended, with `how` one of:

| `how` | Meaning |
|---|---|
| `"user"` | the user dismissed it: backdrop tap, back or ESC, navigating back from an overlay |
| `"plugin"` | the plugin called `close()` after it appeared. A button in your own component reaches your `helix.ui.on` handler, which records the answer and calls `close()`, so an answer reports `"plugin"` |
| `"app"` | the app closed it: a remote-control sweep, or a hot reload of the component |
| `"cancelled"` | the plugin called `close()` while it was still waiting; it never appeared |

Unloading the plugin (including a printer switch, which reloads every plugin) closes
and withdraws everything it has shown or waiting and runs none of these callbacks.

**Presentation order.** A plugin surface never covers an app dialog. While one is
showing (an error, a warning, the print-complete dialog, the PIN pad, the lock screen,
the abort or busy screen), every plugin dialog, overlay and confirm waits, in the order
requested across all plugins, and appears when the screen is clear. An app dialog always
opens on top of a plugin surface, and the plugin surface is still there when it closes.
Plugin dialogs from different plugins show one at a time, but a plugin never waits on
its own dialog: its confirm or overlay opens over it at once. Presenting wakes the
display; if waking brings up the lock screen, the surface waits until it is unlocked.
`src/plugin/plugin_presenter.cpp#decide` is the rule. To ask something when a print
finishes, watch `print_state` and open a dialog when it reads `"complete"`; it appears
once the user has dealt with the print-complete dialog (`examples/plugins/calibration-bridge`).
| `helix.ui.text_width(str[, font])` | integer px | width `str` draws at in the live theme font, measured the way `c:text` draws it (no letter spacing, one line); `font` is a base token without the `font_` prefix (default `"body"`), and a size-suffixed or unknown token is an error, as for canvas text; `str` is at most 1024 bytes. Call it where the layout is decided (a widget hook, a canvas redraw), since the theme font changes with the display tier |

### Editors: the keypad and the keyboard

`helix.ui.edit_number` and `helix.ui.edit_text` let the user enter one value with the
app's own input: the numeric keypad every temperature and speed field uses (with the
user's Number Pad Layout), and the app keyboard in a small dialog. The plugin hands over
the starting value and its constraints and gets back the accepted value or a cancel.

```lua
helix.ui.edit_number({
    title = "Interval", value = interval, min = 1, max = 1000, decimals = 0, unit = "h",
    validate = function(v)
        if v % 10 ~= 0 then return false, "Use a multiple of 10 hours" end
        return true
    end,
    on_accept = function(v) interval = v; save() end,
})
helix.ui.edit_text({title = "Task name", value = name, max_length = 32,
                    on_accept = function(s) name = s end})
```

| Option | `edit_number` | `edit_text` |
|---|---|---|
| `title` | header text, at most 47 bytes | dialog heading, at most 1024 bytes |
| `value` | starting value (the keypad opens empty, as everywhere in the app) | starting text, no longer than `max_length` |
| `min`, `max` | required; `-99999 <= min < max <= 99999` | |
| `decimals` | `0` (default), `1` or `2`: digits after the decimal point; `0` hides the point and hands back an integer | |
| `unit` | shown inside the display, at most 15 bytes | |
| `placeholder` | | hint shown while the field is empty |
| `max_length` | | 1 to 1023 characters, default 256 |
| `password` | | `true` masks the text |
| `on_accept(value)` | required | required |
| `on_cancel()` | optional | optional |
| `validate(value)` | optional | optional |

Any other key is an error. A `min` below 0 adds a +/- key beside the keypad's display,
which flips the sign of what has been typed; a range at or above zero shows no sign key. A
value outside `min`..`max` is refused by the keypad itself with the range in an error toast,
before `validate` sees it.

`validate(value)` runs when the user presses Set or OK. Return `true` to accept, or `false`
and a message (at most 256 bytes) to keep the editor open with that message in an error
toast. It runs under the menu-check rules (§8, Menu items): synchronous, a short time
budget, and no side effects, so `helix.ui.toast`, `subject:set`, storage writes and every
async call raise inside it. A validator that raises, overruns or returns anything but
`true` rejects the value with "Invalid value"; the error counts toward the plugin's error
limit like a failed check. The log names it an "editor validator" (for example
"helix.ui.toast is not allowed in an editor validator").

`on_accept(value)` runs once with the accepted value, after the editor has closed.
`on_cancel()` runs once for every other ending: Back, Cancel, a tap on the backdrop, ESC,
or the editor failing to open (the keypad already open for someone else). A plugin has at
most one editor shown or waiting; another call while one is open raises. Editors wait their
turn behind app dialogs like a confirm (§8, Presentation order). The keypad is an overlay
and a plugin dialog sits above overlays, so `edit_number` raises while the plugin has a
dialog shown or waiting: close it first, or open the keypad from an overlay or the
settings screen. `edit_text` opens over the plugin's dialog. Neither may be called during
`on_unload` or in a menu check. Unloading the plugin closes its editor and runs neither
callback.

### Tables

`helix.ui.table(name, spec)` defines the data of the `plugin_table` named `<id>__<name>`
(§7) and returns its handle. `name` is 1-48 of `[a-z0-9_-]`. Calling it again with the
same name returns the same handle with the new columns and `on_activate`; the rows and
the selection are dropped.

```lua
local tasks = helix.ui.table("tasks", {
    columns = {{title = "Task", width = 50}, {title = "Interval"}, {title = "Since"}},
    on_activate = function(key) helix.ui.overlay("maint__task", {task = key}) end,
})
tasks:set_rows({
    {key = "rails-lubrication", "Lubricate rails", "100 h", "120 h"},
    {key = "belts", "Check belts", "250 h", "180 h"},
})
```

| Call | Returns | Notes |
|---|---|---|
| `helix.ui.table(name, spec)` | handle | `spec.columns`: 1 to 8 tables with `title` (a string, at most 256 bytes) and an optional `width`, a whole percent from 1 to 100 of the table's width; the widths add up to at most 100 and the columns without one share what is left. `spec.on_activate(key)` is optional |
| `t:set_rows(rows)` | | replaces every row; `rows` is a list of at most 500 tables, each a `key` (a string of 1 to 128 bytes, unique in the list) and up to one cell per column as positional strings or numbers of at most 256 bytes; a missing cell is empty. `{}` empties the table |
| `t:selected()` | key or `nil` | the selected row's key |
| `t:select(key)` | boolean | selects the row with `key`, or clears the selection for `nil`; `false`, changing nothing, when no row has `key`. Selecting is not a tap: `on_activate` does not run |

A tap on any cell of a data row selects that row and runs `on_activate(key)` with the key
of the row it showed, so a row that moved because others were added or removed still
answers with its own key. A tap on the header row activates nothing, and neither does a
scroll: the table scrolls natively and only a press released without scrolling counts.
When `set_rows` replaces the rows, the selection stays on its key if a row still has it
and clears if none does; it never moves to whatever took the old position. Validate the
key against your data before acting on it.

Rows and columns count against the plugin's `memory_mb` like canvas lists: a `set_rows`
that would cross the cap raises "table:set_rows: plugin memory budget exceeded" and keeps
the rows it had. Unloading the plugin empties its tables and taps then reach nothing.
`set_rows`, `select` and `helix.ui.table` are refused in a menu check; `selected` is a read.

### Menu items

A plugin can add actions to four app menus. Each item names one of the plugin's
`helix.ui.on` handlers; tapping it closes the menu and calls that handler with a table
describing what the menu was opened on.

| `menu` | Where it appears | Opened on |
|---|---|---|
| `print_file` | the dots button (`dots_vertical`) on the print details screen (Print select, a file opened) | the file shown |
| `filament_slot` | the long-press menu of a filament slot, any filament system, in a column of its own beside Filament and Spool | the slot |
| `spool` | the Spoolman spool menu, after the app's items | the spool |
| `tile` | the configure (gear) button of the plugin's own home tile, in home edit mode | the tile |

The external spool's menu takes no plugin items, and neither does the print file list's
long-press (Delete). No other app menu takes them. A plugin item can only add a row: it
cannot hide, reorder or relabel the app's own items, and tapping it only calls the
plugin's own handler with plain data.

Declared in `manifest.json`:

```json
"menu_items": [
  {"id": "calibrate", "menu": "print_file", "label": "Calibrate with this file",
   "icon": "tune", "handler": "calibrate", "check": "can_calibrate"}
]
```

| Field | Rules |
|---|---|
| `id` | unique within the plugin |
| `menu` | `print_file`, `filament_slot`, `spool` or `tile`; a `tile` item needs at least one declared widget |
| `label` | English source, translated through the plugin's `i18n/` pack |
| `icon` | an icon name from `include/ui_icon_codepoints.h`, optional (`puzzle_outline` when omitted); a name not in that table rejects the manifest, since the icon widget would draw a broken-image glyph for it |
| `handler` | a `helix.ui.on` handler name, required |
| `check` | a `helix.ui.on` handler name, optional (below) |

`handler` and `check` are local names, the same 1-48 chars of `[a-z0-9_-]` that
`helix.ui.on` takes. A malformed item rejects the manifest with the field named. Per-menu limits (items per
plugin, and plugin rows per menu so a menu never scrolls) are in §10.

**The handler** receives one table:

| `menu` | Fields |
|---|---|
| `print_file` | `source` (`"printer"` or `"usb"`), `path` (relative to the gcodes root; `nil` for a USB file), `local_path` (USB files only), `name`, `estimated_time_s`, `filament` (a list, one per tool: `tool`, `material`, `color`, `grams`), `layer_height`, `object_height`, `tools_used` |
| `filament_slot` | `unit`, `slot` (both 0-based), `label` (the slot's name as the menu shows it), `tool` (the mapped tool or `nil`), and what is loaded when known: `material`, `color` (`"#RRGGBB"`; `nil` when the slot reports the app's default grey `#808080`, which stands for an unknown colour, so a grey spool reads as unknown), `brand`, `spool_id` (Spoolman). An empty slot carries none of these four |
| `spool` | `spool_id`, `material`, `color` (`"#RRGGBB"`), `brand`, `remaining_g` (`nil` when Spoolman reports no remaining weight or 0 g: the two cannot be told apart) |
| `tile` | `widget` (the widget's name: its declared id without the `<id>__` prefix) |

A field the printer does not report, or that has not loaded yet, is `nil`: the print
details screen fetches some file metadata after it opens. The handler is an ordinary
plugin entry, with the same time budget, fault rules and permissions as any
`helix.ui.on` handler; it may open a dialog or overlay, or call Moonraker within the
plugin's permissions.

```lua
helix.ui.on("calibrate", function(file)
  helix.ui.dialog("my-plugin__start", {path = file.path or file.local_path})
end)
```

**The check.** An item without `check` always shows, enabled. With `check`, the named
handler is called with the same table when the menu opens and returns `true` (shown),
`false` (hidden), or `{visible = true, enabled = false, reason = "Only for .gcode files"}`
(shown greyed, with the reason). In the table, an absent `visible` or `enabled` means
`true` and `reason` is optional; the reason shows under the label whenever it is set. The
result is read raw: its fields must be plain booleans and a string, and a table carrying
a metatable is not a valid result (`src/plugin/plugin_host.cpp#read_check_value`).

A check runs in check mode (`src/plugin/lua_runtime.cpp#call_sync`): anything with
a side effect or a wait raises "`<call>` is not allowed in a menu check" before acting.
That covers every async call (`helix.gcode`, `helix.moonraker`, `helix.http`,
`helix.sleep`), `helix.storage.set`, `helix.settings.set`, `subject:set`, opening a
dialog, overlay, confirm, editor or toast, playing a sound, changing a table, closing a surface, `helix.canvas` and every canvas
method (a check never draws), creating a subject, `helix.widget`, starting or cancelling
a timer, registering a handler, observer, watch or subscription, and `require` (a module's
top level is ordinary entry work, and a refusal inside it would cache a half-loaded
module). Reading subjects, printer fields, settings, storage and the plugin's own Lua
state is fine. The check's coroutine is closed inside check mode and its budget, so a
pending `<close>` handler runs under the same rules. A check has its own short time
budget, separate from the entry budget. A check that raises, overruns, yields, returns
nothing or returns anything else hides only that item for this opening and counts toward
the plugin's error limit (§10), never as a fault by itself; the menu always opens.

```lua
helix.ui.on("can_calibrate", function(file)
  if file.source == "usb" then
    return {visible = true, enabled = false, reason = "Copy the file to the printer first"}
  end
  return true
end)
```

**What the user sees.**
- On print details, the bottom row becomes three icon buttons (Delete, the dots button,
  Print) once any enabled plugin declares a `print_file` item; for a USB file, where
  Delete is hidden, it is the dots button and Print. Whether the button shows is decided
  from the declarations alone, because the file's time, height and layer metadata arrive
  after the screen opens; the checks run when it is tapped, against the context as it
  stands then. When every item is hidden for that file the menu says "No plugin actions
  for this file". Without such a plugin the row is unchanged, with Print's full label.
- In the filament slot menu, plugin items get their own column (`col_plugins` in
  `ui_xml/ams_context_menu.xml`) beside Filament and Spool, each plugin's items headed
  with its name; the column is hidden when no item shows, so the menu is then unchanged.
  The card already scrolls at 480x272 without plugin rows, so the column adds width, never
  height: it is a fixed width per screen size (110 px at micro, 120 at tiny, 160 at small,
  200 at medium, 220 at large; the `ams_context_plugin_col_w` tokens) and a longer label
  ellipsizes. The row cap of 5 (§10) is the most that keeps the column no taller than the
  Filament column at 480x272. The Tool and Endless Spool dropdowns stay below the columns.
- In the spool menu plugin items sit under a divider after the app's items, headed the
  same way. In every menu plugins are in load order and items in manifest order; a plugin
  cannot choose its position among other plugins.
- A tile with `tile` items gets the gear in home edit mode (decided from the declaration,
  no check runs to show it); its items open in a menu below the tile, and only that
  plugin's items appear there. When every one is hidden the menu says "No actions right
  now".
- Settings > Plugins lists "Adds to: ..." (Print details, Filament slot menu, Spool menu,
  Its home tile) for a plugin with menu items, in the enable prompt after the permission
  lines and on the plugin's row. The line is informational: menu items need no permission
  of their own, since a handler can do only what the plugin's granted permissions allow,
  so adding or changing them in an update does not ask again.

Disabling, faulting or unloading a plugin removes its items at once; a tap that lands
after that does nothing.

### helix.sound (`src/plugin/lua_bind_sound.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.sound.play(name[, priority])` | `true` or `false` | plays one cue of the user's active sound theme through the app's sound manager; `priority` is `"ui"` (default) or `"event"` |

`name` is one of `button_tap`, `toggle_on`, `toggle_off`, `nav_forward`, `nav_back`,
`dropdown_open`, `print_complete`, `print_cancelled`, `error_alert`, `error_tone` or
`test_beep`. The boot chime (`startup`) and the urgent alarm (`alarm_urgent`) are the
app's own, and so is the `"alarm"` priority, which nothing interrupts: an unknown name or
priority raises.

The user's settings decide what is heard, exactly as for the app's own sounds: nothing
plays with Sounds off, the first six names are UI sounds and stay silent with UI sounds
off, a theme without the cue plays nothing, and a printer without a speaker plays
nothing. An `"event"` cue is interrupted only by an app alarm; a `"ui"` cue by anything.

Request a cue once per event, not on every status update. A plugin's cues at one priority
are at least 150 ms apart (`"ui"`) or 1 s apart (`"event"`); a cue inside that gap is
dropped and the call returns `false`. `false` also means the screen has no sound path at
all. `true` says the cue was handed to the sound manager, not that it was heard. A cue is
fire-and-forget: nothing calls back, so unloading the plugin leaves nothing pending.

```lua
helix.ui.on("job_done", function() helix.sound.play("print_complete", "event") end)
```

### helix.widget (`src/plugin/lua_bind_widget.cpp`)

`helix.widget(id, hooks)` registers hooks for one declared widget; `id` is the short name
after `<id>__`, and the widget must be in the manifest's `widgets` array. Hooks, all
optional:

| Hook | Called with |
|---|---|
| `on_attach()` | the tile's tree enters the home panel |
| `on_detach()` | the tile's tree leaves it |
| `on_size(cols, rows, w, h)` | `cols`/`rows` in cells (a `half_cells` tile can get `1.5`), `w`/`h` in pixels |
| `on_activate()` / `on_deactivate()` | the page holding the tile is shown / hidden |

### helix.canvas (`src/plugin/lua_bind_canvas.cpp`)

`helix.canvas(name)` returns a handle for one drawing surface. `name` is the short name
after `<id>__` (like a subject name) and must match a `plugin_canvas` element's `name=` in
the plugin's XML (§7). One canvas name is one drawing: every live `plugin_canvas` with
that name shows the same committed list, so give a tile canvas and an overlay canvas
different names. Calling `helix.canvas("x")` again returns a handle onto the same canvas.
There is no fixed number of canvases: each one, its name and its lists count against the
plugin's `memory_mb` together with its Lua heap. An empty name is an error.

Drawing is staged, then published:

- The draw methods append to a list being built. `c:commit()` publishes that list to every
  live widget with the name and starts a new empty list; committing an empty list blanks
  the canvas. `c:clear()` discards the list being built without publishing.
- Coordinates are content-box pixels with the origin at the widget's top-left corner:
  finite numbers within +-16384. Stroke widths and `border_width` are 0 to
  64. Angles are degrees.
- Colors are the XML color tokens without the `#` (`"primary"`, `"text_muted"`, ...); an
  unknown color token is an error. Fonts are base tokens without the `font_` prefix
  (`"body"`, `"heading"`, ...); the base-token rule in §7 applies and is enforced by the
  same check.
- Every options table accepts only the keys listed for its call; an unknown key is an
  error. Omitted options use the defaults in the table.
- Each staged primitive charges the plugin's memory cap for the exact bytes it adds
  (`include/plugin_canvas.h#DisplayList/bytes`), and a new canvas charges its entry and
  name. A call that would cross the cap raises "helix.canvas: plugin memory budget
  exceeded" and adds nothing; the plugin stays loaded. `commit()` moves the charge onto
  the published list and releases the bytes of the list it replaces (an empty commit
  releases it all); `clear()` releases the pending charge; unload releases everything.

| Call | Options | Defaults |
|---|---|---|
| `c:line(x1, y1, x2, y2[, opts])` | `color`, `width`, `opa` | `"text"`, 1, 100 |
| `c:polyline(points[, opts])` | `color`, `width`, `opa`, `fill`, `fill_opa`, `baseline` | `"text"`, 1, 100, -, 100, - |
| `c:rect(x, y, w, h[, opts])` | `fill`, `border`, `border_width`, `radius`, `opa` | -, -, 1, 0, 100 |
| `c:circle(cx, cy, r[, opts])` | `fill`, `border`, `border_width`, `opa` | -, -, 1, 100 |
| `c:arc(cx, cy, r, a1, a2[, opts])` | `color`, `width`, `opa` | `"text"`, 1, 100 |
| `c:text(x, y, str[, opts])` | `font`, `color` | `"body"`, `"text"` |

To fit or center canvas text, measure it with `helix.ui.text_width(str, font)` using the same
`font` the `c:text` call names.

`opa` is the primitive's alpha as a percent, 0 to 100 (default 100, fully opaque); it
covers a stroke, a fill and a border alike.

`polyline` fills the area between the line and `baseline` (a y coordinate, drawn with
`fill` at `fill_opa` percent) as one continuous shape: the fill's edge follows the line
instead of stepping at the samples. `fill` and `baseline` are one option in practice and
neither is accepted alone. The fill's triangles are derived at draw time from the
polyline's own points, so it adds no units to the cap.

`points` is one flat array `{x1, y1, x2, y2, ...}`: an even-length array of at least two
points. A polyline counts each of its points toward the list's 4096-unit cap (§10); every
other primitive counts one. `rect` and `circle` need `fill` or `border` (either alone is
enough), and `radius` rounds a rect's corners. `str` is at most 256 bytes.

`c:size()` returns the content size as two values `w, h`, both `0` before the widget is
laid out. `c:on_size(fn)` registers `fn(w, h)` for each size the handler has not been
told: once after first layout and again on every real resize, coalesced when resizes
arrive faster than the main loop drains them. `c:on_size(nil)` removes the handler.
Rebuild the drawing for the new size inside the handler.

A commit publishes a retained list: the widget replays it in its own draw event, so Lua
never runs during rendering, and tokens resolve at draw time, so a theme switch is just
the next repaint. Rebuild the whole list for a new state; a list cannot be patched in
place.

### helix.printer (`src/plugin/lua_bind_printer.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.printer.get(name)` | value or `nil` | `nil` before the app subject exists; an unknown name is an error |
| `helix.printer.watch(name, fn)` | | `fn(value)` on each change, not for the current value; a watch lives until the plugin unloads, there is no unwatch |

Fields (`src/plugin/lua_bind_printer.cpp#printer_fields`):

| Name | Type |
|---|---|
| `connected` | boolean: the Moonraker connection |
| `klippy_state` | string: Klipper's own state, `"ready"`, `"startup"`, `"shutdown"` or `"error"`; `nil` while `connected` is `false` |
| `print_state` | string |
| `progress` | integer (percent) |
| `filename` | string |
| `extruder_temp`, `extruder_target` | degrees C |
| `bed_temp`, `bed_target` | degrees C |
| `chamber_temp`, `chamber_target` | degrees C |

The table is the contract: these Lua names stay stable while the subjects behind them can
change. No permission is needed for either call.

`connected` and `klippy_state` answer different questions. Klipper can restart, shut down
or lose its MCU while Moonraker stays connected, so `connected` stays `true` through all
of it; `klippy_state` is what tells you Klipper itself is gone. A Klipper that disconnects
from Moonraker reads `"shutdown"` until it reports again. Anything other than `"ready"`
means status values may be stale and commands will fail. While Moonraker itself is
disconnected nobody knows Klipper's state, so `klippy_state` reads `nil`; watch `connected`
to learn when that starts, since losing Moonraker does not change `klippy_state` by
itself. You do not need to re-query on the way back: every subscription gets a fresh
snapshot when Klipper returns to ready (below).

```lua
helix.printer.watch("klippy_state", function(state)
    if state ~= "ready" then show_unavailable() end
end)
```

### helix.moonraker and helix.gcode (`src/plugin/lua_bind_moonraker.cpp`)

| Call | Permission | Returns | Notes |
|---|---|---|---|
| `helix.moonraker.query(objects)` | none | table or `nil, err` | `objects` maps a status object name to `true` (every field) or a list of field names |
| `helix.moonraker.call(method[, params])` | none for the read-only methods below, else `moonraker_write` | value or `nil, err` | the app's print, restart and G-code guards can refuse a write with `nil, err` (below) |
| `helix.moonraker.upload(root, path, content)` | `moonraker_write` | `true` or `nil, err` | writes a file through Moonraker; `root` is `"gcodes"` or `"config"` |
| `helix.moonraker.download(root, path)` | `moonraker_write` | string or `nil, err` | `root` is `"gcodes"` or `"config"`; a body over the memory cap comes back as an error, not a fault |
| `helix.moonraker.on_agent_event(event, fn)` | none | | `fn(agent, data)` when Moonraker posts that agent event; the channel to a companion process on the printer (a slicer-side calibration wizard, an OrcaSlicer plugin); at most 16 handlers per plugin |
| `helix.moonraker.subscribe(objects, fn)` | none | handle | live status, see below |
| `helix.gcode(script)` | `gcode` | `true` or `nil, err` | runs one G-code script |

Read-only methods callable through `helix.moonraker.call` without any permission
(`src/plugin/plugin_permissions.cpp#is_readonly_moonraker_method`):
`printer.objects.query`, `printer.objects.list`, `server.info`, `server.files.list`,
`server.files.metadata`, `server.temperature_store`, `machine.system_info`, and the
HelixPrint plugin's reads: `server.helix.status`, `server.helix.3mf_conversions`,
`server.helix.analysis`, `server.helix.journal`, `server.helix.config`,
`server.helix.filament_usage`, `server.helix.storage`, `server.helix.plr.status`.

The HelixPrint plugin's other methods are refused whatever the plugin's permissions,
with `nil, err` naming the method (`<method> is not available to plugins`), because
they heat, move, delete or rewrite on the screen user's confirmation:
`server.helix.plr.plan`, `plr.prepare`, `plr.commit`, `plr.abort`, `plr.discard`,
`config_edit`, `print_hooks.screen_start`, `storage_cleanup`, and any `server.helix.*` method not named
here, including ones added later. The exceptions are `server.helix.print_modified` and
`server.helix.print_edited` (with `moonraker_write`), which start a print the way
`printer.print.start` does and carry its checks (below). A new read-only HelixPrint
endpoint joins the read-only list in `src/plugin/plugin_permissions.cpp`, the one
allowlist both checks read, in the same change that adds it.

A write the permission allows can still come back as `nil, err`: `helix.moonraker.call`
applies the refusals the app's own print, restart and G-code paths apply, before
anything is sent (`src/api/moonraker_gcode_guards.cpp#reject_plugin_rpc`).
`printer.print.start`, `server.helix.print_modified` and `server.helix.print_edited` refuse a file path with
directory traversal or illegal characters. While spools lie on the bed (the drying
latch), print start, `printer.print.resume`, `server.job_queue.start`, the modified- and edited-print
starts, `printer.restart`, `printer.firmware_restart` and a `machine.services.restart` of a
Klipper service are refused, and so is `printer.gcode.script` with any G-code the latch
does not allow. `printer.gcode.script` with homing G-code is refused while a print is
printing or paused. `helix.gcode` goes through the app's own G-code path, so the homing
and spools-on-bed refusals apply to it too. `err` is the refusal's English message, for
example `Homing is disabled while a print is in progress`.

`subscribe` mirrors Klipper's status subscription: `objects` maps an object name to
`true` or a field-name list, and `fn(status, snapshot)` receives a table
`{[object] = {field = value}}` and a boolean.

- `snapshot == true`: a full snapshot. It holds every subscribed field of every subscribed
  object Klipper reports right now, so replace your state with it instead of merging. An
  object missing from it is not available (not configured, or Klipper is not ready).
- `snapshot == false`: a change. It holds only the fields that changed; merge it into
  what you have.

The first delivery is a snapshot of the current values. Every subscription gets another
whenever the app re-reads the printer: after Moonraker reconnects, after Klipper restarts
and comes back to ready (`FIRMWARE_RESTART`, a config change, recovery from shutdown),
and when the app's own subscription changes. So the values a tile shows stay what Klipper
reports, including changes made elsewhere and fields that never change, with no polling.
A field you did not subscribe to is never delivered, even when Klipper sends it. The
returned handle's `cancel` method (`handle:cancel()`) stops it. Limits, each raising an error that names it: at
most 8 subscriptions per plugin totalling 16 objects, 32 fields per object, object names
at most 64 bytes of printable ASCII.

### helix.http (`src/plugin/lua_bind_io.cpp`)

| Call | Permission | Returns |
|---|---|---|
| `helix.http.get(url[, opts])` | `http` | `{status = code, body = text}` or `nil, err` |
| `helix.http.post(url[, opts])` | `http` | same |

`opts` is a table: `body` (string), `headers` (table of strings), `timeout_ms` (default
10000, clamped to 1-60000). Only `http://` and `https://` URLs. At most 2 requests in
flight per plugin. Requests do not follow redirects and refuse the printer's own host
and local interface addresses, which carry the control the `gcode` and `moonraker_write`
permissions gate (`include/plugin_backend.h#plan_http_target`). A response over the
memory cap comes back as an error.

### helix.storage (`src/plugin/lua_bind_io.cpp`)

| Call | Permission | Returns | Notes |
|---|---|---|---|
| `helix.storage.get(key)` | `storage` | value or `nil` | one JSON object in `plugin-data/<id>.json` beside the app settings; invalid JSON starts empty |
| `helix.storage.set(key, value)` | `storage` | | `value` of `nil` erases the key; the whole store is capped at 256 KB. Changes reach disk about 0.5 s later, one write per burst, and on unload or shutdown; a SIGTERM (supervisor kill) inside that 0.5 s loses them |

### helix.settings (`src/plugin/lua_bind_io.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.settings.get(key)` | value | the key must be declared in the manifest; returns the stored value if it fits the declaration, else the default |
| `helix.settings.set(key, value)` | | writes one of the plugin's own declared keys through the same validation, persistence and `on_change` path as the generated settings screen; undeclared keys, `action`/`info` rows and rejected values raise. The value already held is a no-op (no save, no `on_change`); the disk write is deferred about 0.5 s, like `helix.storage.set` |
| `helix.settings.on_change(key, fn)` | | `fn(value)` after each accepted change, from either path |

### helix.i18n (`src/plugin/lua_bind_core.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.i18n.t(s)` | string | resolves the English source through the app's own catalog first, then the plugin's `i18n/` pack for strings the app does not translate, then returns `s` unchanged |
| `helix.i18n.locale()` | string | the active language code (`"en"`, `"de"`, ...) |
| `helix.i18n.on_change(fn)` | | `fn(code)` after a language change, once the formatting tables are updated; several handlers allowed |

### helix.timer, helix.sleep, helix.json (`src/plugin/lua_bind_core.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.timer.after(ms, fn)` | handle | one shot; `handle:cancel()` |
| `helix.timer.every(ms, fn)` | handle | repeating; `handle:cancel()` |
| `helix.sleep(ms)` | | async; yields the entry and resumes after the delay |

Intervals are 1 ms to 24 h; a plugin holds at most 64 live timers.

| Call | Returns | Notes |
|---|---|---|
| `helix.json.encode(value)` | string | tables deeper than 32, cycles, and non-string object keys are errors |
| `helix.json.decode(s)` | value or `nil, err` | invalid JSON, or nesting deeper than 64, returns `nil` plus an error string |

### The sandbox

Libraries: `base`, `string`, `table`, `math`, `utf8`, `coroutine`. There is no `io`, `os`,
`package` or `debug`. `load` and `require` accept text chunks only; `dofile`, `loadfile`
and `string.dump` are nil; `collectgarbage` accepts only `count`, `collect` and `step`
(`src/plugin/lua_runtime.cpp#install_sandbox`). `require("name")` loads
`<plugin>/name.lua` or `<plugin>/lib/name.lua`, with dotted names mapping to folders,
rejects `..` and absolute paths, and caches the result
(`src/plugin/lua_runtime.cpp#lua_require`).

`setmetatable` refuses a metatable carrying `__gc`: finalizers run with the time-budget
hook off, so a looping one would hang the UI thread, and one armed in a menu check would
act later, outside check mode. The metatables the app owns are locked: `getmetatable`
on a string, or on a subject, timer, subscription or canvas handle, returns `"locked"`,
so plugin code cannot change how strings or bindings behave.

Define a global `on_unload` function to run once before the plugin closes, on reload,
disable, removal and shutdown. A faulted plugin's `on_unload` does not run, so never keep
state there that a fault would lose. It may not open overlays or confirm dialogs.

## 9. Permissions and consent

Four permissions (`src/plugin/plugin_permissions.cpp`):

| Permission | Unlocks |
|---|---|
| `gcode` | `helix.gcode` |
| `moonraker_write` | `helix.moonraker.call` beyond the read-only methods, `upload`, `download` |
| `http` | `helix.http.get`, `helix.http.post` |
| `storage` | `helix.storage.get`, `helix.storage.set` |

Everything else - subjects, the UI calls, `helix.sound`, `helix.printer`, `helix.moonraker.query`,
`subscribe`, `on_agent_event`, timers, settings - needs no permission. A call without its
permission raises a Lua error naming it.

Enabling a plugin shows what the user is approving, in the consent dialog's own words
(`src/plugin/plugin_consent.cpp#permission_line`; rewording these is a product change):

- `gcode`: "Send any G-code command. This is full control of the printer: every macro,
  including ones that run shell commands, is reachable."
- `moonraker_write`: "Change printer settings through Moonraker, including uploading,
  rewriting and deleting files in the printer's config folder."
- `http`: "Connect to servers on your network and the internet."
- `storage`: "Keep its own data on this screen (up to 256 KB)."

A plugin with no permissions reads "This plugin asks for no special permissions." An
update that asks for a permission the user has not granted parks the plugin at *needs
approval* instead of loading; enabling it again approves only the new lines.

## 10. Limits and faults

| Limit | Value | Source |
|---|---|---|
| Time per Lua entry | 50 ms of the main thread's CPU time, so a busy machine cannot trip it, or 1 s in all, which bounds an entry stalled off-CPU; once tripped, no depth of `pcall` holds the entry open | `include/lua_runtime.h#LuaRuntime/Limits`, `src/plugin/lua_runtime.cpp#budget_hook` |
| Memory per plugin | the manifest's `memory_mb` (1-64, default 2) | `src/plugin/plugin_host.cpp#load` |
| Memory, all plugins | min(RAM / 16, 64 MB); a plugin that does not fit stays *over memory budget* | `src/plugin/plugin_host.cpp#plugin_memory_budget` |
| Errors | the third within 60 s faults the plugin | `src/plugin/lua_runtime.cpp#report_error` |
| Finalizers | `__gc` is not available to plugins: `setmetatable` raises on a metatable carrying it, since a finalizer runs outside every time budget | `src/plugin/lua_runtime.cpp#install_sandbox` |
| Async results | a body that would cross the memory cap arrives as an error, not a fault | `include/lua_bindings.h#push_rpc_capped_body` |
| Canvases per plugin | no fixed number: each canvas's entry, name and lists count against `memory_mb` with the Lua heap | `src/plugin/lua_bind_canvas.cpp#charge` |
| Canvas list units | 4096 per list; a polyline counts each point, its area fill none | `include/plugin_canvas.h#kMaxCanvasUnits` |
| Canvas tokens | 32 distinct color and font tokens per list | `include/plugin_canvas.h#kMaxCanvasTokens` |
| Canvas coordinates | finite, within +-16384; stroke and border widths 0 to 64 | `include/plugin_canvas.h#kMaxCanvasCoord` |
| Canvas opa | 0 to 100 percent on line/polyline/arc/rect/circle, and fill_opa on polyline | `src/plugin/lua_bind_canvas.cpp` |
| Canvas text | at most 256 bytes per string | `src/plugin/lua_bind_canvas.cpp` |
| Measured text | at most 1024 bytes per `helix.ui.text_width` string | `src/plugin/lua_bind_ui.cpp` |
| Dialogs | one `helix.ui.dialog` shown or waiting per plugin | `src/plugin/lua_bind_ui.cpp#kMaxDialogs` |
| Confirms | one `helix.ui.confirm` shown or waiting per plugin | `src/plugin/lua_bind_ui.cpp#kMaxOpenConfirms` |
| Editors | one `helix.ui.edit_number` or `helix.ui.edit_text` shown or waiting per plugin; a validator runs under the menu-check budget | `src/plugin/lua_bind_ui.cpp#start_editor` |
| Sound cues | per plugin, 150 ms between `"ui"` cues and 1 s between `"event"` cues; a cue inside the gap is dropped | `include/lua_bindings.h#kPluginUiCueIntervalMs` |
| Canvas list bytes | charged against the plugin memory cap as staged; a call that would cross it raises, never faults | `include/plugin_canvas.h#DisplayList/bytes` |
| Tables | 8 columns, 500 rows, 256 bytes per cell or title, 128 bytes per row key; rows and columns count against `memory_mb` | `include/plugin_table.h#kMaxTableRows` |
| Home tiles | at most 32 `widgets` per plugin | `include/plugin_manifest.h#kMaxWidgetsPerPlugin` |
| Custom icons | at most 16 per plugin; a PNG of at most 64 KB and 256 x 256 pixels, else its fallback draws | `include/plugin_manifest.h#kMaxIconsPerPlugin`, `include/ui_icon_image.h#kMaxImageIconFileBytes` |
| Menu items | at most 4 per menu per plugin, 16 per manifest; a label is one line of at most 48 bytes | `include/plugin_manifest.h#kMaxMenuItemsPerMenu` |
| Plugin rows per menu | across every plugin, per opening: print details 8, filament slot 5, spool 6, a tile 4; rows past it are left out in load order | `src/plugin/plugin_menu.cpp#row_cap` |
| Menu checks | 10 ms of CPU or 100 ms in all, separate from the entry budget; an overrun is an error (it counts toward the error limit), never a fault by itself | `include/lua_runtime.h#LuaRuntime/CheckLimits` |

A fault (time budget, memory, error count, or `main.lua` failing to load) unloads the
plugin, marks it *faulted* with the reason in Settings > Plugins, and shows a "Plugin
disabled" toast. The app keeps running. Turn the plugin off and on in Settings > Plugins
to reload it.

## 11. Debugging

Run with `-vv`: every plugin line is tagged `[plugin <id>]`, including Lua errors with a
traceback and the reason a load or a fault happened. `helix.log.debug` shows at `-vv`
(`docs/devel/LOGGING.md`).

- Settings > Plugins shows each plugin's status and reason: *loaded*, *disabled*,
  *needs approval*, *invalid* (manifest or XML rejected, with the reason), *incompatible*,
  *over memory budget*, *faulted*.
- Read a subject exactly: with the app's remote control running,
  `helix-screen ctl text <id>__<subject>` (`docs/devel/HELIXCTL.md`; pin the socket as
  that doc says when several instances run).
- `HELIX_MOCK_PLUGINS_DIR=<dir>` makes a `--test` run serve that directory as the
  printer's `config/helixscreen/plugins/` folder, exercising install and sync end to end
  without a printer (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`).
- The `--test` mock printer defines the LED effects `breathing`, `fire_comet`, `rainbow`
  and `static_white`, so led-effects works against it out of the box.

## 12. Worked examples

**temp-spark** (`examples/plugins/temp-spark`) - no permissions, and the canvas worked
example: subjects carry the tile's text, `helix.canvas` carries the sparkline, and the
sample timer drives both:

```lua
local value_text = helix.subject.string("value", "--")
...
timer = helix.timer.every(helix.settings.get("interval_s") * 1000, tick)
```

Live readings come from `helix.printer.watch` with no permission, and the window is
seeded from Moonraker's own history through the read-only
`server.temperature_store`:

```lua
local ok, e = pcall(helix.printer.watch, h.temp, function(v) ... end)
local store, err = helix.moonraker.call("server.temperature_store", {include_monitors = false})
```

The sparkline and the overlay plot are one `draw()` that rebuilds a canvas's whole
list from the window and commits it, quoted verbatim from `main.lua`; `render()` calls
it for the tile's `spark` canvas and for the overlay's `graph` with the detail flag set
(gridlines, area fill, dot, dashed target), and `on_size` re-renders on every resize:

```lua
local function draw(c, detail)
    local w, h = c:size()
    if w > 4 and h > 4 and #samples >= 2 then
        local lo, hi = stats()
        lo, hi = range(lo, hi, detail)
        -- The newest dot needs its radius clear of the right edge, so the line
        -- stops a little short of it instead of touching the canvas border.
        local xmax = w - 1 - (detail and 4 or 3)
        local pts = {}
        for i, t in ipairs(samples) do
            pts[#pts + 1] = (i - 1) * xmax / (#samples - 1)
            pts[#pts + 1] = (h - 1) * (1 - (t - lo) / (hi - lo))
        end

        local labels = {}
        if detail then
            -- Gridlines at a whole-degree step (1, 2, 5, 10, 25, 50) labelled
            -- at the left edge; the built-in graph picks its step the same way.
            local step = 50
            for _, s in ipairs({1, 2, 5, 10, 25, 50}) do
                if s >= (hi - lo) / 4 then
                    step = s
                    break
                end
            end
            local v = math.ceil(lo / step) * step
            while v <= hi do
                local y = (h - 1) * (1 - (v - lo) / (hi - lo))
                c:line(0, y, w - 1, y, {color = "border"})
                if y >= 14 then
                    -- math.ceil yields a float, so the label goes through %d
                    -- to read "100°" instead of "100.0°".
                    c:text(2, y - 13, ("%d\u{00B0}"):format(v), {font = "xs", color = "text_muted"})
                    labels[#labels + 1] = y
                end
                v = v + step
            end
        end

        -- One polyline carries the stroke and the area fill down to the plot's
        -- floor; the fill's edge follows the line instead of stepping at the
        -- samples.
        c:polyline(pts, {
            color = "primary",
            width = 2,
            fill = "primary",
            fill_opa = FILL_OPA,
            baseline = h - 1,
        })

        -- A dot marks the newest sample.
        c:circle(pts[#pts - 1], pts[#pts], detail and 3 or 2, {fill = "primary"})

        if detail and target_on() then
            -- Dashes 6 on, 4 off, like the built-in graph's target trace.
            local y = (h - 1) * (1 - (target_now - lo) / (hi - lo))
            local x = 0
            while x < w - 1 do
                c:line(x, y, math.min(x + 6, w - 1), y, {color = "text_muted"})
                x = x + 10
            end
            -- "Target" sits on the left, above the dashed line, in the first
            -- band clear of the degree labels; the newest data rides the right
            -- edge, so the label never meets the dot.
            for _, ty in ipairs({y - 15, y + 4, y - 29, y + 18}) do
                local free = ty >= 0 and ty + 13 <= h
                for _, gy in ipairs(labels) do
                    if ty < gy and ty + 13 > gy - 13 then
                        free = false
                    end
                end
                if free then
                    c:text(2, ty, "Target", {font = "xs", color = "text_muted"})
                    break
                end
            end
        end
    end
    c:commit()
end
```

```lua
spark:on_size(render)
graph:on_size(render)
```

The tile's tap is one `event_cb` in XML and one handler in Lua:

```lua
helix.ui.on("open", function() helix.ui.overlay("temp-spark__detail", {title = selected().label}) end)
```

**led-effects** (`examples/plugins/led-effects`) - the `gcode` permission. The user-typed
effect name is validated before it reaches a command line: anchored character class,
length bound tied to subscribe's 64-byte object-name cap, refuse and toast otherwise:

```lua
if type(name) == "string" and #name <= 64 - #"led_effect " and name:match("^[%w_]+$") then
```

The toggle sends G-code and lets the live subscription report the result, so the tile
never guesses its own state:

```lua
local ok, err = helix.gcode(line)
sub = helix.moonraker.subscribe({[object] = {"enabled"}}, function(status) ... end)
```

The plugin's id contains a hyphen, so its XML uses `bind_flag_if_eq` instead of `cond`,
and `on_size` adapts the tile to its width in whole cells:

```lua
helix.widget("tile", { on_size = function(cols) wide:set(cols >= 2 and 1 or 0) end })
```

The maintainer's view of the same system - the sync pipeline, the sandbox internals, the
subscription union - is `docs/devel/architecture/12-system-services.md`.

## 13. Translations

A plugin's user-facing strings are English by default and translate through the
same machinery the app uses. Ship an optional `i18n/` folder beside `main.lua`
with one or more LVGL translation packs:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de">
  <translation tag="No events yet" de="Noch keine Ereignisse"/>
</translations>
```

The host registers every `i18n/*.xml` when the plugin loads. Packs cannot be
unregistered (LVGL keeps them until the app exits), so a path registers once: a
reload reuses what is already loaded, and an edited pack takes effect on the
next app start (plugin hot reload does not re-read it). A pack file over 256 KiB
is skipped with a warning.

English is the identity locale: `lv_tr()` returns the key itself when nothing
translates it, so an English-only plugin ships no `i18n/` at all. A pack
declaring `en` is refused, because the English text is the tag.

Lookup order is the app's own catalog first, then plugin packs. A plugin pack
only supplies tags the app does not translate: a pack entry for a tag the app
already translates (`Cancel`, `Started`, `Paused`, common settings vocabulary)
is never used in a language whose app pack translates it, and an empty slot in
a multi-language pack cannot hide an app string. That holds whether the
language was the boot language or was switched to later. Such an entry is dead
weight, so `scripts/check_plugin_i18n_shadow.py` (run from quality-checks) fails
on any example or fixture pack entry whose tag exists in
`translations/<lang>.yml`. Two plugins mapping the same English string
differently is first-registered-wins among plugin packs - keep translations
faithful to the source.

Where strings resolve:

- **manifest.json** `name`, settings `label`s and widget `name`/`description`
  translate at render time (the Plugins screen, the generated settings rows,
  the widget catalog). The top-level `description` is not displayed. The file
  stays English.
- **XML** static text: add `translation_tag="English text"` to a widget; the
  engine resolves and hot-reloads it on a language change like any app string.
- **Lua**: `helix.i18n.t("English text")` resolves through the same table.
  `helix.i18n.locale()` returns the active code (`"en"`, `"de"`, ...), and
  `helix.i18n.on_change(fn)` calls `fn(code)` after a language change so
  subject-carried strings re-render. Anything the plugin sends outward (a
  notification payload, a toast) follows the screen's language by translating
  through `t()` at send time.

`print-watcher` is the worked example: its `i18n/de.xml` carries only the
strings the app catalog does not already say. `ambient-tile` ships one too,
including a `%d` format specifier its German entry preserves in place.


