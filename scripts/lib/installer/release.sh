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

# Cached manifest from R2 (set by get_latest_version, consumed by download_release)
_R2_MANIFEST=""

# Fetch a URL to stdout using curl or wget
# Returns non-zero if neither is available or fetch fails
fetch_url() {
    local url=$1
    if command -v curl >/dev/null 2>&1; then
        curl -sSL --connect-timeout 10 "$url" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- --timeout=10 "$url" 2>/dev/null
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
    if command -v curl >/dev/null 2>&1; then
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
    else
        return 1
    fi
}

# Extract "version" value from manifest JSON on stdin
# Uses POSIX basic regex only (BusyBox compatible)
parse_manifest_version() {
    sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1
}

# Extract platform asset URL from manifest JSON on stdin
# Greps for the platform-specific filename pattern then extracts the URL
# Uses POSIX basic regex only (BusyBox compatible)
parse_manifest_platform_url() {
    local platform=$1
    grep "helixscreen-${platform}-" | \
        sed -n 's/.*"url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1
}

# Validate a tarball is a valid gzip archive and not truncated
# Args: tarball_path, context (e.g., "Downloaded" or "Local")
# Exits on failure
validate_tarball() {
    local tarball=$1
    local context=${2:-""}

    # Verify it's a valid gzip file
    if ! gunzip -t "$tarball" 2>/dev/null; then
        log_error "${context}file is not a valid gzip archive."
        [ -n "$context" ] && log_error "The ${context}may have been corrupted or incomplete."
        exit 1
    fi

    # Verify file isn't truncated (releases should be >1MB)
    local size_kb
    size_kb=$(du -k "$tarball" 2>/dev/null | cut -f1)
    if [ "${size_kb:-0}" -lt 1024 ]; then
        log_error "${context}file too small (${size_kb}KB). File may be incomplete."
        exit 1
    fi
}

# Check if we can download from HTTPS URLs
# BusyBox wget on AD5M doesn't support HTTPS
check_https_capability() {
    # curl with SSL support works
    if command -v curl >/dev/null 2>&1; then
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
    printf '%b\n' "  2. Download: ${BOLD}helixscreen-${platform}-${version}.tar.gz${NC}"
    printf '\n'
    printf '%b\n' "  3. Copy to this device (note: AD5M needs -O flag):"
    if [ "$platform" = "ad5m" ]; then
        # AD5M /tmp is a tiny tmpfs (~54MB), use /data/ instead
        printf '%b\n' "     ${CYAN}scp -O helixscreen-${platform}.tar.gz root@<this-ip>:/data/${NC}"
        printf '%b\n' "     ${YELLOW}Windows: use WSL, WinSCP (SCP mode), or PuTTY pscp${NC}"
        printf '\n'
        printf '%b\n' "  4. Run the installer with the local file:"
        printf '%b\n' "     ${CYAN}sh /data/install.sh --local /data/helixscreen-${platform}.tar.gz${NC}"
    else
        printf '%b\n' "     ${CYAN}scp helixscreen-${platform}.tar.gz root@<this-ip>:/tmp/${NC}"
        printf '\n'
        printf '%b\n' "  4. Run the installer with the local file:"
        printf '%b\n' "     ${CYAN}sh /tmp/install.sh --local /tmp/helixscreen-${platform}.tar.gz${NC}"
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

    # Check HTTPS capability first
    if ! check_https_capability; then
        show_manual_install_instructions "$platform" "latest"
    fi

    # Try R2 manifest first (faster CDN, no API rate limits)
    local manifest_url="${R2_BASE_URL}/${R2_CHANNEL}/manifest.json"
    log_info "Fetching latest version from CDN..."

    _R2_MANIFEST=$(fetch_url "$manifest_url") || true
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
    else
        log_warn "CDN unavailable, trying GitHub..."
    fi

    # Fallback: GitHub API
    local url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    log_info "Fetching latest version from GitHub..."

    if command -v curl >/dev/null 2>&1; then
        # Use basic sed regex (no -E flag) for BusyBox compatibility
        version=$(curl -sSL --connect-timeout 10 "$url" 2>/dev/null | grep '"tag_name"' | sed 's/.*"\([^"][^"]*\)".*/\1/')
    elif command -v wget >/dev/null 2>&1; then
        # Use basic sed regex (no -E flag) for BusyBox compatibility
        version=$(wget -qO- --timeout=10 "$url" 2>/dev/null | grep '"tag_name"' | sed 's/.*"\([^"][^"]*\)".*/\1/')
    fi

    if [ -z "$version" ]; then
        log_error "Failed to fetch latest version."
        log_error "Check your network connection and try again."
        log_error "Tried: $manifest_url"
        log_error "Tried: $url"
        exit 1
    fi

    echo "$version"
}

