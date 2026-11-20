#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# serve-local-update.sh — Local update server for testing the helix-screen
#                          download → install → restart flow end-to-end.
#
# OVERVIEW
# --------
# Builds a platform package, generates a manifest.json with a fixed high test
# version (99.0.0) guaranteed to be newer than any real install, and serves
# both over HTTP. The running helix-screen binary sees the manifest version
# as newer than itself and triggers the full self-update path without
# touching VERSION.txt.
#
# The test version is always 99.0.0 so the manifest is always seen as newer
# regardless of what version is currently installed on the device. Use
# --no-bump to serve the exact VERSION.txt version instead.
#
# Supported platforms (mirrors the *-docker Makefile targets):
#   pi            Raspberry Pi 64-bit aarch64  (default)
#   pi32          Raspberry Pi 32-bit armhf
#   ad5m          FlashForge Adventurer 5M (armv7-a)
#   cc1           Centauri Carbon 1 (armv7-a)
#   k1            Creality K1
#   k1-dynamic    Creality K1 (dynamic linking)
#   k2            Creality K2
#   snapmaker-u1  Snapmaker U1
#
# QUICK START
# -----------
#   # 1. Set your device's address (once, add to ~/.zshrc or ~/.bashrc):
#   export HELIX_TEST_USERNAME=pi
#   export HELIX_TEST_PRINTER=helixscreen.local   # or an IP address
#
#   # 2. First time only — configure the device to use dev channel:
#   ./scripts/serve-local-update.sh --configure-remote
#
#   # 3. Every subsequent test iteration:
#   ./scripts/serve-local-update.sh [--platform PLATFORM]
#
#   helix-screen will check the manifest, see a newer version, download
#   the zip, run install.sh, and restart itself.
#
#   To test the installer directly on the device (bypassing the update checker):
#   scp dist/helixscreen-PLATFORM.zip USERNAME@PRINTER:/tmp/
#   ssh USERNAME@PRINTER 'sh /tmp/install.sh --local /tmp/helixscreen-PLATFORM.zip'
#   (install.sh is copied to /tmp/ automatically by --configure-remote)
#
# ENVIRONMENT VARIABLES
# ---------------------
#   HELIX_TEST_PRINTER   Device hostname or IP  (default: helixscreen.local)
#   HELIX_TEST_USERNAME  Device SSH username    (default: pi)
#
#   Legacy names still accepted for backward compatibility:
#   HELIX_TEST_PI_HOST → HELIX_TEST_PRINTER
#   HELIX_TEST_PI_USER → HELIX_TEST_USERNAME
#
# OPTIONS
# -------
#   --platform PLATFORM   Target platform to build and serve (default: pi).
#                         See platform list above.
#   --configure-remote    SSH into the device, select the dev channel in
#                         settings.json, install a root-owned override file at
#                         /var/lib/helixscreen/update_urls.json pointing dev_url
#                         at this server, enable HELIX_LOG_LEVEL=debug in
#                         helixscreen.env for debug logging, copy install.sh to
#                         /tmp/ for local install testing, and restart the
#                         helixscreen service. Run once per device (or after a
#                         factory reset). Requires SSH key access and sudo.
#                         Note: uses Pi paths (~/helixscreen/). For other
#                         platforms configure the two files manually.
#   --no-bump             Serve the exact version from VERSION.txt instead of
#                         99.0.0. Useful if you manually set a higher version.
#   --no-build            Skip compile + package. Patches install.sh from the
#                         repo into the existing dist/ zip so script-only
#                         changes (install.sh, no binary changes) can be tested
#                         immediately.
#   --port PORT           HTTP port to listen on (default: 8765).
#
# DEPENDENCIES
# ------------
#   - Docker (for *-docker cross-compilation targets)
#   - python3 (for the HTTP server and device settings.json patch)
#   - ssh + scp with key for $HELIX_TEST_USERNAME@$HELIX_TEST_PRINTER
#   - zip and unzip (for the --no-build install.sh patch)
#
# NOTES
# -----
#   - The bumped version is only used for the manifest.
#     VERSION.txt is never modified.
#   - After install the device binary still reports VERSION.txt's version
#     (< 99.0.0), so the next run will again offer an update — intentional
#     for iteration.
#   - To reset the device back to the stable channel, remove
#     /var/lib/helixscreen/update_urls.json and set update/channel back to 0 in
#     settings.json on the device.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults (overridable via env) ────────────────────────────────────────────
# Accept both new names and legacy PI_* names for backward compatibility
PRINTER="${HELIX_TEST_PRINTER:-${HELIX_TEST_PI_HOST:-helixscreen.local}}"
USERNAME="${HELIX_TEST_USERNAME:-${HELIX_TEST_PI_USER:-pi}}"
PORT=8765
BUILD=1
BUMP=1
CONFIGURE_REMOTE=0
PLATFORM=pi

