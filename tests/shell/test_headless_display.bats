#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression tests for headless SDL startup.
#
# Why this exists: LVGL asks SDL for an ACCELERATED renderer (LV_SDL_ACCELERATED
# in lv_conf.h). Video drivers that offer only a software renderer — the dummy
# and offscreen drivers, GPU-less containers, CI runners — fail that request.
# LVGL's own failure path in lv_sdl_window_create() then freed the driver data
# BEFORE lv_display_delete(), so the LV_EVENT_DELETE handler (release_disp_cb)
# read the freed pointer and SIGSEGV'd in deinit_display(). The crash landed
# before lv_sdl_window_create() could return NULL, so DisplayManager's existing
# fbdev-fallback / "all backends exhausted" handling was unreachable.
#
# Net effect: the obvious way to run the UI headless (SDL_VIDEODRIVER=dummy)
# segfaulted, and there was no documented headless path for driving panels in
# tests. These tests lock the crash fix, the automatic software-renderer
# fallback, and the end-to-end headless ctl loop.

setup() {
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    BIN="$REPO_ROOT/build/bin/helix-screen"
    # Private config dir, mirroring tests/ui/helix/app.py. bats runs FILES in
    # parallel (--jobs), and the single-instance lock lives in the config dir —
    # so sharing the default one makes this file and test_helixctl_json.bats
    # race for it, and the loser dies with "Another instance is already running".
    export HELIX_CONFIG_DIR="$BATS_TEST_TMPDIR/config"
    mkdir -p "$HELIX_CONFIG_DIR"
    WINDOW_PATCH="$REPO_ROOT/patches/lvgl_sdl_window.patch"
    SW_PATCH="$REPO_ROOT/patches/lvgl_sdl_sw_android_debug.patch"
    WINDOW_SRC="$REPO_ROOT/lib/lvgl/src/drivers/sdl/lv_sdl_window.c"
    SW_SRC="$REPO_ROOT/lib/lvgl/src/drivers/sdl/lv_sdl_sw.c"
}

require_binary() {
    [ -x "$BIN" ] || skip "helix-screen not built (run 'make -j')"
}

# The patched LVGL sources only exist once the submodule is checked out and
# mk/patches.mk has run. Jobs that validate shell scripts without building
# (release.yml's validate-shell) have neither, so assert the patch files —
# which are the source of truth for what ships — before reaching for the
# working-tree copy, then skip the source half.
require_lvgl_src() {
    for src in "$@"; do
        [ -r "$src" ] || skip "lvgl submodule not checked out ($(basename "$src"))"
    done
}

# Unique per-test socket. Unix socket paths cap at ~108 bytes, and
# BATS_TEST_TMPDIR is often too long, so anchor in /tmp directly.
headless_socket() {
    echo "/tmp/hs-bats-$$-${BATS_TEST_NUMBER}.sock"
}

@test "headless boot under SDL_VIDEODRIVER=dummy does not segfault" {
    require_binary
    local log="$BATS_TEST_TMPDIR/headless.log"

    # 139 = 128 + SIGSEGV(11). The pre-fix binary died here every time.
    run env SDL_VIDEODRIVER=dummy timeout -s KILL 15 "$BIN" --test -vv \
        --remote-socket "$(headless_socket)"
    printf '%s\n' "$output" > "$log"

    [ "$status" -ne 139 ]
    # 137 = SIGKILL from timeout, i.e. it was still running happily. Any other
    # non-zero status means it exited early on its own.
    [ "$status" -eq 137 ] || [ "$status" -eq 0 ]
}

@test "headless boot creates an SDL display via the software renderer" {
    require_binary
    local log="$BATS_TEST_TMPDIR/sw.log"

    run env SDL_VIDEODRIVER=dummy timeout -s KILL 15 "$BIN" --test -vv \
        --remote-socket "$(headless_socket)"
    printf '%s\n' "$output" > "$log"

    grep -q "SDL display created" "$log"
    # The accelerated attempt is expected to fail first; what matters is that we
    # retried and said so, rather than crashing or giving up.
    grep -qi "software renderer" "$log"
}

