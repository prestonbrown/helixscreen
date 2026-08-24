#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixPrint Moonraker Plugin Installer
#
# This script installs the helix_print Moonraker plugin.
#
# Usage:
#   ./install.sh              # Interactive install (manual config steps)
#   ./install.sh --auto       # Full auto-install (updates config, restarts Moonraker)
#   ./install.sh --uninstall  # Remove the plugin
#
# Remote install (from GitHub):
#   curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/moonraker-plugin/remote-install.sh | sh

set -e

# Get script directory (POSIX-compatible)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_FILE="$SCRIPT_DIR/helix_print.py"
# Legacy name, kept only so --uninstall can clean up installs that predate
# 0d5bc370d (which merged helix_phase_tracking.cfg into helix_macros.cfg)
LEGACY_PHASE_TRACKING_CFG="helix_phase_tracking.cfg"
AUTO_MODE=false
ENABLE_PHASE_TRACKING=false

# Colors for output (works with printf)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() {
    printf "${GREEN}[INFO]${NC} %s\n" "$1"
}

warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$1"
}

error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1"
    exit 1
}

# Find Moonraker installation
find_moonraker() {
    moonraker_path=""
    user_path="$1"

    # Check common locations (iterate without arrays)
    for loc in \
        "$HOME/moonraker" \
        "$HOME/klipper_config/moonraker" \
        "/home/pi/moonraker" \
        "/home/klipper/moonraker" \
        "/root/printer_software/moonraker" \
        "$user_path"
    do
        if [ -n "$loc" ] && [ -d "$loc/moonraker/components" ]; then
            moonraker_path="$loc"
            break
        fi
    done

    # Also check if moonraker is installed as a package
    if [ -z "$moonraker_path" ]; then
        pip_loc=$(python3 -c "import moonraker; print(moonraker.__path__[0])" 2>/dev/null || true)
        if [ -n "$pip_loc" ] && [ -d "$pip_loc/components" ]; then
            moonraker_path="$(dirname "$pip_loc")"
        fi
    fi

    printf '%s' "$moonraker_path"
}

# Find config directory
find_config_dir() {
    for loc in \
        "$HOME/printer_data/config" \
        "$HOME/klipper_config" \
        "/home/pi/printer_data/config" \
        "/home/pi/klipper_config" \
        "/root/printer_data/config"
    do
        if [ -d "$loc" ] && [ -f "$loc/moonraker.conf" ]; then
            printf '%s' "$loc"
            return 0
        fi
    done
    printf ''
}

# Main installation
main() {
    info "HelixPrint Moonraker Plugin Installer"
    printf '\n'

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker
    moonraker_path=$(find_moonraker "$1")

    if [ -z "$moonraker_path" ]; then
        error "Could not find Moonraker installation.
Please provide the path: ./install.sh /path/to/moonraker"
    fi

    components_dir="$moonraker_path/moonraker/components"

    if [ ! -d "$components_dir" ]; then
        error "Components directory not found: $components_dir"
    fi

    info "Found Moonraker at: $moonraker_path"
    info "Components directory: $components_dir"
    printf '\n'

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target
Please remove it manually before installing."
    fi

    # Use ln -sf for atomic replacement (removes existing symlink first)
    ln -sf "$PLUGIN_FILE" "$target"
    info "Created symlink: $target -> $PLUGIN_FILE"
    printf '\n'

    # Remind about configuration
    info "Installation complete!"
    printf '\n'
    printf '%s\n' "Next steps:"
    printf '%s\n' "  1. Add the following to your moonraker.conf:"
    printf '\n'
    printf '%s\n' "     [helix_print]"
    printf '%s\n' "     # enabled: True"
    printf '%s\n' "     # temp_dir: .helix_temp"
    printf '%s\n' "     # symlink_dir: .helix_print"
    printf '%s\n' "     # cleanup_delay: 86400"
    printf '\n'
    printf '%s\n' "  2. Restart Moonraker:"
    printf '%s\n' "     sudo systemctl restart moonraker"
    printf '\n'
    printf '%s\n' "  3. Verify the plugin is loaded:"
    printf '%s\n' "     curl http://localhost:7125/server/helix/status"
    printf '\n'
}

