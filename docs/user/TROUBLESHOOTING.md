# Troubleshooting

Find the symptom you see, then open the page for it. Each page is organized by what you see on the screen, not by cause.

**Start here for any problem:** tap the **Notifications** widget on the Home screen, then **Printer Events**. It lists Klipper shutdowns with their reason, lost MCU connections, pauses and printer errors. If that does not explain it, [turn on debug logging and send a debug bundle](troubleshooting/diagnostics.md#turning-on-debug-logging) from **Settings > Help & About > Upload Debug Bundle**.

## Pick your symptom

| What you see | Page |
|--------------|------|
| "Cannot connect", **Login Required**, disconnects during a print, WiFi will not join or lists no networks, a recovery dialog after a Klipper error | [Connection and network problems](troubleshooting/connection.md) |
| Black or rotated screen, wrong size or colors, brightness does nothing, touch does not respond or lands in the wrong place, a Home widget vanished | [Screen and touch problems](troubleshooting/display-touch.md) |
| Start Print does nothing, **Preparing Print** drags on, wrong layer count or time remaining, a power cut, a calibration that fails | [Printing and calibration problems](troubleshooting/printing.md) |
| No slots on the filament panel, load or unload fails, OrcaSlicer or Spoolman shows the wrong spools | [Filament system and Spoolman problems](troubleshooting/filament-systems.md) |
| Settings do not save, the wizard keeps returning, wrong printer model, a feature is missing, you want to start over | [Settings, setup wizard and printer model problems](troubleshooting/settings-config.md) |
| The service does not start after an install or update, or your printer is a Snapmaker U1, Adventurer 5M or 5X, or Creality K1 | [Install, update and printer-specific problems](troubleshooting/updates-install.md) |
| The interface is slow or the printer runs out of memory | [Slow interface and memory problems](troubleshooting/performance.md) |
| You need logs, a debug bundle, the restart command for your printer, or want to report a problem | [Logs, debug bundles and getting help](troubleshooting/diagnostics.md) |

## Sections that moved

Older links and bookmarks land on this page. Each entry below points to where the section lives now.

<a id="creality-print-can-no-longer-find-or-connect-to-the-printer"></a>
- Creality Print can no longer find or connect to the printer: [updates-install](troubleshooting/updates-install.md#creality-k1-creality-print-can-no-longer-find-or-connect-to-the-printer)

<a id="layer-count-is-wrong-stuck-at-0-or-total-layers-missing"></a>
- Layer count is wrong, stuck at 0, or total layers missing: [printing](troubleshooting/printing.md#the-layer-count-is-wrong-stuck-at-0-or-the-total-is-missing)

<a id="reset-helixscreen-or-re-run-the-setup-wizard"></a>
- Reset HelixScreen or re-run the setup wizard: [settings-config](troubleshooting/settings-config.md#reset-helixscreen-or-re-run-the-setup-wizard)

<a id="orcaslicer-doesnt-pick-up-your-slot-colors-or-materials"></a>
- OrcaSlicer doesn't pick up your slot colors or materials: [filament-systems](troubleshooting/filament-systems.md#orcaslicer-does-not-pick-up-your-slot-colors-or-materials)

<a id="when-the-printer-errors-or-disconnects-recovery-dialog"></a>
- When the Printer Errors or Disconnects (Recovery Dialog): [connection](troubleshooting/connection.md#a-recovery-dialog-covers-the-screen-printer-error-shutdown-or-disconnect)

<a id="a-feature-is-missing-for-hardware-my-printer-has"></a>
- A feature is missing for hardware my printer has: [settings-config](troubleshooting/settings-config.md#a-feature-is-missing-for-hardware-my-printer-has)

<a id="screen-dims-after-a-few-seconds"></a>
- Screen dims after a few seconds (Adventurer 5M): [updates-install](troubleshooting/updates-install.md#adventurer-5m-the-screen-dims-a-few-seconds-after-boot)

<a id="flashforge-adventurer-5m-issues"></a>
- Flashforge Adventurer 5M issues: [updates-install](troubleshooting/updates-install.md#adventurer-5m-the-screen-dims-a-few-seconds-after-boot)

<a id="random-solid-colors-during-screen-sleep-ad5x"></a>
- Random solid colors during screen sleep (AD5X): [display-touch](troubleshooting/display-touch.md#solid-red-green-or-blue-fills-while-the-screen-sleeps-ad5x)

<a id="update-failed-in-mainsail--screen-wont-start-after-update-ad5x"></a>
- Update failed in Mainsail, screen won't start after update (AD5X): [updates-install](troubleshooting/updates-install.md#ad5x-the-screen-will-not-start-after-an-update-failed-in-mainsail)

<a id="binary-wont-start-glibc-version-not-found"></a>
- Binary won't start (GLIBC version not found): [updates-install](troubleshooting/updates-install.md#the-service-never-starts-glibc-version-not-found)

<a id="display-upside-down-or-rotated"></a>
- Display upside down or rotated: [display-touch](troubleshooting/display-touch.md#the-picture-is-upside-down-or-rotated)

<a id="accidental-button-presses-after-scrolling"></a>
- Accidental button presses after scrolling: [display-touch](troubleshooting/display-touch.md#a-button-fires-when-i-lift-my-finger-after-scrolling)

<a id="screen-goes-dark-at-sleep-but-the-backlight-stays-on"></a>
- Screen goes dark at sleep but the backlight stays on: [display-touch](troubleshooting/display-touch.md#the-screen-goes-dark-at-sleep-but-the-backlight-stays-on)

<a id="touch-feel--which-setting-do-i-tune"></a>
- Touch feel: which setting do I tune?: [display-touch](troubleshooting/display-touch.md#touch-feel-which-setting-do-i-tune)

<a id="unauthorized-everywhere-moonraker-requires-a-login"></a>
- "Unauthorized" everywhere: Moonraker requires a login: [connection](troubleshooting/connection.md#login-required-or-unauthorized-everywhere)

<a id="wrong-printer-model-identified"></a>
- Wrong printer model identified: [settings-config](troubleshooting/settings-config.md#helixscreen-shows-the-wrong-printer-model)

<a id="brightness-slider-or-screen-dimming-does-nothing"></a>
- Brightness slider or screen dimming does nothing: [display-touch](troubleshooting/display-touch.md#the-brightness-slider-or-screen-dimming-does-nothing)

<a id="ultrawide-or-portrait-screen-looks-stretched-cramped-or-clipped"></a>
- Ultrawide or portrait screen looks stretched, cramped, or clipped: [display-touch](troubleshooting/display-touch.md#an-ultrawide-or-portrait-screen-looks-stretched-cramped-or-clipped)

<a id="a-widget-disappeared-from-the-home-screen"></a>
- A widget disappeared from the home screen: [display-touch](troubleshooting/display-touch.md#a-widget-disappeared-from-the-home-screen)

<a id="snapmaker-u1-blank-screen-and-off-the-network-after-a-reboot"></a>
- Snapmaker U1: blank screen and off the network after a reboot: [updates-install](troubleshooting/updates-install.md#snapmaker-u1-blank-screen-and-off-the-network-after-a-reboot)

<a id="cfs-shows-no-slots-creality-k2"></a>
- CFS shows no slots (Creality K2): [filament-systems](troubleshooting/filament-systems.md#the-cfs-shows-no-slots-creality-k2)

*Back to: [User Guide](USER_GUIDE.md) | [Installation](INSTALL.md)*
