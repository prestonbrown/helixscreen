# Print Status Camera Button + Unified Remote-Detection Subject

Date: 2026-08-18
Status: Approved design (user approved scope + both behavior shifts)

## Problem

When HelixScreen runs as a remote screen (not on the printer's own host) and the
printer has a webcam configured, the print status panel offers no way to check
the live print visually. The fullscreen camera viewer already exists
(`camera_fullscreen.xml` + `open_standalone_camera_fullscreen()`), but its only
entry points are the home camera widget and Settings → Hardware → Camera.

Underneath this, "is the screen remote from the printer?" is answered
inconsistently across the codebase by at least five private hand-rolled
variants (loopback-literal string compares, private URL parsers), most of which
miss `::1`, the machine's own hostname, and its own interface IPs. The codebase
already has the complete, correct predicate: `helix::is_moonraker_on_same_host()`
(`src/system/host_identity.cpp`).

## Goals

1. A **Camera** button in the print status panel's control-button row, visible
   exactly when: a webcam is configured AND the screen is remote from the
   printer AND the platform has camera code compiled in.
2. Tapping it opens the existing fullscreen camera overlay.
3. One canonical, live truth for "Moonraker is remote," published as an
   XML-bindable subject, with the existing scattered remote-detection sites
   swept onto it (or onto the shared predicate where a subject is the wrong
   shape).

## Non-goals

- Any new camera streaming/plumbing (overlay, stream reuse, close behavior all
  exist).
- Changing which hosts count as local beyond what `is_moonraker_on_same_host()`
  already computes (loopback literals + own hostname + own interface IPs, with
  per-host caching).
- Touching diagnostic/preset/wizard locality logic (see "Left alone").

## Truth model: `moonraker_is_remote` subject

- **Owner:** `PrinterNetworkState` (`src/printer/printer_network_state.cpp`),
  registered next to `printer_connection_state`, default `0` (unknown/local).
- **Semantics:** 1 when the Moonraker we are *actually connected to* (per the
  live websocket endpoint) does not resolve to this host; 0 otherwise.
- **Update point:** `MoonrakerManager::process_notifications()` on the
  `ConnectionState::CONNECTED` edge (moonraker_manager.cpp:258, where `m_api`
  is already in scope):
  `set_moonraker_is_remote(!is_moonraker_on_same_host(extract_host_from_websocket_url(m_api->get_websocket_url())))`
- Recomputed on **every** CONNECTED (printer switches update it); not cleared on
  disconnect (a stale-remote button that opens a "Connecting Camera…" overlay is
  better than a flapping one; the overlay already handles stream failure).
- **Threading:** `PrinterState::set_moonraker_is_remote()` delegates to
  `PrinterNetworkState` following the exact shape of
  `set_printer_connection_state()` (defers onto the UI thread via
  `async_lifetime_`), since `process_notifications()` may run off-main.
- **Naming:** capability-shaped (`moonraker_is_remote`), matching telemetry's
  `moonraker_is_local` event key. No vendor names.

### Predicate consolidation (Tier B)

`extract_host_from_websocket_url()` moves from
`include/helix_plugin_installer.h` to `include/host_identity.h` (impl into
`host_identity.cpp`), giving "which host are we talking to" one home:
predicate + URL parser together. Update the two existing users
(`helix_plugin_installer.cpp`, `telemetry_manager.cpp`) to the new include.
`moonraker_client.cpp`'s `host_of_endpoint()` stays — it parses `host:port`
endpoints, not scheme URLs; different input shape, not a twin.

## Feature: the button

- **XML:** `ui_xml/print_status_panel.xml` + `ui_xml/portrait/print_status_panel.xml`,
  Row 2 of `button_grid`, between `btn_tune` and `btn_cancel`:
  `<ui_button name="btn_camera" height="#button_height" flex_grow="1" icon="video" text="Camera" translation_tag="Camera">`
- **Visibility — one inline cond (declarative rule 7), no C++ compound observer:**
  `cond="printer_has_webcam eq 1 and moonraker_is_remote eq 1 and platform_extras_available eq 1"`
  - `printer_has_webcam` — existing discovery-driven capability subject.
  - `platform_extras_available` — existing ESP32 cut gate (camera code is
    compiled out there); desktop/embedded builds render as before.
- **Callback:** `on_print_status_camera` registered in
  `register_camera_widget()` (camera_widget.cpp — it already owns
  `on_camera_fullscreen_close` and the standalone entry point) → calls
  `open_standalone_camera_fullscreen(lv_display_get_screen_active(nullptr))`,
  which already: no-ops without a webcam, delegates to an attached home
  CameraWidget's live stream when one exists (single-MJPEG-client safe), and
  creates the standalone overlay otherwise.
- Visible in **all** panel states (preparing/printing/paused/terminal) — user
  decision; still useful post-failure to eyeball the result.
- **Translation:** new user-facing string "Camera" → wrap in
  `make translation-sync` + `make translations` workflow, stage YAMLs +
  `ui_xml/translations/*.xml` (L064).
- Icon `video` already exists (used by the Settings camera row) — no font regen.

## The sweep (approved behavior shifts)

**Shift 1 — stricter predicate.** Sites using loopback-literal-only checks start
matching own-hostname/interface-IP hosts as local. Approved: loopback-only is
too simplistic and misses real same-host shapes.

**Shift 2 — live endpoint over Config host.** Sites reading
`Config moonraker_host` switch to the connected-endpoint truth, so they stop
going stale across printer switches. Approved.

### Tier A — UI-shaped behavior → consume the subject

| Site | Today | Becomes |
|------|-------|---------|
| `src/ui/ui_overlay_timelapse_videos.cpp:654-676` | hand-rolls WS-URL parse + `timelapse::is_local_host()` into `is_local_moonraker_` | read `moonraker_is_remote` at refresh; delete the parser block |
| `src/ui/panel_widgets/printer_image_widget.cpp:182` | `host != "127.0.0.1" && host != "localhost"` remote check | read/observe subject at the fetch decision |
| `src/ui/panel_widgets/shutdown_widget.cpp:388-391` | Config-host read at setup → `allow_local_fallback` | observe subject (value may arrive after widget attach; PanelWidget instances are recycled, so apply from `attach()` too) |
| `src/ui/ui_panel_input_shaper.cpp:1021-1029` | Config-host read → result interpretation | read subject |
| `src/ui/ui_spoolman_overlay.cpp:1250-1255` | Config-host read → gates config-path feature | read subject at overlay open |

Constraint: subject reads happen on the main thread (observer guards or
main-thread reads at existing decision points); background-thread decisions
capture the value at queue time.

Two near-misses resolved explicitly:

- `ui_change_host_modal.cpp:462-474` — UI-shaped (wording of the "Moonraker
  service" advice) → **Tier A**, reads the subject.
- `ams_backend_ad5x_ifs.cpp:3027-3033` — runs during backend construction in
  subsystem init, before discovery settles the subject; a subject read there is
  both too early and off the UI path → **predicate-based**: swap its private
  Config read + `is_moonraker_on_same_host()` call stays, using the relocated
  canonical functions only.

### Left alone (different semantics or wrong layer)

- `debug_bundle_collector.cpp:867,1682` — point-in-time diagnostic snapshot.
- `moonraker_client.cpp:906` — error wording on the WS background thread with
  the endpoint already in hand; cannot read subjects there.
- `telemetry_manager.cpp:1987` — event-time classification, stays
  predicate-based (but uses the relocated `extract_host_from_websocket_url`).
- `config.cpp` `preset_targets_this_device()` — runs pre-connect by definition.
- Wizard probing / `moonraker_discovery_sequence` localhost camera probe —
  probing, not locality gating.
- `timelapse_thumbnailer.cpp` local check — background-thread work, keeps
  predicate (relocate parser only if it duplicates; else untouched).

## Mock & dev verification

- `--test` mock connects to loopback → `moonraker_is_remote` = 0 → button hidden
  (correct: a mock IS the same host). Mock always reports a webcam.
- New `HELIX_MOCK_REMOTE_PRINTER=1` (documented in
  `docs/devel/ENVIRONMENT_VARIABLES.md`, Mock & Testing section) following the
  `RuntimeConfig::should_mock_*()` accessor pattern (no auto-mock rule); it
  forces the subject to 1 in the MoonrakerManager update so the button + Tier A
  paths are drivable via `ctl` in `--test`.
- Mock client already records its URL (`set_last_url`), so locality reads are
  realistic; the env var only overrides the verdict.

## Testing

- **Subject lifecycle:** default 0; set to 1/0 on connected edges; printer
  switch flips it (unit test on PrinterNetworkState via PrinterState façade,
  `HelixTestFixture` base).
- **Mock override:** `should_mock_remote_printer()` parsing + forced-1 path.
- **Button gating:** XML fixture test — all 8 combinations of the three gate
  subjects → `btn_camera` hidden flag (pattern: existing print-status XML
  tests).
- **Sweep regressions:** per Tier A site, assert behavior follows the subject
  (e.g. timelapse overlay picks remote vs local playback when subject flips);
  each site's existing tests updated from loopback-literal expectations to
  predicate-based ones.
- Manual: `HELIX_MOCK_REMOTE_PRINTER=1 ./build/bin/helix-screen --test -vv`,
  `ctl navigate print_status` (or auto-print), verify button → fullscreen →
  close; screenshot for the record.

## Docs

- `ENVIRONMENT_VARIABLES.md`: `HELIX_MOCK_REMOTE_PRINTER` entry.
- `host_identity.h` comment updated to mention the subject as the UI-facing
  mirror of the predicate.
