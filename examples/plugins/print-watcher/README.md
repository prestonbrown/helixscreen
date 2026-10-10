# print-watcher

Print-event notifications to ntfy, Discord, Telegram or a generic webhook,
as one adaptive home tile. The printer's state edges - started, resumed,
paused, completed, cancelled, failed - become queued POSTs, each built for
the configured service, and the tile shows what was sent and what failed.

The author guide is [docs/devel/PLUGIN_DEVELOPMENT.md](../../docs/devel/PLUGIN_DEVELOPMENT.md)
(§13 for translations). This is the `http` permission example, and the
worked example for a plugin overlay that controls its own settings.

## What it demonstrates

- `helix.http.post`: the one permission. Payloads differ per service -
  ntfy takes the message as a body with an `X-Title` header, Discord and
  the generic webhook take JSON, Telegram takes `chat_id`/`text` at a
  per-token URL - and each destination is validated to its shape before
  anything reaches the network, so a typo is a status line, not a POST to
  a half-parsed URL.
- The send queue: the sandbox allows two requests in flight; events are
  posted one at a time and the queue drains as each settles, so a burst of
  edges never races that limit. Failures land on the tile's status line -
  the quiet failure policy: no toast, no retry, the line and the log entry
  carry it.
- `helix.printer.watch`: state edges classify into events against the six
  states the screen reports; the filename and progress snapshot rides the
  event, not a later read.
- `helix.settings.get`/`set`/`on_change`: the detail overlay's switches
  write through `set` - the same validation and persistence path the
  generated settings screen uses - and `on_change` mirrors every accepted
  write back, so the overlay, the generated screen and the tile agree no
  matter which surface made the change. Text and enum rows stay in the
  generated screen: a plugin cannot read widget state, so free-form input
  has no path back to Lua.
- `i18n/de.xml`: a translation pack. English source strings resolve
  through it, then the app catalog (the event words translate for free),
  then unchanged; notification payloads follow the screen's language.
  `helix.i18n.on_change` re-renders subject-carried strings on a switch.
- The adaptive size ladder: one component, authored 1x1, resizable 4x2.
  The 1x1 is the bell with the armed word; two columns add the last event
  (or a "No events yet" placeholder that keeps the layout stable); two
  rows swap the glance for the event log; micro heights drop the word and
  let the glyph carry the identity, muted swapping bell for volume_off.

## Run it

```sh
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

Then Settings > Plugins > Print Watcher > enable (it asks to connect to
servers on your network and the internet - the `http` permission), set the
Service and Destination in its settings, and add the tile from the home
panel's widget catalog under the Plugins category. Use "Send test
notification" for a first send without waiting for a print; to watch the
whole flow, run with `HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6` and the mock
print's edges will queue real POSTs to the configured destination.

## Layout

- `manifest.json` id, one 1x1-to-4x2 widget, the `http` permission, the
  service/destination/event settings, the test action row.
- `main.lua` edge classification, per-service payload building and
  validation, the one-at-a-time send queue, settings mirrors and the
  overlay's flip handlers, the size ladder, translation adoption.
- `ui/print-watcher__tile.xml` header, glance zone (bell/word/last event),
  event log zone, destination line, delivery status foot.
- `ui/print-watcher__detail.xml` the control overlay: mute and event
  switches, destination echo, recent-events log, test button.
- `i18n/de.xml` the translation pack; only strings the app catalog does
  not already say.

A destination is deliberately never echoed in full on the tile: the detail
overlay shows it, the generated settings screen edits it.