# Uninstall function (interactive)
uninstall() {
    info "Uninstalling HelixPrint plugin..."

    moonraker_path=$(find_moonraker "$1")

    if [ -z "$moonraker_path" ]; then
        error "Could not find Moonraker installation."
    fi

    target="$moonraker_path/moonraker/components/helix_print.py"

    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        warn "Found regular file (not symlink) at $target"
        warn "Please remove it manually if desired."
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    printf '\n'
    printf '%s\n' "Don't forget to:"
    printf '%s\n' "  1. Remove [helix_print] section from moonraker.conf"
    printf '%s\n' "  2. Restart Moonraker: sudo systemctl restart moonraker"
}

# Wait for Moonraker to become available after restart
wait_for_moonraker() {
    max_attempts=30
    attempt=0
    moonraker_url="${MOONRAKER_URL:-http://localhost:7125}"

    info "Waiting for Moonraker to become available..."

    while [ "$attempt" -lt "$max_attempts" ]; do
        if curl -s --max-time 2 "$moonraker_url/server/info" > /dev/null 2>&1; then
            info "Moonraker is ready!"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done

    warn "Moonraker did not respond within ${max_attempts} seconds"
    return 1
}

# Locate the shipped helix_macros.cfg
#
# It defines the HELIX_PHASE_* and HELIX_READY macros that the plugin injects
# into PRINT_START. The sibling layout holds in both a git checkout and a
# deployed HelixScreen - mk/cross.mk ships assets/ and moonraker-plugin/ side by
# side (DEPLOY_ASSET_DIRS).
find_helix_macros_cfg() {
    for candidate in \
        "$SCRIPT_DIR/../assets/config/helix_macros.cfg" \
        "$SCRIPT_DIR/../config/helix_macros.cfg" \
        "/opt/helixscreen/config/helix_macros.cfg"
    do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

# Install phase tracking macros and optionally instrument PRINT_START
install_phase_tracking() {
    config_dir="$1"
    moonraker_url="${MOONRAKER_URL:-http://localhost:7125}"

    if [ -z "$config_dir" ]; then
        warn "Config directory not found - skipping phase tracking setup"
        return 1
    fi

    info "Setting up detailed print preparation tracking..."

    # Install helix_macros.cfg BEFORE instrumenting. Instrumenting first would
    # leave PRINT_START calling macros that do not exist, which Klipper does not
    # catch at config load - it fails at print start with "Unknown command".
    if ! macros_cfg=$(find_helix_macros_cfg); then
        warn "helix_macros.cfg not found alongside the plugin"
        warn "Skipping phase tracking - PRINT_START will be left untouched"
        return 1
    fi

    cp "$macros_cfg" "$config_dir/helix_macros.cfg"
    info "Installed helix_macros.cfg"

    # Add include to printer.cfg if not already present
    printer_cfg="$config_dir/printer.cfg"
    if [ -f "$printer_cfg" ]; then
        if ! grep -q '\[include helix_macros.cfg\]' "$printer_cfg"; then
            # Create backup
            backup_file="${printer_cfg}.bak.$(date +%Y%m%d_%H%M%S)"
            cp "$printer_cfg" "$backup_file"
            info "Created backup: $backup_file"

            # Append include at end of file (safe, simple approach)
            printf '\n' >> "$printer_cfg"
            printf '%s\n' "[include helix_macros.cfg]" >> "$printer_cfg"

            info "Added [include helix_macros.cfg] to printer.cfg"
        else
            info "Helix macros include already present in printer.cfg"
        fi
    else
        warn "printer.cfg not found - please add [include helix_macros.cfg] manually"
        return 1
    fi

    # Call plugin API to instrument PRINT_START macro
    info "Instrumenting PRINT_START macro..."
    response=$(curl -s -X POST "$moonraker_url/server/helix/phase_tracking/enable" 2>/dev/null || printf '%s' '{"error": "API call failed"}')

    if printf '%s' "$response" | grep -q '"success".*true'; then
        info "PRINT_START macro instrumented successfully"
    else
        warn "Could not auto-instrument PRINT_START macro"
        warn "You can enable this later from HelixScreen Settings"
        warn "Or manually add HELIX_PRINT_COMPLETE to the end of your PRINT_START macro"
    fi

    return 0
}

# Remove phase tracking from printer config
remove_phase_tracking() {
    config_dir="$1"
    moonraker_url="${MOONRAKER_URL:-http://localhost:7125}"

    if [ -z "$config_dir" ]; then
        return 0
    fi

    info "Removing phase tracking..."

    # Call plugin API to strip instrumentation first (while Moonraker is still running)
    curl -s -X POST "$moonraker_url/server/helix/phase_tracking/disable" 2>/dev/null || true

    # helix_macros.cfg and its include are deliberately left in place: it is the
    # shared HelixScreen helper macro file (HELIX_START_PRINT, HELIX_CLEAN_NOZZLE
    # and friends), not a phase-tracking artifact. Removing it here would break
    # features the user never uninstalled. Stripping the instrumentation above is
    # what actually disables phase tracking.

    # Clean up the legacy pre-0d5bc370d file, which nothing uses any more
    printer_cfg="$config_dir/printer.cfg"
    if [ -f "$printer_cfg" ] && grep -q "\[include $LEGACY_PHASE_TRACKING_CFG\]" "$printer_cfg"; then
        backup_file="${printer_cfg}.bak.$(date +%Y%m%d_%H%M%S)"
        cp "$printer_cfg" "$backup_file"
        grep -v "\[include $LEGACY_PHASE_TRACKING_CFG\]" "$printer_cfg" > "$printer_cfg.tmp"
        mv "$printer_cfg.tmp" "$printer_cfg"
        info "Removed legacy phase tracking include from printer.cfg"
    fi

    if [ -f "$config_dir/$LEGACY_PHASE_TRACKING_CFG" ]; then
        rm "$config_dir/$LEGACY_PHASE_TRACKING_CFG"
        info "Removed legacy $LEGACY_PHASE_TRACKING_CFG"
    fi
}

# Restart Moonraker service
restart_moonraker() {
    info "Restarting Moonraker..."
    if command -v systemctl > /dev/null 2>&1 && systemctl list-units --type=service 2>/dev/null | grep -q moonraker; then
        sudo systemctl restart moonraker || warn "Failed to restart Moonraker via systemctl"
    elif command -v service > /dev/null 2>&1; then
        sudo service moonraker restart || warn "Failed to restart Moonraker via service"
    else
        warn "Neither systemctl nor service found - please restart Moonraker manually"
    fi
}

# Auto-uninstall function (non-interactive, for HelixScreen integration)
auto_uninstall() {
    info "HelixPrint Auto-Uninstall Mode"
    printf '\n'

    # Find Moonraker (auto-detect only)
    moonraker_path=$(find_moonraker "")

    if [ -z "$moonraker_path" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    target="$moonraker_path/moonraker/components/helix_print.py"

    # Find config directory
    config_dir=$(find_config_dir)

    # Remove phase tracking BEFORE removing plugin (need API access)
    remove_phase_tracking "$config_dir"

    # Remove symlink
    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        error "Found regular file (not symlink) at $target - manual removal required"
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    # Remove config section if possible
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        moonraker_conf="$config_dir/moonraker.conf"

        if grep -q '^\[helix_print\]' "$moonraker_conf"; then
            # Create backup before modifying config
            backup_file="${moonraker_conf}.bak.$(date +%Y%m%d_%H%M%S)"
            cp "$moonraker_conf" "$backup_file"
            info "Created backup: $backup_file"

            info "Removing [helix_print] section from moonraker.conf"
            # Use awk for cross-platform config section removal
            # This correctly handles helix_print as the last section in the file
            awk '
                /^\[helix_print\]/ { skip = 1; next }
                /^\[/ { skip = 0 }
                !skip { print }
            ' "$moonraker_conf" > "$moonraker_conf.tmp" && mv "$moonraker_conf.tmp" "$moonraker_conf"
        fi
    fi

    # Restart Moonraker
    restart_moonraker

    # Wait for Moonraker to come back up
    wait_for_moonraker

    printf '\n'
    info "Auto-uninstall complete!"
}

# Auto-install function (non-interactive, for HelixScreen integration)
auto_install() {
    info "HelixPrint Auto-Install Mode"
    printf '\n'

    AUTO_MODE=true

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker (auto-detect only)
    moonraker_path=$(find_moonraker "")

    if [ -z "$moonraker_path" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    components_dir="$moonraker_path/moonraker/components"

    # Find config directory
    config_dir=$(find_config_dir)

    info "Moonraker: $moonraker_path"
    info "Config: ${config_dir:-not found}"
    if [ "$ENABLE_PHASE_TRACKING" = "true" ]; then
        info "Phase tracking: ENABLED"
    fi
    printf '\n'

    # Pre-flight permission checks
    if [ ! -w "$components_dir" ]; then
        error "Cannot write to components directory: $components_dir"
    fi

    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        if [ ! -w "$config_dir/moonraker.conf" ]; then
            error "Cannot write to moonraker.conf: $config_dir/moonraker.conf"
        fi
    fi

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target"
    fi

    # Use ln -sf for atomic replacement (removes existing symlink first)
    ln -sf "$PLUGIN_FILE" "$target"
    info "Created symlink: $target"

    # Auto-configure moonraker.conf if possible
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        moonraker_conf="$config_dir/moonraker.conf"

        # Check if [helix_print] section already exists
        if grep -q '^\[helix_print\]' "$moonraker_conf"; then
            info "Config section [helix_print] already exists"
        else
            # Create backup before modifying config
            backup_file="${moonraker_conf}.bak.$(date +%Y%m%d_%H%M%S)"
            cp "$moonraker_conf" "$backup_file"
            info "Created backup: $backup_file"

            info "Adding [helix_print] section to moonraker.conf"
            printf '\n' >> "$moonraker_conf"
            printf '%s\n' "[helix_print]" >> "$moonraker_conf"
            printf '%s\n' "# HelixScreen plugin - auto-configured" >> "$moonraker_conf"
        fi
    else
        warn "Could not find moonraker.conf - manual config required"
    fi

    # Restart Moonraker
    restart_moonraker

    # Wait for Moonraker to come back up
    wait_for_moonraker

    # Install phase tracking if requested
    if [ "$ENABLE_PHASE_TRACKING" = "true" ]; then
        install_phase_tracking "$config_dir"

        # Need to restart Klipper to load the new macros
        info "Restarting Klipper to load phase tracking macros..."
        moonraker_url="${MOONRAKER_URL:-http://localhost:7125}"
        curl -s -X POST "$moonraker_url/printer/restart" 2>/dev/null || warn "Could not restart Klipper via API"

        # Give Klipper time to restart
        sleep 5
    fi

    printf '\n'
    info "Auto-install complete!"
}

show_help() {
    printf '%s\n' "Usage: $0 [OPTIONS] [MOONRAKER_PATH]"
    printf '\n'
    printf '%s\n' "Options:"
    printf '%s\n' "  --auto, -a              Full auto-install (updates config, restarts Moonraker)"
    printf '%s\n' "  --with-phase-tracking   Enable detailed print preparation tracking"
    printf '%s\n' "  --uninstall, -u         Remove the plugin symlink (interactive)"
    printf '%s\n' "  --uninstall-auto        Full auto-uninstall (removes config, restarts Moonraker)"
    printf '%s\n' "  --help, -h              Show this help message"
    printf '\n'
    printf '%s\n' "Arguments:"
    printf '%s\n' "  MOONRAKER_PATH     Path to Moonraker installation (auto-detected if not provided)"
    printf '\n'
    printf '%s\n' "Environment Variables:"
    printf '%s\n' "  MOONRAKER_URL      URL for Moonraker API health check after restart"
    printf '%s\n' "                     Default: http://localhost:7125"
    printf '%s\n' "                     Example: MOONRAKER_URL=http://192.168.1.100:7125 ./install.sh --auto"
    printf '\n'
    printf '%s\n' "Examples:"
    printf '%s\n' "  ./install.sh --auto                         # Auto-install without phase tracking"
    printf '%s\n' "  ./install.sh --auto --with-phase-tracking   # Auto-install with phase tracking"
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --auto|-a)
            AUTO_MODE=true
            shift
            ;;
        --with-phase-tracking)
            ENABLE_PHASE_TRACKING=true
            shift
            ;;
        --uninstall|-u)
            uninstall "$2"
            exit 0
            ;;
        --uninstall-auto)
            auto_uninstall
            exit 0
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            # Assume it's a moonraker path for interactive mode
            if [ "$AUTO_MODE" = "true" ]; then
                auto_install
            else
                main "$1"
            fi
            exit 0
            ;;
    esac
done

# No arguments left - run in appropriate mode
if [ "$AUTO_MODE" = "true" ]; then
    auto_install
else
    main ""
fi
