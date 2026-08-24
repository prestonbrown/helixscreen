#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: release
# Release download and extraction
#
# Reads: GITHUB_REPO, TMP_DIR, INSTALL_DIR, SUDO
# Writes: CLEANUP_TMP, BACKUP_CONFIG, BACKUP_ENV, ORIGINAL_INSTALL_EXISTS

# Source guard
[ -n "${_HELIX_RELEASE_SOURCED:-}" ] && return 0
_HELIX_RELEASE_SOURCED=1

# R2 CDN configuration (overridable via environment)
: "${R2_BASE_URL:=https://releases.helixscreen.org}"
: "${R2_CHANNEL:=stable}"

# Plain HTTP endpoint for systems without SSL (K1, AD5M BusyBox wget)
: "${HTTP_BASE_URL:=http://dl.helixscreen.org}"

# Cached manifest from R2 (set by get_latest_version, consumed by download_release)
_R2_MANIFEST=""

# Which transport delivered _R2_MANIFEST ("https" or "http"). A hash read from
# the plain-HTTP mirror is integrity-only — it travelled the same channel the
# archive will — so download_release warns instead of claiming authenticity.
_R2_MANIFEST_TRANSPORT=""

# Which archive format is in use for this install. Set by download_release() or
# use_local_tarball() based on what was found/provided. Consumed by
# extract_release() and validate_tarball() to dispatch to the right tooling.
# Values: "zip" (preferred) or "tar.gz" (legacy fallback).
_ARCHIVE_FORMAT="tar.gz"

# Version tag and platform of a DOWNLOADED archive, recorded by download_release()
# after it normalizes the version. When set, the staged file carries them
# (helixscreen-<plat>-<ver>.zip) so a kept or aborted download shows which build
# it is. Empty by default so every other path (--local installs, tests that
# pre-stage an archive) keeps the bare helixscreen.<ext> name.
_ARCHIVE_VERSION=""
_ARCHIVE_PLATFORM=""

# Resolve the staged archive path in TMP_DIR for the current format. Versioned
# only when download_release() has recorded a platform+version; bare otherwise.
_archive_tmp_path() {
    local stem="helixscreen"
    if [ -n "${_ARCHIVE_PLATFORM:-}" ] && [ -n "${_ARCHIVE_VERSION:-}" ]; then
        stem="helixscreen-${_ARCHIVE_PLATFORM}-${_ARCHIVE_VERSION}"
    fi
    case "${_ARCHIVE_FORMAT:-tar.gz}" in
        zip) echo "${TMP_DIR}/${stem}.zip" ;;
        *)   echo "${TMP_DIR}/${stem}.tar.gz" ;;
    esac
}

# Detect whether curl supports the flags we need (not BusyBox curl).
# BusyBox curl lacks -S, -L, --connect-timeout, --max-time, --progress-bar, etc.
# Cache the result so we only probe once.
_REAL_CURL=""
_has_real_curl() {
    if [ -z "$_REAL_CURL" ]; then
        if command -v curl >/dev/null 2>&1 && curl --version 2>/dev/null | grep -qi "curl"; then
            _REAL_CURL=yes
        else
            _REAL_CURL=no
        fi
    fi
    [ "$_REAL_CURL" = "yes" ]
}

# User-Agent for python downloads. Our CDN/origin returns HTTP 403 to requests
# with an empty UA or the default Python-urllib/x.y UA; any other UA passes.
_PY_UA="helixscreen-installer/1.0"

# Core python urllib GET (fallback when curl/wget unavailable). Writes the
# response to DEST, or to stdout when DEST is "-". Sends a non-default
# User-Agent so the CDN doesn't 403 us (it rejects the urllib default).
# Returns non-zero on any error. Args: url dest("-"=stdout) [max_seconds]
# NOTE: urllib's timeout is a per-socket-operation (inactivity) timeout, not a
# total transfer deadline, and min_speed CDN fail-fast is not honored here.
_py_get() {
    _has_python || return 1
    HELIX_PY_URL="$1" HELIX_PY_DEST="$2" HELIX_PY_UA="$_PY_UA" \
        HELIX_PY_TIMEOUT="${3:-300}" "$_PY_BIN" - <<'PYEOF'
import os, sys, urllib.request, shutil
url = os.environ["HELIX_PY_URL"]
dest = os.environ["HELIX_PY_DEST"]
ua = os.environ["HELIX_PY_UA"]
try:
    timeout = float(os.environ.get("HELIX_PY_TIMEOUT", "300"))
except ValueError:
    timeout = 300.0
try:
    req = urllib.request.Request(url, headers={"User-Agent": ua})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        if dest == "-":
            shutil.copyfileobj(resp, sys.stdout.buffer)
        else:
            with open(dest, "wb") as out:
                shutil.copyfileobj(resp, out)
except Exception:
    sys.exit(1)
PYEOF
}

# Fetch a URL to stdout via python urllib. Args: url
_py_fetch() { _py_get "$1" "-" 15; }

# Download a URL to a file via python urllib. Args: url dest [max_seconds]
_py_download() { _py_get "$1" "$2" "${3:-300}"; }

# Test a zip archive for validity via python zipfile (fallback when unzip
# unavailable). Returns non-zero if the archive is missing or corrupt.
# Args: archive
_py_unzip_test() {
    _has_python || return 1
    HELIX_PY_ZIP="$1" "$_PY_BIN" - <<'PYEOF'
import os, sys, zipfile
path = os.environ["HELIX_PY_ZIP"]
try:
    with zipfile.ZipFile(path) as zf:
        if zf.testzip() is not None:
            sys.exit(1)
except Exception:
    sys.exit(1)
PYEOF
}

# Extract a zip archive into a destination directory via python zipfile
# (fallback when unzip unavailable). Restores unix permission bits from each
# entry's external_attr (extractall does NOT preserve them); files under a
# bin/ path are forced owner-executable so helixscreen/bin/helix-screen ends up
# runnable even when the zip carries no mode bits. Args: archive destdir
_py_unzip_extract() {
    _has_python || return 1
    HELIX_PY_ZIP="$1" HELIX_PY_DESTDIR="$2" "$_PY_BIN" - <<'PYEOF'
import os, sys, stat, zipfile
path = os.environ["HELIX_PY_ZIP"]
destdir = os.environ["HELIX_PY_DESTDIR"]
try:
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            zf.extract(info, destdir)
            # Directory entries: nothing to chmod.
            if info.filename.endswith("/"):
                continue
            target = os.path.join(destdir, info.filename)
            # Restore unix permission bits from the zip's external_attr
            # (extractall/extract do NOT preserve them).
            mode = (info.external_attr >> 16) & 0o777
            if mode:
                os.chmod(target, mode)
            # Ensure files under the top-level bin/ are owner-executable, even
            # when the zip carried no exec bit (e.g. bin/helix-screen stored as
            # 0644 or with no mode bits at all). Anchored to a leading "bin/"
            # component so unrelated nested dirs named "bin" aren't affected.
            parts = info.filename.split("/")
            if len(parts) > 1 and parts[0] == "bin":
                st = os.stat(target)
                os.chmod(target, st.st_mode | stat.S_IXUSR)
except Exception:
    sys.exit(1)
PYEOF
}

# Fetch a URL to stdout using curl or wget
# Returns non-zero if neither is available or fetch fails
fetch_url() {
    local url=$1
    if _has_real_curl; then
        curl -sSL --connect-timeout 10 "$url" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- --timeout=10 "$url" 2>/dev/null
    elif _has_python; then
        _py_fetch "$url"
    else
        return 1
    fi
}

# Fetch a URL via plain HTTP (for systems without SSL support)
# Prefers wget (BusyBox wget handles HTTP fine but not HTTPS)
fetch_url_http() {
    local url=$1
    if command -v wget >/dev/null 2>&1; then
        wget -qO- --timeout=10 "$url" 2>/dev/null
    elif _has_real_curl; then
        curl -sSL --connect-timeout 10 "$url" 2>/dev/null
    elif _has_python; then
        _py_fetch "$url"
    else
        return 1
    fi
}

# Download a file via plain HTTP (for systems without SSL support)
# Args: url dest [max_seconds]
download_file_http() {
    local url=$1 dest=$2 max_secs=${3:-300}
    if command -v wget >/dev/null 2>&1; then
        if [ -t 2 ]; then
            wget --timeout="$max_secs" -O "$dest" "$url" && \
                [ -f "$dest" ] && [ -s "$dest" ]
        else
            wget -q --timeout="$max_secs" -O "$dest" "$url" && \
                [ -f "$dest" ] && [ -s "$dest" ]
        fi
    elif _has_real_curl; then
        download_file "$url" "$dest" "$max_secs"
    elif _has_python; then
        _py_download "$url" "$dest" "$max_secs" && [ -f "$dest" ] && [ -s "$dest" ]
    else
        return 1
    fi
}

# Download a URL to a file
# Returns 0 on success (file exists and is non-empty), non-zero on failure
# Sets _DOWNLOAD_HTTP_CODE to the HTTP status (curl only, empty for wget)
# Args: url dest [max_seconds] [min_speed_bps]
#   max_seconds:  total transfer timeout (default 300)
#   min_speed_bps: abort if average speed stays below this for 20s (default 0 = disabled)
#                  Set to e.g. 51200 (50 KB/s) for CDN attempts so a slow CDN
#                  fails fast and the caller can fall through to a better source.
_DOWNLOAD_HTTP_CODE=""
download_file() {
    local url=$1 dest=$2 max_secs=${3:-300} min_speed=${4:-0}
    _DOWNLOAD_HTTP_CODE=""
    if _has_real_curl; then
        local http_code progress_flag speed_flags
        if [ -t 2 ]; then
            progress_flag="--progress-bar"
        else
            progress_flag="-s"
        fi
        # Speed floor: abort if average speed stays below min_speed for 20 consecutive
        # seconds. Lets a slow CDN fail fast so we can fall through to GitHub.
        if [ "${min_speed:-0}" -gt 0 ] 2>/dev/null; then
            speed_flags="--speed-limit ${min_speed} --speed-time 20"
        else
            speed_flags=""
        fi
        # Note: progress output goes to stderr naturally (no 2>&1).
        # The \n before %{http_code} ensures it's on its own line for tail -1.
        http_code=$(curl -SL \
            --connect-timeout 30 \
            --max-time "$max_secs" \
            $progress_flag \
            $speed_flags \
            -w "\n%{http_code}" \
            -o "$dest" \
            "$url") || true
        http_code=$(printf '%s' "$http_code" | tail -1)
        _DOWNLOAD_HTTP_CODE="$http_code"
        [ "$http_code" = "200" ] && [ -f "$dest" ] && [ -s "$dest" ]
    elif command -v wget >/dev/null 2>&1; then
        # wget has no built-in speed floor; rely on max_secs being short for CDN calls.
        if [ -t 2 ]; then
            wget --timeout="$max_secs" -O "$dest" "$url" && \
                [ -f "$dest" ] && [ -s "$dest" ]
        else
            wget -q --timeout="$max_secs" -O "$dest" "$url" && \
                [ -f "$dest" ] && [ -s "$dest" ]
        fi
    elif _has_python; then
        # urllib can't honor min_speed (no speed floor); rely on max_secs.
        if _py_download "$url" "$dest" "$max_secs" && [ -f "$dest" ] && [ -s "$dest" ]; then
            _DOWNLOAD_HTTP_CODE="200"
            return 0
        fi
        return 1
    else
        return 1
    fi
}