VALID_PLATFORMS="pi pi32 ad5m cc1 k1 k1-dynamic k2 snapmaker-u1"

# ── Args ──────────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --configure-remote) CONFIGURE_REMOTE=1; shift ;;
        --no-build)         BUILD=0; shift ;;
        --no-bump)          BUMP=0; shift ;;
        --platform)         PLATFORM=$2; shift 2 ;;
        --port)             PORT=$2; shift 2 ;;
        --help|-h)
            sed -n '2,/^set -/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        pi|pi32|ad5m|cc1|k1|k1-dynamic|k2|snapmaker-u1) PLATFORM=$1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Validate platform ─────────────────────────────────────────────────────────
if ! echo "$VALID_PLATFORMS" | grep -qw "$PLATFORM"; then
    echo "ERROR: Unknown platform '$PLATFORM'"
    echo "  Valid platforms: $VALID_PLATFORMS"
    exit 1
fi

# ── Dependency checks ─────────────────────────────────────────────────────────
_missing=()
command -v python3 >/dev/null || _missing+=(python3)
if [[ $BUILD -eq 1 ]]; then
    command -v docker >/dev/null || _missing+=(docker)
else
    command -v zip >/dev/null || _missing+=(zip)
    command -v unzip >/dev/null || _missing+=(unzip)
fi
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    command -v ssh >/dev/null || _missing+=("ssh")
    command -v scp >/dev/null || _missing+=("scp")
