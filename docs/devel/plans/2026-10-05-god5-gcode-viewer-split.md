# GOD-5: splitting ui_gcode_viewer.cpp

Audit finding GOD-5 (`2026-09-30-architecture-audit.html#GOD-5`). `src/ui/ui_gcode_viewer.cpp` is
3,580 lines: state class, 3D geometry budget, 2D setup, draw, input, size/watchdog, create/delete,
async loader, a 49-entry C API, XML registration, test access, and the ESP32 no-op branch. Delete
this file when the tranche ships.

## C API

- Dead (no caller outside the file): `zoom`, `set_view`, `set_camera_azimuth/elevation/zoom`,
  `set_debug_colors`, `set_show_travels`, `set_specular`, `set_extrusion_color`,
  `get_filament_type`, `set_ghost_mode`. Plus 22 orphan stubs in the ESP32 branch that no header
  declares. Delete; then check whether the GLES-side setters they fed lose their last caller.
- File-local: `pick_object`, `get_tool_palette` become static.
- Test-only: `get_tool_colors`, `clear_tool_colors` move to `helix::test_access`.
- 36 live entries stay (print status, print-select detail, preview setup, exclude-object, AMS).

## Seams (all private, under src/ui/)

- `gcode_viewer_state.h`: the state class and `get_state()`.
- `gcode_viewer_loader.cpp`: async build result, streaming and full loads, the 3D geometry budget,
  on-demand 3D build.
- `gcode_viewer_input.cpp`: press/pressing/release/gesture, long-press timer, pick; one
  `install_input_handlers(obj, st)`. Reuses `view_gestures.h`.
- `ui_gcode_viewer.cpp` keeps create/delete/draw/size/watchdog/registration and the live API.
- `ViewOptions` plus one `apply_view_options(st)` replaces ~40 hand-written renderer `set_*`
  calls; called when a renderer is created and from each setter, and must not re-upload VBOs when
  nothing changed (Pi 3B VC4 sustains ~4M tris/s).

## Threading and lifetime rules to keep

- One loader thread per viewer; `start_build` cancels and joins the previous one. The worker
  touches only the state's atomics and data it allocated; never `lv_*`, never the renderer.
- Results return through `queue_update` guarded by `lv_obj_is_valid`, `get_state`, then the load
  generation.
- Teardown order on load and clear: 2D renderer (its ghost thread holds the streaming
  controller), streaming controller, `cancel_build`, file, 3D geometry.
- GL objects are created, drawn, uploaded and destroyed on the LVGL thread only.
- `delete_cb`: registry removal, occluder callback detach, timers, `cancel_build`, delete state.

Known hole (unverified): a result queued just before cancel survives the delete; a new viewer at
the same address restarts at generation 0, bumps to 1 and passes every guard. Fix by seeding the
generation from a process-wide counter, in its own commit with a test.

## Commits (pinning test first)

1. Delete the dead entries and orphan stubs; make the two helpers static; fix
   `16-gcode-pipeline.md` (~-330).
2. Test-only accessors to test access.
3. State header (pure move).
4. Loader (pure move); zeus ASAN at the end of the branch.
5. Input (pure move), with a new tap-pick test first.
6. `ViewOptions` (~-150), pinned by a test that sets colors/highlight/exclude, loads, then
   switches 2D, 3D, 2D and checks every renderer received them.
7. Generation fix (~+15).

Ends near 1,300 (shell) + 720 (loader) + 430 (input) + 400 (state), about -470 net.

## Risks

- Commit 6 is the only GLES behaviour change: on-device Pi check (mode switch, AMS colors
  mid-print, exclude an object).
- `mk/pi-dual-link.mk` compiles the file twice; new .cpp files join its GLES variant list.
- The ESP32 no-op branch must still define every entry firmware-linked callers use.
- Review each move with `git diff --color-moved`.