# Extract "version" value from manifest JSON on stdin
# Uses POSIX basic regex only (BusyBox compatible)
parse_manifest_version() {
    sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1
}

# Extract platform asset URL from manifest JSON on stdin
# Greps for the platform-specific filename pattern then extracts the URL
# Uses POSIX basic regex only (BusyBox compatible)
parse_manifest_platform_url() {
    local platform=$1
    grep "helixscreen-${platform}-" | \
        sed -n 's/.*"url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1
}

# Extract a platform's SHA256 from manifest JSON on stdin.
# Args: platform [zip]   — "zip" reads zip_sha256, anything else reads sha256.
# Prints the hex digest, or nothing when the manifest has no hash for it.
#
# Splitting each line on '"' and comparing whole fields (rather than regex) is
# what keeps "sha256" from also matching "zip_sha256", and "pi" from matching
# "pi32". Works on both the pretty-printed manifest jq emits and a compact
# one-line variant. awk is present on every target (BusyBox included).
parse_manifest_platform_sha256() {
    local platform=$1 kind=${2:-tar}
    local key="sha256"
    [ "$kind" = "zip" ] && key="zip_sha256"
    awk -v plat="$platform" -v key="$key" '
        {
            n = split($0, p, "\"")
            hit = 0
            for (i = 1; i <= n; i++) {
                if (p[i] == plat) { inblk = 1; hit = 1; continue }
                if (inblk && p[i] == key) { print p[i+2]; exit }
            }
            # Left the platform object without finding the key.
            if (inblk && !hit && index($0, "}")) inblk = 0
        }
    '
}

# Print the lowercase SHA256 of a file, or nothing when this system has no way
# to hash. Callers MUST treat empty as "unverifiable", never as "verified".
# BusyBox ships sha256sum on every platform we support, but old/minimal builds
# can omit the applet, hence the openssl and python fallbacks.
_sha256_file() {
    local f=$1 out=""
    if command -v sha256sum >/dev/null 2>&1; then
        out=$(sha256sum "$f" 2>/dev/null | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        out=$(shasum -a 256 "$f" 2>/dev/null | awk '{print $1}')
    elif command -v openssl >/dev/null 2>&1; then
        out=$(openssl dgst -sha256 "$f" 2>/dev/null | awk '{print $NF}')
    elif _py_has_module hashlib; then
        out=$("$_PY_BIN" -c 'import hashlib,sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as fh:
    for chunk in iter(lambda: fh.read(1 << 20), b""):
        h.update(chunk)
print(h.hexdigest())' "$f" 2>/dev/null)
    fi
    printf '%s' "$out" | tr 'A-Z' 'a-z'
}

# Set by _verify_archive_hash when a candidate was dropped purely for lack of a
# hash on the plain-HTTP transport. download_release reads it to decide whether
# the "everything failed" message should explain the override.
_UNVERIFIED_HTTP_REFUSED=false

# Gate a staged download on its manifest SHA256.
# Args: file expected_sha transport("https"|"http")
# Returns 0 to accept the file, 1 to reject it.
#
# Policy:
#   - hash present + match          -> accept
#   - hash present + mismatch       -> REJECT (always, both transports)
#   - hash present, no hash tool    -> falls through to the transport rule
#   - no hash, transport=https      -> accept; TLS authenticated the bytes
#   - no hash, transport=http       -> REJECT. An unauthenticated download
#     extracted over a live install is remote root on a printer. Overridable
#     with HELIX_ALLOW_UNVERIFIED_HTTP=1 for pinned-version installs on
#     HTTPS-incapable firmware, where no manifest hash exists to check.
#
# The no-hash-over-HTTP rejection is a WARNING, not an error, because it fires
# mid-cascade in the normal case: generate-manifest.sh withholds zip_url /
# zip_sha256 from the BusyBox platforms in ZIP_EXCLUDE_PLATFORMS (ad5m, ad5x,
# cc1, k1, k2, snapmaker-u1 — helixscreen#993), so on exactly the HTTP-only
# devices the zip candidate has no hash and must be skipped before the tar.gz
# candidate, which does have one, succeeds. Only download_release's final
# all-candidates-exhausted message spells out the override.
_verify_archive_hash() {
    local file=$1 expected=$2 transport=$3
    local actual

    if [ -n "$expected" ]; then
        actual=$(_sha256_file "$file")
        if [ "$actual" = "$expected" ]; then
            log_info "SHA256 verified ($(basename "$file"))"
            return 0
        fi
        if [ -z "$actual" ]; then
            log_warn "No sha256sum/openssl/python on this system — cannot verify $(basename "$file")."
        else
            log_error "SHA256 MISMATCH for $(basename "$file")"
            log_error "  expected: $expected"
            log_error "  actual:   $actual"
            log_error "The download does not match the release manifest. Refusing to install it."
            return 1
        fi
    fi

    [ "$transport" = "https" ] && return 0

    if [ "${HELIX_ALLOW_UNVERIFIED_HTTP:-0}" = "1" ]; then
        log_warn "Installing an UNVERIFIED plain-HTTP download (HELIX_ALLOW_UNVERIFIED_HTTP=1)."
        return 0
    fi
    _UNVERIFIED_HTTP_REFUSED=true
    log_warn "Refusing an unverified plain-HTTP download: $(basename "$file")"
    log_warn "No SHA256 was published for this artifact, and plain HTTP cannot"
    log_warn "authenticate what it delivered — the archive is extracted over the"
    log_warn "live install as root."
    return 1
}

# Populate _R2_MANIFEST if it isn't already. HTTPS first, plain-HTTP mirror
# second (matching get_latest_version's own source order), so an HTTPS-capable
# box gets an authenticated set of hashes even when it later has to fall back
# to the HTTP mirror for the much larger archive.
# Returns 0 when a manifest is in hand.
_ensure_manifest() {
    [ -n "$_R2_MANIFEST" ] && return 0

    if check_https_capability; then
        _R2_MANIFEST=$(fetch_url "${R2_BASE_URL}/${R2_CHANNEL}/manifest.json") || true
        if [ -n "$_R2_MANIFEST" ]; then
            _R2_MANIFEST_TRANSPORT="https"
            return 0
        fi
    fi

    _R2_MANIFEST=$(fetch_url_http "${HTTP_BASE_URL}/${R2_CHANNEL}/manifest.json") || true
    if [ -n "$_R2_MANIFEST" ]; then
        _R2_MANIFEST_TRANSPORT="http"
        return 0
    fi
    _R2_MANIFEST_TRANSPORT=""
    return 1
}

# True when the cached manifest describes the exact version being downloaded.
# The channel manifest only ever describes the LATEST release, so an explicit
# `--version vOLD` must not be checked against it — the hashes would be for a
# different build and every candidate would "mismatch".
_manifest_covers_version() {
    local want=$1 mver
    [ -n "$_R2_MANIFEST" ] || return 1
    mver=$(echo "$_R2_MANIFEST" | parse_manifest_version)
    [ -n "$mver" ] || return 1
    case "$mver" in
        v*) ;;
        *) mver="v${mver}" ;;
    esac
    [ "$mver" = "$want" ]
}

# Test a zip archive's integrity with whatever tool can actually do it.
# Echoes "ok" / "corrupt" / "unverifiable" and always returns 0, so callers
# can distinguish "this archive is bad" from "nothing here can check it".
#
# `unzip -t` cannot be the first choice because support for it varies by
# firmware vintage (prestonbrown/helixscreen#993):
#   - BusyBox 1.31.1 (Creality K1, verified on-device) has no -t at all:
#     "unzip: invalid option -- 't'", exit 1. An intact download is then
#     reported as a corrupt archive and every update fails.
#   - BusyBox 1.32+ (verified on 1.36.1, Elegoo Centauri Carbon) does support
#     -t and reports CRC errors correctly.
#   - info-zip (Debian/Pi/desktop) supports -t correctly.
# python3's zipfile.testzip() does a real per-entry CRC check and behaves
# identically everywhere, so it goes first and we never have to probe unzip's
# feature set. It needs zlib as well as zipfile: release zips are deflated, and
# the AD5M's python3.7 is built without zlib, where ZipFile() raises
# "Compression requires the (missing) zlib module" on a perfectly good archive.
_zip_integrity() {
    local archive=$1

    if _py_has_module zipfile zlib; then
        if _py_unzip_test "$archive"; then
            echo "ok"
        else
            echo "corrupt"
        fi
        return 0
    fi

    # No usable python zipfile (absent, or built without zlib -- AD5M).
    # `unzip -l` reads the central directory on both BusyBox and info-zip, so it
    # catches truncation and garbage without ever false-failing a valid archive
    # the way -t does on BusyBox 1.31 and older.
    if command -v unzip >/dev/null 2>&1; then
        if unzip -l "$archive" >/dev/null 2>&1; then
            echo "ok"
        else
            echo "corrupt"
        fi
        return 0
    fi

    echo "unverifiable"
}

# Validate an archive is readable and not truncated.
# Dispatches on the archive's filename extension: *.zip uses the portable
# integrity check above, everything else uses `gunzip -t`. Exits on failure.
# Args: archive_path, context (e.g., "Downloaded " or "Local ")
validate_archive() {
    local archive=$1
    local context=${2:-""}

    case "$archive" in
        *.zip)
            case "$(_zip_integrity "$archive")" in
                corrupt)
                    log_error "${context}file is not a valid zip archive."
                    [ -n "$context" ] && log_error "The ${context}may have been corrupted or incomplete."
                    exit 1
                    ;;
                unverifiable)
                    log_error "${context}file is a zip but neither unzip nor python3 is available to validate it."
                    exit 1
                    ;;
            esac
            ;;
        *)
            if ! gunzip -t "$archive" 2>/dev/null; then
                log_error "${context}file is not a valid gzip archive."
                [ -n "$context" ] && log_error "The ${context}may have been corrupted or incomplete."
                exit 1
            fi
            ;;
    esac

    # Releases should be >1MB — catch truncated downloads that nonetheless
    # parse as valid archives (e.g. an empty zip central directory).
    local size_kb
    size_kb=$(du -k "$archive" 2>/dev/null | cut -f1)
    if [ "${size_kb:-0}" -lt 1024 ]; then
        log_error "${context}file too small (${size_kb}KB). File may be incomplete."
        exit 1
    fi
}

# Backwards-compatible wrapper — new code should call validate_archive.
validate_tarball() {
    validate_archive "$1" "${2:-}"
}