# Download release tarball (tries R2 CDN first, falls back to GitHub)
download_release() {
    local version=$1
    local platform=$2

    local filename="helixscreen-${platform}-${version}.tar.gz"
    local dest="${TMP_DIR}/helixscreen.tar.gz"

    mkdir -p "$TMP_DIR"
    CLEANUP_TMP=true

    # Try R2 CDN first
    local r2_url=""
    if [ -n "$_R2_MANIFEST" ]; then
        # Extract URL from cached manifest
        r2_url=$(echo "$_R2_MANIFEST" | parse_manifest_platform_url "$platform")
    fi
    # Fall back to constructed R2 URL if manifest didn't have it
    if [ -z "$r2_url" ]; then
        r2_url="${R2_BASE_URL}/${R2_CHANNEL}/${filename}"
    fi

    log_info "Downloading HelixScreen ${version} for ${platform}..."
    log_info "URL: $r2_url"

    if download_file "$r2_url" "$dest" 300 51200; then
        # Quick validation — make sure it's actually a gzip file
        if gunzip -t "$dest" 2>/dev/null; then
            local size
            size=$(ls -lh "$dest" | awk '{print $5}')
            log_success "Downloaded ${filename} (${size}) from CDN"
            return 0
        fi
        log_warn "CDN download incomplete or timed out, trying GitHub..."
        rm -f "$dest"
    else
        log_warn "CDN download failed, trying GitHub..."
        rm -f "$dest"
    fi

    # Fallback: GitHub Releases
    local gh_url="https://github.com/${GITHUB_REPO}/releases/download/${version}/${filename}"
    log_info "URL: $gh_url"

    if download_file "$gh_url" "$dest"; then
        validate_tarball "$dest" "Downloaded "
        local size
        size=$(ls -lh "$dest" | awk '{print $5}')
        log_success "Downloaded ${filename} (${size}) from GitHub"
        return 0
    fi

    log_error "Failed to download release."
    log_error "Tried: $r2_url"
    log_error "Tried: $gh_url"
    if [ -n "$_DOWNLOAD_HTTP_CODE" ] && [ "$_DOWNLOAD_HTTP_CODE" != "200" ]; then
        log_error "HTTP status: $_DOWNLOAD_HTTP_CODE"
    fi
    log_error ""
    log_error "Possible causes:"
    log_error "  - Version ${version} may not exist for platform ${platform}"
    log_error "  - Network connectivity issues"
    log_error "  - CDN and GitHub may be unavailable"
    log_error ""
    log_error "To install manually, download on another machine and use:"
    log_error "  ./install.sh --local /path/to/${filename}"
    exit 1
}