fi
if [[ ${#_missing[@]} -gt 0 ]]; then
    echo "ERROR: Missing required dependencies: ${_missing[*]}"
    exit 1
fi

# ── Version ───────────────────────────────────────────────────────────────────
BASE_VERSION="$(tr -d '[:space:]' < "$PROJECT_DIR/VERSION.txt")"  # e.g. 0.10.4

# Use a fixed high test version so the manifest is always newer than any real
# install, regardless of which branch or version is currently on the device.
if [[ $BUMP -eq 1 ]]; then
    TEST_VERSION="99.0.0"
else
    TEST_VERSION="$BASE_VERSION"
fi

# install.sh prepends "v" to the manifest version, so the manifest must use
# bare version numbers (e.g. "99.0.0") to avoid "vv99.0.0" in the installer.
VERSION_BARE="${TEST_VERSION}"
VERSION="v${TEST_VERSION}"
# The release zip carries no version in its name (make release-<platform>).
ZIP_NAME="helixscreen-${PLATFORM}.zip"
ZIP_PATH="$PROJECT_DIR/dist/$ZIP_NAME"

# ── Local IP ──────────────────────────────────────────────────────────────────
LOCAL_IP=$(ipconfig getifaddr en0 2>/dev/null || \
           ip route get 1 2>/dev/null | awk '{print $7; exit}' || \
           echo "127.0.0.1")
BASE_URL="http://${LOCAL_IP}:${PORT}"

# ── Port availability check ────────────────────────────────────────────────────
if lsof -iTCP:"$PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
    echo "ERROR: Port $PORT is already in use."
    echo "  Kill the existing process or use --port to choose another port."
    exit 1
fi

# ── SSH reachability check (only for --configure-remote) ──────────────────────
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    echo "[serve-local-update] Checking SSH connectivity to ${USERNAME}@${PRINTER}..."
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${USERNAME}@${PRINTER}" true 2>/dev/null; then
        echo ""
        echo "ERROR: Cannot reach ${USERNAME}@${PRINTER} via SSH."
        echo "  Check that the device is powered on and SSH key auth is configured."
        exit 1
    fi
    echo "  OK"
fi

echo ""
echo "  serve-local-update"
echo "  ══════════════════"
echo "  Platform     : $PLATFORM"
echo "  Base version : $BASE_VERSION  (VERSION.txt — unchanged)"
echo "  Test version : $TEST_VERSION  $([ $BUMP -eq 1 ] && echo "(fixed high — always newer than installed)" || echo "(no bump)")"
echo "  Package      : $ZIP_NAME"
echo "  Serving at   : $BASE_URL"
echo "  Device       : ${USERNAME}@${PRINTER}"
echo ""

# ── Bundle install.sh from modules ───────────────────────────────────────────
# install.sh is generated from scripts/lib/installer/*.sh. Regenerate before build or
# patch so the package carries the installer the modules describe.
echo "[serve-local-update] Bundling install.sh from modules..."
make -C "$PROJECT_DIR" --no-print-directory installer
INSTALLER="$PROJECT_DIR/build/installer/install.sh"
echo ""

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ $BUILD -eq 1 ]]; then
    echo "[serve-local-update] Compiling ${PLATFORM} binary (docker)..."
    make -C "$PROJECT_DIR" "${PLATFORM}-docker"
    echo ""
    echo "[serve-local-update] Packaging..."
    make -C "$PROJECT_DIR" "release-${PLATFORM}"
    mkdir -p "$PROJECT_DIR/dist"
    cp "$PROJECT_DIR/releases/$ZIP_NAME" "$ZIP_PATH"
    echo ""
fi

if [[ ! -f "$ZIP_PATH" ]]; then
    echo "ERROR: Package not found: $ZIP_PATH"
    echo ""
    echo "  Either run without --no-build, or build and copy it manually:"
    echo "    make ${PLATFORM}-docker release-${PLATFORM}"
    echo "    mkdir -p dist && cp releases/$ZIP_NAME dist/"
    exit 1
fi

# ── Patch install.sh into the existing zip (--no-build fast path) ─────────────
# The zip holds the package at its root (install.sh beside bin/), so the
# patched entry is written at the same path.
if [[ $BUILD -eq 0 ]]; then
    echo "[serve-local-update] Patching install.sh into $ZIP_NAME..."
    PATCH_DIR="$(mktemp -d)"
    trap 'rm -rf "$PATCH_DIR"' EXIT
    if ! unzip -l "$ZIP_PATH" install.sh >/dev/null 2>&1; then
        echo "ERROR: $ZIP_NAME has no install.sh at its root."
        exit 1
    fi
    cp "$INSTALLER" "$PATCH_DIR/install.sh"
    chmod +x "$PATCH_DIR/install.sh"
    (cd "$PATCH_DIR" && zip -q "$ZIP_PATH" install.sh)
    rm -rf "$PATCH_DIR"
    trap - EXIT
    echo "[serve-local-update] install.sh patched."
    echo ""
fi

# ── Manifest ──────────────────────────────────────────────────────────────────
MANIFEST_PATH="$PROJECT_DIR/dist/manifest.json"
ZIP_SHA256=$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')
ZIP_SIZE=$(wc -c < "$ZIP_PATH" | tr -d ' ')
# Same fields as scripts/generate-manifest.sh: zip_* for the in-app updater,
# url/sha256/size mirroring them for the installer.
cat > "$MANIFEST_PATH" <<EOF
{
  "version": "${VERSION_BARE}",
  "tag": "${VERSION}",
  "notes": "serve-local-update test build",
  "assets": {
    "${PLATFORM}": {
      "url": "${BASE_URL}/${ZIP_NAME}",
      "sha256": "${ZIP_SHA256}",
      "size": ${ZIP_SIZE},
      "zip_url": "${BASE_URL}/${ZIP_NAME}",
      "zip_sha256": "${ZIP_SHA256}",
      "zip_size": ${ZIP_SIZE}
    }
  }
}
EOF
echo "[serve-local-update] Manifest → $MANIFEST_PATH"
echo "  SHA256: ${ZIP_SHA256}"
echo "  Size:   ${ZIP_SIZE} bytes"

# ── Configure remote device ────────────────────────────────────────────────────
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    echo "[serve-local-update] Configuring ${USERNAME}@${PRINTER} ..."
    REMOTE_PY_OUT="$(ssh "${USERNAME}@${PRINTER}" "python3 -c \"
import json, os, re, tempfile

# Select the dev channel in settings.json. The dev URL itself lives in the
# root-owned /var/lib/helixscreen/update_urls.json installed below: settings.json
# sits in Moonraker-web-editable storage, so the app ignores any URL it names.
path = os.path.expanduser('~/helixscreen/config/settings.json')
if not os.path.exists(path):
    # Try legacy name
    path = os.path.expanduser('~/helixscreen/config/helixconfig.json')
    if not os.path.exists(path):
        print('  ERROR: settings.json not found at ~/helixscreen/config/')
        print('  Run the HelixScreen setup wizard first, then re-run --configure-remote.')
        raise SystemExit(1)
with open(path) as f:
    data = json.load(f)
data.setdefault('update', {})
data['update']['channel'] = 2
data['update'].pop('dev_url', None)
data['update'].pop('r2_url', None)
with open(path, 'w') as f:
    json.dump(data, f, indent=2)
print('  update/channel =', data['update']['channel'], '(dev)')

# Stage the trusted override under an unpredictable name; the sudo install in
# the next ssh drops places it root-owned where config_trust accepts it. A
# fixed /tmp name would let another local user pre-plant or symlink the path
# between this write and the install.
_fd, override = tempfile.mkstemp(prefix='helixscreen-update-urls.', suffix='.json', dir='/tmp')
with os.fdopen(_fd, 'w') as f:
    json.dump({'dev_url': '${BASE_URL}/'}, f)
print('STAGED_OVERRIDE=' + override)

# Enable HELIX_LOG_LEVEL=debug in helixscreen.env (enables debug logging via launcher).
# Launcher checks INSTALL_DIR/config/ first, then /etc/helixscreen/.
DEFAULTS = '''# HelixScreen environment configuration
# See config/helixscreen.env in the repo for full documentation.
MOONRAKER_HOST=localhost
MOONRAKER_PORT=7125
HELIX_LOG_LEVEL=info
'''
env_candidates = [
    os.path.expanduser('~/helixscreen/config/helixscreen.env'),
    '/etc/helixscreen/helixscreen.env',
]
env_path = next((p for p in env_candidates if os.path.exists(p)), None)
if env_path is None:
    env_path = env_candidates[0]
    os.makedirs(os.path.dirname(env_path), exist_ok=True)
    content = DEFAULTS
    print('  Created', env_path, 'with defaults')
else:
    content = open(env_path).read()
    # If the file is missing essential defaults (was corrupted by a previous run),
    # restore them before setting HELIX_LOG_LEVEL.
    if 'MOONRAKER_HOST' not in content:
        content = DEFAULTS + content
        print('  Restored missing defaults in', env_path)
# Set HELIX_LOG_LEVEL=debug (remove any existing line first, then append)
content = re.sub(r'^#?\s*HELIX_LOG_LEVEL=.*\n?', '', content, flags=re.MULTILINE)
content = re.sub(r'^#?\s*HELIX_DEBUG=.*\n?', '', content, flags=re.MULTILINE)
content = content.rstrip('\n') + '\nHELIX_LOG_LEVEL=debug\n'
open(env_path, 'w').write(content)
print('  HELIX_LOG_LEVEL=debug  (debug logging enabled in', env_path + ')')
\"" 2>&1)"
    echo "$REMOTE_PY_OUT"
    # The python above stages the override under an unpredictable mkstemp name
    # and prints it as STAGED_OVERRIDE=<path>; carry it into the sudo install
    # below rather than a fixed /tmp path another user could pre-plant.
    STAGED_OVERRIDE="$(printf '%s\n' "$REMOTE_PY_OUT" | sed -n 's/^STAGED_OVERRIDE=//p')"
    if [[ -z "$STAGED_OVERRIDE" ]]; then
        echo "  ERROR: remote setup failed before staging update_urls.json"
        exit 1
    fi
    echo ""
    # The state dir must exist and stay owner-locked (755 root:root); a
    # group/world-writable parent makes the app refuse the override file.
    echo "[serve-local-update] Installing update_urls.json (root-owned, 644) on ${USERNAME}@${PRINTER} ..."
    ssh "${USERNAME}@${PRINTER}" "sudo mkdir -p /var/lib/helixscreen && sudo chmod 755 /var/lib/helixscreen && sudo install -o root -g root -m 644 '$STAGED_OVERRIDE' /var/lib/helixscreen/update_urls.json && rm -f '$STAGED_OVERRIDE'"
    echo "  dev_url = ${BASE_URL}/  (in /var/lib/helixscreen/update_urls.json)"
    echo ""
    echo "[serve-local-update] Copying install.sh to /tmp/ on ${USERNAME}@${PRINTER} ..."
    scp "$INSTALLER" "${USERNAME}@${PRINTER}:/tmp/install.sh"
    echo "  To test the installer directly on the device (bypasses update checker):"
    echo "    scp ${ZIP_PATH} ${USERNAME}@${PRINTER}:/tmp/"
    echo "    ssh ${USERNAME}@${PRINTER} 'sh /tmp/install.sh --local /tmp/${ZIP_NAME}'"
    echo ""
    echo "[serve-local-update] Restarting helix-screen on ${USERNAME}@${PRINTER} ..."
    ssh "${USERNAME}@${PRINTER}" "sudo systemctl restart helixscreen"
    echo "[serve-local-update] helix-screen restarted."
    echo "  To revert: sudo rm /var/lib/helixscreen/update_urls.json on the device and set update/channel back to 0 in ~/helixscreen/config/settings.json."
    echo ""
fi

# ── Serve ─────────────────────────────────────────────────────────────────────
echo ""
echo "  Ready"
echo "  ─────────────────────────────────────────────────"
echo "  Manifest : ${BASE_URL}/manifest.json"
echo "  Package  : ${BASE_URL}/${ZIP_NAME}"
echo ""
echo "  Device expects (set via --configure-remote if not already done):"
echo "    update/channel  = 2                            (~/helixscreen/config/settings.json)"
echo "    dev_url         = \"${BASE_URL}/\"   (/var/lib/helixscreen/update_urls.json, root-owned)"
echo ""
echo "  Press Ctrl+C to stop."
echo "  ─────────────────────────────────────────────────"
echo ""

cd "$PROJECT_DIR/dist"
python3 -m http.server "$PORT"
