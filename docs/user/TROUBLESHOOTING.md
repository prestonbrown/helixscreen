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

## Moved sections

Links to the headings of the old single-page guide land here and point to the page that now holds the content.

<a id="quick-debugging-guide"></a>
- Quick Debugging Guide: [diagnostics](troubleshooting/diagnostics.md)

<a id="startup-issues"></a>
- Startup Issues: [display-touch](troubleshooting/display-touch.md)

<a id="helixscreen-crashes-immediately-segfault"></a>
- HelixScreen crashes immediately (segfault): [HelixScreen starts and exits at once (crash on startup)](troubleshooting/display-touch.md#helixscreen-starts-and-exits-at-once-crash-on-startup)

<a id="no-display-device-found"></a>
- "No display device found": [The log says no display device was found](troubleshooting/display-touch.md#the-log-says-no-display-device-was-found)

<a id="logs-are-empty-or-not-appearing"></a>
- Logs are empty or not appearing: [The log is empty or missing](troubleshooting/diagnostics.md#the-log-is-empty-or-missing)

<a id="connection-issues"></a>
- Connection Issues: [connection](troubleshooting/connection.md)

<a id="cannot-connect-to-moonraker"></a>
- "Cannot connect to Moonraker": [The screen says it cannot connect to Moonraker](troubleshooting/connection.md#the-screen-says-it-cannot-connect-to-moonraker)

<a id="connection-lost-during-print"></a>
- "Connection lost" during print: ["Disconnected" during a print](troubleshooting/connection.md#disconnected-during-a-print)

<a id="wifi-wont-connect"></a>
- WiFi won't connect: [WiFi won't connect](troubleshooting/connection.md#wifi-wont-connect)

<a id="wifi-isnt-responding"></a>
- "WiFi isn't responding": [WiFi isn't responding](troubleshooting/connection.md#wifi-isnt-responding)

<a id="wifi-permission-denied"></a>
- WiFi permission denied: [No WiFi networks listed, or WiFi permission denied](troubleshooting/connection.md#no-wifi-networks-listed-or-wifi-permission-denied)

<a id="screen-shutdownreboot-is-not-permitted"></a>
- Screen Shutdown/Reboot is not permitted: [Shutting down or rebooting from the screen is not permitted](troubleshooting/connection.md#shutting-down-or-rebooting-from-the-screen-is-not-permitted)

<a id="ssltls-certificate-errors"></a>
- SSL/TLS certificate errors: [Certificate errors](troubleshooting/connection.md#certificate-errors)

<a id="display-issues"></a>
- Display Issues: [display-touch](troubleshooting/display-touch.md)

<a id="black-screen-on-startup"></a>
- Black screen on startup: [The screen stays black](troubleshooting/display-touch.md#the-screen-stays-black)

<a id="wrong-screen-size-or-resolution"></a>
- Wrong screen size or resolution: [The picture is the wrong size or resolution](troubleshooting/display-touch.md#the-picture-is-the-wrong-size-or-resolution)

<a id="resolution-stuck-at-the-wrong-size-or-the-size-override-has-no-effect"></a>
- Resolution stuck at the wrong size, or the size override has no effect: [The resolution will not change](troubleshooting/display-touch.md#the-resolution-will-not-change)

<a id="ui-elements-look-too-large-or-too-small"></a>
- UI elements look too large or too small: [Buttons and text look too large or too small](troubleshooting/display-touch.md#buttons-and-text-look-too-large-or-too-small)

<a id="colors-are-wrong-red-and-blue-swapped"></a>
- Colors are wrong (red and blue swapped): [Red and blue are swapped](troubleshooting/display-touch.md#red-and-blue-are-swapped)

<a id="5ghz-wifi-networks-not-showing"></a>
- 5GHz WiFi networks not showing: [Only 2.4GHz WiFi networks show](troubleshooting/connection.md#only-24ghz-wifi-networks-show)

<a id="touch-input-issues"></a>
- Touch Input Issues: [The screen does not respond to touch](troubleshooting/display-touch.md#the-screen-does-not-respond-to-touch)

<a id="touch-not-responding"></a>
- Touch not responding: [The screen does not respond to touch](troubleshooting/display-touch.md#the-screen-does-not-respond-to-touch)

<a id="taps-register-as-swipes"></a>
- Taps Register as Swipes: [Taps register as swipes](troubleshooting/display-touch.md#taps-register-as-swipes)

<a id="the-toolhead-plate-or-bed-tab-wont-move-the-toolhead"></a>
- The Toolhead plate or Bed tab won't move the toolhead: [The Toolhead plate or Bed tab will not move the toolhead](troubleshooting/display-touch.md#the-toolhead-plate-or-bed-tab-will-not-move-the-toolhead)

<a id="unintended-clicks-while-scrolling"></a>
- Unintended Clicks While Scrolling: [A button fires while I scroll](troubleshooting/display-touch.md#a-button-fires-while-i-scroll)

<a id="touch-input-is-inaccurate"></a>
- Touch Input is Inaccurate: [Taps land in the wrong place](troubleshooting/display-touch.md#taps-land-in-the-wrong-place)

<a id="touch-is-offset-from-visual-elements"></a>
- Touch is offset from visual elements: [Touch is offset from what is on screen](troubleshooting/display-touch.md#touch-is-offset-from-what-is-on-screen)

<a id="calibration-doesnt-help--touches-still-wildly-off"></a>
- Calibration doesn't help — touches still wildly off: [Calibration did not help](troubleshooting/display-touch.md#calibration-did-not-help)

<a id="print-issues"></a>
- Print Issues: [printing](troubleshooting/printing.md)

<a id="files-not-appearing"></a>
- Files not appearing: [The file list is empty or a file is missing](troubleshooting/printing.md#the-file-list-is-empty-or-a-file-is-missing)

<a id="print-wont-start"></a>
- Print won't start: [Tapping Start Print does nothing](troubleshooting/printing.md#tapping-start-print-does-nothing)

<a id="preparing-takes-long-or-seems-stuck"></a>
- Preparing takes long or seems stuck: [Preparing Print takes a long time or seems stuck](troubleshooting/printing.md#preparing-print-takes-a-long-time-or-seems-stuck)

<a id="cant-pause-or-cancel-print"></a>
- Can't pause or cancel print: [Pause or Cancel does not respond](troubleshooting/printing.md#pause-or-cancel-does-not-respond)

<a id="snapmaker-u1-resume-shows-filament-not-set"></a>
- Snapmaker U1: Resume shows "Filament not set": [Snapmaker U1: Resume says Filament not set](troubleshooting/printing.md#snapmaker-u1-resume-says-filament-not-set)

<a id="power-cut-out-during-a-print"></a>
- Power cut out during a print: [Resume interrupted print? appears after a power cut](troubleshooting/printing.md#resume-interrupted-print-appears-after-a-power-cut)

<a id="time-remaining-is-inaccurate-or-slow-to-settle"></a>
- Time remaining is inaccurate or slow to settle: [Time remaining is inaccurate or slow to settle](troubleshooting/printing.md#time-remaining-is-inaccurate-or-slow-to-settle)

<a id="amsmulti-material-issues"></a>
- AMS/Multi-Material Issues: [filament-systems](troubleshooting/filament-systems.md)

<a id="ams-slots-not-detected"></a>
- AMS slots not detected: [The filament panel shows no slots (AMS, Happy Hare, AFC)](troubleshooting/filament-systems.md#the-filament-panel-shows-no-slots-ams-happy-hare-afc)

<a id="loadunload-fails"></a>
- Load/Unload fails: [Load or Unload does nothing](troubleshooting/filament-systems.md#load-or-unload-does-nothing)

<a id="spoolman-issues"></a>
- Spoolman Issues: [Spoolman Issues](troubleshooting/filament-systems.md#spoolman-issues)

<a id="spoolman-not-showing"></a>
- Spoolman not showing: [Spoolman does not show up](troubleshooting/filament-systems.md#spoolman-does-not-show-up)

<a id="spool-data-not-syncing"></a>
- Spool data not syncing: [Spool data is out of date](troubleshooting/filament-systems.md#spool-data-is-out-of-date)

<a id="spoolman-didnt-deduct-a-print-multi-filament-systems"></a>
- Spoolman didn't deduct a print (multi-filament systems): [Spoolman did not deduct a print (multi-filament systems)](troubleshooting/filament-systems.md#spoolman-did-not-deduct-a-print-multi-filament-systems)

<a id="a-lane-shows-different-details-than-mainsail-fluidd-or-the-spool-in-spoolman"></a>
- A lane shows different details than Mainsail, Fluidd or the spool in Spoolman: [A lane shows different details than Mainsail, Fluidd or Spoolman](troubleshooting/filament-systems.md#a-lane-shows-different-details-than-mainsail-fluidd-or-spoolman)

<a id="only-some-spools-showing-in-spoolman-lists"></a>
- Only some spools showing in Spoolman lists: [Only some of my spools show in Spoolman lists](troubleshooting/filament-systems.md#only-some-of-my-spools-show-in-spoolman-lists)

<a id="calibration-issues"></a>
- Calibration Issues: [printing](troubleshooting/printing.md)

<a id="input-shaper-measurement-fails"></a>
- Input Shaper measurement fails: [The input shaper measurement fails](troubleshooting/printing.md#the-input-shaper-measurement-fails)

<a id="screws-tilt-shows-wrong-adjustments"></a>
- Screws tilt shows wrong adjustments: [Screws tilt shows the wrong adjustments](troubleshooting/printing.md#screws-tilt-shows-the-wrong-adjustments)

<a id="performance-issues"></a>
- Performance Issues: [performance](troubleshooting/performance.md)

<a id="ui-feels-slow-or-laggy"></a>
- UI feels slow or laggy: [The interface feels slow or laggy](troubleshooting/performance.md#the-interface-feels-slow-or-laggy)

<a id="high-memory-usage"></a>
- High memory usage: [The printer runs out of memory](troubleshooting/performance.md#the-printer-runs-out-of-memory)

<a id="configuration-issues"></a>
- Configuration Issues: [settings-config](troubleshooting/settings-config.md)

<a id="first-run-wizard-keeps-appearing"></a>
- First-run wizard keeps appearing: [The setup wizard keeps appearing](troubleshooting/settings-config.md#the-setup-wizard-keeps-appearing)

<a id="settings-not-saving"></a>
- Settings not saving: [Settings do not save](troubleshooting/settings-config.md#settings-do-not-save)

<a id="wrong-printer-detected"></a>
- Wrong printer detected: [HelixScreen shows the wrong printer model](troubleshooting/settings-config.md#helixscreen-shows-the-wrong-printer-model)

<a id="mainsail-or-fluidd-shows-an-old-helix_print-update"></a>
- Mainsail or Fluidd shows an old helix_print update: [Mainsail or Fluidd lists an old helix_print update](troubleshooting/updates-install.md#mainsail-or-fluidd-lists-an-old-helix_print-update)

<a id="creality-k1-series-issues"></a>
- Creality K1 Series Issues: [Creality K1: Creality Print can no longer find or connect to the printer](troubleshooting/updates-install.md#creality-k1-creality-print-can-no-longer-find-or-connect-to-the-printer)

<a id="black-screen-after-boot"></a>
- Black screen after boot: [Adventurer 5M: black screen after boot](troubleshooting/updates-install.md#adventurer-5m-black-screen-after-boot)

<a id="service-commands-sysv-init"></a>
- Service commands (SysV init): [Adventurer 5M: service commands](troubleshooting/updates-install.md#adventurer-5m-service-commands)

<a id="sshscp-notes"></a>
- SSH/SCP notes: [Adventurer 5M: SSH and SCP notes](troubleshooting/updates-install.md#adventurer-5m-ssh-and-scp-notes)

<a id="windows-users-scp--o-not-supported"></a>
- Windows users: scp -O not supported: [Windows users: scp -O not supported](troubleshooting/updates-install.md#windows-users-scp--o-not-supported)

<a id="forgex-not-installed"></a>
- ForgeX not installed: [Adventurer 5M: the installer says Forge-X is not installed](troubleshooting/updates-install.md#adventurer-5m-the-installer-says-forge-x-is-not-installed)

<a id="restoring-guppyscreen"></a>
- Restoring GuppyScreen: [Adventurer 5M: going back to GuppyScreen](troubleshooting/updates-install.md#adventurer-5m-going-back-to-guppyscreen)

<a id="gathering-diagnostic-information"></a>
- Gathering Diagnostic Information: [diagnostics](troubleshooting/diagnostics.md)

<a id="enabling-debug-logging"></a>
- Enabling Debug Logging: [Turning on debug logging](troubleshooting/diagnostics.md#turning-on-debug-logging)

<a id="mainsailos--raspberry-pi-systemd"></a>
- MainsailOS / Raspberry Pi (systemd): [Restarting HelixScreen](troubleshooting/diagnostics.md#restarting-helixscreen)

<a id="flashforge-adventurer-5m--forge-x-sysv-init"></a>
- Flashforge Adventurer 5M / Forge-X (SysV init): [Restarting HelixScreen](troubleshooting/diagnostics.md#restarting-helixscreen)

<a id="after-collecting-logs"></a>
- After collecting logs: [Collecting logs](troubleshooting/diagnostics.md#collecting-logs)

<a id="system-information"></a>
- System Information: [System information](troubleshooting/diagnostics.md#system-information)

<a id="collecting-logs"></a>
- Collecting Logs: [Collecting logs](troubleshooting/diagnostics.md#collecting-logs)

<a id="configuration"></a>
- Configuration: [Settings and hardware information](troubleshooting/diagnostics.md#settings-and-hardware-information)

<a id="display-information"></a>
- Display Information: [Settings and hardware information](troubleshooting/diagnostics.md#settings-and-hardware-information)

<a id="getting-help"></a>
- Getting Help: [Getting help](troubleshooting/diagnostics.md#getting-help)

<a id="check-existing-resources"></a>
- Check Existing Resources: [Getting help](troubleshooting/diagnostics.md#getting-help)

<a id="opening-a-new-issue"></a>
- Opening a New Issue: [Opening a new issue](troubleshooting/diagnostics.md#opening-a-new-issue)

*Back to: [User Guide](USER_GUIDE.md) | [Installation](INSTALL.md)*
