# Timelapse Feature

Integration with [moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse) plugin for automated timelapse recording during prints.

---

## What It Provides

HelixScreen does not implement timelapse recording itself. It provides:

1. **Plugin detection** -- discovers whether moonraker-timelapse is installed during printer connection
2. **Install wizard** -- guides users through SSH install, moonraker.conf configuration, and restart/verification
3. **Settings UI** -- configures the plugin (enable/disable, recording mode, framerate, auto-render)
4. **Print status toggle** -- quick enable/disable button on the print status panel
5. **Real-time event handling** -- subscribes to WebSocket `notify_timelapse_event` for frame capture and render progress tracking
6. **Render progress notifications** -- toast notification at render start and on completion
7. **Video browser** -- dedicated video browser overlay with thumbnail grid, responsive cards, render controls, and video playback

All actual frame capture, rendering, and video storage is handled by the moonraker-timelapse plugin on the printer.

---

## Plugin Detection

moonraker-timelapse is a Moonraker component (`moonraker.conf`), not a Klipper object, so it never appears in `printer.objects.list`. The discovery sequence detects it in Moonraker's component list (`src/api/moonraker_discovery_sequence.cpp`): it calls `PrinterState::set_timelapse_available(false)` first, so switching to a printer without the plugin clears it, then `set_timelapse_available(true)` when `timelapse` is listed.

This flows through:
1. `PrinterCapabilitiesState::set_hardware()` -- ORs `PrinterDiscovery::has_timelapse()` (a Klipper `[timelapse]` object, if one ever exists) with the component-detected value, so the hardware batch never clears a detected plugin, and sets the `printer_has_timelapse` subject
2. XML bindings react: timelapse settings row shown (plugin installed) or setup row shown (plugin missing + webcam present)

The advanced panel uses dual visibility binding for the setup row:
```xml
<bind_flag_if_not_eq subject="timelapse_owner" flag="hidden" ref_value="0"/>
<bind_flag_if_eq subject="printer_has_webcam" flag="hidden" ref_value="0"/>
```
This means "show when nothing records timelapses AND webcam IS available."

### Firmware-owned timelapse

A printer whose database entry declares a pre-print option with `"capability": "timelapse"` records timelapses in its firmware (Snapmaker U1: `u1_timelapse`, which writes `time_lapse_camera`). That firmware can also register a stub `timelapse` Moonraker component that answers `/machine/timelapse/settings` with `{"enabled": false}` and ignores writes, so `printer_has_timelapse` alone does not mean the plugin works.

The `timelapse_owner` subject (`TimelapseOwner`: 0 none, 1 plugin, 2 firmware) carries the decision. Firmware outranks the plugin. The plugin settings row and the Print Status button need owner 1, and the install row needs owner 0. On a firmware-owned printer the pre-print option is the only control. The Timelapse Videos row, the printer manager chip and `has_any_preprint_options` still read `printer_has_timelapse`.

---

## Install Wizard

`TimelapseInstallOverlay` is a 6-step wizard overlay opened from the "Setup Timelapse" row in the Advanced panel.

| Step | Action |
|------|--------|
| 1. Check webcam | `api->get_webcam_list()` -- abort if none found |
| 2. Check plugin | `api->get_timelapse_settings()` -- skip to done if already responding |
| 3. SSH instructions | Show git clone + make install commands; user taps "Check Again" |
| 4. Configure Moonraker | Download `moonraker.conf`, check/append `[timelapse]` + `[update_manager timelapse]` sections, upload |
| 5. Restart Moonraker | `api->restart_moonraker()` with 20 s disconnect suppression |
| 6. Verify | Re-query `get_timelapse_settings()` after 8s delay, update capability state on success |

Key patterns:
- **Alive guard**: `std::shared_ptr<bool>` captures prevent callbacks from touching destroyed overlay
- **Config modification**: `has_timelapse_section()` / `append_timelapse_config()` are pure static functions, public for testability (`tests/unit/test_timelapse_install.cpp`)
- **Disconnect suppression**: `EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::LONG)` prevents the disconnect modal during intentional Moonraker restart

---

## Settings UI

`TimelapseSettingsOverlay` provides configuration when the plugin is installed. Opened from the "Timelapse" row in Advanced panel (visible when `printer_has_timelapse == 1`).

| Setting | Options | API |
|---------|---------|-----|
| Enable Timelapse | Toggle | `enabled` param |
| Recording Mode | Layer Macro / Hyperlapse | `mode` param |
| Framerate | 15 / 24 / 30 / 60 fps | `output_framerate` param |
| Auto-render | Toggle | `autorender` param |

Settings are read via `GET /machine/timelapse/settings` and saved via `POST /machine/timelapse/settings?param=value` (query string format, not JSON body).

---

## Print Status Toggle