# Use a local tarball instead of downloading
use_local_tarball() {
    local src=$1

    log_info "Using local tarball: $src"

    validate_tarball "$src" "Local "

    # Point TMP_DIR tarball location to the source file directly
    # This avoids copying large files on space-constrained systems
    mkdir -p "$TMP_DIR"
    CLEANUP_TMP=true

    # Create symlink or use directly based on what the extraction expects
    # The extract_release function looks for ${TMP_DIR}/helixscreen.tar.gz
    local dest="${TMP_DIR}/helixscreen.tar.gz"
    if [ "$src" != "$dest" ]; then
        # Resolve to absolute path so a symlink created in $TMP_DIR doesn't
        # become dangling if the user passed a relative path.
        # (BusyBox readlink may not support -f, so try realpath first.)
        local abs_src
        abs_src=$(realpath "$src" 2>/dev/null)
        [ -n "$abs_src" ] || abs_src=$(readlink -f "$src" 2>/dev/null)
        [ -n "$abs_src" ] || abs_src="$src"

        # Prefer a symlink to avoid copying large files on constrained devices,
        # but *verify* the staged tarball is readable. Fall back to copying.
        if ln -sf "$abs_src" "$dest" 2>/dev/null && [ -r "$dest" ]; then
            : # symlink OK
        elif cp "$abs_src" "$dest" 2>/dev/null && [ -r "$dest" ]; then
            : # copy OK
        else
            log_error "Failed to stage tarball at $dest"
            log_error "Source: $abs_src"
            log_error "Check that the source file exists and the temp directory is writable."
            exit 1
        fi
    fi

    local size
    size=$(ls -lh "$src" | awk '{print $5}')
    log_success "Using local tarball (${size})"
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

    # Read first 20 bytes of ELF header as space-separated hex
    # Try hexdump first (BusyBox), fall back to od (POSIX), then xxd
    local header
    header=$(dd if="$binary" bs=1 count=20 2>/dev/null | hexdump -v -e '1/1 "%02x "' 2>/dev/null) || true
    if [ -z "$header" ]; then
        header=$(dd if="$binary" bs=1 count=20 2>/dev/null | od -A n -t x1 -v 2>/dev/null | tr '\n' ' ' | tr -s ' ' | sed 's/^ //;s/ $//') || true
    fi
    if [ -z "$header" ]; then
        header=$(dd if="$binary" bs=1 count=20 2>/dev/null | xxd -p 2>/dev/null | sed 's/../& /g;s/ $//') || true
    fi

    if [ -z "$header" ]; then
        log_error "Cannot read binary header (file may be empty or corrupted)"
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

    local machine_lo machine_hi
    machine_lo=$(echo "$header" | awk '{print $19}')
    machine_hi=$(echo "$header" | awk '{print $20}')

    # Determine expected values based on platform
    local expected_class expected_machine_lo expected_desc
    case "$platform" in
        ad5m|pi32)
            expected_class="01"
            expected_machine_lo="28"
            expected_desc="ARM 32-bit (armv7l)"
            ;;
        ad5x|k1)
            expected_class="01"
            expected_machine_lo="08"
            expected_desc="MIPS 32-bit (mipsel)"
            ;;
        pi)
            expected_class="02"
            expected_machine_lo="b7"
            expected_desc="AARCH64 64-bit"
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