@test "SDL_VIDEODRIVER=dummy silences audio (no beeps under ctl)" {
    # SDL's video and audio subsystems are independent: SDL_VIDEODRIVER=dummy
    # alone still opens the real PulseAudio/PipeWire/ALSA device and audibly
    # beeps through the desktop speakers — pure spam for a headless run.
    # main()'s silence_audio_if_headless() forces SDL_AUDIODRIVER=dummy so a
    # headless run stays headless. The visible signal is the driver name in
    # the [SDLSound] init log; without the override it would be 'pulseaudio'
    # or 'pipewire' on a dev box.
    require_binary
    local log="$BATS_TEST_TMPDIR/audio.log"

    # -u SDL_AUDIODRIVER: make sure the test isn't polluted by the dev box's
    # own audio driver setting. HELIX_CONFIG_DIR isolates the instance lock.
    run env -u SDL_AUDIODRIVER SDL_VIDEODRIVER=dummy timeout -s KILL 15 "$BIN" \
        --test -vv --remote-socket "$(headless_socket)"
    printf '%s\n' "$output" > "$log"

    # If SDL audio initialized at all, it must be on the dummy driver.
    # (On a CI runner with no SDL audio at all, SDLSoundBackend::initialize()
    # fails and SoundManager falls through to ALSA/PWM/none — the line is
    # absent and the assertion is vacuously satisfied.)
    if grep -q "SDLSound" "$log"; then
        grep -q "driver 'dummy'" "$log"
    fi
}

@test "explicit SDL_AUDIODRIVER is respected under SDL_VIDEODRIVER=dummy" {
    # overwrite=0 in main()'s setenv: a developer who wants to debug audio
    # under a headless window exports SDL_AUDIODRIVER themselves and that
    # wins. We assert that the binary does NOT overwrite it with 'dummy'.
    # Use a real but unlikely-to-be-default value; we can't guarantee the
    # box has pulseaudio/pipewire, but 'alsa' is part of SDL's stock list
    # and selecting it via env still gets passed through to SDL_Init.
    require_binary
    local log="$BATS_TEST_TMPDIR/audio-explicit.log"

    run env SDL_AUDIODRIVER=alsa SDL_VIDEODRIVER=dummy timeout -s KILL 15 "$BIN" \
        --test -vv --remote-socket "$(headless_socket)"
    printf '%s\n' "$output" > "$log"

    # Driver name should be 'alsa' if SDL audio came up at all — never
    # silently overridden to 'dummy'.
    if grep -q "SDLSound" "$log"; then
        ! grep -q "driver 'dummy'" "$log"
    fi
}

@test "headless instance can be driven via ctl and capture a screenshot" {
    require_binary
    local sock shot
    sock="$(headless_socket)"
    shot="/tmp/hs-bats-$$-${BATS_TEST_NUMBER}.png"
    rm -f "$sock" "$shot"

    env SDL_VIDEODRIVER=dummy "$BIN" --test -vv --remote-socket "$sock" \
        > "$BATS_TEST_TMPDIR/ctl.log" 2>&1 &
    local pid=$!

    local i
    for i in $(seq 1 40); do
        [ -S "$sock" ] && break
        sleep 1
    done
    [ -S "$sock" ] || { kill -9 "$pid" 2>/dev/null; false; }

    run "$BIN" ctl --socket "$sock" navigate filament
    local nav_status=$status

    run "$BIN" ctl --socket "$sock" screenshot "$shot"
    local shot_status=$status

    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$sock"

    [ "$nav_status" -eq 0 ]
    [ "$shot_status" -eq 0 ]
    [ -s "$shot" ]
    rm -f "$shot"
}

@test "lv_sdl_window_create failure path deletes the display before freeing dsc" {
    # Freeing dsc first leaves release_disp_cb reading a dangling pointer.
    # Lock the ordering in both the live source and the patch that recreates it.
    grep -q "lv_display_delete" "$WINDOW_PATCH"

    require_lvgl_src "$WINDOW_SRC"
    local body
    body="$(sed -n '/Failed to initialize window/,/return NULL;/p' "$WINDOW_SRC")"
    [ -n "$body" ]

    local del_line free_line
    del_line="$(printf '%s\n' "$body" | grep -n 'lv_display_delete' | head -1 | cut -d: -f1)"
    free_line="$(printf '%s\n' "$body" | grep -n 'lv_free(dsc)' | head -1 | cut -d: -f1)"

    # dsc may legitimately no longer be freed here at all (release_disp_cb owns
    # it). If it still is, it MUST come after the delete.
    if [ -n "$free_line" ]; then
        [ "$del_line" -lt "$free_line" ]
    fi
    grep -q "lv_display_delete" <<< "$body"
}