`PrintLightTimelapseControls` manages the timelapse toggle button on the print status panel. It:
- Shows a video/video-off icon via the `timelapse_button_icon` string subject, from the `timelapse_enabled` subject: the plugin's global setting, seeded at discovery and refreshed by every successful write from this button, the settings overlay and the pre-print toggle
- Calls `api->set_timelapse_enabled(bool)` on click
- Is hidden unless `timelapse_owner` is 1 (plugin), because firmware-owned timelapse is set per print and a plugin write does nothing there
- Uses `ui_queue_update()` to marshal UI updates from API callback thread to LVGL thread

This class also manages the light button (LED toggle) -- they share the same helper since both appear as action buttons on the print status panel.

---

## Moonraker API Methods

The timelapse methods are on the `ITimelapseAPI` sub-API (`api->timelapse()`, declared in `include/i_moonraker_sub_apis.h`, implemented in `src/api/moonraker_timelapse_api.cpp`); `restart_moonraker()` is on `IMoonrakerAPI` and the file calls are on the files sub-API:

| Method | HTTP | Endpoint |
|--------|------|----------|
| `get_timelapse_settings()` | GET | `/machine/timelapse/settings` |
| `set_timelapse_settings()` | POST | `/machine/timelapse/settings?params...` |
| `set_timelapse_enabled()` | POST | `/machine/timelapse/settings?enabled=True/False` |
| `get_webcam_list()` | GET | `/server/webcams/list` |
| `restart_moonraker()` | POST | `/server/restart` |
| `render_timelapse()` | POST | `/machine/timelapse/render` |
| `save_timelapse_frames()` | POST | `/machine/timelapse/saveframes` |
| `get_last_frame_info()` | GET | `/machine/timelapse/lastframeinfo` |
| `list_files("timelapse")` | GET | `/server/files/list?root=timelapse` |
| `delete_file("timelapse/...")` | DELETE | `/server/files/timelapse/...` |

---

## Event Handling & Video Management

### TimelapseState

`TimelapseState` is a singleton (`TimelapseState::instance()`) that manages timelapse-specific state via LVGL subjects:

| Subject | Type | Purpose |
|---------|------|---------|
| `timelapse_render_progress` | int (0-100) | Current render progress percentage |
| `timelapse_render_status` | string | Render state: `"idle"`, `"rendering"`, `"complete"`, `"error"` |
| `timelapse_frame_count` | int | Number of frames captured during current print |
| `timelapse_capture_info` | string | Print filename and timestamp (e.g., "benchy.gcode · Mar 10, 14:32") |

Subjects are initialized in `subject_initializer.cpp` and frame count resets when a new print starts.

### Event Handling

The moonraker-timelapse plugin emits `notify_timelapse_event` WebSocket notifications. `TimelapseState::attach(client)` registers the `timelapse_state` handler for them and `detach(client)` removes it (`src/printer/timelapse_state.cpp`).

`TimelapseState::handle_timelapse_event()` dispatches based on the `action` field:

- **`"newframe"`** -- increments `timelapse_frame_count` subject
- **`"render"`** -- updates `timelapse_render_status` and `timelapse_render_progress`; triggers toast notifications at 25% progress boundaries to avoid UI spam

### Render Progress Notifications

Render events produce user-visible toast notifications:

| Event | Notification |
|-------|-------------|
| Render starts (first progress event) | "Rendering timelapse..." |
| Render complete (`status == "success"`) | "Timelapse rendered successfully" |
| Render error (`status == "error"`) | "Timelapse render failed: {msg}" |

On render success, the frame count resets to 0 and the render section hides automatically.

### Video Management

The timelapse settings overlay manages videos:

| Action | API Call | Description |
|--------|----------|-------------|
| List videos | `list_files("timelapse", ...)` | Shows rendered timelapse videos |
| Render video | `render_timelapse()` | POST `/machine/timelapse/render` -- triggers manual render |
| Save frames | `save_timelapse_frames()` | POST `/machine/timelapse/saveframes` -- saves frames as ZIP |
| Get last frame | `get_last_frame_info()` | GET `/machine/timelapse/lastframeinfo` -- info about last captured frame |
| Delete video | `delete_file("timelapse/filename.mp4")` | Removes a rendered timelapse file |

### Video Browser Overlay

`TimelapseVideosOverlay` provides a dedicated overlay for browsing, rendering, and playing timelapse videos. Accessed from the **Advanced > Timelapse Videos** row.