# Check if we can download from HTTPS URLs
# BusyBox wget on AD5M doesn't support HTTPS
check_https_capability() {
    # python urllib with the ssl module reaches HTTPS on python-only systems
    # like recent Creality K2 firmware — treat as HTTPS-capable. (An ssl-less
    # python can still download over the plain-HTTP mirror, so it must NOT be
    # reported HTTPS-capable here.)
    if _py_has_module ssl; then
        return 0
    fi

    # curl with SSL support works (skip BusyBox curl which lacks needed flags)
    if _has_real_curl; then
        # Test if curl can reach HTTPS (quick timeout)
        if curl -sSL --connect-timeout 5 -o /dev/null "https://github.com" 2>/dev/null; then
            return 0
        fi
    fi

    # Check if wget supports HTTPS
    if command -v wget >/dev/null 2>&1; then
        # BusyBox wget outputs "not an http or ftp url" for https
        if wget --help 2>&1 | grep -qi "https"; then
            return 0
        fi
        # Try a test fetch - BusyBox wget fails immediately on https URLs
        if wget -q --timeout=5 -O /dev/null "https://github.com" 2>/dev/null; then
            return 0
        fi
    fi

    return 1
}

# Show manual install instructions when HTTPS download isn't available
show_manual_install_instructions() {
    local platform=$1
    local version=${2:-latest}

    echo ""
    log_error "=========================================="
    log_error "  HTTPS Download Not Available"
    log_error "=========================================="
    echo ""
    log_error "This system cannot download from HTTPS URLs."
    log_error "BusyBox wget (common on embedded devices) doesn't support HTTPS."
    echo ""
    log_info "To install HelixScreen, download the release on another computer"
    log_info "and copy it to this device:"
    printf '\n'
    printf '%b\n' "  1. Download the release:"
    if [ "$version" = "latest" ]; then
        printf '%b\n' "     ${CYAN}https://github.com/${GITHUB_REPO}/releases/latest${NC}"
    else
        printf '%b\n' "     ${CYAN}https://github.com/${GITHUB_REPO}/releases/tag/${version}${NC}"
    fi
    printf '\n'
    printf '%b\n' "  2. Download: ${BOLD}helixscreen-${platform}.zip${NC}"
    printf '\n'
    printf '%b\n' "  3. Copy to this device (note: AD5M needs -O flag):"
    if [ "$platform" = "ad5m" ]; then
        # AD5M /tmp is a tiny tmpfs (~54MB), use /data/ instead
        printf '%b\n' "     ${CYAN}scp -O helixscreen-${platform}.zip root@<this-ip>:/data/${NC}"
        printf '%b\n' "     ${YELLOW}Windows: use WSL, WinSCP (SCP mode), or PuTTY pscp${NC}"
        printf '\n'
        printf '%b\n' "  4. Run the installer with the local file:"
        printf '%b\n' "     ${CYAN}sh /data/install.sh --local /data/helixscreen-${platform}.zip${NC}"
    else
        printf '%b\n' "     ${CYAN}scp helixscreen-${platform}.zip root@<this-ip>:/tmp/${NC}"
        printf '\n'
        printf '%b\n' "  4. Run the installer with the local file:"
        printf '%b\n' "     ${CYAN}sh /tmp/install.sh --local /tmp/helixscreen-${platform}.zip${NC}"
    fi
    printf '\n'
    exit 1
}

# Get latest release version from GitHub (with R2 CDN as primary source)
# Returns the tag name as-is (e.g., "v0.9.3")
# Args: platform (for error message if HTTPS unavailable)
get_latest_version() {
    local platform=${1:-unknown}
    local version=""

    # Try HTTPS sources first (R2 CDN → GitHub API)
    if check_https_capability; then
        # Try R2 manifest first (faster CDN, no API rate limits)
        local manifest_url="${R2_BASE_URL}/${R2_CHANNEL}/manifest.json"
        log_info "Fetching latest version from CDN..."

        _R2_MANIFEST=$(fetch_url "$manifest_url") || true
        _R2_MANIFEST_TRANSPORT="https"
        if [ -n "$_R2_MANIFEST" ]; then
            version=$(echo "$_R2_MANIFEST" | parse_manifest_version)
            if [ -n "$version" ]; then
                # Manifest has bare version (e.g., "0.9.5"), we need the tag (e.g., "v0.9.5")
                version="v${version}"
                log_info "Latest version (CDN): ${version}"
                echo "$version"
                return 0
            fi
            log_warn "CDN manifest found but version could not be parsed, trying GitHub..."
            _R2_MANIFEST=""
            _R2_MANIFEST_TRANSPORT=""
        else
            log_warn "CDN unavailable, trying GitHub..."
        fi

        # Fallback: GitHub API
        local url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
        log_info "Fetching latest version from GitHub..."

        # Use basic sed regex (no -E flag) for BusyBox compatibility
        version=$(fetch_url "$url" | grep '"tag_name"' | sed 's/.*"\([^"][^"]*\)".*/\1/')

        if [ -n "$version" ]; then
            echo "$version"
            return 0
        fi
        log_warn "HTTPS sources failed, trying HTTP fallback..."
    else
        log_warn "HTTPS not available, trying HTTP fallback..."
    fi

    # HTTP fallback for systems without SSL (K1, AD5M BusyBox wget)
    local http_manifest_url="${HTTP_BASE_URL}/${R2_CHANNEL}/manifest.json"
    log_info "Fetching latest version via HTTP..."

    _R2_MANIFEST=$(fetch_url_http "$http_manifest_url") || true
    _R2_MANIFEST_TRANSPORT="http"
    if [ -n "$_R2_MANIFEST" ]; then
        version=$(echo "$_R2_MANIFEST" | parse_manifest_version)
        if [ -n "$version" ]; then
            version="v${version}"
            log_info "Latest version (HTTP): ${version}"
            echo "$version"
            return 0
        fi
    fi

    # All sources exhausted — show manual instructions
    log_error "Failed to fetch latest version from any source."
    show_manual_install_instructions "$platform" "latest"
}

# Map a detected platform identifier to the release artifact platform name.
# Most platforms use their own artifact name, but some share artifacts.
# Prints the release platform name to stdout.
get_release_platform() {
    local platform=$1
    local RELEASE_PLATFORM
    case "$platform" in
        snapmaker-u1) RELEASE_PLATFORM="snapmaker-u1" ;;
        *) RELEASE_PLATFORM="$platform" ;;
    esac
    echo "$RELEASE_PLATFORM"
}

# Try to download + validate a single candidate URL.
# Args: url dest transport ("https" | "http") [expected_sha256]
# Returns 0 on success (archive staged at dest), 1 otherwise.
# Validation is format-aware based on dest's filename extension, and is
# followed by the integrity gate in _verify_archive_hash — a gzip/zip CRC only
# proves the bytes arrived intact, not that they are the bytes we published.
_try_download_candidate() {
    local url=$1 dest=$2 transport=$3 expected_sha=${4:-}
    case "$transport" in
        https)
            download_file "$url" "$dest" 300 51200 || return 1 ;;
        http)
            download_file_http "$url" "$dest" 300 || return 1 ;;
        *)
            return 1 ;;
    esac

    case "$dest" in
        *.zip)
            # unzip -tqq returns 0 if the central directory + every entry CRC is valid.
            # Fall back to python zipfile when unzip isn't present (K2 firmware).
            if command -v unzip >/dev/null 2>&1; then
                unzip -tqq "$dest" >/dev/null 2>&1 || return 1
            elif _has_python; then
                _py_unzip_test "$dest" || return 1
            else
                return 1
            fi
            ;;
        *)
            gunzip -t "$dest" 2>/dev/null || return 1
            ;;
    esac

    _verify_archive_hash "$dest" "$expected_sha" "$transport" || return 1
    return 0
}

