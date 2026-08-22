# helixctl — Remote Control & UI Driving

The helixctl client drives a running HelixScreen instance over JSON-RPC 2.0. It
replaces the old `-p`/`--panel` launch flags: instead of booting the binary
directly into a panel or overlay, you boot it once and then navigate, click,
fill fields, toggle switches, scroll, and capture screenshots from the command
line — with the **real widget lifecycle** (`init_subjects` / `create` /
`on_activate` / teardown), not empty shells.

This is the tool the screenshot pipeline uses, and the way to bring up any
panel/overlay/modal for debugging.

> ### ⚠️ Always pass an explicit socket
>
> A bare `helix-screen ctl` resolves to a **fixed per-user path**
> (`$XDG_RUNTIME_DIR/helixscreen-control.sock`, else `/tmp/helixscreen-control.sock`).
> With two instances up it silently drives **whichever started first** — and a command
> that lands on the wrong instance still reports success. That is how you end up
> "verifying" a change against someone else's app.
>
> Anyone running in a worktree, or alongside another agent or terminal session, must
> pick a unique path on **both** sides:
>
> ```bash
> # Derive one from the worktree so it is stable across restarts and unique per tree
> export HELIX_SOCK=/tmp/helix-$(basename "$(git rev-parse --show-toplevel)").sock
>
> ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" &
> ./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
> ```
>
> **A private socket is not full isolation.** `--remote-socket` moves the control
> socket; it does not move the config-dir flock, so a second instance still collides
> on config. For a genuinely independent instance set `HELIX_CONFIG_DIR` as well —
> see [Running a fully isolated second instance](#running-a-fully-isolated-second-instance).
>
> Before trusting any UI result, confirm what is running: `pgrep -x helix-screen`.

## One binary — `ctl` / `repl` subcommands

There is **no separate `helixctl` binary**. The client is folded into
`helix-screen` and reached via subcommands (they dispatch before any
app/display initialization, so they start instantly and never touch the UI):

```bash
helix-screen ctl <command> [args]     # one-shot command
helix-screen ctl -s <socket> <cmd>    # against an explicit socket
helix-screen ctl -C <path> <cmd>      # resolve this command's target inside <path>
helix-screen repl                     # interactive REPL
helix-screen ctl                      # no command → also drops into the REPL
```

> **Dev/test only.** The entire remote-control subsystem (server, transports,
> and this client) is compiled in **only when `HELIX_ENABLE_REMOTE_CONTROL` is
> defined** — the default for native dev builds. Release/cross builds for
> shipped devices exclude it entirely (no code, no overhead). A plain `make -j`
> builds it; to put it in a device dev image, build with
> `make PLATFORM_TARGET=<t> ENABLE_REMOTE_CONTROL=yes`.
>
> **The matching deploy turns it on for you.** The build flag alone is only half
> the story: the server still listens only under `--remote`, and the init scripts
> exec the launcher with no arguments. The link rule records the choice in
> `build/<platform>/bin/.build-features`, and every `make deploy-*` reads it and
> sets `HELIX_REMOTE_CONTROL=1` in the device's `helixscreen.env` before starting
> the app — so a device built with `ENABLE_REMOTE_CONTROL=yes` is reachable with
> `ctl` as soon as the deploy finishes. `helixscreen.env` is excluded from every
> deploy, so the setting survives redeploys; put `HELIX_REMOTE_CONTROL=0` there to
> opt back out and it will be left alone.

### Machine-readable output — `--json`

`helix-screen ctl --json <command>` prints the raw JSON-RPC `result` on one line and
exits 0. A **server** error prints the raw `error` object to stderr and exits non-zero;
a **client-side** usage error (unknown command, missing argument, no instance at the
socket) stays human-readable on stderr and also exits non-zero. This split means a
script can trust the exit code without inspecting the payload, while a typo still
produces a sentence rather than a protocol object.

The REPL ignores `--json` — formatted output is the reason the REPL exists.

    helix-screen ctl --json current | jq -r .panel
    helix-screen ctl --json resolve row_hardware | jq -r .path

Options are the same in both modes: `-s/--socket <path>` picks the instance,
`-C/--cwd <path>` scopes the command (see [the working directory](#the-working-directory)).

## Enabling the server

The control server runs as a background thread inside `helix-screen`.

| How | When it starts |
|-----|----------------|
| `--test` | Auto-enabled (no extra flag needed) |
| `--remote` | Opt-in for a non-test build |
| `--remote-socket <path>` | Override the socket path (default below); implies `--remote` |

```bash
# Boot a mock instance with the server up
./build/bin/helix-screen --test --skip-wizard --remote -vv &

# Drive it
./build/bin/helix-screen ctl navigate controls
./build/bin/helix-screen ctl ls
```

`--skip-wizard` suppresses the first-run wizard so automation lands on the home
panel. (It replaces the old side effect where `-p <panel>` implicitly skipped
the wizard.)

## Running headless (CI, ssh, containers)

Driving the UI needs no display server. SDL's dummy video driver is enough:

```bash
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket /tmp/hs.sock &
./build/bin/helix-screen ctl --socket /tmp/hs.sock navigate filament
./build/bin/helix-screen ctl --socket /tmp/hs.sock screenshot /tmp/filament.png
```

LVGL asks SDL for an accelerated renderer, which the dummy driver cannot
provide. The SDL backend catches that and retries with the software renderer, so
no extra environment variable is needed — you'll see
`Using software renderer (no GPU acceleration)` in the log. (Setting
`SDL_RENDER_DRIVER=software` yourself also works and skips the failed first
attempt.)

Audio is silenced the same way automatically: `main()` notices
`SDL_VIDEODRIVER=dummy` and forces `SDL_AUDIODRIVER=dummy` (unless you've
exported `SDL_AUDIODRIVER` yourself — that wins). Without it, a headless
run still opens the real PulseAudio/PipeWire/ALSA device and audibly beeps
through the desktop speakers on every `ctl click`. The `[SDLSound]` log
line names the driver actually in use, so `driver 'dummy'` is the visible
signal it took effect.

Screenshots are fully rendered in headless mode: capture goes through
`lv_snapshot_take()`, which re-renders the object tree into its own buffer
rather than reading back the display surface.

`scripts/screenshot.sh` switches to the dummy driver on its own when neither
`DISPLAY` nor `WAYLAND_DISPLAY` is set; `HELIX_HEADLESS=1` forces it on a
machine that does have a display.

## Transports (socket | HTTP)

The server speaks JSON-RPC over one of two transports, selectable at runtime:

| Flag | Default | Meaning |
|------|---------|---------|
| `--remote-transport socket\|http` | `socket` | Which transport to bind |
| `--remote-socket <path>` | see below | Unix-socket path (socket transport) |
| `--remote-http-bind <host>` | `127.0.0.1` | HTTP bind address (implies http) |
| `--remote-http-port <n>` | `7130` | HTTP TCP port (implies http) |

**Unix socket** (default) — local, owner-only (0600), no network exposure. The
`ctl`/`repl` client speaks this. Socket path resolution (client and server use
the same order):
1. `--remote-socket <path>` / `helix-screen ctl -s <path>` (explicit)
2. `$XDG_RUNTIME_DIR/helixscreen-control.sock`
3. `/tmp/helixscreen-control.sock`

**HTTP/TCP** — a minimal `POST /rpc` JSON-RPC endpoint. Binds loopback by
default; LAN exposure is opt-in via `--remote-http-bind`. This is the base for
the post-1.0 web config UI (the same embedded server will serve it).

```bash
./build/bin/helix-screen --test --remote --remote-transport http --remote-http-port 7130 &
curl -s -X POST http://127.0.0.1:7130/rpc \
  -d '{"jsonrpc":"2.0","method":"ping","id":1}'
# {"id":1,"jsonrpc":"2.0","result":"pong"}
```

### More than one instance

The well-known socket path is a fixed per-user location, so two instances want
the same file. On a device there is only ever one; on a dev box there are easily
two — two agent sessions, two worktrees, or an accidental double start.

The first instance owns the well-known path. A second one **does not take it**:
it probes with a `connect()`, finds a live owner, and parks on
`helixscreen-control-<pid>.sock` beside it, logging a warning. Both stay
reachable, and neither gets silently hijacked.

The client resolves in the same spirit. If the well-known path is live it uses
it — so with two apps running, a bare `helix-screen ctl` drives whichever one
started first, silently. If the well-known path is dead or absent, the client
looks for pid-suffixed instances: exactly one is used automatically, and
several make it refuse to guess:

```
$ helix-screen ctl ls
Error: several HelixScreen instances are running. Pick one with -s:
  --socket /run/user/1000/helixscreen-control-48211.sock
  --socket /run/user/1000/helixscreen-control-51907.sock
```

**With more than one app up, check which one you are driving before trusting a
UI or gesture result.** A command that lands on the wrong instance still
reports success. `pgrep -x helix-screen` is the quick sanity check.

Stale socket files are cleaned up at server start, not at exit: a `SIGTERM`
fast-exit deliberately skips teardown, so files outlive their process. Each
start sweeps `helixscreen-control-<pid>.sock` files whose pid is gone. The
sweep keys on that pid rather than a `connect()` probe — a probe cannot tell a
crashed instance from one that has called `bind()` but not yet `listen()`, and
unlinking that one would strand it.

### Running a fully isolated second instance

The auto-fallback above keeps two instances *reachable*, but it does not make them
*independent* — and it only kicks in by accident, after the collision. If you are
deliberately running a second app (a worktree, a parallel agent session, an
experiment against a different config), pin both axes up front:

| Axis | Flag / env | Without it |
|------|-----------|------------|
| Control socket | `--remote-socket <path>` (and `ctl -s <path>`) | Second instance parks on a pid-suffixed path; a bare `ctl` drives whichever started first |
| Config dir + flock | `HELIX_CONFIG_DIR=<dir>` | Second instance contends for the same settings.json and single-instance lock |

You need **both**. `--remote-socket` alone still collides on config.

```bash
# One block, per worktree — stable across restarts, unique per tree
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock"
export HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"

# The config dir must already exist. The app does not create it — it fails with
# "Cannot open lock file .../.helix-screen.lock: No such file or directory".
mkdir -p "$HELIX_CONFIG_DIR"

HELIX_CONFIG_DIR="$HELIX_CONFIG_DIR" \
  ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" \
  > "/tmp/helix-$TREE.log" 2>&1 &

./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot "/tmp/$TREE.png"
```

Deriving both names from the worktree basename means two agents in two trees never
collide, and re-running in the same tree reuses the same paths instead of littering
`/tmp` with pid-suffixed sockets.

`HELIX_CONFIG_DIR` is documented in `ENVIRONMENT_VARIABLES.md`.

## Commands

`helix-screen repl` (or `helix-screen ctl` with no command) drops into an
interactive REPL — line editing, history, tab completion — whose prompt is a
live breadcrumb of the navigation stack, e.g. `controls / motion_panel_0 > `.

### Navigation (filesystem metaphor)
| Command | Meaning |
|---------|---------|
| `ping` | Health check — answers `pong` once the control server is accepting |
| `status` | Panel, connection state, and printer status in one response |
| `navigate <panel>` | Go to a base panel, exactly as tapping its navbar button does — **pops any open overlay stack**, honours the connection/Klipper gating (a blocked panel is an error, not a silent no-op), and a second `navigate home` while already on Home resets the carousel |
| `cd <container>` | Move the working directory — see below. **No UI side effect** |
| `go_back`, `back` | Pop the current overlay |
| `help`, `?` | Print the command list (same as `-h`/`--help`) |
| `current`, `pwd` | Show the panel, overlay stack, and working directory |
| `resolve <target>` | Print the absolute locator a target resolves to |
| `list_panels` | List the registered base panels (the fixed `PanelId` set) |
| `wake`, `screensaver` | Reset the idle timer / dismiss the screensaver |

### The working directory

The REPL keeps a **cwd**, and every target resolves relative to it. `cd` into
the row you are working on and then type short names instead of long ones:

```text
controls > cd settings_list
controls / settings_list > ls              # just this subtree, not the screen
controls / settings_list > click 'toggle[1]'
controls / settings_list > cd ..
controls >
```

`cd` is a **pure move**. It changes where later commands resolve from and never
touches the UI — unlike the old `cd <widget>`, which reached its destination by
*clicking* it, so a mistyped destination could dispatch a real event on a live
printer. To make something happen, use `click`; to go somewhere, use `cd`. The
two hierarchies meet at exactly one point: `cd ..` goes up one container, and at
an overlay's root it pops the overlay, because an overlay *is* just a widget in
that tree.

`cd` alone (or `cd /`) returns to the active root — whatever is frontmost on
screen.

**The cwd follows navigation.** Anything that swaps the frontmost thing on
screen — `navigate`, a `click` that opens an overlay, `go_back`, a modal
appearing — resets the cwd to the new root. The client watches the `root_path`
that `get_current` reports rather than guessing which commands navigate, so a
click that *turned out* to open an overlay is handled the same as an explicit
`navigate`. A cwd can therefore never address a subtree that has been torn down.

The cwd lives in the **client**, as a path string re-resolved on every command.
Caching a widget pointer across commands would be a use-after-free the first
time an overlay closed or hot reload rebuilt the panel underneath it.

One-shot `ctl` has nowhere to keep a cwd between processes, so it takes `-C`:

```bash
helix-screen ctl -C s/settings_list click 'toggle[1]'
helix-screen ctl -C s/settings_list ls
```

#### On the wire

The cwd is purely a client-side convenience; the server stays stateless. Anything
speaking JSON-RPC directly (the HTTP transport, a script) gets the same behaviour
from two additions:

| Field | Where | Meaning |
|-------|-------|---------|
| `scope` | request param on any widget command | Absolute locator to resolve `name`/relative `path` inside. Omitted or unresolvable means the whole active screen plus the top layer, exactly as before |
| `root_path` | `get_current` response | Absolute locator of the frontmost panel, overlay or modal |

An **unresolvable `scope` is deliberately not an error** — a client's cwd can go
stale under it, and falling back to a screen-wide search beats refusing every
subsequent command.

```bash
curl -s -X POST http://127.0.0.1:7130/rpc -d '{"jsonrpc":"2.0","id":1,
  "method":"click","params":{"name":"toggle[1]","scope":"s/main/settings_list"}}'
```

### Introspection & widget interaction
| Command | Meaning |
|---------|---------|
| `ls`, `describe_screen` `[target]` | List on-screen widgets: name, `path`, `layer`, type, available actions. With a target, list only that widget's subtree (plus the widget itself); with no target, the working directory. The response also carries `topmost_layer` and `active_screen` — compare an entry's `layer` against `topmost_layer` to tell a frontmost widget from one stacked behind it — and `scope` whenever the listing was confined to a subtree. The REPL's rendering groups widgets by what you can do to them, with a final `(inert)` line for labels, icons and containers that carry no action (repeats collapsed as `name x6`) |
| `list_components` | List **every** registered XML component (live registry): panels, overlays, modals, cards, rows — the full introspectable surface |
| `list_callbacks` | List every registered event-callback name (overlay/modal open-handlers, button callbacks). Names only — nothing is fired |
| `click <target>` | Click a widget (also toggles switches/checkboxes) |
| `set_value <target> <v>` | Set a value (slider, switch, dropdown, textarea) |
| `scroll <target> [dx dy]` | Scroll a widget into view, or by a delta |
| `focus <target>` | Focus a widget through its input group. Fires the real `LV_EVENT_FOCUSED`, so a registered textarea raises the on-screen keyboard — `click` does not, and leaves it hidden. Fails if the widget is not in an input group |
| `text <target>` | Read a widget's text: `lv_label`, `lv_textarea`, or `lv_dropdown` (its selected option). Descends into a composite (e.g. a button wrapping a label) the same way `click` descends to a value-control. Raises rather than returning `""` if the widget has no text concept at all — an empty label and "not a text widget" are different facts |
| `state <target>` | Read a widget's LVGL states and flags: `checked`/`disabled`/`focused`/`pressed` as booleans plus an active-`states` array, and `hidden`/`clickable`/`scrollable` under `flags`. Descends a composite row to its control, matching what `click`/`set_value` act on; when it descends, a `target` subobject carries the named widget's own `path` + `flags` (a hidden row's inner switch carries no flag of its own). A HIDDEN widget still resolves by name (only `ls` filters hidden subtrees), so `bind_flag_if` and `disabled=`-prop contracts are assertable here |
| `set_text <target> <text>` | Overwrite a **label's** text. Resolves the target the same way `text` reads it, so a composite works. For labels the app sets imperatively — a value that comes from a backend field rather than a subject, so `set` cannot reach it (e.g. the AMS loading-error message). A label driven by `bind_text` is restored the next time its subject changes; set the subject instead when one exists |
| `geom <target> [depth]` | Measured geometry: position, size, declared-vs-computed size, flex/scroll state |
| `get_const [scope] <name>` | Resolve an XML `#const` to the value the renderer actually sees |

### Synthetic pointer — testing gestures

`click` is `lv_obj_send_event(obj, LV_EVENT_CLICKED)`: a widget-level event with no
input device and no coordinates behind it. That is right for "press this button",
but it cannot exercise anything gestural. Code that reads `lv_indev_active()` or
`lv_indev_get_point()` sees nothing, long-press timers never start, and LVGL's
scroll-versus-click arbitration never runs.

These commands drive a second, `ctl`-owned pointer device through **LVGL's real
input pipeline**, so gestures behave exactly as they do under a finger.

| Command | Meaning |
|---------|---------|
| `press <x> <y>` | Put the pointer down at screen coordinates x,y |
| `move <x> <y>` | Move it — a drag while pressed, a hover while released |
| `release [x y]` | Lift it, at x,y if given, otherwise where it currently is |
| `long_press <x> <y> [hold_ms]` | Press at x,y, hold past the long-press threshold, then release - the whole gesture inside one request |

Each command returns only after LVGL has sampled the device twice, so sequences do
not race the indev timer.

The device is created lazily on the first pointer command and coexists with the
real SDL/evdev pointer; LVGL supports multiple pointer indevs. Instances that never
receive a pointer command never register it.

**A long press cannot be assembled from the shell.** `press`, `sleep`, `release` looks
like it should work and does not: every `ctl` invocation is its own process and
connection, so the hold elapses with no client attached, and the command that follows
re-samples the device in a way that restarts the press. `long_press` exists because the
hold has to happen server-side. It latches the press, holds without touching the pointer
while LVGL keeps sampling it on its own timer - exactly as under a resting finger - and
then releases (`src/remote/remote_control_server.cpp:1998-2035`).

`hold_ms` is optional. Omitted, the server derives the hold from the **configured**
long-press time (`InputSettingsManager::get_long_press_time()`, the Touch & Input
setting) plus a margin - `pointer_long_press_hold_ms()` in `include/remote_pointer.h:38-55`.
Two reasons it is derived rather than hardcoded: LVGL starts counting from the sample
that first reports the press, not from the moment the command ran, so a hold of exactly
the threshold races the indev timer and intermittently lands a plain click; and raising
Long Press Time in settings would otherwise silently turn `long_press` back into a click.
Pass `hold_ms` only when the gesture itself needs a specific duration. The response
reports `held_ms`.

```bash
# Long-press a key and lift in place
helix-screen ctl long_press 100 300

# Slide from one widget onto another before lifting
helix-screen ctl press 100 300
helix-screen ctl move 100 260
helix-screen ctl release

# Drag to scroll a list, proving a tap does NOT fire mid-scroll
helix-screen ctl press 400 200
helix-screen ctl move 400 160
helix-screen ctl move 400 120
helix-screen ctl release
```

Separate commands are right for those last two: what matters is where the pointer goes,
not how long it rests, and the press stays latched between connections. `long_press`
always ends in its own release (`src/remote/remote_control_server.cpp:2031`), so a
hold-then-slide gesture - long-press to raise a popover, then slide onto it - has no
single-command form.

Get coordinates from `geom <target>` — it reports each widget's absolute `x`, `y`,
`w` and `h`, so aim at a rect's centre rather than guessing.

#### Home-grid Edit Mode and the widget catalog

```bash
# 1. Enter Edit Mode - one long press anywhere on the home grid
helix-screen ctl navigate home
helix-screen ctl geom filament          # a tile's rect; its name is its PanelWidgetDef id
# {"x": 40, "y": 120, "w": 100, "h": 50, ...}
helix-screen ctl long_press 90 145      # centre of that rect

# 2. Open the widget catalog - click the nav bar's "+", NOT a second long press
helix-screen ctl click nav_btn_edit_add
helix-screen ctl ls
```

The `long_pressed` handler is registered on `carousel_host`, the grid's own container
(`ui_xml/home_panel.xml:11`), and the press reaches it by bubbling: the carousel, its
scroll container, its tiles and the page containers all get `LV_OBJ_FLAG_EVENT_BUBBLE`
(`src/ui/ui_panel_home.cpp:241-256`), and `set_event_bubble_recursive()` re-flags every
descendant of a page container after its widgets are populated
(`src/ui/ui_panel_home.cpp:43-51`, called at `:495`). So aiming at a tile is fine - it is
not necessary to hit the gutter between tiles.

**Open the catalog with `click nav_btn_edit_add`, not a second long press.** Entering Edit
Mode already selects whatever widget was under the press and starts dragging it
(`src/ui/ui_panel_home.cpp:954-958`), and `GridEditMode::handle_long_press` opens the
catalog only when nothing is selected (`src/ui/grid_edit_mode.cpp:1012-1046`) - so a
second long press on a tile starts a drag instead. The nav bar's `+`
(`ui_xml/navigation_bar.xml:22-28`) goes straight to `HomePanel::open_widget_catalog()`
(`src/xml_registration.cpp:331-332`) with no such condition. A press that lands on empty
grid selects nothing, and *then* a second long press does open the catalog - but the
button is the case that always works.

**When the long press appears to do nothing**, check the suppressors before suspecting the
pointer. `should_suppress_edit_mode()` (`src/ui/ui_panel_home.cpp:854-889`) drops it when:

| Condition | How to check |
|-----------|--------------|
| Home edit mode is off in Touch & Input (#1245) | `ctl get settings_home_edit_mode_enabled` - check this first |
| The lock screen is up | `ctl ls` shows the PIN pad as the topmost layer |
| A scroll object is active on the indev | a preceding drag left the list scrolling; `ctl wait_idle` or release cleanly first |
| The press target is an arc or slider | those consume drags for value adjustment - aim at a different part of the tile |

Drift is not a factor with `long_press`: entering Edit Mode also requires a stationary
hold (`src/ui/ui_panel_home.cpp:927`), and the synthetic pointer never moves during it.

**`home_edit_mode` is a read-only reflection - do not `set` it.** `ctl set home_edit_mode 1`
returns success and does not enter Edit Mode. The subject is written by
`GridEditMode::enter()` / `exit()` (`src/ui/grid_edit_mode.cpp:66`, `:120`); setting it by
hand only unhides the nav bar's edit buttons, which bind to it
(`ui_xml/navigation_bar.xml:25`). Clicking the `+` then still does nothing, because
`HomePanel::open_widget_catalog()` no-ops unless `grid_edit_mode_.is_active()`
(`src/ui/ui_panel_home.cpp:1020-1024`). `long_press` is the only way in.

A **target** is one of:

| Form | Example | Means |
|------|---------|-------|
| name | `row_theme` | The widget of that name, searched inside the cwd |
| indexed name | `toggle[1]` | The 2nd `toggle` in the cwd, in document order (0-based) |
| glob | `'row_*'` | Every matching name — see below |
| relative path | `row_theme/toggle` | Walked down from the cwd |
| absolute path | `@s/main/settings_list` | From the screen root, ignoring the cwd |

Paths are written in **names**, not child indices: `s/main_content/settings_list/row_theme`
rather than `s/15/1/1/2`. Each segment is the widget's name where that name is
unique among its siblings, `name[k]` where it is not, and a bare child index
only where the widget has no name at all. The `@` on an absolute path is
optional, since widget names never contain `/`.

Name segments are also the stable ones. Inserting a row above the target shifts
every child index after it, but not a name — which matters because a locator is
something a person copies out of `ls` and retypes later.

An **all-numeric locator still resolves** exactly as it did before names were
introduced, so anything holding an older path keeps working.

`toggle[1]` counts matches *within the cwd*, which is what gives the ordinal a
meaning you can predict. Unscoped, it counts across the whole screen, where an
opening overlay can shift it — so `cd` first when you intend to use one.

A name segment that matches several siblings with no ordinal to pick between
them resolves to **nothing**, and the error names the candidates. Silently
taking the first is the same failure mode as #1179: a command that reports
success while acting on a widget the caller never addressed.

Name lookup resolves to the **topmost visible** match: hidden subtrees are
skipped, and among what remains the widget in the frontmost overlay wins
(the top layer outranks the active screen; later siblings outrank earlier
ones). Overlays stay in the tree when another is pushed on top of them, so
without this a name present in both would resolve to the one behind — a click
that reports success and does nothing.

Mutating responses echo `path` (the widget actually hit), `handlers` (its
registered event count — `0` means the click cannot do anything), and
`active_screen` (a `panel > overlay > overlay` breadcrumb). Check
`active_screen` first when a command appears to do nothing: a first-run wizard
or an unexpected modal swallows input while every response still reads as
success.

A full-screen `ls` on a settings page runs to hundreds of entries. Scope it once
you know the row you want — `ls row_filament_auto_cooldown` returns that row and
its handful of children, `scope` in the response echoing the subtree root. With
a cwd set, a bare `ls` is already scoped to it.

**Wildcards.** A target name containing `*` (any run of characters, including
none) or `?` (exactly one) is matched as a glob against every visible named
widget on the active screen and the top layer:

```bash
helix-screen ctl ls 'row_*'          # every settings row, each with its subtree
helix-screen ctl ls '*cooldown*'     # find it without knowing the full name
helix-screen ctl click '*auto_cooldown*'
```

Quote the pattern so your shell doesn't expand it against the filesystem first.

`ls` lists **all** matches — that is the point of it — and reports `scope` as an
array plus a `matched` count when there is more than one. A match nested inside
another match is not a scope of its own: `row_*` on a settings page hits both
each row and the `row_icon` within it, and listing both would emit the icon
twice (once as a scope root, once inside its parent's subtree) and report a
doubled count. Only the outermost matches become scopes; the inner ones still
appear, in their parent's listing. `click`, `set_value`
and `scroll` instead require the pattern to identify **exactly one** widget: on
multiple matches they fail with the candidate names and `@path`s rather than
acting on whichever came first, since driving the UI somewhere unintended is
much worse than an error. Glob matching skips hidden subtrees, same as `ls`.

**Composite rows resolve to the control inside them.** A settings row is a
clickable container wrapping the actual switch/dropdown, so a naive
`click <row>` would fire CLICKED on the container and do nothing visible.
`click` and `set_value` therefore descend to a **value-control** (switch,
checkbox, slider, arc, dropdown, textarea) when the target isn't one itself and
its visible subtree holds exactly one — the response reports `descended_to` with
that child's path. Rows with no value-control (a category row that opens an
overlay) are clicked as-is, so navigation is unaffected. If several candidates
exist the container is clicked and they are listed under `candidates`, so you
can re-issue against a specific `@path`.

The descent is **bounded to scaffolding**, in two ways. A target that already
carries a click handler of its own is acted on literally — never descended
into — and the search never tunnels past a descendant that carries one either.
Without those bounds a full-screen context-menu backdrop resolved onto whatever
single visible control the menu happened to contain and dispatched its event:
`click` on the AMS slot menu's backdrop reached the backup-slot dropdown and
sent `SET_RUNOUT` to the printer, from a click whose only intent was to dismiss
(#1179). To close a menu or overlay, clicking its backdrop now dismisses it;
`go_back` remains equivalent and is clearer about intent.

#### `geom` — why a widget is the size it is

`ls` tells you a widget exists; `geom` tells you how big it ended up and what it
asked for. The pair is what distinguishes "my widget is missing" from "my widget
is present but computed to zero", which look identical on screen.

```bash
helix-screen ctl geom details_catalog_selector
helix-screen ctl geom details_view 2      # recurse 2 levels into children
```

| Field | Meaning |
|-------|---------|
| `x`, `y` | Absolute screen coordinates (comparable against a screenshot) |
| `w`, `h` | Computed size |
| `content_w`, `content_h` | Inner area, padding excluded |
| `req_w`, `req_h` | The size **as authored**: `"content"`, `"50%"`, or a pixel count |
| `flex_grow` | Flex grow factor |
| `hidden`, `scrollable` | Flag state |
| `scroll` | `top`/`bottom`/`left`/`right` scrollable extents |

`req_*` reports the authored form rather than the raw coord, because LVGL packs
`LV_SIZE_CONTENT` and percentages into the integer — printed raw they surface as
meaningless sentinels. A `req_h` of `content` or a nonzero `flex_grow` sitting
next to a computed `h` of `0` is the signature of a flex child collapsing in a
content-sized parent, which has no free space to distribute.

#### `get_const` — what value the renderer actually resolved

```bash
helix-screen ctl get_const color_swatch_grid grid_width   # scoped lookup
helix-screen ctl get_const space_md                       # globals
helix-screen ctl get_const @color_swatch_grid             # dump every const in a scope
```

A scoped lookup falls back to `globals` when the name is not in the component's
own scope, mirroring how the renderer resolves an unqualified `#const`; the
`scope` field in the reply says which one answered. Consts registered from C++
can silently disagree with what XML resolves — `lv_xml_register_const()` is a
no-op when the name already exists in the scope, so a component's fallback
`<consts>` win unless the C++ side uses `lv_xml_update_const()`. This command
reads the resolved value, so it shows which one is really in effect.

### Screenshots & sample-data screens
| Command | Meaning |
|---------|---------|
| `screenshot [path] [--target W] [--stable]` | Capture a screenshot. With no path, a timestamped `.bmp` in the runtime dir; a path ending in `.png` is encoded as PNG (in-app, via lodepng). `--target` crops to a named widget's bounds; `--stable` polls until the pixels stop changing before capturing. The response reports the file actually written under `path`, plus `w`/`h` and `stable_frames` |
| `demo <name>` | Bring up a screen that can't be reached by navigation in mock mode |

`demo` covers screens that only appear on a real printer event or configured
state, constructed with representative sample data and the real lifecycle:
`preflight-check`, `color-mismatch`, `runout-modal`, `lock-screen`,
`print-status`, `print-tune`, `ams`, `camera`, `ams-error-toast`,
`action-prompt-worst`, `action-prompt-many`.

`action-prompt-many` raises a Klipper `action:prompt` carrying seven material
presets, the case where the buttons cannot share one row and must wrap. Both
`action-prompt-*` demos need a live `action:prompt_begin`, so no sequence of
clicks reaches them in mock mode.

`ams-error-toast` raises the two-line AMS error toast (message plus
`AmsError::suggestion`) using the longest suggestion any backend produces. The
mock AMS backend has no print gate, so no sequence of clicks reaches that
refusal — this is how the toast's layout gets checked on a 480x272 panel.

### Diagnostics & lifecycle
| Command | Meaning |
|---------|---------|
| `wait_idle [--timeout N]` | Block until `UpdateQueue` and `HttpExecutor` are both quiet (default 10s), so a script can gate on real async work instead of a fixed `sleep` |
| `freeze` | Stop the moving parts for a reproducible capture: `lv_anim_delete_all()` plus `animations_enabled = 0`, and pause every periodic `lv_timer` except two (see below). Returns `{"frozen": true, "timers_paused": N}` |
| `unfreeze` | Reverse `freeze`: resume exactly the timers it paused, re-enable animations. Returns `{"frozen": false, "timers_resumed": N}` |
| `log [-n N]` | Tail the app's in-memory log ring buffer (default 50 lines). Printed as raw lines, so it pipes to `grep` |
| `shutdown` | Ask the app to exit its main loop (`app_request_quit`), running the normal shutdown path |
| `reset` | Return to the home panel with no overlays or modals open. Returns `{"panel": "home", "overlays_popped": N, "modals_cleared": N, "toasts_cleared": N}` |

`log` reads the same ring buffer the debug bundle's `log_tail` uses — capacity
scales with the device's RAM and is overridable via `HELIX_LOG_RING_LINES`. It
means a scripted run can read the app's own log without redirecting stdout to a
file first. The ring is installed by `init_early()`, so on a short-lived instance
it still holds the Phase 2 config-load trail that runs before the full logger
exists (`LOGGING.md` § "Ring-Buffer Sink Lifecycle").

#### `reset` — a cheap alternative to rebooting between tests

Booting `helix-screen` costs about two seconds, so a full test corpus shares
one instance (`tests/ui/conftest.py`'s session-scoped `helix_app` fixture) and
resets it between tests instead of restarting it. `reset` pops every overlay
off the navigation stack, dismisses every modal, dismisses every toast, and
switches the base panel to `home` — all through the same live-safe paths the
UI itself uses (`NavigationManager::go_back()`, `Modal::hide()`,
`ToastManager::hide()`), never the synchronous, teardown-only
`ModalStack::clear()` (its only other caller is `Application::shutdown()`,
where deleting everything synchronously is fine because nothing else is
running).

`overlays_popped` and `modals_cleared` are exact counts. The overlay count is
sampled once before any popping starts — rereading it mid-loop wouldn't
reflect anything, since `go_back()` defers its actual work to the next
`UpdateQueue` tick even when called from the UI thread. The modal count is
re-checked per iteration instead, since `Modal::hide()`'s own bookkeeping
(marking the entry "exiting") *is* synchronous — only the widget deletion is
deferred. Both loops are capped (overlays at 32, modals at 16): a stack that
will not drain is a bug, and looping unboundedly on the UI thread would turn
it into a hang instead of a report. Hitting either cap logs a `spdlog::warn`
so it doesn't pass silently as "reset just didn't have much to do."

`toasts_cleared` is 0 or 1 — "were there any" — not an exact count.
`ToastManager` exposes a dismiss-all (`hide()`) but its visible-toast counter
is private, and adding a public accessor was out of scope for this change; a
real count is a follow-up for whichever future `toasts` command Tier 2 adds.

`reset()` does not touch mock printer/backend state (`scenario`, subject
values set via `set`) — only navigation, modals, and toasts. A test that
leaves the mock in a particular scenario still needs to clean that up itself
(e.g. in a `try/finally`, so a scenario doesn't leak into whichever test runs
next in a shared session regardless of how the current one exits).

**`reset` itself is asynchronous — pair it with `wait_idle`.** As noted above,
`overlays_popped` counts `go_back()` *calls*, each of which only enqueues its
pop onto the next `UpdateQueue` tick; the RPC returns as soon as those calls
are issued, not once the pops have actually landed. Calling `reset()` and
immediately trusting the screen is at `home` with no overlays is exactly the
race `wait_idle()` exists to close — `tests/ui/conftest.py`'s autouse fixture
does `reset(); wait_idle()` as a pair for this reason, not `reset()` alone.

From the one-shot client, `quit` and `exit` are accepted as aliases for
`shutdown`. **In the REPL they are not** — there, `quit`/`exit`/Ctrl-D leave the
REPL and `shutdown` stops the app, which is the only reading that keeps both
meanings available.

#### `wait_idle` — what it can and cannot see

`wait_idle` polls two counters from the transport thread — `UpdateQueue` pending
work (including anything buffered by a `ScopedFreeze` held internally) and `HttpExecutor` in-flight
items on both lanes — and returns once both read zero on two consecutive
samples (a single zero reading can land in the gap between one callback
finishing and the next being enqueued by the work it just completed). A
timeout names the nonzero counter(s) rather than just saying time ran out.

**Animations are deliberately not one of the counters.** An earlier version of
this design also counted `lv_anim_count_running()`, but a real UI has
legitimately perpetual animations, so "zero animations running" is not a
reachable idle state in general — it is a property of one screen in one
settings configuration, not of the app having finished its work. Concretely:
`print_file_detail`'s loading spinner lives inside the eagerly-built
`print_select_panel`, so its animations run from boot onward regardless of
which screen is displayed — counting them made `wait_idle` succeed only when
the `animations_enabled` setting happened to be off. Animation-driven pixel
churn is covered instead by the frame-hash screenshot gate, which measures
whether pixels actually stopped changing rather than inferring it from a
counter. (Separately, that spinner burning three animation timers forever
while invisible is a real, still-open performance defect, independent of the
`wait_idle` contract — see the design spec's "Determinism model" for two
attempted fixes, both reverted, and why.)

It is **best-effort by design**, not a hard guarantee: the enumeration surface is
large and grows. Known gaps:

| Source | Where | Note |
|---|---|---|
| Raw `lv_async_call` | `panel_widget_manager.cpp` (home-panel widget-gate rebuild), `ui_nav_manager.cpp` (overlay-close), `ui_filament_path_layers.cpp`, `grid_edit_mode.cpp` | LVGL exposes only call/cancel — no count API |
| Thumbnail render thread | `gcode_object_thumbnail_renderer.cpp` | Own `std::thread`, not `HttpExecutor` |
| GCode geometry build | `ui_gcode_viewer.cpp` (`build_thread_`) | Same |
| GCode layer/streaming | `gcode_layer_renderer.h`, `gcode_streaming_controller.h` | Same |
| Mock backends | `moonraker_client_mock.cpp` (`simulation_thread_` + 3 timers), `moonraker_client_mock_print.cpp` (2 timers), `ams_backend_mock.cpp` (6 threads), `wifi_backend_mock.cpp` (2) | Mock mode **adds** nondeterminism |
| Deferred deletion | `safe_delete_deferred` / `lv_obj_delete_async` / `safe_clean_children` sites | Escapes the queue by design — `pending == 0` does not mean the old subtree is gone |

#### `freeze` / `unfreeze` — the other half of determinism

`freeze` pairs with `wait_idle` for screenshot-quality stability: `wait_idle`
waits for async work to *land*, `freeze` stops the moving parts so a captured
frame doesn't change again a moment later. It combines three things:

- `lv_anim_delete_all()` — stop animations already running.
- Flipping the existing `animations_enabled` **subject** to `0` to prevent new
  animations from starting (honored at ~51 call sites) — but **not** via
  `DisplaySettingsManager::set_animations_enabled()`. That setter calls
  `Config::save()` on every call, so going through it would persist the
  change to `settings.json` on every `freeze` and write it back on every
  `unfreeze`. `freeze` is a transient test-mode toggle; a `--remote` dev
  instance killed or crashed between the two would otherwise leave a real
  user's config with animations permanently disabled — automated tests never
  see this because `--test` uses `settings-test.json`. The handler instead
  reads and writes `DisplaySettingsManager::subject_animations_enabled()`
  directly (an accessor already public and already used by several widgets to
  observe this setting), and remembers the real pre-freeze value so `unfreeze`
  restores it exactly rather than assuming "on".
- Pausing every periodic `lv_timer` one at a time via `lv_timer_pause()`,
  **with a two-entry skip list**.

**The skip list, and why a global `lv_timer_enable(false)` cannot be used
instead:** `UpdateQueue`'s processor is itself an `lv_timer`
(`include/ui_update_queue.h`), and `RemoteControlServer::execute_on_ui_thread`
dispatches every handler — including `unfreeze` itself — through
`helix::ui::queue_update()`. A global timer disable stops that processor along
with everything else, so the very next command blocks for the 10s UI-thread
timeout and throws. `freeze` would brick the control channel it arrived on.
Two timers are therefore left running by identity:

1. `UpdateQueue`'s own timer (exposed via a `timer()` accessor), or the
   control server can never dispatch another command, `unfreeze` included.
2. The display refresh timer (`lv_display_get_refr_timer()`), or nothing
   renders and a frame-hash screenshot gate never observes a new frame.

`unfreeze` resumes exactly the set of timers `freeze` paused — tracked as a
`std::vector<lv_timer_t*>` on the server — and restores `animations_enabled`
to its captured pre-freeze value (not unconditionally "on"). A timer
legitimately deleted while frozen (e.g. panel teardown) is skipped rather than
dereferenced: `unfreeze` walks the live timer list to confirm each tracked
pointer still exists before resuming it.

Both ends are idempotent: a `freeze` while already frozen returns the existing
`timers_paused` count rather than re-scanning (every timer would already read
paused, so a naive re-scan would track none of them and orphan the original
set); an `unfreeze` with no prior `freeze` is a no-op returning
`timers_resumed: 0`, so a defensive `try/finally: unfreeze()` never needs to
guard whether `freeze` actually ran first.

**Issue `freeze` from a settled screen, not mid-transition.**
`lv_anim_delete_all()` fires each animation's `deleted_cb`, not its
`completed_cb` — and overlay teardown (`NavigationManager::overlay_slide_out_complete_cb`,
`ui_nav_manager.cpp`) is wired to `completed_cb`, since that's what marks the
close as genuinely finished rather than merely interrupted. Freezing while an
overlay's close animation is still in flight therefore deletes the animation
without ever running its completion logic, stranding the overlay: never
popped, its close callback never invoked. Not reachable from this harness's
own automated tests (they force `animations_enabled=0` before boot, so overlay
transitions never animate to begin with — see "Golden corpus scope" in
`UI_TESTING.md`), but real for a `--remote` dev instance with animations left
on. Wait for a transition to finish (or don't fight it — freeze right after a
`navigate`/`click` rather than while one is still resolving) before freezing.

See "Golden corpus scope" in `UI_TESTING.md` for the full determinism
rationale.

#### `screenshot --stable` / `--target` — the frame-hash gate

`wait_idle` and `freeze` are both best-effort: neither one can see raw
`lv_async_call` work, the gcode/thumbnail build threads, or the mock
backends' own threads (see the gap table above). `--stable` is the black-box
backstop — it hashes the actual captured pixels (FNV-1a over the composited
RGBA buffer) and polls, at most 180 samples 16ms apart (~3s), until three
consecutive frames hash identically. It throws rather than returning a
possibly-mid-repaint frame if the screen never settles in that window, naming
the likely fix:

```
Screen never stabilized: no 3 identical consecutive frames within 3s. Try `freeze` first.
```

`--stable` measures pixels, not timers — it complements `freeze` (which stops
the *known* movers) rather than replacing it. The common pattern is both
together: `freeze` first to kill animations and pause timers, then
`screenshot --stable` to also rule out whatever `freeze` doesn't enumerate.

`--target <widget>` crops the capture to that widget's bounding box instead of
the whole screen, clamped to the captured buffer (a widget can extend past the
screen edge). It is a widget target like any other, so it resolves inside the
working directory — `cd` to a card and `screenshot --target` a name within it
means the name inside that card, not the first one on screen. This is what keeps a golden corpus maintainable: a widget-scoped
golden only changes when that widget's own pixels change, instead of going red
every time anything else on screen moves. The crop's `w`/`h` in the response
match `geom <widget>`'s reported `w`/`h` exactly.

Internally, the server exposes this as three C++ pieces so capture and
encoding are independent: `helix::capture_frame(CapturedFrame&, lv_obj_t*
crop_to = nullptr)` (snapshot + top-layer composite + optional crop, no disk
I/O), `helix::frame_hash(const CapturedFrame&)` (the FNV-1a hash the stability
loop compares), and `helix::write_frame(const CapturedFrame&, out_path)`
(encode + write). `save_screenshot()` — still the entry point its three
`application.cpp` callers use (SIGUSR1, the 'S' key, the auto-screenshot loop
handler) — is now a thin wrapper over the two.

### Subjects & scenarios
| Command | Meaning |
|---------|---------|
| `get <subject>` / `set <subject> <value>` | Read / write a bound subject |
| `list_subjects` | List all registered subjects |
| `wait_for <subject> <value> [--timeout N]` | Block until a subject matches |
| `scenario <name>` / `list_scenarios` | Apply / list mock scenarios |

## Interactive REPL

`helix-screen repl` (or `helix-screen ctl` with no command) opens an interactive
session with line editing, persistent history, and Tab completion (over commands,
subject names, panels, and scenarios). It reconnects per command, so it survives
the app restarting mid-session — handy while iterating on XML with hot reload.

The prompt is a **live breadcrumb of the navigation stack plus the working
directory**, so you always know where you are and what a bare name will resolve
against. `pwd` (`current`) spells both out, `ls` lists what is here:

```text
$ helix-screen repl
helix-screen control REPL — type 'help' for commands, Tab for completion, Ctrl-D to quit

> navigate controls
controls > ls
  click   btn_motion btn_nozzle_temp btn_extrusion btn_fan btn_bed_mesh ...
  (42 widgets — `ls` shows all; @path targets any one)
controls > click btn_motion            # opens the overlay; cwd follows it
controls / motion_panel_0 > cd jog_grid
controls / motion_panel_0 / jog_grid > set_value jog_distance 10
controls / motion_panel_0 / jog_grid > cd ..
controls / motion_panel_0 > pwd
  panel:   controls
  overlays: [motion_panel_0]
  cwd:     s/motion_panel_0  (active root)
controls / motion_panel_0 > cd ..      # at the root, up pops the overlay
controls > quit
```

Tab completion covers commands, subject names, panels and scenarios — and, for
any command that takes a target (`cd`, `ls`, `click`, `focus`, `text`,
`set_text`, `state`, `geom`, `set_value`, `scroll`, `resolve`), the **widget
names in the current cwd**.
Those are re-read on every keypress rather than cached: the tree changes under
the REPL constantly, and a cache would offer widgets that have left the screen.

Every command from the tables above works at the prompt. A handful are
REPL-only:

| Command | Meaning |
|---------|---------|
| `cd` | The working directory only persists within a session (one-shot uses `-C`) |
| `help` | Show the in-REPL command summary |
| `refresh` | Reload the Tab-completion caches (subjects/scenarios/panels) after state changes |
| `quit`, `exit`, `Ctrl-D` | Leave the REPL |

## Bringing up a panel or overlay (replaces `-p`)

> Note: `cd <widget>` used to be a way to *reach* an overlay — it clicked the
> widget. It no longer does anything to the UI; use `click` for that.

| Old | New |
|-----|-----|
| `helix-screen --test -p motion` | boot `--test --remote`, then `helix-screen ctl navigate controls; helix-screen ctl click btn_motion` |
| `helix-screen --test -p settings` | `helix-screen ctl navigate settings` |
| `helix-screen --test -p print-status` | `helix-screen ctl demo print-status` |
| `HELIX_SSAO=0 helix-screen --test -p bed-mesh` | `HELIX_SSAO=0 helix-screen --test --remote &` then `helix-screen ctl navigate controls; helix-screen ctl click btn_bed_mesh` |

Environment variables that used to pair with `-p` (`INPUT_SHAPER_AUTO_START`,
`SCREWS_AUTO_START`, `HELIX_MOCK_DRYER_SPEED`, `HELIX_GCODE_STREAMING`, …) still
apply — set them on the launch, then navigate to the panel with the client.

The exact navigation recipe for each documentation screen lives in
`scripts/screenshot-recipes.sh` (the single source of truth), used by both
`scripts/screenshot.sh` (single shot) and `scripts/screenshot-all.sh` (batch).

## Screenshots

`scripts/screenshot.sh` drives the client end to end — it boots a fresh
instance on a private socket, runs the recipe for the requested screen,
captures, and tears the instance down:

```bash
./scripts/screenshot.sh helix-screen motion motion --test      # an overlay
./scripts/screenshot.sh helix-screen zoffset zoffset --test     # a calibration screen
./scripts/screenshot.sh helix-screen preflight preflight-check --test  # a sample-data modal
./scripts/screenshot.sh helix-screen tiny-home home --test -s 480x320  # a size variant
```

See `scripts/screenshot-recipes.sh` for every recognized token.

## Architecture

- **Server** — `src/remote/remote_control_server.cpp`: transport-agnostic
  JSON-RPC dispatch. Runs handlers on the LVGL main thread via the update queue
  + a promise so widget operations execute on the UI thread.
- **Transports** — `IRemoteTransport` (`include/remote_transport.h`) with a
  shared accept loop in `src/remote/socket_server_base.cpp`; two backends:
  `unix_socket_transport.cpp` (default) and `http_transport.cpp` (minimal
  self-contained HTTP/1.1, no libhv HttpServer dependency).
- **Locators & target resolution** — `src/remote/widget_resolution.cpp`
  (`include/widget_resolution.h`): `path_of()` / `path_segment_for()` emit a
  locator, `resolve_path()` reads one back, `parse_indexed_name()` splits a
  `name[k]` token, `resolve_actionable()` decides what a click lands on. Split
  out of the server because `mk/tests.mk` excludes `remote_control_server.o`
  from the test link (it drags in the transports and the toast manager), and
  this is where the interesting mistakes live — see
  `tests/unit/test_remote_target_resolution.cpp`.
- **Client** — `src/remote/remote_client.cpp` (`helix::remote_client_main`,
  dispatched from `src/main.cpp` on the `ctl`/`repl` subcommand). Bundles
  `lib/linenoise` for the REPL. No standalone binary. Owns the working
  directory: a path string, sent as `scope` and re-resolved server-side on every
  command. Never a cached `lv_obj_t*` — that would be a use-after-free the first
  time an overlay closed or hot reload rebuilt the panel underneath it.
- **Sample-data screens** — `helix::show_demo_overlay()` in
  `src/application/application.cpp`.
- **Compile gate** — the whole subsystem is filtered out of the build unless
  `ENABLE_REMOTE_CONTROL=yes` (`Makefile`); the `HELIX_ENABLE_REMOTE_CONTROL`
  define guards the server start/stop, the demo bringup, and the `ctl`/`repl`
  dispatch in `main.cpp`.
