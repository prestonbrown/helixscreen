#!/bin/bash

# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set -e

# Show help
show_help() {
    cat << 'EOF'
Usage: screenshot.sh [BINARY] [NAME] [TOKEN] [FLAGS...]

Capture a screenshot of the HelixScreen UI, driven by helix-screen ctl.

Launches the binary with its remote-control server on a private socket, drives
the UI to the requested screen with a navigation recipe, captures a screenshot,
converts it to PNG, and shuts the instance down. Each capture is an isolated,
freshly-booted process with its own throwaway config directory — no state leaks
between shots, and nothing a capture writes survives it.

Arguments:
  BINARY    Binary name in build/bin/ (default: helix-screen)
  NAME      Output filename suffix (default: timestamp)
            Screenshot saved to: /tmp/ui-screenshot-<NAME>.png
  TOKEN     Screen to capture (optional; default: home). May be a base panel
            (home, controls, filament, settings, advanced, print-select), an
            overlay (motion, bed-mesh, network, zoffset, ...), or a sample-data
            screen (preflight-check, color-mismatch, runout-modal, lock-screen,
            print-status, print-tune). See scripts/screenshot-recipes.sh for the full list.
            An unknown token is tried as a bare `navigate <token>`.
  FLAGS     Additional flags passed to the binary (e.g., --light,
            -s 800x480, --layout ultrawide). Pass --wizard to capture the
            first-run wizard (suppresses --skip-wizard).

            Captures run against mock data (--test) by default. Pass --real to
            capture against the configured printer instead — that runs the app
            in production mode against a COPY of your real config/settings.json,
            so it still refreshes ~/.helixscreen/*.backup, and it is opt-in.

            --recipe '<steps>'  Drive the UI with these `helix-screen ctl`
            steps (semicolon-separated) instead of a recipe-table lookup —
            for a screen with no table entry yet. Overrides TOKEN's recipe.

            --printer <id>      Which printer the mock impersonates
            (voron_24, voron_trident, k1, ad5m, generic_corexy,
            generic_bedslinger, multi_extruder). Sets HELIX_MOCK_PRINTER.
            Default: unset, which is the mock's own default of Voron 2.4.
            Ignored (with a warning) under --real.

Determinism:
  A capture must depend only on its explicit inputs, so every run gets a fresh
  HELIX_CONFIG_DIR under /tmp, seeded from config/settings.json.template (mock)
  or a copy of config/settings.json (--real), and deleted on exit. Without
  this, the app read and wrote the repo's gitignored config/settings-test.json
  and inherited whatever the last dev or agent session had left there — theme,
  active printer, and saved home-panel widget layout all leaked into captures.

  Theme: mock captures default to DARK (the app's shipped default). Pass
  --light for a light capture, or --dark to be explicit. --real follows the
  captured config's own dark_mode unless you pass --dark/--light.
  The color palette is the shipped default (assets/config/themes/defaults/);
  set HELIX_THEME=<name> to pin a different one.

Environment Variables:
  HELIX_SCREENSHOT_DISPLAY   Display index to open the window on (default: auto)
  HELIX_SCREENSHOT_TIMEOUT   Max seconds to wait for the control socket (default: 20)
  HELIX_SCREENSHOT_DELAY     Settle seconds after the recipe before capture (default: 1.5)
  HELIX_SCREENSHOT_OPEN      If set, opens the screenshot in a viewer
  HELIX_THEME                Color theme name (default: the config's, i.e. helixscreen)
  HELIX_MOCK_PRINTER         Mock printer identity; --printer <id> is the flag form

Examples:
  ./scripts/screenshot.sh                                 # default binary, home
  ./scripts/screenshot.sh helix-screen home-panel home
  ./scripts/screenshot.sh helix-screen motion motion -s small
  ./scripts/screenshot.sh helix-screen zoffset zoffset
  ./scripts/screenshot.sh helix-screen preflight preflight-check
  ./scripts/screenshot.sh helix-screen wizard-wifi "" --wizard
  ./scripts/screenshot.sh helix-screen safety "" \
      --recipe 'navigate settings; click row_safety'
  ./scripts/screenshot.sh helix-screen ad5m-home home --printer ad5m
  ./scripts/screenshot.sh helix-screen light-home home --light
  ./scripts/screenshot.sh helix-screen live-home home --real   # real printer

Output:
  Screenshots are saved to /tmp/ui-screenshot-<NAME>.png, encoded by the app.

Dependencies:
  - none beyond the built binary (PNG is encoded in-app via lodepng)
EOF
    exit 0
}

case "${1:-}" in
    -h|--help|help) show_help ;;
esac

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${BLUE}ℹ${NC} $1"; }
success() { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
error() { echo -e "${RED}✗${NC} $1"; }

# Project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# shellcheck source=screenshot-recipes.sh
source "$SCRIPT_DIR/screenshot-recipes.sh"

BINARY="${1:-helix-screen}"
BINARY_PATH="./build/bin/${BINARY}"
HELIXCTL=("./build/bin/helix-screen" ctl)

NAME="${2:-$(date +%s)}"
PNG_FILE="/tmp/ui-screenshot-${NAME}.png"

# Third arg: TOKEN (a screen) unless it starts with '-', in which case it's a flag.
TOKEN=""
if [ $# -ge 3 ]; then
    if [[ "${3}" == -* ]]; then
        shift 2; EXTRA_ARGS=("$@")
    else
        TOKEN="${3}"; shift 3 2>/dev/null || true; EXTRA_ARGS=("$@")
    fi
else
    shift 2 2>/dev/null || true; EXTRA_ARGS=("$@")
fi

# Script-level flags that take a value. Neither is a binary flag, so both are
# consumed here and never forwarded.
#   --recipe '<ctl steps>'  captures a screen with no table entry, without
#                           having to add one first.
#   --printer <id>          pins the mock printer identity for this capture.
#                           An inherited HELIX_MOCK_PRINTER is honored as the
#                           default so the env-var form keeps working; unset
#                           means the mock's own default (Voron 2.4), which is
#                           what bare --test has always produced.
INLINE_RECIPE=""
SHOT_PRINTER="${HELIX_MOCK_PRINTER:-}"
FILTERED_ARGS=()
pending=""
for a in "${EXTRA_ARGS[@]}"; do
    if [ -n "$pending" ]; then
        case "$pending" in
            recipe) INLINE_RECIPE="$a" ;;
            printer) SHOT_PRINTER="$a" ;;
        esac
        pending=""
        continue
    fi
    case "$a" in
        --recipe) pending="recipe"; continue ;;
        --printer) pending="printer"; continue ;;
    esac
    FILTERED_ARGS+=("$a")
done
if [ -n "$pending" ]; then
    error "--${pending} requires a value"
    exit 1
fi
EXTRA_ARGS=("${FILTERED_ARGS[@]}")

# Wizard capture: --wizard is forwarded to the binary, where it sets force_wizard
# and overrides the --skip-wizard that --test otherwise implies. We also withhold
# our own --skip-wizard and run no recipe (we just capture the boot screen).
WIZARD_MODE=0
for a in "${EXTRA_ARGS[@]}"; do
    [ "$a" = "--wizard" ] && WIZARD_MODE=1
done

# Mock by default. Without --test the app runs in PRODUCTION mode: it reads and
# rewrites the developer's real config/settings.json, drops tool_spools.json and
# telemetry_queue.json into the repo, and — because /var/lib/helixscreen is
# root-owned — falls through to rewriting ~/.helixscreen/*.backup. Screenshots
# are overwhelmingly taken against mock data, so that has to be opt-in, not the
# accident you get by forgetting a flag. --real captures against the configured
# printer instead; it is consumed here and never forwarded to the binary.
REAL_MODE=0
FILTERED_ARGS=()
for a in "${EXTRA_ARGS[@]}"; do
    if [ "$a" = "--real" ]; then REAL_MODE=1; continue; fi
    [ "$a" = "--test" ] && REAL_MODE=0
    FILTERED_ARGS+=("$a")
done
EXTRA_ARGS=("${FILTERED_ARGS[@]}")

# Headless: no display server at all (CI, ssh session, container). SDL's dummy
# video driver needs no window system, and the SDL backend falls back to the
# software renderer on its own. Screenshots still come out correct because
# capture goes through lv_snapshot_take(), which re-renders the object tree
# into its own buffer rather than reading back the display. HELIX_HEADLESS=1
# forces this on a machine that does have a display, so it is checked before the
# Wayland auto-detect below — otherwise that branch would claim SDL_VIDEODRIVER
# first and the request would be silently ignored.
#
# No need to also export SDL_AUDIODRIVER=dummy here: the binary's
# silence_audio_if_headless() in main() notices SDL_VIDEODRIVER=dummy and
# forces it itself, so a screenshot sweep doesn't beep on every panel change.
if [ -z "$SDL_VIDEODRIVER" ] && { [ "${HELIX_HEADLESS:-0}" = "1" ] ||
    { [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; }; }; then
    export SDL_VIDEODRIVER=dummy
    info "Headless — using SDL_VIDEODRIVER=dummy (software renderer)"
fi

# On a Wayland desktop, force SDL's native Wayland driver (avoids XWayland GLX crash).
if [ -n "$WAYLAND_DISPLAY" ] && [ -z "$SDL_VIDEODRIVER" ]; then
    export SDL_VIDEODRIVER=wayland
    info "Wayland session detected — using SDL_VIDEODRIVER=wayland"
fi

# Display index
if [ -z "$HELIX_SCREENSHOT_DISPLAY" ]; then
    if [ -n "$WAYLAND_DISPLAY" ]; then HELIX_SCREENSHOT_DISPLAY=0; else HELIX_SCREENSHOT_DISPLAY=1; fi
fi

SOCKET_TIMEOUT="${HELIX_SCREENSHOT_TIMEOUT:-20}"
SETTLE="${HELIX_SCREENSHOT_DELAY:-1.5}"

# Binary present + executable
if [ ! -f "$BINARY_PATH" ]; then
    error "Binary not found: $BINARY_PATH"; info "Build first with: make"; exit 1
fi
[ -x "$BINARY_PATH" ] || chmod +x "$BINARY_PATH"
if [ ! -x "./build/bin/helix-screen" ]; then
    error "helix-screen not found: ./build/bin/helix-screen"; info "Build it with: make -j"; exit 1
fi

# Private per-invocation socket so we never collide with a dev instance.
SOCK="/tmp/helix-shot-$$.sock"
LOG="/tmp/helix-shot-$$.log"
rm -f "$SOCK" 2>/dev/null || true

# Private per-invocation CONFIG directory, for the same reason. Without it the
# app runs with CWD=repo root and reads *and writes* config/settings-test.json
# (mock) or config/settings.json (--real) — gitignored files any dev or agent
# session may have left in an arbitrary state. A capture then silently inherits
# the last run's theme, active printer, and saved home-panel widget layout,
# which reads as a layout bug rather than as leaked state.
#
# HELIX_CONFIG_DIR supplies the DIRECTORY only; Config::resolve_path() keeps the
# caller's filename, so --test still reads settings-TEST.json inside it (seeding
# settings.json there would be a silent no-op) and --real still reads
# settings.json. The app does NOT create the directory — a missing one aborts
# startup — so mktemp -d does it for us.
CONFIG_DIR="$(mktemp -d /tmp/helix-shot-config-$$-XXXXXX)"
export HELIX_CONFIG_DIR="$CONFIG_DIR"

HELIX_PID=""
cleanup() {
    if [ -n "$HELIX_PID" ] && kill -0 "$HELIX_PID" 2>/dev/null; then
        # Ask it to exit cleanly (flushes logs, runs shutdown paths); fall back
        # to a signal if the control socket is already gone.
        "${HELIXCTL[@]}" -s "$SOCK" shutdown >/dev/null 2>&1 || kill "$HELIX_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$HELIX_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -0 "$HELIX_PID" 2>/dev/null && kill "$HELIX_PID" 2>/dev/null || true
    fi
    rm -f "$SOCK" "$LOG" 2>/dev/null || true
    # Pattern-guarded: only ever remove a directory this script created.
    case "$CONFIG_DIR" in
        /tmp/helix-shot-config-*) rm -rf "$CONFIG_DIR" 2>/dev/null || true ;;
    esac
}
trap cleanup EXIT

# Seed the throwaway config so the run starts from a known document rather than
# from whatever defaults the app synthesizes.
#
# Mock: settings.json.template is the shipped default and is tracked, so it is
# the same bytes on every machine — dark_mode true, theme "helixscreen",
# active_printer_id "default", and no saved panel_widgets, so the home panel
# builds its stock widget grid. It also carries wizard_completed=false, which
# (together with the --skip-wizard below) is what keeps FirstRunTour from
# auto-starting over the capture: FirstRunTour::should_auto_start() bails when
# Config::is_wizard_required(). Flipping wizard_completed to true in the
# template would put the tour overlay in every home screenshot.
#
# --real: copy the developer's real settings.json instead, so the capture shows
# the configured printer exactly as it does today — but the app's writes land in
# the temp copy and are thrown away. An empty dir would NOT be neutral here:
# outside test mode Config::init() falls back to the rolling-backup chain
# (/var/lib/helixscreen, then ~/.helixscreen/settings.json.backup) and would
# quietly resurrect some older config. Mock captures are immune to that —
# backups_enabled() is false in test mode — but --real is not, so seed it.
if [ "$REAL_MODE" = "1" ]; then
    if [ -f config/settings.json ]; then
        cp config/settings.json "$CONFIG_DIR/settings.json"
        info "Seeded throwaway config from config/settings.json (writes are discarded)"
    else
        warn "config/settings.json not found — --real will fall back to the rolling backup chain"
    fi
    [ -f config/helixscreen.env ] && cp config/helixscreen.env "$CONFIG_DIR/helixscreen.env"
else
    cp config/settings.json.template "$CONFIG_DIR/settings-test.json"
fi

# Mock printer identity. Exported only when asked for: setting it at all makes
# MoonrakerManager::init() clear the saved printer type before detection, so
# leaving it unset preserves the historical bare---test behavior exactly.
if [ -n "$SHOT_PRINTER" ]; then
    if [ "$REAL_MODE" = "1" ]; then
        warn "--printer/HELIX_MOCK_PRINTER ignored under --real"
        unset HELIX_MOCK_PRINTER
    else
        export HELIX_MOCK_PRINTER="$SHOT_PRINTER"
        info "Mock printer: $SHOT_PRINTER"
    fi
else
    unset HELIX_MOCK_PRINTER
fi

# Assemble launch flags. --skip-splash for speed; --remote for the control server.
LAUNCH_FLAGS=(--remote --remote-socket "$SOCK" --skip-splash
              --display "$HELIX_SCREENSHOT_DISPLAY")
[ "$WIZARD_MODE" = "0" ] && LAUNCH_FLAGS+=(--skip-wizard)

# Theme. --dark/--light set args.dark_mode_cli, which Application::init_theme()
# prefers over the config's /dark_mode. The seeded template is already dark, but
# pass the flag anyway so the capture states its theme instead of depending on a
# template field that could drift. Only for mock captures: --real is meant to
# show the printer as configured, so it follows its own dark_mode.
THEME_FLAG_GIVEN=0
for a in "${EXTRA_ARGS[@]}"; do
    case "$a" in
        --dark | --light) THEME_FLAG_GIVEN=1 ;;
    esac
done

if [ "$REAL_MODE" = "1" ]; then
    warn "Capturing in PRODUCTION mode (--real): this run uses a throwaway copy of your"
    warn "real config, but production mode still refreshes ~/.helixscreen/*.backup."
else
    # Only add --test when the caller did not already pass it explicitly, so the
    # flag never appears twice on the command line.
    printf '%s\n' "${EXTRA_ARGS[@]}" | grep -qx -- '--test' || LAUNCH_FLAGS+=(--test)
    if [ "$THEME_FLAG_GIVEN" = "0" ]; then
        LAUNCH_FLAGS+=(--dark)
    fi
fi

info "Launching ${BINARY} (private socket $SOCK)..."
"$BINARY_PATH" "${LAUNCH_FLAGS[@]}" "${EXTRA_ARGS[@]}" > "$LOG" 2>&1 &
HELIX_PID=$!

# Wait for the control socket.
waited=0
while [ ! -S "$SOCK" ]; do
    if ! kill -0 "$HELIX_PID" 2>/dev/null; then
        error "Binary exited before the control socket appeared"
        tail -15 "$LOG" 2>/dev/null
        exit 1
    fi
    if [ "$waited" -ge "$((SOCKET_TIMEOUT * 2))" ]; then
        error "Timed out after ${SOCKET_TIMEOUT}s waiting for control socket"
        exit 1
    fi
    sleep 0.5; waited=$((waited + 1))
done

# Run the navigation recipe (skip in wizard mode — the wizard shows itself).
if [ "$WIZARD_MODE" = "0" ]; then
    if [ -n "$INLINE_RECIPE" ]; then
        RECIPE="$INLINE_RECIPE"
        info "Recipe (inline): $RECIPE"
    else
        RECIPE="$(screenshot_recipe_for "${TOKEN:-home}")"
        info "Recipe: $RECIPE"
    fi
    IFS=';' read -ra STEPS <<< "$RECIPE"
    for step in "${STEPS[@]}"; do
        # trim leading/trailing whitespace
        step="$(echo "$step" | sed 's/^ *//;s/ *$//')"
        [ -z "$step" ] && continue
        # Surface the control server's error text — a silently-skipped step
        # produces a screenshot of the wrong screen, which is worse than a fail.
        if ! STEP_ERR=$("${HELIXCTL[@]}" -s "$SOCK" $step 2>&1 >/dev/null); then
            warn "Recipe step failed: '$step'${STEP_ERR:+ — $STEP_ERR}"
        fi
    done
else
    info "Wizard mode: capturing boot screen (no recipe)"
fi

# Let animations/transitions settle, then capture straight to PNG. The app
# encodes it (lodepng), so there is no BMP hop and no ImageMagick dependency.
sleep "$SETTLE"
if ! CAPTURE_ERR=$("${HELIXCTL[@]}" -s "$SOCK" screenshot "$PNG_FILE" 2>&1 >/dev/null); then
    error "helix-screen ctl screenshot failed${CAPTURE_ERR:+: $CAPTURE_ERR}"
    tail -10 "$LOG" 2>/dev/null
    exit 1
fi
if [ ! -f "$PNG_FILE" ]; then
    error "Screenshot not written: $PNG_FILE"; tail -10 "$LOG" 2>/dev/null; exit 1
fi

PNG_SIZE=$(ls -lh "$PNG_FILE" | awk '{print $5}')
echo ""
success "Screenshot ready!"
echo "  File:  $PNG_FILE ($PNG_SIZE)"
if [ -n "$INLINE_RECIPE" ]; then
    echo "  Recipe: $INLINE_RECIPE"
else
    echo "  Token: ${TOKEN:-home}"
fi
echo ""

if [ -n "$HELIX_SCREENSHOT_OPEN" ]; then
    command -v open &>/dev/null && open "$PNG_FILE" || { command -v xdg-open &>/dev/null && xdg-open "$PNG_FILE"; }
fi