# Download release archive. Prefers the unversioned .zip (consumed by both this
# installer and Moonraker Update Manager) and falls back to the legacy versioned
# .tar.gz if no .zip is available at any transport — kept for bridge releases
# during the zip migration. Sets _ARCHIVE_FORMAT to record which format won.
# Tries R2 CDN, then GitHub Releases, then the plain-HTTP mirror.
download_release() {
    local version=$1
    local platform=$2
    platform=$(get_release_platform "$platform")

    # Normalize to the v-prefixed release tag. Release artifacts live under the
    # tag at every transport — R2 dir (releases/vX.Y.Z/), versioned tar filename
    # (helixscreen-<plat>-vX.Y.Z.tar.gz), and the GitHub tag (vX.Y.Z). A bare
    # version reaches here whenever the caller bypasses get_latest_version's own
    # normalization: an explicit `--version 0.99.80`, or the in-app updater
    # passing the manifest's bare `.version` field. Without the `v` every
    # candidate URL 404s. ("local" placeholder for --local installs is untouched.)
    case "$version" in
        v* | local) ;;
        [0-9]*) version="v${version}" ;;
    esac

    # Stage downloads under an identifiable local name (helixscreen-<plat>-<ver>.zip);
    # extract_release()/use_local_tarball() read it back via _archive_tmp_path().
    _ARCHIVE_VERSION="$version"
    _ARCHIVE_PLATFORM="$platform"

    mkdir -p "$TMP_DIR"
    CLEANUP_TMP=true

    # NOTE: *_filename are the fixed server-side asset names (fetch URLs); *_dest are
    # the local staged paths. Only the local names carry the version — keep them in
    # sync with _archive_tmp_path() above.
    local zip_filename="helixscreen-${platform}.zip"
    local zip_dest="${TMP_DIR}/helixscreen-${platform}-${version}.zip"
    local tar_filename="helixscreen-${platform}-${version}.tar.gz"
    local tar_dest="${TMP_DIR}/helixscreen-${platform}-${version}.tar.gz"

    # Make sure we have a manifest to check against. get_latest_version() is
    # invoked as `version=$(get_latest_version ...)`, so the _R2_MANIFEST it
    # caches lives and dies in that command substitution's subshell and never
    # reaches here — without this refetch every download is unverifiable.
    _ensure_manifest || true

    # The channel manifest only ever describes the LATEST release. Anything it
    # says (asset URLs *and* hashes) is off-limits when the caller pinned an
    # older --version, or we would download the newest build under the old
    # version's name and then fail its hash.
    local manifest_ok=false
    if _manifest_covers_version "$version"; then
        manifest_ok=true
    fi

    # Build candidate URL lists. Zip gets tried before tar at every transport.
    local zip_r2="${R2_BASE_URL}/releases/${version}/${zip_filename}"
    local tar_r2=""
    if [ "$manifest_ok" = true ]; then
        tar_r2=$(echo "$_R2_MANIFEST" | parse_manifest_platform_url "$platform")
    fi
    if [ -z "$tar_r2" ]; then
        tar_r2="${R2_BASE_URL}/releases/${version}/${tar_filename}"
    fi

    local zip_gh="https://github.com/${GITHUB_REPO}/releases/download/${version}/${zip_filename}"
    local tar_gh="https://github.com/${GITHUB_REPO}/releases/download/${version}/${tar_filename}"

    local zip_http="${HTTP_BASE_URL}/releases/${version}/${zip_filename}"
    local tar_http="${HTTP_BASE_URL}/releases/${version}/${tar_filename}"
    if [ "$manifest_ok" = true ]; then
        local http_manifest_url
        http_manifest_url=$(echo "$_R2_MANIFEST" | parse_manifest_platform_url "$platform")
        if [ -n "$http_manifest_url" ]; then
            tar_http=$(echo "$http_manifest_url" | sed "s|${R2_BASE_URL}|${HTTP_BASE_URL}|")
        fi
    fi

    # Expected SHA256s. All three transports serve byte-identical artifacts
    # (one CI job uploads the same files to the GitHub release and to R2, and
    # generates the manifest from them), so one pair of hashes covers every
    # candidate.
    local zip_sha="" tar_sha=""
    if [ "$manifest_ok" = true ]; then
        zip_sha=$(echo "$_R2_MANIFEST" | parse_manifest_platform_sha256 "$platform" zip)
        tar_sha=$(echo "$_R2_MANIFEST" | parse_manifest_platform_sha256 "$platform")
        # A hash fetched over the plain-HTTP mirror travelled the same
        # unauthenticated channel as the archive will, so it proves integrity
        # (truncation, a stale mirror) but not authenticity. Say so rather than
        # letting "SHA256 verified" imply more than it does.
        if [ "${_R2_MANIFEST_TRANSPORT:-}" = "http" ]; then
            log_warn "Release hashes came from the plain-HTTP mirror: they detect corruption,"
            log_warn "but cannot prove the release is genuine. Prefer an HTTPS-capable system."
        fi
    else
        log_warn "No release manifest for ${version} — SHA256 verification unavailable."
    fi

    log_info "Downloading HelixScreen ${version} for ${platform}..."

    local size
    # --- Attempt 1: R2 CDN, zip preferred ---
    log_info "URL: $zip_r2"
    if _try_download_candidate "$zip_r2" "$zip_dest" https "$zip_sha"; then
        _ARCHIVE_FORMAT="zip"
        size=$(ls -lh "$zip_dest" | awk '{print $5}')
        log_success "Downloaded ${zip_filename} (${size}) from CDN"
        return 0
    fi
    rm -f "$zip_dest"
    log_info "URL: $tar_r2"
    if _try_download_candidate "$tar_r2" "$tar_dest" https "$tar_sha"; then
        _ARCHIVE_FORMAT="tar.gz"
        size=$(ls -lh "$tar_dest" | awk '{print $5}')
        log_success "Downloaded ${tar_filename} (${size}) from CDN"
        return 0
    fi
    rm -f "$tar_dest"
    log_warn "CDN download failed, trying GitHub..."

    # --- Attempt 2: GitHub Releases ---
    log_info "URL: $zip_gh"
    if _try_download_candidate "$zip_gh" "$zip_dest" https "$zip_sha"; then
        _ARCHIVE_FORMAT="zip"
        size=$(ls -lh "$zip_dest" | awk '{print $5}')
        log_success "Downloaded ${zip_filename} (${size}) from GitHub"
        return 0
    fi
    rm -f "$zip_dest"
    log_info "URL: $tar_gh"
    if _try_download_candidate "$tar_gh" "$tar_dest" https "$tar_sha"; then
        _ARCHIVE_FORMAT="tar.gz"
        size=$(ls -lh "$tar_dest" | awk '{print $5}')
        log_success "Downloaded ${tar_filename} (${size}) from GitHub"
        return 0
    fi
    rm -f "$tar_dest"

    # --- Attempt 3: plain-HTTP mirror (BusyBox wget fallback) ---
    log_info "Trying HTTP fallback..."
    log_info "URL: $zip_http"
    if _try_download_candidate "$zip_http" "$zip_dest" http "$zip_sha"; then
        _ARCHIVE_FORMAT="zip"
        size=$(ls -lh "$zip_dest" | awk '{print $5}')
        log_success "Downloaded ${zip_filename} (${size}) via HTTP"
        return 0
    fi
    rm -f "$zip_dest"
    log_info "URL: $tar_http"
    if _try_download_candidate "$tar_http" "$tar_dest" http "$tar_sha"; then
        _ARCHIVE_FORMAT="tar.gz"
        size=$(ls -lh "$tar_dest" | awk '{print $5}')
        log_success "Downloaded ${tar_filename} (${size}) via HTTP"
        return 0
    fi
    rm -f "$tar_dest"

    log_error "Failed to download release."
    log_error "Tried zip: $zip_r2"
    log_error "Tried zip: $zip_gh"
    log_error "Tried zip: $zip_http"
    log_error "Tried tar: $tar_r2"
    log_error "Tried tar: $tar_gh"
    log_error "Tried tar: $tar_http"
    if [ -n "$_DOWNLOAD_HTTP_CODE" ] && [ "$_DOWNLOAD_HTTP_CODE" != "200" ]; then
        log_error "HTTP status: $_DOWNLOAD_HTTP_CODE"
    fi
    log_error ""
    log_error "Possible causes:"
    log_error "  - Version ${version} may not exist for platform ${platform}"
    log_error "  - Network connectivity issues"
    log_error "  - CDN, GitHub, and HTTP mirror may be unavailable"
    if [ "$_UNVERIFIED_HTTP_REFUSED" = true ]; then
        log_error "  - Every remaining candidate was a plain-HTTP download with no"
        log_error "    published SHA256, which this installer will not extract over"
        log_error "    your install unverified."
    fi
    log_error ""
    log_error "To install manually, download on another machine and use:"
    log_error "  ./install.sh --local /path/to/${zip_filename}"
    if [ "$_UNVERIFIED_HTTP_REFUSED" = true ]; then
        log_error ""
        log_error "Installing the CURRENT release instead usually fixes this — its"
        log_error "manifest carries hashes for every artifact. To accept an"
        log_error "unverified download anyway (not recommended):"
        log_error "  HELIX_ALLOW_UNVERIFIED_HTTP=1 sh install.sh ..."
    fi
    exit 1
}

# Use a local release archive instead of downloading.
# Accepts either .zip (preferred) or .tar.gz (legacy). Format is detected from
# the source filename extension and recorded in _ARCHIVE_FORMAT for
# extract_release() to dispatch on.
use_local_tarball() {
    local src=$1

    log_info "Using local archive: $src"

    case "$src" in
        *.zip)    _ARCHIVE_FORMAT="zip" ;;
        *.tar.gz|*.tgz) _ARCHIVE_FORMAT="tar.gz" ;;
        *)
            log_error "Unrecognized archive extension: $src"
            log_error "Expected a .zip or .tar.gz file."
            exit 1
            ;;
    esac

    validate_archive "$src" "Local "

    # Stage the archive under its canonical TMP_DIR name so extract_release()
    # and the in-app update flow can find it regardless of the source path.
    # Prefer a symlink to avoid copying large files on constrained devices.
    mkdir -p "$TMP_DIR"
    # shellcheck disable=SC2034  # consumed by common.sh error_handler (sibling module)
    CLEANUP_TMP=true

    local dest
    dest=$(_archive_tmp_path)
    if [ "$src" != "$dest" ]; then
        # Resolve to absolute path so a symlink created in $TMP_DIR doesn't
        # become dangling if the user passed a relative path.
        # (BusyBox readlink may not support -f, so try realpath first.)
        # NOTE: `|| abs_src=""` on each substitution is REQUIRED under `set -e`:
        # on BusyBox (K2/AD5M) `realpath` is often absent, so the substitution
        # exits 127 and would abort the whole installer before the fallback runs.
        local abs_src
        abs_src=$(realpath "$src" 2>/dev/null) || abs_src=""
        [ -n "$abs_src" ] || abs_src=$(readlink -f "$src" 2>/dev/null) || abs_src=""
        [ -n "$abs_src" ] || abs_src="$src"

        # Prefer a symlink to avoid copying large files on constrained devices,
        # but *verify* the staged tarball is readable. Fall back to copying.
        if ln -sf "$abs_src" "$dest" 2>/dev/null && [ -r "$dest" ]; then
            : # symlink OK
        elif cp "$abs_src" "$dest" 2>/dev/null && [ -r "$dest" ]; then
            : # copy OK
        else
            log_error "Failed to stage archive at $dest"
            log_error "Source: $abs_src"
            log_error "Check that the source file exists and the temp directory is writable."
            exit 1
        fi
    fi

    local size
    size=$(ls -lh "$src" | awk '{print $5}')
    log_success "Using local archive (${size}, ${_ARCHIVE_FORMAT})"
}

# Validate binary architecture matches the current system
# Args: binary_path platform
# Returns: 0 if valid, 1 if mismatch or error
validate_binary_architecture() {
    local binary=$1
    local platform=$2

    if [ ! -f "$binary" ]; then
        log_error "Binary not found: $binary"
        return 1
    fi

    # Read first 20 bytes of ELF header as space-separated hex.
    #
    # Each tool reads the file directly. Piping through `dd` made all three
    # attempts share a single producer, so one broken `dd` defeated the whole
    # fallback chain: a Pi CM4 with 32-bit userspace under a 64-bit kernel
    # segfaults in `dd bs=1` via the libarmmem memcpy shim that Raspberry Pi OS
    # force-loads from /etc/ld.so.preload.
    #
    # Order is load-bearing, verified against the physical fleet:
    #   hexdump - the only capable tool on minimal BusyBox (Elegoo CC1, where
    #             the `od` applet lacks -A/-t/-N and `xxd` is absent)
    #   od      - covers hosts with no hexdump at all (BTT CB1)
    #   xxd     - last; missing from several embedded images
    #
    # Each candidate must start with the ELF magic to be accepted, so a tool
    # that emits address offsets or a usage banner falls through to the next
    # method rather than being misread as a corrupt binary.
    local header="" raw="" candidate=""

    _try_header() {
        [ -n "$header" ] && return 0
        [ -z "$candidate" ] && return 1
        [ -z "$raw" ] && raw=$candidate
        case "$candidate" in
            "7f 45 4c 46 "*) header=$candidate ;;
        esac
        [ -n "$header" ]
    }

    candidate=$(hexdump -v -n 20 -e '1/1 "%02x "' "$binary" 2>/dev/null) || true
    _try_header
    if [ -z "$header" ]; then
        candidate=$(od -A n -t x1 -v -N 20 "$binary" 2>/dev/null | tr '\n' ' ' | tr -s ' ' | sed 's/^ //;s/ $//') || true
        _try_header
    fi
    if [ -z "$header" ]; then
        candidate=$(xxd -p -l 20 "$binary" 2>/dev/null | sed 's/../& /g;s/ $//') || true
        _try_header
    fi

    # Safety net: the original dd-piped forms. Retained so any platform whose
    # tools reject the direct-read flags behaves exactly as it did before.
    if [ -z "$header" ]; then
        candidate=$(dd if="$binary" bs=1 count=20 2>/dev/null | hexdump -v -e '1/1 "%02x "' 2>/dev/null) || true
        _try_header
    fi
    if [ -z "$header" ]; then
        candidate=$(dd if="$binary" bs=1 count=20 2>/dev/null | od -A n -t x1 -v 2>/dev/null | tr '\n' ' ' | tr -s ' ' | sed 's/^ //;s/ $//') || true
        _try_header
    fi
    if [ -z "$header" ]; then
        candidate=$(dd if="$binary" bs=1 count=20 2>/dev/null | xxd -p 2>/dev/null | sed 's/../& /g;s/ $//') || true
        _try_header
    fi

    # A file that read fine but isn't an ELF still needs the "not an ELF"
    # diagnosis below, not "cannot read".
    if [ -z "$header" ] && [ -n "$raw" ]; then
        header=$raw
    fi

    if [ -z "$header" ]; then
        log_error "Cannot read binary header (no working hex reader, or file is empty)"
        log_error "  Tried: hexdump, od, xxd (direct and via dd)"
        return 1
    fi

    # Parse space-separated hex bytes into individual values
    # Header format: "7f 45 4c 46 CC ... XX XX MM MM ..."
    # Byte 0-3: ELF magic (7f 45 4c 46)
    # Byte 4: EI_CLASS (01=32-bit, 02=64-bit)
    # Byte 18-19: e_machine LE (28 00=ARM, b7 00=AARCH64)

    local magic
    magic=$(echo "$header" | awk '{printf "%s%s%s%s", $1, $2, $3, $4}')
    if [ "$magic" != "7f454c46" ]; then
        log_error "Binary is not a valid ELF file"
        return 1
    fi

    local elf_class
    elf_class=$(echo "$header" | awk '{print $5}')

    local machine_lo
    machine_lo=$(echo "$header" | awk '{print $19}')

    # Determine expected values based on platform
    local expected_class expected_machine_lo expected_desc
    case "$platform" in
        ad5m|k2|pi32)
            expected_class="01"
            expected_machine_lo="28"
            expected_desc="ARM 32-bit (armv7l)"
            ;;
        ad5x|k1)
            expected_class="01"
            expected_machine_lo="08"
            expected_desc="MIPS 32-bit (mipsel)"
            ;;
        pi|snapmaker-u1)
            expected_class="02"
            expected_machine_lo="b7"
            expected_desc="AARCH64 64-bit"
            ;;
        x86)
            expected_class="02"
            expected_machine_lo="3e"
            expected_desc="x86_64 64-bit"
            ;;
        *)
            log_warn "Unknown platform '$platform', skipping architecture validation"
            return 0
            ;;
    esac

    local actual_desc
    if [ "$elf_class" = "01" ] && [ "$machine_lo" = "28" ]; then
        actual_desc="ARM 32-bit (armv7l)"
    elif [ "$elf_class" = "01" ] && [ "$machine_lo" = "08" ]; then
        actual_desc="MIPS 32-bit (mipsel)"
    elif [ "$elf_class" = "02" ] && [ "$machine_lo" = "b7" ]; then
        actual_desc="AARCH64 64-bit"
    elif [ "$elf_class" = "02" ] && [ "$machine_lo" = "3e" ]; then
        actual_desc="x86_64 64-bit"
    else
        actual_desc="unknown (class=$elf_class, machine=$machine_lo)"
    fi

    if [ "$elf_class" != "$expected_class" ] || [ "$machine_lo" != "$expected_machine_lo" ]; then
        log_error "Architecture mismatch!"
        log_error "  Expected: $expected_desc (for platform '$platform')"
        log_error "  Got:      $actual_desc"
        log_error "  This binary was built for the wrong platform."
        return 1
    fi

    log_info "Architecture validated: $actual_desc"
    return 0
}