**Features:**
- **Responsive thumbnail grid** - cards auto-size to fit 2 rows on screen using `calculate_card_dimensions()` (same pattern as print file selection)
- **Companion thumbnails** - the preview moonraker-timelapse renders as `<name>.jpg` beside each video (`<name>.thumb.jpg` is also accepted), fetched from the `timelapse` root
- **Thumbnail caching** - uses the shared `ThumbnailCache` (`get_thumbnail_cache()`, keyed by `thumbnail_cache_id()`)
- **Render controls** - frame count with capture info (filename + date), progress bar with inline status text, Render Now button
- **Video playback** - the in-app player (below) when ffmpeg is installed and the board can decode, else a QR code a phone scans to watch
- **Delete with confirmation** - long-press card for delete option, confirmation dialog before removal
- **Generation-based stale rejection** - `nav_generation_` and `thumb_generation_` counters prevent stale async callbacks from modifying UI after navigation changes

**What a tap does** is one pure rule, `helix::timelapse::choose_video_open_action()`
(`include/timelapse_playback.h`), which the card tap and History's open both follow:

| ffmpeg on PATH, the board decodes, no print running | Moonraker | Action |
|---|---|---|
| yes | same host | play the file in place |
| yes | remote | download to the runtime dir, then play the copy |
| no | any | QR code of the video's URL for a phone, else a toast |

"The board decodes" is `inapp_player_supported()`: every PlatformTier but EMBEDDED. While a
print holds the machine a tap goes to the phone, the same rule that holds back thumbnail
extraction: a full-length decode is CPU Klipper needs. Each card's badge says which (`tap_badge_for()`).

### In-app player

`TimelapsePlayerOverlay` (`include/ui_overlay_timelapse_player.h`,
`ui_xml/timelapse_player_overlay.xml`) plays a video on the app's own screen, on every
display backend. It never links libav: `PlayerDecoder` (`include/timelapse_player_decoder.h`)
runs `ffmpeg` as a child with its stdout on a pipe.

- **Probe** - `ffprobe` for size, frame rate and duration; ffmpeg's own stderr header when
  ffprobe is missing.