# Extract tarball with atomic swap and rollback protection
extract_release() {
    local platform=$1
    local tarball="${TMP_DIR}/helixscreen.tar.gz"
    local extract_dir="${TMP_DIR}/extract"
    local new_install="${extract_dir}/helixscreen"

    # Pre-flight: check TMP_DIR has enough space for extraction
    # Tarball expands ~3x, so require 3x tarball size + margin
    local tarball_mb extract_required_mb tmp_available_mb
    tarball_mb=$(du -m "$tarball" 2>/dev/null | awk '{print $1}')
    [ -z "$tarball_mb" ] && tarball_mb=$(ls -l "$tarball" | awk '{print int($5/1048576)}')
    extract_required_mb=$(( (tarball_mb * 3) + 20 ))

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

    log_info "Extracting release..."

    # Phase 1: Extract to temporary directory
    mkdir -p "$extract_dir"
    cd "$extract_dir" || exit 1

    if [ "$platform" = "ad5m" ] || [ "$platform" = "ad5x" ] || [ "$platform" = "k1" ]; then
        # BusyBox tar doesn't support -z
        if ! gunzip -c "$tarball" | tar xf -; then
            # Check if it was a space issue vs actual corruption
            local post_mb
            post_mb=$(df "$tmp_check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
            if [ -n "$post_mb" ] && [ "$post_mb" -lt 5 ]; then
                log_error "Failed to extract tarball: no space left on device."
                log_error "Filesystem $(df "$tmp_check_dir" | tail -1 | awk '{print $1}') is full."
                log_error "Try: TMP_DIR=/path/with/space sh install.sh ..."
            else
                log_error "Failed to extract tarball."
                log_error "The archive may be corrupted. Try re-downloading."
            fi
            rm -rf "$extract_dir"
            exit 1
        fi
    else
        if ! tar -xzf "$tarball"; then
            local post_mb
            post_mb=$(df "$tmp_check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
            if [ -n "$post_mb" ] && [ "$post_mb" -lt 5 ]; then
                log_error "Failed to extract tarball: no space left on device."
                log_error "Filesystem $(df "$tmp_check_dir" | tail -1 | awk '{print $1}') is full."
                log_error "Try: TMP_DIR=/path/with/space sh install.sh ..."
            else
                log_error "Failed to extract tarball."
                log_error "The archive may be corrupted. Try re-downloading."
            fi
            rm -rf "$extract_dir"
            exit 1
        fi
    fi

    # Phase 2: Validate extracted content
    if [ ! -f "${new_install}/bin/helix-screen" ]; then
        log_error "Extraction failed - helix-screen binary not found."
        log_error "Expected: helixscreen/bin/helix-screen in tarball"
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

        # Backup config (check new location first, then legacy)
        if [ -f "${INSTALL_DIR}/config/helixconfig.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/helixconfig.json.backup"
            cp "${INSTALL_DIR}/config/helixconfig.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (from config/)"
        elif [ -f "${INSTALL_DIR}/helixconfig.json" ]; then
            BACKUP_CONFIG="${TMP_DIR}/helixconfig.json.backup"
            cp "${INSTALL_DIR}/helixconfig.json" "$BACKUP_CONFIG"
            log_info "Backed up existing configuration (legacy location)"
        fi

        # Backup helixscreen.env (preserves HELIX_LOG_LEVEL and other env customizations)
        if [ -f "${INSTALL_DIR}/config/helixscreen.env" ]; then
            BACKUP_ENV="${TMP_DIR}/helixscreen.env.backup"
            cp "${INSTALL_DIR}/config/helixscreen.env" "$BACKUP_ENV"
            log_info "Backed up existing helixscreen.env"
        fi

        # Choose backup dir name for atomic swap.
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
        if ! $(file_sudo "${INSTALL_DIR}") mv "${INSTALL_DIR}" "$INSTALL_BACKUP"; then
            log_error "Failed to backup existing installation."
            rm -rf "$extract_dir"
            exit 1
        fi
    fi

    # Phase 5: Move new install into place
    $(file_sudo "$(dirname "${INSTALL_DIR}")") mkdir -p "$(dirname "${INSTALL_DIR}")"
    if ! $(file_sudo "$(dirname "${INSTALL_DIR}")") mv "${new_install}" "${INSTALL_DIR}"; then
        log_error "Failed to install new release."
        # ROLLBACK: restore old installation
        if [ -d "${INSTALL_BACKUP:-}" ]; then
            log_warn "Rolling back to previous installation..."
            # Remove partial new install that may block the rollback mv
            [ -d "${INSTALL_DIR}" ] && $SUDO rm -rf "${INSTALL_DIR}"
            if $SUDO mv "$INSTALL_BACKUP" "${INSTALL_DIR}"; then
                log_warn "Rollback complete. Previous installation restored."
            else
                log_error "CRITICAL: Rollback failed! Previous install at $INSTALL_BACKUP"
                log_error "Manually restore with: mv $INSTALL_BACKUP ${INSTALL_DIR}"
            fi
        fi
        rm -rf "$extract_dir"
        exit 1
    fi

    # Phase 6: Restore config and settings
    # Try TMP_DIR backup first; fall back to the .old directory's copy.
    # Under systemd's PrivateTmp=true, TMP_DIR lives in a volatile mount that
    # can disappear if the service restarts.  The .old directory is on the real
    # filesystem and survives any restart.
    $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config" 2>/dev/null || true
    if [ ! -f "${INSTALL_DIR}/config/helixconfig.json" ]; then
        if [ -n "${BACKUP_CONFIG:-}" ] && [ -f "$BACKUP_CONFIG" ]; then
            $(file_sudo "${INSTALL_DIR}/config") cp "$BACKUP_CONFIG" "${INSTALL_DIR}/config/helixconfig.json" 2>/dev/null && \
                log_info "Restored configuration from backup" || \
                log_warn "Failed to restore configuration from TMP_DIR backup"
        fi
    fi
    # Fallback: restore from .old directory if TMP_DIR backup was lost or failed
    if [ ! -f "${INSTALL_DIR}/config/helixconfig.json" ] && [ -n "${INSTALL_BACKUP:-}" ]; then
        if [ -f "${INSTALL_BACKUP}/config/helixconfig.json" ]; then
            $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/config/helixconfig.json" "${INSTALL_DIR}/config/helixconfig.json" 2>/dev/null && \
                log_info "Restored configuration from previous install backup" || \
                log_warn "Failed to restore configuration from .old backup"
        elif [ -f "${INSTALL_BACKUP}/helixconfig.json" ]; then
            $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/helixconfig.json" "${INSTALL_DIR}/config/helixconfig.json" 2>/dev/null && \
                log_info "Restored configuration from previous install backup (legacy location)" || \
                log_warn "Failed to restore configuration from .old backup"
        fi
    fi
    if [ ! -f "${INSTALL_DIR}/config/helixconfig.json" ] && [ "$ORIGINAL_INSTALL_EXISTS" = true ]; then
        log_warn "Could not restore helixconfig.json from any backup source!"
        log_warn "User configuration may have been lost."
    fi

    # Restore helixscreen.env
    if [ ! -f "${INSTALL_DIR}/config/helixscreen.env" ]; then
        if [ -n "${BACKUP_ENV:-}" ] && [ -f "$BACKUP_ENV" ]; then
            $(file_sudo "${INSTALL_DIR}/config") cp "$BACKUP_ENV" "${INSTALL_DIR}/config/helixscreen.env" 2>/dev/null && \
                log_info "Restored helixscreen.env from backup" || \
                log_warn "Failed to restore helixscreen.env from TMP_DIR backup"
        fi
    fi
    if [ ! -f "${INSTALL_DIR}/config/helixscreen.env" ] && [ -n "${INSTALL_BACKUP:-}" ]; then
        if [ -f "${INSTALL_BACKUP}/config/helixscreen.env" ]; then
            $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/config/helixscreen.env" "${INSTALL_DIR}/config/helixscreen.env" 2>/dev/null && \
                log_info "Restored helixscreen.env from previous install backup" || \
                log_warn "Failed to restore helixscreen.env from .old backup"
        fi
    fi

    # Restore any remaining user data from previous config/ (custom_images/,
    # printer_database.d/, etc.).  Only copies items that don't already exist in
    # the new install so bundled files are never overwritten.
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

    # Cleanup
    rm -rf "$extract_dir"
    log_success "Extracted to ${INSTALL_DIR}"
}

# Remove backup of previous installation (call after service starts successfully)
cleanup_old_install() {
    # Clean both the standard .old and any timestamped fallback backups
    for _backup in "${INSTALL_DIR}.old" "${INSTALL_DIR}.old."*; do
        if [ -d "$_backup" ]; then
            rm -rf "$_backup" 2>/dev/null || $SUDO rm -rf "$_backup" 2>/dev/null || true
            log_info "Cleaned up previous installation backup: $_backup"
        fi
    done
}
