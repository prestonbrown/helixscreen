# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Optional

from core.logger import Logger
from extensions.base_extension import BaseExtension
from extensions.helixscreen import (
    HELIXSCREEN_INSTALLER_URL,
    HELIXSCREEN_SERVICE_NAME,
    find_install_dir as _find_install_dir,
)
from utils.input_utils import get_confirm


# noinspection PyMethodMayBeStatic
class HelixscreenExtension(BaseExtension):
    """KIAUH extension for HelixScreen touchscreen UI."""

    def install_extension(self, **kwargs) -> None:
        """Install HelixScreen using the bundled installer."""
        Logger.print_status("Installing HelixScreen...")

        install_dir = _find_install_dir()
        if install_dir:
            Logger.print_warn(
                f"HelixScreen appears to be already installed at {install_dir}"
            )
            if not get_confirm("Reinstall HelixScreen?", default_choice=False):
                return

        try:
            _run_installer()
            Logger.print_ok("HelixScreen installed successfully!")
            _print_post_install_info()
        except subprocess.CalledProcessError as e:
            Logger.print_error(f"Installation failed with exit code {e.returncode}")
            raise
        except Exception as e:
            Logger.print_error(f"Installation failed: {e}")
            raise

    def update_extension(self, **kwargs) -> None:
        """Update HelixScreen to the latest version."""
        Logger.print_status("Updating HelixScreen...")

        if not _find_install_dir():
            Logger.print_error(
                "HelixScreen is not installed. Please install it first."
            )
            return

        try:
            _run_installer(update=True)
            Logger.print_ok("HelixScreen updated successfully!")
        except subprocess.CalledProcessError as e:
            Logger.print_error(f"Update failed with exit code {e.returncode}")
            raise
        except Exception as e:
            Logger.print_error(f"Update failed: {e}")
            raise

    def remove_extension(self, **kwargs) -> None:
        """Remove HelixScreen."""
        Logger.print_status("Removing HelixScreen...")

        if not _find_install_dir():
            Logger.print_warn("HelixScreen does not appear to be installed.")
            return

        if not get_confirm(
            "This will remove HelixScreen and restore your previous screen UI. "
            "Continue?",
            default_choice=False,
        ):
            return

        try:
            _run_installer(uninstall=True)
            Logger.print_ok("HelixScreen removed successfully!")
            Logger.print_info(
                "Your previous screen UI has been restored."
            )
            Logger.print_info(
                "A reboot may be required for changes to take effect."
            )
        except subprocess.CalledProcessError as e:
            Logger.print_error(f"Removal failed with exit code {e.returncode}")
            raise
        except Exception as e:
            Logger.print_error(f"Removal failed: {e}")
            raise


def _run_installer(update: bool = False, uninstall: bool = False) -> None:
    """Download and run the HelixScreen bundled installer."""
    curl_cmd = ["curl", "-sSL", HELIXSCREEN_INSTALLER_URL]
    sh_cmd = ["sh"]

    if update:
        sh_cmd.extend(["-s", "--", "--update"])
    elif uninstall:
        sh_cmd.extend(["-s", "--", "--uninstall"])
    else:
        sh_cmd.extend(["-s", "--", "--source", "kiauh"])

    Logger.print_status("Downloading installer...")
    curl_result = subprocess.run(
        curl_cmd,
        capture_output=True,
        check=True,
    )

    mode = " (update)" if update else " (uninstall)" if uninstall else ""
    Logger.print_status(f"Running installer{mode}...")
    subprocess.run(
        sh_cmd,
        input=curl_result.stdout,
        check=True,
    )


def _print_post_install_info() -> None:
    """Print helpful information after installation."""
    Logger.print_info("")
    Logger.print_info("HelixScreen is now running on your touchscreen!")
    Logger.print_info("")
    Logger.print_info("Useful commands:")
    if Path("/run/systemd/system").exists():
        Logger.print_info(f"  systemctl status {HELIXSCREEN_SERVICE_NAME}")
        Logger.print_info(f"  journalctl -u {HELIXSCREEN_SERVICE_NAME} -f")
    else:
        Logger.print_info("  cat /tmp/helixscreen.log")
        Logger.print_info("  /etc/init.d/S*helixscreen status")
    Logger.print_info("")
    Logger.print_info("For more information, visit:")
    Logger.print_info("  https://github.com/prestonbrown/helixscreen")