- **Decode** - `ffmpeg -ss <t> -i <file> -an -vf scale=WxH -pix_fmt <fmt> -f rawvideo -`, the
  size fitted to the screen with the video's aspect (`fit_frame()`), the format the display's
  own: `rgb565le` for a 16-bit display, `bgr24` for 24, `bgra` (LVGL XRGB8888) for 32. One
  thread for the decoder, the filters and the raw encoder: each defaults to one thread per
  core, every thread holding its own frames, which more than doubled ffmpeg's memory. A
  small `-probesize`/`-analyzeduration` (a local file's header says enough); never
  `-fflags nobuffer`, which ends an MP4 early. The child runs at nice 10 and dies with the
  app (`PR_SET_PDEATHSIG`). It starts through `helix::spawn_child()`
  (`include/system/child_process.h`), the one fork-and-exec the plugin installer's shell
  runs use too: signals blocked across the fork and reset in the child, every inherited fd
  above stderr closed.
- **Frames** - a worker thread cuts the stream into frames (`FrameAssembler`) and queues
  each in `FrameExchange` one frame period before it is due, stamped with its due time on
  the monotonic clock (`pace_frame()`). A main-thread timer looking every quarter period
  (`present_poll_ms()`, 4 to 10 ms) copies the newest due frame (`pick_due_frame()`) into
  one draw buffer and invalidates the image; it stops while paused. Neither side waits on
  the other: a UI more than a period late skips frames, and a full queue drops its oldest.
- **Controls** - tap toggles the bars, which hide after 3 s while playing; play/pause (pause
  stops reading, so ffmpeg blocks on the pipe); a scrub bar that seeks on release by
  replacing the child with one started at `-ss`; at the end the last frame stays with a
  replay button.
- **Seeks** - every seek starts a new decode run with a higher generation. The exchange
  discards frames from older runs (`begin_generation()`), and the overlay ignores an end
  report from one, so a run that ended just as a seek landed cannot leave the player on
  its Replay screen.
- **Lifetime** - leaving the overlay, destroying it and seeking each kill and reap the child
  (`terminate_and_reap()`); the worker is joined before the decoder goes away.

Measured on the desktop mock (headless SDL, XRGB8888), 20 s testsrc clips at 30 fps, on a
32-thread host running other builds (load average 18 to 46):

| Clip | Screen | Decoded at | Added RSS | Main thread | Whole app | ffmpeg CPU | ffmpeg peak RSS | Dropped of 600 |
|---|---|---|---|---|---|---|---|---|
| 1280x720 H.264 | 800x480 | 800x450 | ~12 MB | 3.2% | 6.1% | 9.7% | 80 MB | 1 |
| 640x480 H.264 | 800x480 | 640x480 | ~10 MB | 2.4% | 4.6% | 3.4% | 71 MB | 1 |
| 1280x720 H.264 | 480x272 | 480x270 | ~6 MB | 2.2% | 3.8% | 7.1% | 69 MB | 1 |
| 640x480 H.264 | 480x272 | 362x272 | ~5 MB | 2.2% | 3.6% | 3.6% | 61 MB | 1 |

CPU is percent of one core on a desktop x86 host; a printer's board costs several times
more. ffmpeg's peak RSS includes about 50 MB that this host's ffmpeg maps for its libraries
before decoding anything; with the threads defaulted, the 1280x720 decode peaked at 125 MB
on 32 cores and 86 MB on 4. The stats line at close puts every frame in one bucket: shown,
skipped or pushed out because the UI was late, pushed out because the decoder ran ahead,
or discarded unshown at a seek or close.

**Thread safety:**
- All subject updates via `helix::ui::queue_update()`
- `alive_` shared_ptr pattern prevents callbacks after overlay destruction
- `ScopedFreeze` on UpdateQueue during `clear_video_grid()` drain+destroy sequence

### Timelapse Thumbnailer

`include/timelapse_thumbnailer.h` (namespace `helix::timelapse`, implemented in `src/print/timelapse_thumbnailer.cpp`) provides utility functions:

| Function | Purpose |
|----------|---------|
| `find_companion_thumbnail(video, listing)` | The preview beside a video: moonraker-timelapse's `video.jpg`, else `video.thumb.jpg` |
| `ffmpeg_extract_args(input, output)` | Builds ffmpeg command to extract first frame as JPEG |
| `is_video_file(filename)` | Checks for `.mp4`, `.mkv`, `.avi` extensions |

Local vs remote playback is decided by the `moonraker_is_remote` subject
(`PrinterState::is_moonraker_remote()`) - the live websocket endpoint's verdict -
not by a hostname check in the thumbnailer.

---

## File Map

| File | Purpose |
|------|---------|
| `include/ui_overlay_timelapse_install.h` | Install wizard overlay class |
| `src/ui/print/ui_overlay_timelapse_install.cpp` | Wizard step implementation |
| `ui_xml/timelapse_install_overlay.xml` | Install wizard layout (step progress, SSH instructions, action button) |
| `include/ui_overlay_timelapse_settings.h` | Settings overlay class |
| `src/ui/print/ui_overlay_timelapse_settings.cpp` | Settings fetch/save, event handlers |
| `ui_xml/timelapse_settings_overlay.xml` | Settings layout (toggles, dropdowns) |
| `include/ui_print_light_timelapse.h` | Print status light + timelapse button helper |
| `src/ui/print/ui_print_light_timelapse.cpp` | Toggle handlers, subject management |
| `include/printer_capabilities_state.h` | `printer_has_timelapse` subject |
| `src/printer/printer_capabilities_state.cpp` | Capability-gated setter (`printer_has_timelapse`) |
| `include/printer_discovery.h` | `has_timelapse()` detection from Moonraker objects |
| `include/moonraker_types.h` | `TimelapseSettings` struct |
| `src/api/moonraker_timelapse_api.cpp` | HTTP API methods for timelapse |
| `src/api/moonraker_api_controls.cpp` | Contains `restart_moonraker()` |
| `tests/unit/test_timelapse_install.cpp` | Config parsing tests |
| `ui_xml/advanced_panel.xml` | Timelapse/Setup rows (capability-gated on `printer_has_timelapse`) |
| `include/timelapse_state.h` | TimelapseState singleton class |
| `src/printer/timelapse_state.cpp` | Event dispatch, subject management, render notifications |
| `tests/unit/test_timelapse_state.cpp` | Unit tests for TimelapseState event handling and notifications |
| `include/ui_overlay_timelapse_videos.h` | Video browser overlay class |
| `src/ui/print/ui_overlay_timelapse_videos.cpp` | Video grid, thumbnails, playback, render controls |
| `ui_xml/timelapse_videos_overlay.xml` | Video browser layout (render section, grid, empty state) |
| `ui_xml/timelapse_video_card.xml` | Individual video card component (thumbnail, gradient, labels) |
| `include/timelapse_thumbnailer.h` | Thumbnail extraction and playback utilities |
| `src/print/timelapse_thumbnailer.cpp` | Utility function implementations |
| `tests/unit/test_timelapse_videos.cpp` | Phone URL tests |
| `include/timelapse_playback.h` | Open-action rule, probe parsing, ffmpeg arguments, frame math, pacing, frame assembly and handoff |
| `include/timelapse_player_decoder.h` | ffmpeg child + worker thread that feeds the player |
| `include/ui_overlay_timelapse_player.h` | The in-app player overlay |
| `ui_xml/timelapse_player_overlay.xml` | Player layout (frame, bars, scrub, E-stop) |
| `tests/unit/test_timelapse_playback.cpp` | Tests for the pure player rules |
| `tests/unit/test_timelapse_player_decoder.cpp` | Decoder lifetime: stop, seek, pause, end, reaping |
| `include/display_format.h` | `format_short_date()` for smart date display |
| `src/system/locale_formats.cpp` | `format_localized_short_date()` with 4 locale patterns |
