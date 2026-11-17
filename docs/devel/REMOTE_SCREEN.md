# Built-in Screen Server

HelixScreen serves its own UI as a Mainsail/Fluidd webcam and accepts taps and drags back from a browser (prestonbrown/helixscreen#1825). It runs only where the firmware ships no screen server of its own. User-facing description: `docs/user/guide/settings/connection.md` § "Screen Sharing".

## Key files

| File | Role |
|---|---|
| `include/remote_screen_sink.h` | `RemoteScreenFrame` (one flushed dirty rect) and the `RemoteScreenSink` interface |
| `include/remote_screen_blit.h` | `blit_remote_frame`: the one dirty-rect copy every sink uses, with both bounds guards |
| `include/remote_screen_server_sink.h` | `ScreenFrameStore` (shadow frame, generation, JPEG cache, viewer gating) and `ScreenServerSink` |
| `include/jpeg_encode.h` | RGB888 conversion and the vendored `lib/stb/stb_image_write.h` encoder |
| `include/remote_screen_http_server.h` | `ScreenHttpServer`: the libhv routes |
| `include/remote_screen_control.h` | Touch parsing, the request gate, `PairingStore`, `TouchSession` |
| `include/remote_screen_service.h` | `RemoteScreenService`: owns the server for the app's lifetime, the hooks into LVGL |
| `include/remote_screen_settings_manager.h` | The settings and the subjects the Screen Sharing page binds to |
| `include/ui_settings_screen_sharing.h` | The Screen Sharing page under Settings > Connection: the toggles and the pairing rows |
| `include/remote_screen_webcam.h` | The Moonraker webcam entry and its sync |
| `include/screen_secrets.h` | The widgets that show a secret, and whether one is on the display |
| `include/bitmap_font_5x7.h` | The font the placeholder frame is drawn with |
| `include/remote_pointer.h` | The synthetic LVGL pointer shared with `helix-screen ctl` |
| `assets/remote_screen/index.html` | The page: polls snapshots, sends pointer events, pairs |
| `scripts/lib/installer/remote_screen.sh` | The nginx route and its removal |

## Data path

```
flush_cb hook (main)          ScreenFrameStore                 ScreenHttpServer (libhv thread)
DisplayManager::install_ ──▶  shadow frame (logical size) ──▶  GET snapshot.jpg / stream.mjpg
color_transform_hook          generation++                     RGB888 under the lock, encode outside
                                                               JPEG cached by generation, rate-capped
```

- **Frames are logical.** The hook wraps the backend's `flush_cb`, and the fbdev driver rotates inside that callback (`lib/lvgl/src/drivers/display/fb/lv_linux_fbdev.c#"Not all framebuffer kernel drivers support hardware rotation"`), so the hook sees what the user sees. The shadow is `disp_w × disp_h` and the served image is upright.
- **No viewer, no work.** `ScreenFrameStore::wants_frames()` is true only within `kViewerIdleMs` (10 s) of a request or while a stream is open. The shadow is allocated by the first frame after a request and freed by the housekeeping tick once the viewer is gone. The first request after idle asks the UI for a full repaint (`request_full_redraw`) and waits up to `kFirstFrameWaitMs`; with nothing captured it answers 503 `Retry-After: 1`.
- **Rate caps** (`snapshot_rate_cap`): 5 fps idle; while printing 2 fps, or 1 fps on a constrained host (`MemoryInfo::is_constrained_device()` or one core). A static screen costs one encode in total: the cache is keyed on the frame generation.
- **Memory while watched:** shadow `W×H×bpp` + RGB888 `W×H×3` + one JPEG. About 2.8 MB at 800×480×32.

## Idle: a watched display never sleeps

A sleeping panel serves a black image (software overlay) or a frozen one (panel power-off swaps the flush for a no-op, so the capture hook stops seeing frames; `DisplaySleepController::suppress_flush_for_sleep`). Hardware blank (`FBIOBLANK`) keeps rendering, so the image stays live while the panel is dark. Dim only lowers the backlight and changes nothing served; the screensaver is drawn by LVGL, so a viewer sees it.

**Who counts as a viewer.** Only the HelixScreen page (`assets/remote_screen/index.html`, also what Mainsail and Fluidd embed as an `iframe` with control on) marks its snapshot polls with `?viewer=1` (`kViewerPageParam`), and only that marker feeds `ScreenFrameStore::note_page_view` and `page_viewer_present`. Every other request (an unmarked `snapshot.jpg`, `stream.mjpg`, `POST touch`) feeds only `note_request` / `wants_frames`, which decides whether frames are captured and nothing else. The screen is published as a Moonraker webcam, so Obico, OctoEverywhere, Home Assistant camera entities, moonraker-timelapse and a view-only `mjpegstreamer` tab all poll it unattended; counting them would keep the display from ever sleeping and wake it (into the PIN lock) whenever they resume. A touch never counts, so it cannot race the wake latch as an arrival.

While `RemoteScreenService::viewer_active` is true (the server is up and the page polled within `kViewerIdleMs`), `decide_idle`'s `remote_viewer` input caps idle at the dim stage: a due sleep becomes a dim or screensaver start, and an already dimmed display stays there. The page polls every 200 ms while visible (1 s on errors) and stops when hidden, so 10 s covers a slow poll and releases a closed or hidden tab within ten seconds. Its `info` poll keeps running in a hidden tab and is unmarked. When the viewer goes, an overdue sleep happens on the next tick. A viewer arriving (`remote_viewer_arrived`, the tick `viewer_active` turns true) wakes a sleeping display the way a touch does, auto-lock included; a viewer that stays never wakes it again. The controller logs at info when the hold engages and releases, and when a held sleep dims or starts the screensaver.

## Routes

All relative to the mount (`/helixscreen/screen/` behind nginx, `/` on the LAN port).

| Route | Purpose |
|---|---|
| `GET snapshot.jpg` | One JPEG, query string ignored, `Cache-Control: no-store` |
| `GET stream.mjpg` | `multipart/x-mixed-replace; boundary=helixframe`, one part per new frame. One joinable thread feeds every open stream, at most 4 |
| `GET info` | `{w, h, control, paired}` |
| `GET /` | The page |
| `POST touch` | `{type: down|move|up, x, y}` in image pixels |
| `POST pair` | `{code}` → the pairing cookie |

## Control

`POST touch` is handed to the UI thread (`ui_queue_update`), which wakes the display and calls `RemotePointer::set_logical_state`. The HTTP thread then waits for two indev samples (`RemotePointer::wait_for_samples`), so a fast press/release pair is never coalesced away. LVGL rotates every pointer indev from physical to logical, so `set_logical_state` applies the inverse rotation (`remote_image_to_indev_point`); `ctl`'s pointer commands use the same path.

`TouchSession` releases a press that nothing followed within `kTouchStaleMs` (3 s) and drives the `remote_screen_control_active` subject behind the corner badge (`ui_xml/remote_control_indicator.xml`, on `lv_layer_top()`).

## Security model

1. Off until the user turns it on, on the device. Control is a second opt-in and only holds while view is on. View, control and the webcam entry are per printer (`/printers/<id>/remote_screen/{enabled,control,webcam}`); the pairing code, paired tokens and port belong to the screen and stay global.
2. **Behind nginx** (`config/.remote_screen_route` exists and Moonraker runs on this host): the server binds `127.0.0.1`. The installer's `location /helixscreen/screen/` block runs `auth_request` against Moonraker's `/access/user`, so whoever Moonraker would let in may view and control. The block forwards `Host $http_host`, which the Origin check needs, and passes the client's other headers on unchanged (nginx's default), `X-Forwarded-Host` and `Forwarded` among them. A tagged `location = /helixscreen/screen { return 301 screen/; }` answers the mount without its slash: nginx's own redirect would name its scheme, host and port, which is wrong behind a TLS or path-prefixing reverse proxy that does not rewrite `Location`. A rerun of the installer adds that line to a route an older installer wrote. The installer adds the block only when nginx has `auth_request` and Moonraker answers `/access/user` with 200 or 401; otherwise it writes no route file and the server faces the LAN.
3. **On the LAN** (no route file, or a screen pointed at another host's Moonraker): the server binds `0.0.0.0`. View is open; control needs a paired browser. The device shows a 6-digit code (`/remote_screen/pair_secret`); `POST pair` with it returns a 128-bit token as an `HttpOnly; SameSite=Strict` one-year cookie. Up to 8 browsers stay paired, least recently used evicted. 5 wrong codes per minute, then 429; 50 cumulative wrong codes rotate the code. "Forget Paired Browsers" clears all tokens and rotates the code.
4. **The code never leaves the device in a frame.** The Screen Sharing page marks its pairing code row with `helix::ui::mark_screen_secret`. The flush hook stamps every frame with `helix::ui::screen_secret_state()` (main thread): Shown while a marked widget is `lv_obj_is_visible`, Unknown with no display. A frame stamped anything but Absent leaves the shadow unclean, and `ScreenFrameStore::snapshot` then serves a placeholder (gray, `kScreenHiddenMessage` centered) on both `snapshot.jpg` and `stream.mjpg`. The shadow is clean again only when a full repaint drawn with the code Absent completes, which the store asks for through `note_request`. The page itself stops polling while it shows the pairing prompt. Any new widget that renders the code must be marked the same way.
5. **Cross-site requests** (`check_touch_request`): `touch` and `pair` need `Content-Type: application/json` and `X-Helix-Touch: 1`, which together force a CORS preflight the server never answers, and an `Origin` (when sent) whose authority matches `Host`. Behind nginx only, an `Origin` matching the forwarded host (`trusted_forwarded_host`: the first `X-Forwarded-Host` entry, else `host=` of the first `Forwarded` element) is accepted too, so a reverse proxy in front of the printer that rewrites `Host` still lets taps through. That is safe there because the server binds `127.0.0.1` and every request came through the auth-checked route; a browser cannot add either header cross-site without the preflight above. Facing the LAN the headers are ignored, since anyone can send them, and pairing is still required.

## Why nginx, and no Moonraker-plugin route

Measured on a K1C (the weakest host class) during a motion-only print, three 170 s windows:

| Window | helix-screen CPU | Moonraker CPU | Klipper buffer_time min / avg | Timer too close |
|---|---|---|---|---|
| No viewer | 14% | 6% | 2.001 / 2.307 s | 0 |
| Snapshot polling, 5 req/s | 18% | 6% | 2.007 / 2.347 s | 0 |
| MJPEG stream open | 13% | 6% | 2.000 / 2.326 s | 0 |

Serving the screen directly costs Moonraker nothing and leaves Klipper's margin unchanged;
the stream ran at 0.94 fps against the 1 fps cap. A route through the `helix_print`
plugin could only add a JPEG relay inside Moonraker's single-threaded loop, so it was not
built. Where nginx has no `auth_request` (the K1 Helper Script and K2 builds both lack it)
the server faces the LAN with pairing; the Pi's MainsailOS nginx has it.

## Where it runs

`remote_screen_firmware_provides()` decides: a platform hook that exports `HELIX_REMOTE_SCREEN=firmware`, or sets up the U1's fb0 mirror (`HELIX_REMOTE_SCREEN_FB0`), means the firmware serves the screen and the Screen Sharing row stays hidden. Android never runs it. The K-Touch (ESP32) firmware compiles only the settings manager; everything else is in `app_srcs_excluded.txt`.

## Moonraker webcam

`sync_remote_screen_webcam` runs on every connect (discovery tail step `remote_screen_webcam`) and on every settings change. It re-decides the server's mode for the connected printer and, while the server runs and that printer's **Add to Mainsail/Fluidd Webcams** (`remote_screen/webcam`, default on) is set, posts one entry, `HelixScreen Screen` (with ` (<hostname>)` appended when the Moonraker is on another host, so two screens never overwrite each other): `iframe` of the page with control on, `mjpegstreamer` on `stream.mjpg` view-only, root-relative on the mount behind nginx (`/helixscreen/screen/...`) and absolute (`http://<lan ip>:<port>/`) otherwise. No URL form keeps a reverse proxy's path prefix: Mainsail resolves webcam URLs against `<scheme>://<hostname>/` (its webcam mixin's `convertUrl`, from the socket store's `getHostUrl`) and Fluidd against the page's origin (its camera mixin's `buildAbsoluteUrl`), so a path without a leading slash resolves exactly like a root-relative one. Root-relative is kept because Mainsail also moves it onto the web UI's own port when that is not 80 (a K1's 4408/4409); a plain relative path loses that. Behind a prefixing proxy, the page itself (`<prefix>/helixscreen/screen/`) works, since it uses only relative URLs. On the LAN the address is plain `http`, so an `https` Mainsail blocks it as mixed content and it is not reachable from outside the network. With sharing or the webcam setting off it deletes the entry: by the uid stored for that printer (`remote_screen_webcam_uid`), else only if one is listed. Both off paths are the same reconcile with no item, so turning the webcam setting off and on adds a fresh entry just as toggling sharing does.

**Printer switch.** The settings are per printer, so `RemoteScreenSettingsManager` registers a `PrinterCacheRegistry` invalidator. On a switch it reloads view, control and webcam for the new printer and calls the service's printer handler, which runs only `apply_server()`: the server stops, starts or changes control to match, before the previous connection is dropped. It does not touch Moonraker then, because the API still belongs to the previous printer; the new printer's connect reconciles its own list. The previous printer's entry stays registered and serves nothing until the screen is switched back to it. A config from before this split had a global `/remote_screen/enabled`; config migration v31 moves it to the active printer only. Discovery skips every such entry (`webcam::is_own_screen_webcam`) so the camera view never shows the screen it is drawn on.

## Tests

| Tag | Covers |
|---|---|
| `[remote_screen][blit]` | Format pairs, partial-mode origin, both bounds guards |
| `[jpeg]` | Encode/decode round trip, conversion |
| `[remote_screen][server_sink]` | Viewer gating, cache, rate cap, resize |
| `[remote_screen][secret]` | Placeholder while the code is shown, the full-repaint rule, the marked-widget state, the Screen Sharing page's row |
| `[remote_screen][http]` | Routes, 503 path, stream, stop() promptness |
| `[remote_screen][control]` | Gate table, forwarded host, pairing, stale press, indicator, touches through LVGL |
| `[remote_screen][settings]` | Settings rules, service start/stop |
| `[remote_screen][webcam]` | Entry shapes, mock round trip, discovery self-exclusion |
| `[remote_pointer]` | Rotation round trip at 0/90/180/270 |
| `[remote_viewer]` | The viewer signal, and the display it keeps out of sleep |
| `tests/shell/test_remote_screen_installer.bats` | The nginx edit, its relative redirect and upgrade, idempotence, revert and removal |