# Find a writable, persistent mount dir on a DIFFERENT filesystem than
# INSTALL_PARENT with room for an off-partition rollback backup.
#
# Used when the install partition is too tight to hold the old + new install
# at once (e.g. K2 /opt: ~240MB, ~81MB free). Relocating the old install to a
# roomy partition frees the install fs so the new tree can move in, and the
# off-partition copy serves as the rollback source on failure.
#
# --- Filesystem measurement helpers -----------------------------------------
#
# `-P` is load-bearing, not decoration: without it df wraps a long device name
# onto its own line, and `tail -1 | awk '{print $1}'` then returns a BLOCK COUNT
# where the caller expects a device. Two filesystems would compare unequal by
# accident and a rename would be mistaken for a copy. POSIX output is one line
# per filesystem, which is why detect_rollback_dir already uses it.
#
# `-P` alone reports 512-byte blocks, so pair it with `-k` to get the 1K units
# the arithmetic below assumes. Verified on BusyBox 1.29.3 and 1.33.2.

# Echo the filesystem identity for a path (df's device column). Two paths with
# the same value are on one filesystem, so a mv between them is a rename.
_fs_id() {
    df -kP "$1" 2>/dev/null | tail -1 | awk '{print $1}'
}

# Echo free space in MB on the filesystem holding a path.
_fs_free_mb() {
    df -kP "$1" 2>/dev/null | tail -1 | awk '{print int($4/1024)}'
}

# Echo the size of a directory tree in MB.
#
# Echoes NOTHING when the measurement fails. Callers must treat that as
# "unknown" and refuse to proceed — never as zero. A zero silently turns every
# space guard below into `free < 10`, which passes on any healthy filesystem
# and protects nothing; that is the failure mode where the install dies
# mid-copy with ENOSPC having reported no problem at all.
# Verified present on BusyBox 1.29.3 (AD5M) and 1.33.2 (K2).
_tree_size_mb() {
    du -ms "$1" 2>/dev/null | awk '{print $1}'
}

# Decide whether the install swap can actually be performed.
#
# Args: SRC (the extracted tree) DST_PARENT (INSTALL_DIR's parent)
# Returns 0 to proceed, 1 to refuse. Logs the reason either way.
#
# A mv within one filesystem is a rename and needs no free space; across
# filesystems it is a copy-then-delete needing the whole tree's worth. The
# staging dir is routinely on a different partition now (the K2 stages on
# /mnt/UDISK and installs to /opt, because /opt is a 240MB overlay).
#
# The Phase 4 sizing upstream is not a substitute: it measures BEFORE the old
# install is moved aside, and its tight path then relocates that old install
# off-partition on the assumption the freed space suffices — which is false
# whenever the new tree is materially larger than the old one. This runs after
# all of that, against real free space, so an impossible copy fails having
# written nothing rather than dying half-way through with ENOSPC.
_check_swap_space() {
    local src="$1" dst_parent="$2"
    local src_fs dst_fs need_mb free_mb

    src_fs=$(_fs_id "$src")
    dst_fs=$(_fs_id "$dst_parent")

    # Same filesystem: the mv is a rename, so it consumes nothing.
    if [ -n "$src_fs" ] && [ -n "$dst_fs" ] && [ "$src_fs" = "$dst_fs" ]; then
        return 0
    fi

    need_mb=$(_tree_size_mb "$src")
    free_mb=$(_fs_free_mb "$dst_parent")

    # Both values must be plain digits before they reach `$(( ))`.
    #
    # This is not defensive padding. The installer runs under `set -eu`, and
    # dash aborts on `$(( garbage + 10 ))` with "Illegal number" — which would
    # kill the run in the one window where the old install has already been
    # moved aside and the new one is not in place yet, i.e. leave the printer
    # with no install at all. An empty value is just as bad the other way:
    # `$(( "" + 10 ))` quietly evaluates to 10, so the guard degrades to
    # `free < 10` and stops protecting anything.
    case "${need_mb}" in ''|*[!0-9]*) need_mb='' ;; esac
    case "${free_mb}" in ''|*[!0-9]*) free_mb='' ;; esac

    # Unmeasurable is NOT the same as fine. Say so out loud and continue rather
    # than inventing a zero, which would silence the guard on every platform.
    if [ -z "$need_mb" ] || [ -z "$free_mb" ]; then
        log_warn "Could not measure free space for the cross-filesystem install swap; proceeding without the check."
        return 0
    fi

    if [ "$free_mb" -lt $(( need_mb + 10 )) ]; then
        log_error "Not enough space to install: the new tree is ${need_mb}MB but only ${free_mb}MB is free on ${dst_parent}."
        log_error "The staging directory is on a different filesystem, so this step must COPY the tree, not rename it."
        log_error "Free up space on ${dst_parent} and re-run the installer."
        return 1
    fi

    log_info "Cross-filesystem install swap: ${need_mb}MB to copy, ${free_mb}MB free on ${dst_parent}"
    return 0
}

# Put the previous installation back after the swap could not be completed.
#
# Shared by every abort between "old install moved aside" and "new install in
# place": at that point INSTALL_DIR does not exist, so bailing without this
# leaves the box with no install at all. Args: REASON (logged only).
_restore_install_backup() {
    local reason="${1:-unknown}"

    [ -d "${INSTALL_BACKUP:-}" ] || return 0

    log_warn "Rolling back to previous installation (${reason})..."
    # Remove partial new install that may block the rollback mv.
    # Unescalated first, then escalate — the same two-attempt idiom the
    # stale-backup removals use. A forced $SUDO is not the safe choice here:
    # under NoNewPrivileges (self-update) sudo cannot run at all, so escalating
    # first turns a rollback that would have worked on a user-owned parent into
    # a failed one, and a failed rollback leaves the box with no install.
    if [ -d "${INSTALL_DIR}" ]; then
        $(path_sudo "${INSTALL_DIR}") rm -rf "${INSTALL_DIR}" 2>/dev/null || \
            $SUDO rm -rf "${INSTALL_DIR}" 2>/dev/null || true
    fi
    if $(path_sudo "$INSTALL_BACKUP") mv "$INSTALL_BACKUP" "${INSTALL_DIR}" 2>/dev/null || \
       $SUDO mv "$INSTALL_BACKUP" "${INSTALL_DIR}"; then
        log_warn "Rollback complete. Previous installation restored."
        # Off-partition rollback: the cross-fs mv leaves an empty
        # helixscreen-rollback/ dir behind. Remove it, but ONLY when its final
        # component is exactly helixscreen-rollback (never a bare mount root —
        # see the K2 /mnt/UDISK wipe incident).
        if [ -n "${HELIX_OFFSITE_ROLLBACK_DIR:-}" ]; then
            case "$HELIX_OFFSITE_ROLLBACK_DIR" in
                */helixscreen-rollback)
                    rm -rf "$HELIX_OFFSITE_ROLLBACK_DIR" 2>/dev/null || $SUDO rm -rf "$HELIX_OFFSITE_ROLLBACK_DIR" 2>/dev/null || true ;;
            esac
        fi
        return 0
    fi

    log_error "CRITICAL: Rollback failed! Previous install at $INSTALL_BACKUP"
    log_error "Manually restore with: mv $INSTALL_BACKUP ${INSTALL_DIR}"
    return 1
}