@test "window_create clears dsc->window after destroying it on init failure" {
    # SDL_DestroyWindow() without nulling the field lets release_disp_cb destroy
    # the same window a second time.
    grep -q "dsc->window = NULL" "$WINDOW_PATCH"

    require_lvgl_src "$WINDOW_SRC"
    local body
    body="$(sed -n '/Failed to initialize SDL backend/,/LV_RESULT_INVALID;/p' "$WINDOW_SRC")"
    [ -n "$body" ]
    grep -q "dsc->window = NULL" <<< "$body"
}

@test "deinit_display null-guards the backend display data" {
    # When init_display() fails, backend_data was never set. LV_ASSERT_NULL is
    # compiled out in our lv_conf, so an unguarded ddata->texture is a NULL deref.
    grep -qE 'if\s*\(\s*!\s*ddata\s*\)' "$SW_PATCH"

    require_lvgl_src "$SW_SRC"
    local body
    body="$(sed -n '/^static void deinit_display/,/^}/p' "$SW_SRC")"
    [ -n "$body" ]
    grep -qE 'if\s*\(\s*!\s*ddata\s*\)' <<< "$body"
}

@test "screenshot.sh honors HELIX_HEADLESS over Wayland auto-detection" {
    # Both branches guard on an empty SDL_VIDEODRIVER, so whichever runs first
    # wins. With the Wayland block first, HELIX_HEADLESS=1 on a Wayland desktop
    # was silently ignored.
    local script="$REPO_ROOT/scripts/screenshot.sh"
    local headless_line wayland_line
    headless_line="$(grep -n 'SDL_VIDEODRIVER=dummy' "$script" | head -1 | cut -d: -f1)"
    wayland_line="$(grep -n 'SDL_VIDEODRIVER=wayland' "$script" | head -1 | cut -d: -f1)"

    [ -n "$headless_line" ]
    [ -n "$wayland_line" ]
    [ "$headless_line" -lt "$wayland_line" ]
}

@test "SDL backend retries with the software renderer when the first attempt fails" {
    local src="$REPO_ROOT/src/api/display_backend_sdl.cpp"
    grep -q "SDL_HINT_RENDER_DRIVER" "$src"
    grep -q "software" "$src"
}

@test "SDL backend does not blame the renderer when there is no video driver" {
    # Two distinct failures reach the same branch. If SDL_CreateWindow failed
    # because no video driver exists, a different render driver cannot help and
    # LVGL will not re-run SDL_Init anyway (its `inited` flag is already set) —
    # so retrying is pointless and "accelerated renderer unavailable" is a wrong
    # diagnosis that sends people debugging the GPU instead of the driver.
    local src="$REPO_ROOT/src/api/display_backend_sdl.cpp"
    grep -q "SDL_WasInit(SDL_INIT_VIDEO)" "$src"
    grep -q "No usable SDL video driver" "$src"
}

@test "release_disp_cb quits SDL only after destroying its resources" {
    # SDL_Quit() tears down the video subsystem that owns the texture, renderer
    # and window. Quitting first leaves deinit_display() destroying dangling
    # handles.
    grep -q "lv_sdl_quit" "$WINDOW_PATCH"

    require_lvgl_src "$WINDOW_SRC"
    local body
    body="$(sed -n '/^static void release_disp_cb/,/^}/p' "$WINDOW_SRC")"
    [ -n "$body" ]

    local deinit_line quit_line
    deinit_line="$(printf '%s\n' "$body" | grep -n 'deinit_display' | head -1 | cut -d: -f1)"
    quit_line="$(printf '%s\n' "$body" | grep -n 'lv_sdl_quit' | head -1 | cut -d: -f1)"

    [ -n "$deinit_line" ]
    [ -n "$quit_line" ]
    [ "$deinit_line" -lt "$quit_line" ]
}

@test "headless smoke script exists, is executable and has valid syntax" {
    local script="$REPO_ROOT/scripts/smoke-headless.sh"
    [ -x "$script" ]
    bash -n "$script"
}

@test "CI runs the headless smoke test, not just --help" {
    # build.yml's only runtime check used to be `helix-screen --help || true`,
    # which cannot fail — a startup segfault shipped green.
    local wf="$REPO_ROOT/.github/workflows/build.yml"
    grep -q "smoke-headless.sh" "$wf"
    # And it must not be neutered with `|| true`.
    ! grep -qE "smoke-headless\.sh.*\|\| *true" "$wf"
}