# Args: NEEDED_MB INSTALL_PARENT
# Echoes a qualifying candidate dir and returns 0, or echoes nothing and
# returns 1 if none qualifies. Requires NEEDED_MB + 20 MB free on the candidate.
detect_rollback_dir() {
    local needed_mb=$1 install_parent=$2
    local _candidates _cand _dev _free _install_dev

    # Candidate mounts, overridable for testability.
    _candidates="${HELIX_ROLLBACK_CANDIDATES:-/mnt/UDISK /usr/data /mnt/data /data /user-resource /oem /userdata /var/tmp}"

    # Filesystem device backing the install partition — candidates on the same
    # device free no space when we relocate there.
    _install_dev=$(df -P "$install_parent" 2>/dev/null | tail -1 | awk '{print $1}')

    for _cand in $_candidates; do
        [ -d "$_cand" ] || continue

        # Must be writable (honor sudo fallback like elsewhere in this file).
        if touch "$_cand/.helix_rollback_test" 2>/dev/null; then
            rm -f "$_cand/.helix_rollback_test" 2>/dev/null
        elif $SUDO touch "$_cand/.helix_rollback_test" 2>/dev/null; then
            $SUDO rm -f "$_cand/.helix_rollback_test" 2>/dev/null
        else
            continue
        fi

        _dev=$(df -P "$_cand" 2>/dev/null | tail -1 | awk '{print $1}')
        # Skip tmpfs — volatile RAM, a reboot mid-update loses the rollback.
        [ "$_dev" = "tmpfs" ] && continue
        # Skip same filesystem as the install — relocating there frees nothing.
        [ "$_dev" = "$_install_dev" ] && continue

        _free=$(df "$_cand" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
        if [ -n "$_free" ] && [ "$_free" -ge $(( needed_mb + 20 )) ]; then
            echo "$_cand"
            return 0
        fi
    done

    return 1
}

# Extract archive with atomic swap and rollback protection.
# Dispatches on _ARCHIVE_FORMAT for zip vs tar.gz. Expects the archive already
# staged at _archive_tmp_path() by download_release() or use_local_tarball().
extract_release() {
    local platform=$1
    local archive
    archive=$(_archive_tmp_path)
    local extract_dir="${TMP_DIR}/extract"
    local new_install="${extract_dir}/helixscreen"

    # Pre-flight: check TMP_DIR has enough space for extraction.
    # Archive expands ~3x, so require 3x archive size + margin.
    local archive_mb extract_required_mb tmp_available_mb
    archive_mb=$(du -m "$archive" 2>/dev/null | awk '{print $1}')
    [ -z "$archive_mb" ] && archive_mb=$(ls -l "$archive" | awk '{print int($5/1048576)}')
    extract_required_mb=$(( (archive_mb * 3) + 20 ))

    local tmp_check_dir
    tmp_check_dir=$(dirname "$TMP_DIR")
    while [ ! -d "$tmp_check_dir" ] && [ "$tmp_check_dir" != "/" ]; do
        tmp_check_dir=$(dirname "$tmp_check_dir")
    done
    tmp_available_mb=$(df "$tmp_check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')

    if [ -n "$tmp_available_mb" ] && [ "$tmp_available_mb" -lt "$extract_required_mb" ]; then
        log_error "Not enough space in temp directory for extraction."
        log_error "Temp directory: $tmp_check_dir (${tmp_available_mb}MB free, need ${extract_required_mb}MB)"
        log_error "Try: TMP_DIR=/path/with/space sh install.sh ..."
        exit 1
    fi

    log_info "Extracting release (${_ARCHIVE_FORMAT})..."

    # Phase 1: Extract to temporary directory
    mkdir -p "$extract_dir"
    cd "$extract_dir" || exit 1

    local extract_ok=false
    case "${_ARCHIVE_FORMAT:-tar.gz}" in
        zip)
            # The zip has a FLAT layout (no top-level helixscreen/ dir). That's
            # the contract with Moonraker Update Manager — it extracts straight
            # into the printer_data helixscreen directory. To keep downstream
            # code layout-agnostic, we re-create the helixscreen/ prefix here
            # by extracting into a subdirectory.
            # -o overwrites without prompting (no TTY in CI / systemd contexts),
            # -q suppresses the per-file listing. Fall back to python zipfile
            # when unzip isn't present (recent Creality K2 firmware).
            mkdir -p helixscreen
            if command -v unzip >/dev/null 2>&1; then
                ( cd helixscreen && unzip -q -o "$archive" ) && extract_ok=true
            elif _has_python; then
                _py_unzip_extract "$archive" "${extract_dir}/helixscreen" && extract_ok=true
            fi
            ;;
        *)
            # `-o` means "don't restore user:group". Without it a root extract
            # restores the numeric uid/gid the archive was built with (1001 on
            # the CI runner), leaving the install owned by a user that does not
            # exist on the printer. It is needed here as well as at packaging
            # time because archives already published still carry those ids.
            #
            # `-o` is the ONLY portable spelling: BusyBox tar (1.33.2 on the K2)
            # documents `-o` but has no --no-same-owner long option, so passing
            # the long form fails extraction outright on every Creality box.
            # GNU tar accepts -o as an alias for --no-same-owner when extracting.
            #
            # BusyBox tar doesn't support -z; use gunzip pipe on embedded platforms
            case "$platform" in
                ad5m|ad5x|k1|k2)
                    gunzip -c "$archive" | tar xof - && extract_ok=true ;;
                *)
                    tar -xzof "$archive" && extract_ok=true ;;
            esac
            ;;
    esac

    if [ "$extract_ok" = false ]; then
        local post_mb
        post_mb=$(df "$tmp_check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
        if [ -n "$post_mb" ] && [ "$post_mb" -lt 5 ]; then
            log_error "Failed to extract archive: no space left on device."
            log_error "Filesystem $(df "$tmp_check_dir" | tail -1 | awk '{print $1}') is full."
            log_error "Try: TMP_DIR=/path/with/space sh install.sh ..."
        else
            log_error "Failed to extract archive."
            log_error "The archive may be corrupted. Try re-downloading."
        fi
        rm -rf "$extract_dir"
        exit 1
    fi

    # Phase 2: Validate extracted content.
    #
    # The in-place update path (NoNewPrivileges, parent read-only) deletes the
    # old contents before moving the new ones in — so a new tree missing any
    # critical top-level entry leaves the user with a half-installed system
    # (see prestonbrown/helixscreen#970: ui_xml absent → "Could not find
    # HelixScreen data root" dead loop). Validate the full set up-front so we
    # bail BEFORE touching the existing install.
    _missing=""
    for _required in bin/helix-screen ui_xml assets; do
        if [ ! -e "${new_install}/${_required}" ]; then
            _missing="${_missing} ${_required}"
        fi
    done
    if [ -n "$_missing" ]; then
        log_error "Extracted archive is incomplete — missing:${_missing}"
        log_error "Expected layout: helixscreen/{bin/helix-screen, ui_xml/, assets/, …}"
        log_error "The archive may be corrupted or built without all asset stages."
        log_error "Existing installation is untouched."
        rm -rf "$extract_dir"
        exit 1
    fi

    # Phase 3: Validate architecture
    if ! validate_binary_architecture "${new_install}/bin/helix-screen" "$platform"; then
        log_error "Aborting installation due to architecture mismatch."
        rm -rf "$extract_dir"
        exit 1
    fi

    # Phase 4: Backup existing installation (if present)
    if [ -d "${INSTALL_DIR}" ]; then
        ORIGINAL_INSTALL_EXISTS=true

        # Backup config (check new name first, then legacy names)
        if [ -f "${INSTALL_DIR}/config/settings.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/settings.json.backup"
            cp "${INSTALL_DIR}/config/settings.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (from config/settings.json)"
        elif [ -f "${INSTALL_DIR}/config/helixconfig.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/settings.json.backup"
            cp "${INSTALL_DIR}/config/helixconfig.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (from config/helixconfig.json)"
        elif [ -f "${INSTALL_DIR}/settings.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/settings.json.backup"
            cp "${INSTALL_DIR}/settings.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (legacy root location)"
        elif [ -f "${INSTALL_DIR}/helixconfig.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/settings.json.backup"
            cp "${INSTALL_DIR}/helixconfig.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (legacy root location)"
        fi

        # Backup helixscreen.env (preserves HELIX_LOG_LEVEL and other env customizations)
        if [ -f "${INSTALL_DIR}/config/helixscreen.env" ]; then
            BACKUP_ENV="${TMP_DIR}/helixscreen.env.backup"
            cp "${INSTALL_DIR}/config/helixscreen.env" "$BACKUP_ENV"
            log_info "Backed up existing helixscreen.env"
        fi

        # Under NoNewPrivileges (self-update from in-app), we prefer the
        # atomic swap (mv old; mv new) if the parent dir is writable (service
        # file v0.97.4+ adds ReadWritePaths for it).  Fall back to the racy
        # in-place content replacement only on older service files where the
        # parent is still read-only under ProtectSystem=strict.
        if _has_no_new_privs; then
            # Verify INSTALL_DIR is writable.  Older service files only grant
            # ReadWritePaths to config/, so ProtectSystem=strict blocks writes
            # to the rest.  If so, the ExecStartPre ownership fix in the new
            # service file will resolve this on next restart.
            if ! touch "${INSTALL_DIR}/.update_test" 2>/dev/null; then
                log_error "Cannot write to ${INSTALL_DIR} (read-only under ProtectSystem)."
                log_error "The systemd service file needs updating to allow self-updates."
                log_error "Fix: re-run the installer once with:"
                log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
                rm -rf "$extract_dir"
                exit 1
            fi
            rm -f "${INSTALL_DIR}/.update_test" 2>/dev/null

            # Test if parent dir is writable — if so, skip the racy in-place
            # path and fall through to the atomic swap below.
            local _parent_dir
            _parent_dir="$(dirname "${INSTALL_DIR}")"
            if touch "${_parent_dir}/.update_test" 2>/dev/null; then
                rm -f "${_parent_dir}/.update_test" 2>/dev/null
                log_info "Parent dir writable — using atomic swap"
                # Fall through to the standard atomic swap path below
            else
                log_info "Self-update: replacing install contents in-place (parent read-only)..."

                # Remove old contents (except config/).
                # Don't use || true — if rm fails, we must not proceed to mv
                # because mv can't overwrite a non-empty directory.
                local _inplace_failed=false
                for _item in "${INSTALL_DIR}"/*; do
                    [ -e "$_item" ] || continue
                    _base=$(basename "$_item")
                    [ "$_base" = "config" ] && continue
                    if ! rm -rf "$_item"; then
                        log_error "Failed to remove old ${_base}"
                        _inplace_failed=true
                    fi
                done
                # Hidden files too
                for _item in "${INSTALL_DIR}"/.*; do
                    [ -e "$_item" ] || continue
                    _base=$(basename "$_item")
                    case "$_base" in .|..) continue ;; esac
                    rm -rf "$_item" 2>/dev/null || true
                done

                if [ "$_inplace_failed" = true ]; then
                    log_error "In-place update failed. Install may be in a broken state."
                    log_error "Fix: re-run the installer: curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
                    rm -rf "$extract_dir"
                    exit 1
                fi

                # Move new contents in (except config/)
                for _item in "${new_install}"/*; do
                    [ -e "$_item" ] || continue
                    _base=$(basename "$_item")
                    [ "$_base" = "config" ] && continue
                    if ! mv "$_item" "${INSTALL_DIR}/${_base}"; then
                        log_error "Failed to install: ${_base}"
                        rm -rf "$extract_dir"
                        exit 1
                    fi
                done
                # Hidden files too
                for _item in "${new_install}"/.*; do
                    [ -e "$_item" ] || continue
                    _base=$(basename "$_item")
                    case "$_base" in .|..) continue ;; esac
                    mv "$_item" "${INSTALL_DIR}/${_base}" 2>/dev/null || true
                done

                # Merge new config defaults without overwriting user files.
                # New versions may ship config files that didn't exist before.
                if [ -d "${new_install}/config" ]; then
                    for _item in "${new_install}/config"/*; do
                        [ -e "$_item" ] || continue
                        _base=$(basename "$_item")
                        if [ ! -e "${INSTALL_DIR}/config/${_base}" ]; then
                            mv "$_item" "${INSTALL_DIR}/config/${_base}" 2>/dev/null || true
                            log_info "Added new config default: ${_base}"
                        elif [ -d "$_item" ] && [ -d "${INSTALL_DIR}/config/${_base}" ]; then
                            # Merge directory contents (e.g. printer_database.d/)
                            for _subitem in "$_item"/*; do
                                [ -e "$_subitem" ] || continue
                                _subbase=$(basename "$_subitem")
                                if [ ! -e "${INSTALL_DIR}/config/${_base}/${_subbase}" ]; then
                                    mv "$_subitem" "${INSTALL_DIR}/config/${_base}/${_subbase}" 2>/dev/null || true
                                fi
                            done
                        fi
                    done
                fi

                rm -rf "$extract_dir"
                log_success "Updated in-place at ${INSTALL_DIR}"
                return 0
            fi
        fi

        # Standard path (fresh install or non-self-update with sudo access):
        # atomic directory swap via rename in parent directory.

        # Sizing: can the install partition hold the old AND new install at once?
        # On a cramped overlay (e.g. K2 /opt) it can't — the old .old copy plus
        # the freshly extracted tree exceeds free space and the move-new-in step
        # fails mid-write with ENOSPC. When tight, relocate the old install to a
        # roomy persistent partition first (option B) so the install fs has room.
        local new_install_mb install_parent install_free_mb swap_margin_mb
        new_install_mb=$(du -ms "$new_install" 2>/dev/null | awk '{print $1}')
        [ -z "$new_install_mb" ] && new_install_mb=0

        install_parent=$(dirname "${INSTALL_DIR}")
        while [ ! -d "$install_parent" ] && [ "$install_parent" != "/" ]; do
            install_parent=$(dirname "$install_parent")
        done
        install_free_mb=$(df "$install_parent" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
        [ -z "$install_free_mb" ] && install_free_mb=0

        swap_margin_mb=10

        if [ "$install_free_mb" -lt $(( new_install_mb + swap_margin_mb )) ]; then
            # Tight: the install partition can't hold old + new simultaneously.
            roomy=$(detect_rollback_dir "$new_install_mb" "$install_parent") || roomy=""
            if [ -n "$roomy" ]; then
                # Relocate the old install OFF the cramped partition to a roomy,
                # persistent one. The off-partition copy is the rollback source.
                HELIX_OFFSITE_ROLLBACK_DIR="${roomy}/helixscreen-rollback"
                INSTALL_BACKUP="${HELIX_OFFSITE_ROLLBACK_DIR}/helixscreen"
                if [ -e "$INSTALL_BACKUP" ]; then
                    rm -rf "$INSTALL_BACKUP" 2>/dev/null || $SUDO rm -rf "$INSTALL_BACKUP" 2>/dev/null || true
                fi
                if ! $(file_sudo "$roomy") mkdir -p "$HELIX_OFFSITE_ROLLBACK_DIR" 2>/dev/null; then
                    log_error "Failed to create off-partition rollback dir at ${HELIX_OFFSITE_ROLLBACK_DIR}"
                    rm -rf "$extract_dir"
                    exit 1
                fi
                log_info "Install partition tight (${install_free_mb}MB free, need ~${new_install_mb}MB); staging rollback backup off-partition at ${HELIX_OFFSITE_ROLLBACK_DIR}"

                # Cross-fs move (copy to roomy + delete) frees the install fs.
                if ! $(path_sudo "${INSTALL_DIR}") mv "${INSTALL_DIR}" "$INSTALL_BACKUP"; then
                    log_error "Failed to relocate existing installation off-partition."
                    rm -rf "$extract_dir"
                    exit 1
                fi
            else
                # Tight and nowhere safe to back up — never delete the old
                # install without a working rollback copy. Fail clean.
                log_error "Not enough space on ${install_parent} (${install_free_mb}MB free) to install this update (~${new_install_mb}MB), and no alternate partition with room for a safe rollback backup was found."
                log_error "Free up space on ${install_parent} and try again."
                rm -rf "$extract_dir"
                exit 1
            fi
        else
            # Roomy: keep the existing same-fs atomic swap behavior.
            # Prefer INSTALL_DIR.old; if it exists and can't be removed (e.g. root-owned
            # under NoNewPrivileges), fall back to a timestamped name so the swap succeeds.
            INSTALL_BACKUP="${INSTALL_DIR}.old"
            if [ -d "$INSTALL_BACKUP" ]; then
                log_info "Removing stale backup from previous install..."
                if ! rm -rf "$INSTALL_BACKUP" 2>/dev/null && ! $SUDO rm -rf "$INSTALL_BACKUP" 2>/dev/null; then
                    INSTALL_BACKUP="${INSTALL_DIR}.old.$(date +%s)"
                    log_warn "Could not remove stale .old dir (root-owned?); using $INSTALL_BACKUP instead"
                fi
            fi

            # Atomic swap: move old install to backup
            if ! $(path_sudo "${INSTALL_DIR}") mv "${INSTALL_DIR}" "$INSTALL_BACKUP"; then
                log_error "Failed to backup existing installation."
                rm -rf "$extract_dir"
                exit 1
            fi
        fi
    fi

    # Phase 5: Move new install into place
    $(file_sudo "$(dirname "${INSTALL_DIR}")") mkdir -p "$(dirname "${INSTALL_DIR}")"

    # A mv within one filesystem is a rename and needs no free space. Across
    # filesystems it is a copy-then-delete that needs the whole tree's worth —
    # and TMP_DIR is routinely on a different partition now (the K2 stages on
    # /mnt/UDISK and installs to /opt, because /opt is a 240MB overlay).
    #
    # Phase 4 already sized this, but only against the state BEFORE the old
    # install was moved aside. Its tight path relocates the old install
    # off-partition and assumes that freed enough room, which is false whenever
    # the new tree is materially larger than the old one. Re-measure against
    # real free space here, so an impossible copy fails having written nothing
    # instead of dying half-way through with ENOSPC.
    if ! _check_swap_space "${new_install}" "$(dirname "${INSTALL_DIR}")"; then
        _restore_install_backup "space check"
        rm -rf "$extract_dir"
        exit 1
    fi

    if ! $(file_sudo "$(dirname "${INSTALL_DIR}")") mv "${new_install}" "${INSTALL_DIR}"; then
        log_error "Failed to install new release."
        _restore_install_backup "failed swap"
        rm -rf "$extract_dir"
        exit 1
    fi

    # Phase 6: Restore config and settings
    # User's config always takes priority over bundled defaults so customizations
    # survive updates.  Try TMP_DIR backup first; fall back to the .old copy.
    # (Under systemd PrivateTmp=true the TMP_DIR mount can vanish on restart,
    # so the .old directory on the real filesystem acts as a safety net.)
    $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config" 2>/dev/null || true

    # Does a user config actually exist to restore?  Must match the candidate
    # chain the restore below walks, or the removal here outruns it.
    #
    # ORIGINAL_INSTALL_EXISTS is not that test: it is set from `[ -d INSTALL_DIR ]`
    # alone, and embedded targets keep logs and cache under the install dir
    # (K1: /usr/data/helixscreen/{logs,cache}), so the directory routinely
    # predates a first install with no config in it.
    _have_restore_candidate=false
    if [ -n "${BACKUP_CONFIG:-}" ] && [ -s "$BACKUP_CONFIG" ]; then
        _have_restore_candidate=true
    elif [ -n "${INSTALL_BACKUP:-}" ] && { [ -f "${INSTALL_BACKUP}/config/settings.json" ] ||
        [ -f "${INSTALL_BACKUP}/config/helixconfig.json" ] ||
        [ -f "${INSTALL_BACKUP}/settings.json" ] ||
        [ -f "${INSTALL_BACKUP}/helixconfig.json" ]; }; then
        _have_restore_candidate=true
    fi

    # Remove the archive's settings.json only when a restore candidate exists.
    # If the restore below then fails, we want the file to be MISSING so that
    # Config::init()'s restore_from_backup() safety net kicks in.  Removing it
    # with nothing to put back instead strands the app with no config at all -
    # and on the preset platforms (k1/k2/cc1/ad5m/ad5x/snapmaker_u1, see
    # mk/cross.mk) that file IS the platform preset carrying display rotation
    # and hardware mapping.
    #
    # Note: helixscreen.env is NOT removed here — there is no Config::init
    # safety net for env files, and the restore step below uses `cp` which
    # overwrites, so leaving the tarball's bundled copy in place gives both
    # behaviors (user backup wins if present, bundled default stays otherwise).
    # Removing it caused the env file to disappear permanently across upgrades
    # when no backup existed (Pi user report 2026-05-13).
    if [ "$_have_restore_candidate" = true ]; then
        rm -f "${INSTALL_DIR}/config/settings.json" 2>/dev/null
    fi

    # _restore_config_file SRC DEST LABEL — copy a single config file with
    # appropriate sudo, logging success or warning on failure.
    _restore_config_file() {
        local _src=$1 _dest=$2 _label=$3
        if $(file_sudo "$(dirname "$_dest")") cp "$_src" "$_dest" 2>/dev/null; then
            log_info "Restored $_label"
        else
            log_warn "Failed to restore $_label"
        fi
    }

    # Restore settings.json — try candidates in priority order (new name, then legacy)
    _config_dest="${INSTALL_DIR}/config/settings.json"
    if [ -n "${BACKUP_CONFIG:-}" ] && [ -s "$BACKUP_CONFIG" ]; then
        _restore_config_file "$BACKUP_CONFIG" "$_config_dest" "settings.json from TMP_DIR backup"
    elif [ -n "${INSTALL_BACKUP:-}" ]; then
        if [ -f "${INSTALL_BACKUP}/config/settings.json" ]; then
            _restore_config_file "${INSTALL_BACKUP}/config/settings.json" "$_config_dest" "settings.json from .old backup"
        elif [ -f "${INSTALL_BACKUP}/config/helixconfig.json" ]; then
            _restore_config_file "${INSTALL_BACKUP}/config/helixconfig.json" "$_config_dest" "settings.json from .old backup (migrated from helixconfig.json)"
        elif [ -f "${INSTALL_BACKUP}/settings.json" ]; then
            _restore_config_file "${INSTALL_BACKUP}/settings.json" "$_config_dest" "settings.json from .old backup (legacy root location)"
        elif [ -f "${INSTALL_BACKUP}/helixconfig.json" ]; then
            _restore_config_file "${INSTALL_BACKUP}/helixconfig.json" "$_config_dest" "settings.json from .old backup (legacy root location)"
        fi
    fi
    if [ ! -f "$_config_dest" ] && [ "$_have_restore_candidate" = true ]; then
        log_warn "Could not restore settings.json from any backup source!"
        log_warn "User configuration may have been lost."
    fi

    # Mark a packaged config the installer deliberately kept.
    #
    # A packaged settings.json is ambiguous to Config::init(): byte-for-byte the
    # same document ships with a fresh install and is what Moonraker's type:web
    # update leaves behind after its rmtree() destroys the user's copy.  Nothing
    # inside the document, and neither the age nor the richness of the rolling
    # backup, separates the two.  The installer can: it knows there was no user
    # config here.  Moonraker never runs the installer and its rmtree() takes
    # the marker with it, so an absent marker means the config was clobbered and
    # Config::init() should restore the rolling backup over it.
    _fresh_marker="${INSTALL_DIR}/config/.helix-fresh-install"
    if [ "$_have_restore_candidate" = false ] && [ -f "$_config_dest" ]; then
        $(file_sudo "${INSTALL_DIR}/config") touch "$_fresh_marker" 2>/dev/null || true
    else
        $(file_sudo "${INSTALL_DIR}/config") rm -f "$_fresh_marker" 2>/dev/null || true
    fi

    # Restore helixscreen.env — user may have customized HELIX_LOG_LEVEL,
    # MOONRAKER_HOST, etc.  Overwrite the bundled default with their backup.
    _env_dest="${INSTALL_DIR}/config/helixscreen.env"
    if [ -n "${BACKUP_ENV:-}" ] && [ -s "$BACKUP_ENV" ]; then
        _restore_config_file "$BACKUP_ENV" "$_env_dest" "helixscreen.env from TMP_DIR backup"
    elif [ -n "${INSTALL_BACKUP:-}" ] && [ -f "${INSTALL_BACKUP}/config/helixscreen.env" ]; then
        _restore_config_file "${INSTALL_BACKUP}/config/helixscreen.env" "$_env_dest" "helixscreen.env from .old backup"
    fi

    # One-time migration: older env templates shipped `HELIX_LOG_LEVEL=info`
    # uncommented, which silently shadowed the in-app Settings → System → Log
    # Level setting on every restart. Comment it out IFF it still matches the
    # exact old default, so users who deliberately set a different value keep
    # their customization.
    if [ -f "$_env_dest" ] && grep -q '^HELIX_LOG_LEVEL=info[[:space:]]*$' "$_env_dest"; then
        $(file_sudo "$_env_dest") sed -i 's/^HELIX_LOG_LEVEL=info[[:space:]]*$/#HELIX_LOG_LEVEL=info/' "$_env_dest" 2>/dev/null && \
            log_info "Migrated helixscreen.env: commented out default HELIX_LOG_LEVEL=info (in-app Log Level setting now applies)"
    fi

    # Prune legacy seed files from the .old backup before the restore loop.
    # Older releases shipped these inside config/; current releases ship them
    # under assets/config/ instead. Without this prune, the loop below would
    # restore the *old* shipped versions on top of the new ones — users would
    # be stuck on stale printer_database.json / themes / presets forever.
    # User-state files (settings.json, telemetry_*.json, custom_images/,
    # printer_database.d/, themes/<user-themes>.json) are untouched.
    if [ -n "${INSTALL_BACKUP:-}" ] && [ -d "${INSTALL_BACKUP}/config" ]; then
        for _legacy_seed in printer_database.json printing_tips.json default_layout.json helix_macros.cfg; do
            $(file_sudo "${INSTALL_BACKUP}/config") rm -f "${INSTALL_BACKUP}/config/${_legacy_seed}" 2>/dev/null || true
        done
        for _legacy_seed_dir in presets print_start_profiles sounds platform; do
            $(file_sudo "${INSTALL_BACKUP}/config") rm -rf "${INSTALL_BACKUP}/config/${_legacy_seed_dir}" 2>/dev/null || true
        done
        # themes/defaults moved out of themes/; keep user themes (sibling files).
        $(file_sudo "${INSTALL_BACKUP}/config/themes") rm -rf "${INSTALL_BACKUP}/config/themes/defaults" 2>/dev/null || true
        # Transient crash/watchdog state must never be restored. A fresh install
        # ships no crash.txt, so the restore loop below would otherwise copy the
        # old one back on EVERY update — resurrecting the "previously crashed"
        # dialog the user already dismissed (dismiss only rotates crash.txt to
        # crash_1.txt; it does not survive the .old round-trip). crash_history.json
        # is intentionally NOT pruned — it is the dedup store and should persist.
        $(file_sudo "${INSTALL_BACKUP}/config") rm -f \
            "${INSTALL_BACKUP}/config/crash.txt" \
            "${INSTALL_BACKUP}/config/"crash_*.txt \
            "${INSTALL_BACKUP}/config/crash_report.txt" \
            "${INSTALL_BACKUP}/config/.crash_restart_count" 2>/dev/null || true
    fi

    # Restore any remaining user data from previous config/ (custom_images/,
    # printer_database.d/, user themes, etc.).  Only copies items that don't
    # already exist in the new install so bundled files are never overwritten.
    # Uses [ ! -e ] instead of cp -n for BusyBox compatibility.
    # For directories that exist in both old and new installs (e.g. printer_database.d/),
    # merge at the file level so user additions are preserved alongside new bundled files.
    if [ -n "${INSTALL_BACKUP:-}" ] && [ -d "${INSTALL_BACKUP}/config" ]; then
        for _item in "${INSTALL_BACKUP}/config"/*; do
            [ -e "$_item" ] || continue
            _base=$(basename "$_item")
            if [ ! -e "${INSTALL_DIR}/config/${_base}" ]; then
                # Item doesn't exist in new install — restore the whole thing
                if $(file_sudo "${INSTALL_DIR}/config") cp -r "$_item" "${INSTALL_DIR}/config/${_base}" 2>/dev/null; then
                    log_info "Restored user data: config/${_base}"
                else
                    log_warn "Failed to restore user data: config/${_base}"
                fi
            elif [ -d "$_item" ] && [ -d "${INSTALL_DIR}/config/${_base}" ]; then
                # Both old and new have this directory — merge individual files
                for _subitem in "$_item"/*; do
                    [ -e "$_subitem" ] || continue
                    _subbase=$(basename "$_subitem")
                    if [ ! -e "${INSTALL_DIR}/config/${_base}/${_subbase}" ]; then
                        if $(file_sudo "${INSTALL_DIR}/config/${_base}") cp -r "$_subitem" "${INSTALL_DIR}/config/${_base}/${_subbase}" 2>/dev/null; then
                            log_info "Restored user data: config/${_base}/${_subbase}"
                        else
                            log_warn "Failed to restore user data: config/${_base}/${_subbase}"
                        fi
                    fi
                done
            fi
        done
    fi

    # Cleanup — cd out first since we cd'd into extract_dir earlier;
    # removing the CWD causes "getcwd: cannot access parent directories"
    # errors in subsequent commands (#703)
    cd / 2>/dev/null || true
    rm -rf "$extract_dir"

    # Remove legacy files that were shipped in older releases but are no longer needed.
    # Source fonts (.ttf/.otf) are only used by font regen scripts during development,
    # not at runtime. Removes ~35 MB of dead weight on embedded devices.
    local _legacy_removed=0
    for _ext in ttf otf; do
        for _f in "${INSTALL_DIR}"/assets/fonts/*."${_ext}"; do
            [ -f "$_f" ] || continue
            $(file_sudo "${INSTALL_DIR}") rm -f "$_f" 2>/dev/null && _legacy_removed=$((_legacy_removed + 1))
        done
    done
    $(file_sudo "${INSTALL_DIR}") rm -f "${INSTALL_DIR}/assets/fonts/.clang-format" 2>/dev/null && _legacy_removed=$((_legacy_removed + 1))
    [ "$_legacy_removed" -gt 0 ] && log_info "Removed $_legacy_removed legacy dev-only files from assets/fonts"

    log_success "Extracted to ${INSTALL_DIR}"
}

# Remove backup of previous installation (call after service starts successfully)
# Reclaim directories a previous version left on the wrong filesystem.
#
# Two kinds, both measured on a K2 whose root overlay is ~240MB while its user
# partition at /mnt/UDISK is 27.5GB:
#   - caches: the app used to cache thumbnails and modified gcode under
#     /usr/data, i.e. on the overlay. It now caches on /mnt/UDISK.
#   - scratch dirs: before cleanup was armed on EXIT (it hung off a bash-only
#     ERR trap that never fired under ash/dash), an interrupted install left the
#     whole download behind. One unit held a 60MB archive for two months.
#
# Platforms declare what to reclaim in STALE_CACHE_DIRS; a no-op elsewhere.
#
# SAFETY: same guard shape as the off-partition rollback cleanup below — the
# final path component must be exactly "cache" or name itself an installer
# scratch dir, and never a top-level directory. A past incident wiped a K2's
# /mnt/UDISK mount root via an unguarded rm -rf.
cleanup_stale_cache_dirs() {
    [ -n "${STALE_CACHE_DIRS:-}" ] || return 0

    local _stale _kind
    for _stale in $STALE_CACHE_DIRS; do
        case "$_stale" in
            /*/*/cache)                 _kind="cache" ;;
            /*/*helixscreen-install*)   _kind="scratch" ;;
            *)
                log_warn "Refusing to remove unexpected reclaim path: $_stale"
                continue
                ;;
        esac

        # Never remove the scratch dir this run is actively staging into.
        if [ -n "${TMP_DIR:-}" ] && [ "$_stale" = "${TMP_DIR%/}" ]; then
            continue
        fi

        [ -d "$_stale" ] || continue

        rm -rf "$_stale" 2>/dev/null || $SUDO rm -rf "$_stale" 2>/dev/null || true
        log_info "Reclaimed stale ${_kind} directory: $_stale"

        # Drop the now-empty parent too, but only if nothing else lives there.
        rmdir "$(dirname "$_stale")" 2>/dev/null || true
    done
    return 0
}

cleanup_old_install() {
    # Keep .old as a last-resort recovery path if config wasn't restored.
    # Without this guard, a failed Phase 6 + cleanup = permanent config loss.
    if [ "$ORIGINAL_INSTALL_EXISTS" = true ] && [ ! -f "${INSTALL_DIR}/config/settings.json" ]; then
        log_warn "Config not restored — keeping .old backup for recovery"
        return 0
    fi

    # Clean both the standard .old and any timestamped fallback backups
    for _backup in "${INSTALL_DIR}.old" "${INSTALL_DIR}.old."*; do
        if [ -d "$_backup" ]; then
            rm -rf "$_backup" 2>/dev/null || $SUDO rm -rf "$_backup" 2>/dev/null || true
            log_info "Cleaned up previous installation backup: $_backup"
        fi
    done

    # Clean the off-partition rollback backup (option B; tight-space upgrades).
    # CRITICAL SAFETY GUARD: a past incident wiped a K2's /mnt/UDISK mount root
    # via an unguarded rm -rf. Only ever remove a path whose final component is
    # exactly "helixscreen-rollback" — never a bare mount root.
    if [ -n "${HELIX_OFFSITE_ROLLBACK_DIR:-}" ] && [ -d "$HELIX_OFFSITE_ROLLBACK_DIR" ]; then
        case "$HELIX_OFFSITE_ROLLBACK_DIR" in
            */helixscreen-rollback)
                rm -rf "$HELIX_OFFSITE_ROLLBACK_DIR" 2>/dev/null || $SUDO rm -rf "$HELIX_OFFSITE_ROLLBACK_DIR" 2>/dev/null || true
                log_info "Cleaned up off-partition rollback backup: $HELIX_OFFSITE_ROLLBACK_DIR" ;;
            *) log_warn "Refusing to remove unexpected rollback path: $HELIX_OFFSITE_ROLLBACK_DIR" ;;
        esac
    fi
}
