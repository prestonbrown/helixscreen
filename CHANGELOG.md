# Changelog

All notable changes to HelixScreen will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0-beta.16] - 2026-11-04

The sixteenth beta of 1.1 is mostly fixes as we near 1.1.0. Filament slots keep their details when Moonraker is slow, slow print starts no longer end as failures, the AD5X starts without pegging Moonraker, and new installs of the helper macros stay out of your Fluidd and Mainsail macro list.

### Added

- **Set each Snapmaker U1 toolhead's nozzle** (#1927) - after a swap, pick its size and Standard or High Flow in the filament system's Toolhead settings. High Flow is available at 0.4 mm only, and a refused change is reported and the setting goes back. Thanks to Roman Lisunov.
- **Orca filament ids from Spoolman** (#2002, #1923) - a Spoolman filament with an `orca_filament_id` extra publishes that id for the slot its spool is linked to, so OrcaSlicer can pick that exact preset. It clears when the spool or the extra is removed. The id must be a single token with a letter, so a record id or preset name typed into the field does not replace a good catalog id. Thanks to Roman Lisunov.

### Changed

- **HelixScreen's print signals stay out of your macro list** - a new install of the helper macros names its signals with a leading underscore (`_HELIX_READY`, `_HELIX_ENDED`, `_HELIX_RESET` and `_HELIX_PHASE_*`). A printer that already has the macros keeps the old names, including when it updates, so an existing `PRINT_START` needs no change. On a new install, add `_HELIX_READY` to the end of `PRINT_START`; a guide that still says `HELIX_READY` gets you an `Unknown command` message and no ready signal.
- **AD5M Pro fan names and sound** - aux is the side fan and chamber is circulation, existing Forge-X and Z-Mod settings are corrected, and the KlipperMod AD5M Pro ships with sound off.
- **The runout dialog leads with Load on an AD5X** - a paused, homed AD5X can now load, so that is the first choice offered.

### Fixed

**Printing**

- **A slow print start stays preparing** (#1898) - a printer that runs a long start sequence before answering no longer has the job marked failed while it carries on. Stop can still cancel it, and a start refused with the connection down fails right away.
- **Print start collector** (#1875) - fixed a crash risk from its timer being deleted off the UI thread.

**Filament**

- **Filament slots keep their details when Moonraker is slow to answer** (#2008) - if the printer's saved lane records could not be read within 5 seconds of connecting, HelixScreen could overwrite them with what the firmware reports, losing the catalog product, color name, vendor and any color or material you set. It now holds its writes until it has read them.
- **AD5X paused loads** - a load refuses while filament sits at the toolhead, the refusal names homing only when homing is the reason, and the runout Load sends the right channel.
- **A purge that timed out does not run late** - a slow home no longer times out the purge on screen and then runs it anyway, and a purge that failed on the way to the bin stays cancelled.

**Calibration**

- **A failed Z-offset run closes its probe session** (#1994) - Mainsail and Fluidd no longer keep a manual probe dialog open after the failure, and the error stays on screen.
- **Purge chute calibration never moves the head unasked** - on a K1 with a CFS, Cancel, Back or a sleeping screen could move the toolhead, even after a print had started from another client. These now send nothing during a print, sleep keeps your session, and Save refuses when the printer is not homed.

**Startup**

- **Moonraker stays responsive on a large config folder** - each HelixScreen start walked the whole config folder three times, which held Moonraker near 100% for about 4 minutes on an AD5X. It now lists only the folders it needs.

**Sound**

- **`M300` stays silent during a print** - on a printer whose only speaker is its `M300` beeper, such as the KlipperMod AD5M Pro, a tune played mid-print stalled the toolhead. A paused or idle printer still beeps, and a preset's sound default now applies at once.

**Installer**

- **K1 with the Guilouz Helper Script** (#1894) - the installer no longer mistakes these printers for Simple AF, so the Creality backend stays available to Creality Print and Creality Cloud, and uninstall can restore what it disabled.

**Interface and sensors**

- **Humidity chips name their temperature** (#1922) - a humidity chip's temperature reading now names its chip.
- **Color picker leak** - each close of the color picker no longer leaks memory.
- **Plugin install result icon** - the result screen shows its icon correctly.
- **Sample Bed Level button** - the settings template and configuration guide named a macro that does not exist. They now use `HELIX_BED_MESH_IF_NEEDED`.

## [1.1.0-beta.15] - 2026-11-02

The fifteenth beta of 1.1 is almost all fixes as we near 1.1.0. Lua plugins that send G-code or register callbacks no longer crash the screen at startup, and builds without a camera view no longer show a Camera button that does nothing. The WiFi radio gets its country back after a firmware update resets it, so 5 GHz networks reconnect on their own. A printer whose Moonraker requires a login can now take its API key on the screen. A K2 Pro running k2-improvements is recognized as a K2 Pro with its stock probe, a QIDI Box shows the material and color its own screen saved, and the Snapmaker U1 checks its heads before pressure advance calibration instead of pausing two minutes into a print.

### Added

- **Enter the Moonraker API key on the screen** (#1929, #1930) - Settings > Connection has an API Key row, and the Change Host dialog a masked key field beside the host and port; saving reconnects with the key at once. The setup wizard shows the field when Test Connection finds a printer that requires a login. A printer that refuses the login says so once, with a button to the key, instead of a toast for every refused request, and fixing the login on the Moonraker side clears the warning.
- **Set a slot's material in one tap before printing** - when the filament check stops a print because a slot has no material set and a calibration needs one, it offers to set the slot to the material the file names. A file that names no material gets no button, since a guess would calibrate at another material's settings.

### Changed

- **WiFi country** - the United States, United Kingdom, Germany, Canada, Australia and France are listed first. A country the printer's own system set is remembered, so when a firmware update resets the radio to the restricted worldwide mode, HelixScreen sets it again at the next start and 5 GHz networks reconnect without a visit to the settings.
- **Print Status shows the chamber temperature with the chamber heater off** - the chamber chip appeared only while the chamber heated or was above 50°C, so a QIDI Q2 printing PLA, or a printer with only a chamber sensor, never showed it. It now shows whenever the printer has a chamber.

### Fixed

**Printing**

- **Pressure advance calibration on the Snapmaker U1** - a head holding filament with no material set made the printer pause two minutes into the start. The filament check now stops the print before it heats, and only when calibration needs the material; with calibration off the head prints as it is.
- **Pre-print ends when the printer's own start macro does** - on the FlashForge Creator 5 with OpenCreator's Kalico, the start macro runs before the file streams, and the screen sat on Purge until the first layer. It now moves on when the macro returns, and this also works for the second and later prints in a session.
- **K2 pre-print steps** - the checklist no longer marks Purge done while the nozzle is being cleaned. The cleaning step pushes a little filament to check flow, which read as the purge; Purge now shows once, after the bed mesh, when the real purge runs.

**Filament systems**

- **K2 with a CFS at boot** (#1933) - a K2 that has a CFS no longer looks like a single-spool printer for a moment while the CFS is still answering at boot, after a Klipper restart, or after Refresh RFID or Comm test.
- **QIDI Box with newer Box firmware** - slots set on the Box's own screen (Box firmware 02.03.01.21 and later) showed no color or material; they now show both, and the same spool is no longer mistaken for a new one before the Box has described it.
- **AD5X purge on Z-Mod** - Purge moves to the bin first, using Z-Mod's own bin macros, instead of extruding wherever the nozzle happened to be. Forge-X is unchanged.
- **Filament use with a multiACE on a Snapmaker U1** - filament a nozzle drew from an ACE bay was charged to the head's lane instead of the bay's spool, so the bay's remaining weight never went down.

**Plugins**

- **Lua plugins on K1, AD5X, Creator 5 and K2 builds** (#1995) - a plugin that sent G-code, or registered a `helix.ui.on` or `helix.settings.on_change` handler, crashed the screen at startup and kept it in a crash loop. On these builds the compiler gave every plugin binding's private state one shared slot, so each binding read another's state as its own. Each binding now keeps its own.

**Printers and calibration**

- **K2 Pro with k2-improvements** - it was detected as a K2 Plus, because the mod had an entry only for the Plus. The K2 Pro has its own now, and on both a K2 that kept the stock PRTouch probe no longer shows it as a Cartographer, so its probe tools work. A fitted Cartographer still shows as one.
- **Centauri Carbon bed screws** - Bed Screws shows Klipper's own turns again, measured from a base screw, instead of COSMOS's manual-leveling numbers that asked for about two turns on every corner of a nearly level bed. The Bed Screws and Bed Mesh quick actions run again instead of failing with "Invalid macro name".
- **AD5X stale filament sensor** - AD5X installs first set up on 1.0.x stop reporting a missing `runout_sensor` on every boot. The settings migration drops or renames the stale entry and keeps your role for it.

**Screen and devices**

- **Camera button on builds without a camera view** (#1993) - the K1-family, AD5X, AD5M, K2, Centauri Carbon and Snapmaker U1 builds have no on-screen camera view, but still showed a Camera row in Settings > Hardware that did nothing, and on a remote screen a More button with an empty menu. Both are hidden on those builds now. The camera guide lists which printers have no camera view.
- **WiFi says 5 GHz when it sees 5 GHz** - the country row stopped claiming "2.4 GHz only" on radios whose 5 GHz support shows up only in scan results, and the WiFi address shows even when wpa_supplicant leaves it out.
- **Ethernet status** - an unplugged port reads No cable and shows no address, even when the old address is still on the interface. Hidden networks no longer appear in the scan list as rows of dots, and with WiFi and Ethernet both connected the Test Network button stays on screen.
- **Color picker crash** (#1836) - closing a screen while a color picker was open could crash the next color picker opened.
- **Closing a camera view** no longer freezes the screen for up to 3 seconds on a camera that stopped answering.
- **Smaller fixes** - the uptime text is translated, the Last print failed icon is red, and an idle Centauri Carbon no longer re-reads print-time estimates four times a second.

**Updates and installing**

- **Snapmaker U1 env file permissions** (#1682) - the directory holding `helixscreen.env` belongs to the printer_data owner with mode 755, so a save from the web UI no longer leaves the launcher refusing it. When it does refuse, it gives the `chmod` that fixes it instead of a `chown` the next save undoes.
- **Installs on ZFS** - a freshly downloaded update is no longer rejected as "too small" on a ZFS filesystem.

## [1.1.0-beta.14] - 2026-10-28

The fourteenth beta of 1.1, and the first after the 1.1 feature freeze, so most of it is fixes. Stop during a print's preparation now cancels a start the printer has already accepted instead of letting it go on to print, the AD5X IFS unloads the lane that is really loaded, a K2 without a CFS is treated as the single-spool printer it is, and the Android app shows https cameras. The fit check reads the nozzle each Snapmaker U1 head really has and judges only the tools a file uses. The last features before the freeze arrive with it: a Toolhead widget you can move the toolhead from, moves that lift clear of a print still on the bed, filament chips on the file screen that change a tool's slot in place, warnings for an open door and for files that mix high- and low-temperature materials, QIDI's full M604 load wizard, and Creator 5 calibrations and chamber air modes on Z-Mod.

**Upgrading from beta.13?**

- **Nothing on the Motion screen homes by itself any more:** the Move tab's positions, Park, the coordinates and the Bed tab stay disabled until the printer is homed, and only a Home button homes it.
- **Screen Sharing has its own page, and is set per printer:** Share Screen, Allow Remote Control and pairing moved from the Connection page to **Settings > Connection > Screen Sharing**. Sharing now belongs to the printer you turn it on for: if it was on, it stays on for the printer you were connected to and is off for your other printers. A new **Add to Mainsail/Fluidd Webcams** switch there decides whether a shared screen is listed as a **HelixScreen Screen** webcam; turn it off to share the screen in a browser without the webcam entry.
- **MedusaHC follows the upstream Irbis3D controller:** swaps always use your machine's own `T<n>` and `DROP_TOOL`, with or without klipper-toolchanger, and tool mapping rewrites the file. If your printer still runs the abandoned community fork of the MedusaHC module, move to Irbis3D's controller: a MedusaHC is now recognized only when the controller's `pin_watch` is present.
- **Update channel edits in Mainsail or Fluidd stick:** changing HelixScreen's channel in `moonraker.conf` by hand now switches the screen's channel too, instead of being written back.

### Added

- **A Toolhead widget you can move the toolhead from** - X, Y and Z, shown as `--` until each axis is homed, with a plate view and a crosshair for the nozzle. Once the printer is homed and idle, tap the plate to pick a spot and confirm with Move here, or drag to move the toolhead live, like the Motion screen's Bed tab; while it can move, a hold or drag on the plate never enters edit mode or swipes the page. A 2x3 tile adds three preset buttons (Front, Center and Park by default; any Move-tab position, Park, Z +10, Z -10 or a macro, set from the gear in edit mode). A tap anywhere else opens Motion. Not available on the K-Touch.
- **Moves lift clear of a print left on the bed** - after a print ends (finished, canceled or stopped with an error, from any app), moves across the plate, the Move tab's positions and Park lift above its height plus the Bed Map Clearance, and the readout says why ("will lift to 77.6mm: last print is 72.6mm tall"). The print's objects are outlined on the plate, and a Cleared button ends it once you confirm the bed is clear; the next print ends it too. A part too tall to clear refuses the move with a notice. It survives a restart and is kept per printer.
- **Filament chips you can change in place** - the FILAMENTS card on the print file screen shows one chip per tool the print uses: the file's material on its color, over the spool and lane number that will feed it. The outline says whether the pairing is a problem: grey when it matches, amber when only the color differs, red when the tool has no slot, its slot is empty or the material differs. Tap a chip for a list of Auto and every slot, best match first, each with a reason, and "More options..." for the full Filament Mapping dialog. On a printer with no filament system the card compares the file with the loaded spool, with a swap icon on each later filament change.
- **Door-open warning** - on a printer with a door or lid switch, Start Print warns with **Start Anyway** when a door is open and the print uses a material that needs a warm enclosure (ABS, ASA, PC, nylon), and a door opened mid-print on such a print shows a notice once. PLA, PETG and unknown materials never warn. A new Inputs section in Settings > Devices > Sensors shows every switch input, marks which are doors, and turns the check off.
- **Mixed-temperature warning** - a file that pairs a material needing a heated enclosure with one that cannot share its nozzle temperature, such as PLA with ABS, gets a **Mixed-Temperature Materials** warning before it starts, saying whether the two share one nozzle or one chamber and bed.
- **3MF plates by name** - the plate stepper shows the name you gave each plate in the slicer, with its position beside it, and the plate printed is always the plate shown.
- **QIDI M604 loads finish on the screen** - on the Q2 and Max 4 (firmware 01.01.02.x), a load from HelixScreen now runs the whole stock wizard: push the filament in, Extrude more or Done, then the nozzle cools and the filament sensor is turned back on. A load used to stop after the first step with a hot nozzle and ungripped filament. With a QIDI Box, box slots load and unload the way the stock screen does them.
- **Creator 5 on Z-Mod: calibrations and chamber air** - Tool Offsets runs Z-Mod's own offset calibration prompt, a new **Vertical Artifact Compensation** row under Advanced tunes the X and Y motors with a head you pick (also on FlashForge's stock motor resonance tester), and Fan Control gains a **Chamber air** row for Recirculate, Exhaust and Off.
- **MedusaHC** - tool offsets edit X, Y and Z, and a change to the mounted tool applies right away; a **Clean nozzle** action under Maintenance runs the printer's `CLEAN` macro; a failed swap names the tool it was going to, and Resume after a failed swap waits for that tool to heat above 190°C.
- **OrcaSlicer 2.5 picks the exact filament preset** - lane records now carry the filament's OrcaSlicer id when exactly one catalog product has it, so OrcaSlicer 2.5 selects that product's preset instead of a generic one. Earlier OrcaSlicer versions match by material type, as before.
- **Notification History without the bell** - with no Notifications widget on any home page, **Advanced > Notification History** opens notification history and Printer Events.

### Changed

- **Large previews on slow boards** - in Auto, a file whose 3D preview would cost too much CPU shows its thumbnail instead, judged from how fast this board read earlier files, with a lower limit while a print runs. Preview work runs at the lowest priority so it yields to the screen and to Klipper, arc-fitted files parse faster, and a layer bigger than the preview cache no longer re-parses on every frame, which stalled the Centauri Carbon for most of a second at a time.
- **Settings > Sensors** - every kind of sensor uses the same row, with one switch size. Chamber and dryer humidity chips, and board thermistors such as an extruder board's, now appear, and a probe you set to no role keeps it.
- **Run Macro dialog** - fields sit in columns on wider screens, and the dialog sizes to its content.
- **Tool changes say "Switching to"** - the toast appears when the change is accepted, so a swap that then fails no longer reports success first.
- **Bed Map Clearance** - its description now reads "Plate moves and presets lift to this height first", since it governs the Toolhead widget and the Move tab too.

### Fixed

**Printing**

- **Stop during preparing** (#1890) - on printers that run their start sequence inside the print command, Stop while homing, heating or probing only stopped the screen's tracking, and the print went on to start. Stop now cancels on the printer, waiting for the start to be accepted if it is still on its way, and the job stays in preparing until the printer confirms. A reprint or upload withdrawn during the transfer never starts.
- **Fit check on the Snapmaker U1** (#1925, #1926) - it read the 0.4 mm the config lists for every head, so a correct job for a 0.6 mm head was warned about and a 0.4 mm job sent to a 0.2 mm head passed. It now reads the size and type each head reports, and judges only the tools the file prints with. A file sliced for the other nozzle type (High Flow or standard) gets a warning with **Start Anyway**, either way round.
- **The layer count no longer jumps on every travel move** - on printers whose slicer does not report layers, the current layer is worked out from the nozzle height, and each Z-hop bumped it up a few layers and back. A new layer now shows once the nozzle has stayed at that height for a moment.
- **Two start warnings at once** - the second dialog opened behind the first; it now waits for the first to close.
- **The file's material** - the file details named the material of the slicer's first filament even when the print used another tool.

**Filament systems**

- **AD5X IFS unload** (#1897) - on the standalone IFS module, Unload could target a remembered lane instead of the one the module reports loaded. It now follows the module, and unloading an unlabeled loaded lane heats to a usable temperature instead of being refused on a cold nozzle.
- **K2 without a CFS** (#1904) - with no CFS attached, the K2 is a single-spool printer: the filament system screens, slot widgets and bypass controls stay out of the way, Load and Unload run the K2's own external-spool load and its cut and retract, and the pre-print checks no longer ask you to turn off a bypass it cannot switch. Its toolhead filament sensor is back in Settings > Sensors, the sensor setup wizard and the clog meter.
- **Filament mapping on two filament systems** - picking a slot on the second system remapped the same-numbered slot on the first.
- **Mapping checks judge the lane that prints** - OpenAMS and ACE are judged on the lane each tool really loads from, an Auto pick is judged on its routed lane instead of reading as unmapped, and printers whose firmware picks lanes at print time (Z-Mod) no longer claim one.
- **Snapmaker U1 multi-head load and unload** - the sidebar shows the run's own steps instead of reading an unload as a tool swap, follows the head being worked on, and no longer stays busy for 10 minutes when the firmware never reports.
- **Cancel only where it works** - QIDI Box, Snapmaker U1 and Multi-ACE no longer offer a Cancel or Abort that does nothing. Elsewhere the status line has a Cancel while an operation runs.
- **MedusaHC** - a load waiting for you to act gave up after about 19 seconds instead of 3 minutes. A run ends cleanly when Klipper restarts or the connection drops, and stops before a print starts; a print cannot start while one runs. Swaps are refused while a dock sensor fault stands, and the dock sensors, not the tool number, say which tool is on the carriage.
- **Slot picker width** - the picker sizes to its rows instead of a fixed 480px, so short choices no longer leave it half empty and long lane names are no longer cut off with room to spare.
- **Tool changers without klipper-toolchanger** no longer get a missing-hardware warning on every start, and a CFS clog meter reads "Buffer" instead of "AFC buffer".

**Screen and devices**

- **Printers whose Moonraker requires a login** - with `force_logins` on, Moonraker refuses every request that carries no credential, so the screen showed nothing (seen on a Snapmaker U1). The `moonraker_api_key` setting is now sent, to the configured printer only, including on the K-Touch.
- **Hotend fan on the K2** - the hotend fan now shows in Fan Control, without a dial: it runs with the heater and is never commanded from the screen.
- **Missing filament sensors** - a configured filament sensor that disappears from the printer is flagged by the hardware check again.
- **Thermistor carousel** - a carousel placed before the printer's sensors were found stayed empty.
- **Screen Sharing after a printer switch** - the Share Screen and Allow Remote Control rows and the pairing code no longer vanish, and the remote-control indicator keeps tracking.
- **Centauri Carbon with RustiCC** - HelixScreen recognizes RustiCC's printer services and runs at a lower priority beside them, as it does beside Klipper.
- **More crash reports** - a crash on an illegal instruction, a trap or a bad system call now leaves a crash report, and a crash the app could not record itself is recorded by the watchdog.

**Updates and installing**

- **K2 self-update on a full system partition** - an update no longer refuses over a read-only or full `/etc/init.d` it does not need to write, the boot script is replaced safely or kept, and boot links that already work are left alone, so a full partition cannot leave the printer without autostart.

**Android**

- **https cameras** (#1891) - cameras on an https address now show, a camera whose pictures keep failing says so instead of spinning on Connecting Camera, and the Spoolman QR scanner shows camera errors.

## [1.1.0-beta.13] - 2026-10-25

The thirteenth beta of 1.1. The headline is Glass Effects, an optional lit, glassy finish for buttons, cards and controls on screens fast enough to draw it. Prints that end or pause on an error now say why, and finding a printer gets easier: the connection screens sweep your local network for Moonraker, fall back to port 80, and test an address without dropping the printer you are connected to. Print Status follows up on beta.12 with a speed chip that reads the live toolhead speed and flow, a camera button in the preview card on remote screens, and a picture that holds still while fans spin up. MedusaHC tool changers gain a purge length setting and explain failed tool changes, AFC faults during a paused print open their recovery dialog, and the K-Touch catches fast taps on its keyboard.

**Upgrading from beta.12?**

- **Print Status speed chip:** it now shows how fast the toolhead is moving in mm/s, with the volumetric flow in mm³/s under it, instead of the speed and flow percentages. The percentages live in Tune; the chip still turns amber while either is not 100%, and tapping it opens Tune.
- **Fan names:** the hotend fan reads "HE" in fan settings, as on its chip, and the fan stack's third row reads "Aux" (it read "Chamber"), the same fan the Print Status chip calls Aux.
- **Pre-print options:** the toggles use the step names the progress checklist shows, so Nozzle Priming now reads "Purge".
- **Slot menu:** its two columns are headed **Actions** and **Spool Info**.
- **MedusaHC loads and unloads** always use HelixScreen's own feed and retract moves and never call your printer's LOAD_FILAMENT or UNLOAD_FILAMENT macros.

### Added

- **Glass Effects** - an appearance option under Settings > Appearance that gives buttons a lit, glassy finish and every card and panel a soft light from its top-left corner. Switches, sliders, input fields, keyboard keys and the navigation bar get it too. It is drawn from your theme's own colors, so it works with every theme in dark and light mode, and it leaves filament color swatches, camera views and previews untouched. It only appears on hardware fast enough to draw it: HelixScreen measures this once on your screen and hides the option on slower boards and 16-bit panels, which keep the flat look.
- **Why a print stopped** - a canceled or failed print shows the reason, such as a quad gantry level that never converged, in the completion dialog, the print screen's cancel and error messages and the cancel toast. A paused print says why it paused too. The reason is Klipper's own message when it gives one, else the last error the printer reported. Works on the K-Touch as well.
- **Find printers on your local network** - the setup wizard's connection step and Change Host / Add Printer list every Moonraker on your local network, not only those that announce themselves (which needs `[zeroconf]` in Moonraker). Picking one fills in the address and port. A Moonraker behind a login is listed too, and a printer running HelixScreen finds its own Moonraker. This also works on the K-Touch.
- **Camera in the Print Status preview** - on a screen running on a different machine than the printer, a camera button in the corner of the preview card swaps the thumbnail or G-code render for the live webcam feed. Tap it again to go back; the choice carries over to the next print.
- **Live speed and flow on the home print card** - the detailed print card shows the toolhead speed in its own row and the volumetric flow beside the filament used, dropping either when the card is too small.
- **Random screensaver** - picks a different screensaver your screen can run each time it goes idle, never the same one twice in a row. Select it under Settings > Display > Screensaver.
- **MedusaHC purge length** - a Purge Length setting (0-200 mm, default 50) under Purge & Wipe in Device Operations sets how far a load purges past the nozzle, per printer. When loading several tools, each next tool now preheats to its full load temperature.
- **MedusaHC says why a tool change failed** - the fault dialog quotes the controller's own message (for example "sensors did not confirm T1"), and a failure that is not shown as a fault appears once as a "Tool change failed" notice.
- **Resume reloads the saved bed mesh** (#1864) - with the HelixPrint plugin's power-loss recovery, a new row in Settings > Printing adds HelixScreen's include line to printer.cfg so a resume reloads the mesh the print started with, keeping a backup, and offers the Klipper restart that applies it. Tapping it again removes the lines; uninstalling the plugin removes them too.
- **Clear Spool in the slot editor** - printers without a filament system can take a spool off the slot from the editor, and it works while Spoolman is offline.
- **The QIDI Box shows its own logo** (#1514) - instead of the generic filament system chip.
- **Chamber reading on the home picture without a heater** (#1879) - a printer with only a chamber thermometer now gets the chamber chip on the home printer picture, as Print Status already showed it.

### Changed

- **The print status picture holds still** (#1879) - the chips around the printer picture no longer rearrange it when a fan spins up, the speed is tuned or the light turns on. The layout is decided once for preparing and once for printing; a reading that becomes unusual mid-print (a spinning aux fan) slots into free space instead. A wide picture shrinks to make room for a column of chips beside it before chips are laid over the photo, and chips over the photo get a near-solid background. The picture is also drawn from a copy sized to fit, as on the home screen.
- **Moonraker on port 80** - when the port field still holds the default 7125 and nothing answers there, the connection screens try port 80 (Moonraker behind a web server) and save the port that answered. A port you typed, a saved port and a printer picked from the list are tried as they are.
- **Test Connection leaves your printer alone** - testing an address in Change Host / Add Printer no longer switches the screen to it; only Save does. The result names the port that answered and Klipper's state, or what failed.
- **CFS on the K1 series** (#968) - the fit check before a print allows for the 5 mm of Y travel the CFS upgrade takes on a K1, K1C or K1 Max.
- **Pre-print time estimate** - the estimate counts this print's own heating instead of replaying the last print's, so a cold ASA start after a quick PLA one is no longer predicted far too short.
- **Screensavers on slower boards** - a screensaver that had to drop to a lighter setting climbs back once the screen keeps up again, and a step down taken while printing lasts only for that print.

### Fixed

**Filament systems**

- **AFC faults during a paused print** - an AFC load that failed mid-print paused it without a dialog. It now opens the loading error dialog, and recovery ejects the lane of the newest fault. On the K-Touch, AFC errors outside a print get the dialog too, and the dialog no longer reappears as the error text changes.
- **Stale AFC error in the sidebar** - an old AFC error message no longer stays in the status line through a print that runs fine.
- **AD5X on Z-Mod's AFC layer: no tool remap error at print start** - Z-Mod maps tools in its own "Select print materials" prompt and refuses the screen's remap, so the screen no longer offers one there or sends it. That prompt's Start print button is also no longer read as the print having started.
- **MedusaHC Unload on docked tools** - Unload is offered for docked heads in the Filament screen, the slot menu and the Unload picker, instead of saying no filament is loaded.
- **MedusaHC sensor error** - a tool docked by hand after a failed change, or a restart, no longer leaves the screen showing a sensor error once the docks agree, and one failure is reported once instead of twice.
- **Slot cards keep the material** - a spool whose name includes the material, such as "Overture Gray ASA", no longer hides the material line.
- **Empty spools** - an empty spool's side view draws its bare core instead of nothing.
- **Unlinking a slot as a different filament** - the slot no longer keeps the old spool's weight.
- **Spoolman external spool** - an unlinked external spool entry you edited no longer vanishes.
- **Spoolman starting after Moonraker** - when Spoolman comes up late, as it can on the Centauri Carbon at boot, Spoolman features turn on as soon as it is ready instead of staying off.

**Printing**

- **Chamber presets that never heated** - on printers whose M141 macro only drives an exhaust fan, chamber temperatures are now sent to the chamber heater directly.
- **Flow during a retract** - the flow readout shows 0.0 mm³/s instead of a negative number.
- **Home print card** (#1440) - a long filament line scrolls again instead of being cut off.
- **Heaters with no target** show a dash instead of "/ 0" on the temperature and filament screens.
- **Closing the printer type warning keeps your type** (#1881) - tapping X on the "printer type mismatch" warning now counts as keeping the saved type, like Review > Keep current, so it stops coming back on every restart.

**Screen and devices**

- **Camera views** - closing a camera view no longer freezes the screen for up to five seconds, and two views of the same camera take turns, so a camera that serves one viewer at a time keeps streaming.
- **WiFi permission problems** - when HelixScreen is not allowed to manage WiFi, it says so instead of "WiFi system not ready".
- **WiFi switch on the QIDI Q2** - turning WiFi back on no longer snaps the switch back off.
- **Navigation bar icons on 480x272 screens** - the Controls and Filament icons are no longer clipped at the sides.
- **Calibration & Tools card** - has its border again, like the cards beside it.
- **HelixPrint storage report loads again** (#1885) - the plugin's storage module failed to load on Moonraker because it was loaded before the photo module it reads from.
- **Storage warnings now translate** (#1867) - when a storage cleanup folder was configured unsafely, the plugin overview showed the Moonraker plugin's raw English sentence; it is translated now like every other plugin message.

**K-Touch**

- **Fast taps** (#1884) - quick taps and rolled-over keys on the keyboard are no longer lost, and keys no longer show a preview bubble. A failing touch connection no longer floods the log.
- **Reconnects** - after the connection to the printer drops and comes back, the screen picks up Klipper's state again instead of leaving the navigation bar grayed out until a reboot.

## [1.1.0-beta.12] - 2026-10-24

<!-- whatsnew
The twelfth beta of 1.1.

- A redesigned Print Status screen built around your printer's picture
- A pre-print checklist with times for each start step
- Guided power-loss resume on the QIDI Q2 (HelixPrint plugin)
- Files that don't fit the printer are stopped before they start
- Print photos in history, a Storage page and Printer Events
- Spoolman follows each tool change and links spools from CFS tags
- Name your belts left and right in Belt Tension
-->

The twelfth beta of 1.1. The headline is a new Print Status screen: your printer's picture with a chip for each heater, fan and light pointing at its part, a checklist of the start sequence while the print prepares, and one strip for progress and the buttons. HelixPrint plugin 1.4.0 brings a large batch with it: a guided power-loss resume on the QIDI Q2, a check that a file fits the printer (which also works without the plugin), camera photos of every print in history, a Storage page with cleanup, a Printer Events log, and Spoolman switching spools at every tool change. Belt Tension learns which belt is which, bed screw adjustment says Tighten or Loosen from the probed heights, and heater calibration can tune any tool on a tool changer. Filament Mapping now follows the printer's own tool map instead of overwriting it, AFC tool changers handle tool numbers past the lane count, MedusaHC loads go in at the dock, and there are fixes for ACE, CFS, the Creator 5, Centauri Carbon reboots, QIDI, K1 first boot and label printing.

**Upgrading from beta.11?**

- **Print Status:** Camera, Timelapse and Light moved into a **More** button on the strip, and the light can also be toggled from its chip on the printer picture. Tapping the Speed / Flow chip opens Tune; switch between percent and mm/s there.
- **Tool changers:** only the active tool's temperature shows on Print Status for now, as the badge on the nozzle chip. Every tool's temperature is still on the Controls and temperature screens (#1818).
- **Flow steps:** the Tune flow buttons step by 1% and 5%, and the home Speed / Flow widget's flow buttons by 1%. Speed keeps 5% and 10%.
- **Bed screws on AD5M and AD5X:** the rotation arrows now point the way Klipper's console says, so they are reversed from beta.11. Follow the Tighten or Loosen word, which comes from the probed heights.
- **Config backups:** copies HelixScreen makes before writing your Klipper config now live in the hidden `.helix_backups` folder in your config folder.

### Added

**Print Status**

- **A new Print Status screen** - the screen is built around a picture of your printer. Each heater, fan, the light and speed/flow get a chip, and on printers whose picture has been mapped each chip sits beside its part with a dotted line to it, and the part glows while it is active. A heater chip shows a flame while it heats, a check at its target and a snowflake while it cools, turns amber when it is off target, and carries a progress bar while it climbs. The speed/flow chip turns amber when either is not 100%. Tap any chip for its detail.
- **A pre-print checklist** - while the print prepares, the preview is replaced by the start sequence: finished steps with how long they took, the current step in amber with a countdown, and an estimate for each step still to come. Small screens show the current step and a dot per step. A step the printer has not announced is never listed ahead of time.
- **One strip for the job** - progress, time remaining, finish time, elapsed time, the filament in use and the next queued job sit in one strip with Pause, Tune, More and Cancel.
- **Fits every screen** - the layout is chosen from the space the screen actually has, portrait or landscape, down to 480x320. When chips do not fit, the picture shrinks a little first, then chips showing an ordinary value (a fan at 0% or on auto, the light off) step aside, and only then do the chips move onto the parts themselves. Fan chips keep their Part, HE and Aux labels beside the picture. A heater off target or a tuned speed is never left out.
- **Snapmaker U1 top module** - the U1's picture shows its top module when it is fitted (or a chamber heater is set up), and switches live when you lift it off.

**HelixPrint plugin**

The plugin updates to 1.4.0. Each feature below says what works without it; [HelixPrint Plugin](docs/user/guide/helixprint-plugin.md) in the user guide covers each one on attached and remote screens.

- **Power-loss recovery on the QIDI Q2** (#1716) - the plugin saves the print's position about once a second, whichever app started the print. After a power cut, Resume opens a guided resume that checks the part is still on the bed, heats bed and nozzle together with the nozzle left in place, lifts and homes, has you confirm the nozzle height, then purges away from the part and continues. Nothing moves until you tap a button, and a big red Cancel stops everything. Saving is on by default on eMMC storage and off on an SD card, under **Settings > Printing > Power-loss recovery**.
- **Does the file fit this printer?** - before a print starts, the file is compared with the printer's travel, heater limits and tools. A file that would move past the travel, overheat or use a missing tool is stopped with **File Does Not Fit This Printer**, naming each problem; a different nozzle size gets a warning with **Start Anyway**; a different printer model is only noted. The findings show in a card in the file's details. This works without the plugin too, from the file's preview; with it, every move is checked, purge line and wipe tower included. **Hold slicer prints that do not fit** (Settings > Safety & Alerts, off) lets the plugin stop a slicer's upload-and-print that would fail the check.
- **Print photos** - two camera pictures of every print, one after layer 2 and one at the end, shown with the slicer preview in the print's history details. On by default in **Settings > Devices > Print photos** when a webcam is set up. With the plugin the printer takes them for every print; without it, the screen takes them while it is connected.
- **Storage** - **Settings > System > Storage** shows what fills the printer's storage and cleans up print copies, file analysis indexes, photos, timelapse videos and old logs, never your own files. The plugin adds an optional daily automatic cleanup. HelixScreen also refuses an upload, print copy or USB copy that would leave less than 100 MB free.
- **Printer Events** - tap the Notifications widget, then **Printer Events**, for a log of what happened to the printer: shutdowns and their reasons, lost MCUs, firmware restarts, pauses, runouts, errors and how each print ended. With the plugin it includes what happened while no screen was connected. Debug bundles include it.
- **Switch spools at each tool change** - in **Settings > Devices > Spoolman**, the active Spoolman spool follows the tool or lane that is printing, so each spool is charged for its own filament. With the plugin it covers every print and the print's history lists the spools it used; without it, the screen does it while connected.
- **Start options for prints from other apps** - **Settings > Printing > Apply start options to prints from other apps** sends your standing print options, such as which U1 toolheads unload at the end, to prints started from Mainsail, Fluidd, a slicer or the job queue.
- **Thumbnails and faster previews** - the plugin reads every uploaded file once: files saved without a picture get a thumbnail (in Mainsail and Fluidd too), large files preview sooner, and per-tool filament is ready for the pre-print checks. On the K-Touch this is the only way to get them.
- **Faster starts for edited prints** - when a print start changes the file, the plugin makes the copy on the printer, so a large file no longer travels to the screen and back.
- **Safer config saves** - Input Shaper Save, the PID/MPC switch and probe settings are written by the plugin in one step, refused if someone else changed the same setting meanwhile, with the last five copies of each file kept.
- **AD5X and ACE update instantly** - with the plugin, lane changes are pushed to the screen instead of polled.
- **Plugin overview** - **Advanced > HelixPrint Plugin** lists every feature as Active, Off or Unavailable with a plain reason, and tapping one opens the page where its switch lives.

**Filament and Spoolman**

- **Link Spools from Tags** - on a Creality CFS, a tag whose serial number names a Spoolman spool (as CFTag writes it) links its slot to that spool when the spool goes in. A tag naming a spool Spoolman doesn't have gets a notice, and a spool you pick while the same tag stays in is kept. On by default; turn it off in the CFS settings.
- **Creator 5 spools set from outside** - a spool an NFC reader or another tool binds to a Z-Mod Creator 5 head shows right away instead of after a reboot.

**Calibration and settings**

- **Which belt is which?** - Belt Tension can buzz each path's motor so you can see which belt it drives, then name the paths left and right or upper and lower. Results, tuning and every instruction then use those names ("Tighten the left belt one full turn"), saved per printer.
- **Heater calibration on tool changers** (#1870) - PID and MPC calibration offer a tool row on printers with several heated heads, defaulting to the active tool, instead of always tuning the first extruder.
- **Bed screws diagram** - the bed is shaded by probed height in the bed mesh colors, so a twisted bed shows as a saddle, with the spread between the highest and lowest corner beneath it. The screw furthest off is highlighted.
- **1 hour Display Sleep** option.
- **FLSUN T1 Pro picture** for the home screen and Printer Manager.

### Changed

- **Bed screws say Tighten or Loosen** - the word comes from each corner's probed height, which is right on any spring-mounted bed, and the arrow always matches Klipper's own direction. Every probe homes first, Re-probe included.
- **Home printer picture** - its chips use the same dotted leader lines and side-by-side layout as Print Status.
- **Home print card** - the stats use Print Status's icons and drop the least useful piece when a row is tight, and the nozzle icon carries its tool number.
- **Flow icon** - flow shows a nozzle instead of a water drop, on Print Status and the home Speed / Flow widget.
- **Filament sensor icons** - sensors show a sensor icon instead of the spool.
- **Progress bars** have an outline, so the empty part of the track stays visible on any card.
- **3D Pipes** draws one pipe at a time, five to a screen, like the Windows original.
- **Help** - the docs QR codes open this release's pages, and Help gains Getting Started and User Guide rows.
- **Share Screen** - opening the screen in a browser wakes a sleeping display, and while the page is open the screen dims and runs its screensaver but never goes black. Webcam integrations such as Obico and Home Assistant no longer keep it awake.
- **K-Touch** - screens switch about 20% faster.

### Fixed

**Filament systems**

- **Filament Mapping follows the printer's tool map** (#1859, #1862) - a print no longer sends a remap you did not pick: rows you left alone follow the printer, even when its tool table arrives or changes after you opened the file, and an AFC, Happy Hare or CFS tool you remapped yourself stays on its lane. A guessed free lane is never sent as a remap. The map a print's remap displaced is put back after the print, even across a restart or a torn file.
- **Filament Mapping picks are kept** - a pick survives the file's colors arriving late, OK in the remap dialog keeps only the rows you changed, flipping auto color mapping takes effect on the open file, and a hidden mapping card sends nothing.
- **Pre-print messages** - a tool with no lane assigned says so, instead of asking you to load filament.
- **AFC tool numbers past the lane count** - a lane running as T7 on a four-lane unit can be remapped, restored and changed to, and remapping two lanes swaps their tools as AFC does.
- **AFC tool changers** (#1824, #1850) - tapping a parked head only mounts it instead of swapping filament, a Load that needs a lane swap really loads, and a head missing from the toolchanger is warned about instead of leaving the screen on Selecting for five minutes.
- **MedusaHC loads** - a Load inserts the filament at the docked hot end first, heats the right tool, and trusts a swap only once the dock sensors confirm it; a failed or canceled run turns the heaters off and never moves a carriage the sensors cannot vouch for. Unload ends by saying which tools to pull the strand from, and the status line no longer reads idle between swaps.
- **ACE Feed Assist** (#1853) - the switch shows what the driver is actually doing, flips back if the driver doesn't take the change, and is grayed out with no tool loaded.
- **Filament fault dialogs** (#1838) - faults use the standard dialog with its usual buttons, ESC and the backdrop close without acting, and a fault with no recovery actions offers OK.
- **Spoolman active spool** - reloading the same spool sets it active again, so the next print is tracked; linking a spool to the loaded lane makes it active at once; the external spool only becomes active while it feeds; and unlinking clears only its own spool.
- **Spool picker on a remapped lane** - the lane's own spool is no longer grayed out.
- **Lane settings** show the tool a multi-tool lane is running, not its first tool.
- **CFS tags** - an unknown tag record no longer appears as the brand, and a box dropping off the bus is not treated as a new spool.
- **Unwired hubs** - a unit whose lanes feed no tool ends at its hub in the unit view, as it does in the overview.

**Printing**

- **K2 bed mesh ran twice** - with Auto Bed Mesh on, the pre-print mesh is skipped when the file's start G-code already calls the same command, and only then.
- **Print history dates** (#1865) - Started and Ended show the date over the time instead of clipping.
- **First-run tour** waits while a dialog, such as a power-loss resume, covers Home.

**Screen and devices**

- **Centauri Carbon reboot and shutdown** (#1869) - Reboot and Shut Down from HelixScreen work on COSMOS, and the last frame no longer lingers on the next boot.
- **K1 series first boot** - the splash and screen come up the right way round on the first start instead of running the rotation check.
- **QIDI** (#1854) - the stock screen is matched however its service starts it, and the makerbase tuning service it wrongly disabled is turned back on once.
- **Sheet labels over IPP** (#1851) - printed blank pages; they print again.
- **Label printer settings** - typing an address or port is no longer overwritten by a printer found on the network, and the saved printer shows in the list.
- **Text fields on large screens** - one-line fields on 1920x1200 and similar screens no longer bounce while you type.
- **Share Screen** (#1856) - the tap that wakes the display no longer presses what is under it, and renaming the screen's webcam in Mainsail or Fluidd sticks.
- **Plugin tables** (#1858) - dragging across a table whose rows all fit no longer taps a row.

**Install and updates**

- **Installer re-runs keep your settings** - detected rotation and touch settings are only seeded on a fresh install.
- **Forge-X** - the installer takes over the display on a fresh Forge-X too, and when a display change is still waiting for a reboot it says to reboot first.
- **The `main/scripts/install.sh` command** used by older guides and forum posts works again.

## [1.1.0-beta.11] - 2026-10-21

<!-- whatsnew
The eleventh beta of 1.1.

- Plugins can play sounds, show tables and use the keypad, keyboard and their own icons
- Multiple ACE units on the ACEPRO driver
- Early Bondtech INDX support
- Share Screen: see and drive the screen from Mainsail or Fluidd
- Belt Tension tells you which belt to turn and how far
- Live response charts for belts and input shaping
- Snapmaker U1 tool offset calibration and safer resumes
-->

The eleventh beta of 1.1. Beyond the headline features, Belt Tension, Pressure Advance tuning, Tool Offsets, pause at a layer and per-lane drying no longer need beta features turned on. Plugins get sounds, tables, the app's keypad and keyboard and their own icons, and the HelixPrint plugin installs, updates and uninstalls without leaving Moonraker down or restarting it mid-print. Filament systems recover from a Klipper restart instead of sticking on busy, and there are fixes across ACE, AFC, CFS, Happy Hare, AD5X and the Snapmaker U1. The K1 and AD5X download is about half its old size, the Pi app starts with or without GPU libraries, and the installer copes with more K2 and multi-user setups. WiFi can find 5 GHz networks before the first connection, 480x320 screens get bigger buttons, plain motion sensors can feed Clog Detection, and the Android app reaches printers on Wi-Fi while mobile data is on and no longer draws oversized on high-DPI phones.

The headline features: daisy-chained ACE units on the ACEPRO driver each show as their own unit, Bondtech INDX tool changers get early support, and Share Screen puts the touchscreen in Mainsail or Fluidd as a camera you can watch or, once paired, drive. Belt Tension grows a guided tuning loop that says which belt to turn, which way and how far, and both it and Input Shaper draw the response live while they measure. The Snapmaker U1 gains its toolhead offset calibration on screen and a much more careful Resume.

**Upgrading from beta.10?**

- **Slot menu:** a filament slot's menu is actions only. Assigned Tool and the Endless Spool backup moved into Edit Spool and save with its other changes, and Select Slot is gone (the selector menu still moves the selector).
- **Multi-Filament System Management** moved from Settings > Devices to Settings > Printing.
- **Snapmaker U1 tool offsets:** the calibration lives under Controls > Calibration > Tool Offsets, like every other printer's.
- **Time zone:** if you never picked one, the screen now follows the system's time zone instead of showing UTC.

### Added

**Plugins**

For plugin authors:

- **Sound cues** (#1826) - `helix.sound.play` plays a cue from the user's sound theme, so their sound settings apply. Plugins get the everyday UI and event cues, not the startup chime or urgent alarms, and cues are rate-limited per plugin.
- **Tables** (#1827) - `helix.ui.table` builds a table with your own columns and widths. `set_rows` replaces its rows, each carrying a key; tapping a row runs your handler with that key, and a refresh keeps the selected row by its key.
- **The app's keypad and keyboard** (#1828) - `helix.ui.edit_number` opens the numeric keypad with a minimum, maximum, decimals and unit, including negative ranges, and `helix.ui.edit_text` opens the on-screen keyboard with a length limit or password mode. An optional validator can reject a value with a message while the editor stays open, and an editor can open another from its accept handler.
- **Custom icons** (#1829) - a plugin can ship its own PNG icons and use them for its Home tile in the widget catalog and in its own layouts. They take the theme's colors like the built-in icons, and a missing or oversized file falls back to the icon you name.
- **Printer state** - `subscribe` delivers a full snapshot on its first call and again after a reconnect or a Klipper restart, and `helix.printer` reports Klipper's state, reading nil while Moonraker is away.

For everyone:

- **HelixPrint plugin installs and updates you can trust** - installing, updating or removing the plugin restarts Moonraker through Moonraker's own API, waits for a running print to finish first, and says when a manual restart is needed instead of claiming it is done. A plugin Moonraker cannot read, or a folder it cannot link into, is refused before any config is touched. `moonraker.conf` and its included files are edited safely and restored exactly on uninstall, and it works on printers with no curl or wget (such as the K2 Plus) and on Moonraker ports other than 7125.
- **The app keeps the plugin current** - at start it repairs a plugin link left pointing at a moved install, and after an update it restarts Moonraker once, after any print, so the new plugin code actually loads.
- **Installing on another machine's Moonraker** - the command the app shows is a single `curl ... | sh` that needs no git, and a failed install keeps the previous plugin.
- **The install prompt waits its turn** (beta features) - it never opens during a print, is offered once the print and its completion screen are done, stays away for the session after an uninstall, and Install refuses while a job holds the printer. A plugin turned off in `moonraker.conf` no longer loops the prompt; installing turns it back on.
- **HelixPrint plugin 1.3.0** reports which of its features are available, and the app uses each one the installed plugin supports, so an older plugin keeps working feature by feature instead of being judged by its version.

**Filament systems**

- **Several ACE units on the ACEPRO driver** - each daisy-chained ACE shows as its own unit, named after its model (ACE Pro or ACE Pro 2), with its own dryer, temperature and humidity, firmware version and connection state. Slots show the driver's tool numbers where they differ from the slot, and a load from the screen while another tool is in the head runs as the driver's tool change, showing the old filament retracting first.
- **Bondtech INDX** (#1333) - early support for INDX tool changers, detected from the `[indx]` module. Tool changes, parking, and loads by material preset or temperature work from the screen, with batch Load, speed mode, guided tool cycle and heater calibrations, an offer to update the heater model after a material change, and a confirmed recovery when a tool is not seated. Tool Offsets can seat a tool by hand. Docked tools show their material without a temperature, and the nozzle refuses to heat with no tool on the head. Please report how it behaves on your machine in #1333.
- **Snapmaker U1 toolhead offset calibration** - the U1's own multi-head offset calibration runs as a guided flow under Controls > Calibration > Tool Offsets, in the stock screen's order with its brush, plate and probing prompts. It refuses to start during a print, and a calibration left behind by a closed session or another screen is cleaned up or asked about rather than left holding the heaters.
- **Clog Detection from a plain motion sensor** (#1823) - a Klipper `filament_motion_sensor` you enabled in Settings feeds the Clog Detection tile when nothing else reports clogs, reading OK, Jammed (only during a print) or Off when Klipper has disarmed it.
- **CFS: a spool running dry mid-print** - when a CFS box runs a spool out during a print while the toolhead is still fed from the tube, a warning names the slot before the toolhead runs dry.
- **Endless Spool switch for Happy Hare** - single-unit Happy Hare setups can turn Endless Spool on and off from the screen.

**Share Screen**

- **See and control the touchscreen from a browser** (#1825) - turn on Settings > Connection > Share Screen and the screen appears as a camera in Mainsail and Fluidd. With Allow Remote Control on, you can tap through it from the browser after pairing with the 6-digit code shown on the printer, and a small badge on the screen shows when a browser is in control. Where Moonraker has authorization set up, the installer serves it through the printer's own web address; otherwise it is reached on the LAN with pairing. The Screen Sharing guide in the docs covers setup.

**Calibration**

- **Guided belt tuning** (#1736) - after a Belt Tension check, Tune walks you to matched belts one round at a time: which belt to adjust, which way, and on screw tensioners how far in eighths of a turn, learning from each turn what it actually changed. It notices belts that already match, works zone by zone, and gives no advice when readings don't repeat. Each check runs a few settling laps first, kept to the middle of the bed on tool changers. Layouts cover portrait, ultrawide and the smallest screens.
- **Live response charts** (#1812) - Belt Tension, the Input Shaper screen and the setup wizard's input shaper step draw each sweep's response as it happens, then swap in the final curve. The chart takes most of the screen while measuring and is left out on screens too small to show it.

**Screen and settings**

- **WiFi country** - Network Settings shows and sets the radio's country under the WiFi switch, so 5 GHz networks show up in scans before the first connection.
- **Number pad** - fields that take negative values get a +/- key, the field's name shows above the value, and the keys keep the same shape on every screen size.
- **Power tile color** - the power tile takes the same color choices as the macro tile.
- **K2 Pro and K2 Plus CFS pictures** - each K2 model with a CFS connected shows its own picture.

### Changed

- **Out of beta** - Belt Tension, Pressure Advance tuning, Tool Offsets, Z calibration, pause at a layer and Happy Hare per-lane drying work with beta features off. Only the Dev update channel and the plugin install prompt still need it.
- **Filament system settings** - every system's settings screens use the same switches, sliders and lists as the rest of Settings, with icons, a typed value for sliders, and the reason a setting is unavailable shown under it.
- **Slot cards** - the line above each spool names the spool or product, not a made-up color name, and Edit Spool no longer saves one.
- **Filament catalog order** - both pickers list a family's base material first, then its "+" and filled grades, then the rest.
- **480x320 screens** - buttons are 44 px tall for easier taps, with dialogs, prompts and the filament sidebar fitted to match.
- **Smaller MIPS download** - the K1 and AD5X package drops pictures for other printers and unused translation files, about half its previous size.
- **One Pi and x86 app** - HelixScreen starts whether or not the GPU libraries are installed, using them when they are.
- **US English** - on-screen text uses US spelling (canceled, color, behavior) in English.

### Fixed

**Filament systems**

- **ACEPRO driver commands** (#1821, #1835) - feed, retract and feed assist name the unit and slot the driver expects, a status update for one unit no longer lands on another, dryer commands over the REST bridge reach only the unit they mean, and feed and retract work with nothing loaded.
- **ACE load status** - a unit's own busy and ready reports no longer end a screen load early or wipe an error the unit reported during it.
- **AFC tool changes from Orca** (#1848) - files whose tool changes carry extra parameters, as Orca's AFC Change Filament G-code writes them, show every tool in the preview and the filament mapping.
- **AFC toolchangers pick the right toolhead** (#1824) - Load and tool change send the lane or tool number and let AFC choose the head.
- **AFC tool change progress** - the steps no longer reset when AFC briefly reports Idle mid-change, and older AFC releases no longer treat later plain loads as tool changes.
- **AFC messages over AFC prompts** - an AFC error that arrives with its prompt no longer pops a toast on top of it.
- **Load and unload results read the latest status** (#1761, #1847) - a filament system's success check now runs after the printer's newest status, so a load or unload is not judged from a sensor reading that was already out of date.
- **Klipper restarts end stuck operations** - a load, unload or tool change caught by a Klipper restart no longer leaves the filament system busy, on every system including the U1, OpenAMS, multiACE and tool changers. A long operation that only outlives its timer is warned about, not cut short.
- **Happy Hare settings** - speed and toolhead sliders no longer refuse every change as an invalid value, and live controls such as gear sync and motors work before Happy Hare's config has loaded.
- **Unavailable buttons stay unavailable** - a calibration or device button the system cannot run right now is grayed out with its reason instead of sending its command, and machine-moving actions wait for the print to end.
- **Refused changes are not shown as made** - an Endless Spool switch the printer rejects flips back, and an Assigned Tool or backup the system refuses keeps Edit Spool open with the change still pending.
- **CFS** (#1820) - a load or unload fault clears once the toolhead sensor shows the problem was fixed by hand, and on the community box firmware a reprint sends its filament mapping too and a start with nothing to map clears a stale one.
- **AD5X unload with no known lane** (#1324) - Unload from the Filament screen, the runout dialog, the sensor tile and bed drying asks which lane, or offers to clear the extruder alone, and always has a plain Cancel. The lane picker fits on every screen.
- **An unhomed Load no longer homes first** (#1819) - a `LOAD_FILAMENT` macro HelixScreen does not recognize runs as it is, so a Voron whose macro only extrudes no longer gets a G28.
- **The filament mapping card shows up when slots arrive late** - on a multi-lane printer the card appears even when the filament system reports in after the file opened.

**Snapmaker U1**

- **Resume after a failed load** - a head whose load failed or never reached the toolhead holds Resume until a retried load is confirmed, a finished hand feed counts, and Resume is refused while a feed is still moving. With multiACE active, a runout never sends the stock refeed.
- **Slicer-started prints** - a print started from Orca or Fluidd gets one clear warning when it uses a toolhead past T0, since the firmware then skips per-head feeding and the end-of-print unload.
- **Reprints keep their routing** - a reprint reads the file's toolheads, so per-head feeding and unload at end still apply.
- **Soft filament** - TPU95A, TPU+, PVA+, Flex and other soft grades are treated as soft, so unload at end stays off for them.
- **Spool records** - when the U1 resets a head's record after filament leaves it, the spool you set is written back while it stays in the feeder, and no write lands during a tag read or a calibration.
- **Dirty-bed pauses get their own prompt** - the U1's pause reason now reaches the screen, so a dirty bed is no longer treated like a runout. A print start no longer sends an input shaper calibration flag.
- **No more console flood on connect** - the filament temperature table is read from the printer's files instead of printing tens of kilobytes into every console.

**Printing and stability**

- **Recovery restarts** - a second Restart Klipper or Firmware Restart is refused while one is still running, a restart that ends in an MCU shutdown no longer blocks the Firmware Restart it needs, and a refused tap keeps the recovery dialog open.
- **Switching printers from Home could crash** - the switch now waits for the screen to finish what it was doing.
- **G-code with Windows line endings could freeze the screen** while it was scanned for layer information.
- **Pre-print countdown** - options turned off on print details no longer count toward it, and adaptive meshes count the grid stock Klipper actually probes.
- **Jogging alongside other frontends** - jogs are no longer let through a bed mesh or other long command started from Mainsail or Fluidd, and still work during the app's own homing.
- **Taps** - slow taps on folders and Home tiles register, and releasing a long press on a macro, file card or speed tile no longer also taps it.
- **WiFi switch** - takes the first tap, stays put while the radio changes, and finishes the change even if you leave Network Settings.
- **Power tiles** keep updating after a Klipper restart.
- **Fireworks screensaver** - shells kept climbing without bursting after a few minutes.
- **Input Shaper results** - the comparison table fits its rows, the legend no longer hides behind Save, and the filled curve draws without stripes.
- **Long values in info rows** wrap inside the row instead of clipping or breaking the label.
- **Printer Manager** - the build volume chip leaves out endstop overtravel, and Moonraker's version shows on installs whose server reports none.
- **Creality Nebula Pad** - a retrofitted Ender is offered as an Ender instead of a Creality Hi.
- **Connection tests say why they failed** - the setup wizard and Change Host tell a timeout from a refused port.
- **Label printing over IPP** (#1822) - printers that answer "100 Continue" or use a different print path, such as Canon PIXMA, print again.

**Install and updates**

- **Installer on Creality K2** - finds the `moonraker.conf` Moonraker actually loads, sees Klipper and Moonraker on k2-improvements, and leaves a k2-improvements WebRTC camera alone.
- **Installer** - the KIAUH extension goes into the right user's home, a running KlipperScreen or log tail is no longer mistaken for Klipper, a Moonraker running as another user can load the plugin, and uninstall removes the updater entry from included config files too.
- **Update channel** - switching channels updates the updater entry wherever it lives, including an included file.
- **Z-Mod restarts** - the screen no longer disappears when Z-Mod restarts it on a Klipper start.
- **Black screen at startup** on boards with a display card but no libdrm, and on EGL displays, now shows the splash.
- **K2 debug bundles** keep much more of Klipper's log.
- **BTT K-Touch** - shows Klipper, Moonraker and app versions, and its time zone is set from your phone during setup.

**Android app**

- **Printers on Wi-Fi with mobile data on** - connections to printers on your network go over Wi-Fi, while updates and everything else still use whichever network reaches the internet.
- **Screen scale on high-DPI phones and foldables** - the UI is no longer oversized, and it rescales when a foldable opens or closes.
- **Reported as Android** - telemetry and crash reports no longer count phones as Raspberry Pis.

## [1.0.6] - 2026-10-21

<!-- whatsnew
- No more frozen screen when Wi-Fi hangs, and fixes for crashes on reprint and camera stop
- Filament systems recover from a Klipper restart instead of sticking on busy
- Snapmaker U1, K2 Plus CFS, ACE and AFC fixes: resume, tool changes, box readings
- Runout recovery heats the nozzle first; Cool Down turns off every toolhead
- Updates and installs that failed on K1 and systemd printers now work, and say why
-->

1.0.6 is a maintenance release, mostly fixes ported from 1.1 that a 1.0 user would run
into. The main themes:

- **A screen that stays responsive.** A Wi-Fi driver that stops answering can no longer
  freeze the touchscreen, and crashes on reprinting from History and on stopping a camera
  are fixed.
- **Recovering from a Klipper restart.** A load, unload or tool change caught by a
  restart no longer leaves the filament system stuck on busy, and a second restart can't
  be fired while the first one is still running.
- **Filament systems doing what you asked.** The Snapmaker U1 resumes after a slot edit
  and finishes its unloads, a K2 Plus CFS on community firmware shows each box's own
  readings, ACE feed assist works on the Kobra-S1/ACEPRO driver, and AFC toolchangers
  pick the right head.
- **Safer runouts and cool-downs.** Runout dialogs heat a cold nozzle before Load,
  Unload or Purge, and Cool Down turns off every toolhead on a toolchanger.
- **Updates and installs that work, or say why not.** Updates install on systemd printers
  that run HelixScreen as root, the installer works on a K1 and on hosts with systemd,
  and a failed update shows its reason.
- **A complete Android app**, with every filament system, screensavers and label printing
  built in.

### Added

- **Tap a toolhead to switch tools** - on printers with more than one tool, tapping
  another tool's head on the filament system's screen offers to switch to it. A new
  **Confirm tool changes** setting (Settings > Safety, on by default) decides whether the
  home screen's tool switcher, the filament screen's toolheads and the Filament screen's
  tool list ask before the printer moves. A paused print always asks, and a running print
  refuses the change.
- **FLSUN T1 Pro detection** (#1781) - the T1 Pro is recognized and gets its own preset.
- **SUNLU ABS and ASA in the filament catalog**, with SUNLU's published temperatures.

### Changed

- **Anonymous telemetry** (only when you have opted in) reports usage as the change since
  the last report, keeps the unsent part across a power cut, and records why an update
  failed.

### Fixed

**Stability and recovery**

- **The screen no longer freezes when Wi-Fi stops answering** - Wi-Fi scans, joins, the
  radio switch and status checks now run in the background, so a hung Wi-Fi driver can't
  lock up the touchscreen or drop the UI mid-scan. If the Wi-Fi service stops responding,
  Network Settings says **WiFi isn't responding**, keeps the last network list, and checks
  again on its own until the service recovers. Forget opens one dialog per tap. (#1023)
- **A Klipper restart ends a stuck filament operation** (#1832) - a load or unload that was
  in progress when Klipper restarted no longer leaves the filament system busy.
- **Firmware Restart and Restart Klipper refuse a second restart** (#1832) - tapping
  restart again while Klipper is still coming up could leave it halted. The second tap now
  says a restart is already running and leaves the recovery dialog open to retry from.
- **Crash reprinting from History** - reprinting a job could crash the app (seen on a
  Creator 5 Pro).
- **Crash stopping a camera on Android** - the app no longer aborts when a camera stream
  takes too long to stop after switching printers.

**Filament systems**

- **ACE feed assist and manual feed/retract work on the Kobra-S1/ACEPRO driver** (#1821) -
  the Feed assist toggle and the manual feed and retract buttons failed with "Must specify
  either T=<tool> or INSTANCE=<inst> INDEX=<slot>". They now name the ACE unit and its slot.
  ValgACE and the native driver are unchanged.
- **AFC toolchangers load and change to the right toolhead** (#1824) - Load and tool change
  named a toolhead by position, so on setups where AFC's tool numbers don't follow extruder
  order (a Snapmaker U1 with a ViViD, for example) they picked the wrong head. They now send
  `CHANGE_TOOL LANE=` and `T<n>` and let AFC choose the head.
- **AFC toolchanges keep their progress** - the brief Idle AFC reports between the unload
  and load of one tool change no longer clears the operation status or restarts the step
  progress partway through the change.
- **Large AFC setups** - every lane of a rig with up to 64 lanes is shown and tracked;
  slots past 20 no longer grey out.
- **Runout dialogs heat a cold nozzle first** - Load, Unload and Purge from a runout dialog
  failed on a cold nozzle with "Extrude below minimum temp". They now heat to the
  filament's temperature first, and cool back down after an idle operation.
- **K2 Plus CFS on community firmware** - box temperature and humidity show on the box
  that reported them, survive slot edits instead of disappearing, and a material changed on
  the box reaches the slot instead of being covered by the first material seen. A color
  change is still recognized as a new spool when the firmware switches format.
- **Snapmaker U1: RESUME is no longer refused** after editing a slot - edits write the
  head's firmware filament record the way the printer expects, and heads the Extended
  Firmware had locked are repaired once, outside a print.
- **Snapmaker U1: tag readings** - a spool with no tag clears what the previous tag
  wrote, and leaves a slot you set by hand alone.
- **Snapmaker U1: unloads finish** (#1832) - an unload or a spool-insert preload no longer
  leaves the filament system stuck on busy.
- **Snapmaker U1: soft filament is saved as soft** - TPU, TPE, PEBA, PVA and Flex filament,
  including grades the printer has no entry for such as TPU-85A, TPU95A or PVA+, are now
  saved to the printer as soft filament.
- **Snapmaker U1: no phantom heater** - the U1's extruder offset calibration module is no
  longer listed as a heater, so it stops appearing as a Hotend Heater choice in the setup
  wizard, and a saved hardware list that named it no longer warns that a heater went missing.
- **Snapmaker U1 with multiACE** (#1426) - the U1's own filament system stays in use when
  an ACE is also reported.
- **AD5X: colors set by other tools** - a lane record written by another tool no longer
  pins its color and material, so later color changes from the printer show again.
- **Tool changes check again at Confirm** - a tool change decides whether it can run when
  you confirm it, not when the dialog opened.
- **Filament path values stay in range** - slot counts, path positions, progress and spool
  fill levels above their maximum are now held at it, so a very large lane count can't draw
  past the end of the filament path.
- **Edit overlay label** - a slot that names its spool reads Brand · Line · Material
  without repeating itself.
- **The AMS sidebar's Unload button keeps working after switching printers.**

**Printing**

- **Cool Down turns off every toolhead** (#1768) - on toolchangers the active tool stayed
  hot while the toast said Heaters off. The default Cool Down now turns off every extruder
  and the chamber heater; a customized macro runs as written.
- **Snapmaker U1: a dirty-bed pause is not a runout** - a pause for a dirty bed no longer
  opens the runout guidance, and the print screen shows the printer's own reason. Runout is
  judged only by the lane being fed.
- **Resume refusals explain themselves** - when the printer refuses to resume for a known
  reason, the screen says why instead of showing the raw error.
- **Print Tune Z-offset hint** - the hint says "auto-saved by firmware" only where the
  firmware really keeps the offset (AD5M / AD5X on ZMOD), and on the U1 that a baby step
  lasts until the next tool change.
- **Pre-print progress on AD5M Pro with Forge-X 1.4.2** - the nozzle cleaning, bed mesh
  and priming line are named, the cool-down before probing is no longer called Heating
  nozzle, and the time estimate holds through it.
- **Snapmaker U1 pre-print** - tool Z touches read as Probing Z.
- **Adaptive bed mesh progress** counts the grid Klipper actually probes.
- **Exclude object works for non-English model names** - objects named in Chinese,
  Japanese and other non-ASCII text can be excluded mid-print.
**Screen and controls**

- **Z-Mod color pickers** (#1806) - color prompts from Z-Mod, Fluidd and Mainsail show
  each button in its color, and a picker with many colors is a grid of tappable swatches
  instead of one row of slivers.
- **Long macro prompts fit the screen** - text and buttons scroll above the footer, and
  prompts with five or more buttons use a grid.
- **ESC closes the spaghetti, runout and plugin-install prompts without choosing** an
  answer for you.
- **The on-screen keyboard keeps the field you tapped visible** when moving between fields.
- **Typing in the middle of a field** no longer jumps the cursor to the end.
- **Waking the screen** - the touch that wakes the display no longer presses whatever is
  under your finger.
- **Settings has a Printer row** - it opens the Printer Manager, which was otherwise only
  reachable from the home screen's printer image widget.
- **Power tiles keep updating after a Klipper restart** - a power device's home tile
  stopped following its device once Klipper restarted, until the home screen was rebuilt.
  Tiles now keep tracking their device across restarts.
- **Barcode scanner for spools** (#1793) - with a barcode scanner configured, scanning a
  spool asks for a scan with it instead of starting the camera and moving the bed.
- **Screensaver setting after switching update channels** - a setting saved by a newer
  release no longer comes back as Flying Toasters.
- **Label printing to IPP printers and CUPS shares** (#1822) - printers that send an
  interim "100 Continue" (such as Canon PIXMA) no longer fail with **HTTP error: 100
  Continue**, and a printer shared through CUPS prints to the queue path it advertises
  instead of failing with client-error-not-found.
- **Print status preview buttons** line up in the corner.
- **Printer detection** - a four-extruder machine or a MIPS host without any IFS sign is no
  longer named an AD5X.

**Updates and installing**

- **Updates install on systemd printers that run HelixScreen as root** - on a K2 running
  systemd-based custom firmware (and any other root install under systemd), Install Update
  stopped at **Error: cannot write the service file in /etc/systemd/system**. The check
  behind that message no longer runs where an update never writes that file.
- **A failed update reopens its dialog** (#1727) - an update that failed after you closed
  the download dialog now shows the error instead of nothing, and the reason for a failed
  download or install is recorded. (#1813)
- **Installing on Creality K1** (#1813) - the installer no longer mistakes the firmware's
  stand-in `curl` for the real one.
- **The installer sees Klipper and Moonraker on systemd firmware** - on hosts such as a K2
  running k2-improvements, the installer warned that Klipper and Moonraker were not running
  when they were. It now checks the running processes and systemd services properly, and
  no longer mistakes a log viewer, KlipperScreen or moonraker-obico for Klipper or
  Moonraker.
- **Update channel changes reach a stanza in an included file** - when
  `[update_manager helixscreen]` lives in a file moonraker.conf includes (such as
  mainsail.conf), the installer now updates and removes it there instead of adding a
  second copy to moonraker.conf.
- **The installer never deletes a mounted scratch directory** when it can't tell whether
  it is a mount.
- **helixscreen.env is read more safely** (#1682) - its settings are read as plain values
  from a known list, and only from a file owned by you or root.

**Android**

- **Every filament system is built in** - a K2 Plus with a CFS showed no filament system
  on Android. CFS, IFS, ACE, QIDI and Snapmaker support are now included.
- **Screensavers and label printing** are included in the Android app.
- **Updates from Google Play** - the app announces only an update Play is actually serving.
- **Settings backups** - the app keeps its rolling settings backups in its own storage
  instead of failing to write any, and an update no longer overwrites themes or printer
  database overrides you saved.

## [1.1.0-beta.10] - 2026-10-18

<!-- whatsnew
The tenth beta of 1.1.

- Plugins can add menu items and dialogs
- A new Edit Spool editor
- Pre-print time estimates you can trust
- A stuck WiFi driver no longer freezes the screen
- Creality K2 custom CFS firmware support
- Pressure advance tuning, pause at a layer, new Home tiles and screensavers
- Android: telemetry, settings backups and a smaller download
-->

The tenth beta of 1.1. Plugins can now add their own items to the app's menus (print details, filament slots, Spoolman spools and their own Home tile) and ask questions in dialogs that look like the app's. Edit Spool is a new editor with real header buttons, fingertip-sized colour targets, a custom colour view, saves that finish reliably and "from tag" badges. The pre-print time estimate now uses the file's temperatures and your printer's own history, a stuck WiFi driver can no longer freeze the screen, and Creality K2 owners on Jacob10383's custom CFS firmware get per-box readings, slot loads and filament mapping. AFC setups grow to 64 lanes, Settings gains a Printer row, the Printer Manager lists everything that was detected, the Android app gets working telemetry and backups, and there is pressure advance tuning, pause at a layer, four new Home tiles and three new screensavers.

**Upgrading from beta.9?**

- **Snapmaker U1 unload at end of print:** HelixScreen now sends your choice with every print it starts, so a different choice made from another app between prints is replaced by HelixScreen's.
- **Filament names:** a slot's Spool Info is now Edit Spool, and the filament features are named Endless Spool, Filament Mapping and Assigned Tool everywhere.
- **Tool changers:** Load and Unload no longer mount or park a tool. Mount and Park are their own buttons in the sidebar and the slot menu.
- **Maintenance Meter:** it is now a built-in Home tile that takes over the example plugin's meters and intervals the first time it runs. You can disable the plugin once the tile is placed.

### Added

**Plugins**

- **Plugin menu items** - an enabled plugin can add its own items to the print details menu (the dots button), to a column of their own in a filament slot's menu, to the Spoolman spool menu, and behind the gear on its own Home tile in edit mode. Each item gets the file, slot or spool the menu was opened on, and can hide itself or show greyed out with a reason. A plugin only adds rows: the app's own items stay as they are, and Settings > Plugins lists which menus a plugin adds to, before you enable it and on its row.
- **Plugin dialogs** - a plugin can ask a question in a dialog that looks and closes like the app's own, sized for your screen. A plugin dialog or screen that arrives while an error, a warning, the print-complete dialog or the lock screen is up waits its turn. Calibration Bridge now asks which result was best this way.
- **More widgets per plugin** - a plugin can build up to 32 widgets.

**Filament systems**

- **A new Edit Spool editor** - tap a slot and choose Edit Spool for one screen with the filament's colour and remaining weight, the temperatures for what you picked, and Material, Variant, Brand and Product lists that narrow each other down, with the full catalog one tap away. Spoolman's spool list and QR scan are round buttons in the header, Save is in the header too, and Back with unsaved changes asks first. Colour swatches are at least 7 mm across, and the colour view puts the presets and a custom colour picker side by side where both fit. On systems that read a spool's tag, a "from tag" badge marks each value the tag supplied, and Reset to tag puts them back. A save still talking to Spoolman finishes even if you leave, and a save that only partly succeeds says which part did not.
- **Creality K2 custom CFS firmware** - on Jacob10383's K2 Plus community firmware and its CFS module, loads pick the slot with the module's own slot selection, each CFS unit shows its own temperature and humidity, and a unit that stops answering reads Disconnected until it comes back. With a module on API revision 3, the Filament Mapping you choose when starting a print is sent to the CFS just before it starts; a mapping that points at a slot the CFS cannot feed stops the print from starting and says why.
- **Snapmaker U1 with multiACE** (#1660) - the ACE units behind each head load, swap, unload and dry from the filament screen, and bay spools can be edited.
- **Colour swatches show the filament** - multi-colour spools draw split into their colours, and silk, matte, marble and sparkle filaments draw with their finish, on spools, slots, the tool switcher, the filament mapping chip and the temperature graph.
- **Happy Hare 4: dry only the lanes you choose** (#1026, beta) - on a unit with a heater per lane, the drying screen shows a chip per lane; clear some to dry the rest, turn on Rotate to have an eSpooler turn the spools, and a badge on each spool shows its drying state.
- **Happy Hare 4: dry to a humidity** (#1755) - the dryer controls take an optional humidity target; leave it blank for the printer's default.
- **AFC: Calibrate Toolhead** - on AFC versions with the guided toolhead calibration, the Toolhead settings start with a Calibrate Toolhead button that walks you through it on screen.
- **Tap a toolhead on the filament path to switch to it** - on the U1, tool changers, and AFC or Happy Hare in toolchanger mode. A new **Confirm tool changes** setting (Settings > Printing, on by default, shown only on multi-tool printers) decides whether that tap, the Tool Switcher and the Filament screen's tool list ask first. A paused print always asks.
- **One head picker for Load and Unload** - Load opens the head picker on every printer, Unload is one tap on a single head, and a disabled action says why.
- **Catalog additions** - Eryone filaments, SUNLU ABS and ASA, and recycled grades filed under their polymer as the Recycled variant.

**Printing**

- **Pressure advance tuning on a live test print** (#982, beta) - Advanced > Calibration > Pressure Advance Tuning prints a small tower and raises pressure advance with height; tap - or + to take over, then pick the band that looked best. The result can be saved to the spool's Spoolman filament and is applied whenever that filament becomes active. Print Tune gains a live pressure advance row.
- **Pause at a layer** (#1703, beta) - tap the layer count on the print screen to pause at the next layer or one you pick, with the scheduled layer shown beside the count and as a tick on the progress bar. Needs the `SET_PAUSE_NEXT_LAYER` / `SET_PAUSE_AT_LAYER` macros and a slicer that reports layers.
- **Sliced .3mf files print where the printer cannot read them** - the file list shows a .3mf's plate picture and details from inside the archive, a plate stepper picks the plate, and Print sends that plate as G-code. The HelixPrint plugin converts uploaded .gcode.3mf plates on the printer, and the print status screen previews what will print.
- **Chamber not heating prompt** - when a print waits on a chamber that is not heating, or on a chamber target the printer refused, a prompt offers Try Again, Keep waiting or Cancel print.
- **Snapmaker U1 bed check** - the U1's dirty-bed pause gets its own prompt (Restart print, Cancel print, Turn off bed check), and Settings > Safety & Alerts gains a Bed Clear Check switch.

**Home**

- **Job Queue tile** - the next job's name, how many wait behind it, and a **Bed clear · Start next** button while the printer is free; larger sizes add its picture, time estimate and the jobs behind it. Start next runs the same checks as the file view. Needs Moonraker's `[job_queue]`.
- **Print Stats tile** - the hours printed this week with a bar per day, and lifetime prints, hours and filament where there is room.
- **Printer Message tile** (#1125) - the line your macros show with `M117` or `SET_DISPLAY_TEXT`, with or without a print running.
- **Maintenance tile** (#1807) - HEPA filter, nozzle and linear rail hour meters with a due notice, and a detail screen to reset each meter or change its interval.
- **A new screen copies the printer's own home layout** - when the printer runs HelixScreen on its own display, other screens that connect copy that layout the first time, and the Widget Catalog has a From Printer button to do it again.
- **Humidity drop** - humidity readings on Home, the filament screens and the Dryer tile draw a drop filled to the reading, gently moving when animations are on.

**Printer**

- **Settings > Printer** - opens the Printer Manager, so the printer's name, picture, model, software and detected hardware are one tap away without the Home printer picture.
- **Printer Manager lists everything that was detected** - the Capabilities section grows from 18 chips to over 40, grouped by area, with details (Probe · Beacon, CoreXY, Tool Changer · 4 Tools, 2 MCUs). Capabilities forced on in settings say "set manually", and each chip opens the screen that uses it or a short note on what it is.
- **The K2 picture shows an attached CFS** - a Creality K2 with a CFS connected shows the CFS on top, switching on its own as it reports in or drops out. A picture you picked yourself stays.
- **Jog-tab layouts** (#865) - Settings > Printing > Motion > Layout offers the jog pad, Arrows (the Mainsail arrangement), Arrows + Home (the Fluidd arrangement) and Axis Rows.
- **Save Z-Offset can be turned off per printer** (#1766) - **Save Z-Offset Buttons** in Settings > Motion. It starts off on tool changers, where one machine-wide save would shift every tool's offset.

**Screensavers**

- **Three new savers** - Fireflies (glowing swarms chasing through the dark), Aqua Surface (slow-motion water with a ring clock) and High Fly (a flight over sunlit clouds with a thin clock). 3D Pipes now looks like the Windows original, and 3D Maze gains loops, rooms, distance fog and your recent prints framed on the walls.
- **Flip Clock is the new default** - on fresh installs. A screensaver you already chose stays.
- **Screensaver after** - screens without a dimmable backlight, such as the Android app, offer Settings > Display > Screensaver after instead of always waiting 10 minutes.

### Changed

- **Tool switcher** - the Home tile fits up to six heads, Park and the feeder in one row, the picker is a grid of the same segments, each head shows its filament colour and material, and a tool changer fault marks every head but the mounted one. Default layouts give tool changers the Tool Switcher.
- **Snapmaker U1: unload at end of print sticks** - turn it on per toolhead under Print Behaviour and it applies to every print, including Print Again and a restart from the beginning. A head with soft filament such as TPU is never unloaded.
- **Snapmaker U1 picture** - a new top-down view.
- **RFID spools on the U1** - inserting a tagged spool no longer opens the editor; an untagged one still does.
- **Spoolman icon** - the real Spoolman logo, everywhere it appears.
- **More room on small print file screens** - the sliced-colors switch is a palette button on the 3D preview, options are toggle pills, and four options fit at 480x320 without scrolling. Small portrait screens show every option.
- **2D preview shades creases** (#1314) - outer walls darken where the surface folds, so ledges and rims read as edges.
- **Plural forms** (#1808) - counts use each language's own plural forms, including Russian's.
- **Buttons fit their translated labels** (#1307) - rows of buttons share room so a long label takes space from short neighbours instead of clipping.

### Fixed

**Print preparation**

- **The prep time estimate on print details** - "~6 min prep time" counts heating up to the file's own first-layer temperatures and matches the countdown once the print starts. It learns from how long your printer actually took, cold bed and warm, and turning a pre-print option on or off moves the estimate only by what that step really costs.
- **Estimates no longer balloon** - the estimate could jump to two or three times the real preparation in the bed's last slow degrees.
- **A heater wait names its heater** - the label follows the waiting command (M109, M190, M191 or TEMPERATURE_WAIT), so a cool-down before probing no longer shows the wrong heater.
- **FlashForge AD5M on Forge-X 1.4** - preparation follows the mod's own steps through to the first layer, instead of finishing at the nozzle clean.
- **Snapmaker U1 pre-print steps** - the Z touch after each tool pick reads Probing Z, and the bed soak and mesh steps get their own labels.

**WiFi and stability**

- **A stuck WiFi service froze the screen** - every scan, status check or network tap could hold the screen for up to 15 seconds. WiFi now runs in the background; after a few slow answers the screen shows "WiFi isn't responding", keeps the last list on screen, and checks again on its own until the service answers.
- **Klipper that failed to start looked shut down forever** - the screen now asks until Klipper is ready or reports its error, the recovery dialog shows Klipper's own message, and a Firmware Restart that does not bring Klipper back runs the platform's recovery script once, with a time limit.
- **Switching printers could crash** - the favourite macro colour dialog crashed after a live switch, and the AFC fault diagram lost its labels.
- **Reprint from Print History could crash** (#1815) - it now keeps its own copy of the file name.
- **Display resize and shutdown crashes** (#1809, #1810) - the resize timer and teardown no longer touch freed screens.
- **E-stop stayed dead** (#1795) - after a dismissed confirmation. An open dialog or the PIN pad now covers the navbar, E-stop included.
- **Firmware errors** - U1 shutdown reasons show as text instead of raw JSON.

**Filament systems**

- **AFC: up to 64 lanes** - a large AFC or Happy Hare setup shows every lane instead of greying out the extra ones.
- **AFC toolhead sensors showed up as runout switches** - they are left to AFC now, since toggling one could tell AFC a tool was unloaded.
- **The load step list skipped its first steps** - after a tool change from a macro or the console, the next load no longer starts with its early steps already passed or showing the last "Load complete".
- **Long printer prompts pushed their buttons off the screen** - a prompt with many buttons, such as AFC's calibration asking which unit, scrolls while Cancel and Back stay on screen, and five or more buttons form a grid.
- **Cold nozzle loads** - Load, Unload and Purge from the Home tile, the runout dialogs and the external spool menu heat a cold nozzle first instead of being refused.
- **Selecting a parked hub-fed head swapped its filament** (#1811) - the Tool Switcher keeps the lane already loaded in it.
- **Snapmaker U1 Resume refused with "not edit filament"** - a material set on the screen is written to the U1's own filament record, and a refused resume says "Filament not set" with what to do.
- **Snapmaker U1 runout** - a preloaded feeder on another head no longer turns a dirty-bed pause into the runout guidance.
- **Print Tune no longer says the U1 Z-offset is auto-saved** - a baby step lasts until the next tool change or the end of the print.
- **AD5M: Print Label did nothing** - the button is hidden in the Spoolman editor, which has no label printing there.

**Display and settings**

- **Light taps said the printer was busy** - quick taps on the lights no longer bring up "Printer is busy"; the message appears only when something else keeps the printer busy.
- **Screensaver settings** - builds without screensavers hide the screensaver rows and Test button, and the dim and sleep rows follow one rule.
- **Picking a theme froze the screen for two seconds** - each pick now draws once.
- **Leaving the theme explorer kept the previewed mode** - leaving without Apply puts back the theme and mode you had.
- **Nord theme** - an old copy that hid the current Nord and was rewritten on every update is removed; a Nord you edited is kept.
- **The waking touch pressed what was under it** - the touch that wakes the screen now does nothing until the finger lifts.
- **Small colour targets** - every colour you can pick has a fingertip-sized touch area, and neighbours no longer steal taps.
- **Fans in the Printer Manager** - the Fans chip never appeared; it now shows when the printer has controllable fans besides part cooling.
- **Exclude objects with non-Latin names** - Chinese, Japanese and other model names can be excluded, and names the font cannot draw show as Object N.
- **On-screen keyboard** - the field you type in stays above the keyboard when focus moves.
- **Small and portrait screens** - the Controls screen, Print Tune at 272x480, the filament dryer controls and the printer picker fit their space.
- **`HELIX_SCREEN_SIZE` left no trace in the log** - the size applied, or the warning for a bad value, is logged now.

**Install and platforms**

- **Shutdown and Reboot from the screen** - work when HelixScreen runs as a regular user.
- **Settings after a web update** - user config and uninstall records survive a Moonraker web update on SysV and procd platforms.
- **Self-update downloads** (#1813) - a curl stand-in is no longer mistaken for real curl, and a failed download records why.
- **`helixscreen.env` settings** (#1682) - the launcher accepts every setting the docs tell you to set.
- **QIDI Q2** - shares the Plus 5 and Max 4 chamber heater, sensor and aux fan presets.
- **BTT K-Touch** - more of the app's data moved out of the ESP32's internal RAM, keeping room for the WiFi driver.

**Android app**

- **Settings were never backed up** - the app now keeps its rolling backups in its own storage, and Android's backup takes just your settings.
- **An update could overwrite your own files** - an update now only adds missing files.
- **Telemetry never sent** - it now sends when turned on, as on every other platform.
- **A smaller APK** - test prints and font sources are left out, saving over 200 MB, and an update removes the copies an older version unpacked.

## [1.1.0-beta.9] - 2026-10-15

<!-- whatsnew
The ninth beta of 1.1.

- New Home tiles: toolhead, Z offset, speed and flow, dryer, host health, bed mesh, exclude-object map
- Tool switcher rebuilt for toolchangers, with Park
- Four new screensavers
- Redesigned filament screens, including portrait
- QIDI Plus 5 support, plus a desktop Linux build
-->

The ninth beta of 1.1. Home gets a batch of new tiles (toolhead, Z offset, speed and flow, dryer, host health, bed mesh and the exclude-object map) and default layouts that now adapt to the printer and screen size. Toolchanger owners get a rebuilt tool switcher with Park. There are four new screensavers, the filament screens are redone for small and portrait displays, the QIDI Plus 5 is supported, and a windowed build for Linux desktops exists.

**Upgrading from beta.8?**

- **Screensaver:** the setting now stores the saver by name. Your old choice is carried over automatically. A saver too heavy for your board is hidden, and a lighter one runs if your stored choice was one of those.
- **Home layout:** an unedited default layout re-resolves when your printer's hardware changes. If you have customized Home, your arrangement is left alone.

### Added

**Home**

- **New Home tiles** - Toolhead (homed state, axis positions, Z bar, Home All), Z Offset (value with Save and baby-step buttons), Speed and Flow, Dryer (box, chamber and bed drying with progress), Host Health (status color, sparklines, MCU rows, disk free), Bed Mesh (heatmap, color scale, Calibrate and Load profile) and Exclude Objects (a mini map from the Column size up that opens the full map). Each tile picks a layout that fits the room it is given.
- **Default layouts adapt** - the default Home layout is built per screen size, takes the tiles your printer actually has, leaves clean gaps instead of filler, and keeps notifications in a corner. It updates itself if you add hardware, until you edit it. The 1920x1080, 1080x2400 and Creality Hi's 320x240 and 320x1480 screens are fully covered.
- **Chamber light from the printer image** - tap the bulb chip on the printer image to switch the chamber light.
- **Pre-print progress shows it is alive** - the spinner is gone; while a print is preparing, a soft highlight sweeps the unfilled part of the progress bar, on the Print Status screen and the Home tile, so a long heat-soak never looks frozen.
- **Half-cell icon tiles on small screens** - on 480x272 and 480x320 panels, icon-only tiles (light, notifications, lock, firmware restart and the like) shrink to half a cell and drop their caption.

**Toolchangers**

- **Tool switcher rebuilt** (#1784) - a segmented strip that keeps every head a finger-sized target, names each head by its G-code number, shows a detail block when there is room, and can Park the mounted head. A paused print still offers a head change, behind a confirmation. A configured tool name shows as its label next to the G-code name (#1769).

**Screensavers**

- **Four new savers** - Hyperspace (a flight through rainbow bands), 3D Maze (a Wolfenstein-style walk through a maze), Flip Clock (each digit flips on its own card, and it counts down a running print) and Bouncing Logo (the HelixScreen logo as the bouncing sprite). Fireworks gain palm, crossette and strobe shells.
- **Heavy savers step aside** - a saver too heavy for the board is hidden and a lighter one runs in its place. The setting remembers the saver by name.

**Filament systems**

- **Filament screens redone** - the loaded spool leads its card as a strip, readings sit in tiles or a pill on the box, sidebar actions go two to a row (or text alone on larger screens), labels fit whole in every language, and headers name the filament system. Small screens get a compact unit view, and portrait stacks the diagram over a full-width sidebar with page dots floating on the canvas.
- **Endless spool reads clearly** - groups are drawn as brackets and chains keep arrows, so each hop reads on its own.
- **OpenAMS logo** (#1786) - OpenAMS units show their own full-color logo.

**Printers and platforms**

- **QIDI Plus 5** - detected and supported, with the QIDI Box and the on-device framebuffer.
- **FLSUN T1 Pro and S1** - detected, and both are treated as enclosed.
- **Windowed build for Linux desktops** - an x86 release that runs in a window, with an installer flavour that installs it per user. In-app install is refused there.
- **Round beds** - the bed mesh (3D surface, zero plane, wireframe and 2D heatmap), the Bed tab in Motion and the exclude-object map draw a round plate on delta printers, with a 50 mm reference grid.

**Plugins**

- **`helix.ui.text_width`** (#1774) - plugins can measure text in the live theme font.

### Fixed

**Updates**

- **A board never gets another board's build** - the updater refuses with a message instead, and a Creator 5 will not install a release that has no build for it.
- **Update dialog** (#1727) - reopens when an update fails after the download dialog had closed.
- **Unified MIPS build** (#1789) - finds its download in releases cut before it existed.
- **Google Play installs** - only announce the update Play will actually serve.

**Home**

- **Upgrading a 1.0 Home layout** - a tile that has to grow to fit (Tips, for whole-word titles) grows only into free space and never pushes a neighbour you placed out of its spot; the file keeps the layout you saved.
- **Layout fixes** - the Hi's readout cell takes a CFS first, the printer image keeps a margin, tip titles ellipsize instead of breaking mid-word, tiles refuse sizes that cannot hold their widest reading, and Speed and Flow rows no longer overlap in a 2x1 cell.
- **Temperature card** - rows share columns so values line up.
- **Clock and navbar** - a short clock shows the date wherever it fits (1.5x1 is usually enough), and on 480x272 the E-stop button keeps a margin inside the navbar with the same tap target.
- **Tile glyphs** - a tile with its caption hidden draws the same glyph size as its neighbours.

**Toolchangers and filament systems**

- **U1 and Snapmaker tool changes** (#1784) - a parked head is no longer the active tool, and the empty carriage stays empty through rediscovery. Parking refuses a paused job.
- **Tool numbering** - a tool's slot is its Klipper tool number, not its position in a sorted name list.
- **AD5X IFS** (#1324) - a failed load or unload macro puts the system in an error state, and unload names the lane even when no live state does. A load preheat shows the nozzle temperature once.
- **CFS** - the box temperature and humidity show on the box that reported them.
- **AMS** - the lane count label no longer runs under the unit card's environment box, and AMS reads the system once per change instead of repeatedly (#1777).

**Setup and sensors**

- **Printer type mismatch** - a mismatch now shows a toast and a history entry instead of a boot modal, and the review is per warning.
- **First-boot presets** - a preset applied at first boot sets filament sensor roles in the same session, and every preset apply reloads filament, probe and width sensor config.
- **Barcode scanner** (#1793) - a configured scanner skips the camera and bed move.
- **Camera** (#1788) - the Moonraker webcam list is read on builds without a camera widget, and loopback camera ports are probed on every printer except the FlashForge MIPS boards.
- **Plugin install on K1** (#1790) - finds a Creality K1 Moonraker and logs why an install fails.

**Display and text**

- **Text input** - typing mid-string no longer jumps the cursor to the end.
- **Long labels** (#1775) - dotted labels stay on one line and long macro descriptions wrap.
- **Button labels** - multi-word button labels use Title Case.
- **Creality K1** - a release build finds its panel's DPI.
- **Creator 5 and Pro** - skip the backlight call and get FlashForge scroll tuning. The AD5X touch fit no longer applies to a Creator 5.
- **Exclude-object map** - draws the bed grid and whatever of the edge is in view.
- **Firmware Restart** - has its own icon everywhere it appears, and the Lock Screen tile always shows a lock, greyed out until a PIN is set.

**Stability**

- **Memory leaks** (#1279) - fixed in grid layout, icon grid cells, split buttons, the camera decoder and the power device teardown.
- **Wi-Fi** (#1293) - the wpa monitor socket is closed once, after detach.
- **Crash marker** - one that cannot be removed no longer kills the app.
- **Sleep** - a sleep controller destroyed while asleep removes its overlay.
- **3D G-code viewer** (#1742) - makes its context on the display's EGL display, and the crash guard covers driver init.
- **K-Touch** - home widget data moved out of scarce internal memory.
- **Display brightness re-assert** - its timer is cancelled when the display shuts down, so it can no longer fire on freed memory.

**Telemetry**

- **Board reporting** - crash reports and telemetry carry the resolved board next to the platform.
- **More accurate statistics** - every event carries one envelope (version, platform, board, boot id), usage snapshots send deltas that survive a power cut or a full queue, overlays report a stable id, and every error category reaches the server.

## [1.0.5] - 2026-10-13

<!-- whatsnew
- Plugin installs work on Creality K1-series printers
- The QR scanner finds cameras Moonraker lists, and a stock K1's built-in camera
- Creality CFS auto-refill shows and sets the right state
- Update failures say why, before downloading
- Fix for repeated 3D G-code viewer crashes on some GPUs
-->

### Fixed

- **Plugin installs on Creality K1-series printers** (#1790) - the installer now finds
  Moonraker where a K1 keeps it, and when an install fails the reason is logged instead of
  discarded.
- **QR scanner finds the camera on more printers** (#1788) - builds without the camera
  widget (K1/KE, AD5M, CC1, K2, U1) now read Moonraker's webcam list, and a stock K1's
  camera on 127.0.0.1:8080, which Moonraker doesn't list, is found by probing. AD5X is
  excluded from the probe because its kernel crashes when a streamer is woken.
- **Creality CFS auto-refill** - the setting is read from the box's first status frame, so
  it no longer shows Off until a re-query, and the toggle sends the value you pick.
- **Update failures explain themselves** (#1778) - when an update can't be installed (for
  example, a full root overlay), the dialog shows the actual cause instead of a stray line of
  advice, and the space check runs before the download.
- **3D G-code viewer crash on GPU driver start-up** (#1742) - a crash while the GPU driver
  initializes now falls back to the 2D viewer on later opens instead of crashing every time.
- **Wi-Fi shutdown closes its event socket once** - stopping Wi-Fi could close a file
  another part of the app had just opened.
- **Power device teardown** - the power device list stops reacting to print state once torn
  down, and no longer leaks on every reconnect.

## [1.1.0-beta.8] - 2026-10-11

<!-- whatsnew
The eighth beta of 1.1.

- Four new example plugins and a plugin tutorial
- Plugin translations and plugin-controlled settings
- Creator 5 on Z-Mod: Load feeds the head, plus an unload notice
- OpenAMS dryer, environment readings and fault recovery
- AMS unit view pages through every unit
- K2 chamber targets at the resting value show as Auto
-->

The eighth beta of 1.1. Plugin authors get four new example plugins, a step-by-step
tutorial, per-plugin translations and a way for a plugin to drive its own settings, and
plugin calls to Moonraker now go through the same safety checks as the app's own. The
Creator 5 on Z-Mod can feed a head from the screen, ACE parked slots are drawn at the hub,
OpenAMS units show their own temperature, humidity and dryer, and a K2 chamber target that
matches the resting value now displays instead of vanishing.

**Upgrading from beta.7?**

- **Plugin authors:** a translation pack can no longer replace a string the app already
  translates, and a pack that declares `en` is skipped (the English text is the tag). An
  edited pack takes effect on the next app start.
- **Creator 5 on Z-Mod:** Unload shows a cut-and-pull notice instead of running. Mounting a
  head without feeding is Select slot in the slot menu.

### Added

**Plugins**

- **Four new example plugins and a build tutorial** - Maintenance Meter (consumable hour
  meters on a Home tile, with confirm-guarded resets), Print Watcher (print start, finish and
  failure notifications to ntfy, Discord, Telegram or a webhook), Ambient Tile (room
  conditions from Home Assistant or any JSON endpoint, with a sparkline) and Calibration
  Bridge, a proof of concept for a slicer calibration companion that asks you to pick a
  result on screen. A step-by-step plugin tutorial walks through building the first one.
  Plugins can be shared in the Discord #plugin-showcase channel (#1771).
- **Plugin translations** - a plugin can ship an `i18n/` folder with translation packs, and
  its manifest text, XML labels and Lua code (`helix.i18n`) follow the app language. A pack
  only supplies words the app does not already translate, so a plugin cannot change the
  app's own labels; English packs are refused and a pack over 256 KiB is skipped. An edited
  pack needs an app restart.
- **Plugins can drive their own settings** - `helix.settings.set` lets a plugin's own screen
  act as the control surface for the settings it declares. The generated settings screen
  stays in step, and setting a key to the value it already holds saves nothing.

**Filament systems**

- **Z-Mod on the FlashForge Creator 5** (#1779) - Load on a head now feeds that head through
  Z-Mod's afclite shim (`CHANGE_TOOL LANE=E<n>`), heating only that head. The Creator 5 has
  no automatic unload, so Unload shows a notice telling you to cut and pull the filament at
  the head and sends nothing. The firmware's tool-state query is read at connect, a docked
  head without filament shows as Empty, and a fault from the shim opens a dialog in the
  firmware's own words.
- **ACE parked slots** (#1678) - on a Kobra S1 an idle slot the driver reports as parked is
  drawn staged at the hub instead of on its spool.
- **OpenAMS temperature, humidity and dryer** - each unit shows its own temperature and
  humidity, and the dryer can be started and stopped per unit from the environment screen.
  A running dryer stays stoppable.
- **OpenAMS fault recovery** - a latched stop or pause fault on a unit puts the system in an
  error state, marks the unit's slots and opens the recovery dialog, translated. Reset and
  the dialog clear the fault on each unit that offers it. The FPS pressure sensor is drawn as
  a one-sided gauge.
- **Unit view pages through the units** - on the AMS overview, tapping a unit card zooms into
  that unit with arrows, swipe and page dots, and dashed stubs show the rest of its hub. The
  overview draws units, hub, buffer and toolhead for every printer, and up to 16 units.

### Fixed

**Plugins**

- **Plugin Moonraker calls get the app's safety checks** - a plugin with write access could
  start a print while the spool latch was up, or with an unsafe path, and could send restarts
  and G-code the app itself refuses. Plugin calls now get the same refusals as the app's own.
- **Plugin runtime** - a plugin's settings save no longer counts against its time budget, a
  language-change handler can register another handler, and a plugin drawing at its memory
  cap no longer gets disabled on its next start.
- **Example plugin fixes** - Maintenance Meter counts a print already running when it loads
  and stops counting while offline. Print Watcher never shows the destination's secret, and
  ignores boot-time states. Ambient Tile masks credentials, shows dashes for missing
  readings and skips overlapping polls. Calibration Bridge keeps its picker on screen when a new calibration
  arrives, asks before Cancel stops a print, and only resets once the cancel succeeds.

**Filament systems**

- **Creator 5 Pro on Z-Mod** - the printer no longer gets the wrong filament system (which
  sent macros the firmware does not have), the swap steps read as Pick up tool and Dock
  tool, and the debug bundle names the board. A four-head FlashForge with no recognised
  firmware is no longer named an AD5X.
- **ACE** (#1678, #1679) - a paused failed feed is no longer drawn loaded to the nozzle, a
  seated tool reads as loaded at the nozzle once the toolhead sensor is made, Endless Spool
  mode changes made at runtime reach the screen, and the tip-handling step label follows the
  language.
- **OpenAMS** - units on one lane share a hub and toolhead, unload no longer refuses with
  several lanes loaded, a spool link written by openams_spoolman after an edit here wins over
  it, and extra slots in a filament group show as endless-spool backups instead of the tool.
  Loading a backup slot loads that slot. Unit fault descriptions are translated.
- **AMS overview** - hub boxes no longer overlap, units past the eighth keep their hub and
  buffer, a unit with no logo shows none, and a ninth unit shows its own humidity.
- **CFS auto-refill** - the toggle sends the setter the fork's firmware accepts, only counts
  as sent once Klipper completes it, and endless spool no longer reads Off after boot.

**Temperatures**

- **Chamber target equal to the resting value on K2** - a deliberate chamber target such as
  35 no longer vanishes; the number displays with an Auto indicator, and the print status
  screen shows chamber status for these targets too. The chamber card also uses the bed
  card's large header when it has no diagnostics rows.
- **Cool Down on toolchangers** (#1768) - turns off every extruder heater, and the chamber
  heater, instead of leaving the active tool at its target while the toast said heaters off.

**Updates**

- **Update refusals say why** (#1778) - a full root overlay now shows the first real error
  instead of its closing advice, and the app refuses before downloading ~60 MB it cannot
  install.

**Stability and speed**

- **Crashes after leaving Print Files and Print Status** - several screens could touch widgets
  freed after a deleted view, including the file detail view, its thumbnail after Back, the
  exclude-object viewer, the clog bar and fan dial, and the print-prep option count.
- **Sound** - the tracker no longer gets freed mid-tick, and a tick callback cannot deadlock
  the sequencer.
- **Faster AMS screens** - the unit view and filament path read the system once per refresh.
- **Faster screen switches and Print Files on ESP32** - Print Files builds at the first idle
  moment after connect, the new screen shows before it activates, the file detail view shows
  its thumbnail, and the first Moonraker connect waits for the boot Wi-Fi thread to exit.

## [1.1.0-beta.7] - 2026-10-08

<!-- whatsnew
The seventh beta of 1.1.

- Redrawn filament path for AMS units, with a dry-box look and spool glow
- Skip bed mesh, QGL and Z-tilt per print
- AFC, OpenAMS, ACE and Happy Hare 4 additions
- Faster startup and Print Files
- K-Touch: boots safe after a crash loop, fits more printers
- GPU rotation on DRM/EGL displays
- K2 filament cuts no longer drive the toolhead into the frame
-->

The seventh beta of 1.1. The filament path diagram was redrawn: hubs, buffers, sensors and
routes follow what each AMS unit really has, and dry-box units get a lid and spools sitting
on the box floor. You can also skip bed mesh, quad gantry leveling and Z tilt per print from
the file details, and AFC, OpenAMS, ACE and Happy Hare 4 each pick up something new. Startup,
Print Files and the Macros screen do less work, the app falls back to a safe layout when it
crash-loops, and the K-Touch gets a run of printer-switching and memory fixes.

**Upgrading from beta.6?**

- **Per-print leveling skips need one macro file.** Install or update from the Macros screen
  stages `helix_skips.cfg` and adds one include line at the top of printer.cfg (one backup is
  made, with a one-time rollback). Nothing changes if your printer has no wrappable bed mesh,
  QGL or Z tilt step.
- **Opening the slot editor on a new spool is off by default.** Turn it on in Settings >
  Printing > Filament.

### Added

**Filament systems**

- **Redrawn filament path for AMS units** - every unit type paints one route from spool to
  toolhead with sensor bands on the tubes, PTFE-style tubes with an accent glow on the active
  route, a hub sized to its tubes, and hub and buffer stacked above the toolhead. Parallel
  and mixed rigs show their routes too, a toolhead sensor band appears only when the unit
  has a toolhead sensor, and the bypass route follows the same error colours as the rest.
  The system overview uses the same painter, the tool label is a badge on the toolhead's
  corner, and one unit of several ends in a short fading output stub. Bands follow the
  sensors each unit actually reports.
- **Lane details on the AMS screen** - a lane that measures its remaining filament length
  shows it under the spool and on the loaded card, and a lane that reports its temperature
  shows it above its humidity. A unit its backend reports offline shows Disconnected, fades
  but stays tappable, and the header keeps its title and chip in view. Device operations
  lists unit firmware and serial, toolchange purge, Spoolman mode and the pending spool.
- **Dry-box units look like boxes** - spools sit on the box floor under a glass lid, with
  humidity and the material label kept clear of the spools. The current spool glows around
  its own outline (no slot box) and pulses during a load.
- **Open the slot editor when filament is inserted** (#1335) - optional, off by default
  (Settings > Printing > Filament). Loading filament into an empty lane, or into a printer
  with no AMS or with bypass engaged, opens the slot or external-spool editor. It stays
  quiet while printing, during a filament operation, and right after startup or reconnect.
- **AFC TD-1 scans in the slot editor** (#1151) - when the lane has a TD-1 scan, the editor
  shows its colour and TD with a Use scan button.
- **OpenAMS temperature and humidity** (#1150) - shown on the unit, taken from the sensor the
  unit's config names.
- **ACE Endless Spool** (#1679) - ACE shows the driver's Endless Spool groups. Only ready
  slots count as swap targets, and materials compare ignoring case and stray spaces.
- **Happy Hare 4 clog detail** (#1755) - the clog bar shows the FlowGuard trip reason or the
  tangle prevention state on one line under the bar. Happy Hare 4's gate vendor shows as the
  lane brand and edits write back.

**Printing**

- **Skip bed mesh, QGL and Z tilt per print** - the file details screen gets a toggle for each
  of these steps your printer's start macro runs, next to the other print options. Needs no
  plugin.
- **Spaghetti detection shows the camera frame** (#1506) - the alert dialog includes the
  latest frame, using the camera widget's source, rotation and flips.

**Settings and setup**

- **Wizard preferences page** (#1336) - a new step before the summary with taste-only toggles
  (UI sounds, and the AMS options when your printer has one) so you do not have to find them
  in Settings later.
- **Safe mode after a crash loop** (#1315) - three restarts inside two minutes now start the
  app with plugins off and the shipped default Home layout, and layout edits are refused so
  the saved layout is not overwritten.
- **LED settings warn about macros that set LEDs** - if PRINT_START or PRINT_END reaches an
  LED command, directly or through other macros, LED settings says so.
- **Rotation on DRM/EGL displays runs on the GPU** - any angle works, touch follows the
  rotation, and a DSI panel with no rotation-capable plane no longer needs the framebuffer
  fallback.
- **Wi-Fi link log** (#1190) - the app records signal and link changes and includes a network
  section in debug bundles, for chasing dropouts.

**BTT K-Touch (alpha)**

- **Falls back off a printer that crashes the panel at boot** (#1750) - after repeated boot
  crashes the K-Touch holds off connecting until you pick a printer.
- **Adventurer 5M and 5M Pro images** are in the K-Touch printer picture set.

**Connection**

- **A moved printer is offered its new address** (#1217) - when a connect fails, one quick
  network scan runs, and if a single Moonraker carries the saved printer's hostname you are
  offered its new address.

**Android**

- **Targets Android API 36 for Google Play** (#1639).
- **The Android Update button opens the GitHub release** for a sideloaded install, instead of a
  Play Store listing that does not exist yet.

### Fixed

**Filament systems**

- **K2 filament cuts no longer crash the toolhead** - a K2 Pro's toolhead hit the frame when
  an unload, swap or bypass cut ran from the purge chute. Every K2 cut now uses the
  firmware's positioned cut, which moves to the cutter first. The K2 debug bundle platform
  is labelled Creality K2, so a K2 Pro no longer reads as a mismatch.
- **AFC unlink survives reconnects** (#1717) - unlinking a spool on a lane that remembers its
  spool no longer comes back after a reconnect or when the lane restates its spool, Clear
  Spool leaves another lane's active spool alone, only the loaded lane sets Spoolman's active
  spool, and an eject keeps the unlink.
- **Toolhead sensor reporting** - AFC decides per unit from its own extruders, repaints when
  the toolhead-sensor answer in the config changes, and units report one only if the
  hardware has one. OpenAMS environment readings
  attach only to a matched unit, and false path error highlights are gone.
- **Happy Hare 4 and ACE** - the clog note follows the language and clears a stale reason
  (#1755), and the spool picker shows each spool's name on large screens.
- **A different slot grid repaints the filament path**, a secondary filament system's
  ams_slot shows that system's tool badge, and the path paints once per open at its final
  size instead of flashing a spinner and rebuilding.
- **The AMS screen no longer acts after it is closed**, and a reopened one keeps a single
  operation stepper (#1761).
- **Switching printers drops unsaved spool changes** - spool assignments are kept per printer,
  survive a switch race, a failed load or an unreadable file, and one printer's spools never
  seed another's.
- **Default ABS preset reads ABS/ASA**, since the two share a temperature range.
- **Path errors show where they happen** - a jammed (BLOCKED) lane reads as an error on every
  screen, a buffer fault tints the buffer box instead of the hub, AFC's buffer fault shows
  as a countdown, and a lane off the active route shows its own error. Filament stuck at
  the hub after a failed unload now shows in the trunk, and Happy Hare's bowden progress
  fills the output tube during a load or unload.
- **Theme colours** - the filament path, the overview and the dry-box tray follow a theme or
  dark-mode switch while open, including tube and overview colours in the light theme.
- **Unit detail layout** - the box, its readout and the tubes below are centered, and the
  remaining length no longer pushes the material label away.
- **AntHead on the K-Touch** draws the default toolhead glyph, since the AntHead image is
  not shipped there.

**Printing**

- **Stray template text in a macro no longer fails a filament operation** - a malformed Jinja
  tag that Klipper reports as an unknown command now shows a warning and the load or unload
  carries on.
- **Pre-print progress** - a purge during filament load counts once, QGL and Z tilt are
  recognised from configured probe points and stationary purges from motion, macros that
  narrate gantry leveling, Z tilt and purging are picked up by the generic profile, and
  Klipper's empty bed mesh reads as no mesh (so mesh clearing is detected on every printer).
- **Print options card** - the PRINT OPTIONS card is hidden when no row would show (a Voron
  whose START_PRINT always runs QGL and mesh no longer gets an empty card), and macro rows
  vanish right away when a reconnect or Klipper restart clears the analysis.
- **Option tiles show on and off clearly** - an unchecked tile has an empty box and a muted
  label.
- **Print status Light button** drives your light rather than the first
  NeoPixel, so a Voron with a BoxTurtle no longer toggles the corner indicators.
- **Starting a modified print on the K-Touch** - starting a print with a stripped step or a
  tool remap no longer fails with an empty "Failed to download G-code" toast; the original
  file prints, and a remap is refused with a reason. The file detail view also skips what
  the K-Touch cannot fetch.
- **Home print card after a cancelled job** - the leftover "Print starting..." message no
  longer shows after a cancel, or after a plugin subscribes to more printer objects.
- **Runout with filament at the feeder** - an autofeed runout where filament is still at the
  feeder now says the motion sensor is stuck.
- **Spoolman QR codes** (#1744) with `web+spoolman:` in any letter case are recognised.

**Printer switching and connection**

- **A switch covers the screen until the new printer connects** - a "Switching to X" card
  shows during the rebuild, then Connecting, and it drops when no connect starts. The
  spinner holds still on limited displays.
- **Printer list and switch details** - a picked printer's confirmation opens after the list
  closes, a printer not yet connected is named by its host rather than an id, renaming starts
  from the saved name, and About shows the display name. Wizard paths record a switch
  away like the picker does (#1750).
- **A real client with no saved host connects to the local Moonraker** instead of an empty
  address.
- **Moonraker restart after a plugin install or uninstall works** - the app now asks Moonraker
  to restart itself, so the UI no longer marks a plugin installed while Moonraker never
  loaded it.
- **Z-Mod is no longer reported on stock Creator 5 and 5 Pro** (#1754), whose part fan is
  named fanM106.
- **Stock U1 SSH starts at boot** when the stock Root Access setting is on.

**Screens and display**

- **Saved brightness applies at startup** - a saved 42% no longer comes up at 100%.
- **Long translations stay inside buttons and labels** (#1307) - print tune speed and flow,
  PID fan hint, filament status line, history stat cards and fan names are limited in width,
  and the print status speed and flow row wraps.
- **480x320 overflow** (#1309) in LED swatches, About paths and the theme preview.
- **Scroll bars and gutters** - the gutter attaches when a list overflows after the screen
  opens, hidden lists skip the late re-walk, and a styled thumb length redraws its whole
  track.
- **Macros screen** (#1748) - a held press is dropped when rows rebind, rows grow when the
  list gets taller, and the scroll position is clamped after the list shrinks.
- **Toasts redraw only the area they land in**, and overlays and panel sweeps leave open
  modals visible.
- **Limited displays skip fades and slides** - overlays and modals open and close without the
  slide, fade and scale, and the loading pill shows only on those displays, translated.
- **Thumbnails** - the old thumbnail is kept while the source switches, every cache writer
  sniffs the real image format, and a file list refresh no longer holds every card's decoded
  image on the K-Touch.
- **A gradient canvas that cannot resize** no longer references the freed buffer.
- **Heavy CJK font loads** read the font in large blocks instead of millions of tiny reads.
- **Framebuffer crash reports** - fbdev crashes resolve to readable
  backtraces.

**BTT K-Touch (alpha)**

- **Four or more printers fit in the saved settings** - the settings file is written compact,
  so a 3-printer K-Touch no longer runs out of flash blocks and fails every save, and no
  pre-migration copy is kept.
- **Materials table works** - preset buttons show their materials instead of `---`.
- **Fewer memory and startup problems** - the first Moonraker connect runs on the UI thread,
  discovery, auto-detect, safety limits and light buttons run as on desktop, WLED discovery
  no longer logs an error on every connect, and large static data moved out of internal RAM.
  Print Files stops holding every thumbnail.
- **Settings bindings the firmware left unresolved** are registered.
- **Add and switch cycles the connection once** instead of twice.

### Changed

- **Startup does less work** - screens' XML is loaded the first time it is needed instead of
  all at boot (#1756), and a printer switch, resize and hot reload keep working with that.
- **Print Files is faster** - cards are built while the listing arrives, a refresh repaints
  only the rows whose data changed, cards carry fewer objects, and the shared gradient is
  drawn 1:1.
- **The Macros screen scrolls with a recycled row list** (#1748), so long macro lists open
  and scroll faster.
- **Scrolling redraws less** - a scroll redraws a scroll bar's thumbs rather than their tracks,
  and rounded corners are only repainted where a scroller draws them.
- **AMS screens stay built between opens**, so reopening them is quicker, and the first open
  shows the loading pill.
- **The K-Touch connect and switch path is shared with desktop**, so one fix covers both.

## [1.0.4] - 2026-10-08

<!-- whatsnew
- Happy Hare v4 support
- K2 filament cuts no longer drive the toolhead into the frame
- USB prints copy the file to the printer first
- Spoolman temperature, spool weight and active-spool fixes
- Fixes for crashes at boot with a labeled USB stick and when closing the macro wizard
- Saved brightness and screen rotation apply at startup
-->

### Added

- **Happy Hare v4 support** (#1479) - Happy Hare 4 rigs now work end to end. Bypass is shown
  only when a unit really has one, per-gate entry sensors read correctly, multi-unit machines
  get the commands they need (home, motors, servo, gate calibration, per-unit heaters), and
  tunables are read from the new split config sections. Settings a unit's hardware cannot take
  are skipped instead of erroring, and stale readings clear when a unit has no buffer or
  encoder. The clog and gear settings now use the parameter names your Happy Hare release
  expects, so the clog screen works on every version.

### Changed

- **Android builds target API 36**, as Google Play requires.

### Fixed

**Stability**

- **K2 filament cuts no longer drive the toolhead into the frame** - unload, swap and the
  bypass fallback sent a bare cutter command from the purge chute, which crashed a K2 Pro's
  head into the frame, and the uncut filament was then pulled back. Every K2 cut now goes
  through the box's own positioned cut, as Creality's tool change does.
- **Crash at boot with a labeled USB stick on K1** (#1746) - the volume label was read after
  its memory was freed, which crashed the app on every start and tripped the crash-loop guard.
- **Crash closing the macro enhancement wizard** (#1762) - the close and cancel buttons used
  the wizard after it had been freed.
- **Crash when a dialog closed with animations off** - the dialog's contents could be freed in
  the wrong order, which showed up as a restart on the buffer status screen.
- **Memory read past the end in the G-code preview on Raspberry Pi 3B** - once the viewer grew
  larger than its cached frame, drawing could read beyond the image.

**Printing and files**

- **Reprint starts the original file** - after a print that ran from a temporary rewritten copy,
  Reprint failed with "Unable to open file".
- **USB files are copied to the printer before printing** - the USB tab used to hand the printer
  only the file name, so a print worked only if a same-named file already existed. Print now
  uploads the file (reusing an identical copy) and prints that, with its colors and thumbnail.
- **Delete is hidden for USB files** - it could remove a same-named file on the printer instead
  of the one on the stick.
- **Print thumbnail for prints in a subfolder** - the active-print thumbnail no longer fails
  to load.
- **QIDI .3mf prints keep their thumbnail** - a .3mf with no extracted G-code no longer
  downloads the whole archive, and a late reply for a previous file leaves the current
  preview alone.
- **The excluded-objects list keeps its scroll position** - it no longer jumps back to the top
  each time the printing object changes.
- **Temperature graph after a Klipper restart** - the nozzle line no longer freezes flat.
- **Pre-print status** - the empty bed mesh Klipper and Kalico report is read as "no mesh", so
  the mesh step is detected correctly.
- **Snapmaker U1 spaghetti warning appears when the code arrives after the pause** - the modal
  no longer gets skipped, and a detection pause wakes a sleeping display.

**Filament systems**

- **Spoolman active spool (#1717)** - editing an idle lane no longer takes the active spool
  away from the lane feeding the toolhead.
- **AFC spool unlink survives reconnect and restart** (#1717) - an unlinked spool stays
  unlinked.
- **Spoolman temperatures and spool weight** - Spoolman's single temperature value is read
  properly (ranges no longer show 0), the spool wizard sends data Spoolman accepts, and
  editing a spool's weight changes the spool instead of its filament.
- **A deleted Spoolman spool reads as no spool**, not as an unreachable server.
- **Spool assignments are kept per printer** - switching printers no longer copies one
  printer's spools into another's database.
- **Spool picker shows spool names on large screens.**
- **Tool badge on multi-system rigs** - a lane on a secondary filament system shows its own
  tool number.
- **Changing printers clears the old filament system** - the Multi-Filament card and lane
  colors from the previous printer no longer linger.
- **CFS** (#1464, #1761) - stock box updates that report only changed units no longer drop the
  others, and action completion no longer races status updates. Chained CFS and QIDI Box
  units draw one toolhead in the overview, and cards are ordered by address.

**Printers and display**

- **Snapmaker U1 SSH after reboot** - SSH starts again at boot when the stock Root Access
  setting is on.
- **Saved brightness applies at startup** instead of 100%.
- **Screen rotation survives a restart from Settings** - the watchdog used to relaunch at the
  old angle.
- **Creator 5 no longer detected as Z-Mod** (#1754).
- **K2 camera** (#1670) - the camera service keeps reclaiming the camera from a late-starting
  stock camera app.
- **Touch on DSI panels** - an honest transposed-looking touch range, such as Waveshare 2.8in
  DSI, corrects itself, and a range solved unrotated stays live on a rotated display.
- **Touch calibration on fresh installs** isn't flagged for recheck unnecessarily.
- **Multi-printer names** - a printer not yet connected shows its host instead of printer-N,
  and renaming starts from the saved name.
- **Leaving Change Host after Test reconnects the saved printer.**

**Installer and updates**

- **Android Update button** - a sideloaded install opens the GitHub release page instead of a
  Play Store listing that does not exist yet.
- **Beta channel is applied reliably** - installing a prerelease now sets the in-app update
  channel too, including on older BusyBox systems, and the choice survives settings backups.
  `--version` finds its manifest in any channel, and installing a local archive never moves
  the channel.
- **settings.json is written safely** - atomic writes that keep its permissions and symlink,
  and an unparseable file is left for the app to recover rather than replaced.
- **Moonraker restarts when its update entry is rewritten**, so Mainsail shows the right feed.
- **Update service skips cleanly** - benign skips no longer leave the unit marked failed, and
  a fresh install's update watcher paths are filled in.

## [1.1.0-beta.6] - 2026-10-07

<!-- whatsnew
The sixth beta of 1.1.

- Works with Happy Hare 4.0, including multi-unit setups
- Pick objects to skip before a print starts
- New Filament Buffer widget and Buffer Status screen
- K-Touch: add printers and switch between them live, steadier on weak Wi-Fi
- Faster Home screen
- Set heater targets from the thermistor widget
- Phone-style number pad
-->

The sixth beta of 1.1. The biggest piece is Happy Hare 4.0 support: multi-unit rigs, the
renamed settings and the per-unit hardware rules all work now, and clog detection settings
finally reach the printer on every Happy Hare release. You can also pick objects to skip
from the file details before a print starts, and the filament buffer gets its own home
widget and status screen. On the BTT K-Touch you can add printers and switch between them
without a restart, and a long run of connection fixes keeps it on Moonraker over a weak
Wi-Fi link. The rest is a faster Home screen, heater and graph improvements, buzzer sound
cleanup and an installer logo.

**Upgrading from beta.5?**

- **The number pad is now phone-style** (1-2-3 on top). Settings > Touch & Input > Number
  Pad Layout switches it back to Calculator.
- **The filament buffer left the Clog Detection widget.** That widget now shows the clog
  detector alone; add the new Filament Buffer widget to see the buffer.
- **Buzzers no longer play music.** Printers with a buzzer still play every UI tone, but the
  Test Tracker and other music controls are hidden unless the printer has a sound card.

### Added

**Filament systems**

- **Happy Hare 4.0 support** (#1479) - HelixScreen reads Happy Hare 4's new config layout, so
  tip method, heater limits and speed defaults load and the device actions no longer sit on
  "Loading configuration". Multi-unit rigs show each unit with its own gate count, name,
  topology and heaters, and commands carry the `UNIT=` Happy Hare 4 requires. Actions and
  settings go to a unit that actually has the servo, selector, encoder or buffer they need,
  and ones no unit supports are disabled. Bypass shows only when a unit has one, each gate's
  entry sensor and eSpooler state read correctly, and settings sliders send Happy Hare 4's
  renamed parameters. Happy Hare 3 is unchanged.
- **Filament Buffer home widget** - an upright buffer slider with the reading beside it,
  coloured by how far it is from target. At 2x1 the last minute scrolls out of the slider
  and the lean is said in words (Running tight, Running loose, Balanced). Works with Happy
  Hare sync feedback, AFC and OpenAMS pressure sensors.
- **Buffer Status screen** - tapping the widget, the loaded-spool card or the path diagram
  opens a live view of the buffer feeding the toolhead: a tall slider, the reading and
  target, the last minute, and the filament system's own details.
- **The loaded-spool card and the path diagram show the buffer** - a small slider beside
  the material on the card, and the buffer box on the path diagram recolours live.

**Printing**

- **Skip objects before a print starts** - the file details screen has the same skip button
  as the print status screen. Pick objects from the list, the map or the 3D render, and they
  are excluded as soon as Moonraker confirms the start. Picking every object refuses the
  start, and queuing the file instead of printing drops the picks and says so. Needs
  `[exclude_object]` on the printer.

**Home screen**

- **Set a heater target from the thermistor widget** - tapping a tile bound to a
  heater_generic or temperature_fan opens the temperature keypad, and the tile shows current
  and target. The keypad limit is the heater's own max_temp.
- **Filament dryer and other generic heaters on the temperature graph** - heater_generic
  objects other than the chamber are graphed, with history, on the graph screen and the home
  graph widget.

**BTT K-Touch (alpha)**

- **Add printers and switch between them** - Add Printer and the printer switcher work on the
  K-Touch, and a switch reconnects live instead of restarting the board. If the board is too
  short on memory to switch safely it restarts into the chosen printer instead. A newly found
  printer is named from Mainsail, Fluidd or its hostname.

**Settings**

- **Number pad layout** - Settings > Touch & Input > Number Pad Layout picks Phone (default)
  or Calculator. The phone layout puts a confirm key in the pad on whole-number fields, and
  the unit now sits inside the display.
- **PWM buzzer on any Linux board** - a passive buzzer on a hardware PWM pin works on a Pi or
  any other board by setting `sound.pwm_channel` in settings.json, or `HELIX_PWM_SOUND` to
  override it.
- **Add Printer can save a printer that is off** - if Test has not passed, Save asks
  "Can't reach this printer. Save anyway?" instead of being disabled.

**Installer**

- **The installer shows the HelixScreen logo** - as a real image in kitty, iTerm2 and sixel
  terminals, and as block art everywhere else. chafa is no longer needed.

### Fixed

**Filament systems**

- **Happy Hare clog detection settings now work** - the clog detection mode and length never
  reached the printer on any Happy Hare release, because the parameter names were wrong. Each
  release now gets the names it accepts, and the clog settings dialog pre-fills the configured
  length and sends it only when you change it.
- **Happy Hare whole-machine heater start and stop** sends one command per heated unit, and a
  partly failed dry stops the units it started.
- **Switching printers clears the old filament system** - after changing host or switching
  away from an AFC, Happy Hare or other filament system printer, its lanes, the
  Multi-Filament card and the home widgets no longer stay on screen.
- **The loaded-spool card fits on small screens** - the colour bar narrows and the meter labels
  hide below the medium size, so the material name keeps its line at 480x320.

**Printing**

- **Pre-print progress for silent PRINT_START macros** - a macro that prints no status (a
  Voron V0, for example) now shows homing and purging, counts the bed heat down from a real
  estimate, and learns its length for next time. Macros that clean the nozzle before meshing
  keep their leveling time.
- **QIDI .3mf prints keep their thumbnail** on the print status screen instead of showing an
  error toast, including Q2 plates and names in any case.
- **The objects button on the print status screen** appears and hides as the printer's
  `[exclude_object]` support is discovered or changes.
- **Print Files no longer reloads every 5 seconds** when a file name appears twice in the list,
  and those cards no longer show a blank name.

**Printer switching and Change Host**

- **Leaving a printer that is printing asks first**, and deleting the connected printer while
  it prints switches without asking twice.
- **A switch shows the new printer's state** - the previous printer's job, message,
  temperatures and history are cleared, a file or thumbnail download from the old printer
  never lands on the new one, and a same-named file on each printer gets its own thumbnail.
- **The connection-failed prompt waits while the printer list is open**, so a tap meant for
  a printer no longer lands on Change Address, and it is dropped once you pick a printer.
- **Printer names stay with their printer** - a name that resolves late goes to the printer
  it was asked for, never recreates a deleted printer, and a lost connection no longer saves
  the hostname as the name.
- **The printer list's connection dot** shows only on the connected printer, in its live
  colour, and the switch toast says Switched to rather than Connected to.
- **Change Host fixes** - leaving the dialog after Test reconnects the saved printer, editing
  the address clears the old status message, a blank address is refused before Save anyway,
  and adding a printer that already exists switches to it instead of duplicating it.
- **A printer's saved default Home layout** is read back instead of rebuilt after a switch.

**Screens**

- **Temperature graph legend has its own row**, so a trace near the top of the scale no
  longer runs under the legend text.
- **Generic heaters are named as heaters** (Chamber Heater, Filament Dryer Heater), and a
  chamber heater is no longer graphed twice when the chamber sensor is not reporting.
- **The thermistor tile shows --°C** for a sensor the printer does not report, instead of 0°C.
- **temperature_fan targets above 120°C** are allowed up to the fan's configured max_temp.
- **The Dry Filament button** on the bed card is a centred outline button, and outline
  buttons lose the stray shadow.
- **The Motion button on the Controls screen** uses light text on its dark fill.
- **Print status library buttons** shrink to fit a short card instead of overlapping.
- **The 2x1 Filament Buffer tile** keeps its padding at 480x320.

**Installer and updates**

- **Mainsail and Fluidd offer the right update channel** - the app keeps the HelixScreen
  update_manager channel in moonraker.conf in step with its own, so a beta install is no
  longer offered a "downgrade" to stable. Moonraker restarts after the change unless a print
  is running, and two screens sharing one Moonraker do not touch each other's entry.
- **A beta install restored from an old config backup stays on the beta channel.**
- **The first self-update after a fresh install** no longer restarts twice.

**Sound**

- **A PWM buzzer is never left sounding** after the app exits or crashes, and quiet volume
  settings no longer mute every tone.
- **Switching away from an AD5M** stops the old printer's M300 beeper.

**BTT K-Touch (alpha)**

- **Stays connected on weak Wi-Fi** - the board keeps Wi-Fi awake instead of in power save,
  print history loads ten jobs at a time and waits until the printer is discovered, a slow
  large reply has 20 seconds between pieces instead of being cut off, and a link only counts
  as dead when it goes fully silent. Together these stop the reconnect every 15-45 seconds
  seen around -75 dBm.
- **Fewer restart loops** - a stalled connection or discovery reconnects on its own, automatic
  restarts are capped until the board has been connected for ten minutes, and the screen
  never freezes waiting on a stuck connection during a switch.
- **Thumbnails behave on a weak link** - a stalled download gives up and is tried once more,
  a card that scrolls away cancels its download, loading a thumbnail no longer glitches the
  screen, and scrolling back reuses recent thumbnails instead of downloading them again.
- **Opening Print Files no longer glitches the screen** - the first open of the file list
  draws without the flicker it used to show while the list loaded.
- **Macros and bed geometry reach the screen** after discovery.

### Changed

- **The Home screen rebuilds much faster** - the print status card builds its printing views
  only during a print, and switching between printers whose layouts hold the same widgets
  re-seats the tiles instead of rebuilding Home.
- **Scrolling redraws less** - lists redraw only the rows that scrolled in, and on the K-Touch
  a scroll moves what is already drawn and renders only the new strip (about three times less
  work per frame).
- **The Macros screen opens faster.**
- **The Clog Detection widget** shows the clog detector alone; the buffer has its own widget.

## [1.1.0-beta.5] - 2026-10-07

<!-- whatsnew
The fifth beta of 1.1.

- Spool wizard searches SpoolmanDB as you type
- Excluded objects greyed out with a red hatch in the G-code preview, numbered badges on the render
- Print history keeps paging in older jobs
- K-Touch: smoother scrolling, far fewer out-of-memory aborts, steadier Wi-Fi
- Installer shows its steps and keeps a log
- Cleaner outlined inputs and grouped settings pages
-->

The fifth beta of 1.1. The headline additions are SpoolmanDB search in the spool wizard,
excluded objects shown in the G-code preview, and a pass over the installer. A lot of the
rest is the BTT K-Touch: scrolling feels right, print cards and file lists stop running the
board out of memory, and Wi-Fi is steadier. Settings pages and text fields get a visual
cleanup, and a long list of fixes covers printing from rewritten files, Spoolman and config
handling.

**Upgrading from beta.4?**

- **Spoolman filament temperatures are one number.** Spoolman stores a single temperature per
  filament, so the wizard and the spool screens now show one value instead of a range.
- **The external vendor and filament catalog calls are gone.** They never worked against
  Spoolman. Use the new SpoolmanDB search in the spool wizard instead (needs Spoolman 0.26 or
  newer).

### Added

**Filament systems**

- **Search SpoolmanDB from the spool wizard** - type in the vendor step and matching spools
  appear as you type. Picking one fills in vendor, material, colour, temperatures and weights,
  and reuses a vendor and filament you already have when they match exactly. The search is
  hidden on Spoolman servers older than 0.26.
- **Alarm when a print takes the machine with spools still on the bed** - if spools are
  latched on the bed for drying and a print starts, the screen wakes and raises an alarm
  instead of letting the print start fail (#1745).

**Printing**

- **Excluded objects show up in the G-code preview** - they are greyed out with a red hatch in
  2D and 3D, and the 2D and 3D render carries numbered badges that match the object list. The
  side list keeps its scroll position and puts the status beside the name in portrait.
- **Pre-print options from your PRINT_START macro** - when the printer database has no
  options for a printer, the print file screen builds them from what PRINT_START actually
  does. Wrapper macros are followed into the macro that does the work.
- **Print history pages in older jobs** - the history list loads further back as you scroll,
  and the dashboard says when its stats cover only the newest prints. The K-Touch now has
  print history too, so Reprint Last, the idle print card and the history marks in the file
  list work there.
- **A notification with no title leads with its message** in the history list.

**Home screen**

- **Printer images are cleaner** - the white backgrounds are cut out of four printer images,
  the art is cropped to its content, there is new generic CoreXY art, and the image shrinks
  slightly in pinned mode so callout leader lines still fit.
- **Plugin widgets can take half-cell sizes** if the plugin asks for it.

**Installer**

- **The installer shows what it is doing** - it detects first, prints a plan, asks once before
  changing anything, and then shows each step as it runs. The log survives failures, apt and
  service output goes to the log instead of the screen, and `--dry-run` and `--verbose` are
  supported. `--uninstall` lists what it removed and what it kept.

**BTT K-Touch (alpha)**

- **Leaves a weak access point** - when Moonraker keeps stalling on one access point, the
  board moves on to another with near-equal signal.

### Fixed

**Filament systems**

- **Spoolman deletes and missing data read correctly** - a deleted spool is reported as not
  found instead of Spoolman being unreachable, and a missing lane data entry reads as empty.
  This now works against a real Moonraker, not just the mock.
- **Spool weight edits and new vendors save correctly** - a weight edit changes the spool
  instead of its filament, and a new vendor's website goes in the vendor's comment.
- **Happy Hare loads and unloads no longer flash Idle** before the printer reports them (#1057).
  A preheat from the sidebar cancels the previous operation's cooldown and says when a load is
  dropped.
- **The home spool row** stacks the name over the spool and re-measures when it resizes.
- **Colour names** - the picker's black and other near-blacks are named Black, and the
  darkest ones Deep Black, instead of Dark Gray.

**Printing**

- **Reprint after a rewritten print** - a print started from a rewritten copy reprints the
  original file, keeps working after a restart, shows in history as the original's print, and
  follows a print started by another client.
- **Pre-print options stay with their printer** - stored settings reset when the hardware
  changes, and an option the firmware already holds no longer strips the file.
- **Starting a print with spools on the bed** opens the prompt instead of failing.
- **Snapmaker U1 spaghetti detection** raises its modal even when the detection code arrives
  after the pause.
- **Timelapse frame counts** restart with each new print and survive joining a print already
  running.
- **Bed mesh overlays and 2D touch** follow the frame on screen, so the 2D view draws again
  when forced.
- **Print Files rebuilds an open detail view** when a new analysis lands, and a metadata
  request lost with the connection is fetched again instead of extracting the G-code.
- **Cached thumbnails that are not real PNGs** are deleted and regenerated, and unknown free
  space is not treated as disk pressure.
- **Thumbnails are sturdier** - the active print's thumbnail is found for files in subfolders,
  downloads land whole or not at all, a refresh keeps a local file's thumbnail, and leftovers
  from a crash are cleaned from the cache.

**Screens and navigation**

- **Screens opened from a home card are no longer blank** - Print Files, Recent Prints and
  Reprint Last come up activated.
- **Fewer double closes and ghost screens** - a cancelled duplicate push no longer closes the
  stacked screen twice, and a push for a screen closed in the meantime is dropped.
- **The recovery dialog has a close X** in its header, and never shows a ready-era message
  under a shutdown title.
- **The temperature graph rebinds after a Klipper restart** and a graph built while hidden
  starts paused.
- **Notification panels** no longer stack a second one while one slides out.
- **A scroll that starts on a settings row** leaves the row unpressed.
- **Bed screw intro text spacing** and the Heater Calibration title fit properly.
- **Probe settings left unset show Klipper's default** and no longer save the word "default".
- **Text fields bound to a setting keep their full value** instead of being cut to one character.
- **Dialog buttons fill their rounded corners.**
- **Joining a hidden Wi-Fi network shows Connecting** while it connects.
- **The filament runout dialog's Resume button** shows its accent and blocked states.

**Config**

- **Config migration is safer** - a config from a newer build gets no default printer
  injected, a versionless config with no printer starts from fresh defaults, legacy display
  keys migrate before the defaults, and the touch calibration of a versionless config is left
  alone.
- **PRINT_START analysis downloads only what it needs** - the printer config download follows
  the include chain a few files at a time, reads includes the way Klipper does, retries when
  the queue is full, and fails loudly instead of returning a partial config.
- **A broken printer database** shows one toast instead of one per lookup.
- **Beacon accelerometers** found only through the resonance tester no longer drop out on
  every second discovery.

**Installer and updates**

- **A beta install stays on the beta channel** - settings and the update_manager stanza record
  it, an explicit `--version` finds its manifest in any channel, and a `--local` archive never
  moves the channel.
- **settings.json is written safely** - by one writer, atomically, keeping its mode and link,
  and an unreadable one is left for the app to recover.
- **Moonraker restarts when an existing update_manager stanza is rewritten.**
- **The plan reads sensibly** - `Update <installed> -> <target>`, only the stock UIs that run,
  and no version learned by running the installed binary. `--clean` lists its deletions and
  asks once.
- **Update failures show the real cause** on the touchscreen, not an earlier harmless error.
- **An update never installs from a `--test` run**, the update service skips cleanly instead of
  failing, and the launcher stops display-sleep only on a host that has it.
- **Device deploys strip group and other write permission** from what they unpack.

**BTT K-Touch (alpha)**

- **Scrolling feels right** - throw, limit and long-press settings reach every touch input
  including the K-Touch's, with a shorter glide by default there, and momentum follows elapsed
  time so slow-rendering screens scroll as far as fast ones.
- **Out-of-memory aborts are reduced** - a file list, history page or Print Select list too
  big for memory shows an error and keeps the list, and the network lane sizes its buffers
  from what it receives and fails a request instead of the board.
- **Print card thumbnails use far less memory** - they decode once at card size, only on-screen
  cards hold one, and they come back after leaving Print Files. The active print's thumbnail is
  released and kept correctly, and the file detail backdrop uses half the memory.
- **Wi-Fi is steadier** - access point steering is bounded to near-equal signal and its own
  scans, rides out a failed join, and BSSIDs are redacted from the logs.
- **Overlay screens have square corners** on the K-Touch, as on other limited hardware.

**Plugins**

- **Runaway plugins are stopped sooner** - the time budget counts CPU time with a one second
  wall-clock ceiling, a backtracking string pattern trips the budget, and `string.rep` of an
  empty string returns at once.
- **Plugin storage writes** no longer happen inside a plugin call.
- **Print symlinks** carry the original's full path, and a failed link leaves no directories.

**Other**

- **A USB stick's volume name no longer crashes** the app when the list is read (#1746).
- **Debug bundles are more useful** - they keep Klipper webhooks state and git SHAs, carry
  object-list errors, and Android release builds may upload diagnostics.

### Changed

- **Settings pages group their rows in captioned cards**, and the PID screen is titled Heater
  Calibration.
- **Text inputs, dropdowns and spinboxes are outlined**, field outlines clear 3:1 contrast in
  every theme, the off-state switch knob is muted (outlined white in light mode), dropdown
  popups stay opaque, and dropdown row labels are centered.
- **Pressed feedback is lighter on slow hardware** - on limited tiers and the K-Touch, a
  pressed card shows a primary-colour ring instead of scaling down, and buttons keep their
  pressed colour.
- **The home screen edit mode is much smoother** - dragging, resizing and leaving edit mode no
  longer rebuild the page, and nothing is saved until edits settle.
- **The widget catalog opens faster**, and the temperature graph stops sampling while its
  screen is inactive.

## [1.1.0-beta.4] - 2026-10-05

<!-- whatsnew
The fourth beta of 1.1.

- Lua plugins: home tiles, screens and settings from the printer's config folder
- Motion Bed tab: tap or drag the plate to move the toolhead
- Belt Tension rebuilt to compare both belts with resonance sweeps
- Android APK for 32-bit devices like the Echo Show 5 and Fire tablets
- Alpha firmware for the BTT K-Touch
- CFS with a box switched off, ACE toolchanges, USB printing fixed
- Screens free their memory when closed
-->

The fourth beta of 1.1. It adds Lua plugins, which put their own tiles, screens and
settings on the printer's screen, a Bed tab on the Motion screen, and a 32-bit Android
build. Belt Tension is rebuilt to compare both belts the way Shake&Tune
does. Most of the rest is fixes: CFS chains with a box switched off, ACE toolchanges,
printing from a USB stick, touch on rotated screens, and a long list of rare crashes. Many
screens now give their memory back when you close them.

**Upgrading from beta.3?**

- **Configure PRINT_START is gone from Advanced.** The beta macro rewrite is removed. The
  pre-print skip options on the print file screen work as before.
- **Printing from a USB stick copies the file first.** Print and Add to Queue upload the file
  to `usb_prints/` in your G-code folder, with progress, then print that copy. Files on the
  stick can no longer be deleted from the screen.
- **Screens slide in from the right in portrait too**, with a left back arrow, the same as
  landscape.

### Added

**Plugins**

- **Lua plugins** - drop a plugin into `helixscreen/plugins` in your printer's config folder
  and HelixScreen picks it up, on every printer you connect to. A plugin can add home screen
  tiles, its own screens and a settings page, and draw charts. Each one runs in a sandbox with
  its own memory and time limits, and asks before it gets access to G-code, Moonraker or the
  network. Settings > Plugins lists them, and nothing runs until you enable it there. A
  plugin that changes on the printer reloads in place. Two example plugins (a temperature
  sparkline and an LED effects tile) and an author guide are in the HelixScreen repository.
  Not available on Android yet.

**Motion**

- **Bed tab** - tap the plate to send the toolhead there, or drag and it follows your finger.
  Below the new Bed Map Clearance setting (5mm by default) the first move lifts Z first, and
  the readout says so before you tap. The tab is disabled during a print and offers Home when
  the printer is not homed.

**Calibration**

- **Belt Tension is rebuilt around resonance sweeps** (#1721) - still a beta, under Advanced >
  Calibration on CoreXY printers with an accelerometer. Instead of listening to a plucked belt,
  it runs a resonance sweep on each belt path and compares the two curves, leading with how
  similar they are and pairing up the peaks, the way Shake&Tune compares belts.

**Android**

- **A 32-bit Android APK** - devices that refused the app with "no matching ABIs" can install
  `helixscreen-android-armv7`: the Echo Show 5 (2nd gen) on LineageOS, Fire tablets and older
  phones. Low-density screens like the Echo Show's are drawn at full size instead of at half.

**BTT K-Touch (alpha)**

- **HelixScreen firmware for the BigTreeTech K-Touch** - the release now carries
  `helixscreen-esp32-ktouch` firmware for the standalone ESP32 touchscreen. It joins your
  Wi-Fi and talks to your printer's Moonraker like a remote screen. This is an alpha with
  rough edges; the [install guide](https://github.com/prestonbrown/helixscreen/blob/main/docs/user/guide/install-esp32.md) covers flashing from the
  browser or with esptool, first-boot setup and recovery.

**Filament systems**

- **ACE shows a toolchange as it happens** (#1678) - the path draws the outgoing lane
  retracting until the hub clears, then the lane being loaded.
- **A CFS box that is switched off shows as Not connected** (#1464) - its card is dimmed, its
  bays are left out of counts and lane choices, and tool changes and Clear Spool refuse them.

**Other**

- **The widget catalog says why a widget will not fit** - a widget with no free space on the
  page shows how much room it needs instead of failing when you tap it.
- **Pressed feedback** on more rows, chips, value fields, camera source rows and filament
  temperature rows (#1297).

### Fixed

**Filament systems**

- **CFS chains keep each bay's state when a box drops out** (#1464) - bays are numbered the way
  the firmware numbers them, so a Klipper restart with one box off no longer shifts spools and
  colours onto the wrong bays, and a box behind a switched-off one still works.
- **CFS and QIDI Box chains draw one toolhead** in the filament overview, since every box feeds
  the same extruder.
- **QIDI Box follows live updates** - after startup, changes on the box never reached the
  screen.
- **ACE reads the right slots**, and French "Currently Loaded" no longer shows garbled
  characters (#1678).
- **Snapmaker U1 tool changer state survives a temperature update**, and a malformed slot colour
  leaves the slot's colour alone.
- **A gate sensor that flickers raises one "Same spool?" notice**, not a stack of them.
- **AFC and Happy Hare show the right Spoolman vendor** right after a spool is relinked.
- **A loaded black or grey spool shows on the nozzle** instead of the bare metal tip.

**Printing**

- **Temperature changes during an AD5X print start no longer fail** - a temperature, fan or
  light change during the start macro's bed mesh is sent once the macro finishes instead of
  timing out with "Failed to set temperature", and a jog in that window is refused instead of
  queued for minutes.
- **The bed mesh countdown moves** - during the pre-print bed mesh the time left no longer sits
  at a few seconds for minutes on end.
- **Pre-print timing history is kept** when a new start is recorded, instead of losing or
  duplicating earlier entries.
- **Park and the Bed tab lift never drive into the plate** when the printer reports a Z move
  before confirming it.
- **Action prompts behave like Mainsail's** - closing one on screen ends it on the printer, each
  button group gets its own row, and secondary buttons use the secondary colour.
- **Belt Tension and input shaper refuse to start with too little free memory** (#1738), which
  could wedge Klipper on the CC1. When Klipper stalls, the message says to power-cycle.

**USB sticks**

- **A USB print prints the file you picked** - Moonraker used to receive only the file name, so
  a print found the file only if a same-named one was already on the printer.
- **USB file details load from the stick** - preview, pre-print checks and thumbnails read the
  file in place instead of waiting out a timeout, Cura's JPEG thumbnails show, and listing the
  stick no longer freezes the screen.

**G-code preview**

- **Two printers with a same-named file no longer share a preview**, and a preview whose size
  no longer matches the file on the printer is fetched again.
- **Closing and reopening the print status screen mid-download** joins the running download
  instead of starting a second one into the same file.
- **The 3D preview uses the theme colour** for a file with no colour information, matching 2D,
  instead of a rainbow height gradient, and filament colours survive switching to 3D while
  printing.
- **The Building preview label shows real progress** and is translated; the loading spinner
  shows while a 3D switch builds.

**Touch and display**

- **Touch calibration on a rotated screen** (#1714) - cancelling or retrying no longer
  reinstalls an old touch range, and the preview uses the range it just solved.
- **Panels whose touch range looks swapped correct themselves** (Waveshare 2.8in DSI and
  similar) from the first touch past the screen edge, and stay correct on a rotated screen.
- **Screens rotated by the kernel get touch right on first boot** (#1428).
- **No crash at startup with background blur on some Allwinner boards** (#1742) - blur reuses
  the display's graphics context instead of loading the driver a second time.
- **K2: the camera stays available** (#1670) when the stock camera app starts late.

**Look and theme**

- **Colours set in a screen's layout survive a theme or dark/light switch** (#1735).
- **Light mode text is readable** on Motion's selected tab, the jog pad and the bed mesh labels.
- **Print file cards fit** under the Recently Printed banner instead of clipping the bottom row.
- **Stacked confirm dialogs keep their own buttons and text** instead of taking the top one's.
- **A grey picked in a Spoolman colour picker is saved** instead of reading as no colour.

**Bluetooth and accessories**

- **Bluetooth scans** - starting a new scan no longer crashes the settings screens or waits out
  the old one, a scan that could not run says so instead of "0 found", and pairing works on
  systems whose adapter is not hci0.
- **Barcode scanners with a generic name are listed**, and opening the scanner screen no longer
  freezes it for up to 5 seconds.
- **A USB label printer with an unreadable ID no longer crashes the app.**

**Stability**

- **Raspberry Pi OS 32-bit: no crash when a screen deletes itself** (#1601) - the 32-bit stack
  layout made an internal safety check fail.
- **Raspberry Pi OS 32-bit no longer crashes with heap corruption** (#1732) - pi32 builds carry
  their own C++ runtime, about 1 MB more.
- **A crash on a background thread leaves a crash report**, where before the app exited with
  nothing recorded.
- **A G-code command, file name or network name with invalid characters** no longer leaves a
  request hanging (#1493).
- **Rare crashes fixed** when switching printers, closing screens with animations off, closing
  the G-code viewer while it loads, starting a sound at startup, and when Moonraker, history and
  Spoolman replies arrive after a screen is gone.
- **The file list no longer floods slow printers with rescans** - one metadata scan runs at a
  time, and a file that keeps failing waits before it is tried again.

**Other**

- **Error messages and toasts from the printer connection are translated**, including Wi-Fi
  failures.
- **Connection restored is a success toast** and no longer adds a second row to the bell.
- **Cartographer and Beacon probes are found** as one probe, and the probe's state shows; K1,
  K2 and QIDI printers keep their probe Z offset live.
- **Editing a setting that appears twice in printer.cfg** changes the copy Klipper actually
  reads.
- **Debug bundles redact anything they cannot check** before upload.
- **Installer** (#1534, #1693) - a vendor screen or KIAUH service that re-enables itself stays
  off, and installing from a desktop session no longer takes down your desktop.

### Changed

- **Less memory** - Motion, temperature graph, theme editor, calibration, PID, Z offset, touch
  calibration, printer manager, LED and filament system screens free their memory when you
  close them. The print file list loads on first open instead of at startup, and bed mesh
  holds 0.75 to 1.5 MB less.
- **Screens open with one redraw** - a settings or overlay screen used to redraw the whole
  display once before it appeared and again as it slid in. It is now built off screen, which
  saves a full-screen draw on every open on slower printers.
- **Updates, crash reports and debug bundles verify the server's certificate** - every release
  package ships its own certificate bundle, so printers with outdated system certificates
  still connect. Printers and other devices on your network are unaffected.
- **Settings from releases before 0.99.4 start fresh** - the setup wizard runs again, your
  printer connection is kept, and the old file is saved as `settings.json.pre-migration`.

## [1.0.3] - 2026-10-04

### Changed

- **Belt Tension is withdrawn from 1.0** - the sweep read its accelerometer data from a
  folder Klipper never writes to, so it failed on every printer
  ([#1721](https://github.com/prestonbrown/helixscreen/issues/1721)).
- **The log records more by default** - production builds log at Info and write each line
  to the log file as it happens, so a log taken right after a problem already holds what
  happened.

### Fixed

**Stability**

- **Raspberry Pi OS 32-bit no longer crashes with heap corruption** - the pi32 build used the
  system's C++ runtime, which is built for an older CPU generation and keeps its bookkeeping
  differently from what the app expected, so ordinary file listing wrote into freed memory
  (`malloc(): unsorted double linked list corrupted`). pi32 builds now carry their own
  runtime, which adds about 1 MB to the package
  ([#1732](https://github.com/prestonbrown/helixscreen/issues/1732)).
- **An unusual character in a name no longer loses a save** - a Wi-Fi network, printer or
  file name containing bytes that are not valid text could crash the app or silently drop
  the settings save. Those bytes are now replaced and the save goes through.
- **The file list no longer floods slow printers with rescans** - the print list asked
  Moonraker to rescan a file's metadata on every miss, and on a slow host the queue starved
  the screen's own requests. One scan runs at a time, and a file that keeps failing waits 10
  minutes before it is tried again.

**Printing and filament systems**

- **Several CFS units on community CFS firmware show one card per unit** - with two or more
  boxes chained, every lane was drawn in one long row with overlapping labels. Each box is
  now its own unit, so the multi-filament overview opens with one card per CFS.
- **The file detail view no longer freezes the screen on large G-code files** - backing out
  while the preview was still building waited out the whole build on the interface thread,
  which could take many seconds on a large print. Cancelling now stops the indexing,
  geometry and cache work promptly instead
  ([#1706](https://github.com/prestonbrown/helixscreen/issues/1706)).
- **File thumbnails on printers without file metadata stay bounded** - if Moonraker ignores
  range requests and answers a small header read with the whole G-code file, the download
  stops once the requested bytes have arrived
  ([#1706](https://github.com/prestonbrown/helixscreen/issues/1706)).
- **Pre-print status on the Snapmaker U1 shows each real step** - the preparation screen
  used to sit on one label for most of the run: "Priming..." replaced every step from the
  first toolhead move until the first layer, and a step following an earlier related one
  (probing Z after homing the axes, bed mesh and plate detection after the bed inspection)
  kept showing the first step's name instead of its own. Each step now shows as it happens:
  homing, probing Z, bed mesh, plate detection.
- **The heat-first warning checks the slot you selected** - with a cold nozzle, Load and
  Unload warn about, or preheat for, the material of the slot the operation acts on, not
  whichever spool happens to be loaded or on the external holder. Purge, Extrude and
  Retract use the loaded lane's material.
- **Happy Hare: Recover sets the real gate and whether filament is loaded** - Recover opens
  a dialog to pick the gate that is really selected and whether filament is loaded,
  instead of a bare `MMU_RECOVER` that could leave the position Unknown. The error popup's
  own Recover sends a plain `MMU_RECOVER` and lets Happy Hare detect, where it used to send
  an `UNLOADED=1` Happy Hare does not accept.
- **Bypass could stay unavailable after an unload** - the filament path now settles on the
  system's own state, so bypass becomes available once an unload finishes, including one
  that ends with a CFS cut ([#1512](https://github.com/prestonbrown/helixscreen/issues/1512)).
- **Device operation messages said "AFC" on every filament system** - homing, recovering and
  aborting now read the same on Happy Hare and every other system.
- **The Change Filament spool picker tells two spools of one vendor apart** - each row now
  carries Spoolman's filament name.
- **Your Material Temperatures edits survive a Moonraker update** - `user_filaments.json`
  now lives in your config directory, linked into printer_data, instead of the install
  directory a Moonraker web update replaces.
- **Scan QR opens the barcode scanner on printers built without a camera** - on the AD5X,
  AD5M, K1, K2, CC1 and U1 the button did nothing ([#1726](https://github.com/prestonbrown/helixscreen/issues/1726)).

**Files and history**

- **.gcode.3mf files keep their thumbnail, layer height and pre-print checks** in the file
  details, and their names show without either extension ([#1713](https://github.com/prestonbrown/helixscreen/issues/1713)).
- **Print history reads more jobs correctly** - it translates the job status, finds
  thumbnails for files in subfolders, ages a job with no end time by its start, and reads
  number fields Moonraker sends as text ([#1713](https://github.com/prestonbrown/helixscreen/issues/1713)).
- **The home screen's last-print tile** words an unfinished job by its status and hides
  stats that are zero ([#1713](https://github.com/prestonbrown/helixscreen/issues/1713)).

**Updates**

- **Updates, crash reports and debug-bundle uploads verify the server's certificate** - every
  release package ships its own certificate bundle, so printers whose system certificates
  are years out of date still connect.
- **Updates follow your update channel** - the installer and Moonraker's update manager use
  the beta or dev channel you picked instead of always pulling stable.
- **Installing the Moonraker plugin from the screen works on installed releases** - the
  release package now includes the plugin.
- **Mainsail shows the new version after an update** - the first start after an update asks
  Moonraker to refresh it ([#1727](https://github.com/prestonbrown/helixscreen/issues/1727)).
- **An update restarts HelixScreen through systemd**, so the service picks up its updated
  unit files ([#1713](https://github.com/prestonbrown/helixscreen/issues/1713)), and the restart no longer fails when `~/.helixscreen` is missing.

**Screen and input**

- **Screen rotation at 90 or 270 degrees on DRM displays draws correctly** - the picture is
  rotated once, in software, instead of being handed to a display plane that could not lay
  it out ([#1275](https://github.com/prestonbrown/helixscreen/issues/1275)).
- **Home screen text follows a language change** - fan, nozzle, spool, camera, preheat and
  print-stats labels switch language with the rest of the screen instead of staying in the
  old one.
- **The last-print card keeps its thumbnail** - it retries when you return to the home
  screen and keeps its picture when the print's details are re-read.
- **Scroll Guard works again** - with the post-scroll click guard turned on, a tap right
  after a scroll no longer goes through on the printer's touchscreen. Changing the setting
  needs a restart.
- **The Full corner radius no longer turns dialogs into circles** - buttons still round into
  pills, while cards, dialogs and inputs stop at a pill-shaped radius.

## [1.1.0-beta.3] - 2026-09-29

<!-- whatsnew
The third beta of 1.1.

- Installing the HelixPrint plugin from the screen works again
- Updates stay on the beta channel
- E-stop on the navigation bar during a print
- Smoother 3D preview on slower boards like the Pi 3
- Home screen tiles size down to half a cell and up past one
- Changing the language updates the whole screen
-->

The third beta of 1.1. It fixes two problems that beta.2 testers hit: installing the HelixPrint
plugin from the screen, and updates that ignored the beta channel. It also puts the E-stop
on the navigation bar, makes the 3D preview usable on slower graphics chips, and gives home
screen tiles a wider range of sizes. Changing the language now updates text that used to
stay in the old language until a restart.

**Upgrading from beta.2?**

- **Installing the HelixPrint plugin from the screen works.** In beta.2, every install from a release
  failed with "Install script not found". Try it again after you update.
- **Updates follow your update channel.** The install script and Moonraker's update manager
  now read the channel set in Settings > Updates. Before, they always picked the latest
  stable release, so an update could take a beta tester back to 1.0.
- **The E-stop moved to the navigation bar.** While a print is running, it sits at the end of
  the bar on every screen, including over dialogs and number pads. The separate E-stop
  buttons on Home, Controls and Print Status are gone.

### Added

**Printing**

- **E-stop on the navigation bar** - shown whenever a job holds the machine, and kept above
  screens, dialogs and number pads. The lock screen and fullscreen camera keep their own.
- **The print file screen has a portrait layout** - options scroll above a Delete/Print row
  that stays put, and pre-print options are checkable tiles in two columns, so long labels
  wrap instead of running under a switch.
- **Bed drying heats a plain chamber heater too** - on a printer with a chamber heater and no
  dryer, the drying run holds the chamber at the material's air temperature and turns it off
  when the run ends.
- **Tool remapping rewrites `TOOL=` and `INITIAL_TOOL`** in the print file, and warns about
  start-macro values it leaves alone. The warning appears only when your picks move a tool.

**3D preview on slower boards**

- **The 3D preview no longer stalls the screen on a slow graphics chip** - still frames are
  drawn in slices, and a lighter view is drawn while your finger is down. Walls are shaded
  as smooth surfaces, and still frames are sharper.
- **Large files stay in 3D** - a file with too many triangles for the graphics chip gets a
  simplified 3D view instead of dropping to 2D.
- **Small files follow the print in 3D on low-memory boards** - a Pi 3 now loads a Benchy-size
  file fully during a print instead of always streaming it.

**Home screen**

- **Tiles size more freely** - most tiles shrink to half a cell, and grow past a cell and a
  half with a larger icon. Fan and sensor tiles keep their name and shrink the icon instead.
  The print controls tile stacks when tall and drops its labels when narrow.
- **G-code console widget** - at two cells tall it shows the printer's latest output.
- **Drop a widget on another to swap them** (#1503) while editing the home screen.

**Other**

- **Hide the on-screen keyboard while a USB keyboard is attached** (#1572) - the Hardware
  Keyboard setting in Settings > Touch & Input.
- **Debug bundles describe the host** (#1692) - the Linux distribution, failed services, and
  other programs that might be fighting over the display.
- **Pressed feedback** on the cards and rows that showed none (#1297).

### Fixed

**Installing and updating**

- **Plugin install from the screen** - release builds now include the plugin's install
  script.
- **Updates stay on your channel** - beta and dev installs update to beta releases instead of
  the latest stable one.
- **`--uninstall` removes a home-directory install** (#1600), and refuses an install folder it
  cannot safely delete.
- **Free-space checks work on BusyBox systems**, and a printer that ignores a custom install
  folder still migrates an existing install (#1674).

**Changing the language**

- **The whole screen follows a language change** - home widgets, tool, filament system,
  temperature graph and preheat text, Check for Updates status and the timelapse button
  update right away instead of after a restart.
- **Durations use the current language**, and capitalized labels work for Cyrillic, Greek and
  accented Latin text.

**Filament**

- **Filament diameter comes from your printer** (#1504) instead of assuming 1.75mm, including
  for filaments HelixScreen creates in Spoolman.
- **Restoring a tool remap after a print is more reliable** (#1684) - if the filament system
  refuses the restore, it is retried once it will accept it, never during a print, and a tool
  that was unmapped before the print stays unmapped.

**Screen and touch**

- **The screen no longer freezes while Wi-Fi status is read** (#1023) - Settings, the network
  tile, the setup wizard and the Network screen read it in the background. Each read could
  block the screen for up to 10 seconds.
- **Forget and the Wi-Fi on/off switch are hidden** on systems that cannot do them (#1429).
- **The touchscreen is detected properly on the Snapmaker U1** and other boards whose build has
  no libinput, so touch calibration detection runs.
- **The layer counter no longer jumps back** at the start of a print.
- **The last print card** keeps its thumbnail, retries it when you come back to Home, and
  never shows a picture for a file that is gone.
- **Print Status at 480x272** keeps both icon and label on its action buttons.
- **The prep-time estimate appears the first time** you open a file's details.

### Changed

- **Faster drawing** - the toolhead, spool, filament path and darkened backdrop draw with less
  work, and the screen redraws only when something changed.
- **Network library messages appear in HelixScreen's log** (#1317), tagged `[libhv]`.

## [1.1.0-beta.2] - 2026-09-28

<!-- whatsnew
The second beta of 1.1.

- Settings regrouped into twelve pages, each one tap away
- Motion screen: Jog and Move tabs, Park, tap a coordinate to go there
- Dry filament on the heated bed, or with a Panda Breath
- Live temperature, fan and light chips on the printer picture
- One LEDs screen for every light, one light per home button
- Happy Hare recovery that asks what is really loaded
- Pinch to zoom the 3D views
-->

The second beta of 1.1. It adds the Motion screen's Move tab, filament drying on the
heated bed and with a chamber heater, live status on the home screen's printer picture,
and a rebuilt LEDs screen. Settings is reorganized so every page is one tap from the
Settings screen. It also fixes a K2 crash at print start and a long list of filament
system problems, most of them on Happy Hare and the Snapmaker U1.

**Upgrading from beta.1?**

- **Settings pages moved; your settings did not.** Nothing is reset. Display & Sound
  splits into Display, Appearance, Sound and Language & Time, Hardware & Devices becomes
  Devices, Safety & Notifications becomes Safety & Alerts, Network and Printers move to
  a new Connection page, and Update Channel and Check for Updates move to a new Updates
  page. The table under Changed has the full map.
- **Print Completion Alert now uses its documented default.** A printer that never
  changed the setting showed only the short notification; it now shows the full-screen
  summary. A choice you made yourself is kept.
- **Material Temperatures changes move to `user_filaments.json`.** They are moved over
  automatically on the first start. Going back to 1.0 or beta.1 loses them, because
  those builds read them from `settings.json`, so note them down first if you might go
  back.
- **AD5X and K1 series: touch works again.** beta.1 did not respond to touch at all on
  these printers; update over SSH or the web interface if the screen is unusable.
- **"Home printer first?" is gone.** A filament load or unload on an unhomed printer
  homes and carries on, without asking.

### Added

**Motion**

- **Jog and Move tabs** (#865) - the Motion screen opens on Jog every time, with the
  position readout in the header on both tabs. The Move tab sends the toolhead to nine
  named positions laid out like the bed seen from above, placed on the print plate from
  your `[bed_mesh]` area so none aims at a purge area or tool dock. The positions grey
  out while the toolhead is moving, and the whole tab is disabled during a print.
- **Park** - runs your printer's own parking macro when it has one, otherwise lifts the
  nozzle 10mm and parks over the rear of the plate. Point it at a different macro in
  Settings > Printing > Macro Buttons.
- **Tap a coordinate to move there** - a number pad opens for that axis, refuses a value
  outside the printer's range, and homes first when needed. A Target / Actual chip beside
  the coordinates switches which position is shown.
- **Hold to repeat** on the jog pad and Z buttons, and **jog limits you can see**: a
  button greys out at its limit, and a press that cannot move says which limit it hit.

**Drying filament**

- **Dry filament on the heated bed** (#1730) - on an enclosed printer, Dry Filament on
  the bed card or in Advanced unloads the toolhead, homes, moves the plate as far from
  the nozzle as it goes, and asks you to lay the spools on it under a box. A banner shows
  each step as it runs, and Stop leaves the plate where it is. The bed holds a drying
  temperature for the material, capped at 90°C, for 12 hours, and HelixScreen will not
  home, move or start a print until you confirm the spools are off. Printers you enclosed
  yourself can be marked enclosed in Settings > Printing.
- **Dry filament with a Panda Breath** (#1299) - on stock firmware, the chamber card runs
  the unit's drying cycle from a material preset, with the live temperature, the time
  left and a Stop button, and an optional 70°C bed assist that turns itself off however
  the run ends.

**Home screen**

- **The printer picture shows what the printer is doing** (#1397) - temperature chips
  appear while a heater is on or still hot, and the part fan and light show when they
  run. Tap a chip for that heater's graph, the fan controls or the light controls. On
  the 13 most common printers each chip points at its part; on others, including your
  own photo, Tag parts in the image picker lets you place them.
- **One LEDs screen for every light** (#1130) - a tab per light, with a dot showing
  whether it is on and in what color, and each tab shows only what that light can do.
  Each home light button controls one light or all of them, chosen from the gear in edit
  mode, and Automatic LED Control has its own "Applies to" list.

**Filament systems**

- **Happy Hare: tell the MMU what is really loaded** - Recover opens Recover State: pick
  the gate that is really selected (or Bypass) and whether filament is loaded, or let
  Happy Hare detect it. Nothing moves and no tool is remapped. The slot menu gains
  Preload, Maintenance gains Load Extruder and Unload Extruder, and Accessories gains
  Refresh Spoolman.
- **OpenAMS pressure sensor on the filament path** (#1724) - OpenAMS without AFC shows
  each unit's FPS as a box on the path; tap it for the current pressure.

**Everywhere**

- **Pinch to zoom the 3D views** - on a capacitive screen, pinching zooms the 3D G-code
  preview and the 3D bed mesh, and a two-finger drag moves the zoomed view.

### Changed

- **Settings is twelve pages in three groups** (#1023) - Screen, Printer and
  HelixScreen, every page one tap from the Settings screen, several with a short status
  line such as your brightness or whether an update is waiting.

  | Was | Now |
  |-----|-----|
  | Display & Sound | Display, Appearance, Sound, Language & Time |
  | Printing > Toolhead Style, G-code Preview, Z Movement | Appearance |
  | Hardware & Devices | Devices |
  | Hardware & Devices > Printers | Connection > Printers |
  | Safety & Notifications | Safety & Alerts |
  | Safety & Notifications > Allow cold load/unload, Cool nozzle after filament ops | Printing |
  | System > Network Settings, Host | Connection |
  | System > Touch & Input | Touch & Input, on the Settings screen |
  | Display & Sound > Scroll Buttons, System Keyboard, Keep Navigation Bar | Touch & Input |
  | Help & About > About > Update Channel, Check for Updates | Updates |

- **Filament operations on an unhomed printer home without asking** - the "Home printer
  first?" dialog is gone. On the Snapmaker U1, which homes and heats for its own loads,
  HelixScreen no longer sends a home or a preheat first, and the load step bar follows
  the printer's own order (Home, Select, Feed, Heat, Extrude, Purge), showing Heating and
  Purging on those steps.
- **Material types and your Material Temperatures changes live in one editable file** -
  the built-in materials ship in the filament catalog, and your changes move to
  `user_filaments.json`, which you can edit from Mainsail or Fluidd to change a material
  or add your own.
- **The Controls panel fits small screens** - Calibration & Tools holds only calibration,
  Motors Off sits beside Motion, and the Light becomes a Quick Actions choice. Advanced
  also gains a Pressure Advance row (#1452) and hides sections that are empty on your
  printer.
- **The log records more by default** - production builds log at Info and write each line
  as it happens, so a log from a problem you just hit already holds what happened.

### Fixed

**Crashes**

- **AD5X, K1, K1C and K1 Max: the touchscreen did not respond** - beta.1's build for these
  printers read touch events in the wrong size, so every tap was lost. Touch works as in
  1.0 again, and a barcode scanner on USB is read correctly too.
- **K2: the screen crashed and restarted during print start** - matching the printer's
  start-sequence messages could run out of the small stack these printers give each
  thread, with no crash report. K1, AD5X and Creator 5 Pro builds had the same limit.
- **Raspberry Pi 3 and older: a large G-code preview could freeze the screen** - the 3D
  preview now limits its detail to what the GPU can draw, and a file too big for that
  shows in 2D.

**Filament systems**

- **Happy Hare: a fault often showed no recovery popup** (#1323) - a fault during a print,
  or a failed load or home outside one, now always opens the popup with Happy Hare's own
  reason. Outside a print it leads with Recover instead of Resume. Turning the MMU motors
  on no longer homes the MMU.
- **Snapmaker U1: Unload could be unavailable, or the screen stuck on Unloading** - a head
  fed to the nozzle by purging can be unloaded, picking a tool no longer reads as loaded,
  and an unload that settles straight into its resting state, or one left over from
  before a restart, ends properly.
- **Snapmaker U1: bed drying stopped with "No unload macro found"** - the drying flow now
  unloads through the filament system, the same way the Unload button does.
- **Filament stuck in the toolhead with no slot claiming it could not be unloaded**
  (#1324) - on the AD5X and other systems that can tell, the sidebar and Filament panel
  Unload pull it from the active head, and a slot's own Unload greys out in that state.
- **Stock CFS asked "Same spool?" for a re-inserted spool** (#1710) - a tag that reads back
  the same values is recognised as the same spool. On AFC, a lane record from an older
  plugin build no longer replaces a spool edit you made.
- **A second filament system's slots showed the first system's materials**.
- **The heat-first warning checked the wrong slot** - Load and Unload check the slot you
  selected, and Purge, Extrude and Retract the loaded one.
- **The MMU's selector, buffer and bypass boxes ignored taps right after the screen
  opened**.
- **Device operation messages said "AFC" on every filament system** - homing, recovering
  and aborting now read the same on all of them.
- **The Change Filament spool picker could not tell two spools of one vendor apart** - each
  row now carries Spoolman's filament name.

**Screens and files**

- **Fan and light controls could show frozen dials** after being opened from two
  different places. Every entry point now opens the same live screen.
- **Scroll Guard had no effect** - with the post-scroll click guard turned on, a tap right
  after a scroll still went through. It works again, on the printer's touchscreen, and
  needs a restart after changing it.
- **The number keypad was cramped on 480x320 and 480x272 screens**.
- **.gcode.3mf files lost their thumbnail and layer height in the file details** (#1713),
  and print history showed untranslated job states and missing thumbnails for files in
  subfolders.
- **Mainsail kept showing the old version after an update** (#1727) - the first start
  after an update asks Moonraker to refresh it.
</content>
</invoke>

## [1.1.0-beta.1] - 2026-09-26

<!-- whatsnew
The first beta of 1.1.

- A rebuilt home screen: square grid, pages you drag widgets between
- Faster drawing on every board, GPU rendering on the Pi
- Pressure advance and belt tension, measured on the printer
- Tool offsets for tool changers, batch load/unload on the Snapmaker U1
- Macros remember their parameters
- Add-on chamber heaters, with diagnostics
- Your filament edits, the printer and Spoolman stop overwriting each other
- Print prep tracking with no config changes
-->

The first beta of the 1.1 line. It carries everything in 1.0.2 plus about two thousand
changes made on the trunk since 1.0 branched, and most of them land in four places.
The home screen was rebuilt on a square grid, and editing it finally works the way you
expect. Tool changers and multi-toolhead machines gained the tools they were missing:
per-tool offsets with automatic calibration, the multi-tool screens on IDEX and
dual-extruder machines, and on the Snapmaker U1, loading or unloading several heads in
one go. Add-on chamber heaters became first-class, with a diagnostics card that tells
you why a heater is not heating. And the filament screens now know where every piece
of lane information came from, so an edit you make, the printer's own record and
Spoolman's spool data stop silently overwriting one another.

**Why the version has a `-beta.1` on it.** Beta builds carry the release they are
heading for plus a prerelease suffix, which sorts below the plain `1.1.0` that the
stable 1.1 release will use, and above every `1.0.x` fix on the stable line. So a
hotfix on 1.0 never looks like an upgrade to you, and `1.1.0` arriving does. To get
this build, set the update channel in Settings to Beta.

**Upgrading from 1.0?**

- **Your home screen is converted, not reset.** Widgets keep their relative place: one
  that filled the left third of the screen still does, and widgets that touched still
  touch. Sizes can shift slightly because the new grid divides the screen differently,
  and a widget that genuinely no longer fits is re-placed on its own. Your widgets,
  their settings and your extra pages all carry over. The conversion runs the first time
  the home screen is drawn, and what you see then is what gets saved.
- **Going back to 1.0 costs you the home layout.** A 1.0 build saves home positions in
  its own grid's units, so when you return to 1.1 the home screen comes back rearranged,
  or, from some 1.0 builds, reset to its defaults. Arrange it again in edit mode.
  Everything else survives the round trip.
- **Your 1.0 settings are kept** (#1305). The first start of 1.1 saves your old
  `settings.json` beside the new one as `settings.json.pre-migration`, before converting
  it. HelixScreen never reads that copy back; it is there if you want your 1.0 settings.
- **Display sleep turns the backlight off, as in 1.0.2** (#1594, #1708). 1.0.1 powered
  the panel down at sleep, which left some AD5X, U1, K1, K2 and Pi DSI panels wedged or
  cycling colours on wake. The panel is now powered down only when there is no backlight
  to turn off. The `/display/panel_power_off` setting in settings.json still forces it.
- **Print preparation tracking needs nothing from your config** (#1234). The phase
  tracking toggle, the installer's `--with-phase-tracking` flag and the plugin service
  that rewrote PRINT_START are gone; see Changed below for what happens to a PRINT_START
  that was already instrumented.
- **Some installs move on disk, automatically.** On the AD5M, HelixScreen's own files
  move to `/data/.helixscreen` so they stop showing up in the print file list. On the K2,
  the install moves off the 240MB system overlay onto the user partition.
  Both are migrated in place by the update.
- **`helixscreen.env` values are taken literally** (#1682). The file is no longer run
  through the shell, so `$VAR`, `${VAR}`, `$(command)` and backticks are not expanded.
  A line using them is skipped with a warning in the log, so write the final value
  itself. Only the settings documented for this file are accepted; anything else is
  skipped and logged. A few values are checked before they are used: the log file has
  to be a `.log` under `/tmp`, `/var/log` or the install folder, and `HELIX_NICE` has to
  be 0 to 19. An env file that the web interface can edit, like the one the Snapmaker U1
  keeps in `printer_data`, is honoured.
- **A custom update server moves out of `settings.json`** (#1718). `settings.json` can be
  edited from the web interface, so it no longer chooses where updates are downloaded
  from. To point a printer at your own server, put `r2_url` or `dev_url` in
  `/var/lib/helixscreen/update_urls.json`, a file only root or HelixScreen's own user may
  own. The old keys in `settings.json` are ignored, and the log says where to move them.
  A custom `log_path` has to sit under `/tmp`, `/var/log` or HelixScreen's own folders.
- **A refused `helixscreen.env` says so on screen** (#1712). When the file cannot be
  trusted (wrong owner, or still writable by others after the automatic repair) the
  launcher used to fall back to defaults with nothing on the display. Now a startup
  warning says what is wrong, names the one command that fixes it, and stays on screen
  until closed; lines the launcher skips (a setting this file may not change, shell
  syntax in the value, a bad quote) are listed with the key and the reason each was
  ignored.

**Still being proven.** Two features in this beta are hidden unless beta features are
on, because they have not yet been checked on enough real machines: the belt tension
tuner, which has never measured a real belt, and the Tool Offsets screen. Several
filament-system changes below were built from captured printer data rather than on the
hardware itself, and say so where that is the case. Reports from owners of those
machines are exactly what a beta is for.

### Added

**Home screen**

- **One square-cell grid on every panel** (#1126, #1559) - the home screen divides both
  axes by the same cell size, so a cell is square on every display and a rotated panel
  transposes its layout exactly. Widgets place and resize at half-cell steps, three times
  the resolution the old grid had. Every shipped
  layout was redrawn for the new grid, including a new ultrawide layout for 1480x320 and
  1920x440 panels, which used to draw into the left fifth of the screen.
- **Edit mode shows where a widget can land** - whole-cell dots are always drawn, and the
  half-cell dots between them appear only while the selected widget can use them. A
  visible dot is a legal drop target.
- **Pages swipe in edit mode, and widgets move between them** (#1638) - carry a widget
  past a page's edge, or hold it there, and the next page slides in under your finger.
  Add a page by tapping the "+ Add page" tile one swipe past your last page, in edit
  mode or not; it is there even when you have a single page. In edit mode you can also
  drag a widget past your last page: an empty page slides in, and dropping the widget
  there creates the page. At your first page's left edge nothing slides in: let go with
  most of the widget past that edge and a new page is created in front of it. At the
  8-page limit there is no "+ Add page" tile and neither edge creates a page.
- **The widget catalog opens by category, with search** (#1016) - thirty-seven widgets in
  one scroll had become something you hunt through, worst on 480px panels. The catalog
  now opens on five categories, has a search box that matches names and descriptions,
  and lists widgets this printer cannot use in their own section with the reason. Widget
  names and descriptions are translated for the first time.
- **UI Scale setting, with a layout per scale** (#1484) - Settings -> Display & Sound
  gains a UI Scale choice: Automatic, which shows the scale it picked, or a fixed 100 to
  200%. Each scale keeps its own home arrangement, so trying a different scale and coming
  back restores the layout you had, which 1.0 could not do. On very high-density
  displays such as phones, Automatic now scales by the screen's real pixel density
  instead of drawing everything a third of its intended size.
- **Proper sizing on 1080p and larger panels** - a 1080p screen used to get the same
  widget size as a 720p one while text and icons grew 1.6x around it, so labels ran out
  of their boxes. Large panels have their own size tier now.
- **Navigation buttons read by shape, not just colour** - inactive buttons use outline
  icons and the active one is larger and filled, which survives washed-out panels and
  colour-blind eyes. (thanks @just-trey)

**Home screen widgets**

No widget types were added or removed; most of the existing ones changed.

- **Icon tiles resize** - the single-icon tiles (power, lock, shutdown, firmware restart,
  LED, network and the rest) scale with their tile instead of staying one fixed size, and
  can be resized for the first time. On most panels they take half-cell steps in width;
  on the smallest panels, and for the Power tile, they keep whole cells. A widget refuses
  a size it cannot draw, and edit mode shows that size in red.
- **The filament sensor tile is tappable** - a tap opens its dialog, where Load, Unload,
  Purge, Resume and Cancel Print all work. Which sensor the tile watches is picked in
  edit mode.
- **The clog meter is a readable scale** (#1017) - at one cell the gauge, its value and
  its label were crammed into a box narrower than the words. It is now a two-cell
  horizontal scale labelled at each end (TANGLE and CLOG for FlowGuard, SAFE and FAULT or
  a detection length for the others), so a reading tells you which fault it is heading
  toward. Tapping the FlowGuard tile shows the actual reading instead of only the mode.
- **The temperature and fan stacks adapt to their shape** - given height, they put each
  icon above its reading; kept compact, they sit side by side.
- **The nozzle temperatures tile fits any size** (#1613) - it picks its font by the
  tile's height and its labels by width, shortening labels before shrinking numbers.
- **Choose which camera a widget shows** (#1487) - a camera widget's settings list
  Automatic plus every webcam the printer has, noting snapshot-only feeds and cameras
  that were ruled out. The choice is remembered by camera name, so it survives an address
  change, and falls back to Automatic if that camera goes away.
- **Scheduled pauses are marked on the progress bar** (#1509) - filament changes (`M600`),
  `PAUSE`/`M601` and slicer pause-at-layer stops each draw a tick where they fall in the
  print, on the home print card and the print screen. A file sliced with `M73` progress
  places the ticks at the slicer's own percentages.
- **The bypass tile works** - it could be added but never appeared, and it showed as
  engaged whenever the printer did not report bypass at all.
- **Firmware restart and job queue tiles** no longer clip, and icons step down a size on
  large panels, where ten widget and screen-size combinations were clipping.

**Tool changers and multi-toolhead printers**

- **IDEX and dual-extruder printers get the multi-tool screens** (#1350) - a printer is
  now recognised as multi-tool by counting its hot ends, so IDEX machines, dual
  extruders and tool changers that do not run klipper-toolchanger get the tool selector
  and filament screens instead of the single-extruder ones, and `T0`/`T1` tool switching
  works on them. Built from test configurations; not yet verified on these machines.
- **Tool Offsets screen with automatic calibration** (#1480) - per-tool X, Y and Z offsets
  in one place, and a one-tap calibration that runs your printer's own routine, follows
  it live, and saves every axis with a single restart. Where that routine calibrates
  every tool, the manual paper test is hidden. Beta-gated for now. (thanks @Monstrofil)
- **Per-tool Z offset from Print Tune** - Print Tune has a Global / T0..Tn selector, so
  each tool keeps its own first-layer height, and the save button appears whenever any
  tool's offset is unsaved and writes it permanently. It used to vanish after the next
  Klipper restart.
- **Snapmaker U1: load or unload several toolheads at once** - a picker lists every head
  with what it holds, pre-ticks loaded heads for an unload, and says which heads cannot
  take the operation and why. The printer's own batch command does the work, preheating
  the next head while the current one finishes, and the header names the head being
  worked on.

**Filament systems**

- **Your edits, the printer and Spoolman stop overwriting each other** (#1649, #1653) -
  every piece of lane information now has a known source, and the more trustworthy
  source wins instead of whichever wrote last. A colour you pick for a lane beats the
  linked spool's and keeps its colour name. A spool linked from Spoolman owns its brand,
  material and name (read-only while Spoolman is unreachable), and its remaining weight
  comes from the server. Your edits on AFC, Happy Hare and ACE are marked as yours in the
  printer's shared lane record, so Mainsail or Orca refreshing it does not wipe them. A
  reading from one source that leaves a field out no longer blanks what another source
  knows. When a printer repeats an edit you made back to the screen, it is still read as
  your edit, not as the printer's own data, so clearing it brings back what the printer
  really reports (#1633). When another tool writes a lane after you did, the newer edit
  wins, whichever side made it (#1632).
- **A new spool is judged by what the hardware read** (#1710) - when a spool goes into a
  slot, a different tag, or a different material or colour read off the spool, clears the
  old spool's details; a matching read keeps them. When the hardware can't tell (an
  untagged spool, or a slot the firmware only remembers), your details stay and a small
  "Same spool?" notice offers Clear. It is not shown on the lane feeding a print, or for a
  slot with nothing to clear.
- **Spool swaps are noticed, even with the screen off** - each lane remembers a
  fingerprint of the spool it held, so a spool swapped while HelixScreen was not
  running clears the old spool's edits instead of painting them onto the new one.
  Works on QIDI Box, the Snapmaker U1 (by the spool's RFID tag) and Creality CFS,
  including Kalico-based CFS setups.
- **QIDI Box lanes behave like every other system** (#1632) - slot presence, swap
  detection, edits that persist and repaint, and a Clear Spool that clears the Box's own
  records.
- **Humidity and drying per box** - the filament environment screen follows the way
  your hardware actually encloses filament: one heated enclosure over four slots, a
  sealed box per lane, or one box per unit. Each box shows its temperature and humidity
  and names the slots it covers, and its dryer is set from the numeric keypad within
  what that box accepts. A single box opens straight to its details, a few identical
  ones get tabs, and boxes that can do different things are listed side by side. When
  the dryer can only heat some boxes at a time, the waiting ones say they are queued
  and which box is using the heater.
- **Favourite filaments** (#1100) - star any row in the filament catalog. Favourites lead
  the vendor list and float to the top of their vendor's view, on every printer.
- **Load and Unload for the external spool** (#1486) - the bypass spool's menu on the
  filament system screen now has its own Load and Unload, engaging bypass first when
  needed, so you no longer have to go to the Filament screen for it.
- **ACE shows where the filament actually is** (#1677, #1678) - the path reads the ACE's
  hub and toolhead sensors, so a strand parked short of the hub is drawn there rather than
  back at the spool. An ACE Pro with a fifth spool on its bypass switch gets a working
  bypass control.
- **AFC buffers with a pressure sensor drive the buffer meter** - an `FPS_PSF` buffer
  reports what Happy Hare's sync feedback reports, so the buffer meter and path tint
  work on AFC unchanged. Not yet verified on hardware.
- **AD5X tool remapping uses the IFS's own commands** - the screen reads and writes the
  printer's tool map through `IFS_MAP_TOOL`, so a remap matches what the printer reports.
- **Spool labels lead with the spool number** (#1491) - on every label layout the spool
  number is the first and largest line, and weight moves to its own line so a narrow tape
  truncates that rather than the number.

**Chamber heaters**

- **Add-on chamber heaters are detected and explained** (#1290) - DragonBreath and Panda
  Breath heaters are found at startup, each with its own safety ceiling. On the
  temperature screen, the chamber card shows any fault with a translated reason and a
  Reset button, the element temperature, and the filter fan with its own toggle, so the
  graph keeps the rest of the screen. Like
  every heater, the chamber heater's status also shows how hard it is working as a
  percentage. A heater that has dropped off the network says Offline instead of offering
  a target it cannot reach. The Panda Breath's status format has not been verified on a
  stock unit.

**Printing**

- **Heater duty on every heater** (#1290) - the Controls and print status screens show how
  hard each heater is working as a percentage beside its status, so a nozzle running flat
  out and falling behind looks different from one holding temperature easily.
- **Chamber temperature per material** (#1263) - Material Temperatures has a chamber
  column, editable on printers with a chamber heater.
- **Pick the next file while one prints** (#1395) - a Files button on the print status
  screen opens the file list mid-print. Starting a print stays blocked until the current
  one ends, and the start button says so.
- **K2 with the k2-improvements mod** - its print start sequence is recognised, so the
  preparation steps are named correctly, heat soak and the chamber wait show as soaking,
  and the bed is no longer meshed twice.
- **Camera button on remote screens** - running HelixScreen against a printer on another
  machine, the print screen gains a Camera button for the full-screen webcam view. It
  stays hidden on the printer's own screen.
- **Macros remember their parameters** - save default values for any macro that takes
  parameters, from edit mode in the Macros panel or a home Macro Button's options. With
  Ask for parameters on, the form opens already filled in; switch it off and the macro
  runs straight away with the saved values. Defaults are kept per printer, and a
  dangerous macro still asks for confirmation.

**Calibration and tuning**

- **Pressure advance, measured by the printer** (#1452) - on printers that can measure it
  themselves, a Pressure Adv. button on the Controls panel runs the measurement for the
  tool you pick and shows the result, flagged if it's outside the usual range. The
  Snapmaker U1 applies and keeps the value; on the FlashForge Creator 5 Pro you copy it
  into the slicer. Takes a few minutes and heats and purges. (thanks @Monstrofil)
- **Belt tension by plucking** (#1303, #1231) - park the gantry, pluck each belt by hand,
  and the tuner listens on the accelerometer and reports the belt's frequency as the
  median of five good plucks, with a live waveform and spectrum. It reads the whole
  harmonic series rather than the tallest peak, which is what made the old driven sweep
  report some belts a full octave sharp. Beta-gated: this has not yet measured a real
  belt, and it stays hidden until it has (`BELT_TUNER.md` has the validation plan).
- **Switch a Kalico heater between PID and MPC** (#1237) - the method selector on Kalico
  printers is out of beta. It migrates your config in both directions, and cancelling a
  running MPC calibration asks first, because cancelling means an emergency stop and a
  firmware restart.
- **Input shaper saves the shaper you picked** - the chips under the results graph are
  now one choice per axis, and Save writes that choice to your config. Save used to
  write Klipper's own recommendation regardless of what you selected.
- **Motion settings** - jog speeds and step distances are yours to set, under Settings ->
  Printing -> Motion or from the cog on the Motion screen's header. Separate XY and Z
  speed sliders, and all six step distances (the jog pad's inner and outer rings and the
  two Z buttons) take any value you type. Speeds are capped at the printer's own speed
  limit, including one the printer reports after you set them, and Reset Distances asks
  before wiping your values.
- **Z jogs stop at the axis limits** - a Z jog that would run past the end of travel stops
  at the limit and warns once, instead of sending a move Klipper refuses. X and Y already
  worked this way.
- **Your baby-step size is remembered** - the Z-offset step you last used is the one you
  get next time.

**Printers**

- **VzBot 330 and 235** (#1689), **FLSUN S1 and S1 Pro**, and the **base Creality K2**
  (#1606), each with artwork. The base K2 is now told apart from the K2 Plus and Pro, and
  its camera option no longer claims the Plus's AI model.
- **Snapmaker U1 firmware settings** - the pre-print toggles (bed mesh, input shaper,
  pressure advance, timelapse) read and set the firmware's stored values instead of
  always reading off. Tangle sensitivity, end-of-print unload per toolhead, and manual
  bed levelling with a PEI plate check are all on screen.
  Loading preheats to the firmware's own temperature for that filament. Firmware faults
  appear with translated wording.
- **Snapmaker U1 with the multiACE mod** (#1426) - keeps its filament screen instead of
  being mistaken for a plain ACE setup.

**Small and portrait screens**

- **The Motion screen gives the jog pad more room** - the X, Y and Z position moves into
  the header, so on a landscape panel the jog pad takes the width the position card used,
  about a third wider at 480x320. An axis that is not homed shows its position greyed
  out. On a portrait panel the jog pad sits beside a tall Z column, with the jog modes and
  levelling buttons in one row underneath.
- **The Filament screen fits small and portrait panels** - on a portrait panel it becomes
  one column, and the temperature graph shows only when there is room for it, with a graph
  button on the spool strip to open it otherwise. The Load, Unload, Purge, Extrude and
  Retract buttons are full size and stay put whether or not the cold-extrusion warning is
  showing. Cool Down keeps its place in the material grid, greyed out while nothing is
  heating. The tool picker shows the tool's number (or the name you gave it), and on the
  Snapmaker U1 it is labelled Toolhead.

**Display and sound**

- **Bouncing Printer screensaver** (#1680) - the screensaver everyone already knows.
  Your printer drifts across a black screen and reflects off the edges, picking up a new
  tint on every wall. Land a true corner and it celebrates. The sprite is whatever
  printer this screen is attached to, auto-detected or picked by hand, so a Trident
  bounces a Trident. It costs a fraction of what Flying Toasters does; on
  AD5M-class hardware the corner celebration is a backdrop flash rather than confetti, to
  stay clear of the print loop. Select it under Settings -> Display & Sound ->
  Screensaver. (thanks @Tintef)
- **Fireworks screensaver** - shells rise from behind a line of hills under a starry
  night sky, trail sparks, and burst as peonies, chrysanthemums, willows or rings, each
  spark fading from white through its shell's colour to an ember. It adjusts how many
  sparks and bursts it draws to what the board can afford. Select it under Settings -> Display & Sound
  -> Screensaver.
- **All five screensavers on every board** - Flying Toasters, Starfield, 3D Pipes,
  Bouncing Printer and Fireworks now run on the AD5M and Centauri Carbon too, which had
  none, and on every other board with a 16-bit display. Each saver draws less on a
  slower board rather than stalling it, and the starfield redraws only what moved, so it
  costs a small fraction of what it did.
- **The AD5X buzzer plays chords and music** - UI sounds play as chords, tracker music
  plays in four voices, and `M300` beeps from your macros take the buzzer when Klipper
  holds it.
- **On-screen keyboard keys look like keys** - each key has a raised edge in its own
  colour instead of a flat black shadow.
- **Hardware rotation with touch that follows** (#1275) - a panel turned 180 degrees can
  rotate on the display hardware where it supports it, and touch, mouse and touch
  calibration all follow the turned picture. Quarter turns stay in software, which is
  the only way touch can follow them today.

**Setup and settings**

- **Find your printer by searching** (#1028) - first-run printer selection replaces the
  scroll through about 105 machines with a search box, a grid of vendors and a list per
  vendor.
- **Install HelixScreen's helper macros from Settings** (#1271) - a row under Advanced
  installs or updates the macro pack without running the installer. It backs up
  `printer.cfg` first, restarts Klipper right away when idle, offers the restart for
  after the print when one is running, and refuses to touch anything mid-print.
- **Hardware Health shows its status** - the Settings row carries the live result with a
  severity-coloured icon, and critical problems get their own alert icon.

**Files and USB**

- **USB sticks mount on boards that do not mount them** - where nothing else mounts a
  USB stick, HelixScreen mounts it read-only itself and unmounts it on shutdown. Sticks
  plugged in while the app runs appear within about a second, and short DOS-style names
  such as `3DBENC~1.GCO` are recognised as G-code.

**Installing and updating**

- **One package for the K1 series and the AD5X** - both boards run the same build, which
  is smaller than the K1C's was. It is still published under each board's name, so
  existing updaters keep finding it.
- **Smaller downloads** - packages carry only the splash screens and printer images the
  panel's resolution can show (K2 assets 42MB to 9MB, Centauri Carbon to 2.8MB), and
  embedded packages drop music the machine cannot play.
- **Logs and caches survive an update** - on the K1, Centauri Carbon, U1 and AD5X, a web
  update used to delete the logs you needed and throw away the thumbnail cache. Both now
  live beside the install instead of inside it.

### Changed

- **Clear Spool forgets everything** (#1661) - it erases everything HelixScreen and the
  printer's firmware remember about the slot, leaving only what the hardware can
  physically read. That includes the printer's own record: the whole gate map entry on
  Happy Hare, and the slot record on AFC, AD5X, QIDI Box and Kalico-based CFS. ACE, the
  Snapmaker U1 and stock CFS keep their own record, because their firmware offers no way
  to erase it or re-reads the spool's tag. Clearing a single field brings back what the
  machine reports for it. Clear Spool is refused on the lane feeding a print that is
  running, paused or preparing, and the menu says why. On Happy Hare in Spoolman pull
  mode, clears and edits are refused with Spoolman named as the reason, instead of
  appearing to work.
- **Emptying a page in edit mode removes it** (#1638) - moving a page's last widget
  elsewhere, or removing it, deletes the empty page. The main page always stays, and so
  does a page holding widgets greyed out because their hardware is not detected.
- **Print preparation phases are inferred** (#1234) - the screen works out the current
  phase from toolhead movement and temperatures, on any printer, without editing its
  config. A PRINT_START that already carries `HELIX:PHASE` markers keeps working.
  Uninstalling the plugin (Settings -> Advanced, or `install.sh --uninstall`) removes those
  markers from PRINT_START, backing up each file it edits as `<file>.bak.<timestamp>`;
  the change takes effect at the next Klipper restart. The Uninstall row now reports
  what actually happened (a clean removal, one that left a file for you to look at, or a
  failure) instead of always saying it worked.
- **Tool changers without ASSIGN_TOOL remap without beta features** - the job file is
  rewritten before printing whenever remapping needs it, and the plugin's Install row is
  back in Settings -> Advanced, no longer beta-gated. A dialog at startup warns when
  Moonraker is too old for remapping, and can be dismissed per printer.
- **Loading filament does not preheat twice** (#1494, #1495) - when the filament system,
  your macro or the "allow cold load" setting already handles heat, the screen no longer
  preheats first. On QIDI printers this stops a preheat that the stock macro then
  overrode.
- **Temperature keypads stop at your printer's maximum** (#1615, #1619) - and say what
  the limit is when you hit it. Material temperature presets are not limited, so an ABS
  preset above a 260 degree hotend's limit saves fine and is applied at the limit.
- **K1: Creality Print keeps working** (#1468, #1637) - installing HelixScreen on a K1 no
  longer stops Creality's backend services, so Creality Print can still reach the
  printer. Only the stock screen is replaced.
- **K2: spaghetti detection keeps working** (#1378) - installing HelixScreen takes over the
  camera and the stock detection service, so HelixScreen now runs the K2's own AI model on
  camera snapshots during a print instead. It follows the printer's AI settings the first
  time, then Settings -> Safety & Notifications -> Spaghetti Detection and Pause on
  Detection are yours. The alert is translated and shows the detection's confidence, and
  a print that is already paused is not paused again. Uninstalling restores the stock
  service.
- **Cool Down works on the base K2** - it no longer errors on a K2 whose chamber has a
  fan but no heater. Your own customised cooldown macro is never rewritten.
- **The chamber heater owns the chamber reading** (#1465) - a probe named "chamber" no
  longer hides the U1's cavity sensor. A sensor you assign by hand still wins.
- **Snapmaker U1: a print with an unset spool is stopped before it starts** - a tagless
  spool with no material set is caught up front, instead of the printer heating and
  homing and then reporting a runout minutes later.
- **AD5M: no startup music** - playing it ties up the AD5M's single core, and it has
  killed prints. The AD5X keeps its music.
- **Forge-X: installs stay put** - HelixScreen installs outside the folder Forge-X
  cleans (either of its clean actions deleted the install), and an install that
  replaces an older one removes it, which could hold 81MB and put two screens on the
  display at boot. A Forge-X install refuses `--auto-update`, which would have wiped
  your preserved settings.
- **QIDI: the stock screen stays off** (#1533) - it is disabled before it is stopped and
  kept off at every boot, so it no longer comes back after a restart. Installing sets
  aside only the screen software that is actually present, and restores only that.
- **Uninstalling HelixScreen also removes its Moonraker plugin**, so Moonraker stops
  logging an error for it on every boot.
- **Self-built and forked builds do not send diagnostics** (#1410) - debug bundles and
  crash reports upload only from official release builds by default.
  `HELIX_DIAGNOSTIC_UPLOADS=1` turns them on.
- **Tools and lanes are numbered from 1** (#957) - everywhere the screen names a tool, a
  nozzle or a lane for you to read, it counts the way people do: "Tool 1" and "Nozzle 1"
  where it used to say T0, including the pre-print filament check and the tool switcher.
  G-code, macros and the console keep the T0 spelling the printer expects.
- **Portrait home ships the print status card in its Detailed style**, which adds the
  Job Queue button.
- **The empty Plugins screen is gone** from Settings -> System (#1235).

### Fixed

**Performance**

- **Faster drawing on every board** - the screen draws directly instead of through a
  separate render thread, which measured cheaper on every shipping board. The K1 family
  gains an image cache that cuts home screen drawing by about 14%, the home screen loads
  its printer picture at the exact size first instead of rescaling on every visit, and
  the theme skips rebuilding when nothing changed.
- **Raspberry Pi: GPU presentation where the Pi has a hardware renderer** (#1580) - the
  Pi package includes a build that hands finished frames to the GPU. The launcher uses
  it only when it finds a real hardware renderer, and falls back to the standard display
  path if it is declined or crashes.
- **Tool remapping streams the file** instead of holding the whole G-code file in memory.
- **Turning animations off stops all of them**, including screen transitions and the
  heater icon pulse.

**Crashes and stability**

- **One Klipper fault, one dialog** - a shutdown that names its reason no longer stacks a
  Printer Error alert under the recovery dialog. While the recovery dialog is up, a
  further fault shows as a notification and in the notification history instead of
  another dialog.
- **K1 crash while drawing** (#1673) - an image could be freed while the display was
  still drawing it. Teardown now waits for drawing to finish, including behind every
  dialog.
- **Crash tapping jog controls after switching printers** (#1707), and screens from the
  previous printer are freed instead of kept in memory.
- **Unplugging a touchscreen or mouse** could crash the app or leave input dead.
- **A bad byte in a Wi-Fi name, printer name or file name** (#1493) could stop the app or
  silently lose a whole settings save.
- **Small-memory boards** (#724) - a background thread that failed to start could take
  the whole app down.
- **AD5M Pro crash loop** - the watchdog could retry a crashing app forever with the
  screen dark. It now gives up and hands over to the system.
- A whole class of crash when leaving a screen, where a screen reacted to a value that
  had already been freed, is closed off across the app.

**Network**

- **A printer whose address changed was retried at the old one forever** - the Android
  app hanging mid-print until a restart. The printer's name is looked up again before
  every reconnect.
- **A brief disconnect no longer brings up the connection-failed dialog** meant for a
  printer that was never reached.
- **An administrator's radio block is left alone** (#1697) - HelixScreen stopped turning
  Wi-Fi on at boot on machines where it never set Wi-Fi up.
- **Joining Wi-Fi while Ethernet is connected** (#1542) says so immediately, instead of
  timing out after 45 seconds.

**Filament systems**

- **Kobra S1 with the ACEPRO driver never showed a loaded slot** (#1069) - the loaded
  tool is now read from where that driver reports it. Built from a user's captures; not
  yet verified on the machine.
- **The old spool's brand stayed on a lane** (#1672) - when Spoolman dropped a spool, or
  another tool (Mainsail, a macro, the MMU's own screen) swapped one, the outgoing
  spool's details lingered. Seen on AD5X IFS and Happy Hare.
- **Creality CFS** (#1623, #1625, #1512) - restoring the slot mapping at startup refused
  every slot on a box whose size was not yet known, and threw away the record a
  reattached box needed. Clearing a slot left a dangling Spoolman link, and bypass after
  an unload could stay armed forever.
- **AD5X IFS** (#1631, #1654, #1626) - your own colour edit was recorded as if the printer
  had reported it, colours set from the stock screen were misread, and the filament
  system screen never showed a runout.
- **Tool changers: a tool change is no longer reported as a dock fault** - while a tool
  travels between dock and head, its sensors read the same as a fault, and the screen
  raised an error on the first moment of an ordinary swap. Real dock faults still show,
  with one translated wording.
- **An ACE load the driver declined** (#1676) still marked the slot as loaded.
- **ACE stuck on Unloading** (#1720) - an unload that never reached the printer left the
  ACE marked busy until restart.
- **AFC in toolchanger mode** draws its real toolheads (a Box Turtle as four heads, not
  one nozzle behind a hub), single-extruder machines no longer get an "Unknown action"
  toast, and the bowden setting is not offered without a hub.
- **AFC re-announced a resolved error at every start** (#1589) - a message AFC had
  already cleared popped up as new each time HelixScreen connected.
- **Resume and Cancel did nothing** in the paused dialog opened from the filament tile,
  and declining a cancel confirmation left you on a bare screen.
- **One tap on the filament tile disarmed the runout warning** for the rest of the
  session.
- **The clog threshold disappeared** into the fill exactly when the reading crossed it,
  and the loaded-spool card cut filament names short ("Polymak").

**Printing and preview**

- **Creality error codes are explained on every printer** (#1513) - pre-heat, MCU link
  and out-of-range errors showed as raw firmware codes on printers without a CFS.
- **Print preparation shows what the printer is waiting for** - during a long heat-up
  after the firmware's last announced step (such as the Snapmaker U1's plate check), the
  screen names the heater still short of its target, the bed, the nozzle or the chamber,
  and the nozzle keeps its label until it is within 2 degrees. If Klipper shuts down
  during preparation, the progress display stops instead of counting on for up to half an
  hour.
- **A timed-out print request could start a second print.**
- **A start macro that heats before replying** (#1451) no longer shows a timeout dialog
  over a print that started fine.
- **OrcaSlicer calibration prints** (pressure advance, flow, retraction) preview instead
  of drawing blank, and **G-code with signed coordinates** like `G1 X+10.5` (#1658)
  previews completely.
- **A preview from the previous print** could replace the running print's preview, layer
  count and pause ticks.
- **A false "Failed to load G-code preview"** appeared about a minute into a print started
  while the screen was asleep with its panel powered down.
- **A file moved out of the G-code folder** (#1575) stayed listed until you left the
  screen.
- **The budget preview on small boards** (#1555) keeps its tool colours and framing.
- **Queued jobs are checked before they start** - tapping a queued job refuses one that
  contains a command the printer would turn into an emergency stop, as the file detail
  screen always did.
- **Deleting a print history record asks first** (#1373), and **View Timelapse** opens
  the timelapse browser and plays videos stored on the printer.
- **The unread history badge** (#1525) was cleared by a history screen you were not
  looking at.
- **K2 Plus: saving config flashed "Printer Shutdown"** during the normal restart.
- **Qidi Q2: the print thumbnail** showed a sample benchy for the whole print.

**Calibration**

- **Input shaper progress** assumed a fixed 5-100 Hz sweep, so on printers configured to
  sweep higher the bar filled with a quarter of the test left and "Analyzing" sat for the
  last half minute. It follows the real range now.
- **MPC calibration outliving its timeout** (#1544) was reported as failed with Retry
  offered over the still-running calibration.
- **Centauri Carbon** (#1529) - screws tilt, probe accuracy and Z-offset no longer probe
  on an untared load cell, and the tare uses the command COSMOS actually defines.
- **Dismissing the input shaper memory warning** made the next single-axis run carry on
  into an unrequested Y sweep.
- **Z-offset** kept showing an unsaved change after a successful save, and could not tell
  an adjustment clamped at the limit from a failed one.

**Home screen**

- **The printer picture** showed a generic silhouette until restart when detection
  finished late (#1552), and on the K1 family drew blank for up to a second on first
  show.
- **Widgets did not fit large panels** - text sized for a small screen clipped on 1080p:
  fan names cut off, one letter per line on the spool tile, the temperature graph
  overlapping its own labels. Checked on nine screen sizes from 480x272 to 1920x1080.
- **The nozzle temperatures tile** (#1613) clipped at odd sizes, and **half-cell widgets
  lost their background card**.
- **Narrow widgets could only be resized, never dragged**, dropping a widget on an
  occupied spot selected the wrong one, and tapping Done on a later page jumped back to
  the first.
- **Long names show both ends when they are not scrolling** (#1441) - with animations
  off, a file name too long for its space shows its start and end with an ellipsis
  between them, instead of being cut off at the edge, so two jobs that differ only in
  their suffix no longer look identical.
- **A notification closing cancelled your drag**, slider or scroll anywhere on screen.

**Display and touch**

- **Rotated panels** (#1580) - touch no longer drifts from the picture on displays using
  hardware planes.
- **Panels that report their touch range sideways** (#1450) - one touch axis was squashed
  and the other clipped.
- **K2: the dimmest brightness blacked the panel out** (#1709) - on community K2 firmware,
  whose sysfs backlight goes dark at low levels, the slider's minimum now lands on the
  lowest level the panel shows. Stock K2 firmware keeps its full dimming range.
  `/display/backlight_floor_percent` sets the floor on any panel.

**Printer identification**

- **Extra heaters counted as tools** - a heater named like `extruder_mixing` could turn a
  three-tool machine into a four-tool AD5X.
- **Creality CFS machines** (#1498) - plain and CFS K1 variants, and the K2 Plus and Pro,
  are told apart reliably.
- **Delta printers** (#1607) no longer save the wrong vendor on a shared hostname, and
  the Kobra 2 and Qidi Max 4 identify exactly.

**Snapmaker U1**

- **Faults reach the screen** - standing faults such as power loss were not shown at
  all, and coded faults showed raw console text. A spaghetti pause is no longer called
  a dirty bed, and a refused resume over an unset filament explains itself.
- **Per-filament temperatures and per-head end unload** did not work against the real
  firmware.

**Setup**

- **The step counter jumped mid-wizard** (#1550), from "Step 2 of 6" to "Step 3 of 3".
- **AD5M Pro on Forge-X reopened the setup wizard on every boot** after a reconfigure.
- **Closing the theme explorer** discarded a theme you had just saved.

**Installing**

- **Fresh K1 and AD5X installs failed to download**, and a failed download left the
  stock screen switched off with nothing installed in its place.
- **Uninstall** now removes HelixScreen's leftover folder instead of only emptying it.

**Translations**

- **Filament system errors are translated** for the first time, across AFC, Happy Hare,
  AD5X IFS, CFS, ACE and QIDI, and the CFS load failure no longer overflows or cuts a
  character in half.
- **Gate and lane** (#957) - 26 phrases used the word for "slot" where English says gate
  or lane.
- **Japanese and Chinese** (#1620) - missing characters showed as boxes; the material
  temperature table (#1263) fits small screens and is translated; the new motion, batch
  filament and U1 fault text is translated in every language.
- **Text on coloured buttons** (#1648) keeps the theme's tint at readable contrast
  instead of dropping to plain black or white.

## [1.0.2] - 2026-09-25

<!-- whatsnew
The second patch release on the 1.0 line.

Display sleep is fixed: panels that wedged, glowed or showed a test pattern on AD5X, U1,
K1, K2 and Pi DSI now sleep by turning the backlight off, and the screen comes back
properly on wake. Several crashes are gone, including the K1 blank-screen crash and
crashes after a printer switch. QIDI printers get their .3mf thumbnails back.
-->

The second patch release on the 1.0 line. The headline is display sleep: 1.0.1 powered the
panel down alongside the backlight, and on several boards that wedged the panel or lit it
up instead of turning it off. 1.0.2 goes back to switching the backlight off and powering
the panel down only where there is no backlight to switch. The rest is mostly stability,
on K1 class boards and around printer switching, plus QIDI install fixes and readable text
on coloured buttons.

### Added

- **The filament mapping chip names the material** - the top band of each tool's chip
  shows the file's material for that tool (PLA, ASA) on the tool's colour, falling back
  to the tool number when the file names none.

### Fixed

**Display sleep and backlight**

- **Sleep turns the backlight off and leaves the panel powered** - powering the panel down
  alongside a working backlight wedged or lit up panels on AD5X, U1, K1 and K2 (the K2
  Plus glowed and flickered white at the edges) and showed a colour test pattern on a Pi 4
  DSI screen. Sleep now switches the backlight off, and powers the panel down only on
  boards with no backlight control. `/display/panel_power_off` turns panel power-off back
  on for a screen that needs it
  ([#1594](https://github.com/prestonbrown/helixscreen/issues/1594),
  [#1708](https://github.com/prestonbrown/helixscreen/issues/1708)).
- **Waking from sleep brings the screen fully back** - after dimming, then the screensaver,
  then sleep, the panel on screen woke up frozen: its live values and timers stayed
  stopped, and every later screensaver skipped pausing it.
- **The K2 brightness slider no longer blacks out the screen at its low end** - the lowest
  settings map onto the panel's visible range, with a `/display/backlight_floor_percent`
  setting for other panels that go dark early
  ([#1709](https://github.com/prestonbrown/helixscreen/issues/1709)).
- **No false "Failed to load G-code preview"** when a print starts while the display is
  asleep.

**Crashes and stability**

- **K1 class boards no longer crash drawing a QR code or an image** - a canvas buffer could
  be mistaken for a PNG and drawn at a nonsense size, and draw buffers could be freed while
  a render was still reading them
  ([#1673](https://github.com/prestonbrown/helixscreen/issues/1673)).
- **Switching printers no longer sets up a crash** - a toast left over from the previous
  printer stayed on screen forever and crashed the app when tapped
  ([#1719](https://github.com/prestonbrown/helixscreen/issues/1719)); the motion, console
  and macros overlays reopened as stale copies, and tapping home on the motion overlay
  crashed ([#1707](https://github.com/prestonbrown/helixscreen/issues/1707)); and home
  pages could keep drawing with layout data the switch had freed.
- **Switching printers no longer leaks memory** - every overlay, the numeric keypad and the
  AMS panels opened before a switch stayed allocated as hidden copies, several hundred KB
  each, for the rest of the session. They are freed now.
- **A brief connection drop no longer raises the connection-failed dialog** - a printer
  that had connected and lost Moonraker for a few seconds was treated as one that never
  connected.
- **Lower CPU use while drawing** on every shipped board, up to a quarter less on some
  screens.

**Touch**

- **Touch lines up on panels whose driver reports its range sideways** - portrait panels
  such as the Creator 5 Pro's scale touch correctly
  ([#1450](https://github.com/prestonbrown/helixscreen/issues/1450)). If you set
  `HELIX_TOUCH_SWAP_AXES=1` to work around this, remove it, or the axes swap twice.

**Install and uninstall**

- **QIDI printers show .3mf thumbnails again** - with the stock screen stopped, nothing
  unpacked plate thumbnails from .3mf files, so Fluidd and HelixScreen showed none. A small
  helper service now does it, set up on install and on update
  ([#1713](https://github.com/prestonbrown/helixscreen/issues/1713)).
- **The QIDI installer warns what the stock client carries** - on newer QIDI firmware,
  stopping it breaks QIDI Studio box sync, QIDI cloud and box filament edits. The installer
  says so once, and the QIDI guide describes the trade-off.
- **A failed download no longer leaves a printer with no screen** - the installer downloads
  and checks the release before it disables the stock UI, which also keeps the network up
  on boards whose stock UI runs WiFi, like the U1.
- **Uninstall restores the QIDI and MKS stock screen** instead of leaving the boot splash
  up until a reboot.
- **WiFi respects a radio an administrator blocked** - on a machine where WiFi was never
  set up in HelixScreen, a radio blocked at boot stays blocked
  ([#1697](https://github.com/prestonbrown/helixscreen/issues/1697)).

**Themes**

- **Text on coloured buttons stays readable** - buttons, badges, AMS lane and slot badges,
  filament swatches, LED chips, action prompts, the jog pad, screws-tilt indicators and
  step circles missed readable contrast on most shipped themes. They now pick text that
  reads on their fill, keeping the theme's tint where it can
  ([#1496](https://github.com/prestonbrown/helixscreen/issues/1496),
  [#1648](https://github.com/prestonbrown/helixscreen/issues/1648)).
- **A theme saved in the editor sticks** when the theme explorer closes, and the explorer's
  preset picker keeps previewing after the editor has been opened.
- **Edit mode's trash and configure icons are visible** against their selection pill in
  light and dark themes.

**Home screen**

- **The add-page tile is labelled** - it reads "Add page" in the current language, and the
  plus sign no longer looks disabled. The user guide now describes adding and deleting
  pages as 1.0 does it.
- **Tapping anywhere on a controls card opens its overlay**, header included.

**Printing and filament systems**

- **The print status thumbnail shows on printers whose Moonraker metadata is broken** - it
  is read from the gcode header when metadata is missing, as on QIDI Q2 and Max 4.
- **Calibration files preview** - OrcaSlicer pressure advance, flow and retraction tests no
  longer render blank.
- **Snapmaker U1 explains a refused resume** when an extruder's filament type is unset, and
  offers only a dismiss where Resume would be refused again.
- **AFC shows only device actions it can run** - single-extruder machines no longer get an
  "Unknown action" on the LED action, and setups without a hub no longer show a bowden
  length slider they cannot use.
- **The printer setup wizard stops reopening on every boot** on ForgeX and zmod AD5M Pro
  machines after a targeted reconfigure.
- **Filament remap and Snapmaker filament messages are translated** into all eight
  languages.

## [1.0.1] - 2026-09-20

<!-- whatsnew
The first patch release on the 1.0 line.

A Centauri Carbon is no longer identified as a Qidi. The pre-print screen learns each
printer's real heating rate and stops claiming phases the printer has not reached.
Filament colour and temperature edits stick, black included. Bed mesh profiles save
under the name you chose.

K2 install and restore verify the boot link before offering a reboot, and ForgeX
machines keep the display mode they arrived on.
-->

The first patch release on the 1.0 line. The bulk of it is in five areas: which printer
HelixScreen thinks it is talking to, what the pre-print screen claims the printer is
doing, whether filament system edits survive being written, where a bed mesh actually
gets saved, and the install and restore paths on K2 and ForgeX machines.

### Added

- **The pre-print screen follows printers that narrate through the display** - a
  PRINT_START that reports its phases with SET_DISPLAY_TEXT or M117 was previously
  invisible, because commands inside a macro never echo to the console. Those printers
  now get a tracked pre-print sequence like any other.
- **An aux fan role** - a printer declaring an auxiliary fan gets it named and mapped,
  and a preset pointing at it resolves instead of silently doing nothing.

### Fixed

**Printer identification**

- **A Centauri Carbon is no longer identified as a Qidi** - a machine running COSMOS
  matched on a shared command and came up as the wrong printer at 89% confidence, which
  meant the wrong profile for everything downstream. It now identifies exactly.

**Starting a print**

- **Heating estimates are per-printer and hold across prints** - the pre-print screen
  learns each machine's real climb rate rather than a shared guess, saves the whole climb
  rather than the part before a hold, and keeps what it learned when a timeout ends the
  pre-print early.
- **The pre-print screen waits for heaters to actually reach target** - it no longer ends
  on a timeout while a heater is still climbing, and it treats a heater swinging around
  its target as settled rather than still climbing.
- **Centauri Carbon pre-print phases are tracked** - the COSMOS PRINT_START sequence
  matched nothing, leaving every phase on a single default estimate. Its phases now map.
- **A heat soak holds the screen open for the time it announces** - a COSMOS heat soak on
  the Centauri Carbon no longer looks finished minutes before it is.
- **A heating bed is labelled as one** - a heat soak relabels it correctly, and an idle
  mesh step gives way to the nozzle heat rather than sitting in front of it.
- **A file that would trigger an emergency stop is refused before printing** - a gcode
  file calling a command the printer turns into an emergency stop is blocked up front.
- **A file scanned too early is scanned again** - a file checked before the printer's
  macros had been read was cleared on incomplete information and stayed cleared for the
  rest of the session. That answer is now reused only while it still applies.
- **K2 phase narration is the printer's, not ours** - our own pre-start echo stopped
  claiming three phases the printer had not reached.
- **A paused print stops advancing** - progress no longer creeps forward, and it is no
  longer recorded internally either, whatever the printer reports.
- **An active print keeps its status screen** - closing the screen during a print no
  longer discards it, and a job that ends while the screen is hidden releases cleanly.
- **A filament runout that stops the print is reported** - the removal notice also waits
  until a tool change would have refilled, so it is not raised prematurely.
- **The 2D preview appears complete** - it is built off screen and swapped in, rather than
  drawing itself in front of you.

**Filament systems**

- **AFC and Happy Hare edits survive** - a slot's colour and temperatures are no longer
  dropped when the override is written, clearing a slot erases it from disk, and Happy
  Hare's override store is loaded at startup instead of being built empty.
- **Black is a real colour** - a slot set to pure black is dispatched to AFC and Happy
  Hare rather than read as unset (#1597), and the spool editor treats it the same way
  (#1608).
- **A spool weight edit changes only the weight** (#1652) - saving a meter reading no
  longer rewrites the rest of the slot with it.
- **Filament loading respects temperature limits** - a macro prefill stays above the
  extrusion minimum and within the hotend's maximum, is handed the nozzle temperature the
  panel is showing, and on a multi-tool machine uses the extruder the loading slot feeds
  rather than whichever one is active.
- **Homing consent is spent once and returned if unused** - a confirmed home before a
  macro load homes first, a raw extrude never asks, and a failed dispatch releases the
  consent it did not use.
- **An unavailable remap explains itself** - the filament card no longer offers a remap it
  would refuse, dims the chips rather than the whole card, and moves its refusal into a
  help modal that offers the fix. A tool changer with no ASSIGN_TOOL refuses outright.
- **The file-rewriting warning appears only when the file is rewritten** - printers that
  route filament natively were warned that the job file would be modified before
  printing, which was never true for them.
- **Lane presence comes from the sensors** - silk sensors own whether a lane is loaded,
  rather than inferring it from a Spoolman weight poll, and a single-position system is
  no longer clamped to a one-slot span (#957).
- **Snapmaker U1 with a PAXX AFC-Lite** - the Snapmaker filament system takes precedence
  instead of the two fighting over the machine.
- **CFS error messages match Creality's own table** - and the long CFS verdict message no
  longer overflows its box in any language (#1605).

**Bed mesh and calibration**

- **A mesh is saved under the name you chose** - calibration reported success under the
  chosen name while storing the mesh in `default`, and replacing another stored profile
  now asks first.
- **Profile names with spaces work everywhere** - names are quoted on every path they are
  sent, and a name containing a semicolon is refused rather than silently breaking.
- **A mesh is named before it is probed** - so it lands under the name printing will look
  for, rather than somewhere printing cannot find it.
- **The Centauri Carbon uses its own bed mesh macro** - COSMOS machines delegate to the
  firmware's macro and count their mesh points, rather than reporting a mesh that was
  never taken.
- **Your own mesh macro is not failed for an optional command** - only a shipped
  calibration sequence fails on a command the printer does not know. A wrapper macro of
  your own kept probing while being reported as failed.
- **Centauri Carbon mesh calibration no longer waits for a hot bed to cool** - and
  heaters the COSMOS sequence turned on are turned back off when it finishes.
- **Dismissing the mesh naming dialog leaves Probe working** - and the default profile is
  re-probed without asking again.
- **The mesh naming dialog speaks every language** - its save and replace messages were
  English-only regardless of the language set.

**Chamber heaters**

- **A chamber heater or sensor counts only while the printer reports it** - a stale
  assignment no longer presents itself as live, and a sensor named in configuration is
  re-resolved on each discovery.
- **Filament and material temperatures read the resolved chamber heater** - rather than a
  different one than the rest of the screen is showing.

**Installing and restoring**

- **K2 stock UI restore verifies the boot link** (#1641) - it offers a reboot only once
  the link is confirmed, reports an app that failed to start rather than claiming success,
  and kills the carve-out web server properly.
- **The K2 web server carve-out survives restarts and reboots** (#1641, #1665) - it runs
  as a supervised service, and deploying records it in the ledger with both boot links
  verified (#1667).
- **An interrupted K2 or K1 install can be resumed** (#1668) - the disabled-services
  ledger is found wherever the interrupted run left it, and the K1 sweep stops the web
  server with the rest of the stock backend.
- **ForgeX machines keep the display mode they arrived on** - a machine arriving in FEATHER
  mode kept drawing over HelixScreen indefinitely, and the network daemon is now restored
  at startup so first-run WiFi setup is reachable.
- **Presets survive the wizard** (#837) - post-wizard migration seeds role keys that were
  never set, a preset value is no longer lost during startup, and AD5M variants get the
  second filament sensor they declare.

**Networking**

- **Hidden networks can be joined** - both network backends now send what their tool
  requires for a hidden SSID.
- **A network daemon that is installed but down is reported as such** - rather than being
  read as absent and quietly replaced.

**Screen and interface**

- **The panel goes dark at sleep even where the backlight works** (#1594) - previously a
  machine with working backlight control kept showing the last frame.
- **A backlight write the kernel rejects is reported** (#1595) - instead of being treated
  as applied.
- **Home edit mode gestures are classified against their own press point** - so a drag
  started in one tile is not attributed to another.
- **The first-run tour behaves** - it starts once however many things ask for it, waits
  for the wizard to finish, and does not open on top of home edit mode.
- **The WiFi password dialog cannot be left dangling** (#1579) - anything else destroying
  it no longer leaves a stale reference behind.
- **A 2D render is not refused over a saved Thumbnail Only preference** - the two settings
  are now one decision instead of disagreeing.
- **Chinese reads correctly on the slot reservation message** - it used a character the
  bundled font does not carry, so it drew as a missing glyph.
- **LED detection reads live configuration** - and a synthetic strip is never reported
  missing.
- **Print list drops Size and Modified on narrow screens** - rather than crowding the
  file name off.

**Stability and performance**

- **Several crashes on screens that rebuild themselves** - event guards that had stopped
  applying were restored, and an observer on a value that is never freed is now removed
  correctly.
- **History no longer refetches on every timelapse frame** - a timelapse component moving
  and rendering frames throughout a print cost a full history reload each time.
- **Streaming a large gcode preview is budgeted honestly** - a RAM-backed cache was
  charged nothing for the file it streamed, so the memory it used went uncounted.
- **A memory warning that never clears escalates** - instead of sitting at warning
  indefinitely.
- **The Centauri Carbon caches to flash rather than RAM** - and the cache reports whether
  it actually landed in RAM instead of assuming it did.
- **Debug bundles find logs and crash files on every printer** - on Creality, Flashforge
  and Centauri Carbon machines the bundle looked in one place that did not exist there, so
  an uploaded bundle could be missing the crash report entirely.

### Changed

- **Crash reports name real functions on statically linked printers** - on the six
  machines built without position-independent code, the load address was added to symbol
  addresses that were already absolute, so backtraces pointed at the wrong code.
- **Moonraker settings the environment file cannot change are no longer shipped** - they
  only looked configurable.

## [1.0.0] - 2026-09-09

<!-- whatsnew
The first stable release.

Everything your printer can do, on the screen already attached to it: a dashboard
you arrange yourself, bed mesh and input shaper you can actually see, multi-material
handling, and print history. It runs on the hardware you own.

80+ printers auto-detected. 9 languages. Nine platforms, from a printer's own MIPS
panel to a Raspberry Pi.

Coming from 0.99, this is an ordinary update.
-->

**Upgrading from 0.99?** Nothing to do differently. The version jump is cosmetic:
every 0.99 install is on config_version 23 already, so this is an ordinary forward
step for the updater and your settings, layout and printer list carry over untouched.

**1.0.** Eleven months and 203 builds after the first commit, HelixScreen is a
complete touchscreen UI rather than a promising one: 80+ printers it configures itself
for, a dashboard you arrange yourself, bed mesh and input shaper rendered on the
machine, multi-material across seven filament systems, and nine languages. It does that
in about 15MB of RAM on a printer's own board, which is the whole point - the screen you
already own is enough.

Calling it 1.0 is a statement about the line, not about this build in isolation. The
0.99 series ran long on purpose, and stable now means the update channel most installs
track points here.

The last stretch of work went to three places. Networking: on printers whose firmware
runs its own network daemon, the Wi-Fi and Ethernet page drives that daemon instead of
sitting beside it. The AD5X's filament system, drawn the way the machine is actually
built, now understanding the IFS module split out of Z-Mod. And sound on the FlashForge
machines, where the AD5M's buzzer plays music instead of static and the AD5X's silent
buzzer was found and fixed. Around those: USB mice on 64-bit machines, touch calibration
on rotated displays, previews going monochrome on routed multi-tool prints, a quieter
pairing prompt, home-panel crashes chased from a field report, long-untranslated section
titles, and a memory bug in confirmation dialogs that the nightly sanitizer run caught
before any field report.

### Added

**Sound**

- **Music on the AD5M's buzzer** - the built-in buzzer never really worked: the channel it
  needs is not enabled after boot, and the speaker cannot reproduce recorded audio, so
  attempts came out as static. Melodies now play as clean square-wave notes, PC-speaker
  style - following the lead line, shifted into the range the buzzer is actually loud in,
  and ending in silence instead of holding the last note. The buzzer channel is enabled
  automatically at startup, so sound survives every boot, and the playback engine is built
  to never compete with the print host for the processor.

**Networking**

- **Wi-Fi and Ethernet through the printer's own network daemon** - on printers whose
  firmware runs its own network daemon (Forge-X 1.4.2 on the AD5M today), the network page
  now talks to that daemon directly instead of working around it. Joining a network,
  scanning, and the Ethernet row all go through it, so the screen and the firmware no
  longer fight over the radio; the address appears the moment you connect; Ethernet stops
  reporting "no cable" on machines where the firmware deliberately parks the port to allow
  only one transport at a time; and a wrong password reports an authentication failure
  right away instead of failing as a generic timeout 45 seconds later.
  Losing the daemon mid-session falls back to what the kernel still knows rather than
  blanking the page.

**Filament**

- **The standalone IFS module on the AD5X** - the filament system now ships as a module
  in its own right in Z-Mod, reporting its state through its own status objects. HelixScreen now recognizes those directly: which lanes hold material and
  what is on them, which lane is loaded and its colour, read from the module instead of
  polled through the older Z-Mod-era macros - which stand down on their own once the
  module is present.

- **QIDI's stock load and unload macros** - QIDI ships M604/M603 rather than the names
  HelixScreen looked for, so load and unload did nothing on a stock QIDI machine.
- **Cancelling a print from the runout dialog now asks first** - the cancel button on the
  filament-runout dialog ended the print immediately, with no confirmation.
- **Lanes of unknown type report their real fill** (#1367) - a lane whose type could not
  be determined reported empty rather than the level it actually had.
**Updates**

- **Choose Stable or Beta on any install** (#1236) - the update channel was a hidden
  developer setting; it is now a normal choice in Settings. Stable tracks the 1.0 line,
  Beta tracks what becomes 1.1. Picking a channel that offers an older build than the one
  you are running asks before it downgrades, and never notifies you unprompted.

**Pairing**

- **Denied pairing requests stop coming back** (#1376) - denying a slicer or phone app's
  request used to buy silence only until its next reconnect, which re-prompted
  immediately, forever. A denied client is now quiet for a minute, remembered per device:
  denying one phone does not hush a different one, and approving a client clears its
  record.

### Fixed

**Performance**

- **Idle CPU on two-core machines** (#1440) - the screen kept a processor core busy while
  showing nothing new: a filename too long for its space scrolled continuously, the heater
  icon pulsed, and the update queue woke a thousand times a second rather than once per
  display refresh. Turning animations off in Settings now genuinely stops the motion, the
  queue drains on the refresh period, and a filename that is not scrolling is clipped
  instead of animated. This matters most on a two-core board such as the K1 Max, where the
  print host needs the other core.
- **Permanent memory warnings on small machines** - the thresholds that decide when
  HelixScreen sheds caches were scaled to installed RAM only on the roomiest machines, so
  a K1-class box sat at a warning level it could never clear: its warn threshold landed on
  the app's own steady-state footprint, with the clear threshold below it. One field
  session warned every five minutes for over an hour with 130 MB free, and each response
  freed nothing while one of them discarded a cached view the same machine had
  deliberately kept. Every band now scales with the RAM actually present.

**Sound**

- **The AD5X's buzzer was silent** - speaker detection looked for a beeper pin that
  printer does not have; its firmware drives the buzzer through an M300 macro instead, and
  that whole class of machine went unheard while BEEP kept working. Such printers are now
  recognized, and the speaker on/off override in Settings finally reaches the decision:
  forcing it on installs the beeper even where no signal gives it away, forcing it off
  really keeps it out.

**Networking**

- **Wi-Fi scanning stopped after a backend switch** (#1405) - when the Wi-Fi backend was
  swapped mid-scan, the outstanding scan could never complete and periodic scanning on the
  network page silently died for the rest of the session. The switch now resolves the
  scan, and scanning keeps running.
- **A Wi-Fi scan could hang forever** (#1407) - losing the connection to wpa_supplicant in
  the middle of a scan left that scan permanently outstanding: nothing further scanned
  until restart. Every triggered scan is now guaranteed to complete, using whatever
  results the daemon still holds.
- **Stale rows on the network page after a transport flip** (#1398, #1399) - switching
  between Wi-Fi and Ethernet while the page was open left the dead transport's address on
  screen next to the new one until you tapped something. Both rows now refresh on every
  change, and the Wi-Fi address reads correctly on machines that booted in Ethernet mode.
- **A dead network daemon no longer strands the radio** - a boot that ships the firmware's
  network daemon but does not actually run it selected that daemon anyway, failed to start
  it, and left Wi-Fi down for the whole session while wpa_supplicant sat unused. The screen
  now falls back to the supplicant when the daemon proves unreachable, and stays with the
  daemon when it answers - so two clients never end up on one radio.
- **An action a backend cannot do is no longer reported as a failure** - forgetting a
  network, or toggling the radio, on a backend without that call raised a red "Failed to
  forget WiFi network" and snapped the switch back. Those now say plainly that the
  capability is missing, and a daemon-owned machine no longer silently rewrites a stored
  radio-off back to on at startup.

**Input**

- **USB mice on 64-bit machines** - the input probe misread the kernel's capability format
  on 64-bit systems and concluded that no mouse had a left button, so every USB mouse was
  rejected, and touch devices registering through the same path were lost with them. Mice
  now work on 64-bit hosts.
- **Touch calibration on a rotated display** (#1394) - running the touch-calibration
  wizard while the display is rotated stored a mapping that sent every later tap through
  the wrong transform, so touches landed far from your finger. The wizard now stores that
  kind of range only on unrotated displays, and a bad range stored by the previous release
  is ignored rather than applied.

- **Touch calibration on a rotated display, properly this time** (#1394) - completing the
  wizard on a rotated panel left every later touch a quarter turn out, because the affine
  was solved against logical targets but applied before the display rotation, and its
  clamp box was the transpose of the one it was solved against. The axis-swap check made
  it worse by re-solving against transposed samples and installing a matrix nothing feeds.
  Both are fixed, and the wizard now re-reads the screen size each time it is shown, so a
  display rotated part-way through setup no longer lays the targets out at the wrong
  ratios. Unrotated panels behave exactly as before.
- **Touchscreens that report their type in the other bit** - the probe tested the wrong
  bit for a direct-input device, so some panels were classified as pointing devices.
- **Page arrows on resistive panels** - the home screen's page arrows never appeared on
  resistive touchscreens, leaving the other pages unreachable.
**Crashes**

- **Random crashes on the home panel** - the widgets that make up the home panel (nozzle
  temperatures, the tool switcher, thermistors) kept pointers into a screen that had
  already been torn down, so a pending update arriving afterwards wrote into freed memory
  and took the app down with it - a Snapmaker U1 running four toolheads in the field among
  the reporters, and to the user it looks like the screen randomly restarts. The print
  status screen had the same hole while a print was preparing, and it is closed with them.
- **A memory bug in confirmation dialogs** - a confirmation could keep a borrowed pointer
  to its button caption after the screen that showed it was gone, and the next dialog read
  released memory: the kind of fault that surfaces as a random crash or garbled button
  text. It shipped in the previous release and was caught by the nightly sanitizer run,
  before any field report.

- **A null event descriptor in the dispatch loop** (#1470) - a null descriptor reaching
  the event dispatcher took the app down; it is now guarded rather than dereferenced.
**Z offset**

- **Save Z Offset stacking with Helper-Script's save-zoffset** (#1401) - with the Helper
  save-zoffset macro in your config, Save Z Offset folded the offset into the probe while
  the macro also saved it and re-applied it at boot: the offset grew on every save, and a
  reporter's K1 climbed from 0.06 mm to 2.5 mm until the nozzle pressed into the bed.
  HelixScreen now recognizes the macro and steps back, as it already did for Z-Mod - the
  button is hidden and nothing is folded into the probe.
- **The firmware persistence enable is sent once, not on every launch** - the command that
  tells firmware to persist the offset was re-sent at each startup rather than when the
  setting changed.
- **A macro wrapper is not proof the offset persists** - a config merely wrapping
  SET_GCODE_OFFSET was read as evidence the firmware saves the offset, so the screen
  claimed a persistence it did not have.

- **A saved offset that would not clear** - the pending delta stayed on screen after a
  successful save, so the adjustment looked unapplied and inviting a second one.
**Print screen**

- **The last print's result dialog closes when the next print starts** - "Print complete"
  or "Print failed" used to stay on screen over a newly started job, and you had to
  dismiss the previous print's obituary to watch the next one begin. It still stays while
  the printer sits idle, so the result can be read.
- **The progress bar hides during pre-print** - while the printer heats and probes before
  a print, the progress bar is meant to step aside (keeping its space); the styling that
  did it never applied, so the bar stayed fully visible through the whole preparation. It
  behaves now.

- **The progress arc on the print screen** (#1510) - the arc bound to a value that was
  not the one being displayed, so it could disagree with the percentage beside it.
- **Speed and flow on narrow screens** - the speed/flow row was shown on both narrow
  breakpoints, where it had nowhere to go and crowded the rest of the panel.
- **A stutter when a print starts** - the active-print thumbnail was written inside the
  UI update batch, so a large image stalled the frame it landed on. It is written after
  the batch now.
**File detail and preview**

- **Monochrome previews on routed multi-tool prints** - a routing that sent every tool of a
  multi-tool file to a single lane (seen at print start on an AD5X running the IFS module)
  painted the whole model in that lane's colour, trampling the file's own palette. Such a
  routing is now recognized as no colour answer at all, so the slicer's palette stands -
  while lanes that deliberately hold matching spools as runout backups keep their say, and
  a routing that empties takes its colours back off the screen instead of leaving them
  stale for the rest of the file.
- **Colour lists from OrcaSlicer** - a file whose colours arrive as one comma-joined list
  was read as a single colour and painted the whole preview in the first tool's hue. The
  list now parses into the per-tool palette it is.
- **Tool colours with the IFS module and a mapping plugin together** - on a machine running
  both the standalone IFS module and a tool-mapping plugin (lessWaste, bambufy), whichever
  reported last owned the mapping, so the colours could flip back to raw lane numbers
  whenever a module update landed after the plugin's. The plugin's mapping now wins,
  whatever the arrival order.
- **The render takes over from the thumbnail cleanly on the file detail view** - the swap
  now happens the moment the first real frame lands on the canvas. Before, the thumbnail -
  mostly transparent, in OrcaSlicer's case - stacked over the viewer for the whole build,
  with the render and its Building label showing through at mismatched scales, and the
  finished render stood about 18% shorter than the thumbnail it replaced, so the swap
  visibly jumped. The render is now framed to match the thumbnail exactly, and a colour
  change arriving mid-build no longer leaves the old image up.
- **The filaments card on the file detail view** - the card had no surface of its own, so
  its header and colour chips floated loose on the panel while every neighbouring card
  sits raised, and the chips were one fixed height that clipped the lane number top and
  bottom on smaller screens. The card renders like its siblings now, and the chips size
  to the screen - checked at every breakpoint from 480x272 to 1280x720.
- **Multi-colour prints framed against the prime tower** - OrcaSlicer spells its purge
  tower differently from the slicers already recognized, so the tower counted as part of
  the model: auto-fit framed the whole preview around it and shrank the actual part to a
  thumbnail. The tower is now excluded from the fit and skipped by the renderer, which also
  gives the rest of the model back the drawing budget the tower was consuming.
- **Lane colours stayed on a file after the routing cleared** - when a tool routing emptied,
  the request to take those colours back off the preview was rejected by the same guard
  that protects a file's own palette, so stale lane colours stayed for the rest of the
  file. Retraction is now its own path.

**Fans**

- **Blank fan icons in the Fans overlay** - the mark beside each fan row, and the icon in
  the no-fans state, were looked up as images against what is actually an icon glyph, so
  every one of them drew a blank slot. They render now.
- **The fan widget's rows line up** - each row of the fan stack was centred on its own, so
  the icons sat at slightly different positions whenever the speeds read differently (a
  fan at 0% next to one at 100%). Rows now share one left edge with the speeds
  right-aligned to one edge, so nothing shifts as a fan ramps.

**Translations**

- **Ten section titles, in every language** - titles reaching the screen through a less
  common markup path were never picked up for translation, so section headers such as
  Controllable Fans, Auto Fans, the sensor groups, New Spool and Device Section stayed
  English on every non-English device. All nine languages now carry them, along with the
  favourite-macro dialog's questions, which had sat as empty placeholders since they were
  added.
- **The recovery dialog was translated twice** - the text shown when the printer stops
  unexpectedly was run through translation two times over: it rendered correctly only by
  falling back, warned on every refresh, and could show the wrong string outright in a
  locale whose translation is itself a lookup. It is translated once now.
- **The last untranslated strings for 1.0** - the 76 strings still sitting as English
  placeholders are translated in all nine languages, and the word for a filament position
  is now consistent: the generic interface says "slot" everywhere, in every language,
  rather than alternating with the vendor-specific terms.

**Spoolman**

- **Search in the spool picker** - every keystroke rebuilt the whole spool list on the
  spot; with a few hundred spools in Spoolman the first tap stalled the screen for a
  noticeable beat on a desktop and for seconds on a K2, and the keyboard froze with it.
  Typing now re-renders once, after a short pause; clearing the search applies
  immediately; a search with no matches says so with its own card instead of flashing a
  load-error retry; and the loading spinner sits centred.

**Printers**

- **A Qidi Q2 identified as a Plus 4** (#1431) - detection named a printer it could not
  actually separate from a close rival, because two models scored the same on a
  range-wide macro. It now declines to name a machine unless the winner leads every
  candidate naming a different one, and the Qidi entries no longer claim a shared macro
  identifies a single model.

- **A K1 no longer advertises hardware it does not have** - the K1 preset seeded
  hardware entries the machine does not ship, which then showed up as controls for
  equipment that is not there.
- **Bed mesh calibration runs the sequence the printer ships** - the mesh calibration ran
  a generic sequence rather than the one the printer's own configuration defines.
**Filament**

- **A second, wrong error after a failed CFS load** - when the homing move before a load or
  unload failed, the real error arrived with a spurious "Unknown g-code state" stacked on
  top of it. The cleanup step ran even though the operation it was meant to undo had never
  started, and Klipper rejected it. Only the actual failure is reported now.

- **The runout dialog appearing when you pull filament out yourself** - on a printer with a
  plain filament sensor and no filament system, taking filament out while the printer was
  idle raised the "Filament Runout" dialog, reporting a fault for something you had just
  done with your own hands. Nothing is being extruded while the printer sits idle, so
  filament leaving the sensor in that state is someone standing at the machine; the screen
  now just shows the filament as empty and stays out of the way. A sensor already reading
  empty when the screen arrives still offers to load, which is the case worth asking about.

**Home screen**

- **The home screen rearranging itself when Klipper drops out** - a layout worked out while
  the printer was down got written back to disk as though you had arranged it that way, so
  widgets you had placed yourself reverted to an automatic arrangement after a power cycle
  or a firmware restart. Your positions are now only saved once the printer is actually
  reporting ready, so a fault no longer rewrites the layout underneath you.

**Setup**

- **The printer-mismatch button says what it opens** - the button offered when the detected
  printer disagrees with the saved one was labelled for something other than the screen it
  actually opens.

**Installing and updating**

- **The Snapmaker U1 ships with its crash supervisor again** - the U1 was the only
  platform whose release never packaged `helix-watchdog`, so a crash left the screen
  dead with nothing to restart it. That matters most on this machine, because the
  installer also stands the stock touchscreen UI down.

- **A stock UI that reports itself missing** - the installer now warns when a device known
  to ship a vendor UI reports none, rather than continuing quietly and leaving two screens
  competing for the display.
- **QIDI's stock screen across firmware generations** - the installer stopped only the
  screen process one firmware generation used; it now stops it on every generation.

**Files and USB**

- **Every mounted USB drive is scanned** (#1373) - only the first mounted drive was
  searched for printable files, so a second stick appeared empty.

**Job queue**

- **A Print-now tap while the printer is busy** (#1373) - the tap did nothing and said
  nothing; it now answers.

**Settings**

- **The log level shown is the one the sinks run at** - the settings screen displayed the
  ring buffer's floor rather than the level actually being written, so it could claim a
  verbosity that was not in effect.

**Telemetry**

- **The send window no longer depends on the clock's direction** (#1476) - the periodic
  send gate was decided from a wall-clock comparison, so a clock stepping backwards (NTP
  settling at boot is the usual cause) could hold sends off indefinitely. It is decided
  from elapsed time now, and the periodic producers are sized to fit inside one window.

### Changed

- **The temporary Firmware Restart tile is gone from the home screen** - it was added to the
  grid whenever Klipper was down, but placed automatically after every widget with a saved
  position, so on a full home screen it was silently dropped and never appeared at all. The
  recovery dialog that opens on a Klipper shutdown carries the restart action, and Firmware
  Restart is still in the widget catalog if you want a permanent button for it.

- **The AD5X filament path drawing** - the picture showed a machine the AD5X is not: a
  merge unit mid-machine, or a selector with a single output line. The IFS is really four
  lanes into a camshaft selector sitting under the spools, with the combiner hub on the
  toolhead itself - that is what the canvas draws now, one continuous tube from the hub
  down to the nozzle, and no bypass stub, because the machine has none. The toolhead
  style picker in Settings also offers all eight styles instead of five.

### Known Issues

- **Changing the UI scale rearranges the home screen** - the widgets you placed on the
  home screen are laid out again for the new scale, and your previous arrangement is not
  kept. Setting the scale back to what it was does not restore it either. A saved widget
  position is a coordinate in grid tracks, and the UI scale changes the grid those tracks
  are counted against, so the positions are re-seated for the new grid rather than parked
  and restored. If you have a home screen arranged the way you want it, pick the UI scale
  first and arrange the widgets afterwards. (prestonbrown/helixscreen#1484)

## [1.0.0-rc.1] - 2026-09-04

<!-- whatsnew
The 1.0 release candidate. Highlights:

- Wi-Fi and Ethernet now drive the printer's own network daemon on Forge-X AD5M
- The AD5M's buzzer plays melodies as clean notes, and works after every boot
- The AD5X buzzer now works; its filament path is drawn as the machine is built
- USB mice work on 64-bit machines; touch calibration is fixed on rotated displays
- Random crashes on multi-tool home panels are fixed
- A memory bug in confirmation dialogs found by the nightly sanitizer is fixed
- Idle CPU drops sharply on two-core machines, and animations off now means off
-->

**Upgrading from 0.99?** Sound on the AD5M and AD5X now works properly - the AD5M's
buzzer plays melodies cleanly where static was the best it could manage before, and the
AD5X's buzzer, silent until this release, is heard too. If you prefer a quiet machine,
the speaker on/off override in Settings now genuinely silences it.

The 1.0 release candidate - the stabilization pass over the tail of the 0.99 line, ahead
of the first version to call itself stable. The work since v0.99.118 went to three places.
Sound on the
FlashForge machines: the AD5M's buzzer plays music now instead of static, and the AD5X's
silent buzzer was found and fixed. Networking: on printers whose firmware runs its own
network daemon, the Wi-Fi and Ethernet page now drives that daemon instead of sitting
beside it. And the AD5X's filament system, which is drawn the way the machine is actually
built and now understands the IFS module split out of Z-Mod. Around those: USB mice on
64-bit machines, touch calibration on rotated displays, previews going monochrome on
routed multi-tool prints, a quieter pairing prompt, home-panel crashes chased from a field
report, long-untranslated section titles, and a memory bug in confirmation dialogs that
the nightly sanitizer run caught before any field report.

### Added

**Sound**

- **Music on the AD5M's buzzer** - the built-in buzzer never really worked: the channel it
  needs is not enabled after boot, and the speaker cannot reproduce recorded audio, so
  attempts came out as static. Melodies now play as clean square-wave notes, PC-speaker
  style - following the lead line, shifted into the range the buzzer is actually loud in,
  and ending in silence instead of holding the last note. The buzzer channel is enabled
  automatically at startup, so sound survives every boot, and the playback engine is built
  to never compete with the print host for the processor.

**Networking**

- **Wi-Fi and Ethernet through the printer's own network daemon** - on printers whose
  firmware runs its own network daemon (Forge-X 1.4.2 on the AD5M today), the network page
  now talks to that daemon directly instead of working around it. Joining a network,
  scanning, and the Ethernet row all go through it, so the screen and the firmware no
  longer fight over the radio; the address appears the moment you connect; Ethernet stops
  reporting "no cable" on machines where the firmware deliberately parks the port to allow
  only one transport at a time; and a wrong password reports an authentication failure
  right away instead of failing as a generic timeout 45 seconds later.
  Losing the daemon mid-session falls back to what the kernel still knows rather than
  blanking the page.

**Filament**

- **The standalone IFS module on the AD5X** - the filament system extracted from Z-Mod
  ships in Forge-X as a module in its own right, reporting its state through its own
  status objects. HelixScreen now recognizes those directly: which lanes hold material and
  what is on them, which lane is loaded and its colour, read from the module instead of
  polled through the older Z-Mod-era macros - which stand down on their own once the
  module is present.

**Pairing**

- **Denied pairing requests stop coming back** (#1376) - denying a slicer or phone app's
  request used to buy silence only until its next reconnect, which re-prompted
  immediately, forever. A denied client is now quiet for a minute, remembered per device:
  denying one phone does not hush a different one, and approving a client clears its
  record.

### Fixed

**Performance**

- **Idle CPU on two-core machines** (#1440) - the screen kept a processor core busy while
  showing nothing new: a filename too long for its space scrolled continuously, the heater
  icon pulsed, and the update queue woke a thousand times a second rather than once per
  display refresh. Turning animations off in Settings now genuinely stops the motion, the
  queue drains on the refresh period, and a filename that is not scrolling is clipped
  instead of animated. This matters most on a two-core board such as the K1 Max, where the
  print host needs the other core.
- **Permanent memory warnings on small machines** - the thresholds that decide when
  HelixScreen sheds caches were scaled to installed RAM only on the roomiest machines, so
  a K1-class box sat at a warning level it could never clear: its warn threshold landed on
  the app's own steady-state footprint, with the clear threshold below it. One field
  session warned every five minutes for over an hour with 130 MB free, and each response
  freed nothing while one of them discarded a cached view the same machine had
  deliberately kept. Every band now scales with the RAM actually present.

**Sound**

- **The AD5X's buzzer was silent** - speaker detection looked for a beeper pin that
  printer does not have; its firmware drives the buzzer through an M300 macro instead, and
  that whole class of machine went unheard while BEEP kept working. Such printers are now
  recognized, and the speaker on/off override in Settings finally reaches the decision:
  forcing it on installs the beeper even where no signal gives it away, forcing it off
  really keeps it out.

**Networking**

- **Wi-Fi scanning stopped after a backend switch** (#1405) - when the Wi-Fi backend was
  swapped mid-scan, the outstanding scan could never complete and periodic scanning on the
  network page silently died for the rest of the session. The switch now resolves the
  scan, and scanning keeps running.
- **A Wi-Fi scan could hang forever** (#1407) - losing the connection to wpa_supplicant in
  the middle of a scan left that scan permanently outstanding: nothing further scanned
  until restart. Every triggered scan is now guaranteed to complete, using whatever
  results the daemon still holds.
- **Stale rows on the network page after a transport flip** (#1398, #1399) - switching
  between Wi-Fi and Ethernet while the page was open left the dead transport's address on
  screen next to the new one until you tapped something. Both rows now refresh on every
  change, and the Wi-Fi address reads correctly on machines that booted in Ethernet mode.
- **A dead network daemon no longer strands the radio** - a boot that ships the firmware's
  network daemon but does not actually run it selected that daemon anyway, failed to start
  it, and left Wi-Fi down for the whole session while wpa_supplicant sat unused. The screen
  now falls back to the supplicant when the daemon proves unreachable, and stays with the
  daemon when it answers - so two clients never end up on one radio.
- **An action a backend cannot do is no longer reported as a failure** - forgetting a
  network, or toggling the radio, on a backend without that call raised a red "Failed to
  forget WiFi network" and snapped the switch back. Those now say plainly that the
  capability is missing, and a daemon-owned machine no longer silently rewrites a stored
  radio-off back to on at startup.

**Input**

- **USB mice on 64-bit machines** - the input probe misread the kernel's capability format
  on 64-bit systems and concluded that no mouse had a left button, so every USB mouse was
  rejected, and touch devices registering through the same path were lost with them. Mice
  now work on 64-bit hosts.
- **Touch calibration on a rotated display** (#1394) - running the touch-calibration
  wizard while the display is rotated stored a mapping that sent every later tap through
  the wrong transform, so touches landed far from your finger. The wizard now stores that
  kind of range only on unrotated displays, and a bad range stored by the previous release
  is ignored rather than applied.

**Crashes**

- **Random crashes on the home panel** - the widgets that make up the home panel (nozzle
  temperatures, the tool switcher, thermistors) kept pointers into a screen that had
  already been torn down, so a pending update arriving afterwards wrote into freed memory
  and took the app down with it - a Snapmaker U1 running four toolheads in the field among
  the reporters, and to the user it looks like the screen randomly restarts. The print
  status screen had the same hole while a print was preparing, and it is closed with them.
- **A memory bug in confirmation dialogs** - a confirmation could keep a borrowed pointer
  to its button caption after the screen that showed it was gone, and the next dialog read
  released memory: the kind of fault that surfaces as a random crash or garbled button
  text. It shipped in the previous release and was caught by the nightly sanitizer run,
  before any field report.

**Z offset**

- **Save Z Offset stacking with Helper-Script's save-zoffset** (#1401) - with the Helper
  save-zoffset macro in your config, Save Z Offset folded the offset into the probe while
  the macro also saved it and re-applied it at boot: the offset grew on every save, and a
  reporter's K1 climbed from 0.06 mm to 2.5 mm until the nozzle pressed into the bed.
  HelixScreen now recognizes the macro and steps back, as it already did for Z-Mod - the
  button is hidden and nothing is folded into the probe.
- **The firmware persistence enable is sent once, not on every launch** - the command that
  tells firmware to persist the offset was re-sent at each startup rather than when the
  setting changed.
- **A macro wrapper is not proof the offset persists** - a config merely wrapping
  SET_GCODE_OFFSET was read as evidence the firmware saves the offset, so the screen
  claimed a persistence it did not have.

**Print screen**

- **The last print's result dialog closes when the next print starts** - "Print complete"
  or "Print failed" used to stay on screen over a newly started job, and you had to
  dismiss the previous print's obituary to watch the next one begin. It still stays while
  the printer sits idle, so the result can be read.
- **The progress bar hides during pre-print** - while the printer heats and probes before
  a print, the progress bar is meant to step aside (keeping its space); the styling that
  did it never applied, so the bar stayed fully visible through the whole preparation. It
  behaves now.

**File detail and preview**

- **Monochrome previews on routed multi-tool prints** - a routing that sent every tool of a
  multi-tool file to a single lane (seen at print start on an AD5X running the IFS module)
  painted the whole model in that lane's colour, trampling the file's own palette. Such a
  routing is now recognized as no colour answer at all, so the slicer's palette stands -
  while lanes that deliberately hold matching spools as runout backups keep their say, and
  a routing that empties takes its colours back off the screen instead of leaving them
  stale for the rest of the file.
- **Colour lists from OrcaSlicer** - a file whose colours arrive as one comma-joined list
  was read as a single colour and painted the whole preview in the first tool's hue. The
  list now parses into the per-tool palette it is.
- **Tool colours with the IFS module and a mapping plugin together** - on a machine running
  both the standalone IFS module and a tool-mapping plugin (lessWaste, bambufy), whichever
  reported last owned the mapping, so the colours could flip back to raw lane numbers
  whenever a module update landed after the plugin's. The plugin's mapping now wins,
  whatever the arrival order.
- **The render takes over from the thumbnail cleanly on the file detail view** - the swap
  now happens the moment the first real frame lands on the canvas. Before, the thumbnail -
  mostly transparent, in OrcaSlicer's case - stacked over the viewer for the whole build,
  with the render and its Building label showing through at mismatched scales, and the
  finished render stood about 18% shorter than the thumbnail it replaced, so the swap
  visibly jumped. The render is now framed to match the thumbnail exactly, and a colour
  change arriving mid-build no longer leaves the old image up.
- **The filaments card on the file detail view** - the card had no surface of its own, so
  its header and colour chips floated loose on the panel while every neighbouring card
  sits raised, and the chips were one fixed height that clipped the lane number top and
  bottom on smaller screens. The card renders like its siblings now, and the chips size
  to the screen - checked at every breakpoint from 480x272 to 1280x720.
- **Multi-colour prints framed against the prime tower** - OrcaSlicer spells its purge
  tower differently from the slicers already recognized, so the tower counted as part of
  the model: auto-fit framed the whole preview around it and shrank the actual part to a
  thumbnail. The tower is now excluded from the fit and skipped by the renderer, which also
  gives the rest of the model back the drawing budget the tower was consuming.
- **Lane colours stayed on a file after the routing cleared** - when a tool routing emptied,
  the request to take those colours back off the preview was rejected by the same guard
  that protects a file's own palette, so stale lane colours stayed for the rest of the
  file. Retraction is now its own path.

**Fans**

- **Blank fan icons in the Fans overlay** - the mark beside each fan row, and the icon in
  the no-fans state, were looked up as images against what is actually an icon glyph, so
  every one of them drew a blank slot. They render now.
- **The fan widget's rows line up** - each row of the fan stack was centred on its own, so
  the icons sat at slightly different positions whenever the speeds read differently (a
  fan at 0% next to one at 100%). Rows now share one left edge with the speeds
  right-aligned to one edge, so nothing shifts as a fan ramps.

**Translations**

- **Ten section titles, in every language** - titles reaching the screen through a less
  common markup path were never picked up for translation, so section headers such as
  Controllable Fans, Auto Fans, the sensor groups, New Spool and Device Section stayed
  English on every non-English device. All nine languages now carry them, along with the
  favourite-macro dialog's questions, which had sat as empty placeholders since they were
  added.
- **The recovery dialog was translated twice** - the text shown when the printer stops
  unexpectedly was run through translation two times over: it rendered correctly only by
  falling back, warned on every refresh, and could show the wrong string outright in a
  locale whose translation is itself a lookup. It is translated once now.
- **The last untranslated strings for 1.0** - the 76 strings still sitting as English
  placeholders are translated in all nine languages, and the word for a filament position
  is now consistent: the generic interface says "slot" everywhere, in every language,
  rather than alternating with the vendor-specific terms.

**Spoolman**

- **Search in the spool picker** - every keystroke rebuilt the whole spool list on the
  spot; with a few hundred spools in Spoolman the first tap stalled the screen for a
  noticeable beat on a desktop and for seconds on a K2, and the keyboard froze with it.
  Typing now re-renders once, after a short pause; clearing the search applies
  immediately; a search with no matches says so with its own card instead of flashing a
  load-error retry; and the loading spinner sits centred.

**Printers**

- **A Qidi Q2 identified as a Plus 4** (#1431) - detection named a printer it could not
  actually separate from a close rival, because two models scored the same on a
  range-wide macro. It now declines to name a machine unless the winner leads every
  candidate naming a different one, and the Qidi entries no longer claim a shared macro
  identifies a single model.

**Home screen**

- **The home screen rearranging itself during a firmware restart** - when Klipper leaves
  ready unexpectedly the screen inserts a firmware-restart tile at the front of the layout.
  That pass matched saved positions by name without checking whether the entry was still
  enabled, so a disabled widget's leftover cell was handed to the new tile, and a widget
  you had placed yourself then failed to land and fell back to automatic placement - the
  home screen visibly reshuffling at exactly the moment a firmware fault is being reported.

**Setup**

- **The printer-mismatch button says what it opens** - the button offered when the detected
  printer disagrees with the saved one was labelled for something other than the screen it
  actually opens.

### Changed

- **The AD5X filament path drawing** - the picture showed a machine the AD5X is not: a
  merge unit mid-machine, or a selector with a single output line. The IFS is really four
  lanes into a camshaft selector sitting under the spools, with the combiner hub on the
  toolhead itself - that is what the canvas draws now, one continuous tube from the hub
  down to the nozzle, and no bypass stub, because the machine has none. The toolhead
  style picker in Settings also offers all eight styles instead of five.

## [0.99.118] - 2026-08-30

<!-- whatsnew
The fifth 1.0 release candidate. Highlights:

- Tapping an object in the preview outlines it in white instead of recolouring it
- A filament remap chosen before printing now sticks
- Dismissing a confirmation no longer strands the screen behind it
- One filament runout no longer stacks six notifications
- Dark filaments show real form instead of reading as a flat cut-out
- Installing an update no longer locks up the touchscreen
- AD5M printers on the Forge-X 1.4.2 mod keep working
-->

The fifth 1.0 release candidate. Half the cycle went into the G-code preview - object
selection, filament colour and shading were all reworked, and the whole path is faster.
The other half went to the filament mapping flow - one card on the file screen, remaps
that survive print start, an honest auto-refill status - and to confirmation dialogs,
which now clean up after every way they can be closed.

### Added

- **Pairing prompts** - when a slicer or phone app asks to pair over the network, the
  printer shows an Allow/Deny prompt instead of leaving the request unanswered.
- **RFID read on insert** - putting a spool into a CFS bay reads its tag straight away,
  rather than waiting for something else to trigger a scan.
- **Unmount is refused when the dock sensors cannot tell which tool is mounted** - on a
  tool changer, the panel now blocks the unmount and says why, rather than acting on a
  guess about which toolhead is on the carriage.
- **A hot-end tool changer gets its own progress bar** - grip, move, grip, matching what
  the machine actually does, instead of the filament Heat/Cut/Feed steps. The bar hides
  itself on machines that report no phases, and the actions read Mount and Unmount.
- **Archive a spool in Spoolman** - tapping a spool in the Spoolman list now offers
  Archive in its menu, hidden for the spool currently feeding the printer. It asks
  first, then archives; Spoolman's own web page can undo it.
- **Forge-X 1.4.2 on the AD5M** - HelixScreen now defers to Forge-X's own boot splash
  and network daemon on 1.4.2, and no longer triggers its offer to switch the display
  to Feather. Earlier Forge-X releases are unaffected and keep the HelixScreen splash
  and networking.

### Fixed

**G-code preview**

- **Selecting an object** - tapping an object in the preview now outlines it in white
  instead of recolouring it blue, so you can still see its filament colour. The outline
  traces the whole object rather than only its outer walls. Works in the print-status
  preview and in the file browser's detail view, on files sliced with object labels.
- **Dark filaments** - black and near-black spools show real form and depth instead of
  reading as a flat cut-out.
- **Filament colours without an AMS** - on a printer with no AMS and no Spoolman, the 2D
  preview drew every tool in the same placeholder grey. It now uses the real colours.
- **Tool changer previews** - each tool's paths are drawn in the colour of the filament
  actually routed to it, instead of painting the whole model in T0's colour.
- **Files that skip a tool number** - a file using T0 and T2 but not T1 now routes T2 to
  its own lane, instead of sliding it onto the next lane in the list. Verified on a
  Snapmaker U1.

**Multi-tool and filament**

- **Reprinting on a Snapmaker U1** now follows the tool routing the original print ran
  with, instead of resetting it to the firmware default.
- **CFS bays** - naming a bay no longer blanks an untagged one, and a manual assignment
  no longer makes an empty bay look loaded. Note that bay labels are remembered only
  until the panel restarts.
- **Cancelling filament mapping** puts the auto-colour preference back the way it was.
- **A remap chosen before printing now sticks** - on tool-changing printers, the mapping
  picked on the file screen was sent to the printer at print start and silently undone a
  fraction of a second later, so the print ran on the original lanes. Any print started
  after a finished, cancelled or failed one was affected.
- **The remap button no longer appears where the printer would ignore it** - on filament
  systems that cannot accept a remap, the pre-flight check and the file screen no longer
  offer the option.
- **A confirmed runout unties the lane from its dead spool** - after a lane runs out and
  is emptied, it forgets which Spoolman spool was in it (brand, material and colour stay),
  so the fresh roll is not silently recorded against the exhausted one. Previously you
  had to relink it by hand.
- **Auto-refill no longer over-promises** - with Creality auto-refill on but no two lanes
  holding matching filament, the status now says plainly that nothing will switch on
  runout, instead of showing backup arrows and then pausing the print to ask for a spool.
- **Inserting a spool into a busy CFS bay** no longer pops a blocking dialog telling you
  to re-seat the spool. The tag read waits for the box to be idle, and if the reader
  reports a problem you see the printer's own message as a toast.
- **One runout, one prompt** - a single filament runout could raise six notifications,
  including two stacked dialogs about the same slot. The filament system's own prompt
  now wins; sensor toasts remain only on systems that have no prompt of their own.
- **Tool spool records survive a reconnect** - remapping or the filament system
  reconnecting no longer wipes which spool is mounted on each tool, and a tool that
  gives up its lane stops claiming - and saving - a spool now feeding another head.

**Dialogs**

- **Dismissing a confirmation no longer strands the screen behind it** - tapping the
  backdrop or pressing ESC used to fire none of the dialog's handlers: Resume could spin
  until a false timeout notice, the Print button could stay disabled, and a half-answered
  delete could remove the wrong saved page. A dismissal now cleans up the same way a
  cancel would.
- **Holding a dialog button no longer fires it after the dialog closed** - if the printer
  closed a confirmation while your finger was still on its button, lifting the finger
  confirmed the already-closed dialog anyway. A quick second tap during the close
  animation can no longer re-run the action either.
- **Recovering from a stop clears leftover fault dialogs** - when the printer comes back
  after an emergency stop, dialogs the fault raised are swept away, so the Print button
  no longer stays disabled behind a stale gate.

**Recovery and stability**

- **A macro that stops the printer** no longer claims the firmware is restarting. The
  recovery dialog comes up instead.
- **A shutdown during an intentional restart** now reaches the recovery dialog rather
  than being silently swallowed - including when an MCU comes back up already halted,
  which previously left the printer looking idle for the rest of the session.
- **Installing an update** no longer locks up the touchscreen. Install, Cancel, then
  Install again could freeze the interface for up to an hour.
- **LED presets** - the preset buttons no longer vanish when a macro light is selected,
  and a macro light that only supports on and off now actually turns off.
- **Long setting labels and descriptions wrap** instead of being cut off. Most visible
  in German, French and Spanish, and on small screens.
- **Memory pressure takes the interface first, not the print** - on printers where the
  touchscreen shares the machine with Klipper and Moonraker, the kernel used to kill the
  print host before the screen, ending the print. The interface now volunteers to be
  taken down first, and its watchdog brings it straight back.
- **During bed tramming or probing**, the position card no longer resizes on every
  actual-Z update. Each 0.01 mm change used to nudge the jog controls out from under
  your finger.
- **Dialogs left open when switching printers** are now freed instead of leaking.

### Changed

- **Preview performance** - object tapping, antialiasing and the shading pass were all
  reworked. Large models load and redraw faster and use less memory.
- **The file screen's filament card was redone** - one card now shows one chip per tool
  the print actually uses, tool number over lane number, instead of separate cards. Chips
  that do not fit become a "+N" count rather than overflowing, accepting a remap updates
  the chips on the spot, a chip whose lane holds a colour the file did not ask for is
  outlined, and an amber triangle and a red X tell "wrong colour" from "needed lane
  empty" at a glance. The sliced-colours switch moved to its own row and appears only
  while the live 3D viewer is on screen.
- **The strip under the preview during a print** now groups its numbers under icons -
  layers, material used, time - and yields space instead of overlapping on narrow
  panels.

## [0.99.117] - 2026-08-26

<!-- whatsnew
The fourth 1.0 release candidate. Highlights:

- A bed mesh that runs long no longer throws away the print behind it
- Saving a Z offset stops warning you it failed when it actually worked
- Chambers and beds get their own heat limit, not one 400C catch-all
- Touch lands where you press on screens that report their size wrong
- AD5X bed screw hints said tighten when they meant loosen
- Large G-code files open faster, and every layer of the preview is drawn
-->

**This is the fourth 1.0 release candidate**, following 0.99.116. Most of the work is in one
theme: HelixScreen telling you the truth about what the printer just did. A pre-print step that
took too long threw the print away without saying so; a Z offset save, a `SAVE_CONFIG` and a
macro that restarts Klipper all completed correctly but reported failure, because a restart drops
the reply they were waiting on. The rest is fit and finish across printers - touch ranges, bed
screw directions, dashboards - and a faster G-code preview.

### Added

**Printers**

- **Printers run their own preparation before probing** - a printer that declares a preparation
  routine now runs it ahead of a probe or a mesh, so probing starts from the state that printer's
  firmware expects rather than from whatever the nozzle was left in.
- **The Snapmaker U1's home screen shows all four toolheads** - HelixScreen drew the U1 with a
  generic single-extruder layout: one nozzle temperature, no tool switcher, and its four RFID
  spools squeezed into a single small cell. It now reads Nozzle 1-4 and Bed, with a four-wide
  spool strip and a tool switcher.
- **AD5X reads native ZMOD's filament system** (#1344) - lane state comes from ZMOD's own status
  objects, so the IFS reports what is actually loaded.
- **New printer art** - a frame for the DuCR10, and real artwork for the Duender entries that
  were using a stand-in.

**Screens and controls**

- **Type exact values on Machine Limits and Retraction** - both screens were slider-only, so a
  precise figure meant nudging until it landed.
- **One-tap macro buttons** - a macro button set to run directly now does, instead of opening the
  macro list first.
- **LED devices pair their own macros** - a discovered `<NAME>_ON` / `<NAME>_OFF` pair is offered
  as one device rather than two unrelated macros, and a macro name that was never detected can be
  typed in by hand.

**Diagnostics**

- **Debug bundles record the touch range** (#1259, #1276) - both the range the panel declares and
  the range it actually reports, which is the pair needed to diagnose a screen that responds in
  the wrong place.

### Fixed

**Starting a print**

- **A slow pre-print step no longer discards your print** - a pre-print block that finished
  between three and twenty minutes had its print silently dropped, with nothing shown on screen.
  On a K2 Plus that window is an ordinary bed mesh; one 32-hour job was thrown away this way. A
  slow step and a cancelled one are now told apart by what happened, not by how long it took.

**Saving to the printer**

- **A successful save no longer reports as a failure** (#1359) - `SAVE_CONFIG` restarts Klipper,
  which drops the reply HelixScreen was waiting on. The dropped reply was read as an error, so
  every successful save raised a failure toast. The save is now judged by Klipper coming back.
- **A macro that restarts Klipper no longer reports that it failed** (#1345) - the same dropped
  reply, on any macro that wraps a restart.
- **Macros that restart the host ask first** (#1345) - a macro that will restart Klipper or the
  host now confirms before running, rather than taking the printer down without warning.

**Temperature**

- **Each heater is held to its own ceiling** (#1355) - every heater was checked against one
  permissive 400C limit, so a chamber or a bed would accept a target far above what its own
  config allows. Each heater now carries the maximum its printer declares, whatever case that
  printer wrote the section header in.
- **Temperature floors clamp at 0C** (#1353) - a printer reporting a negative minimum could push
  the floor below zero.
- **A misreporting sensor stops flooding the log** (#1348) - runs of out-of-range samples collapse
  into a few lines instead of one per reading.

**Touch and display**

- **Touch lands where you press on panels that misreport their range** (#1259, #1276) - the full
  evdev range is now solved from the calibration you ran, not only from the affine correction, so
  a panel whose declared range is wrong is corrected end to end.
- **A pointer device is no longer configured as a touchscreen** - a libinput pointer was being
  sent touchscreen-only calibration calls it cannot answer.
- **The touch jitter filter is gone** (#1358) - it was unreachable, so it did nothing but add a
  setting.

**Printers**

- **AD5X bed screws turn the right way** - the AD5X was the only FlashForge entry missing a screw
  direction, so the panel said tighten when the correct action was loosen. Confirmed on hardware.
- **Bed screw adjustment names the right screw, and the shorter turn** - the reference screw was
  misidentified and the instruction always offered the long way around.
- **MedusaHC hotend changers are detected on their own** - a MedusaHC that runs without
  klipper-toolchanger was not seen as a changer at all: no dock sensors, no feeder, just six
  plain extruders.
- **AD5X shows heater tiles first** - the status block led with fields that pushed the
  temperatures out of view.
- **The Centauri Carbon dashboard fills its screen** - it was laid out for a smaller panel.
- **AD5X reports an empty slot as empty** (#1344) - the firmware's "no material" sentinel was
  read as a material name.
- **K1, K1C and K1 Max use their whole home screen** - the landscape layout was authored for a
  narrower grid, leaving six of twenty-four cells dead and hard-wrapping the tips box mid-word.

**Filament**

- **A lane is named by what is mapped to it** - a mapped lane showed only its colour, so two
  lanes of the same colour were indistinguishable.
- **A stand-in colour no longer overrides the file's own** - a placeholder could replace a colour
  the G-code actually stated.
- **Printers with a cutter stop asking you to cut by hand**.
- **Filament totals survive a cached file** - opening a file whose tool usage was already cached
  left the palette and the gram estimate blank.
- **The bypass-spool warning is translated** - it appeared in English in all nine languages.

**G-code preview**

- **Every layer of the model is drawn** - the preview's sampling step rounded up, which skipped
  rows of the model on some files.
- **Large files open faster** - indexing no longer re-parses a streamed file behind the UI, and
  the preview pass is sized to the screen rather than to a fixed constant.
- **Closing a preview while it loads no longer crashes** (#1347).

**Screens**

- **An unnamed bed mesh profile no longer overwrites the default** (#1360) - tapping Save with the
  name box empty wrote over the stored `default` mesh without confirming, and a name of nothing
  but spaces was accepted.
- **The job queue count appears** (#1349) - the count was never published, so the queue widget and
  its modal never refreshed and the Job Queue row stayed hidden however you had set it.
- **The LED device editor keeps what you add** - the Add button discarded every new entry, so
  tapping + only told you to tap +. Its fields also sat on two rows, and macro exclusions matched
  fragments of names rather than whole words.
- **Leaving a keypad by the navbar no longer costs the next visit its refresh** - Machine Limits
  and Retraction would then show last-known values instead of querying the printer.
- **A long filename spans the idle card** - it was confined beside the thumbnail, at under half
  the width the card had.
- **Switching printers clears the previous printer's excluded objects** - they stayed on screen
  indefinitely on a printer with no exclude-object support.
- **Print history stops fetching itself twice** - on an AD5X a 500-job list took ten seconds and
  most of a megabyte, then immediately went out again for a list nothing had changed, stalling
  the printer's own queries behind it.
- **Console history is filtered like live console output** - firmware noise was stripped from new
  lines but not from the history fetched when the screen opens.
- **A nozzle-cleaning macro you assign yourself enables nozzle cleaning** (#1354).
- **A camera is only reported when its service is actually running** (#1351).

**Installing and updating**

- **The backlight rule is installed** (#1343) - the step that installs permission rules was never
  called, so screen brightness control did not work on a fresh install.
- **The K2 can install again** - a K2 whose `wget` is a Python shim could not fetch the manifest,
  and the fallback then read two kilobytes of release notes as the version number.
- **The K1 installer checks the partition it actually writes to** - `/etc/init.d` lives on a
  ~97MB overlay while the install directory has gigabytes free. It measured the wrong one, so a
  full overlay passed preflight and the install died after stopping the stock UI, leaving a dark
  screen.
- **Caches from an older layout are reclaimed** - a K1 that had run an older build kept a second
  cache on its smallest partition, competing with the firmware for space.

**Crash reporting**

- **Duplicate crash reports collapse** - reports were grouped by raw address, which changes with
  the architecture, so a single fault filed a separate issue per printer family.
- **A connection-state race in the WebSocket client is closed**.

## [0.99.116] - 2026-08-24

<!-- whatsnew
The third 1.0 release candidate. Highlights:

- The FlashForge AD5X is its own printer now, not one borrowed from the AD5M
- Touch works on first boot on AD5X and AD5M
- K2 camera traffic no longer fills the printer's disk and kills your uploads
- Opening a large G-code file no longer restarts the app on low-memory printers
- A real unload for K1 printers, and honest pre-print countdowns on K1C and K2
- MedusaHC hotend changers, and tool changers that remember their spools
-->

**This is the third 1.0 release candidate**, following 0.99.115. Two areas carry most of the
work: making the FlashForge AD5X a first-class printer rather than one borrowed from the AD5M's
settings, and finishing the filament path so that a lane tap, a spool weight and a pre-print
warning all describe the lane your print will actually use.

### Added

**Printers**

- **FlashForge AD5X installs as its own printer** - it was set up using the AD5M's paths, which
  do not exist on an AD5X. Its cache and log now go where they belong, and an AD5X support
  archive finally carries HelixScreen's own log rather than only the mod launcher's.
- **Touch works on first boot on AD5X and AD5M** - both panels now ship with a measured
  calibration, so the screen responds correctly before you run the three-point tap routine. A
  calibration you save yourself still wins.
- **Forge-X keeps a Z offset permanently** - Forge-X joins the firmwares HelixScreen can ask to
  store the offset rather than losing it on restart.
- **Renamed printer entries still match** - renaming an entry in the printer database used to
  orphan every setting saved under the old name. Former names are recorded now, so a rename no
  longer reads as a different printer.
- **The setup wizard skips the connection step on printer-embedded builds** - a package built for
  one printer already knows where Moonraker is, so it stops asking.

**Filament**

- **MedusaHC hotend changers** - the dock sensors and the servo feeder are read, so a MedusaHC
  reports the tool that is actually mounted rather than the last one it was told to pick. Unload
  also reappears on the Snapmaker U1, which had it hidden for the same reason.
- **Tool changers remember their spools** - colour, material, brand and weight set on a tool did
  not survive anything: every reconnect reset all four tools to grey. They persist now.
- **Filament checks weigh each tool against its own lane** - a multi-tool print used to compare
  the whole file against the one external spool, so a print remapped from one tool to another
  kept warning about a spool it would never touch.
- **A true unload for K1 printers** - Creality's own unload pushes filament roughly 38mm further
  in than it pulls out, because it expects you to cut it at the inlet. HelixScreen now ships an
  unload that renews the melt and pulls the filament clear of the gears. A printer that already
  unloads properly is left alone.

**Printing**

- **The K1C Auto Bed Mesh toggle actually meshes** - it set a parameter the firmware ignores, so
  On did nothing and Off skipped every preparation step, homing included. It now runs one real
  preparation pass at the job's own temperatures.
- **Measured first-print timings for K1C, K2 and K2 Pro** - the generic estimates were out by
  100-800% on these printers, so the countdown before the first layer was fiction. The shipped
  defaults are measured, and learned history takes over after your first prints.

### Fixed

**Crashes and stability**

- **Opening a large G-code file could restart the app on K2 Plus** - a printer with no 3D renderer
  loaded the whole file at once: a 130 MB file grew the app to 387 MB, the system killed it, and
  the preview looked like it was stuck on the slicer thumbnail forever. That screen streams now,
  like every other one.
- **Occasional torn or frozen frames** (#1338) - the render thread and the drawing thread agreed
  on when a frame was finished through values neither one was guaranteed to see.
- **Rare crashes when leaving a screen** - the network widget's polling timer, cached Wi-Fi
  settings pointers, the power screen's cleanup hook and the Wi-Fi manager's shutdown could each
  reach a screen after it was gone.
- **A repeating warning no longer empties a debug bundle** - one warning firing thousands of times
  pushed everything else out of the history the bundle collects.

**K2 and K2 Plus**

- **Uploads dying with "connection reset"** - registering the camera routed every frame through
  the printer's own web proxy, which logged it to a RAM disk with no rotation. About 147 MB a day
  filled that disk, and Moonraker keeps its upload temp file there too, so uploads died mid-stream
  and the browser only saw a reset. Camera install now turns that one log off and reclaims an
  oversized one; uninstall puts the file back byte for byte.
- **The K2 webcam is registered as a stream** rather than the per-frame polling type.

**Filament**

- **Tapping a lane while bypass was on loaded nothing** - the tap fell through to the external
  spool macro; on a K2 Plus that ran a load routine gated on a sensor with nothing in it, reported
  success and let the nozzle cool. The lane you tap now reaches the filament system, and the one
  case that cannot be honoured is refused out loud instead of silently.
- **Spool weights on a second AMS were never refreshed** - only the first unit's lanes were polled,
  so a second unit's weights sat at whatever was last typed in and the low-filament checks read
  them as fact.
- **A Spoolman vendor the catalog does not know no longer empties the material list** - on the
  reporter's K2 only 5 of 19 vendors matched, and picking one of the other 14 left no way to set a
  material at all.
- **Spoolman availability no longer flickers off** during discovery.
- **A black bypass spool is drawn black**, not white, and empty picker rows are dimmed properly.

**Print status and previews**

- **The preview no longer sticks on a placeholder** (#1339) - a print with no slicer thumbnail
  retired the preview reload, so nothing replaced the placeholder for the rest of the print.
- **Print status follows the print you started**, resolving a rewritten upload path the way the
  file list does, keeping the preparing job's identity, and dropping the previous print's model
  when a new one begins.
- **Long filenames scroll** in the library's active view, and the filename spans the detail card.
- **A confirmed long-press delete stays on the print list** instead of leaving the screen.

**Screens and settings**

- **Time zones** (#1340) - UTC+7 was reachable only as "Indochina", which a reporter in Vietnam
  never found. Zones are named for the countries people search for; same-offset zones with
  different daylight-saving rules are listed separately, since picking Denver from Arizona is an
  hour out for half the year. Africa had no entry at all, and -11, -2 and +13 were unreachable.
  32 zones became 61.
- **The default theme is HelixScreen** - the built-in fallback was still Nord, so a printer that
  fell back to it changed colour scheme.
- **A setting reads its documented default while it is still loading** (#1288) rather than showing
  a blank or a zero.
- **The printer type mismatch prompt no longer fires when nothing is wrong.**
- **UI sounds are audible on HDMI and S/PDIF again** (#1337) - those receivers mute themselves
  while re-locking a clock that stopped, swallowing half a second to a second, which is longer than
  every UI sound. Those outputs keep their clock running now.
- **Remote screen mirroring** (#1334) - where DRM is unavailable and the display falls back, the
  mirror read from the wrong row, so part of the screen showed the wrong content and the rest
  stopped updating with a single warning.
- **The home screen's bypass tile could never appear at all.**
- **A Z offset set while idle was stored short on ZMOD** by the last print's probe correction, and
  ZMOD's placeholder colour label no longer renders as a dash on the filament tiles.
- **Two strings blanked in eight languages** by a duplicate translation key are back.
- **Update Channel is hidden** where update checks are turned off, and the Snapmaker U1 defaults to
  firmware-managed updates.
- **The standalone uninstaller restores the platform files** it was skipping.

### Changed

- **Endless spool status is attention-only** - the healthy sentence no longer takes up space on the
  filament screen, and the status moved to the bottom of the left column. Off, Unknown and
  Needs Plugin still surface.
- **The G-code preview frames against the real metadata bounds**, so a model no longer sits
  off-centre in its card.
- **Graph colours come from the active theme** rather than a fixed palette.

## [0.99.115] - 2026-08-20

<!-- whatsnew
The second 1.0 release candidate. Highlights:

- Printers are no longer guessed at from thin evidence, and you can correct the model yourself
- The window between pressing Print and the first layer is now tracked properly
- Buttons no longer send macros your printer does not have
- Input shaper sweeps your printer's real frequency range and shows what changed
- K2 Auto Bed Mesh actually meshes, at the job's own temperatures
- Fixes for crashes in the power panel and panel navigation
-->

**This is the second 1.0 release candidate**, following 0.99.114. The bulk of the work is in two
areas: identifying your printer honestly rather than guessing, and modelling the preparation
window between pressing Print and the first layer going down.

### Added

- **Printer model picker.** Printer Manager's model row is now editable. If your printer was set
  up as the wrong model, you can correct it without deleting the printer and starting over.
- **Print preparation is a tracked job.** The window between pressing Print and the first layer
  now has its own lifecycle, so the print card, media, progress and completion reporting follow
  it instead of showing the previous print or reading as idle.
- **Bypass toggle home-screen widget**, with print-start gating and runout arming.
- **Instant filament colour chips** on the print detail page, answered from a cached G-code read
  keyed by path, size and modification time.
- **Input shaper** now sweeps the printer's real frequency range, shows an old-versus-new delta,
  displays a spinner with elapsed seconds during analysis, and warns when firmware overwrites the
  X result.
- **AMS spool retention.** A Keep Spool Info on Eject toggle, surfaced over the eject control on
  firmware that reports spool IDs, plus external spool support on CFS.
- **New pre-print checks:** bypass engaged, unaccounted toolhead, and a warning when the filament
  grade changes rather than only when the material does.
- **Homing delegation.** Printers whose filament system homes for them no longer get a redundant
  home prompt or G28.

### Fixed

**Printer identification**

- Detection no longer saves a low-confidence guess. Below 85% confidence it records nothing and
  leaves the choice to you, so a later reconnect or your own correction settles it.
- The detector was running without most of its evidence: the Klipper object list was classified
  but never retained on the command-line detection path, so every object-existence and macro
  heuristic silently scored zero. That path is what the installer uses to seed a printer preset.
- Build volume now reaches detection during discovery instead of arriving afterwards. It can
  break a tie between otherwise identical printers, but can never identify one on its own.
- An LED named `chamber_light` no longer reads as a FlashForge Adventurer 5M Pro. Two users'
  Vorons were labelled that way.
- When a saved printer type contradicts what the hardware says, the warning now records why it
  declined to prompt, so a debug bundle can explain itself.

**Macros and filament**

- Configured macro buttons are verified against the printer. A macro your printer does not have
  no longer reaches the dispatch path, where it outranked the filament system itself and
  dead-ended the fallbacks beneath it.
- AFC's toolchange counter is read as the 1-based count it is, so a print no longer ends at
  "162 / 161".
- AFC lane data keyed by tool number resolves through the tool mapping, and mapping resets use
  the name the firmware actually accepts.
- CFS auto-refill and colour writes match Creality's own screen, and bypass unload uses the
  extruder rather than the box's bay retract.

**Crashes**

- Navigation kept raw pointers to panels after their widgets were deleted, and wrote through
  them from queued updates.
- The power panel kept cached widget pointers past their tree's death, and deleted device rows
  from inside a queued batch.

**Printing**

- The K2 Auto Bed Mesh toggle now actually meshes, at the job's own temperatures, with its points
  counted once.
- A print that dies inside PRINT_START is reported as a failure and cools down, instead of
  hanging in preparation.
- Print status resolves media from the preparing job rather than the previous print, and runs its
  per-job resets for prints started in the app.
- Pre-print blocks no longer leave the home screen's print card reading idle.
- The post-unload runout grace is bounded and no longer consumed by gated calls.

**Screens and widgets**

- Two-dimensional G-code view responds to tap and long-press for object selection, and will not
  select geometry it did not draw.
- Loaded filament colour survives a render-mode switch.
- Gated tiles no longer open panels for hardware that is not present.
- Active Spool widget text stays inside its card.
- Touch calibration draws its capture dots through the pre-session calibration.

**Installation**

- Free space is checked before the cross-filesystem install swap, scratch directories are
  reclaimed, and the build host's user and group IDs no longer ship into installs.

### Changed

- Bed-size heuristics now corroborate a printer match rather than establishing one. At common bed
  sizes a dozen or more printer entries claim the same dimensions, so bed size alone is not
  identifying evidence.
- Printer detection may legitimately return no answer. Two printers that present identically over
  Moonraker are ambiguous, and declining is preferred to guessing.

## [0.99.114] - 2026-08-16

<!-- whatsnew
The 1.0 release candidate. Highlights:

- Fixes a lit-but-frozen screen on Centauri Carbon and AD5M
- Idle printers no longer refuse jogs as "busy"
- A Klipper shutdown is no longer displayed as Ready
- CFS: checks filament really moved, plus fan and recovery fixes on K1
- AFC: correct buffer health, tool numbers and spool names
- Single-hotend printers stop reporting tools they do not have
- Dialogs fit on small screens
- New Hazard theme, and lower memory use throughout
-->

**This is the 1.0 release candidate.** It is the code intended to ship as 1.0.0, published as
an ordinary 0.99.x update so that everyone gets it rather than only people who opted into a
beta channel. If it holds up in the field, 1.0.0 follows with no further changes. If it does
not, the fix ships as 0.99.115 and no bad 1.0.0 ever exists.

This started as a sweep through every issue still open against 1.0 and turned into the largest
release the project has shipped. Two fixes stand out because both presented the same way, as
"the printer stopped responding", with nothing in the log to explain it. Waking the display
could park the UI thread on a lock and leave a lit screen ignoring every touch, in one case for
36 minutes. And a Klipper shutdown could be overwritten by a stale snapshot replayed at the end
of discovery, so the app showed Ready for two and a half minutes while the printer was down,
re-enabled navigation and kept sending commands to a dead printer.

Several other defects had gone unnoticed because the wrong thing was being measured: the
Lifetime Print Stats tile quietly reported only your most recent 500 jobs, accelerometers were
never discovered at all so Settings > Sensors was empty on every printer that has one, and two
printers were missing the database entry that applies their preset.

### Added

- **A new theme, Hazard.** ANSI Z535 safety colours on powder-coated black: caution yellow,
  orange for warning, red for danger, with sharp corners and brass seams. Dark-only.
- HelixScreen now detects and reports a hung UI main loop, so a freeze leaves evidence behind
  instead of looking like a dead process. Detection only, nothing is killed. The threshold
  defaults to 60 seconds and is settable at runtime or with `HELIX_HANG_THRESHOLD_SEC`.
- When updates cannot be installed, the notice is now a row you can tap. It explains how to
  update from a terminal and shows a QR code to the documentation.
- The Android app is documented in the README and the install guide: which of the three APKs to
  take, why the `.aab` is not for users, sideloading, permissions, and the uninstall needed to
  move to a future Play Store build.
- `helix-screen ctl` can address unnamed widgets and set label text directly, which makes far
  more of the interface reachable for scripted testing and screenshots.

### Fixed

**Freezes and connection state**

- **A lit screen that ignored every touch.** Waking the display forced a WebSocket reconnect on
  the UI thread, and that reconnect waits for in-flight callbacks with no time bound, so one
  undrained callback parked the main loop permanently. Seen on Centauri Carbon and AD5M and
  confirmed on two wedged devices. The reconnect is now Android-only, where it is actually
  needed, and the wait is bounded
  ([#1245](https://github.com/prestonbrown/helixscreen/issues/1245)).
- **A Klipper shutdown could be displayed as Ready.** Klippy state arrives from two unordered
  sources, and the snapshot replayed at the end of discovery could put Ready back over a live
  shutdown. That re-enabled the nav buttons, auto-dismissed the recovery dialog moments after it
  correctly appeared, and let commands through to a dead printer. Live frames now win over
  replayed ones. Relatedly, an unrecognised printer state no longer falls back to Ready.
- **Jogs refused as "Printer is busy" on an idle printer.** Klipper reports the idle timeout as
  Printing while any gcode runs at all, so housekeeping loops such as bed fan timers, air filter
  timers and AFC's own prep made roughly 7% of jog attempts bounce. HelixScreen now waits for a
  full second of continuous activity before it treats the printer as busy.
- **The change-Moonraker-address prompt landed on top of the setup wizard.** On a fresh install
  it appeared about a minute in, over the wizard's own Connection step, giving two competing
  host-entry dialogs.

**Creality CFS** ([#968](https://github.com/prestonbrown/helixscreen/issues/968), [#1278](https://github.com/prestonbrown/helixscreen/issues/1278))

- **CFS operations now verify that filament actually moved.** A load or unload the firmware
  reports as finished is checked against the toolhead sensor, and a mismatch raises a fault with
  its own recovery action rather than being reported as success.
- **The part-cooling fan no longer blows on the nozzle during every K1 load, cut and flush**, and
  two runout messages that previously paused the print with no dialog at all now show one. Both
  came from commands wrongly believed absent on K1; the K1 firmware image proves otherwise.
- **CFS error recovery works on K1.** It was sending a resume command that firmware does not
  register, so the box never resumed.
- Filament changes no longer pass a parameter the firmware does not accept, so the purge runs at
  your configured length instead of silently falling back to defaults.
- The filament code catalog is cached rather than re-parsed on every box update, which was a
  100 KB parse every time a spool moved.

**AFC and BoxTurtle**

- **Buffer health is attributed to the right unit and stays there.** On multi-unit rigs it landed
  on unit 0 for every unit, then went blank for minutes until the firmware happened to push a
  change. Found on a five-unit rig.
- **A Snapmaker U1 with five units drew six toolheads for four extruders, all claiming T0.** AFC
  publishes free-form config section names, and six places parsed them with a grammar that only
  fits Klipper's own. They now resolve names the way Klipper does.
- **Spool names and vendors resolve from AFC's lane data** using the key names AFC actually
  publishes.
- **Spoolman users are no longer told to upgrade AFC to get filament names.** Names already
  resolve through Spoolman; multi-colour spools are the part that needs the newer AFC, and the
  notice now says so.
- **A spurious warning told users to add an `extruder_name` setting AFC refuses to start
  without.** It fired because the config query lands just after the first status frames.
- AFC lanes follow the firmware's own `current_map` rather than guessing the lowest tool number,
  so multi-tool lanes drive the tool the firmware actually selected.
- Spool assignments are no longer rewritten on weight noise. A lane reporting weight as a float
  that drifts by hundredths caused 590 rewrites of the spool file, 590 database writes and 590
  filament-panel rebuilds in a single session.
- Config writes no longer trigger a full file-list reload. An AFC rig caused 113 complete
  directory round trips in one session while the user sat on the print status screen.

**Tool and nozzle counts**

- **A single-hotend printer behind a 4-lane AMS no longer reports four tools.** The nozzle label,
  the print-status tool badge, its chevron and the nozzle picker all counted lanes rather than
  hotends, and the picker went on to offer nozzles the printer does not have.
- **Preheating "All" on such a printer sent the same target four times** and the confirmation
  claimed it had heated four tools.
- **A 4-port AD5X no longer advertises a 16-tool machine.** It was reporting every addressable
  tool number the firmware register can hold rather than the ones actually mapped.

**AMS and filament**

- **`PLA+`, `PA6-CF`, `PETG-CF` and every other punctuated material name were silently dropped**
  while the save reported success. Our own filament database ships `PLA+`. Names containing a
  space, such as `Silk PLA`, never worked either.
- **A fifth AMS unit produced parser warnings and a permanently dark temperature and humidity
  badge.** The unit limit is raised to eight, and the environment overlay stops showing unit 0's
  humidity for every unit.
- **Spools with no usable weight are drawn full rather than half full**, which had read as half
  gone. Four places in the interface had drifted to different answers and now agree.
- A crash when opening the AMS context menu, and a deadlock risk on Snapmaker hardware, are both
  closed.

**Screens, dialogs and widgets**

- **On small displays a modal's button row could fall off the bottom of the card**, leaving a
  dialog that could not be dismissed. Every breakpoint's limit is now measured from the real
  modal on screen and re-verified from 480x272 up to 2560x1440
  ([#1277](https://github.com/prestonbrown/helixscreen/issues/1277)).
- **A home-screen widget that would not fit announced itself as "removed" on every launch**, and
  a widget that was enabled but held no cell showed in the catalog as placed, greyed out and
  impossible to select or remove.
- **Widget content no longer clips or wraps one letter per line on larger displays.** The size
  bands were a single set of pixel values calibrated for small screens.
- Row 2 of Calibration & Tools no longer runs its labels together when it carries four
  conditional buttons.
- Toggling the printer switcher in the navbar takes effect immediately instead of waiting until
  you leave the overlay.
- A crash in grid edit mode when deleting a widget mid-resize is fixed.
- The print status header no longer shows an empty action button during a print.
- Page-scroll chevrons on the home screen stop landing on a widget's own content.
- Several dialogs that could grow past the screen are capped and scroll properly: AMS loading
  errors, the shutdown and recovery dialogs, and action prompts. The AFC fault diagram no longer
  sits inside the scrolling text area.

**Z-offset**

- **The Z-Offset row showed 0.000 whenever an AD5M or AD5X was idle, and adjusting it while idle
  could throw away your real offset.** ZMOD keeps the authoritative offset in its own storage and
  clears Klipper's live one when a print ends, so an idle nudge was applied to a phantom zero and
  that is what got saved. HelixScreen now reads the stored value, shows it while idle, and sends
  an absolute adjustment when the live offset is not the real base.
- **A printer whose offset sits past 2mm is no longer yanked down to 2.000 on the first tap** in
  the tune overlay, which drove the nozzle into the print. The limit now applies to how far one
  tuning session can travel, not to the offset itself
  ([#1280](https://github.com/prestonbrown/helixscreen/issues/1280)).

**Print history and status**

- The Lifetime Print Stats tile now reports true lifetime totals from Moonraker instead of
  summing the most recent 500 jobs. If your totals disagreed with Mainsail, this was why
  ([#1272](https://github.com/prestonbrown/helixscreen/issues/1272)).
- The idle "Reprint Last" tile no longer offers a file you deleted.
- A prompt button whose macro fails now reports Klipper's own error message instead of closing
  the dialog and leaving an empty screen.

**Hardware, printers and presets**

- Accelerometers are discovered again. Settings > Sensors listed nothing and showed a count of
  zero on every printer with an ADXL345 or LIS2DW
  ([#1262](https://github.com/prestonbrown/helixscreen/issues/1262)).
- The Elegoo Centauri Carbon and Artillery M1 Pro now pick up their presets. Without the
  database link, neither printer received any of its preset settings unless it was installed
  from the factory image ([#1260](https://github.com/prestonbrown/helixscreen/issues/1260)).
- **Touchscreens that report only multi-touch axes are calibrated correctly.** Every coordinate
  past the screen width had been saturating at the edge
  ([#1259](https://github.com/prestonbrown/helixscreen/issues/1259)).
- **An AD5X slot with a type but no colour no longer aborts the Change Type dialog** with nothing
  left to reopen, and the phase display stops inventing a 230C target the printer never had.
- Screen rotation is now a setting in Display & Sound rather than something only reachable by
  editing config, and the rotation probe no longer misses every tap.
- A nozzle sitting just above its target no longer shows as heating while calling itself Ready.
  The temperature shown and the state described are now derived from the same value.
- Large g-code files are handled correctly on 32-bit builds, and the DRM backend keeps its
  64-bit buffer offset there, fixing display init on 32-bit Pi images.
- A printer preset no longer applies its panel's display and touch settings to a different
  machine's screen when HelixScreen is talking to that printer over the network.
- A fresh install no longer loses its shipped platform preset when an earlier `printer_data`
  directory outlives the install.

**Spoolman setup**

- **Spoolman setup no longer dead-ends on a stock Creality K2**, where Moonraker's config lives
  outside the only writable file root.
- **The AD5M's config is no longer rejected as unwritable** when it is in fact writable through a
  linked path.
- Setup no longer claims Moonraker reads its config from somewhere else after any config edit
  since Moonraker last restarted, and a guessed config path must now match exactly before
  anything is written to it.

**Updates and installation**

- **On a standalone-display install the entire Software Updates section vanished**, and the fix
  could only reach users inside the update they were being kept from. HelixScreen now recognises
  the second update route the installer supports, and checking for updates is separated from
  installing them so a machine that cannot install can still tell you an update exists.
- **The installer's own advice could not be followed.** Nine messages told users to repair an
  install with a command that needs `bash`, absent on the BusyBox firmwares, and that would have
  performed a fresh install rather than an update.
- **Upgrading from the CLI failed with a permission error** even after the installer reported
  sudo was available, because it asked whether it could write into a directory rather than
  whether it could rename it.
- **Unprivileged Raspberry Pi installs no longer land under a root-owned parent**, which left
  only the destructive fallback path ([#970](https://github.com/prestonbrown/helixscreen/issues/970)).
- **Three permanent PolKit warnings in Mainsail and Fluidd are gone on Centauri Carbon and K2
  Plus**, and a Moonraker restart now works on firmware that uses neither systemd nor the init
  script we knew about.
- Unlocking beta features, selecting the Dev update channel, then locking beta again no longer
  leaves the app fetching from Dev with no way back to Stable.

**Sound**

- **A printer with sounds turned off no longer holds the audio device open for the whole
  session**, writing silence every period. On Android that also kept an `AudioTrack` open
  ([#1253](https://github.com/prestonbrown/helixscreen/issues/1253)).
- The PWM buzzer backend no longer probes for a beeper on boards that have none, and the AD5M
  stops shipping 919 KB of music it cannot play. `M300` still works everywhere.

**Android**

- **The 1.0 release would have been refused as a downgrade on every existing Android install**
  and rejected by the Play Store, because of how the version code was packed.

**Translations**

- **All nine languages are now fully translated**, up from 99.7% in the eight non-English ones.
  The runtime CJK fonts are rebuilt too: eight characters the new Japanese and Chinese text needs
  were in no font file, and a missing character draws as an empty box with no warning anywhere.
- Translations resolve all C escape sequences in keys, not just hex ones, so a further batch of
  text stops falling back to English.

### Changed

- **Memory use is down across the board**, which matters most on the Centauri Carbon and other
  boards that swap at idle. About 1.2 MB of address space returned by capping allocator arenas,
  1 MB off the CC1 binary by dropping mocks and asserts from shipped builds, 345 KB of fonts that
  the per-platform tiers had never actually pruned, and 615 KB off the shipped layout files, of
  which roughly 310 KB stays resident for the whole session. The print history, the filament
  tables, the keypad and the tips database all stop holding memory they no longer need.
- Debug bundles now include the startup logs. Everything config-related happened before the
  logger was installed, so a bundle could show a "settings were corrupted" message with not one
  matching line anywhere in it.

### Internal

- **The sanitizer CI gates were reporting green while failing.** A run with ten real
  AddressSanitizer reports was reported as passing, because the output was piped without
  `pipefail` and a success banner printed unconditionally. Fixed, along with the three standing
  findings the masked gate had been swallowing.
- **The XML formatter's `--check` claimed files were clean that it had never managed to read**,
  and its errors were being sent to `/dev/null`. Three layouts had been unreadable to it.
- Two use-after-frees closed (the AMS context menu's subjects outliving their storage, and a grid
  resize animation keyed on the wrong object), a latent out-of-bounds write removed, and a
  lock-order inversion between the Snapmaker backend and AMS state fixed.
- **The Android release-signing gate was vacuous** and would have passed on an artifact whose
  signature was never read. This is how v0.99.43 shipped a debug-signed bundle.
- **The Play Store "What's New" text was being chopped mid-clause** on every release, and its
  last two entries silently swapped. A release section can now carry an explicit block used
  verbatim, with a hard error if it exceeds the limit.
- Three Cloudflare Worker test suites had never run in CI. They do now, along with the analytics
  dashboard build.
- The `Build` workflow had been red for a day because the Moonraker plugin test dependency was
  never declared. Declared, with a gate so it cannot recur silently.
- Emergency-stop overlay observers now carry a lifetime token, closing a use-after-free that
  reproduces deterministically when the guard is removed.
- The install base is now estimated from CDN update polls rather than opt-in telemetry alone,
  which puts the real fleet at roughly double what had been quoted.

## [0.99.113] - 2026-08-13

A fix release for two problems that showed up in the field on the Adventurer 5X. Uploading a
debug bundle crashed the app outright on nearly every printer, and because the crash happened
inside the reporting path itself, the bundles that would have reported it never arrived.
Separately, Wi-Fi setup in the first-run wizard could sit on "Connecting" forever after a
password was entered, with no timeout and no way back to the form. Nine languages also pick up
a large batch of text that had been silently falling back to English.

### Fixed

- Uploading a debug bundle no longer crashes on any printer whose `printer.cfg` has more than
  one `[include]` line, which is very nearly all of them. Introduced in 0.99.112.
- Wi-Fi setup in the first-run wizard now reports a timeout after 45 seconds instead of showing
  "Connecting" indefinitely. Cancelling an attempt no longer leaves every later password prompt
  stuck on a spinner with no fields to fill in.
- Debug bundles no longer download and process each log file twice. On a 473 MB printer that was
  an extra multi-megabyte fetch and parse on every single upload.
- Printer details in a debug bundle are now captured before the upload starts rather than read
  from the upload thread, closing a rare crash.
- Text that the interface translates indirectly - filament type names and several status
  messages - now appears in all nine languages instead of falling back to English.
- Filament runout guidance on the Adventurer 5X is now written as whole sentences, so it can be
  translated properly instead of being stitched together from untranslatable fragments.
- The Active Spool widget shows the filament brand from Spoolman
  ([#1264](https://github.com/prestonbrown/helixscreen/issues/1264)).
- Spoolman weight polling now starts at boot, and spool labels refresh when identity details
  arrive rather than staying blank.
- AFC lane maps that arrive as a list are parsed correctly, and the AFC mock no longer reports a
  vendor that real AFC hardware never sends.
- Four chatty log sources no longer crowd out useful history in the debug-bundle ring buffer.

### Changed

- Constants throughout the codebase were renamed from `kCamelCase` to `UPPER_SNAKE_CASE`, the
  convention the contributor docs always specified. No behaviour change.

## [0.99.112] - 2026-08-13

The temperature graph becomes interactive: tap a plotted line and it captions the sample
under your finger, including what the heater was aiming for at that moment. The Klipper
plugin gets an important fix - enabling phase tracking could silently leave PRINT_START
holding two copies of its own body, so every print homed, levelled and heat-soaked twice.
AMS tool-mapping restore now waits for the firmware to confirm the change rather than
trusting its own echo. The ESP32-S3 firmware moved from "first boots" to "survives".
Downloads are 8.1 MB smaller.

### Added

- **Tap the temperature graph to read a sample** - tapping a plotted line pins the nearest sample and captions it with the temperature, the target in effect at that moment, and the time. The caption follows the sample as the graph scrolls, and the hit test covers the drawn line itself, not just the sample dots - so a steep heater ramp, where consecutive samples sit hundreds of pixels apart, is tappable where you can see it.
- **Home temperature tiles show the target while heating (#1267)** - a nozzle, bed or chamber tile climbing to a setpoint now says what it is climbing toward. An idle tile stays a bare reading rather than gaining a permanent "/ -", and the smallest panels are left alone where there is no room.
- **AMS mapping restore is confirmed against the firmware (#1270)** - after a print that remapped lanes, the restore waits for the backend to echo the change back before clearing its recovery record. Backends update their own registry optimistically, so comparing against ourselves confirmed exactly the failure it was meant to catch. Covers AFC, Happy Hare and CFS (not on K1, where the command is a confirmed no-op); backends that publish no status keep the previous behaviour instead of waiting for proof that cannot arrive.
- **Printer artwork for the Artillery Genius Pro, Kingroon KLP1 and Sovol SV07**, cut to match the shipped set. Every database entry now either has artwork or is explicitly listed as missing, so a new printer cannot silently fall through to a generic frame.
- **Debug bundles capture printer.cfg and the macro list** - the config and its include tree ride in their own field rather than competing with the incident window in the Klipper log.
- **An update-channel switch can move backward** - someone who tried the devel track and switched back to stable is ahead of the channel they now want, and used to be told "up to date" forever with no way back. Moving backward is offered with a confirmation, never auto-notified.

### Fixed

- **Enabling phase tracking could duplicate your PRINT_START (#1268)** - the config rewrite ended its capture at the first genuinely empty line, so the original tail survived and PRINT_START came out holding the instrumented body followed by an un-instrumented copy: double home, double QGL, double heat-soak on every print, with no config error to indicate anything was wrong. Enabling now also fails closed if the macros it injects are not defined anywhere in the config - which they never were, because the installer had been pointing at a file merged away some releases ago.
- **Printer-fault alerts stayed up after Klipper recovered (#1266)** - an MCU disconnect raised an alert that survived a FIRMWARE_RESTART from Mainsail, and a cascade of them had to be acknowledged one by one on an already-healthy printer. Alerts raised for a printer-side fault are now swept when Klipper returns to ready.
- **Mapping restore could be spent on a halted Klipper (#1270)** - every backend's send reports success unconditionally, so commands Klipper refused counted as restores; the snapshot was then cleared and the recovery record deleted, stranding the printer on the print's mapping. A halted Klipper at print end is exactly what a cancelled or errored print looks like. Restore now waits for Klipper to come back.
- **Eject and three other AMS actions did nothing on multi-unit setups (#1258)** - the Overview panel and the AMS panel each had their own copy of the context-menu switch and the two had drifted, so Eject, Select Gate, Check Gate and Clear Spool were discarded with no toast and no log line. Single-unit installs were unaffected, which is why a BoxTurtle+NightOwl user had a dead Eject button.
- **AFC lane eject mirrored refusals that were not ours to make (#1258)** - our copy of AFC's own rules had forked into two version eras selected by sniffing the status payload, and there is no reliable AFC version to read. We now send the command and let AFC decide.
- **Touch calibration was unreachable on a rotated panel (#1259)** - the Settings entry point was gated on the same heuristic that decides whether to auto-fire the wizard, and that heuristic cannot see a touch panel mounted 90 degrees from the display. Reported on an FLSUN T1 Pro. Any real touch panel can now reach the wizard manually; the first-run auto-fire keeps its narrower test.
- **A pre-print warning about an option you never chose (#1269)** - adaptive bed mesh emitted its parameters on printers with no way to deliver them, so start-print dropped them and warned on every print. The emit is now gated on deliverability, and the warning names what was actually dropped.
- **Icons kept the size they were built at (#1210)** - the first icon ever created pinned the font face for every icon after it, so a runtime resize re-pointed everything except the icons.
- **The camera feed went blank after closing an overlay** - tapping the navbar button for the panel you are already on restored it visible but deactivated, so the stream never restarted until you bounced to another panel and back.
- **Eight printers rendered as the wrong machine** - 20 database entries named an image that has never existed in the tree, and the failure was silent: an Ender-3 S1, a Neptune 4 Pro and a FLSUN delta all quietly drew a generic CoreXY frame. Eight now resolve to a real image; the remaining eight are listed in a gate that can only shrink.
- **The printer image was soft on large displays** - the widget always enlarged a 300px prerendered tier, up to about 2.2x on a 1024x600 panel, while the full-size source sat unused beside it. It now renders from the source when the request exceeds the tier.
- **Gaps between icons and labels collapsed to zero** on the controls panel and the unified temperature card - two shared styles referenced spacing constants that are not defined yet at the point the file is parsed, so they registered empty.
- **The Snapmaker U1 package shipped no prerendered images** - it was the only one of the ten release targets missing the copy step, so the device decoded 23.5 MB of PNG at runtime for images it had prerendered copies of.
- **The no-thumbnail placeholder failed to load on firmware**, leaving an empty widget.

### Changed

- **Downloads are 8.1 MB (21%) smaller** - every PNG losslessly recompressed and verified pixel-identical against the original. Over half the saving is two files that had been written with compression effectively off.
- **A newer config is left alone** - running an older build after a newer one no longer stamps the config version down and makes the newer build re-run migrations it has already applied.
- **INSTALL.md and TROUBLESHOOTING.md state the glibc floor for the pi and pi32 packages (#1259)** and point at the statically linked cc1 package as the fallback, after a reporter on Debian Buster hit a bare "GLIBC_2.29 not found" with no documented minimum to check against.
- **Documented the AMS bypass-controls override and CFS external spool.**

### Internal

- The ESP32-S3 firmware compiles without RTTI. `dynamic_cast`, `typeid` and `std::type_index` were replaced throughout the tree with process-local type tags and virtual capability queries, dropping 2,076 typeinfo-name symbols and 308 KB from the app image (97.4% -> 92.8% of the OTA slot). A lint gate now forbids the RTTI shapes on the desktop side, where they would still build and silently block the next file pulled into the firmware slice.
- The ESP32 CI build had been red since the port got real, and none of it was the firmware: ESP-IDF v5.5.4 emits exception unwind tables about five times larger than neighbouring releases, and that +1.66 MB was the whole overflow. Pinned to v5.5.5.
- ESP32 robustness: no longer bootloops on a degraded touch ribbon, guards the websocket handle against the timer-task reconnect race, bounds every wait that could outlive the task watchdog, marshals provisioning WiFi calls onto the LVGL thread, and wires up notifications, toasts, emergency stop and the real Moonraker control surface, which had all been abort-if-called stubs. A committed Kconfig default pointing at a real machine on a private LAN was scrubbed.
- The release pipeline: the Pi dual-link ran two whole-program `ld` processes at once with nothing ordering them, which is what the runner OOM-killed on v0.99.110; per-SHA CI ccaches had pushed the repo past GitHub's 10 GB budget and evicted seven of the nine release caches, which is why the v0.99.111 Pi build ran cold; and every toolchain download was a bare `wget` with no retry, which is how a valid zlib URL failed a build 52 ms in. All three fixed. Release channels are now declared per-branch in a file rather than derived from the tag, so devel builds can use plain monotonic versions the updater can actually compare.
- Telemetry records whether Moonraker is local (the verdict only, never the host) and finally surfaces `helix_macros` adoption, which has been collected since schema v2 and was never projected out of storage.
- `helix-screen ctl` gained `long_press`, so gesture-entered screens - edit mode, and the widget catalog behind it - are reachable from automation for the first time. The documented press/sleep/release recipe never worked.
- The temperature graph's tooltip indexed its metadata by slot while resolving by handle, so a remove-then-add cycle would caption the wrong series and eventually read out of bounds.
- The mock client never recorded its connection URL, so everything asking which host we are talking to read it as no connection - the plugin install modal offered the remote path against a local Moonraker.

## [0.99.111] - 2026-08-12

**Upgrading from 0.99.108?** This is your next release. Neither 0.99.109 nor 0.99.110
produced downloadable builds: 109 died on a compile error in its own release commit, and
110's Raspberry Pi build was killed by the runner's OOM killer during the link, which
blocked the release from publishing. Everything both would have delivered is included
below, plus the fixes found while tracking those two failures down.

### Added

- **Native ESP32-S3 firmware target - ALPHA, not for daily use (K-Touch and similar panels)** - a first-booting, highly experimental port: HelixScreen now builds and runs as firmware on ESP32-S3 display hardware (e.g. the BTT K-Touch), not only as a Linux/SDL app. A packed, compressed asset container fits all nine languages and every printer image into a 2 MB storage partition; RLE medium-tier fonts fit the 6.875 MB OTA slot; and a real `EspMoonrakerClient` over `esp_websocket_client` drives live discovery, status and bed-mesh callbacks over WiFi, with first-boot SoftAP captive-portal provisioning and A/B OTA. Ships as a v1 Core+AMS cut: home-only boot with heavy panels deferred to first navigation, draw buffers sized to internal DRAM, and a beam-paced presenter for tear-free scan-out. Camera/QR and the 2D-gcode and bed-mesh-3D renderers are compile-gated out. Expect rough edges, missing features and known instability - this is for developers and early testers only. The Linux, Pi and Android targets are unchanged and remain production-quality.
- **Home-screen edit mode can be disabled (#1245)** - a Touch & Input setting, defaulting on, so a resting finger on a wall-mounted tablet never rearranges widgets. The same global long-press time that gates edit mode is now configurable too.
- **Minimum toast severity (#1213)** - filter non-critical toasts down to a chosen level.
- **Reset Endless Spool from the device operations overlay** - a direct action for Happy Hare setups that need a lane-map reset. It asks first, and now tells you when it worked.
- **AMS says whether the printer will swap spools by itself** - a runout on a multi-lane backend tells you "switching to lane 4" rather than leaving you to discover it.
- **Temperature graph above its controls in portrait** - the graph stacks on top of the control strip on a tall screen.

### Fixed

- **The Android APK shipped the packager's own configuration** - a working `settings.json` with a real printer address, an active printer and `wizard_completed=true` was being packaged, so a fresh install skipped first-run setup and pointed itself at someone else's network. Crash dumps excluded in the previous release were still shipping too, because the packaging step only ever added files and never removed what an earlier build had left behind. Nothing needs a shipped seed: defaults are synthesized when the file is absent. No private data ever reached the git repository, only built APKs.
- **Android background network calls silently failed** - debug-bundle uploads and update checks resolved their Java bridge in a way that cannot see the app's own classes from a background thread, so both failed as "HTTP 0" with no diagnosis. A bodyless HTTP error also erased the real status code, making an expired key and a dead network look identical.
- **AD5X cold-ejected a seated filament the motion sensor mis-read as empty** - the IFS motion sensor reads false while a lane is loaded but idle, so gating ejection on it alone ejected filament that was happily loaded. The gate now requires the head-switch edge that only a real unload raises.
- **The AMS recovery modal went stale mid-error, and then came back after you dismissed it** - it now re-presents when the underlying fault genuinely changes, but a dialog you have answered stays answered. Previously any later cosmetic update to the same fault re-opened it, and after a recovery tap it put live buttons over an in-flight preheat.
- **Reset Endless Spool reported success without doing anything** - on a system that had not yet reported its slot count, the confirmation was accepted and nothing was cleared. It now refuses instead.
- **Closing an overlay could leave two panels on screen at once** - the restored panel was activated before it was made visible again, and by up to three different code paths, so a panel whose activation navigates elsewhere corrupted the panel stack. The double activation also tripped Print Last's retry counter and could start two first-run tours. This also fixes the camera feed staying dead until a tab switch (#1245).
- **The temperature graph on reconnect** - resuming drew a phantom spike bridging the whole gap, chip toggles reset themselves, and a Klipper restart collapsed a 20 minute graph to a few seconds. Grid lines and time labels now line up. The graph also rebuilds its series when discovery adds an extruder.
- **The G-code preview dropped its thumbnail too early on every non-GPU device**, leaving a grey gap while "Building preview" was still running.
- **Two Japanese characters rendered blank** in the new AD5X spool-switchover strings.
- **Debug bundles dropped the incident itself** from `klipper_log` and `log_tail`.
- **Android crash reports and update checks could silently do nothing** - the Java bridge they resolve was looked up in a way that cannot see the app's own classes from a background thread, so the call returned nothing and failed as "HTTP 0" with no diagnosis. All five call sites now share one lookup that works from any thread.
- **One bad value in settings.json could crash the app or refuse to save** - a setting stored with the wrong type (a string where a number or a yes/no belongs, usually from a hand-edit) threw instead of falling back to the default, and writing that setting afterwards failed outright. Bad values are now ignored with a log line naming the setting.
- **The print-status bar could disagree with the progress text** - one source and one formatter now feed the bar, progress text, filament, duration and layer strings.
- **The print-status thumbnail could apply during a mid-teardown relayout** - now guarded.
- **Portrait detection disagreed with the layout override (#1255)**.
- **The BETA badge overlapped row controls (#1244)** - a gutter is now reserved.
- **Android (#1245, #1253)** - reconnect WebSocket on display wake, keep user settings across APK upgrades, preload libturbojpeg on API 24+, and close the SDL audio device at idle.

### Changed

- **Splash version label moved to the upper-left**.
- **Temperatures drop the decimal at 100 °C and above**.
- **Context menus share one component and gained an explicit close button** - all eleven menus are now one card rather than eleven per-menu copies.

### Internal

- The ESP32 source manifest gate accepted lines the firmware build then silently discarded, so it could pass while the build broke. It now enforces exactly what the build consumes, and detects stale and duplicated entries.
- The home-screen edit-mode toggle and long-press slider shipped with no tests; deleting either check failed nothing. Both are now covered, along with the reconnect and endless-spool fixes above.
- Three tests were found asserting the wrong thing or passing only by shard ordering: one encoded the endless-spool bug above, one had been reduced to a tautology that no longer exercised the runout confirm delay, and two depended on state a co-tenant test happened to leave behind. All corrected. No user impact; disclosed for the record.
- The release pipeline could not build Raspberry Pi at all. The link peak had grown past the runner's memory, so it was OOM-killed; separately, the job that warms the compiler cache was sized for a warm build and so could never complete the cold one it always runs, leaving the cache permanently missing. Both are fixed, and the platform build now degrades to a slow build rather than a failed release when the cache is absent.

## [0.99.110] - 2026-08-11 [WITHDRAWN]

No downloadable build was ever produced: the Raspberry Pi link was OOM-killed on every
attempt, which blocked the publish steps. Its entire contents ship in 0.99.111 above.

## [0.99.109] - 2026-08-10 [WITHDRAWN]

No downloadable build was ever produced: a compile error in the release commit broke all nine
device cross-builds and macOS. Its entire contents ship in 0.99.111 above.

## [0.99.108] - 2026-08-09

Filament systems get the bulk of the attention: CFS and AD5X IFS learn to report a runout
instead of staying silent, the community K2 Box fork gains the commands to go with the schema
0.99.107 taught us to read, Load asks before it homes into a part left on the bed, and the
print-active gate that seven backends already enforced now covers the eighth. Android gets a
batch of fixes that make it behave like a real target rather than a port, including a panel
that can finally power off. The home widgets stop deciding their layout from grid spans and
read physical pixels instead, so the same widget reads correctly on every panel and
orientation. Diagnostics got a lot of work too: a debug bundle now reaches past the incident
that prompted it rather than spending its whole budget on Klipper's per-second stats line.

### Added

- **CFS reports filament runouts (#1250)** - the box always pauses before attempting a refill and announces the failure in plain text, so that is what the backend reads now. The old signal was `filament_useup`, a latch the box sets when a spool runs out and clears only on a successful extrude, which meant the runout indicator could light on any unrelated pause. It now requires a false-to-true transition witnessed inside a running job. The runout-modal suppression also narrows from all hub topologies to the backends that actually raise their own fault, which un-silences ACE and QIDI.
- **AD5X IFS reports filament runouts (#1250)** - a head-switch edge, armed only while no operation is in flight, confirmed by a dwell and gated on the print being paused. The gate deliberately does not key off the head filament flag, which the motion sensor also writes and which reads false while a lane is loaded but idle. Recovery offers resume, a bare extruder purge and `IFS_UNLOCK`, but no load button, because every AD5X load path homes and `_G28` on a loadcell-Z machine probes into the part.
- **AD5X says which auto-switchover plugin it found** - so a runout can state plainly that unattended switching needs lessWaste or bambufy, rather than leaving the user to work out why nothing switched.
- **Filament commands for the community K2 Box firmware** - 0.99.107 taught the CFS backend to read the reimplemented box module's schema and identify its dialect, which stopped a populated 4-bay unit rendering as an empty panel, but every operation still went nowhere. The identified Fork dialect now routes through its own load, swap, unload, clear and slot-metadata commands, with the stock K1/K2 envelopes unchanged and cleanup skipped for state the fork never saves.
- **Android can power the panel off at the sleep timeout (#1245)** - SDL asserts `FLAG_KEEP_SCREEN_ON` at video init and nothing ever cleared it, so the device could not sleep while HelixScreen was in the foreground, and our own Display Sleep just painted a black rectangle over a fully lit panel. It now releases keep-screen-on and lets the OS timeout run. Display Sleep set to Never never releases it, so a wall-mounted tablet stays awake.
- **A confirmation before HelixScreen injects a G28 you did not ask for** - `idle_timeout` runs M84 after ten minutes and motor-off clears homing state, so "unhomed with a part still on the bed" is the normal state ten minutes after any print. Tapping Load homed into the part with no warning.
- **The home question is asked before the preheat, not after** - it previously surfaced roughly 140 seconds in, once the nozzle was already at temperature. Declining now costs nothing and heats nothing.
- **An upgrade advisory when AFC predates the v1.2.0 lane fields** - detected by capability rather than version, because AFC has no trustworthy version signal: `AFC_VERSION` is hand-bumped and sat at 1.1.37 through the whole v1.2.0 release, and a live BoxTurtle reported 1.0.0 while running v1.1.0. The advisory fires once, is persisted, and re-arms on downgrade.
- **Bypass can be enabled on firmware that reports none** - Happy Hare defaults `has_bypass` to 0 for `mmu_vendor Other`, which is what a Qidi Box reports, so owners who do feed filament straight to the extruder got no bypass UI at all. Safe to offer because Happy Hare's own `select_bypass()` never consults `has_bypass()`. The row shows only while the firmware says no, and stays visible once enabled so it can be switched back off.
- **AMS errors show the suggestion that goes with them** - every backend populated a suggestion field and nothing ever displayed it, because thirty-odd call sites each open-coded their own error toast. Users read "Cannot run filament operation while printing" and never the half that unsticks them. Toasts gain an optional second line for it, and the verb prefixes survive only where the backend's own message names no operation, so nobody reads "Unload failed: Failed to unload filament".
- **A debug bundle can answer "is Moonraker even running" without asking Moonraker** - every Moonraker-derived section is fetched through Moonraker, so a bundle uploaded while it is down carried five "No response" entries and none of the logs that would explain why, and could not distinguish nothing-listening-on-7125 from listening-on-the-LAN-address-only, which need opposite advice. A new section reads `/proc` instead, listing sockets on the configured port with their bind addresses and any moonraker or klippy process.
- **Both log collectors fall back to local disk** - with the paths derived from the daemons' own argv rather than a guessed list of per-platform data roots, so an empty result is an honest answer where fiction would read as knowledge.
- **An explicit Done button on the print-status configure picker** - it could previously only be dismissed by tapping the backdrop, with nothing on screen saying so.
- **Timestamps in launcher.log** - on platforms where `/var/log` is tmpfs, the launcher log is the only record surviving a reboot, and its lines could not be correlated to anything.
- **`HELIX_REMOTE_CONTROL=1` on a deployed device** - maps to `--remote` so a printer in the field can be driven with `helix-screen ctl`. Off by default, since the socket can drive the whole UI.

### Fixed

- **A failed AFC toolchange took away the Eject button** - AFC pins `current_loading` at the top of TOOL_LOAD and clears it only on the success path, so a failed toolchange latches the lane name until the next successful one, which a user with a jammed lane cannot perform. The unload-mode ranking put recovery above Eject, so the failure that makes someone want to eject was the very thing that removed it, swapping it for a lane reset that retracts toward the hub and never returns filament to the spool. Reachable on a Box Turtle, the dominant AFC setup, with no unusual configuration.
- **Filament operations were refused while paused** - pause-then-swap is the runout recovery workflow, and HelixScreen was the only surface that could not perform the recovery Klipper had just asked for. Only AD5X IFS still refuses, because its remove-current macro hides a `_G28` that probes a loadcell-Z nozzle into the part.
- **AFC Eject reported success and moved no filament** - it sent LANE_UNLOAD for three conditions upstream discards, including one that logs nothing at all, and Klipper acks the gcode either way. The affordance now greys out mid-print instead of being offered into a certain refusal.
- **Load and Unload gating was three partial copies** - the filament panel, the AMS sidebar and the context menu each carried their own rules. One shared rule now answers for all three.
- **Two filament operations could race through the busy check** - the gate read a busy flag and then dispatched, but no backend makes itself busy inside that read: two set it in a later critical section, three inside the hook, and three never set it at all and wait for firmware to echo. It is now a claim taken in the same critical section as the read and held across the dispatch.
- **Snapmaker U1 accepted filament operations mid-print** - it was the only subscription backend with no print-active gate. Its auto-feed homes and its tool select moves the carriage, neither of which the G28 matcher can see. Paused still passes, so runout recovery is unaffected.
- **Declining the pre-operation home wedged AD5X for five minutes** - the generic cancel path reset the action but left the phase tracker armed, so the very next extruder-temperature frame flipped it straight back to heating. The operation then latched a fabricated timeout error that refused every AMS command for the whole 300-second window.
- **Dismissing the home dialog by backdrop or ESC left the backend stuck busy** - permanently on Tool Changer, which has no stuck-action watchdog. Neither dismissal path ever reached the confirm or cancel callback.
- **A declined home left a stale operation label on AD5X** - the sidebar kept showing "Heating nozzle to 230°C" under an idle action until the next operation overwrote it.
- **Happy Hare accepted a bypass select with filament still loaded (#1229)** - the command is fire-and-forget, so it returns success before Klipper answers. HelixScreen reported success and changed nothing while Happy Hare replied with a bare "Operation not possible. Filament is loaded" toast contradicting it.
- **Nothing explained why Happy Hare hid the bypass controls** - the deciding field resolved at trace level and never reached a debug bundle, so 186KB of log from a Qidi Box reporter contained no trace of the decision. The bypass path node was also a one-shot decision taken before the first status frame, and the pre-status default flashed the controls up and then withdrew them.
- **A mid-boot read pinned a 4-lane BoxTurtle to one slot for the whole session** - AFC empties its lane namespace at the start of every PREP and writes each lane back as that lane's prep finishes, so it is empty and then partial for seconds on every boot. A cold boot landing in that window made lanes 2 through 4 permanently invisible. Klipper's object list is the lane-set authority now, and lane data only bootstraps an empty registry.
- **The AFC feature probe read a frame that can never carry the fields it looks for** - the standing subscription is field-scoped and lists none of them, so the probe reported legacy against a BoxTurtle confirmed on v1.2.0, and would have nagged an up-to-date printer to upgrade. An explicit unscoped query fetches the lane object instead.
- **AFC named the wrong lane on a toolchanger** - the guard asked whether any extruder held filament, and parked toolheads routinely grip filament, so it read true on essentially every real multi-tool machine.
- **A toolchanger parking its carriage wiped the slot Klipper had just elected** - the aggregate current-load field goes null on park, and that was being read as authoritative over the per-extruder lane state contradicting it.
- **A Spoolman-linked lane was named from its hex colour** - the loaded-filament card gated the Spoolman name on a field no AFC parse path ever writes, so a BoxTurtle lane rendered "Light Pink PLA" when the real name was already on the slot and never read. Brand now comes from Spoolman even though AFC cannot publish it.
- **The spool card and the loaded card disagreed about the same lane** - on AFC older than v1.2.0, which publishes a spool id but no filament name, a linked lane fell to the untracked "Brand · Material" branch and rendered "Elegoo · PLA" for a spool we could already name.
- **A filament name repeated its own material** - "Elegoo Black Rapid PLA+ PLA" and "eSUN Silk Blue ePLA Silk PLA", one word worse than what the old builder produced. Materials are a small closed vocabulary, so a name containing one really does make the suffix redundant. Brands keep stricter matching, because Sun must never be stripped out of Sunlu.
- **The picked catalog product did not survive reopening the slot editor** - only type and brand were saved, so SUNLU "PLA+ 2.0" round-tripped as brand plus material. Reopening reseeded from vendor and material alone and took whatever sorted first alphabetically, so the lane came back relabelled. Reported on AD5X, but the field was missing from every backend.
- **The nozzle badge stamped a meaningless "0" on every AMS printer** - it was driven by tool count, which expands to one entry per filament slot, so a mock AD5M reported four tools and one hotend. It now counts distinct extruders, so the badge appears only where more than one nozzle needs naming.
- **The tool badge covered the nozzle icon it sits on** - an 18px badge over a 24px glyph, hiding 56% of it with the label clipped. A disc cannot be smaller than the font inside it, so the badge takes its own size and font and shows the tool index rather than the full name.
- **A tool change offered mid-print silently did nothing** - the widget asked for confirmation and dispatched into backends that refuse while printing, so the user consented and the pill did not move. Neither call site passed an error handler either, so any refusal that did arrive vanished entirely.
- **The AMS context menu rendered off the top of the screen** - 313px of card on a 480x272 panel and 520px on 800x480, so the header and the entire Load row were unreachable at every tap point, and Load is the primary action. Two bugs fed it: a content-width card that wasted 73% of a landscape screen, and a clamp that fixed the bottom edge before the top and drove the card to y=-51.
- **A tap on the empty half of a menu row dismissed the menu** - a content-sized card is as wide as its longest row, but every shorter row kept its natural width, so 77px of the "Load" row was backdrop.
- **The MJPEG camera stream stayed dead until you switched tabs (#1245)** - the worker clears its running flag only in `stop()`, but it also exits on its own when the failure budget runs out or on a bad allocation, so the widget believed a dead stream was still live and skipped the restart.
- **Android background and resume skipped the panel lifecycle (#1245)** - coming back to the foreground repainted the screen, which only redraws what the widgets already hold. Nothing re-seeded subjects, re-bound observers, reloaded content or restarted timers, which is why switching tabs away and back un-stuck a stale print-status panel and a resume did not. Backgrounding now suspends symmetrically, so timers and streams stop instead of running against a frozen LVGL thread.
- **A touch during the wake blackout opened edit mode behind the lock screen (#1245)** - disabling the input device is a flag write, so a finger still on the glass kept counting toward a long press from the original touch-down. On Android the wake press reaches the home grid directly, so edit mode opened behind the lock screen and was found once the PIN cleared.
- **Touch recalibration could not be escaped (#943)** - a Q2 whose digitizer over-reports its range compresses every tap into a top-left rectangle, which put Accept, Retry and Cancel out of reach and left the screen cycling to timeout forever with the Back button covered by the capture surface. Verify now installs the just-captured matrix rather than the stored one that is the only reason anyone opens the screen, a press-and-hold aborts from any coordinate a controller can emit, and the unattended loop is bounded.
- **A stock Qidi Q2 detected as an Artillery M1 Pro** - the database credited a shared `probe_air` object to two siblings as if it were model-unique, so both outscored the Q2's own best signal: 100% confidence on one firmware and a Qidi Max 4 at 97% on the next. All three are demoted to family-level confidence.
- **The WiFi toggle froze the UI for tens of seconds** - it ran the radio change synchronously from the LVGL event callback, and that path issues two control commands each able to spend 5s retrying and 10s in select, then did a synchronous config save on the same thread.
- **Switching WiFi off snapped the switch straight back on** - NetworkManager and macOS never implemented the radio change at all, so they inherited a silent no-op success plus a read that always returned enabled.
- **A forget was reported as verified against a config we never read** - the read returns empty both when the network is absent and when the file cannot be opened. The AD5X vendor config is unreadable, so one session logged both "Removal verified on disk" and "did not record this network" about the same path, and the forgotten network was associated again after the next reboot.
- **"WiFi scan failed. Try again." 48ms after the user's own tap** - forgetting the connected network disassociates and immediately rescans, and a busy response to that scan was being read as a failure.
- **A connection-failed prompt hijacked WiFi setup** - the escalation is latched and fires 60 seconds in, which on an unreachable printer is exactly when the user is in Settings fixing it. On an AD5X it landed on top of an open password modal and the passphrase was retyped from scratch.
- **The connection-failed copy was wrong for a printer running HelixScreen itself** - it told someone holding the screen to check that the printer is powered on and that 127.0.0.1 is correct, with Change Address as the primary button.
- **The updater was hidden on the most common Pi install there is** - self-update was gated purely on write access to the install root's parent, so the default layout of a root-owned tree with an unprivileged service user reported it as physically impossible. The installer escalates with sudo for exactly that swap, so those installs update fine.
- **Uninstall left entries behind in moonraker.conf** - removal deleted two comment lines by literal text while the generator writes five, so three orphaned on every install and uninstall cycle, and on one CC1 a line ended up in the file twice. The block is matched structurally now, so it cannot drift out of sync with the generator again.
- **Uninstall never removed HelixScreen from moonraker.asvc** - it stayed in the service allowlist forever.
- **The installer's diagnostic hints did not work for the user running them** - a service user outside the journal groups gets nothing on stdout, so a reporter chasing a touch-calibration bug sent back a zero-byte capture and lost a round trip to it (#943).
- **`make deploy-pi` wiped per-slot filament identity** - brand, spool name, Spoolman ids and weights, for every lane the user had edited. The overrides file was not in the rsync exclude list, though its sibling already was.
- **Spoolman config was written to a file the firmware replaces on upgrade** - the index answering "which file proves the config root is addressable" was reused as the write target, which only holds while the root config happens to define `[server]`. COSMOS 26.07.0 splits the two, so both the setting and its include line landed somewhere an update discards.
- **MacroManager could never upload on a real printer** - an empty destination path means the root of the given root, which is what Moonraker expects, but the path guard refused it outright, so install and update failed validation before any request was built.
- **An upload filename was never validated** - despite reaching the multipart form verbatim, so a traversal path was accepted.
- **Power-loss recovery could not resume a file with a space in its name** - Klipper tokenizes extended parameters with shlex, so an unquoted filename split into tokens with no equals sign and the whole command was rejected as malformed. The stored path was also absolute, which the resume branch cannot match against a list relative to the virtual SD root, so it silently discarded the recovery snapshot.
- **A bare G28 stopped homing on COSMOS 26.07.0** - the CC1's homing_override branches on rawparams, and Klipper does not strip comments from rawparams, so the trailing provenance comment HelixScreen appended to every command made it truthy and routed G28 down the explicit-axis branch where no axis letter matched. Nothing homed, after the override had already done its Z hop.
- **The CC1 start path spent its whole timeout budget doing nothing** - the stock COSMOS resonance macro restarts the UI through a shell command with a 5s timeout, and a command killed on timeout takes its whole process group with it, including the supervisor just forked. About 3s of that went on a flat sleep and a settle that kills nothing on a platform where HelixScreen is the configured UI.
- **A cut-off stop stranded the user on a cleared framebuffer** - the stop can be interrupted with the daemon still exiting, which start read as already-running and no-opped.
- **Stopping the UI took six seconds and ended in SIGKILL** - the pidfile holds the launcher shell, which spends its whole life blocked in a foreground child, and a shell will not run its TERM trap while that child is alive. Measured on a Centauri Carbon it ignored SIGTERM for 12 seconds and counting. Signalling the watchdog first halves the stop and makes it clean, so buffered log output is flushed rather than lost.
- **The watchdog gave up in fifteen seconds on a transient exec failure** - `execv` of an intact binary can return ENOEXEC under memory pressure, seen on a 128MB Centauri Carbon where the UI restarts one second after the input-shaper FFT. Every exec failure was reported as a flat exit 127, so six in a minute spent the budget and left a black screen until the user rebooted.
- **A spurious crash-loop refusal to boot on the CC1** - the crash marker was cleared only in the normal shutdown path, which the SIGTERM handler skips, and COSMOS stops and starts the UI by pidfile around resonance calibration.
- **Klipper config was not found on COSMOS, which has no printer_data directory** - so the CC1 silently skipped update_manager registration, the config symlink and the Klipper include, with every consumer deriving the path the same wrong way.
- **The ZMOD app log was not in the archive users attach** - it went to a directory `TAR_CONFIG` captures on neither AD5M nor AD5X, so reporters sent back launcher stderr and nothing else.
- **An unopenable log path aborted startup** - instead of falling back to the platform's own sink.
- **Every `ctl` shutdown corrupted the heap** - five out of five runs ended in SIGSEGV or SIGABRT with ASAN reporting use-after-free. Two ordering faults: the theme subjects were torn down before the XML scopes holding raw observers on them, and the XML styles were freed before `lv_deinit()`, whose own teardown runs layout passes over widgets pointing at those styles.
- **Four other teardown paths kept the old order** - including the no-input-device path, which shows a fatal error screen first and therefore has a live widget tree when it tears down.
- **A subject name could outlive its subject** - the registries kept resolving a name to storage that was already freed, so the next binding that named it read dead memory.
- **Observers outlived the objects that owned their subjects** - seventy-seven observer sites plus the print-status panel's twenty-five were registered with no lifetime token, so a teardown cycle freed the observer nodes and the next reset dereferenced freed memory.
- **A re-entered touch calibration stranded its previous timer** - still in LVGL's list, still holding a pointer to the panel, and the destructor could only free the newest handle.
- **The navigation backdrop could outlive the screen that owns it** - it is a child of that screen, so any teardown deleting the screen freed it without going through the path that clears the pointer.
- **Work queued during shutdown was spliced back in and run against destroyed objects** - the shutdown drained the pending queue but left the frozen buffer untouched, and a freeze is held across exactly that window.
- **Two heap over-reads found by AddressSanitizer** - a four-byte magic logged as a C string, where the formatter resolves the length before applying the precision, reachable from any malformed file with debug logging on.
- **The crash-report and temperature-graph dialogs could grow past the screen (#1204)** - both roots were content-sized with only an inner flat cap bounding them, which the mock's short crash text and near-empty sensor list never reached. A real backtrace and a many-sensor printer are what get there.
- **The debug bundle dialog rendered off-screen (#1204)** - 479px tall at y=-103 on a 272px panel, with both the header and the button row cut off, because its content area was capped at a flat 400px.
- **PIN entry and the hidden-network form overflowed 480x272 (#1204)** - the keypad's bottom row sat off the screen with 0 and confirm untappable, and the hidden-network form set a minimum height taller than the whole panel.
- **Five dialogs hardcoded an outer width** - none has any intrinsic content width, so the number was arbitrary and on a 480px panel a 400px dialog left 40px of backdrop per side.
- **The timelapse empty state invented 110px of scroll on the smallest screens** - a flat 380px minimum height for one icon and two lines.
- **The last page of a scrolling view overshot the content end** - a fixed page delta handed to LVGL's unbounded scroll entry point parked a sliver of content up top with dead space filling the rest. Presses also accumulated without limit, because the disabled state does not suppress clicks on the dimmed chevron.
- **Runout state sat beside the AMS lane indicator instead of on it** - so an AMS printer drew two filament glyphs, and the one next to the lane number was a muted icon saying nothing about that lane.
- **The fan and thermistor home widgets used a 16px icon where every sibling uses 48** - measured in a real cell, the fan spent 42 of its 113 vertical pixels next to neighbours using 70. The thermistor also gets a tighter row gap, since a seven-glyph reading plus the icon consumed the whole cell width.
- **Humidity and width-sensor icons never resized at all** - both indicators were instantiated with no name, so the engine's auto-naming overwrote the declared one and every lookup returned null.
- **Fan stack icons never resized either** - the restyle looked up a child of the icon object, which is itself a label with no children, so the lookup was always null.
- **A plain 1x1 widget was treated as if it had gained a second row** - a single grid row on the two largest tiers already clears the tall threshold by itself, which flipped the camera widget out of compact mode and started an MJPEG stream nobody asked for.
- **The size constants were each one pixel below the smallest qualifying extent** - despite being compared with `>=`, so on the largest tier a plain single-column widget read as wide.
- **The camera stream dropped on genuinely wide cells** - requiring both axes to clear their floors silently killed it on every multi-column cell whose row happens to be short, the same shape of bug the both-axes rule was meant to fix, just inverted.
- **A freshly built nozzle row disagreed with the rows beside it** - it picked its label from the last grid span seen rather than the pixel decision the rest of the widget already uses, and a real touchscreen never gets a second resize to correct it.
- **The tool switcher baked a row count its real cell could not support** - the widget measures itself before the panel grid is active, so the measurement resolved against the outer panel's content box rather than its eventual cell, and nothing re-ran it afterwards.
- **Grid edit mode computed its geometry against the wrong track count** - the live container sizes its row axis from rows actually in use, not the breakpoint table, so a page whose widgets occupy only the first row was off by whole tracks rather than sub-pixels.
- **Grid cell conversion ignored the gutters between tracks** - and the dots overlay landed a full gutter past the right and bottom edges, clipping the last row and column of dots off it.
- **A debug bundle reached ten minutes back and spent 97% of it on load averages** - Klipper writes an 850-byte stats line every second. On a real AD5X log the dominant noise was a four-line toolhead dump repeated 7916 times, which is not a stats line and sailed straight through the first pass, leaving a payload with less history than the unfiltered tail it replaced. Repeating shapes are collapsed now, so an 11.7MB crash log condenses to 1344 lines carrying the MCU shutdown and all 22 tool changes.
- **A misleading clock-step annotation on every line of a bundle** - the ring buffer stores raw messages and formats them at dump time, so the monotonic column read the dump instant and one bundle shipped 97 clock-step annotations summing to the entire session.
- **The debug ring was one fixed size picked for the smallest board** - it is the only place a bundle can recover DEBUG context on a device whose persistent sinks run at WARN, and one AD5X bundle covered 13.5 minutes and arrived with nothing about the shutdown under investigation. It now scales with device RAM, floored at the old value so nothing regresses.
- **Warnings were lost when the UI was SIGKILLed** - which is routine, by an init script escalating to `kill -9` or by a firmware helper freeing RAM. One CC1 left its log frozen for two weeks and truncated mid-line, so a black-screen incident had no app-level evidence at all.
- **A pending-count debug line filled most of a bundle's ring** - it re-fired on every count decrement, so one healthy poll burst emitted a line per in-flight request and a Spoolman printer spent 1275 of 2000 lines on it, capping its reach at about 3.5 hours of nothing.
- **The test-mode banner named a config file the run never touches** - it printed the default path verbatim, so a run with `HELIX_CONFIG_DIR` set sent you editing the wrong file.
- **A modal opened four LVGL warnings and resolved to nothing** - the exclude-object and cancel dialogs passed callback names that were never registered.
- **The Performance overlay was filed as an unregistered push** - which made a genuine missing registration elsewhere indistinguishable from this benign one in panel telemetry and the crash breadcrumb ring.
- **The XML formatter deleted multi-line comments above the root element** - sixteen of them, one per file, all state tables and rationale notes. They are restored here, and both directions verified.
- **The color picker ignored its own responsive size ladder** - constants are scope-local and were only ever registered into one scope, so the copy embedded in the AMS edit overlay fell back to a fixed 128px. A first-write-wins registration separately discarded the 24/28/32 swatch ladder so every screen got 32, and a hex placeholder starting with `#` was parsed as a constant reference and dropped at all three sites.
- **A stale thumbnail path disarmed active-print recovery** - and the detail view ignored the source timestamp on its own fetch path, so a cache entry could outlive the file it came from.
- **A submodule patch that failed to apply still stamped the tree as patched** - the red line scrolled past once and every later build reported nothing to do. That is how the #1212 null-hloop guard went missing from the tree, and the regression test written to catch it segfaulted instead of failing.
- **The repair path could not repair one of the two submodules** - the reset looped over only one list, so a tree carrying an older revision of a libhv patch was told to run a command that could never fix it.
- **The nightly build was killed at the link on a cold ccache** - it ran the whole build and test compile in one step capped below what the two-step Build workflow already budgets, and trimmed the cache to 500M right after restoring 1.8GB into it.
- **A failed cross-build reported success** - the ssh build was joined to the timing lines with `&&`, so a failure short-circuited past them and exited with the status of an echo. Make then printed a green banner with a negative duration and rsynced whatever stale binaries sat in the fetch directory, in one case a two-month-old build.

### Changed

- **Home widgets pick their layout from physical pixels, not grid spans** - print status, camera, the tool switcher, the job queue, active spool, print stats, the clock, the fan stack, favorite macro, nozzle temps and the temperature stack all decided their layout from colspan and rowspan, which reads differently on every panel and orientation. They now threshold the pixel width and height they were already being handed. The camera migration was the risky one, since that edge starts and stops an MJPEG stream.
- **Widgets declare whether they can occupy half a grid cell** - groundwork for half-cell tracks. The small single-action widgets can; anything rendering a chart, image, list or video frame needs a whole cell.
- **G-code is sent verbatim** - the trailing provenance comment is gone, along with the capability flag and enum that existed only to suppress it. It had already broken M117 and M118 payloads and the AD5X quoted RESPOND echo before it broke homing on COSMOS.
- **The AD5X slot poll backs off to 30 seconds during a print** - measured on a real session, 3902 downloads produced 3 content changes, two of them the first poll after boot establishing a baseline, and each fetch is a loopback HTTP GET on a two-core board that is simultaneously feeding the MCU step queue. Paused keeps the 5 second cadence, since a pause is when spools actually get relabelled, and the transition to done forces an off-cadence poll.
- **Thumbnail fetches go through one guarded path** - results are always delivered on the main thread now, where previously which thread they arrived on depended on which internal path produced them, and callers set image sources from them.
- **Eight locales are complete again and the CJK glyph set is refreshed** - covering the strings added since 0.99.107 closed the previous gap, with twelve obsolete parameterized error strings dropped.
- **The i18n sync tools stopped reporting edits they never made** - without ruamel installed there are no line numbers to splice against, so `obsolete --action delete` printed "Deleted 36 key instances" and left all nine files untouched. Mutating operations now refuse to run rather than lie. The extractor also stopped treating an XML constant reference as translatable text.
- **`--real-ams` selects a real AMS backend under `--test`** - the flag and its predicate already existed and were unit-tested, but nothing ever set it from argv. The mock client now simulates the status frame Happy Hare needs, which it could otherwise never receive.

## [0.99.107] - 2026-08-07

Portrait screens get the rest of the treatment 0.99.106 started: print status, print tune,
motion, bed mesh and the advanced panel each get an arrangement of their own rather than
landscape's row squeezed narrower, and overlays now come down from the top, above the bottom
navigation bar. Alongside that, WiFi gains a radio that actually turns off, a credential store
of its own and a forget-network path; filament Load and Unload stop diverging across the four
screens that can dispatch them; and the last 33 untranslated strings close out all eight
locales.

### Added

- **The WiFi radio can be turned off for real** — the toggle previously detached from wpa_supplicant and left the radio powered and associating. It now drives rfkill, verifies the write, fails the toggle if the write fails, and remembers the choice across restarts. A live radio-off on a device whose only network path *is* the radio routes through a confirmation dialog first, since nothing else could turn it back on.
- **Forget network** — saved networks could be joined but never removed. Removal now mirrors to persistent storage rather than lasting until the next restart.
- **HelixScreen keeps its own WiFi credential store** — for the devices whose vendor wpa_supplicant config will not accept new credentials at all, which previously meant a network you joined was never saved.
- **All eight locales are complete** — every non-English locale sat at 98.7%: three keys were absent entirely (the only ones the build warned about) and thirty were present but empty, which falls back to English with no warning at all. Most were AFC filament-path and fault strings, plus the WiFi forget flow, the hardware-detect prompt, and the power-loss recovery messages. ja and zh introduce no new CJK characters, so the embedded fonts are unchanged.
- **Power-loss recovery on Creality's Klipper fork (K1, K1C, K1 Max, K2 Plus, V3, Hi, i7)** — recovery is exposed through a webhook Moonraker auto-registers, so it was always reachable from a third-party UI; only one undocumented parameter was missing. Mechanism verified against a physical K1C and K2 Plus. Resume is refused unless the detection probe confirmed, because that probe is also what arms the flag the stock sensorless-homing macro reads to choose a full Z clearance lift — without it, homing would drag the nozzle through a tall part.
- **Screws tilt results can be shared by QR (#1226)** — every screw's name, probed Z and adjustment string, plus a QR encoding the same values as plain text. A printer has no OS clipboard, so the QR is how the numbers reach a phone.
- **CFS support for the community Kalico firmware on K2 Plus** — the reimplemented box module shares zero keys with Creality's stock schema, so a fully populated 4-bay CFS rendered as an empty AMS panel. Schema and macro dialect are now detected independently, each read from the module's own registration table; a flat box whose module is unrecognized refuses control rather than emitting stock sequences it may not define.
- **Heater icons carry the same thermal colour as the number beside them** — muted off, red heating, green at temp, blue cooling, across print status, the temperature card, the preheat tiles and the temperature-graph overlay. Previously an icon pulsed red while its own number sat blue. In Maintaining mode the chamber icon now follows the chamber's cooling-ceiling rule the way its label always has.
- **Print status shows the heater icon and its label together** — they were exact breakpoint complements, so the card showed one or the other and never both.
- **An Always Show Bypass Spool toggle for AFC (#1229)** — for anyone who wants the bypass node visible on a machine where it is now hidden by default.
- **Portrait overlays slide down from the top** — anchored above the navigation bar that runs along the bottom in portrait, and full width rather than leaving an 8px sliver of backdrop down each edge.

### Fixed

- **A soft rfkill block locked the radio out permanently** — startup treated any rfkill block as a fatal preflight failure and bailed before the code that can clear a block ever ran, so a block set by HelixScreen's own radio-off became a self-inflicted lockout that survived a reboot. This stranded a real WiFi-only printer for days during this work's own hardware testing. A soft block is now a warning and startup proceeds; a hard block (a physical switch we genuinely cannot clear) stays fatal. A stale block found at startup is cleared outright rather than waiting for someone to walk over and toggle the switch.
- **Typed characters reached the debug bundle** — the on-screen keyboard logged each pressed character at debug level, and a bundle ships the log ring verbatim with a debug floor regardless of configured verbosity, so a passphrase typed into a masked WiFi field left the device inside bundles shared in public channels. Keystrokes are redacted whenever the field is masked and demoted to trace, which also stops them evicting the diagnostic window — one bundle spent 1776 of its 2000 ring lines on this one handler, covering 3m50s.
- **WiFi connect-failure toasts leaked the network name** — the first-run wizard passed raw SSIDs into an error notification, which reaches the log under the hood. The PII gate that catches this everywhere else never looked inside notification macros.
- **A non-UTF-8 SSID grew the credential store without bound** — an SSID is a raw 802.11 octet string with no UTF-8 guarantee, and on every real ARM target such bytes passed validation, were mangled on write, and then never matched the raw SSID on the next connect. Deduplication silently stopped working and a fresh cleartext record was appended on every single connect to that network, forever. Stored values now round-trip losslessly.
- **A saved network stacked a duplicate entry rather than reusing its own.**
- **An IP address was shown while not actually associated with a network.**
- **A failed WiFi scan trigger corrupted the scan suppression state.**
- **The WiFi interface, its wpa_supplicant daemon and its rfkill switch were three independent guesses** — resolved once at init into one identity, with every daemon found logged.
- **A failed radio change fed a false state back into the UI** — the result of the change was discarded and the *requested* on/off state persisted regardless of whether it applied, so a denied rfkill write left the toggle lying about the radio and the next startup reasserting the wrong thing.
- **Load and Unload behaved differently on each of the four screens that can dispatch them** — the filament panel has always routed AMS backend → configured macro → raw G-code, but the AMS sidebar returned silently with no backend and both runout paths navigated away instead, so that ladder reached exactly one surface. All four now share one plan and one router. The sidebar's already-mounted toolchanger guard and its load-versus-swap rule reach the panel for the first time, and the unload-before-load question is answered per slot, so a mixed unit answers differently for a direct lane than a hub-routed one.
- **A toolchanger Load spun for 120 seconds and refused every operation after it** — selecting the tool already mounted is a firmware no-op that never touches the toolchanger's status, so nothing ever cleared the optimistic in-progress state and the panel stayed busy until restart.
- **Toolhead badges named the wrong toolhead on AFC (#1229)** — every T badge was an AFC per-lane map alias, which users read as a tool number; on the reporter's toolchanger AFC maps T0 to the extruder5 lane while Klipper's own T0 is extruder. T now means lane alias only, and toolheads carry Klipper extruder identity as E<n>.
- **AFC drew a bypass spool on machines with no bypass (#1229)** — AFC publishes a virtual bypass sensor whether or not one is wired, so the node was drawn permanently and painted from the external-spool slot: the reporter saw a green ASA bypass spool on a machine that has no bypass at all. Other backends are untouched, where bypass is a real physical position.
- **AFC's current slot froze on whichever writer got there first (#1229)** — every other writer was gated behind "no slot yet", so on a toolchanger a parked toolhead lane was named current with an empty carriage, driving a wrong header, a phantom node in the filament path render, and an unload nobody asked for. It is now derived from carriage mount state at the end of a status frame instead of negotiated across eight parser sites.
- **One unparsed lane tipped a pure-hub AFC unit into mixed routing (#1229)** — Moonraker sends deltas and unit objects sort ahead of per-lane ones, so a frame can describe a unit whose lanes have not been seen yet, and unknown routing counted as direct. Users saw seven toolhead nodes for six extruders.
- **The bypass toggle unloaded the active slot first on AFC and Happy Hare (#1229)** — two AFC users asked for the opposite: send the one command and let the firmware answer. Backends whose users have no console to fall back on keep the existing behaviour.
- **A QIDI tool remap badged one lane and operated on another** — the mapping was written in one direction only, so with T0 remapped to slot 2 the AMS panel showed lane 2 while the filament panel gated and acted on lane 0. It was also the only mapping-capable backend that silently disabled the print-start remap snapshot.
- **Every Load on a Snapmaker U1 seated the carriage and fed nothing** — the unload-before-load rule encodes a serial assumption about clearing one shared path. The U1 is parallel — four toolheads, each with its own extruder — so the rule was true always and routed every Load through a tool change.
- **The filament strip and the AMS panel could show different lanes as loaded** — after an idle unload the embedded mini status kept a lane badged while the AMS panel had already cleared it.
- **A crash while thumbnails were loading in the print file browser (#960)** — four thumbnail callbacks checked a liveness flag and then dereferenced the panel on a worker thread, and the call was virtual, so it dispatched through a freed vtable pointer. Fetch results are also now always delivered on the main thread: which thread they arrived on depended on which internal path produced them, and callers set image sources from them (#1202).
- **A crash when the orientation changed with the bed mesh panel open** — the reactive portrait branch rebuilds the canvas in place and nothing nulled the panel's pointer to the old one, so every later use — including the async render thread's entry point — was a use-after-free.
- **A crash shutting down while the printer was unreachable (#1212)** — a retry timer armed on every failed connect was never recorded, so it could not be cancelled, and the event loop nulls its handle on one thread while still draining timers on another.
- **A language change overlapping a Moonraker error read freed memory (#1219)** — the event handler resolved its notification title on whichever thread raised it, while the translation table is freed and replaced from the main thread.
- **A crash after a print-start preparation timeout (#1221)** — a null check, then a queue drain that ran an already-queued overlay dismissal, then a use of the pointer that dismissal had just nulled. The optimistic-navigation unwind is now gated on print status still being on top, since after a 60-second timeout you may be several screens away and popping blind closed the screen you were on.
- **A modal dismissed through the static hide path leaked its object (#1230)** — its teardown hook never ran, its statics stayed non-null and its widget pointers dangled.
- **A 16-bit PNG overran the decode buffer** — decode output is allocated as a fixed four bytes per pixel while the decoder writes the file's own bits per pixel, so 16-bit RGB and RGBA overran by 1.5x and 2x. Such files are now rejected at allocation.
- **The recovery dialog did not reappear after a backdrop dismiss** — the next shutdown event was spent clearing a stale pointer and showed nothing; the dialog only returned on the event after that.
- **Data races in WebSocket teardown (#1212)** — the connection-state callback had a mutex guarding only the read, so replacing it freed the old target's storage while it was being copied on another thread.
- **A 36-point bed mesh counted to 72 on a Qidi Q2 (#1224)** — progress divided by a sample count read from the printer config, which only works when we recognise that printer's probe section, and the list of those is open-ended across firmware forks. Probe points are now deduplicated by position, which needs no configuration and absorbs retries a fixed divisor cannot.
- **"Bed is Level!" on a bed that was not (#1225)** — Klipper reports every screw relative to the first one in config order, so a mid-range base let two corners sit inside tolerance in opposite directions while the real corner-to-corner error was their sum: 0.111 mm reported as level. The verdict now measures signed spread across all screws, and the tolerance is converted through the real screw thread pitch read from the printer config, so an M4 bed is no longer judged with M3 slack.
- **WLED strips took an error on every action (#1241)** — Moonraker nests the strip map one level deeper than the code unwrapped, so it iterated a single entry named `strips`, posted that as the strip name, and read every polled state as off. The mock emitted the same flattened shape, so the parser and its test fixture agreed with each other and no test caught it.
- **Power devices with hyphens in their name did nothing (#1241)** — Moonraker names power devices after their config section, and a name like `-Power-` was rejected by an identifier check before the request was ever built. The name is URL-encoded and never reaches G-code, so only control characters are rejected now. A rejection surfaces as a toast rather than a log line nobody sees.
- **The four z-step buttons were 0 px wide at 272x480** — the print-tune z-offset row carried flat pixel literals from landscape: rendered, bound, impossible to tap. A non-wrapping help label also overflowed the section by 54-67 px at every portrait size.
- **The header action button rendered taller than the bar containing it** — its height ladder exceeds the header's from the Large tier up, so at 480x800 an 80 px button sat in a 68 px header and clipped off the top of the screen.
- **The advanced panel's E-Stop, Restart Klipper and Firmware Restart labels clipped mid-word in portrait** — three buttons sharing one flex row, each getting a third of the screen. They stack in portrait now; landscape is unchanged.
- **The portrait exclude-object list crowded out the object map** — its height was a constant meaning "the controls take about 55% of the column". They take 14-34%, and because the list is a floating child its percentage never consults a measurement, so the surplus sat on top of a live tappable map: 43% of it at 480x800, and 636 px of empty list at 320x1480. Portrait now measures the controls; landscape keeps its existing rule.
- **The portrait print-status preview reserved the wrong amount of space** — the metadata strip has two stable heights depending on whether the print sets an M117 message, so no fixed reservation was correct. The split is computed with flex now, which also replaces a hand-tuned offset in landscape that had been clipping the top of the artwork. Portrait controls compress into one row of temperature chips, two merged status rows and a single row of five buttons, which roughly doubles the artwork band at 480x800 and frees enough height for the fan row to return; the reclaimed slack carries a temperature mini-graph.
- **The bed mesh panel stacked into a column that starved its canvas in portrait** — reserving height for both cards before sizing the canvas capped it at 117 px on a 480x640 screen. The two cards sit side by side in one row now and the canvas grows to 273 px. Max/Min coordinates round to whole millimetres and drop to an indented sub-line only where they still do not fit, instead of wrapping across three lines.
- **The preparing overlay centred below the artwork, over the dimmed metadata strip** — and its progress bar cost 12 px plus a gap while showing zero.
- **A large modal spinner was sized from the cramped axis** — a 320x1480 panel drew a 48 px spinner inside a 471 px card.
- **A vertical icon layout was discarded when the icon came from a binding** — the button parsed the icon position but only reached the code using it when a static icon attribute was also present.
- **Hot reload brought a component back live but inert** — reloading destroyed the component's scope and took C++-registered subjects with it, so every binding naming one resolved to nothing. Before a companion fix in the XML engine, the same path aborted on a heap error instead.
- **A misleading 14m37s gap in the logs (#1218)** — wall-clock-only timestamps make a clock step look like a stall, and one bundle appeared to show a long Moonraker reconnect gap that was really an 874-second step on a printer with no RTC. It cost real triage effort. Every line now carries a monotonic column and any line the wall clock jumped across is annotated with the size of the jump.
- **A heap snapshot age of 33 hours against a 9.8 hour uptime** — not stale but impossible, and the number was printed as fact with no way to tell which of two causes produced it. Raw clock stamps now ship alongside the age and the reporter says which explanation fits.
- **The temperature chart evicted the entire diagnostic log** — one debug line per series per sample interval, so a bundle arrived with 2000 of its 2000 ring lines being that one message and nothing else from the incident.
- **Crash reports auto-filed GitHub issues for builds that were never published (#1240)** — the ingest key is compiled into every binary and the repo is public, so any fork or local build could file an issue that can never be symbolicated, because no symbol file for that build exists anywhere. One arrived claiming a version this project has never held.
- **Nightly test shards reported green with failures in them** — the sharded loop collected the exit status of a text filter in a pipeline rather than the test binary's, so a shard with 8 failing cases still passed the job.
- **A submodule patch that existed only in a worktree was silently never applied** — and an unreadable patch file reported itself as already applied. Both now fail the build loudly.

### Changed

- **Slider settings persist when you let go, not on every tick of the drag** — brightness, volume and LED startup brightness each wrote the settings file per tick: serialize, two fsyncs and a rolling backup copy, plus a forked shell per tick for the backlight on Sonic Pad. Durability is unchanged; only how often the durable write happens.
- **A tap on the object map no longer does unbounded disk I/O (bundle C2CP6ZAW)** — the G-code layer cache held its lock across a file seek and a parse, so work on one layer blocked every other, and the touch hit-test reached that path from inside a UI callback while a background builder walked all layers through it. On a two-core K2 Plus the reporter worked around it by forcing thumbnail-only rendering.
- **Switching G-code files no longer freezes the UI for a whole parse** — the background load only checked for cancellation after the entire file was parsed, and the main thread waits for that thread, so switching files stalled everything for seconds on a multi-megabyte file.
- **Thumbnail PNGs are read on a worker thread** — scrolling a G-code folder previously put a whole-file read on the main thread for every card, once per file as the listing populated.
- **WiFi scans stop spending two thirds of their time off-channel.**
- **Layouts can branch on orientation and on height** — orientation and the vertical size tier are published as bindable values, so a panel no longer has to infer either from the narrower of its two axes.

## [0.99.106] - 2026-07-31

Most of this release is about screens that are not 800x480. The 480x272 panels get a sweep
through layouts that were drawing past their own edges — a PIN keypad whose bottom row was
off-screen, a bed mesh list with no complete row in it, dryer controls and history amounts cut
off at the card edge — and portrait gets grid sizing, pixel tokens and a default layout of its
own instead of landscape's numbers rotated onto a taller screen. Alongside that, the print file
browser stops cropping its thumbnails, and the thumbnail cache stops walking itself on every
fetch.

### Added

- **An unreachable printer now offers Change Address** — a connection failure raised an OK-only alert, so acknowledging it returned you to the same dead end, and the setting itself is buried under Settings > System > Printer Host with nothing pointing there.
- **Portrait overlays use the whole screen width** — overlay sizing subtracted the navigation bar from the horizontal axis, which only holds when the bar is a full-height strip down the side. In portrait it runs along the bottom, so a 320px Waveshare panel drew 266px overlays with a dead strip of backdrop beside them.
- **The WiFi picker shows each network's band (#1189)** — a 2.4G / 5G / 2.4-5G badge, shown only when the scan actually spans bands.

### Fixed

- **Portrait dropped home widgets permanently on first open (#1215, #1216)** — auto-placement asked for each widget's authored column span unclamped, so anything wider than the grid failed a loop that never ran and was switched off and saved that way, with a toast blaming a grid that had free rows. Portrait also derived row heights from the width constant, rationing the axis with room to spare, and inherited landscape anchors authored for six columns. It now has its own default layout and falls back to a widget's minimum span rather than dropping it. (A widget the old bug switched off stays off — re-add it from the catalog.)
- **Tall screens took their sizing from their narrow axis (#1209)** — every pixel token resolved from min(width, height), which is right for anything that has to fit across and wrong for anything that stacks, so a 1480px-tall panel inherited 32px buttons and 200px dialog bodies from its 320px width. Landscape and square displays are unchanged by construction.
- **Type stayed sized for the startup screen size after a resize (#1210)** — font registration decided which tiers exist from the startup breakpoint and then early-returned forever after. The navigation bar had the same split brain: a 76px ultrawide strip became 132px after any resize.
- **The PIN keypad could not be used at all on a 480x272 screen (#1204)** — it was a fixed 320x340 portrait card, so the bottom row sat off-screen and 0 and confirm were untappable. It now reflows to a landscape card, keeping the full-size touch targets.
- **The bed mesh profile list showed no complete row at 480x272 (#1204)** — a hardcoded 44px row height left a 33px viewport. Axis letters also landed on top of the tick labels on a small canvas, because the gap between them is expressed in world units and shrinks with the plot while the labels do not.
- **The print-status emergency stop was clipped top and bottom (#1204)** — a hardcoded 44px circle inside a header whose height is responsive.
- **Filament-by-type amounts were clipped off the edge of history cards (#1204)** — the row needed 207px of fixed widths inside a card that is 170px wide at 480x272.
- **The AMS dryer controls were clipped at 480x272 (#1192)** — 120px of controls in a 94px box, losing 13px off both the preset dropdown and the Start/Stop button.
- **The fan percentage never appeared on 480x272** — the micro layout hid it on a condition that is always true there, on the one screen size where the slider position is the only other feedback.
- **Print status did not fit in portrait** — the fan row measured its own container against its children, which only answers the fixed-budget question a landscape row poses, so it stayed hidden however much screen was free. The exclude-object list encoded its width as a fraction of a row that portrait does not have.
- **Print file cards lost their metadata and cropped their thumbnails (#1208)** — the two metadata groups each sized themselves to the other, so both collapsed and the time and weight labels laid out past the bottom edge of the card at every screen size. Thumbnail targets also came from a resolution table that had drifted from what the grid lays out, so images overhung their cards and were cropped through the model.
- **The print file grid always chose the narrowest card that would fit** — column count was maximized rather than fitted to the width cards are drawn for, so 800x480 showed 5 cards of 132px against a 170px design. It now shows 4 of 167px; 480x272 is unchanged.
- **An AMS slot edit could be committed from a view that forbids it** — the micro header bar never declared the property that hides a per-view Save button, and an undeclared attribute is dropped in silence. The Macros header had the same permanently-visible dead Save.
- **A crash on shutdown while thumbnails were still being processed (#1202)** — the thread pool was checked under a lock, the lock dropped, and only then used, racing the shutdown that clears it. Worse than a dangling read: a task landing after shutdown restarted the pool and spawned workers past teardown.
- **One unreadable thumbnail could disable cache eviction entirely (#1207)** — eviction ran unsynchronized from the main thread and every download worker, and a single entry another thread had just deleted discarded the whole scan. The partial total then read as "nothing to evict", so the cache grew without bound.
- **A working webcam was rejected as unreachable (#1205)** — the snapshot probe spent one 2s budget on both connecting and responding, so a go2rtc endpoint waiting for a keyframe before it can transcode (measured up to 2.9s on a Pi 5) failed exactly like a stale DHCP entry, and the reporter fell back to a local camera that does not exist. Connect still fails fast at 2s; the response now gets 6s.
- **An unreachable printer reported itself as a halted Klipper** — the gate read only Klipper's state, which starts at SHUTDOWN and has no "unknown", so a session that never opened a WebSocket was indistinguishable from a genuine halt. Every command came back "Klipper is halted — restart firmware to continue", printed verbatim on the PID screen.
- **A wrong or stale printer address retried silently forever** — both existing escalations only arm once you have connected at least once, so an address that never worked produced nothing but a disconnected icon that never names what it is dialing. After 60 seconds with no socket ever opening, the failure is surfaced with the host and port in it.
- **Calibration Start buttons stayed live on a printer that could not be reached** — bed mesh, QGL, Z tilt and nozzle clean put Start in the header bar, which sits outside the container carrying the enable gate, so the body greyed out and the button did not.
- **A dropped socket could kill reconnection permanently** — one transient failure to create a socket, under file-descriptor or memory pressure, left nothing to re-arm the retry. The backoff was also reset on the TCP connect rather than the WebSocket upgrade, so a proxy that answers with Moonraker down produced 25 reconnects in 2.5s where there should have been 5.
- **A busy-printer toast appeared for filament operations you started yourself (#1206)** — the toast keyed on Klipper reporting activity, which an ordinary unload does. The safety gate that blocks late motion during a filament operation is untouched; only the toast changed.
- **A recover prompt offered the wrong recovery (#1172)** — it never read the recovery actions the error itself carried, so every warning was offered the same MCU bounce whatever the fault. An action carrying gcode now runs its own, and a set of them escalates to the dialog that can show them. A button marked OK also no longer transmits "OK" to Klipper.
- **Load was disabled on every tool of a generic toolchanger (#1199)** — the rule keyed on slot presence, which is true for every toolchanger slot forever since a slot there is a physical toolhead, and it offered unmount on tools sitting in their docks.
- **Per-printer capability overrides survived a printer switch (#804)** — bed mesh, QGL, Z tilt, nozzle clean, heat soak, chamber and speaker settings were read once at startup and kept the first printer's values for the life of the process. The add-printer and cancel paths never invalidated anything at all.
- **Removing a printer could delete the only one, or leave the app pointing at something that is not a printer** — the guard counted every key in the printers map, including the ones that are not printers. Toolhead style and U1 detection were also stored install-wide despite describing one machine, and scanner settings per-printer despite describing the host; a migration fans them out correctly.
- **A rendering overflow on rounded-corner clipping** — the mask drawing never clipped its area to the layer buffer, so an overhang wrote past the end of a row.
- **An emergency stop arriving from the network touched main-thread state from the wrong thread** — dialog creation was already deferred but the work in front of it was not.
- **GitHub release notes were cut off partway through** — the release job piped the tag annotation through `head -50`, so anything longer lost its tail while the job still reported success. v0.99.105's notes were the first casualty and have been restored.
- **A misleading log line for gcode held behind a blocking operation (#1206)** — it said the command was being queued, which cost a reporter a session hunting for a flush that never comes. HelixScreen keeps no such queue; what is dropped is the reply, not the request.

### Changed

- **Thumbnail fetches no longer scale with the size of the cache (#1207)** — eviction walked the whole directory with a stat per file on every fetch, from six call sites. Measured over a 40-file cache, 60 directory walks and 3300 stat calls become 0 and 30, and the numbers do not move at 400 files.
- **The XML engine is now its own MIT-licensed project** — helix-xml, our fork of the engine LVGL removed in 9.5, has moved to github.com/prestonbrown/helix-xml and is consumed here as a submodule. Our contributions are relicensed MIT and every file carries per-file provenance against the fork point, so the library contains no GPL and is usable standalone. A fresh clone now needs `git submodule update --init --recursive`.
- **Ultrawide and portrait layouts are documented as alpha** — the README, user guide and FAQ advertised 1920x480 as fully supported and said nothing about portrait. Detection, navigation bar sizing and grid tiers work; every other panel still falls back to the landscape layout.
- **The Open Source Licenses list and COPYRIGHT agree again** — they had drifted apart five ways and both were wrong in places: OpenVDB is Apache-2.0, wpa_supplicant is BSD-3-Clause, and stb and GLM were understated.

## [0.99.105] - 2026-07-29

> **0.99.104 was skipped.** It was tagged but its release build failed on three
> platforms, so no artifacts were ever published — 0.99.103 remained the latest
> release. Everything intended for 0.99.104 ships here, plus the fixes for what
> broke that build.

Filament systems get a deep correctness pass — AFC and Spoolman especially, most of it verified
against live BoxTurtle hardware — alongside a sweep through the crash class where a background
callback outlives the thing it was going to update, and several paths where an update or a file
permission could quietly destroy a working configuration.

### Added

- **A stalled AFC fault now offers the recovery that fits it (#1171)** — AFC was the only filament system whose faults reached you as a bare error with nothing to tap. It now gets the same Resume / Unload-or-Eject / Recover set the other backends have, chosen from where the filament actually is: a heated toolhead unload when filament is at the head, a cold lane eject when it is not.
- **A lane's preheat target comes from the spool you linked to it (#1149)** — AFC has always published the recommended print temperature from the linked Spoolman spool, and it was the last field going unread. Preheat now uses it instead of falling back to the generic figure for the material.
- **The fault-position diagram says where the filament stopped in words, not only in colour (#1196)** — the failing section was distinguished by red alone, which is exactly the red-against-green pairing a colour-blind user cannot separate, and which washes out on a printer screen in daylight. A caption names the failing gap outright, and it also carries the one case position cannot show: a jam at the toolhead rather than between two points.
- **A lane keeps its spool identity across an eject** — brand, spool name, total weight, color name and the Spoolman IDs have nowhere to live in AFC's or Happy Hare's own records, so they are stored separately now. Pulling a spool for maintenance and putting the same one back no longer loses what you entered. (Happy Hare's half was written without hardware to test on and wants validation from an MMU owner.)
- **"It's a new spool" is the primary answer when saving into a lane that is already linked** — putting a different physical spool in a linked lane could previously only overwrite the old spool's identity in Spoolman. It now creates a new spool and rebinds the lane, leaving the linked one untouched. Cancel stays a true abort.
- **Clear Spool is offered whenever a slot carries an assignment**, not only when the lane is empty — a stale assignment does the most damage right after new filament goes in, which is exactly when the affordance used to disappear.
- **Lane position recovery is a mode of the Unload button** — the per-lane Reset entry sent a physical filament move on AFC and a bookkeeping fix on Happy Hare, sharing one word and one icon with the system-wide sidebar Reset. Recovery is now picked from live state like Eject already was, and fault clearing lives on the sidebar.
- **AFC's lane-fault messages get a readable filament-position diagram (#1184)** — AFC welds monospace bar art onto five of its fault messages, and in a proportional font the bars stop lining up with their labels, so the most diagnostic part of the message reads as noise. The art is stripped and the position drawn properly; an unrecognized message is passed through byte-for-byte and the graphic hides, so upstream rewording degrades to plain text.
- **AFC v1.2.0's richer status is read directly (#1149)** — spool name, fill level, preheat bed target and multi-color swatches now come from AFC itself instead of a Spoolman round-trip, alongside the buffer's health fields and each lane's endstop and selector state.
- **Transient overlays cast a shadow (#1178)** — the gap beside a stacked overlay showed the dimmed panel underneath with nothing to explain it, so it read as a rendering artifact rather than as a layer.

### Fixed

- **Load stayed available on a lane AFC already considered loaded, and the tap did nothing (#1194, #1183)** — the button read one aggregate "which lane is active" pointer instead of AFC's own per-lane state, so any disagreement between the two left Load enabled on a lane the firmware would refuse. Every affordance built on that reading was affected, including the active-lane highlight and the context menu offering Recover on a lane that had only reached the hub.
- **Lane recovery could physically de-seat a working lane** — when AFC named no specific lane, recovery was offered on every lane sharing the hub, on the reasoning that a wrong guess costs one harmless refusal. It does not: the lane reset begins with an unconditional 50mm retract before it checks whether that lane's filament is even there, the hub-clear guard has already passed by the time we offer it, and the firmware's own toolhead guard logs its objection and then performs the moves anyway. A wrong guess dragged a seated lane back toward its drive gears. Recovery is now offered only on a lane AFC itself names, and only when that lane's load switch is actually triggered. (#1182, #1187)
- **Tool selection was never sent on an AFC toolchanger** — the extruder list was read from a field AFC only publishes over an HTTP endpoint we never call, so multi-extruder machines looked like single-extruder ones and the tool-select command was silently skipped. It is now read from the list AFC does send.
- **An ejected lane kept its old Spoolman spool when its record was re-read (#1195)** — the two code paths that read lane data disagreed about what a cleared spool means, so depending on which ran last a lane could keep showing the previous spool's name, colour and remaining weight, and a later edit would have written to it. Your own overrides were also being dropped on that path.
- **Several queued AFC messages left a resolved error on screen (#1186)** — clearing a fault sent a fixed number of clears rather than draining until the queue was empty, and nothing else pops those entries, so warnings and already-resolved errors accumulated across a session and the oldest one stayed visible.
- **One fault could produce two notifications (#1197)** — the same fault arrives through two independent observers, and the backstop that exists to catch faults nothing else surfaced could not see that the other had already shown a toast.
- **Load and Unload were offered while a print owned the toolhead** — both are toolhead-motion operations and had no business being tappable mid-print.
- **Per-slot load state is now believed on five more filament systems (#1199)** — AD5X, QIDI Box, CFS and ACE were deciding "is this slot loaded" from a single active-slot pointer. On CFS the consequence was concrete: because no slot was ever marked loaded, the AMS panel never offered Unload on a bay that had filament in it. On AD5X the fix also keeps the active-lane highlight through a runout, which previously dropped it at the moment you needed to recover. (Happy Hare deliberately keeps the existing rule — its own values already are the firmware's truth.)
- **WiFi did not come up until you opened a screen that needed it** — it is now brought up at startup.
- **Saved WiFi credentials could silently fail to persist** — the save was trusted rather than verified, and on devices where wpa_supplicant's config lands on a RAM-backed filesystem it was lost on reboot. The write is now confirmed to have reached disk, and a config that lands on tmpfs is restored.
- **Shutdown did nothing when Moonraker could not reboot the host** — it now falls back to powering down locally.
- **A pending Resume could stay stuck when Klipper rejected the macro** — the button stayed in its in-flight state with no way out.
- **An `action:prompt` command arriving glued to its payload was dropped** — it is now recovered rather than ignored.
- **`--version` failed when another instance was running** — the instance lock was claimed before arguments were parsed.
- **Crash reports could arrive with no reason attached (#960)** — glibc's abort message was read as the wrong type, so the most useful line in a blank abort was missing.
- **Uploaded logs carried network names and MAC addresses (#1191)** — the log ring is captured at debug regardless of your configured verbosity and leaves the machine three ways: the debug bundle, the crash reporter's automatic upload, and the `ctl` log RPC. It held the connected SSID, every neighbouring SSID in range with signal strengths, and the adapter MAC in cleartext. A set of nearby network names is a geolocation fingerprint, and a scan enumerates networks belonging to people who never consented to being in a bug report. Those values are now replaced at the log call site with a per-boot token that still correlates one network across lines.
- **Moonraker one-click updates destroyed custom images, themes, per-printer database overrides and crash history (#1164)** — Moonraker deletes the install directory before extracting, and only the four symlinked config files survived it. Those directories get the same protection now, and existing installs are migrated into place.
- **The first spool save after install stranded every per-tool spool assignment (#1176)** — the atomic save replaced the symlink instead of writing through it, so each later update destroyed the assignments while the relink afterwards made nothing look wrong. Found on a Pi where the surviving copy was three months stale.
- **A read-only settings.json was treated as corrupt and reset to factory defaults** — config reads were opened for read/write, so a root-owned or `0444` file destroyed the entire setup while being perfectly readable.
- **In-app updates marked configured users as needing first-boot setup again** — and on a multi-printer config that was silent data loss: stale-entry recovery then erased the active printer's heaters, fans, sensors, macros and Moonraker host with no prompt.
- **An install with a stale asset name could never update again (#993)** — Moonraker resolves which asset to download from the installed `release_info.json` and falls back to the alphabetically-first asset when it matches nothing, so the fix could only ever arrive through the channel it had broken. That file is validated and repaired at startup now.
- **Installer path overrides were fed to `rm -rf` and `mv` verbatim** — a mount root passed as `TMP_DIR` once wiped a device's `/mnt/UDISK`. Overrides are validated now, downloads are checked against the published SHA256 rather than a CRC alone, and the documented `curl | sh -s -- --clean` invocation no longer skips its delete-your-configuration prompt.
- **First boot was a dead end when Moonraker was up and Klipper was in error** — one of the most common first-boot states. The step had no Skip, Back is hidden when it is first, and the gate stayed closed, so the only exits were `--skip-wizard` or hand-editing settings.json. You could not even reach the app to see the Klipper error.
- **An interrupted wizard collapsed eight later steps on the next boot** — printer identification persisted its preset on Back too, and that marker was trusted as authoritative, with no in-app way back if the preset was wrong.
- **Printer discovery could spin forever (#1161)** — a WebSocket drop mid-discovery left the spinner running and Next disabled with no path out. A 30-second watchdog now unblocks it with a distinct warning.
- **Finishing the wizard with Klipper down flagged every fan, LED and filament sensor as newly appeared hardware on the next healthy boot (#1160)**, with no route back to the steps that were skipped. The snapshot is deferred until the first successful discovery, which then offers to re-run just those steps.
- **Configuring a second printer overwrote the first one's preset marker (#1162)** — it lived at the root of the config rather than under the printer, so widget seeding and wizard step collapsing read a marker belonging to the wrong machine.
- **A filament operation could spin forever (#1183)** — AFC answers a command it has nothing to do about ("lane3 already loaded") in 4ms without ever entering a toolchange, and the completion path keyed entirely on a state transition that therefore never happened. AFC now marks the operation at dispatch and resolves it on the macro's own acknowledgement.
- **A stalled operation left its button stuck for the rest of the session (#1183)** — all eight operation guards shared a timeout that re-enabled the buttons without ever failing the operation, so the per-button spinner state never cleared and the next operation could not complete.
- **An AFC operation that never finished hung the UI with no way out (#1188)** — AD5X was the only backend with a stuck-action timeout, so a silently hung macro, a WebSocket bounce mid-operation, or a Klipper shutdown mid-toolchange left the interface busy indefinitely.
- **Filament recovery ran into a cold nozzle (#1193)** — Resume and Unload dispatched their gcode with no temperature check, so they failed the same way the operation that raised the error did. A post-op cooldown, a print error's heater shutoff, or Klipper's idle timeout can each zero the heater between the fault and the tap; the actions that push filament through the melt zone now preheat and wait.
- **The filament error dialog outlived its fault (#1185)** — it was only ever torn down when the panel was destroyed. On a live printer a lane-reset failure from 19:23 was still offering Resume, Eject and Recover at 19:39 with the print recovered and running at 30%, none of the three buttons correct.
- **A fault could stop the spinner while putting nothing on screen** — a backend can raise an error carrying no event and no `!!` line, which reads as success. A last-resort check now surfaces the backend's own description when no dialog took ownership of the fault.
- **The toolchange step bar never advanced past cut and brush (#1183)** — AFC emits its load and unload narration with no `//` prefix while only the decorative steps carry one, so the most important progress marker could never fire. Separately, a macro that aborts on an undefined command is reported through a channel that still answers "ok", so a purge dying on line 4 of its own body finished with a green checkmark; those now fail the operation and name the missing command.
- **AFC offered Reset on lanes where it could not work, and the refusal latched on screen** — `AFC_LANE_RESET` retracts filament from the bowden back to the hub and refuses unless the filament is at the hub with a free toolhead, but the entry appeared on every lane. Firing it on an ejected lane produced "Hub is already clear", which latched in AFC's message and kept re-firing error toasts hours later.
- **AFC's hub sensor was attributed to every lane at once** — one sensor is shared across a unit, so a triggered hub proves filament is past it but says nothing about whose. Recovery now follows the lane AFC names as active, and falls back to every lane routed to that hub when AFC names none.
- **A latched AFC field drove the filament path graphic and the reset gate** — `loaded_to_hub` is set when a lane is prepped and never clears, so it reads true on all four lanes at once. Both now read the real hub sensor.
- **Pressing Reset left the error text on screen** — AFC's message is a queue and each clear pops a single entry, so clearing once could leave the next queued error showing, indistinguishable from Reset having done nothing. Reset now clears the fault and drains the queue.
- **An ejected AFC lane kept showing the old spool and kept its Spoolman link** — AFC clears a lane itself on eject, but the parser could not express a clear at all, so only material ever went away. A later edit then aimed a Spoolman write at the wrong spool.
- **Saving a spool needed two passes to stick on AFC** — the link write was emitted last, and AFC rewrites lane material, color, weight and temperatures from the linked spool, so one save set the data and then destroyed it.
- **Spool edits were silently dropped on AFC installs reporting an old version** — the version was read from a database namespace nothing in AFC writes, so a current BoxTurtle reports 1.0.0 and every color, material, weight and spool-ID write was skipped with only a log hint.
- **Spoolman was written before you answered the "different filament?" prompt** — the update ran ahead of the confirmation, so Cancel, documented as a true abort, could not retract it.
- **A spool's total weight never reached Spoolman** — only remaining weight and the spool ID were compared, so the edit lit up Save, persisted locally, and Spoolman kept its old value indefinitely.
- **Unlinking a spool and entering a weight in the same save emitted no weight at all**, forcing a second edit.
- **Stale per-tool spool assignments were never cleared** — a lane that lost its spool left its old assignment behind, and those persist to disk and to Moonraker, so they outlived restarts.
- **Turning Spoolman sync off left its poll timer running** — the Spoolman overlay took a polling reference every time it opened and never gave one back, so the toggle was largely decorative.
- **The clog configuration modal sent a Happy Hare command on every backend (#1155)** — AFC's buffer fault detection also offers the clog widget, so an AFC user could pick a mode, press Save and get "Unknown command". ACE, CFS, QIDI Box and the tool changers had the same exposure; the controls are now hidden where they cannot work.
- **Recover did nothing on the AMS overview panel** — the button rendered enabled and its action fell through to nothing.
- **Happy Hare's system-wide fault clear sent nothing** — Reset is pressed when nothing is loaded, and exactly that state was rejected as an invalid slot.
- **The AD5X color picker applied all 24 swatches in turn (#1065)** — zmod echoes every dialog button down the gcode console, and each one was read as an executed edit. One bundle showed 87 echoed buttons producing 162 phantom applies and a 40-second stale material label.
- **Malformed filament-system data could abort the app outright** — a missing key read from a const JSON object is an uncatchable abort in this build, not a catchable error, so five guarded-looking chains could kill the process; on CFS a disconnected unit reports scalars where slot arrays are expected, which killed the whole frame every poll and left the AMS panel silently empty.
- **One malformed field could take out a whole Moonraker response** — a null in file metadata aborted the entire listing, a null among power devices or timelapse settings emptied the list, and a wrongly-typed error field leaked the request until its 60-second timeout and surfaced as a bogus timeout toast.
- **Background callbacks could run against a destroyed owner (#1165, #1146)** — a queued UI update runs at the next drain whether or not the object that queued it still exists, and the crash then lands somewhere unrelated. Every such site in the codebase now carries a lifetime guard, including a genuine one in the QR scanner and the build-volume notification behind #1146.
- **Timers stayed armed on freed objects (#1173)** — the screensavers, print controls, PID calibration, the wizard's auto-probe and the update checker each cancelled their timer only on the normal teardown path, so any teardown that skipped it left the callback pointing at freed memory.
- **Settings > Spoolman touched the UI from a network thread every time it opened**, including creating and deleting timers while LVGL might have been walking its timer list.
- **The input shaper panel's subjects outlived the ordered shutdown pass (#1180)** — it was the one panel that never registered its teardown, so ordering was decided by panel destruction instead.
- **The fan row could freeze at 0% and the wrong fan could be treated as the part fan (#1181)** — re-assigning fan roles zeroed every fan's live reading, and Moonraker only reports changes, so a fan holding a steady speed never restored what was zeroed. Recovery needed a speed change that a steady fan never produces.
- **A paused printer showing an uncoded error had nothing to press (#1152)** — the modal offered no recovery actions at all. It now offers Resume plus a dismiss when the job is genuinely paused.
- **Slow drags did nothing on the bed mesh (#1133)** — rotation accumulated as an integer and threw the remainder away, so a 1px-per-event drag over 200 events moved the mesh zero degrees. The tilt clamp also moved into the renderer, so the view can no longer be driven past the point where the depth sort inverts.
- **Four error-recovery button labels were never translated (#1174).**
- **A widget name repeated inside one layout file left the later one built but never configured (#1136)** — the AMS panel had exactly that, and a gate now catches it.
- **`border_side="left|bottom"` silently removed the border** instead of drawing both sides — the combined form fell through to the unknown-value branch, which returns "no border".

### Changed

- **Backends now declare whether they carry per-slot load truth**, and the AMS layer believes that over its aggregate pointer where they do. Each backend that opted in derives its per-slot answer from the same signals the aggregate is built from, so the two cannot disagree — the benefit is that "can this slot be unloaded" finally reads correctly rather than any behaviour changing underneath you.
- **Overlay width is decided by how you reached the overlay (#1178)** — a destination overlay is full width, a transient layer leaves the backdrop showing at its leading edge, and the same overlay can be either depending on where it was pushed from. Console Settings no longer renders wider than the Console it was pushed from.
- **The transient-layer shadow is lighter on light themes**, where the same alpha rendered as a heavy black band rather than a gradient.
- **AFC's version is read from its status object and lane vendor from `vendor_name`**, tracking changes agreed with upstream; both are inert until that firmware ships.
- **`helixscreen ctl` gained freeze/unfreeze, `wait_idle`, `text`, `reset`, stable and widget-cropped screenshots and JSON output**, its two help listings are generated from one table so neither can drift, and every cross-compiled target can now build it in — the flag was silently dropped by all but one.
- **An out-of-process UI test suite drives a live instance through `ctl` and compares screens against approved golden images**, wired into CI alongside the existing tests.
- **Contributor gates and threading docs** — a wrong rule about lifetime tokens is corrected across five documents, and new gates catch background-thread anti-patterns, timers not cancelled in destructors, duplicate widget names, uncatchable const JSON reads and UI callbacks leaked between tests.

## [0.99.103] - 2026-07-26

### Added

- **Speed, flow, message and LED-effect commands go through while the printer is busy (#1129)** — `M220`, `M221`, `M117`, `SET_LED_EFFECT` and `SET_PIN` are treated as discretionary now, so they are not held behind a long-running operation, and nudging an `output_pin` fan is no longer announced as an "LED change".

### Fixed

- **AFC lane colors and materials from the AFC database were silently unused (#1148)** — the reply to AFC's database query was read from the wrong level of the JSON-RPC envelope, so the guard was false on every successful response: the version stayed "unknown", the per-lane query was never issued, and every lane fell back to default grey. The query also asked for the wrong namespace and could not parse the `#RRGGBB` colors AFC writes there.
- **A current AFC install could be nagged to upgrade** — AFC stopped writing its version to Moonraker in June 2025, so the string we read is either absent or frozen: a live BoxTurtle reports "1.0.0" while its payload proves 1.0.32-era. Nothing keys off that string any more — capabilities are feature-detected — and the version stays only for display and debug bundles.
- **AFC 1.2.0 toolchanges showed a raw state token and lost their toasts** — upstream renamed "Tool swap" to "ToolSwap" and added ToolDock and ToolPickup; none were recognized, so the camelCase token reached the screen verbatim and the swap reported as idle, which also defeated toast suppression. Matching now ignores case, spacing and separators, and an unrecognized state is humanized rather than passed through. Per-extruder status, next pickup and standalone flags are parsed for toolchangers.
- **In-app updates could not bootstrap the fix that repairs them (#993)** — BusyBox and OpenWrt printers (K1, AD5M, K2) verify a download with `unzip -t`, which their BusyBox is too old to have or which is missing entirely, so they rejected a byte-perfect zip as a corrupt download — including the release that fixes the verifier. Those platforms are served the `tar.gz` now, which they can verify with `gunzip -t`.
- **Pressing update in Mainsail could delete a HelixScreen install (#993)** — Moonraker versions before v0.10.0 ignore `asset_name` and download the first release asset by name, after deleting the install directory, which meant a symbol archive rather than the release. Symbol assets are renamed so a real release sorts first, and the installer no longer writes the Moonraker update stanza on a Moonraker that cannot honor it, removing any stanza an earlier install left behind.
- **Moonraker went undetected on Creality's nested layout (#993)** — on a K1/K2 the repo is cloned a level below the install dir, so the probe missed and returned undetermined on exactly the platforms the check exists for. Verified on a K1C.
- **Belt-tension hardware was never detected (#1137)** — the objects list was read from the wrong level of the JSON-RPC envelope and returned its empty default with no error and no log line, so belted-Z and PWM-LED detection was dead code on every printer.
- **Two malformed replies aborted the app rather than failing (#1139)** — a `/printer/info` response with no hostname killed printer detection outright, and a non-object `error` member in a remote-control reply reached `std::terminate`.
- **Klipper-derived busy state survived a Klipper restart (#1129)** — idle-timeout and manual-probe state carried over across a Klippy transition because Moonraker only sends deltas; those subjects are cleared on the transition now.

### Changed

- **The jog pad's diagonal dividers read as gaps cut through the pad** instead of lines drawn over it.
- **Release packages no longer ship the four developer showcase panels (#1135)** — their code was already excluded from release builds; the XML was not.
- **Update telemetry reports self-update success per running version, and whether a device can read a zip at all (#993)** — the signal needed to tell when a platform is safe to serve zip-only. Update events previously had no handler, so the dashboard showed bare counts with no version or reason.
- **Contributor docs and gates** — threading rules are consolidated into `docs/devel/THREADING.md`, the review bar is written down as a rubric, and new lint gates ratchet imperative-UI, logging and design-token debt so it can shrink but not grow.

## [0.99.102] - 2026-07-26

### Fixed

- **In-app updates failed on printers with older BusyBox (#993)** — release zips were verified with `unzip -tqq`, but BusyBox only gained `unzip -t` in 1.32, so the AD5M (1.29.3) and K1 (1.31.1) rejected an intact download outright and every in-app update and Moonraker install failed. Validation now tries `python3 zipfile.testzip()` first, gated on zlib since the AD5M's Python 3.7 has none, and degrades to a structural `unzip -l` probe rather than declaring a good archive corrupt. Verified on K1, AD5M and CC1 hardware.
- **The K2 could not install a zip release at all** — its OpenWrt firmware ships no `unzip` binary and no BusyBox applet, only `python3`, so the pre-download check rejected every update with an `apt-get` hint that does not apply there. The check now asks whether the system can read a zip by any means, extraction gained a Python fallback, and both paths force the exec bit on `install.sh` and `bin/` members so the extracted installer can actually run. Verified on K2 hardware.

### Changed

- **Sonic Pad documentation now states that SonicPad-Debian is the only tested firmware.**

## [0.99.101] - 2026-07-25

LED control gets a round of correctness work, the 3D G-code preview costs noticeably less
memory on small boards, and a batch of null-tolerance fixes stop malformed config or
printer data from taking out a whole screen.

### Added

- **The theme editor has its own preset palette** — picking colors no longer means starting from the active theme's swatches every time.
- **Tap a sent command in the console to paste it back** — reruns and small edits no longer mean retyping. The composer's actions also moved inside the input field.
- **Spoolman mark and spool number in the spool editor header** — you can tell at a glance which physical spool a slot is bound to.
- **Post-operation nozzle cooldown can be switched off** — **Settings > Safety & Notifications > Cool nozzle after filament ops**, on by default and set per printer. AFC runs its own cooldown after a swap, so on those machines you can hand the job to AFC and keep a single timer in charge of the heater.
- **Keyboard long-press and slide-to-select** — the accent/alternate hint now appears in the right place, and sliding onto a hint selects it instead of dismissing the popup.
- **Side-by-side welcome text and language picker** in the setup wizard.
- **`helixscreen ctl` gained screenshots, log tail, shutdown, subtree listing, glob widget targets, geometry and constant introspection, and a synthetic pointer for gesture testing** — the remote-control CLI used for driving and debugging a running instance.

### Fixed

- **LED strips (#1129)** — white-only strips read the W channel instead of showing nothing; a command queued while Klippy is busy now settles its caller callback instead of hanging; in-flight LED state is cleared when Klippy leaves READY; and the startup LED preference is applied once per session rather than re-firing on every rediscovery. Multi-strip selection is no longer collapsed by the overlay, and a macro card whose layout fails to build is skipped instead of taking the row with it.
- **A metadata miss blanked an already-loaded 3D preview** — the preview went blank for the rest of the print whenever G-code metadata was unavailable but the file itself was not (a cached copy, a local path Moonraker cannot resolve, or a scan still in progress). The error is silent, so nothing explained the blank panel.
- **The pre-print overlay never dismissed when layer 0 was never observed** — prints that skipped straight past the first layer left the overlay up.
- **Streaming layer parses lost the extrusion mode (#1127)** — relative-extrusion files could render wrong when parsed incrementally.
- **Spool editing on AMS units** — the filament type selector went missing in spool edit (#1128), the slot editor lost its contents after a rebuild, and `color_rgb` was dropped from in-memory records (#1138). A null sensor reading no longer aborts the whole AMS status frame.
- **Malformed data no longer takes out a screen** — nulls in the config version, printer database and job history are survived rather than fatal; a loader failure is scoped to the offending item instead of discarding the file; read-only probes stopped writing null keys into `settings.json`; temperature limits keep loading when `position_endstop` is null; and the dashboard guards its widget rebuild against config exceptions.
- **Crashes on shutdown and rebuild** — the update queue discards instead of draining in its destructor (#1132), subject deinit can no longer resurrect panel singletons, a rebuild condemns the old subtree before creating its replacement, and an empty size string no longer reads one byte out of bounds in the XML parser (#1121).
- **Desktop/SDL builds** — no more segfault when no accelerated renderer is available, window decorations stop stealing resolution from the emulated panel, Wayland client-side-decoration compensation is bounded, and the SDL audio subsystem is released on shutdown.
- **A cooldown delay of `0` cooled the nozzle immediately** — `filament/cooldown_delay_seconds` is documented as "0 disables auto-cooldown", but a zero delay instead cut the heater on the next tick. It now means off, as written.
- **Smaller UI defects** — an accidental flex gap between the navbar and content, the home panel skipping its finalize pass after a rebuild, key press/release positions read from the wrong input device, and page-scroll setup silently succeeding with no gutter buttons.

### Changed

- **The 3D G-code preview uses less memory** — vertex positions upload as quantized int16 (20 bytes to 12), `RibbonVertex` shrank from 10 bytes to 8 with the normal palette dropped, and enhanced shading now defaults off on constrained devices, where its full-canvas cache was a third large buffer. `HELIX_SSAO=1` forces it back on.
- **43 previously undocumented environment variables are now documented**, along with what a hot reload does and does not restore.

## [0.99.100] - 2026-07-24

Macros get an edit mode so you can hide the ones you never use, filament handling on
multi-lane AMS units acts on the slot you actually picked, and fan selection tracks the
fan that is really cooling the part. All nine translations reach 100% coverage.

### Added

- **Hide macros you don't use** — long-press the macros list to enter edit mode, uncheck the macros you want out of the way, and Save. The hidden set is remembered per printer, so each machine keeps its own list.
- **Complete translations** — all nine languages are at 100% coverage. The 34 remaining untranslated strings across German, Spanish, French, Italian, Japanese, Portuguese, Russian and Chinese are filled in.
- **More illustrated documentation** — the user guide now shows the screens it describes: fan control, sensors, camera, security and the PIN lock, barcode scanner, label printing, print history, the pre-print filament check and the runout recovery dialog.

### Fixed

- **Load and Unload acted on the wrong slot** — on a multi-lane AMS (AFC/BoxTurtle) where a lane other than your selection was loaded to the toolhead, Load operated on the loaded lane instead of the one picked in the dropdown. Both buttons now resolve the slot the same way the button gating does, so the operation can never disagree with what you see.
- **Selecting a filament in the dropdown triggered a physical swap** — on a shared-extruder AMS the dropdown is now selection-only, and the explicit Load button performs the swap. Only a true parallel toolchanger still changes tools on select.
- **Nozzle stayed hot after an AFC swap** — the post-operation cooldown was only scheduled on the gcode path, so a swap that completed through the AMS backend left the nozzle at material temperature indefinitely.
- **Brand lost when saving a spool edit** — opening spool edit on an already-branded slot (say Sunlu) and saving without touching the vendor dropdown overwrote the brand with Generic.
- **A vendor known only to Spoolman could not be selected** — the vendor dropdown was rebuilt from the bundled catalog alone, so a Spoolman-only brand showed as Generic and saved as Generic. Live Spoolman vendors are now merged into the list.
- **AD5X: a routine load wiped your brand override** (#981) — the native LCD emits a material-only colour change on every physical load, which erased the entire per-slot override, reverting Sunlu PETG to Generic PETG and losing the temperatures that drive loading. Firmware truth now wins only for colour and material; brand, spool name and weights are kept.
- **Print status showed 0% for a running part fan** (#1124) — Klipper's auto-controlled `controller_fan` and `temperature_fan` could be promoted ahead of the real toolhead fan (seen on the Sovol SV08). Auto-controlled fans are now excluded, the part-fan choice sticks to the fan that has actually run, and the auxiliary slot prefers a fan you can command over an idle chamber fan.
- **Temperature graphs were empty until reopened** (#1124) — graphs built at startup ran their history backfill before the connection was up. Persistent graphs now refill once history arrives, and again after a reconnect.
- **Pre-print filament check showed no rows** — the "Check filament" dialog collapsed to its one-line explanation instead of listing the per-tool colour swatches.
- **Crash on shutdown** — a reactive row list left a dangling observer when a panel's subjects were torn down before its widgets.
- **Macros empty state** — the empty-state message now uses the full panel area and is centred.

### Changed

- **Developer tooling: remote control** — a running instance can be driven with `helix-screen ctl` (navigate, click, ls, set_value, scroll, screenshot) or an interactive `helix-screen repl`, over a Unix socket or HTTP. This replaces the `-p`/`--panel` launch flags, which are removed, and a new `--skip-wizard` flag suppresses the first-run wizard. Dev and test builds only — shipped device builds do not include it.

## [0.99.99] - 2026-07-22

### Fixed

- **AD5X filament type-then-colour edit** — choosing a filament type and then a colour dropped the new colour, and the button payload could poison the stored material. Both are corrected. (#1065)
- **Pre-print heating label** — the heating-phase label now tracks the actual long-pole heater (the one that gates print start) instead of an arbitrary one, and is guarded against a background-signal race that could relabel it mid-transition.

### Changed

- **Update downloads use zip** — release manifests now advertise the zip archive as the preferred asset (the first phase of retiring tar.gz), and a downloaded update is staged with its platform and version in the filename.

## [0.99.98] - 2026-07-21

This release is largely about filament: a new on-printer product-edit workflow for
building your own catalog, correct OrcaSlicer material matching so synced spools land on
the right preset, and automatic repair of mislabeled slots. It also sharpens print-status
reporting (M117 messages and layer/ETA accuracy), adds automatic ZMOD z-offset
persistence, and clears a cluster of crashes and small-screen layout issues.

### Added

- **Build your own filament catalog** — a new product-edit modal lets you add and edit filament products right on the printer. Your entries live in a user overlay that survives catalog regeneration, so updates never wipe them. (#1120)
- **Automatic ZMOD z-offset persistence** — on printers with a ZMOD probe, HelixScreen enables persistent z-offset on connect, so a calibrated offset survives restarts instead of resetting.
- **M117 messages on more screens** — status messages set via M117 now appear on the idle card and during pre-print heating, QGL and purge, not only mid-print.

### Fixed

- **Filament synced to OrcaSlicer with the wrong material** — a slot set to a specific type like ASA-GF came across in OrcaSlicer as "Generic PLA", putting PLA temperatures on glass-filled ASA. OrcaSlicer matches a slot to a filament preset by its material name alone and quietly falls back to PLA whenever the name isn't one it recognizes. HelixScreen now sends OrcaSlicer the closest name it *does* recognize (ASA-GF → ASA), so it picks a correct preset, while your printer's own screen keeps showing the precise name. A truly unknown material syncs with its color and temperatures but no material selected, rather than a wrong guess. Existing slots are repaired automatically the next time HelixScreen starts.
- **WiFi wizard crash during backend startup on Pi** (bundle WWZE4K9T) — `WifiBackendWpaSupplicant::stop()` captured a local `std::promise` by reference into a deferred `runInLoop` lambda, then returned and destroyed it when its wait timed out; the queued cleanup later called into freed memory and crashed at PC=0x0. The promise is now held in a `shared_ptr` captured by value, so it outlives the deferred completion regardless of whether `stop()` has already returned. This was the root cause behind the heap-corruption signature previously only mitigated in `WizardWifiStep::apply_ethernet_status`.
- **Filament mapping showed unused tools** — the print filament-mapping card listed every tool on the palette; it now shows only the tools a print actually uses, and no longer suppresses the material-mismatch warning for used-but-unresolved slots.
- **Garbage layer and ETA during print start** — fabricated layer/ETA figures are now gated on print duration, so they stop flashing nonsensical values during PRINT_START.
- **M117 handling during prints** — messages are cleared at print end rather than print start, are no longer clobbered by routine status deltas, and no longer leak into the phase-label line.
- **Camera stayed live across network changes** — the K2 webcam is registered with a relative URL, immune to DHCP lease changes and eth/wlan interface flips that previously stranded a baked-in IP.
- **Several crashes** — scroll-container repopulate use-after-free (#1123), a temperature-graph observer race during deferred delete (#1117), hot-reload rebuild of the print-file detail overlay, and duplicated side-effects when reconnecting to unchanged hardware (#1117).
- **AD5X filament loading** — corrected gcode-path TYPE=/HEX= extraction and made the loading-time budget swap-aware. (#1065)
- **Small-screen history filter** — the history filter row collapses on small screens (Snapmaker U1) with an active-filter funnel indicator. (#1116)
- **Pre-print options** — toggles gated on a macro that isn't installed are now hidden instead of doing nothing. (#1122)
- **Touch, fans, installer** — released touch slots keep their coordinates instead of zeroing; a fan's `.part` role falls back to the front-most named fan; installer architecture validation reads the ELF header without `dd`.

### Changed

- **Inputs scale on larger screens** — text fields, dropdowns and toggles use a responsive height so they aren't cramped on big displays; small screens keep the compact 48px size.
- **No more source comments on outgoing gcode** — HelixScreen no longer annotates M117/M118 and other commands with a "; from helixscreen" comment that some firmware echoed back into the console. (bundle A2TPH5V2)

## [0.99.97] - 2026-07-19

> **0.99.96 was withdrawn.** Its phantom-edge-tap fix broke touch input entirely on
> Goodix-based controllers (Creality K2 and likely others), leaving the screen
> unresponsive with SSH as the only recovery. That change is reverted here. If you
> installed 0.99.96 and lost touch, updating to 0.99.97 restores it.

### Added

- **Material variants grouped under their base material** — the filament picker now lists ASA, ASA-CF and ASA-GF under a single ASA heading instead of three unrelated top-level entries, with a chip showing which variant a row selects. Variants remain distinct materials with their own temperatures; only the grouping changed.
- **Missing filament types are selectable again** — ASA-GF, ABS-CF, PC-CF, PC-GF, PET-GF and PLA-GF existed in the material table but had no catalog entry, so they never appeared in the picker. Seventeen further types people actually buy (PLA+, ABS+, ASA+, PA6, PA12, PPA, TPU-95A, the decorative PLAs and others) were added alongside them.

### Fixed

- **CFS slot edits were silently discarded** — setting a slot's material on a Creality K2 wrote the colour to the box, and the next poll read that write back as a physical spool swap and deleted the edit. The material survived on screen until the next restart but never reached OrcaSlicer, which kept syncing stale firmware values.
- **CFS slots never showed empty** — the box keeps reporting a remaining length after a spool is pulled, so an emptied bay stayed "available" indefinitely. Presence now follows the live vendor signal, and the stale record is cleared when a spool is removed.
- **Spoolman configuration silently did nothing** — on stock Creality firmware the settings were written to a file Moonraker never reads, while the UI reported success. The target is now proven reachable before writing, changing an already-configured URL actually takes effect, and a configuration that cannot be written reports a clear error instead of claiming to have worked. Removing Spoolman no longer reports success while leaving it configured.
- **Spool list ordering** — the Spoolman spool picker sorts by most recent activity, so a newly added spool appears at the top instead of below every previously used one.
- **Preheat presets ignored reassigned materials** — reassigning a preset slot updated the filament panel but not the preheat widget, which kept showing and applying the original material's temperature.
- **Filament drying temperatures were too low for some materials** — PET, PET-CF, PET-GF, PA66, PA6-CF and the PPA family were offered a drying profile derived from a different member of their group. Generic PET also shipped with no bed temperature at all.
- **Labels for untracked spools** — a spool with no Spoolman entry printed a meaningless "#0" and a QR code pointing at a record that does not exist; both are now omitted. The Print Label button also appears as soon as a label printer is paired, rather than after reopening the panel.
- **Markdown rendering** — bold text uses a real bold font and headings are visually distinct.
- **Hot reload no longer crashes on mid-save files** — XML is validated before the old component is unregistered.

### Changed

- **Phantom edge tap fix reverted** — the 0.99.96 change required both touch axes to arrive for every new contact, but controllers that omit unchanged coordinates never satisfied that, dropping every touch. Holtek-based screens (BTT-HDMI5) may again see occasional edge taps until a safer fix ships.
- **Hot reload defaults to on for native builds.**

## [0.99.96] - 2026-07-19 [WITHDRAWN]

### Added

- **More timezone offsets** — the timezone picker now covers previously missing zones: Newfoundland (−3:30), Cape Verde (−1:00), Iran (+3:30), Afghanistan (+4:30), Pakistan (+5:00), Nepal (+5:45), Myanmar (+6:30), and New Caledonia (+11:00). Each is bundled so it resolves on devices without system tzdata; existing saved timezones are unaffected.

### Fixed

- **Touch recalibration Accept button** (prestonbrown/helixscreen#1029) — the capture surface no longer covers the Accept/Retry buttons in the verify step, so recalibration can actually be completed.
- **Phantom edge taps on some capacitive touchscreens** — Holtek-based controllers (such as the BTT-HDMI5) could intermittently emit a touch report carrying only one axis, landing as a phantom tap at a screen edge and navigating into the wrong menu. Single-axis glitch frames are now dropped until both X and Y coordinates arrive for a contact.
- **Snapmaker U1 filament load/unload state** — load state is now derived from the firmware's `channel_state` rather than the per-tool motion sensor, which lingers "present" after an unload on current firmware. Unloaded lanes no longer render as loaded or offer Unload, the filament path is drawn to match the actual state, and the load/unload step display covers all firmware feed states.

### Changed

- **In-app updater suppressed when it can't apply** — on installations where the app physically cannot self-update (a read-only rootfs or a permission-mismatched install), the updater no longer offers a download that would fail to install, showing an "Updates aren't available on this installation" notice instead. Firmware-managed installs opt out via the single `HELIX_DISABLE_AUTO_UPDATES` flag.

## [0.99.95] - 2026-07-18

### Added

- **Portrait-orientation layout foundation** (prestonbrown/helixscreen#1110) — min-dimension breakpoints and a bottom-navbar shell lay the groundwork for portrait displays; layouts now pick their sizing from the constrained screen axis (breakpoints, switch presets, navbar height) so they scale correctly on tall, narrow screens.
- **Home button warns when the axes aren't homed** — the jog center home button is tinted as a warning until the printer is homed.

### Fixed

- **Touch recalibration capture area** (prestonbrown/helixscreen#1029) — recalibrating from Settings captures across the full screen.
- **Temperature tool selector on Snapmaker U1** (prestonbrown/helixscreen#1114) — the tool selector is now touch-friendly on the U1.
- **AMS slot material label** (prestonbrown/helixscreen#1065) — the slot material label updates reactively from its own per-slot subject.
- **Log-level setting translation** — the Settings log-level option now translates correctly.
- **Property-based conditional hiding** — `$prop|ref` params resolve correctly so `hidden_if_prop_eq` works in XML layouts.

## [0.99.94] - 2026-07-17

### Added

- **Old filament purges cleanly on swap** — swapping filament holds the previous material's temperature while it purges, so the old filament clears before the new one loads.
- **Touch calibration diagnostics** (prestonbrown/helixscreen#943) — a DRM touch-range env override plus calibration span/raw-sample logging, and a warning + telemetry when the DRM coarse touch scale is skipped.

### Fixed

- **Touch stops responding after a display rotation** (prestonbrown/helixscreen#1112) — input devices are rebuilt after a DRM→fbdev rotation swap so touch keeps working.
- **Touch-calibration use-after-free crash** (prestonbrown/helixscreen#1102, prestonbrown/helixscreen#1112) — the calibration read path uses an owned calibration context instead of freed indev user_data, and the touch indev callback/user_data are cleared in the backend destructor.
- **Wizard recalibration retry** (prestonbrown/helixscreen#943) — a retry reverts the previous affine transform first, so it recalibrates from raw coordinates.
- **AD5X IFS false "hang" on load** (prestonbrown/helixscreen#1065) — a stalled load feed is driven by a sidebar watchdog so the "Working…" state clears instead of reading as a hang, and eject-settling / stale FFMInfo no longer mis-seat IFS lanes.
- **AMS material label refresh** (prestonbrown/helixscreen#1065) — a type-only filament change refreshes the slot label (each slot now has its own material subject).
- **Z-offset readback** — the live Z-offset is rounded instead of truncated.
- **Spool swatch artifacts** — the spool canvas draw buffer is cleared before use.
- **Chamber temperature labels** — chamber temp buffers are zero-initialized so labels don't bind garbage before the first reading.
- **XML layout parser hardening** — style nodes are zeroed before init, component-name copies are bounds-checked with a null-guarded lookup, and parser state is initialized for globals-component registration, preventing garbage bindings and crashes.
- **Redundant swap toast suppressed** — the filament-swap success toast is hidden when swap-preheat is only holding temperature.

## [0.99.93] - 2026-07-16

### Added

- **XML `<if>` / `<else>` structural conditionals** — layouts can build one branch, another, or nothing based on a subject or expression, reactively rebuilding when it changes; only the matching branch is created. Rounds out conditional support alongside `<repeat>` and `${…}`.
- **Integer expressions in `${…}`** — XML `${…}` composition can evaluate integer arithmetic and take numeric component params as operands, not just compose indexed subject names.
- **2D toolpath preview on the print detail view** — devices that render a 2D toolpath (rather than a 3D model) show that preview on the file detail view instead of the flat thumbnail.
- **Filament color routing follows the effective tool match** (Snapmaker U1) — on toolchangers, the gcode preview, spool swatches, and preflight checks color by each tool's effective filament match rather than assuming T0, and `SET_PRINT_EXTRUDER_MAP` is driven from that same match.

### Fixed

- **Power-loss recovery on AFC-modded Snapmaker U1** — the U1 resume offer now also reaches U1s running the AFC filament-system mod.
- **Filament mis-routing on empty or incompatible lanes** — a tool is never matched to an empty lane (which had shown a stale color), the material-blind lane fallback no longer grabs incompatible filament, and lane data is keyed by tool on toolchangers (ingesting foreign keys and migrating stale `laneN` entries).
- **AD5X IFS material and color on insert** (prestonbrown/helixscreen#1065) — physically inserting filament refreshes material and color for auto-tracked lanes.
- **Kobra S1 ACE detection** (prestonbrown/helixscreen#1107) — the Kobra S1 mainline-Python firmware fork's `ace_instance_N` objects are detected.
- **Render-thread crash on ARM64** (prestonbrown/helixscreen#1102) — a missing memory barrier in the software-render→main handoff could free a layer buffer still in use; the handoff is now barriered, and the subject-bound `<repeat>` observer lifetime is tied to its instance to close a related use-after-free.
- **Recycled panel layout** (prestonbrown/helixscreen#1109) — print-status cards and the active-spool row re-apply their layout and visibility when a recycled widget instance is reused, instead of showing a stale layout.
- **Decorative taps reach the button** (prestonbrown/helixscreen#1101) — taps on decorative children of a card or row route to the handler root instead of being swallowed.
- **Blocking-op g-code queueing** (prestonbrown/helixscreen#1108) — benign discretionary commands are queued during blocking operations rather than rejected.
- **Jog precision and toasts** (prestonbrown/helixscreen#1104) — jog axes are gated on an AxisMove epsilon and never emit scientific notation, a populated NOT_READY message beats the generic fallback, and duplicate RPC and gcode-stream toasts dedupe on Klipper's raw wording.
- **Keyboard dismiss keeps the overlay open** — tapping the backdrop to dismiss the on-screen keyboard no longer also closes the overlay behind it.
- **2D backdrop blur crash guard** — abort-modal GPU blur initialization is deferred with a crash-loop guard.

### Changed

- **Pre-print toggles hidden without HelixPrint** — pre-print toggles that require the HelixPrint plugin are hidden when the plugin isn't installed.

## [0.99.92] - 2026-07-15

### Added

- **Jog move coalescing** — rapid jog taps merge into a single pending move instead of queueing one g-code per tap, so the toolhead keeps up with your finger; your own jogs no longer trip the "printer busy" guard, and rapid identical toasts refresh in place instead of stacking.
- **Snapmaker U1 power-loss recovery** — after a power loss, HelixScreen offers to resume the interrupted print at connect time; a declined offer re-arms when it becomes relevant again.
- **Anycubic Kobra S1 mainline fork** (prestonbrown/helixscreen#1069) — ACE filament systems on the Kobra S1's mainline-Python firmware fork work via a REST bridge.
- **XML expressions** — layouts can derive subjects from expressions (`<subject_expr>`) and use inline `cond=` conditions on `bind_flag_if` / `bind_state_if` / `bind_style_if`.
- **XML `<repeat>` looping** — `<repeat count>` expands a fragment with a `$i` index (fixed or subject-bound, reactively rebuilding); `${i}` composes indexed subject names for self-wiring repeated widgets.
- **PAXX firmware-managed installs (Snapmaker U1)** — installs bundled by the PAXX Extended Firmware are supported (firmware owns updates), with a `HELIX_DISABLE_AUTO_UPDATES` opt-out.

### Fixed

- **Snapmaker U1 remote screen going blank** — the remote screen ("gui" webcam) no longer goes black a few minutes after boot: the early-boot splash is retired at UI handoff instead of lingering and clearing the shared framebuffer, and the UI paints a full frame at takeover. The mirror also handles 16-bit framebuffers.
- **Installer upgrades now restart into the new version** (prestonbrown/helixscreen#1106) — installing over a running HelixScreen left the old binary running while reporting success; the installer now restarts when an instance is already up.
- **Print status auto-opens for an active job** (prestonbrown/helixscreen#1099) — connecting while a print is already running opens the print status panel.
- **Quieter no-audio devices** — a missing ALSA device logs a warning instead of an error.
- **Snapmaker U1 uninstall leftover** — uninstall removes the `/oem/.debug` marker the installer created.

### Changed

- **Snapmaker U1 remote-screen setup docs** — enabling must go through the firmware settings web UI (a hand cfg edit doesn't register the "gui" webcam); setup docs corrected.
- **Inline XML text translation** — inline element text in XML layouts participates in translation extraction and runtime language switching.

## [0.99.91] - 2026-07-13

### Added

- **Native QIDI 3MF print previews** (prestonbrown/helixscreen#1092) — print previews are read directly from QIDI's native 3MF files, selecting the newest-modified shadow gcode.

### Fixed

- **Fan speed reporting** (prestonbrown/helixscreen#1096) — reported fan speed is normalized by `max_power` so it matches Mainsail.
- **Timelapse pre-print toggle** (prestonbrown/helixscreen#1094) — the pre-print timelapse toggle seeds its default from the global enabled setting, dropping a redundant print-start write.
- **AMS step connectors** (bundle 77TDH9N6) — step connector lines are recomputed on reflow so they always draw.
- **AMS long-macro toast** (bundle 77TDH9N6) — an advisory RPC-timeout toast no longer fires during long-running AMS macros.

## [0.99.90] - 2026-07-12

### Added

- **Redesigned AMS spool editor** — the slot editor is now a single in-place flow: an overview spool card with an identity chip and Spoolman mark, a "Change filament" row, and in-place views for filament details, color (preset grid + custom hue), and spool details, replacing the old stacked dropdown/modal editor. Choosing a branded filament works even when Spoolman is absent, and scanning a QR repopulates the live editor instead of closing it.
- **Sliced vs. loaded preview colors** (prestonbrown/helixscreen#959) — the gcode preview can recolor to your loaded AMS slot colors, updating live as slots change, with a toggle in print settings to prefer the sliced colors instead.
- **Per-unit dryer environment** — multi-box AMS systems (QIDI) report drying state per box, with environment indicators and the dryer overlay routed to the unit you open.
- **LED quick-toggle** — a LED on/off toggle in the Calibration & Tools grid.
- **USB input device blacklist** (prestonbrown/helixscreen#1095) — a setting to ignore specific USB HID devices for input.
- **Temperature history on connect** (prestonbrown/helixscreen#944) — temperature graphs seed from Moonraker's temperature store at connect, so recent history is shown immediately.

### Fixed

- **Print-start crash on the helix_print plugin** — Klipper calls route through `klippy_apis` with a client fallback, fixing a crash at print start.
- **Motion refused until ready** — jog and home g-code is refused until Klipper is READY, and the jog pad dims and disables when the printer isn't ready.
- **Timelapse capability** (prestonbrown/helixscreen#1094) — a hardware-timelapse batch no longer clobbers a component-detected timelapse capability.
- **AMS spool rendering and saves** — per-slot fill levels render from state everywhere (the overview had shown every spool full), unknown-weight spools render half-full rather than full, a Cancel on the identity confirm aborts the save, switching a linked spool is treated as a relink instead of prompting to overwrite, and managed controls stay hidden without Spoolman.
- **Panel RPC dedupe** (prestonbrown/helixscreen#910, prestonbrown/helixscreen#912) — an in-flight guard dedupes panel RPCs with self-heal, and the print-select file provider drops stale responses via a per-request generation guard.
- **Writable temp paths on locked-down devices** — runtime `/tmp` writes and the splash-status path resolve to a writable directory under `ProtectSystem=strict` sandboxes.
- **Wayland screenshots** — screenshot capture uses the native Wayland SDL driver on Wayland sessions.
- **SAVE_CONFIG restart** — a SAVE_CONFIG-triggered restart is treated as transient rather than surfaced as an error.
- **Small-device memory** — the AMS edit overlay is destroyed on close to reclaim its widget tree.

### Changed

- **Empty XML binding subjects** — an empty `bind_flag`/`bind_state` subject is treated as a no-op, fixing a phantom header button.
- **Translation tooling** — `translation-sync` inserts keys surgically instead of rewriting locale files, honors `do not translate` / `universal` suppression markers, and dead keys were pruned.
- **Filament product ordering** — plain material ranks first and support materials last in the product list.

## [0.99.89] - 2026-07-12

### Added

- **Snapmaker U1 remote screen** (#1031) — the U1 display can be mirrored over the remote-screen path, with RGB565→BGRA conversion and a direct framebuffer blit.
- **More humidity sensors** (prestonbrown/helixscreen#1090) — AHT10, AHT20, and SHT3X humidity sensors are now surfaced in the sensors overlay.

### Fixed

- **In-app updates no longer fail on read-only-`/tmp` devices** — self-update aborted with a "Read-only file system" error while creating its temp directory on devices whose service sandbox makes `/tmp` read-only (e.g. Creality Sonic Pad, OrangePi Zero3). The updater now stages the download and extraction in a writable location beside the install directory. This is why updating to 0.99.88 failed on the Sonic Pad.
- **AMS load no longer freezes in "Heating"** (prestonbrown/helixscreen#1065) — when a filament-system backend rejects a load or unload, the failure is surfaced in the sidebar instead of leaving the UI stuck.
- **AD5X IFS reliability** (prestonbrown/helixscreen#1065) — lanes load directly via `INSERT_PRUTOK_IFS` (the firmware self-swaps and heats), a stalled load feed shows an indeterminate "Working…" state, seated-lane authority is gated on the head-switch sensor, and the material label reconciles across empty-lane transitions.
- **Current layer from Z height** — when the slicer omits layer metadata, the current layer number is derived from Z height (shown without the previous "~" prefix).
- **Safer during homing** — discretionary g-code is refused while the printer is homing or probing.

### Changed

- **Sovol SV08 detection** — the base Sovol SV08 profile gains a ~350 mm build-volume default.

## [0.99.88] - 2026-07-08

### Added

- **Offline branded filament catalog picker** — long-pressing a filament preset opens a vendor → product picker backed by an offline OrcaSlicer-derived catalog, applying that product's branded print temperatures. Branded choices persist across restarts.
- **Grouped settings cards** — settings overlays (Display & Sound, fans, sensors, and the rest of the settings tree) are reorganized into titled group cards, some with a count badge, for a cleaner, more scannable layout.
- **On-screen scroll buttons** — a new "Scroll Buttons" toggle in Display settings adds chevron controls in a reserved gutter for paging long screens without a swipe, animated when Animations are enabled.
- **Multi-color filament swatches** — filament-mapper surfaces and the spool picker show multi-color spools as diagonal split swatches.
- **Resonance-calibration memory warning** — on hosts with under ~200 MB free, HelixScreen warns before starting input-shaper resonance calibration.

### Fixed

- **AD5X IFS seated-lane tracking** (prestonbrown/helixscreen#1065) — the seated lane is derived from the native channel sensor rather than dialog state, ejected lanes clear so the context menu refreshes, the loaded lane resolves via the current slot on single-tool systems, and the operation step tracker highlights the right phase.
- **AD5X material type refresh** (prestonbrown/helixscreen#981) — a non-locked override material is refreshed when the firmware reports a filament type change.
- **CC1 resonance calibration no longer thrashes memory** — the COSMOS gui-switcher can quiesce HelixScreen for resonance calibration on low-memory CC1 hosts.
- **Safer during printing** — app-initiated homing and filament load/unload operations are blocked while a print is active.
- **3D preview on faulting GPUs** (prestonbrown/helixscreen#966) — the 3D model preview is disabled on GPUs that fault inside their driver, avoiding a crash.
- **Qidi Max 4 detection and version display** (prestonbrown/helixscreen#1068) — the build-volume window is centered on the 390×390 bed, and a Moonraker "?" version reports as "Unknown".
- **Single-extruder printing not blocked** — the pre-print check no longer blocks a print on single-extruder printers with no AMS.
- **Wizard Moonraker host prefill** — the setup wizard pre-fills 127.0.0.1 when the stored Moonraker host is empty.
- **Spoolman spool parsing** (prestonbrown/helixscreen#1087) — null numeric fields in a spool record are tolerated instead of failing the parse.
- **Software-rotated panel animations** (prestonbrown/helixscreen#986) — animations default off on software-rotated displays to keep them responsive.

## [0.99.87] - 2026-07-03

### Added

- **Reassign a preset's filament type by long-press** — long-pressing a filament preset button opens an anchored material picker to change its assigned type, and the choice persists per button.
- **Unified filament catalog** — material data (types, temperatures, densities) now comes from a single OrcaSlicer-derived catalog, with PET-CF and PET-GF added and material families sorted sensibly. A user overlay lets edited or custom materials override and extend the catalog.
- **Qidi Max 4 support** — auto-detection and a matching preset for the Qidi Max 4, including its `MULTI_COLOR_BOX_UNLOAD` box-eject dialect.
- **Touch-calibration press marker** (prestonbrown/helixscreen#1082) — the alignment phase shows a persistent dot where you last pressed, so it's clear each point registered.
- **On-demand RFID refresh** (prestonbrown/helixscreen#1077) — the CFS "Refresh RFID" action re-probes tags via `BOX_INFO_REFRESH`.

### Fixed

- **A Qidi Max 4 is no longer confused with a large Creality printer** — printer auto-detection now treats kinematics as a hard rule (every Qidi is corexy), so a cartesian machine such as an Ender 5 Max is never matched to a Qidi Max 4, and an Ender 5 Max is recognized from an `ender5-max` hostname. Detection had been overweighting build volume.
- **CFS slot presence reflects the physical spool** (prestonbrown/helixscreen#1077) — a slot shows as loaded based on the vendor and remaining-length signals rather than a latched RFID color, so a removed spool no longer lingers as present.
- **WiFi connection state stays in sync** (prestonbrown/helixscreen#1059) — CONNECTED/DISCONNECTED events fire on status-poll transitions, with the transition handling hardened, so the WiFi status and icon no longer go stale.
- **Taps during a scroll no longer fire a button** (prestonbrown/helixscreen#1074) — press-lock is released while scrolling, on both themed and XML switches.
- **Soft-restart teardown crash fixed** (prestonbrown/helixscreen#1073) — the app layout is detached safely during a soft restart.
- **Performance graph no longer risks a use-after-free** — the per-MCU name observer holds a lifetime token.

## [0.99.86] - 2026-07-01

### Fixed

- **Spoolman spool picker** (prestonbrown/helixscreen#1071) — "Select Spool" now opens the Spoolman spool picker directly, with spools ordered by most-recently-used and then creation date.
- **Chamber sensor auto-detection** — air-quality sensors are demoted so a real temperature sensor is preferred as the chamber source during auto-detection.
- **WiFi connection stability** — cross-thread WiFi callback and flag state is now mutex-guarded to prevent races during connect and scan.
- **Teardown crash fixes** — keyboard reset and long-press overlay deletion are deferred out of the input/update batch, modal entrance animations cancel cleanly when a modal closes mid-animation, and secondary-fan control tokens are held for the control's lifetime — closing several crash windows.

## [0.99.85] - 2026-06-30

### Added

- **Automatic hardware role healing** (prestonbrown/helixscreen#1062) — fan and heater roles are validated against live hardware and re-resolved when the config changes; stale or unresolved roles route to a targeted wizard reconfiguration that reapplies without resetting your completed setup. Reconfiguration is idle-gated and no longer nags about unconfigured roles.
- **Editable Happy Hare endless spool and per-unit drying** — endless-spool groups are now editable, each MMU/EMU unit reports its own drying environment, and heater-less units still surface environment readings.
- **Input-shaper chart guidance for remote users** — when the frequency-response chart can't be shown because calibration ran on the printer, HelixScreen explains that the chart needs an on-printer install instead of showing a blank chart.

### Fixed

- **AD5X loaded-lane accuracy on native Z-Mod** (prestonbrown/helixscreen#1065) — head-loaded state is derived from the native Z-Mod sensors and cleared on a commanded unload even when filament parks in the lane; Load is gated on toolhead state; the seated lane persists across a power cycle; and a dedicated purging timeout with motion reset avoids stuck states.
- **AD5X + Spoolman spool integrity** (prestonbrown/helixscreen#1071) — HelixScreen no longer auto-writes the Spoolman active spool, keeps the spool link when a lane goes empty (matching AFC/Happy Hare), only creates a spool when you actually edit filament fields, and confirms before overwriting a materially different linked spool. An emptied lane's fill bar now reads empty instead of 75%.
- **AMS material label refresh** (prestonbrown/helixscreen#981) — a slot's material label is re-read when the panel reactivates, and the dryer environment overlay live-refreshes instead of freezing when opened.
- **Input-shaper calibration chart** — an unreadable calibration CSV surfaces a clear message instead of a blank chart, and the service keeps `PrivateTmp` off so Klipper's `/tmp` output stays readable.
- **Stray taps no longer dismiss a panel** (prestonbrown/helixscreen#1066) — in-bounds taps on an overlay root are absorbed instead of closing the panel.
- **Touch calibration restore** (prestonbrown/helixscreen#943) — dismissing the recalibrate control in Settings restores the previous affine calibration.
- **French fan status text** (prestonbrown/helixscreen#1073) — corrects a French fan format string, with a new format-specifier parity guard to catch similar mismatches.
- **Home tile layout** — a tile layout computed while Klipper is not yet READY is no longer persisted.

## [0.99.84] - 2026-06-24

### Added

- **Full UI translation across 9 languages** — translations completed to 100% coverage, now covering dropdown options, property-default modals, text-input placeholders, wizard screens, and backend error/status messages.
- **Customizable macro buttons** — a tabbed "Customize Macro Button" modal configures a favorite macro's appearance and options, including a "run without parameter prompt" toggle to fire a macro immediately. It opens from the favorite widget and replaces the old picker.

### Fixed

- **AD5X loaded-lane reporting** — the active slot is derived from the seated channel on native ZMOD firmware, so the loaded lane is reported correctly.
- **3D render refreshes on a new print** — the print-status view tracks thumbnail and gcode markers separately so the preview reloads for each new print.
- **Quieter AD5X color polling** — background zcolor poll timeouts no longer raise a toast.
- **Screen stays asleep on power-off** (prestonbrown/helixscreen#1049) — LVGL flushes are suppressed during DPMS power-off so the panel doesn't wake back up.

### Changed

- **Consistent modal headers** — six modals adopt a shared header with a uniform close button and responsive padding.

## [0.99.82] - 2026-06-22

### Fixed

- **AD5X IFS reliability** (prestonbrown/helixscreen#981) — a seated channel unloads via the toolhead instead of attempting a cold eject; ejected channels refresh correctly; filament color/type authority is honored; and stale slot sentinels are cleared.
- **CFS slot presence** — a slot is shown as loaded based on physical signals plus your assignment rather than a latched RFID read, so a removed spool no longer lingers as present.
- **QIDI Q2 firmware support** (prestonbrown/helixscreen#1047) — works with the QIDI Q2 `01.01.02` firmware refactor.
- **WiFi status accuracy** (prestonbrown/helixscreen#1059) — when the backend can't reach `wpa_supplicant`, a system-managed link is shown as connected instead of being painted as a hard failure.
- **AD5X memory reporting** — corrects the AD5X memory tier and gates low-memory growth warnings on absolute health, reducing false warnings.
- **FlashForge file browsing** — root-relative directory names are normalized so directory listings resolve correctly.
- **Performance overlay crash** (prestonbrown/helixscreen#1061) — fixes a render crash on 32-bit devices caused by an unresolved-layout draw.
- **Installer release matching** — a bare version is normalized to a `v`-prefixed tag when resolving a release download.
- **Modal teardown crash** — closing a modal whose exit animation is still in flight no longer risks a crash when the screen is torn down first.

## [0.99.81] - 2026-06-18

### Added

- **Status-driven error recovery** — AMS/IFS/QIDI errors detected from live printer status (not just gcode error lines) now open the recovery dialog automatically, so a blocked or jammed lane surfaces an actionable prompt even when the firmware emits no error text.
- **Happy Hare toolchange narration** — multi-material toolchanges on Happy Hare/MMU show a live phase description and offer recovery actions when a toolchange error occurs.
- **CFS empty-spool runout indicator** — a spool running empty is surfaced as a paused-gated runout indicator, with a vendor/brand fallback for the box's material.

### Fixed

- **K2 camera survives upgrades** — existing installs migrate to the corrected camera service type and init script on upgrade; the webcam registers as `mjpegstreamer-adaptive` so HelixScreen and Fluidd can display it; the stock `cam_app` is released from `/dev/video0` on each start; and a conflicting community K2-Camera-main mod is detected and disabled with a warning.
- **AD5X IFS load/unload reliability** (prestonbrown/helixscreen#981) — load and unload finalize on the macro completion ack instead of waiting out a 90-second timeout (no more stuck-on-Purging); channel presence is driven by `IFS_STATUS` ports so emptied channels no longer reappear; a seated-channel unload routes to a toolhead cut instead of a cold eject; and a user color-menu prompt no longer silences live status.
- **U1 pre-print accuracy** — lane-truth runout detection, a scoped filament badge, a real first-layer hand-off, a remap toast, and a richer phase display; plus a toast during `AUTO_FEEDING` resume so the ~86-second refeed isn't a silent hang (prestonbrown/helixscreen#991).
- **Self-update on tight-space devices** — the installer relocates the old install off a cramped partition before updating.
- **Pre-print collector** no longer restarts on mid-print error recovery (prestonbrown/helixscreen#1042).

### Changed

- **Happy Hare filament metadata** — lane data now emits `vendor_name`/`name` aliases for OrcaSlicer schema parity.

## [0.99.80] - 2026-06-18

A large release centered on a new error & recovery center, live per-slot filament state across AMS backends, a pre-flight filament-validation gate before printing, on-screen native filament remapping for the Snapmaker U1, QIDI Box stock-firmware filament control, and print-failure (spaghetti) detection on the Snapmaker U1 — plus temperature-graph rendering fixes and WiFi recovery hardening.

### Added

- **Error & recovery center** — AFC/AMS jams and other errors surface as an actionable recovery dialog with context-aware buttons instead of raw error text; errors are routed and styled by severity, and the notification badge reflects the worst unread severity. Toolchange progress shows as a backend-driven step bar.
- **Live per-slot filament state** — the filament panel binds reactively to live per-slot color, material, and load state across AMS backends, with a two-tone chip showing the gcode-intended color over the actually-loaded color and truthful tool numbers.
- **Pre-flight filament check before printing** — selecting a file runs a validation that detects empty slots and filament/color mismatches across all AMS backends, labels chips from the real gcode tools, and can block the print or open native remapping from the gate.
- **Snapmaker U1 on-screen filament remapping** — remap virtual tools to physical heads from the touchscreen using the firmware's native `print_task_config` commands (sent before `PRINT_START`), with a four-phase load/unload step bar (Home / Select / Heat / Feed-Retract) and a shared remap UI used by all backends.
- **U1 / ACE gcode tool remapper** — a comprehensive tool remapper applied via the HelixPrint plugin, with a per-backend remap strategy.
- **Print-failure (spaghetti) detection on Snapmaker U1** — a detection framework with a U1 stock-firmware source that flags failures on pause, a per-source enable/policy setting, and a Resume / Abort / Reduce Sensitivity response dialog.
- **QIDI Box stock-firmware filament control** (prestonbrown/helixscreen#1041) — real load/unload and dryer indication on stock firmware, per-lane eject via `FORCE_MOVE` (gated on `force_move`, with a config hint when unavailable), configurable eject distance/velocity, dryer remember-last, and firmware-capability detection.
- **Multi-extruder nozzle temperature widget** — a responsive widget showing each nozzle's temperature, collapsing multiplexed AFC/MMU lanes to a single nozzle row.
- **Real screen power-off on devices without backlight control** (prestonbrown/helixscreen#1049) — in-process DRM connector DPMS power-off and a reachable screensaver on no-backlight devices.
- **Snapmaker U1 stock firmware support** — HelixScreen autostarts on stock U1 firmware (via the input-event daemon) with per-head seated-filament truth read from `print_task_config`.
- **Automatic LED control** — LED state is wired into the printer lifecycle, and reprints route through the start controller so the U1 native pre-send applies.

### Fixed

- **Temperature graph rendering** (prestonbrown/helixscreen#979) — the gradient fill now renders under LVGL 9.5 (it was invisible), live curves no longer freeze or go gappy, per-extruder lines stay correct, a drop-to-0 history filter removes spurious dips, and the near-curve gradient is more visible on the U1.
- **WiFi recovers from a failed bringup** (prestonbrown/helixscreen#1036) instead of dying permanently, detects the `wpa_supplicant` control interface from its `-c`/`-C` arguments, and debounces a transient `AUTH_FAILED` before `CONNECTED` (prestonbrown/helixscreen#1050).
- **Touch input** (prestonbrown/helixscreen#943, prestonbrown/helixscreen#986) — multi-touch digitizers are scaled correctly and the recalibrate control stays visible on the DRM backend.
- **No false runout alarms** — empty or never-loaded lanes no longer raise runout or AMS error alarms, and the idle runout modal is suppressed during AMS load/unload.
- **QIDI Box idle load homes first** (prestonbrown/helixscreen#1041), and **QIDI Q2 is no longer misdetected as an Artillery M1** (prestonbrown/helixscreen#1027) — `algo_app.service` is no longer treated as M1-exclusive.
- **Per-printer layout reloads on switch.**
- **Thumbnail/preview robustness** — truncated or non-PNG thumbnail data is rejected before decoding, metadata is fetched on programmatic file selection, and gcode load no longer deadlocks on the display view-mode or leaves a blank preview on re-entry.
- **Fan control overlay** re-registers before each push; the **error-recovery dialog** is width-responsive and fits 4+ buttons on one row (prestonbrown/helixscreen#1043).
- **Ultrawide home layout** uses a responsive grid with a slimmer navbar.
- **Generic LED strips default to white-only** until the Klipper config proves RGB, so the color picker only appears on RGB-capable strips.

## [0.99.79] - 2026-06-14

### Added

- **K2 camera works in HelixScreen and Fluidd** — the installer replaces the K2's proprietary WebRTC camera (which neither HelixScreen nor Fluidd can display) with a bundled MJPEG streamer, registers it in Moonraker, and cleanly restores the stock camera on uninstall.
- **AD5X per-lane filament eject** — the IFS filament system can eject each material lane individually using its configured tube length, instead of only unloading the active channel.
- **QIDI Box write support is always on** — tool mapping and box controls (including the dryer) now work without a feature flag, and box humidity is shown in the dryer overlay.

### Fixed

- **Installer will not wipe a mounted partition** — temp-directory cleanup refuses to `rm -rf` a mountpoint or the filesystem root, and a missing `realpath` on BusyBox firmware no longer aborts the install.
- **Fewer spurious Moonraker warnings on K1/K2/Snapmaker U1** — the "Unable to initialize System Update Provider for distribution: buildroot" warning is suppressed on buildroot firmware without affecting HelixScreen's own updater.
- **No crash adding a third printer** — a use-after-free when re-running the setup wizard for an additional printer is fixed, and per-step skips for AMS/LED/input-shaper steps are honored.
- **AD5X IFS filament presence is accurate** (prestonbrown/helixscreen#981) — channel presence is driven solely by the firmware's color query so emptied channels no longer reappear, external lane unloads and color changes are reflected, and older firmware keeps a working presence fallback.
- **Touch calibration auto-accepts reliably** (prestonbrown/helixscreen#1029) — the wizard accepts a completed calibration when the verify step is shown, not only on the press that triggered it.
- **QIDI Box dryer and mapping fixes** (prestonbrown/helixscreen#1022, prestonbrown/helixscreen#1030) — box humidity appears in the dryer overlay, the tool-mapping card now surfaces, and drying-capable backends without a temperature sensor still show an environment indicator.
- **AMS dryer panel hidden without a humidity reading** (prestonbrown/helixscreen#1022) — the dryer comfort panel no longer shows when there is no live humidity value.
- **Model presets no longer overwrite your Moonraker host** — applying a printer-model preset keeps the IP/host you entered instead of resetting it.
- **Webcam URLs preserve their port**, and HTML/iframe snapshot URLs are skipped so a real image is shown.
- **Debug bundles include fresh logs** instead of a stale leftover log file.
- **AD5M presets declare the E1 sensor** (role=none) in the ForgeX/ZMOD presets.

## [0.99.78] - 2026-06-14

### Added

- **Per-unit filament drying for Happy Hare multi-MMU / EMU** (prestonbrown/helixscreen#1022) — each MMU or EMU unit now reports its own box humidity and temperature, with the dryer heater and a minute-based drying duration controlled per unit.
- **AFC "Unloads After Print" toggle** — a per-printer setting controlling whether AFC unloads filament when a print finishes.
- **Adaptive bed mesh and load-cell probe support** — print start can request an adaptive bed mesh, load-cell probes are supported, and the active bed-mesh profile is highlighted.

### Fixed

- **CC1 survives COSMOS firmware upgrades** — a three-sibling gui-switcher hijack plus boot-time self-heal keeps HelixScreen installed across a COSMOS firmware update.
- **Several rare navigation crashes fixed** — overlay pushes are guarded against a target freed before the deferred push drains and against an empty panel stack, and deleted widgets are scrubbed from the panel stack to close a use-after-free (bundle ZW6ATWSL).
- **Touch calibration reliability** (prestonbrown/helixscreen#943, prestonbrown/helixscreen#986) — capture-on-press / commit-on-release debouncing is now on by default, affine calibration re-enables when entering the verify step, the manual-recalibration button stays available, and the manual calibration row is hidden on devices that don't need it.
- **AD5X IFS respects external color changes** (prestonbrown/helixscreen#981) — an external `CHANGE_ZCOLOR` now releases a stale locked override so the firmware's color and type win, and unload ejects non-active lanes instead of yanking the loaded channel (with a longer action-prompt timeout).
- **CFS no longer shows a false pre-print runout warning** — CFS unloads the toolhead after a print, which was being misread as a runout.
- **QIDI Box dryer temperature shown for Happy Hare**, and Happy Hare drying presets are clamped to the heater's limits.
- **Home screen no longer stalls on the printer image** (prestonbrown/helixscreen#1025) — the printer-image refresh is deferred out of panel attach/activation.
- **Home edit mode ignores widget taps** (prestonbrown/helixscreen#1003) — tapping a widget while rearranging the home screen no longer launches it.
- **On-machine updates work on static-glibc printers** (AD5M and similar) — the libhv DNS resolver wiring now reaches the shipped binary.
- **QIDI Q2 no longer misdetected as an Artillery M1** (prestonbrown/helixscreen#1027) — installer fingerprinting now tells the two printers apart.
- **A second printer is no longer misdetected as Unknown** — the detector restores stripped heuristics before running, so adding a subsequent printer fingerprints correctly instead of falling back to Unknown.

### Changed

- **"AMS" / "Multi-Material" is now "Multi-Filament System"** throughout the UI.
- **Removed the broken load-cell calibration screen** — the mock printer is authoritative for probe behavior; logging docs updated.
- **Filament Environment overlay** now has a responsive landscape layout, and the wide multi-cell spool view gets a card background.
- **Setup wizard skips hardware steps a preset already covers** when you add a subsequent printer, so configuring a second printer is faster.

## [0.99.77] - 2026-06-13

A large release. Headlines: filament-drying control for the QIDI Box and Happy Hare / MMU systems, a new chamber-heating system with an on-home chamber temperature widget and a centralized temperature controller, a redesigned AMS spool widget with on-home print controls, and a deep round of Snapmaker U1 runout/pause-recovery fixes — plus new printer support (Anycubic Kobra/Rinkhals with native ACE, Creality Hi, Creality K2 Pro).

### Added

- **Filament drying control for QIDI Box and Happy Hare / MMU** (prestonbrown/helixscreen#1019) — start and stop drying from the UI with a live countdown, the box/heater target and current temperature, and capability advertisement so the control only appears where it works. QIDI stock routes through `ENABLE_BOX_DRY` / `box_extras`; Happy Hare routes through the MMU heater gcode with the heater name and max temp queried from the Klipper config.
- **Chamber heating support** — a chamber temperature widget on the home screen showing the effective setpoint (heater **or** fan) with heating / maintaining / off states. Sends route through `M141` on printers that define it, and the keypad and presets clamp to the configured chamber `max_temp`.
- **Anycubic Kobra support (Rinkhals firmware)** — adds the Anycubic Kobra 2 Pro, Kobra 3, Kobra 3 V2, Kobra 3 Max, Kobra S1, and Kobra S1 Max to the printer database, fingerprinted on confirmed GoKlipper objects so they auto-detect under [Rinkhals](https://github.com/jbatonnet/Rinkhals). Kobra 3 was corrected from CoreXY to Cartesian. Native Anycubic ACE is now supported for real: the firmware registers the multi-material hub as the `filament_hub` Klipper object (not `ace`), and the ACE backend now detects, queries, and parses `filament_hub` — including dryer status/target/duration and the loaded slot — with the old `ace` object kept as a dormant fallback. Untested on our hardware (we own no Kobra); presets are conservative and gaps are documented.
- **Preliminary Creality Hi support** — adds the Creality Hi (260×260×300 Cartesian bedslinger, Prtouch V3, dual-Z, optional CFS) to the printer database with a stock-config preset and product image. Auto-detection keys on the Hi's real Klipper config (Cartesian kinematics — unique among supported Creality printers, which are otherwise CoreXY). A CFS dialect fix routes a Hi with CFS to the K1-style `BOX_*` macros it actually ships, not the K2 `CR_BOX_*` primitives. Preliminary and untested — we own no Hi hardware.
- **Creality K2 Pro support** — adds the K2 Pro (300 mm build volume) to the printer database. It reuses the existing K2 preset, so it inherits CFS, the active chamber heater, and the K2 macro set; auto-detection distinguishes it from the K2 Plus by build volume and hostname.
- **On-home print controls** — a 2×1 control-buttons widget (pause / resume / stop) with optimistic pending state, usable directly from the home screen.
- **Redesigned AMS home widget** — a wide spool view (up to 4× width) grouped by the active spool, with a lane badge marking it, content-sized cells, and rebuilds skipped when the render is unchanged.
- **Per-sink log patterns with a thread id** on every log line, for clearer diagnosis of cross-thread timing issues.

### Fixed

- **Snapmaker U1 runout & pause recovery overhaul** (prestonbrown/helixscreen#991) — a minimal, port-gated runout dialog with a unified Resume; recovery driven through `AUTO_FEEDING`; the resume classifier wired to `print_stats.exception` with a heating-aware backstop; per-slot unload offered for every loaded toolhead; and corrected pre-print phase and ETA reporting.
- **3D viewer falls back to 2D on fatal GLES draw errors** (prestonbrown/helixscreen#966) instead of crashing.
- **Touch calibration** — calibration session state now resets on every overlay show (prestonbrown/helixscreen#943), and a released finger correctly ends a pinch gesture.
- **Home edit mode now requires a deliberate stationary hold** (prestonbrown/helixscreen#1003), no longer triggering on incidental drags.
- **gcode preview rendering** — solid ghost surfaces render visibly while sparse infill stays see-through, the 2D ghost preview is translucent rather than near-black, and a blank-panel / stuck layer-slider regression is fixed. Cura metadata after header `M73` markers now reads correctly (prestonbrown/helixscreen#942): the header scan stops only at the first motion command, restoring the slicer name, layer height, and bounding-box extents for Cura-sliced files.
- **Input-shaper firmware-halt faults are surfaced clearly** in the calibration wizard (prestonbrown/helixscreen#1021).
- **Clearer K2 fan names** — the K2 fan list no longer shows two indistinguishable "Chamber Fan" entries. Fans now carry function-based labels (Part Cooling, Auxiliary, Chamber Heater Fan, Chamber Circulation), and the auxiliary fan's role mapping was corrected.
- **AD5X IFS Unload no longer homes and stalls** (bundle 7AC4SDEX) — the v0.99.76 unload could still home and then do nothing when no filament was seated at the nozzle. Unload now dispatches the firmware's own toolhead-unload sequence when filament is at the head, and pulls the filament back from the lane with a cold eject when it isn't — instead of issuing a command the firmware treats as a no-op.
- **Moonraker Creality key-error envelopes are decoded** into readable messages, and caller-handled error toasts are deduped.
- **Snapmaker U1 boot reliability** — autostart on Paxx 1.4 (via `S99fb-http`), boot-time SIGTERM self-heal, WiFi decoupled from the helix lifetime, and `helixscreen.init` guaranteed executable.
- **Empty `bind_value` no longer cross-talks** — arcs declared with an empty `bind_value` no longer share the global noop subject (which made unrelated arcs move together).
- **Cross-compiling from git worktrees** now works.

### Changed

- **Centralized temperature control** — nozzle, bed, and chamber sends now route through a single `TemperatureController` with consistent "heater not found" toasts and unified preset and limit handling.

## [0.99.76] - 2026-06-11

### Added

- **Live load/unload phase progress on AD5X IFS** (bundle KLQGENXL) — the IFS load and unload flows now show the live phase instead of a static spinner.

### Fixed

- **Several rare crashes fixed** — dashboard grid-layout heap walk-offs during widget rebuild (prestonbrown/helixscreen#983) and tool-switcher pill layout (prestonbrown/helixscreen#1006), an out-of-range grid cell walk-off (bundle P234RYCL), a crash when opening the AMS / Power / Timelapse overlays (bundle 29QTNSYL), and a sweep of background-thread use-after-free guards.
- **Print thumbnails retry on failure** — a thumbnail that fails to load now retries with backoff and re-triggers on Moonraker events instead of staying blank.
- **AD5X IFS load/unload settles reliably** — load and unload finalize to idle on firmware (`GET_ZCOLOR`) confirmation rather than waiting out a timeout.
- **AD5X IFS Unload actually runs now** — the toolhead Unload was dispatching a firmware no-op, so the printer homed and then did nothing. It now sends the correct ZMOD command, which heats only when filament is present at the nozzle and then retracts. Selecting an idle lane no longer heats the hotend — those route to a cold eject.

## [0.99.75] - 2026-06-10

### Added

- **Redesigned filament path visualization** — the AMS filament path canvas (detail and overview) is rebuilt on a new geometry engine with arc-fillet tube routing, angled non-overlapping merge fans, slimmer active lanes, and hub gear placement, for a cleaner, more legible diagram across linear, hub, parallel, and mixed topologies.
- **Headless printer detection** — a new `--detect-printer` CLI mode (with `--host`/`--port`) queries Moonraker over REST and prints a JSON verdict, and the installer gains Tier-2 Moonraker-based detection behind a confidence gate. Detection now recognizes the Sovol lineup via hostname-free signals, RatOS (V-Core 4 / IDEX / Pro), and reports a runner-up candidate; V-Minion kinematics corrected.
- **Qidi Q2 Happy Hare preset** (prestonbrown/helixscreen#997).
- **Clean wizard re-run** — `--wizard` clears the saved preset marker and host so detection starts fresh.
- **Clearer "no display backend" diagnostics** (prestonbrown/helixscreen#998) — the app now explains why no backend was found instead of failing silently.

### Fixed

- **WebSocket reconnect use-after-free** (bundle UK9QCFY3) — Moonraker WS callbacks are installed once, closing an onclose UAF on reconnect.
- **K2 HTTPS now works** — static OpenSSL is linked into the K2 build, Moonraker readiness timeout raised to 120s with progress logging, and stock system dirs are prepended to PATH in the init script and installer.
- **WiFi bringup no longer stalls the boot splash** — wizard WiFi setup is non-blocking, and the wpa_supplicant control socket is discovered in non-standard directories.
- **AFC unload targets the requested lane**, not the active tool (prestonbrown/helixscreen#999).
- **Several rare crashes fixed** — zero-track grid template underflow, idle-reset relayout during teardown (prestonbrown/helixscreen#1001), home grid-layout activation before children are built (prestonbrown/helixscreen#983), the printer-image cache timer throwing an uncaught exception (prestonbrown/helixscreen#1000), and AMS mini-status rebuild during `lv_deinit` teardown.

### Changed

- **OrcaSlicer documented as the primary supported slicer** (2.3.2+, verified against 2.4.0), with a send-from-Orca guide.

## [0.99.74] - 2026-06-08

### Added

- **AD5X IFS cold per-lane eject/recover** (prestonbrown/helixscreen#996) — idle IFS lanes can be ejected and recovered cold (without heating the toolhead), with a "Recover" affordance for re-seating filament.
- **Light toggle shows in-flight feedback** — the light control marks itself busy the moment you tap it and clears once the g-code command is acknowledged, so the toggle reflects the real LED state instead of snapping back. Light buttons are disabled (with a brief toast) while a toggle is in flight, and in-flight state is cleared on printer disconnect.

### Fixed

- **AD5X IFS toolhead stays unloadable when firmware drops the active slot** (prestonbrown/helixscreen#995).
- **AD5X filament consumption tracked accurately** (prestonbrown/helixscreen#981) — only slots that actually tracked consumption are flushed at pause (not every slot), weight-only consumption is persisted without re-asserting filament identity, and the debug bundle captures the live log instead of a stale leftover.
- **Snapmaker U1 display takeover survives reboot** on PAXX firmware 1.4 — the stock UI binary is disabled so HelixScreen keeps the display after a restart.
- **Two rare crashes fixed** — exclude-objects map-view widget deletion and print-media async callbacks are now deferred (to the main thread / outside the UpdateQueue batch), closing use-after-free and event-list corruption windows.

## [0.99.73] - 2026-06-06

### Added

- **Per-printer settings seeding on install** (prestonbrown/helixscreen#986) — the installer seeds device settings and a Klipper config include directly from the selected printer preset, so preset printers come up correctly configured on first boot.
- **Sovol SV06 Ace preset completed** (prestonbrown/helixscreen#986) — 180° display rotation and filament load/unload macros, with the hardcoded `display.rotate` dropped in favor of the preset-driven value.
- **Localized Happy Hare MMU manual controls and the live-camera tooltip** across all eight languages (Spanish, Portuguese, German, Italian, Japanese, Russian, French, Chinese), with the CJK fonts regenerated to cover the new glyphs.

### Fixed

- **Snapmaker resume is more reliable** (prestonbrown/helixscreen#991) — the pause cause is now classified (runout, terminal, sdcard-inactive, …) instead of a blunt SD-card gate, filament config is re-asserted before resuming, and a post-resume backstop surfaces the restart-required modal when a resume can't proceed.
- **AD5X IFS active slot stays unloadable after a runout** (prestonbrown/helixscreen#995).
- **Self-update downloads the correct release asset** (prestonbrown/helixscreen#993) — fixes the "File is not a zip file" failure when Moonraker's web updater picked the wrong (alphabetically-first) artifact.
- **DRM dumb-buffer displays recover from a competing master** — DRM master is acquired with a bounded retry, fixing permanent flush failures (blank screen) when another process held master at startup.
- **Two rare use-after-free crashes fixed** — a toast input-device detach on teardown (click UAF), and macro-overlay widget pointers nulled on destroy.

### Changed

- **Expanded and corrected the user guide** — documented previously-missing settings and features, removed stale content (e.g. a pressure-advance section for a control that no longer exists), corrected sensor-role and tool-mapping descriptions, and added standalone pages (fans, sensors, security, camera, print history). helixscreen.org rebuilds from these automatically.

## [0.99.72] - 2026-06-05

### Added

- **Happy Hare selector manual controls** — the AMS sidebar and slot context menu now expose Happy Hare's selector operations directly: **Select gate** (`MMU_SELECT`), **Check this gate / Check gates (all)** (`MMU_CHECK_GATE`), **discrete servo positions** (`MMU_SERVO POS=`), a **selector jog**, and a **runtime gear-sync toggle** ("Sync during printing"). The sidebar reset button is relabeled per backend ("Home" on Happy Hare), the selector/hub box is clickable with a gear affordance, and a new capability-gated selector context menu shows only the operations the backend supports. Selector actions report feedback through the AMS status display, including a brief "Recovering" status while Happy Hare's recover runs.
- **Richer crash diagnostics for blank aborts** (prestonbrown/helixscreen#987) — the crash handler now captures the `std::terminate` reason and `abort_msg_state` for otherwise-blank `SIGABRT`s, plus recent ERROR log lines and an `lv_assert` breadcrumb, and the g-code viewer breadcrumbs its streaming render range. `resolve-backtrace.sh` gains a `--bundle` mode with call-spine extraction.
- **Presets can seed device-level touch calibration** — a preset's top-level `input` block (e.g. a known-good touch matrix) now deep-merges into settings before the wizard runs, without clobbering values the user already set.

### Fixed

- **Temperature graph no longer freezes touch on low-power displays** (prestonbrown/helixscreen#979) — the per-column gradient is rendered once into an offscreen buffer and blitted on unchanged redraws, eliminating the ~2–3s touch stall seen on the K2 Plus from repainting it every frame.
- **Snapmaker ACE Pro unload retracts correctly** (prestonbrown/helixscreen#974) — unload routes through `AUTO_FEEDING UNLOAD=1`, symmetric with load, instead of the low-level inner-filament primitive that left filament un-retracted.
- **PID/MPC calibration survives slow-cooling beds** (prestonbrown/helixscreen#988) — the timeout is raised to 20 minutes and the collector keeps listening past the RPC timeout, so large/slow beds finish instead of timing out mid-tune.
- **Theme falls back to default colors for missing or empty keys** (prestonbrown/helixscreen#989) — incomplete custom themes no longer render with broken/blank colors.
- **Print-select no longer logs an LVGL warning for files without a thumbnail** (prestonbrown/helixscreen#990).
- **Button contrast recompute guarded against widget address reuse** (prestonbrown/helixscreen#924) — a deferred contrast pass can no longer apply to a different button that reused the same address.
- **G-code viewer joins its ghost-render worker before mutating shared state** (prestonbrown/helixscreen#987) — closes a race on the color/exclude state during ghost rendering.

### Changed

- **LVGL patch set updated** — backported `LV_CHECK_ARG` argument guards (global via `lv_assert.h`), fbdev arg-guard parity, and a drag-scoped slider patch.

## [0.99.71] - 2026-05-30

### Added

- **In-app audio output device picker** (Settings → Sound) — enumerate ALSA output devices, switch live via `SoundManager::set_output_device`, and persist the selection (`/sound/output_device`) across reboots. Device resolution follows env > settings > default with a default fallback.
- **Brightness control backend for Sonic Pad** displays.
- **"Allow cold load/unload" filament safety setting** (prestonbrown/helixscreen#978).
- **Sovol SV06 ACE and SV06 Plus ACE** added to the printer database (prestonbrown/helixscreen#123).
- **Optional touch-calibration press debouncing** — enable with `HELIX_TOUCH_CAL_DEBOUNCE` (prestonbrown/helixscreen#943).

### Fixed

- **K1 / K1C CFS filament sequencing corrected and slot-edit safety guards added** (prestonbrown/helixscreen#968) — prevents no-extrude and dangerous ramming.
- **Installer and uninstaller/self-update fall back to python3 for download and zip extraction** on K2 firmware that lacks wget, curl, and unzip — uses urllib for HTTP and the built-in zipfile module, gating on zlib so a zlib-less python fails fast with a clear message (prestonbrown/helixscreen#969).
- **Installer auto-configures the ALSA default device** on HDMI-audio screens with no card 0.
- **Several rare crashes fixed** — stale-observer use-after-free on static subjects (prestonbrown/helixscreen#985), grid relayout during teardown (prestonbrown/helixscreen#973), print-select thumbnail callbacks, print-history observer removal during dispatch, and split-button label-width computation (prestonbrown/helixscreen#980).
- **G-code viewer no longer thrashes when a render stalls** — the watchdog now gives up gracefully.
- **Quieted AD5X log spam** from Adventurer5M.json polling (prestonbrown/helixscreen#981).

### Changed

- **Temperature graph rendering optimized** to cut redraw cost on low-power displays (prestonbrown/helixscreen#979) — history decimated to ~400 points and flat target-trace runs coalesced.

## [0.99.70] - 2026-05-24

Headline is **CFS support for Creality K1 / K1C / K1 Max** via the official Creality CFS firmware upgrade (>= v2.3.5.33) (prestonbrown/helixscreen#968) — the K1 firmware exposes a different `BOX_*` macro dialect than K2's `CR_BOX_*` primitives, so the AMS backend now picks the dialect at construction. Plus the rest of the CFS work: **full stock-parity envelope for load/unload/swap** with wipe-before-park, friendly translations for K2 motor init + cutter errors, and the missing K2 procd autostart shim. Also: an **installer hardening pass** (full release-tree validation before in-place update `rm`, Artillery M1 routed to the right pi/pi32 binary), a **`GcodeErrorRouter` extraction** with another L072 sweep and embedded-JSON extraction for `!!` notification lines, **abort/restart routed through `PrinterRecoveryService`** (surfaces recovery UX on stuck klippy), and a **live target trace on temperature graphs** with backfill across panel switches.

### Added

- **CFS support for K1 / K1C / K1 Max** (prestonbrown/helixscreen#968) — requires the official Creality CFS firmware upgrade (>= v2.3.5.33; not bundled with Guilouz / Simple AF / Guppy Mod). The upgrade exposes a plain `BOX_*` macro dialect (`BOX_EXTRUDE_MATERIAL`, `BOX_MATERIAL_FLUSH`, `BOX_CUT_MATERIAL`, etc.) — distinct from the K2's `CR_BOX_*` primitives. `AmsBackendCfs` picks the dialect at construction via `PrinterDetector::is_creality_k1()`; `load/unload/swap_gcode` helpers take a `CfsMacroVariant` arg so existing K2 call sites and tests stay unchanged. K1 envelope is shorter (no `BOX_SAVE_FAN`/`RESTORE_FAN`/`BOX_MODE_WAIT`). New `creality_k1_cfs` / `creality_k1c_cfs` / `creality_k1_max_cfs` database entries mirror the base K1 entries plus a high-confidence `^box$` heuristic — the picker shows "Creality K1C (with CFS)" when the upgrade is detected. K1 base entries unchanged.
- **CFS full stock-parity envelope for load/unload/swap** — including nozzle wipe before park on load and swap, and a Reset CFS recovery action wired up for key840. `BOX_SET_TEMP` dropped in favor of best-effort unwind on error. Friendly translations added for K2 motor init + cutter errors.
- **`GcodeErrorRouter` extracted from `application.cpp`** — Gcode error and `!!` notification routing now lives in its own component, with embedded JSON extracted from `!!` lines for richer error payloads.
- **Live target trace on temperature graphs** — the dashed target line now tracks setpoint changes throughout a print, with segment-start risers, right-alignment, and a per-series target history buffer that backfills replay data so the overlay matches the mini-graph after a panel switch. Buffers allocate/free with their series, reallocate on `set_point_count`, zero on `clear`, and rollback cleanly on OOM (cascade-delete leak plugged). Backed by a new unit-tested target history segmenter.

### Fixed

- **Abort/restart routes via `PrinterRecoveryService`** — replaces bare `firmware_restart` / `services.restart` RPCs and surfaces the recovery flow when klippy is stuck.
- **Installer validates full release tree before in-place update `rm`** (prestonbrown/helixscreen#970) — an incomplete tarball used to leave the device in a half-removed state; the validation gate prevents the destructive `rm` from firing if the new tree isn't fully intact.
- **Artillery M1 downloads the correct pi/pi32 binary** (prestonbrown/helixscreen#970, prestonbrown/helixscreen#971) — installer was routing M1 hosts to the wrong asset.
- **K2 busybox `wget` fallback documented** (prestonbrown/helixscreen#969) — installer instructions cover the K2's limited `wget` so users hit fewer download surprises.
- **`reset_print_card_to_idle` callers route through `lv_async_call`** — closes remaining sync-deletion paths in the print-status reset flow.
- **Embedded JSON extracted from `!!` notification lines** — error toasts/modals now pick up structured payloads instead of treating the whole line as plain text.
- **Updater waits for Moonraker extraction to settle** before refreshing service units — eliminates a race where the post-install systemctl pass would run against partially-extracted files.
- **`GcodeErrorRouter` callbacks routed through `AsyncLifetimeGuard`** ([L072]) — another bg-thread-to-UI safety pass on the notification path. Companion `RecoveryCtx` lifetime + modal dedup fixes also landed.
- **LED panel auto-selects `output_pin` strips and gates UI on real controllability** — single-pin LEDs no longer leave the strip picker stranded and the UI hides controls that wouldn't do anything on the underlying hardware.
- **Modal notification text no longer truncated** — only the toast variant truncates; modals show the full message.
- **K2 boot autostart uses procd shim** ([L086]) — plain SysV `init.d` scripts were silently skipped by procd; the shim restores boot-time launch on dev deploys.
- **Target history preserved when heater turns off** — temperature graph keeps the target trace visible after an Off command instead of flushing the prior history.
- **CI submodule init retries to absorb GitHub Actions auth transients** — submodule clone flakiness no longer red-flags an otherwise-clean build.

### Changed

- **Nightly CI builds only on macOS and skips test execution** — cuts nightly wall-clock time; full test matrix still runs on PRs and release builds.

## [0.99.69] - 2026-05-23

Headline is a **subscription regression fix from v0.99.68** — the Performance overlay's MCU `printer.objects.subscribe` was replacing the discovery subscription, so heater targets, print start, and progress updates stopped arriving on real hardware (Voron Trident + AFC bundle `9852XDXS`). Also: a **standalone fullscreen camera viewer** under Settings → Hardware, **AMS sensor toast suppression** extended to HH/AFC/AD5X IFS/CFS, **user-edited AD5X IFS slot color/material preserved** against firmware re-emission (prestonbrown/helixscreen#965), and a **friendlier installer banner** for non-Pi SBCs using the `pi` package.

### Added

- **Standalone fullscreen camera viewer** (Settings → Hardware → Camera) — opens an MJPEG stream without an on-screen `CameraWidget`. Visible only when the printer has a webcam, delegates to an attached widget if one is on-screen so mjpg-streamer's single-client limit doesn't break both streams, and tears down cleanly via `AsyncLifetimeGuard` + `lv_draw_wait_for_finish` (the `cluster:pstat-async-delete` avoidance pattern from #749). Shows a "Connecting Camera..." spinner until the first frame.

### Fixed

- **Heater targets, print start, and progress updates restored after v0.99.68** (Voron Trident + AFC bundle `9852XDXS`) — `MoonrakerPerformanceSource` issued its own `printer.objects.subscribe` for MCU stats on connect, and per Moonraker "a new request will override a previous request," so it replaced the discovery subscription (heaters, fans, `print_stats`, `virtual_sdcard`, `display_status`, AFC, …). `notify_status_update` then stopped arriving for everything outside MCU; the UI looked frozen. MCU discovery now folds into `MoonrakerDiscoverySequence` and ships in the single union built by `build_subscription_objects(...)`; the performance source registers a `notify_status_update` hook through `subscribe_notifications` instead of issuing a second subscribe. Also fixes a parser bug exposed by the field-narrowing: `bytes_retransmit` is nested inside `last_stats` (not at the top level), so MCU retransmit counts never updated in v0.99.68 either.
- **AD5X IFS self-heals when the plugin macro turns out missing** — three corrections to `has_ifs_vars_` tracking. Klipper webhooks `_do_query()` returns `{}` for unknown objects with the key still present, so a `status.contains("gcode_macro _ifs_vars")` check was falsely true on stock ZMOD without lessWaste/bambufy installed; now requires a non-empty `variables` dict. When `_IFS_VARS` mirror writes come back rejected with `// Unknown command:"X"`, a latch demotes `has_ifs_vars_` so the rest of the session takes the native-ZMOD path and stops echoing rejected gcodes on every `Adventurer5M.json` poll. A `notify_klippy_ready` listener re-queries after `FIRMWARE_RESTART` so adding or removing the plugin macro takes effect without restarting HelixScreen.
- **User-edited slot color and material preserved on AD5X IFS** (prestonbrown/helixscreen#965) — AD5X firmware re-emits the prior `FFMInfo` into `Adventurer5M.json` after print completion, and the `OverwriteAlways` auto-mirror was blindly clobbering both fields in the override. A HelixScreen-edited material would silently revert to whatever firmware re-emitted, persisted to the Moonraker DB and visible after restart. `set_slot_info(persist=true)` now tags both fields user-locked across all four AMS backends (IFS, Snapmaker, CFS, ACE); the mirror's `OverwriteAlways` path skips locked fields. Locks round-trip through `lane_data` as `helix_locked_color` / `helix_locked_material`, and legacy records load with locks pessimistic-defaulted to true so existing user data survives the upgrade. Trade-off: external Mainsail-console / native-LCD edits no longer auto-propagate over a slot the user set in HelixScreen — `clear_slot_override` is the escape hatch.
- **AMS sensor toast suppression extended to HH, AFC, AD5X IFS, and CFS** — a fresh Happy Hare or AFC install pelted users with "Filament sensor available. Add to config for runout detection?" toasts for sensors the AMS backend already owns. The substring filter caught `mmu/afc/gate/lane` patterns, but HH uses conventional names (`extruder`, `toolhead`, `filament_tension`, `filament_compression`) and AFC uses `tool_start`/`tool_end` plus user-named per-lane sensors. New discovery-aware `is_ams_sensor(name, PrinterDiscovery&)` consults the detected backend; for AD5X IFS it suppresses `_ifs_port_sensor_*`, `ifs_motion_sensor`, `head_switch_sensor`; for CFS it suppresses the bare `filament_sensor` conditional on `has_mmu()` so non-CFS users with that name still get the toast. The Settings → Sensors list and the wizard's filament-sensor selection step migrate to the same filter so HH/AFC-managed sensors no longer appear as standalone, user-configurable entries in either flow.
- **Installer banner leads with hardware label on non-Pi SBCs** — QIDI Q2/Plus, BTT CB1, MKS-Pi, etc. all install the `pi` package (generic ARM Linux build), but "Detected platform: pi" read as a misidentification to users whose printer says QIDI on the lid. The banner now leads with the friendly hardware label and reframes "pi" as the install package. Actual Raspberry Pi owners keep the original wording. Banner extracted into `print_platform_banner()` with 11 new bats cases covering real Pi, QIDI/BTT/MKS on pi & pi32, and five non-Pi platforms.

## [0.99.68] - 2026-05-22

Headline is a **Performance overlay** under Settings → System with live CPU / memory / per-MCU readouts and sparklines — backed by `MoonrakerPerformanceSource` on hardware and `MockPerformanceSource` under `--test`. Also: refreshed **Voron printer artwork**, **installer hook dispatch fixed** on CC1 / M1 / AD5X / AD5M-ZMOD, **embedded logs now persisted to flash**, and another **L081 sweep** in bedmesh.

### Added

- **Performance overlay** (Settings → System → Performance) — live CPU load, memory usage, and per-MCU stats (load, awake, retransmits) with sparklines. `MoonrakerPerformanceSource` uses `proc_stats` + mcu subscription on real hardware; `MockPerformanceSource` reads `/proc` and synthesizes MCUs under `--test`. New `helix_sparkline` custom widget, reusable `perf_metric_row` XML component, pre-formatted CPU/mem/per-MCU text subjects, and dynamic per-MCU subjects.
- **Voron printer artwork refresh** (prestonbrown/helixscreen#964) — added V0, Legacy, and V2; retired V2.4-r2 and the 0-2 variants. Existing configs auto-migrate (v16 → v17).
- **Active log destination shown in Settings → About** — confirms at a glance where logs are landing on the current device.

### Fixed

- **Performance overlay now populates on real hardware** — `MoonrakerPerformanceSource::start()` runs at app init, before the Moonraker WS is connected and before the HTTP base URL is set, so the initial REST `/machine/proc_stats` and `printer.objects.list` RPC were both lost (REST: "HTTP base URL not configured"; RPC: rejected by `ready_to_send`, response callback silently dropped). `notify_klippy_ready` would normally retrigger discovery, but Moonraker doesn't emit it on cold-connect to an already-ready Klippy. Adds a multi-listener `add_connected_observer` / `remove_connected_observer` registry on `MoonrakerClient` (fires alongside the primary `on_connected` callback on WS open and Klippy ready, with immediate-fire if already connected). The perf source now hooks this observer to defer its initial handshake until the WS is up.
- **Installer hook dispatch on CC1, M1, AD5X, and AD5M-ZMOD** — missing `case` branches meant platform-specific post-install steps silently no-op'd on these targets.
- **Embedded log routing prefers persistent flash** — prevents log loss on devices where the previous default landed on tmpfs and disappeared across reboot.
- **Theme border-radius applied to progress bars** — they now match the rest of the UI instead of rendering with sharp corners.
- **Retired Voron `printer_image` IDs auto-migrate (v16 → v17)** — configs referencing the old IDs no longer show a broken-image placeholder after upgrade.
- **L081 cleanup in bedmesh background callbacks** — replaced bare `tok.expired()` checks with `lifetime_.bg_cb` to close another Mechanism C UAF surface.
- **Bundled 16-bit PNGs downsampled to 8-bit** — workaround for a lodepng SIGSEGV on certain platforms.

### Changed

- **Performance row moved from About to Settings → System** — keeps About focused on identity / version and groups diagnostic readouts together.

## [0.99.67] - 2026-05-20

### Fixed

- **Manually-set nozzle target preserved across filament load/unload** — if you'd already heated the hotend (e.g. ABS at 240°C) and clicked Load with a different material selected, the post-op cooldown manager killed the heater 120s later thinking it owned the preheat. Now snapshots the live extruder target at every op-handler entry, and the preheat path won't drop a higher manual setpoint to a lower material preset (no more 240→200 dip on cold-nozzle load).
- **Chamber-temp display no longer flashes bed temp on QIDI Q2** (prestonbrown/helixscreen#947) — two independent bugs: (a) heater + thermal-protection sensor coexisting let partial Moonraker updates overwrite chamber_temp with the bed-side sensor reading, so heater is now the canonical source when present; (b) `is_chamber_keyword()` greedily matched `BOX` substrings, so QIDI Box dryer objects (`box1_heater`, `Box1_STM32`) hijacked the chamber slot. Now scores CHAMBER=100 / ENCLOSURE=90 / CAVITY=85 / standalone-token BOX=60 and tracks the best match per channel.
- **Cached overlays reflow to current canvas width on resize** (prestonbrown/helixscreen#951) — `OverlayBase` caches its root widget across show/hide for state preservation, but the width was baked in at first create. When the canvas later shrank (Android navbar pin shrinking the LVGL surface), the cached overlay kept its old width and slid left under the sidebar. The resize callback now re-applies `overlay_panel_width_full`/`overlay_panel_width` to any `*_overlay` widget without destroying it.
- **Android Keep Navigation Bar setting survives fold and app-switch on Galaxy Z Fold 7** (prestonbrown/helixscreen#951) — `configChanges` in AndroidManifest keeps the activity alive across folds so `onResume` never fired; an `onConfigurationChanged` override + `OnApplyWindowInsetsListener` self-correct after SDL's window-style race. With targetSdk 35 + edge-to-edge enforced, the content view is now padded manually by `WindowInsets.Type.navigationBars()` so LVGL no longer draws behind a pinned side navbar (Fold 7 landscape clipping). System-bar icon appearance follows the active theme's background luminance; targetSdk/compileSdk bumped 34 → 35 for Play Store forward compatibility.
- **Snapmaker U1 post-install commands point at the real init script** (prestonbrown/helixscreen#952) — `INIT_SCRIPT_DEST` was `/etc/init.d/S99helixscreen`, a path that has never existed on U1 (the installer patches stock `/etc/init.d/S99screen` to delegate to `helixscreen.init`). Now points at `${INSTALL_DIR}/config/helixscreen.init`. Log-path hint also corrected — U1 writes to `/var/log/helixscreen/launcher.log` or `${INSTALL_DIR}/logs/launcher.log`, never `/tmp/helixscreen.log`.

### Changed

- **QIDI documentation refactor** (#963) — `docs/user/QIDI_SUPPORT.md` reordered by release date, TJC reframed as OEM (not a clone), Plus 4 reclassified to the older MKSPI bucket. Follow-up fixup restored the `plus4`/`plus-4` hostname pattern, GitHub repo URL (no spaces), the FILAMENT_MANAGEMENT anchor, and propagated the TJC-OEM correction to `README.md` + `docs/devel/printer-research/QIDI_PLUS_4_RESEARCH.md`.

## [0.99.66] - 2026-05-19

The big-ticket items: a **toolchanger-aware print-detail FILAMENTS card** with three-tier temperature precedence (user override > vendor profile > printer database), the **QIDI Box AMS backend read-path** (with the write-path landing behind `HELIX_QIDI_BOX_WRITE` for field testing), and a **new fan row in the print-status panel** with live animation and click-through to fan controls. Plus **Snapmaker U1 prepare-for-resume hooks** for modal-driven runout recovery, **`__abort_msg` capture on SIGABRT** for clearer crash reports, a **Touch & Input settings sub-overlay**, and another sweep through L081 background-thread sites.

### Added

- **Toolchanger-aware FILAMENTS card on the print-detail panel** — respects the active extruder, per-tool nozzle temperatures, and tool topology when displaying loaded filament info. Three-tier temperature precedence (prestonbrown/helixscreen#961) means user overrides win over vendor profiles, which win over the printer-database default; live nozzle temperature is shown during heating; mini temp-graph + heater observers rebind correctly on tool change. Compact-mode tool picker scrolls vertically when crowded and horizontally for the pill row; pills even-distribute across two rows when there's vertical room.
- **QIDI Box AMS backend read-path** (prestonbrown/helixscreen#954) — detection wiring, read-only state mirror, per-slot RFID capture from `save_variables`, official filament-list lookup for temperature profiles, drying state mirrored onto unit environment, `last_load_slot` mirror to `current_slot`, integration tests. Write-path landed behind `HELIX_QIDI_BOX_WRITE` for field testing — `is_tool_change` flag propagated onto `AmsAction`, `get_slot_info` bounds-checked against current total slots.
- **Print-status fan row** — new XML component wires part/hotend/aux fan observers with spin animation, adaptive fit based on available height + content density, click-through to fan controls, and a disabled state when the printer is disconnected. Falls between the filament row and the button row.
- **Touch & Input settings sub-overlay** — calibration and tuning knobs (long-press delay, scroll throttle, drag start, etc.) now live under a dedicated overlay in Settings instead of being scattered.
- **`helix_progress_arc` shared component** — single reusable arc widget driven by a diameter token so progress arcs across the UI stay consistent in stroke width and style. Clog meter migrated first.
- **helix-xml `parts=...` attribute on `bind_style`** — apply one bound style to multiple part selectors in a single declaration instead of duplicate `bind_style` rows.
- **helix-xml `scroll_dir` attribute parsing on `lv_obj`** — declarative scroll direction without a custom widget.
- **`SIGUSR1` triggers in-process screenshot** — useful for field-bundle capture without prompting the user to press `S`.
- **Snapmaker U1 `prepare_for_resume` hook + rewritten print-start profile** — modal-driven runout recovery dispatches via a shared `dispatch_prepared_resume` helper; the print-start profile was recaptured from live gcode and rewritten to match. Includes a dirty-bed restart UX, runout sensor `role=runout` assignment, and a post-wizard preset-migration window for filament sensors so existing configs upgrade cleanly.
- **Crash handler captures glibc `__abort_msg` on SIGABRT** (prestonbrown/helixscreen#960) — the abort-message string (assert text, terminate exception, etc.) is now included in the crash bundle alongside the backtrace. Truncated to 256 bytes with the trailing newline stripped.
- **`print_status` silent-phase progression** — invisible cleaning / purge windows mid-print no longer leave the UI looking idle; the arc keeps advancing through the silent phases.
- **`print_status` hourglass during pending Pause/Resume** — optimistic UI updates the icon immediately on tap, then settles to the confirmed state.
- **Home widget Paused badge** on the print-status widget when the print is paused.
- **`print_start` parses Klipper's "Adapted probe count" line** for a live mesh denominator; `virtual_sdcard.is_active` exposed on printer state.
- **Per-tool gcode tracking + material-mismatch UX fix in the mock backend** — exercises multi-tool material-mismatch flows under `--test`.
- **33 strings translated across 9 languages** — 100% coverage for the latest UI additions.

### Fixed

- **QR scanner background thread no longer races on `tok.expired()`** — drops the bg-thread `expired()` checks and relies on a `running_` flag instead, closing one of the remaining L081 Mechanism C surfaces.
- **L081 strict-mode abort gated to non-release builds** — release tarballs no longer crash when the detector trips; telemetry still emits the anomaly. (Snapmaker U1 hit a stray strict-mode abort on 2026-05-14, sig 307b6f48.)
- **gcode-viewer restores live 2D/3D switching mid-print** — toggling render modes during a print no longer leaves the viewer blank.
- **gcode-viewer force-redraws on display wake** — waking from screensaver with a 3D viewer on-screen no longer shows the prior frame.
- **gcode-renderer invalidates SSAO cache after writing new layers** — ambient occlusion stays in sync with streaming geometry.
- **gcode-renderer retries streaming load misses** instead of silently skipping the missed window.
- **`print_status_widget` keeps progress arc live across layout rebuilds** — breakpoint transitions during a print no longer momentarily reset the arc.
- **`filament_sensor` shows disabled state instead of hiding the indicator** — the slot stays in the UI with a "disabled" badge so users can tell a sensor was intentionally disabled vs. missing.
- **One toast per Klipper rejection, not three** — and the generic `!!` toast is deferred so a runout modal can pre-empt it.
- **Restart-from-beginning modal surfaces `print_stats.message`** when present.
- **Screensaver pipes overlay teardown via `safe_delete_deferred`** instead of sync `lv_obj_delete`, avoiding UpdateQueue-batch corruption.
- **Cached navigation overlays re-register on every push** so navbar switches don't drop the binding mid-session.
- **`filament_sensor` defaults to `detected=true`** — dropping the startup grace gate that would briefly show "no filament" on every cold start.
- **Tool-changer AMS always shows the runout modal** — previously could fall through silently when topology wasn't yet known.
- **Filament panel loads directly into the active slot** and skips the AMS-redirect detour when the user just wants to load filament now.
- **AMS demotes halted-Klipper gcode refusals to debug** — a halted printer rejecting a queued gcode is expected, not warn-worthy.
- **Per-tool hardware mappings preserved across `update_from_status`** (prestonbrown/helixscreen#956) — re-querying status no longer overwrites tool-mapping fields the AMS backend owns.
- **`print_status` BED_MESH probe counter resets on sub-phase change** — counting now restarts cleanly at each calibration sub-step.
- **`print_status` configfile probe-count fallback skipped for adaptive meshing** — adaptive paths get their denominator from the parsed "Adapted probe count" line.
- **Crash file writers serialized** — SIGABRT and EXCEPTION paths no longer race to truncate each other.
- **Telemetry crash classifier uses memory-map `r-xp` segments** — improves backtrace symbolization.
- **3D viewer no longer blank when toggled mid-print** in `--test` mode.

### Changed

- **Print-detail FILAMENTS card reworked for toolchanger UX** — single shared layout used across AMS panels, with a bypass-spool widget + material label DRY'd, and an updated External Spool context menu shared from one source. AMS XML tokens consolidated under `ams_*` + shared breakpoint helper.
- **AMS state forwards backend tool topology to `ToolState`** via a new `set_ams_topology` API (prestonbrown/helixscreen#956); `update_from_status` guards against overwriting topology when the AMS owns it.
- **2D gcode render: darker outer walls** for better depth perception in ghost render mode.

## [0.99.65] - 2026-05-15

Same-day cherry-pick on top of v0.99.64 to repair the release-pipeline build. Debian Bullseye's multi-arch apt resolver couldn't reconcile the pre-installed `linux-libc-dev:amd64` against a newer `linux-libc-dev:arm64` from the mirror, blocking `libc6-dev:arm64` install in both the Pi and Pi32 Docker toolchain images. No code changes — same payload as v0.99.64.

### Fixed

- **Pi and Pi32 release builds were blocked by a Debian Bullseye multi-arch apt skew** — `libc6-dev:arm64` (and the armhf equivalent) couldn't be installed because the `linux-libc-dev` version baked into the amd64 base image diverged from the arm64/armhf version on Debian's mirror, so apt refused with "Depends: linux-libc-dev:arm64 but it is not going to be installed". `docker/Dockerfile.pi` and `docker/Dockerfile.pi32` now list `linux-libc-dev:amd64` and `linux-libc-dev:<target-arch>` together in the apt install line, forcing apt to pick a matching version pair. Caught when v0.99.64's release pipeline failed Pi/Pi32 jobs; the other eight platforms built fine.

## [0.99.64] - 2026-05-15

A **unified exclude-objects side panel** with 3D selection brackets replaces the old fullscreen overlay, plus fixes for **CJK font rendering** of new translation keys, an **LVGL flex gap regression** when the first child is hidden, **STRING-subject image binding** (the print-status thumbnail was stuck on the benchy placeholder), and **missing-glyph arrows** in two user-facing dialogs.

### Added

- **Unified exclude-objects side panel** — the fullscreen exclude-objects list is replaced by a slide-in side panel pinned over the print-status controls column. Map view stays in the thumbnail card, the gcode viewer keeps full width (2D / 3D), and the list lands on the right at ~44% in every mode. Row taps and viewer taps fire the same confirm modal; row taps also pulse the matching object's highlight in the viewer for cross-pane feedback. The 3D viewer gains GLES corner-bracket rendering for highlighted objects (line shader + per-frame VBO inside the FBO, sharing the geometry-pass MVP); 2D and 3D both share `AABB::for_each_bracket_arm()` so the bracket geometry stays in lockstep. Bbox-projection fallback in `pick_object` fixes wrong-object picks after the geometry pass clears segments.

### Fixed

- **Print-status thumbnails were stuck on the benchy placeholder** — `bind_src` in helix-xml only handled `LV_SUBJECT_TYPE_POINTER`; STRING subjects (e.g. `print_thumbnail_path`, `print_status_idle_thumb_path`) silently fell back to the static XML `src=`. STRING subjects now route through a local observer that calls `lv_image_set_src` on the resolved path.
- **Flex layout left an unwanted gap before the first laid-out child** when that child was hidden and the next visible sibling had `flex_grow>0` — backport of upstream lvgl/lvgl#9897; the patch is wired into `mk/patches.mk` so submodule resets don't lose it.
- **Missing-glyph arrow boxes in two user-facing dialogs** — the bundled NotoSans Regular/Bold/Light TTFs don't include U+2190–U+2193, so the multi-tool material-mismatch row and the internal-error toast rendered a tofu box where an arrow was supposed to be. Replaced with `->` and `Settings > About` respectively.

### Changed

- **CJK fonts regenerated for new translation keys** — 1127 new CJK characters from the latest translation sync are now included in `noto_sans_cjk_*.bin`, so Chinese/Japanese locales render the new strings (Library, Objects, Show temp for, Follow active tool, Keep Navigation Bar, etc.) instead of falling back to a missing-glyph box.
- **Translation keys synced across all locales** — 7 new keys added with non-English locales populated as untranslated placeholders; compiled artifacts regenerated.
- **Docs: QIDI Q2 stock-firmware 1-line installer + WiFi confirmed working.**

## [0.99.63] - 2026-05-15

This release lands the **Detailed print-status layout** — a multi-tool-aware redesign with per-nozzle temperature pinning, a Library/Detailed picker, and an idle hero pulling thumbnails from print history — plus **3D gcode-viewer performance wins** (vertex packing 36→20 bytes for ~44% buffer reduction, in-place tool-color patching), a **real input-shaper wizard abort** when you back out mid-calibration, and a fix for **settings loss on flash power-loss** by `fsync`'ing `settings.json` and its parent directory.

### Added

- **Detailed print-status layout** — new `Detailed` option in the print-status layout picker (alongside Library), with a multi-tool-aware UI: per-nozzle temperature row using `Nozzle N` display names, tool-pinning picker that fixes the displayed nozzle to a specific extruder, idle hero populated from `PrintHistoryManager`, progress arc with the `%` label nested inside, layer/time/filament rows, and width-gating so the full detail set only appears at sufficient breakpoints. Persisted via `layout_style` and `nozzle_tool_override` config keys.
- **`multi_tool` subject + `T<n>` active-tool label** — print-status exposes a multi-tool subject; the T-label tracks the pinned tool when set, otherwise auto-follows the active extruder.
- **Temperature-graph legend overflow pill** — when more sensors are charted than fit, the legend truncates with a `+N` pill so the chart doesn't get squeezed by an over-long label row.
- **Qidi X-Smart 3 in printer database** — initial profile added; QIDI docs clarified that FreeDi runs as a remote client.

### Fixed

- **Input-shaper wizard now aborts in-progress calibration when you back out** (prestonbrown/helixscreen#945) — exiting mid-calibration previously left the chain running on the printer; the wizard now sends a real abort and allows a clean restart on re-entry.
- **Settings survive flash power-loss** (prestonbrown/helixscreen#943) — `Config::save_settings()` now `fsync`'s `settings.json` and its parent directory before returning. Companion: forced touch-calibration skips when a valid saved cal exists, so a power-loss-induced incomplete write doesn't trigger a recalibration prompt.
- **Android: SDL surface resize propagates through LVGL + theme refresh** (prestonbrown/helixscreen#941) — clamshell foldables (Galaxy Z Flip 7, Fold series) now redraw correctly when the screen geometry changes mid-session.
- **DNS: fail closed on embedded ARM/MIPS when `dns_resolv` fails** — silent DNS misconfiguration on resource-constrained devices no longer surfaces as a generic connection failure with no diagnostic.
- **Indev cancellation on navigation teardown** (prestonbrown/helixscreen#906) — in-flight input-device events at panel/overlay teardown boundaries are now cancelled cleanly, closing one of the L081 Mechanism C surfaces.
- **AfcConfigManager bg-thread access** — residual L081 Mechanism C site flagged by the extended `->expired()` lint.
- **AmsState print-state observer UAF across deinit cycles** — caught by the nightly test gate; observer/lifetime ordering corrected.
- **Memory: pressure-driven viewer release with telemetry context** — dropped the static `is_low_memory` gate in favor of a runtime pressure signal; trigger reason recoverable from bundles.
- **Auto-discovered chamber sensor promoted to CHAMBER role** — sensors discovered via Klipper introspection now correctly classified; chamber-override path skips redundant work when the sensor is already chamber-classified.
- **Temperature graph: stale widget tolerated on config-save** (bundle RP293UCW) — chart hide-pre-delete prevents a crash when the user saves config while the temp graph is mid-redraw.
- **Temperature graph: consistent `Nozzle N` labels in mini-graph + multi-tool defaults; X-axis labels gated on rowspan, not colspan; extruders sorted for stable ordering.**
- **Print library button outlines no longer clip top/bottom** — extra padding restored.
- **Settings > Hardware Health entry now shows unconditionally** — previously hidden behind a stale capability check.
- **M117 row deduplicated and surfaced on the full print panel** — message was being echoed in two places under certain print states.
- **Printer-list spacing** — breathing room added between rows and the `Add Printer` button.

### Changed

- **3D gcode viewer: PackedVertex layout shrunk 36 → 20 bytes** — octahedral normal encoding + RGBA8 color packing cuts vertex buffer size ~44%; companion test updates.
- **3D gcode viewer: tool color changes patch the prepared buffer in place** instead of dumping and rebuilding it, eliminating a noticeable hitch when toggling per-tool colors mid-view.
- **Memory: 'good tier' RSS thresholds scale as a fraction of total RAM** — fixed-MB thresholds were over-eager on small devices and over-permissive on large ones; thresholds now scale with installed RAM.
- **Temperature graph: pre-discovery configs auto-upgrade on load** — removes a class of config-load mismatches when migrating from an older profile snapshot; partition logic simplified.
- **Nightly CI captures core dumps on test-all failure** — failed tests now leave a recoverable artifact for backtrace resolution instead of an opaque failure log.
- **Docs: CC1 OpenCentauri COSMOS links repaired; CC1 architecture clarified as `armv7-a`.**

## [0.99.62] - 2026-05-13

This release lands the **XML linter as a CI gate** (and the ~50 XML cleanups it surfaced), **gcode-streaming polish** for OrcaSlicer and purge/wipe-tower geometry, **launcher and installer hardening** (env preservation on upgrade, rolling-backup sweep on clean, tolerant env parsing with warnings instead of silent abort), and a **local-exec recovery script** that replaces the Moonraker `shell_command` path which never worked on printers without the `helix_recover` macro. Plus an updater fix for a 32-bit `statvfs` overflow that falsely reported "no space" on Pi.

### Added

- **Touch-calibration force honored across wizard and standalone paths** — the wizard's force-recalibrate option and the `--force-touch-calibration` standalone entry point now route through the same path so either reliably triggers a fresh calibration regardless of which one ran.
- **Gcode viewer excludes purge tower and wipe tower from 2D auto-fit bounding box** — slicers that emit a purge/wipe block at the bed edge no longer drag the preview camera off the actual print; the model fills the viewport like it should.
- **About screen surfaces kernel arch + userspace bitness** — useful for triaging cross-arch reports (e.g., SonicPad: aarch64 kernel running armhf userspace). The About panel now shows both lines so debug bundles don't have to guess.
- **Installer sweeps rolling config-backup directories on uninstall and clean** — `~/.helixscreen-backup.*` directories from prior upgrades used to accumulate forever; `uninstall` and `clean` paths now sweep them.
- **XML linter (`helix-xml-linter`) vendored and wired as a CI gate** — imported from GhostTypes/helix-xml-linter@2124093, auto-discovers widgets and design-token constants from the source tree, and runs as a CI gate via the Makefile. Catches unknown-const-refs, silently-dropped attributes, and style-prop typos at lint time instead of at runtime where they'd surface as warnings or misrendered widgets.

### Fixed

- **Installer no longer deletes bundled `helixscreen.env` on upgrade** — prior versions wiped the user's env file during upgrade, taking any custom log level / mock config / language override with it. The upgrade path now leaves it in place; only `uninstall` removes it.
- **Recovery probe uses local `helix-recover.sh` instead of the dead Moonraker `shell_command` path** — printers without the `helix_recover` macro in their Klipper config (most non-Klipper-default builds) had no working recovery action; the probe would fall back to a "method not found" toast or worse, no-op silently. The probe now exec's a local script bundled with the installer, removing the firmware-side dependency entirely.
- **Updater "no space" false-positive on 32-bit Pi (and other armhf platforms)** — `statvfs::f_bavail * f_frsize` overflowed `unsigned long` on armv7, returning 0 even on disks with tens of gigabytes free. Widened the multiplication to `uint64_t` so reported free-space is honest. Adjacent to the prior systemd-namespace `statvfs` mystery (bundle 7ZGHW5KX) but a different mechanism.
- **Update manifest now publishes installer size; fallback floor lowered 200 → 120 MB** — the "minimum required free space" preflight was over-conservative when the manifest size key was absent. Now uses the published size when present and a tighter fallback otherwise.
- **EGL init failure on backdrop-blur is sticky for the boot session** — when GPU-accelerated backdrop blur failed once, the retry loop kept hammering EGL on every frame and spamming logs. First failure now disables the path for the rest of the session; the user gets the software fallback without the log noise.
- **Gcode streaming parser polish** — four targeted fixes to the streaming path: purge filter now applies to full-file mode too (was streaming-only); OrcaSlicer `;LAYER_CHANGE` markers index correctly without false positives on similar substrings; parser position is seeded per-layer so partial reads pick up correctly; axis extraction truncates at `;` comments so commented G1 values don't pollute bbox math.
- **Launcher tolerant env-file parsing with malformed-line warnings** — malformed lines in `helixscreen.env` (bad quoting, stray characters, unterminated strings) used to abort startup with an opaque shell error. The launcher now warns about the bad line and continues with the rest of the file, so one broken variable doesn't leave the app unable to launch.
- **launcher.log moved off tmpfs, prefers FHS `/var/log/helixscreen/`** — `/tmp` is cleared on reboot on many platforms, so launcher.log was already gone by the time anyone could read it after a boot-time crash. Now persists under `/var/log/helixscreen/` with a fallback for permissions-restricted environments.
- **Logging: console sink disabled under daemon mode; per-frame trace spam removed from icon + filament path** — daemon-mode runs had duplicate console output going to journald in addition to the file sink; the console sink is now suppressed when running detached. Separately, two TRACE-level call sites in the icon-cache and filament-path render were firing at 60 Hz; both demoted/removed to silence the spam.
- **About screen logo no longer looks misaligned** — `helixscreen-logo.png` had a baked-in transparent border that made the rendered logo appear off-center in the About panel; cropped at the asset.
- **Installer hardening: uninstall→reinstall race + self-delete guards** — back-to-back uninstall+install (e.g., from the in-app reinstall path, or from a forced reinstall on the update path) could leave the installer mid-flight when its own `$0` lived inside `$INSTALL_DIR`. New `guard_self_delete` + sentinel-path machinery refuses to remove the directory that's currently executing, with clear error messages instead of mysterious half-states.
- **L081 Mechanism C sweep: residual bg-thread `tok.expired()` guards removed** — follow-up to the v0.99.60 codebase-wide cleanup, picking up the remaining redundant guards the strict-mode runtime detector continued to flag.
- **~50 XML correctness fixes surfaced by the new linter** — every cleanup the linter found, batched: unknown-const-refs (`#nonexistent_token` references), style-prop typos (`stryle_pad_all`, CSS-name aliases that LVGL doesn't honor), silently-dropped attributes replaced with their actual LVGL equivalents, `variant="muted"` removed from widgets that don't accept it or where it was redundant on `text_small`/`text_body`, `flag_` prefix and `flags=` shortcut normalized to per-flag attributes, `text_input` attributes (`placeholder`, `keyboard`) standardized, `ui_button` cleanup, and `setting_dropdown_row hide_description` properly wired.

### Changed

- **LVGL `lv_image_set_src` warning now includes object + parent name** — when an image fails to load, the diagnostic identifies *which* image so it's actually triageable instead of a generic "couldn't load image" pointer-style log. Internal LVGL patch (`lvgl-image-set-src-warning-context.patch`).
- **Docs: comprehensive log destinations and retrieval per platform** — new docs page covering where logs live and how to fetch them on Pi, K1, K2, AD5M/AD5X, SonicPad, CC1, and Snapmaker U1, because the answer is different on every device.

## [0.99.61] - 2026-05-13

A round of **launch-time and connect-time UX polish** plus **home-screen widget polish**: the Klipper recovery dialog no longer flashes briefly when starting the app or adding a printer, the home panel no longer renders blank after switching printers, and the Snapmaker U1 wizard correctly identifies a U1 instead of showing *UNKNOWN*. Tapping a fan tile now opens fan control instead of the picker, the fan dial debounces drag-induced Moonraker sends, and the rest of the icon widgets center their icon+label as a group when labels are on. Plus a fix for the M300 sound feedback loop on hosts without a Klipper-declared beeper, the AD5X *Method not found* toast on the recovery probe, and a wake-from-sleep regression during prints when sleep-while-printing is disabled.

### Added

- **Android: "Keep Navigation Bar" setting** — Display & Sound > Appearance now has a toggle (Android only) that pins the system nav bar onscreen instead of using immersive mode + swipe-to-reveal. For users who prefer 3-button nav over gesture controls (prestonbrown/helixscreen#908).

### Fixed

- **Display stays asleep mid-print when `sleep_while_printing=false` (bundle RYAQGL6C)** — `check_display_sleep()` returned early when a print was active, but the early return also skipped the wake-on-touch path. If the display had entered sleep *before* the print started, the user was stranded on a blank screen for the duration of the print (RYAQGL6C: 8 touch events, 18-minute wake delay). Wake requests now fire during prints; only the *entering* of sleep is suppressed.
- **Klipper recovery dialog no longer flashes briefly at launch or after Add Printer** — `PrinterNetworkState` initializes `klippy_state` to SHUTDOWN as a conservative default; the EmergencyStopOverlay observer fired once with this placeholder at subscribe time, briefly showing the recovery dialog before Moonraker reported real state. Same placeholder also injected the `firmware_restart` widget on the home panel for a few frames. Two targeted guards: the overlay drops the initial placeholder fire on every (re)subscription (singleton flag was sticky after the first launch, so Add Printer's soft restart re-triggered the flash), and `PanelWidgetManager` gates `firmware_restart` widget injection on `printer_connection_state == CONNECTED`. Genuine shutdown-at-startup is still surfaced via the parallel `KLIPPY_SHUTDOWN` event path. Regression introduced by 1d13ed6b4's freeze-buffering rework, which was previously masking the placeholder fire.
- **"Request Failed: Method not found" toast on recovery probe (bundle VHXPB8A3)** — `PrinterRecoveryService` probes `shell_command:helix_recover` and falls back to `printer.firmware_restart` if the macro isn't defined; the probe surfaced a scary toast even though the fallback succeeded (AD5X without the macro). `MoonrakerAPI::run_shell_command` gains a `silent` flag; the recovery probe uses it so the global RPC_ERROR event is suppressed for that one call.
- **Home panel blank after switching printers** — `tear_down_printer_state()` destroys the HomePanel singleton; the re-created instance has `finalized_=false`. The cold-launch path calls `finalize_setup()` after `create_overlays()`, but the soft-restart path used by switch_printer didn't, so the home panel rendered as an empty container until next navigation.
- **Snapmaker U1 wizard now identifies as Snapmaker U1, not "UNKNOWN"** — original heuristics matched a hypothetical RFID-extended firmware (`fm175xx_reader`, `FILAMENT_DT_UPDATE/QUERY`) that doesn't exist on stock U1s. Replaced with patterns verified against a live device: `homing_precise_corexy`, `extruder_offset_calibration`, `filament_entangle_detect`, `machine_state_manager`, `defect_detection`, `purifier`, plus the `FEEDING_RUNOUT_EVENT_HANDLE` and `EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL` macros. Old RFID heuristics retained as secondary path for future firmware variants. Kinematics corrected from cartesian to corexy (matches `homing_precise_corexy` + tmc2240 stepper_x/y wiring).
- **M300 sound feedback loop on hosts without a Klipper-declared beeper** — `SoundManager::create_backend()` fell through to the M300 (Klipper gcode beeper) backend whenever Moonraker was connected and no host audio backend (SDL/ALSA/PWM) initialized. On any printer whose Klipper config lacks `[output_pin BEEPER]` + `[gcode_macro M300]`, every UI sound triggered `M300` → `!! Unknown command:M300` → gcode error toast → error_tone sound → M300 feedback loop. Surfaced as "no audio + spam of unknown command M300" on hosts where local audio fails (e.g. Pi where ALSA `default` fails to open). M300 is now installed lazily from `PrinterCapabilitiesState::set_hardware()` only when `hardware.has_speaker()` is true (PrinterDiscovery detects `[output_pin BEEPER/BUZZER/SPEAKER]` in the Klipper config). `set_moonraker_client(nullptr)` drops an active M300 backend so printer switches don't carry it across.
- **Modal dialog sized too tall for short messages** — Save Hardware, Reset to defaults, and similar short confirmations were stuck at 200px regardless of message length, with content stretched to fill (two regressions in `modal_dialog.xml`: `style_min_height=200` from f48ceb70c, `flex_grow=1` on `content_container` from 3482bb989). Content container is now `height=content` with a responsive `#dialog_content_max` token (140/200/260/320/440/600/800 across breakpoints), keeping OK/Cancel on-screen with long content.
- **L081 Mechanism C anti-pattern in `PrintHistoryManager` and `ControlsPanel::handle_home_all`** — bare `if (token.expired()) return;` on a bg thread followed by `this`/member access is a TOCTOU race; detector hit on v0.99.60/ad5x for both callsites. `PrintHistoryManager::subscribe_to_notifications` drops the bare expired check in the WS `notify_history_changed` callback (defer's own guard runs atomically on the main thread). `ControlsPanel::handle_home_all` rewrites the `home_axes` success/error callbacks to use `lifetime_.bg_cb()` instead of manual `tok.defer` with the forbidden bg-thread expired prelude.
- **Tapping a fan widget now opens fan control, not the fan picker** — the fan tile's normal tap showed the *which fan to monitor* picker on every click. That picker is a configuration choice and belongs behind the edit-mode gear; mirrors the FanStackWidget pattern. Normal tap now pushes the fan control overlay; `on_edit_configure` still opens the picker.
- **Fan dial drag debounces SET_FAN_SPEED instead of flooding Moonraker** — dragging the fan dial fired `SET_FAN_SPEED` on every `VALUE_CHANGED` tick, flooding Moonraker mid-drag. Latest value is stashed in a 500ms one-shot timer that resets on each tick; `RELEASED` (or `PRESS_LOST`) flushes immediately so letting go feels responsive. Timer is cancelled in dtor and move ops to keep its `this` user_data safe.
- **Belt-tension panel's orphaned "Freq Graph" button removed** — the button toasted *coming soon* on every press; gone, along with its handler stub and calibration-doc reference.
- **Icon widgets center icon and label as a vertically-stacked group when labels are on** — macros, favorite_macro, motion, lock, led, led_controls, gcode_console, shutdown, network, firmware_restart, and notifications now use the same flex-column layout as temperature: icon and label flow as a centered pair when `show_widget_labels` is on; the label drops out of layout (`LV_OBJ_FLAG_HIDDEN`) when off so the icon re-centers alone in the cell. `panel_widget_ams` and `panel_widget_active_spool` are intentionally left alone (their inner content fills 100%/100% and would clip in a flex column).

### Changed

- **Hardware Issues moved from Settings > System to Settings > Hardware & Devices** — the Hardware Issues row (visible when hardware validation detects missing/changed hardware) lived next to network/telemetry/factory-reset, but it's about printer hardware; it now sits above Printers in the Hardware & Devices overlay where users actually look. User docs and TROUBLESHOOTING.md updated for the new path (`Settings > Hardware Health` → `Settings > Hardware & Devices > Hardware Issues`).
- **Docs refresh** — README, USER_GUIDE, FAQ, INSTALL, ROADMAP, and PRINTER_MANAGER updated with current counts (80+ printers, 7 filament backends, 300+ XML components) and aligned hardware-frugality framing for the 1.0 hero/bullet/FAQ language.

## [0.99.60] - 2026-05-11

The headline is the **completion of the L081 Mechanism C sweep** — 107 background-thread callsites across panels, modals, wizards, calibration, AMS backends, and macro/print preparation chains now route LVGL touches through `tok.defer` / the new `bg_cb` helper, with the strict-mode abort detector active in tests/CI to keep new regressions from landing. Pairs with two AMS firmware-writeback features (CFS via `BOX_MODIFY_TN_DATA`, Snapmaker via `filament_detect/set` Extended Firmware) that finally make color edits persist to the printer, and the **layered filament-path renderer** flipping default-on (legacy renderer deleted) for smoother animation. Round it out with **CFS hardware-error UX** surfacing key8xx faults via `respond_raw` with a recovery chain + purge-gate, and a **WiFi auto-recovery** path for stale NM profiles missing `key-mgmt`.

### Added

- **AMS color firmware-writeback** — color edits in the UI now persist back to the printer firmware. CFS uses `BOX_MODIFY_TN_DATA`; Snapmaker uses `filament_detect/set` (Extended Firmware). Slots auto-mirror firmware-detected colors into `lane_data` so OrcaSlicer sees the same source of truth.
- **"Print Paused" overlay with reason** — status panel now surfaces the pause reason instead of a generic spinner.
- **CFS pre-print step indicator** — synthesizes Cut / Feed / Retract / Purge phases from physical signals (cutter trigger, extruder motion, filament sensor) so the load step row reflects what the box is actually doing.
- **Pre-print `requires_macro` gate** — firmware-dependent options now declare which macro they need; the option row hides when the macro is absent instead of erroring at run time.
- **WiFi auto-recovery for stale NetworkManager profiles** — when a saved connection is missing `key-mgmt` (common after firmware updates or hand-edited keyfiles), we now rewrite the profile and reconnect instead of failing silently.
- **`bg_cb` helper + L081 strict-mode detector** — `lifetime_.bg_cb(tag, fn)` returns a callable that auto-defers the body to the main thread; the runtime detector now aborts under `HELIX_STRICT_BG_THREAD_CHECK=1` (default in `HelixTestFixture`) so new Mechanism C anti-pattern instances fail tests immediately.
- **L081 Mechanism D (freeze-drop) telemetry watch + aggregator** — `telemetry-crashes.py` gains anomaly watch mode; `freeze-drops.sh` aggregates `DROPPED (shutdown)` events across Pi/K2/CC1 (busybox `logread` + `/var/log/messages`).

### Fixed

- **L081 Mechanism C: 107 background-thread callsites across the codebase** — LedController, PrintSelectPanel, belt_tension_calibrator (17 sites), 6 AMS backends, macro/print/preparation chains, UI panels (including real LVGL-from-bg bugs caught by the detector), wizards/modals/overlays, and `camera_stream` all now wrap LVGL touches in `tok.defer` or use `bg_cb`. The forbidden bare `if (tok.expired()) return;` followed by `this`/member access is now gated by lint + strict-mode runtime abort.
- **Layered filament-path renderer is now default; legacy renderer deleted** — the `HELIX_LAYERED_FILAMENT_PATH` scaffold shipped behind a flag last cycle; this release flips the default and removes the old single-pass renderer and `render_cache_` member. LINEAR/HUB topologies now split flow/heat/tip into the `DRAW_POST` pass; PARALLEL splits the animation into `DRAW_POST` as well. Per-setter dirty flags + cached rendered output skip redraws when nothing changed.
- **UpdateQueue::scoped_freeze buffers callbacks instead of dropping** — freeze now splices buffered work back on release; `defer_critical` / `queue_critical` removed (single path covers all cases). `freeze-drops.sh` watches `DROPPED (shutdown)` only — should be zero on builds ≥ 2026-05-11.
- **CFS hardware errors (key8xx) surface via `respond_raw` with recovery + purge-gate** — K2 box driver reports faults as `!!` log lines that `dispatch_action_script` swallowed silently. The modal upgrade for key8xx now offers the recovery chain and gates the purge action behind the user actually clearing the error.
- **AMS state shows "Printing" / "Paused" while a print is active** — status string was stuck on "Idle" because the print-state listener wasn't wired through the AMS card. Now reflects the live `print_stats.state`.
- **GCode viewer streaming mode honors per-tool palette** — the streaming path lost per-tool palette indexing during the v0.99.59 refactor; multi-color prints rendered in T0's color again. Now indexes by the slicer-recorded tool.
- **AD5X IFS tool-mapping caps when plugin is loaded** — `{true, true}` when lessWaste/bambufy active, `{false, false}` for native ZMOD; previously inconsistent.
- **Snapmaker AMS seeds identity `tool_to_slot_map` in constructor** — first-frame render before firmware ACK no longer shows an empty mapping.
- **Snapmaker U1 platform hook no longer kills `lmd` / `unisrv`** — those are camera/timelapse daemons, not competing UIs; killing them broke TIMELAPSE_START. Hook now auto-recovers if they were previously killed.
- **Installer finds `moonraker.conf` on AD5X ZMOD chroot paths** (prestonbrown/helixscreen#938).
- **Installer doesn't kill Snapmaker `unisrv` in competing-UI sweep** (same root cause as the U1 hook fix).
- **AMS filament-remap card hides on backends without editable mapping** — was showing a dead UI on AFC/Happy Hare setups.
- **AMS color_set uses explicit flag, not `color_rgb == 0` sentinel** — legitimately-black filaments no longer get treated as "unset." Companion to the IFS `check_external_color_change` switch from `0 = no-signal` to `optional`.
- **Print-select inlines post-`defer_critical` `MetadataUpdate`** — the metadata flag was getting stuck after a freeze drop; now escapes the freeze with an inline path and self-heals.
- **AMS step indicator anchored to physical heating + live temp label** — the indicator was floating relative to the wrong subject.
- **macOS nightly test gate** — `execute_gcode`-routing change halted the macOS CI gate; re-aligned.

### Changed

- **Calibration docs** — fixed wrong config path; expanded recovery options.
- **belt_tension_calibrator** — collapsed 17 trivial long-form `tok.defer` blocks to `bg_cb`.
- **AD5X IFS** — non-zcolor `gcode_response` lines now drop on the bg thread before the defer hop (less main-thread churn).

## [0.99.59] - 2026-05-09

The headline is **diagnostic instrumentation for the post-v0.99.58 cluster:pstat-async-delete recurrence**. Bundle 3XNZQB2R crashed via a corrupted per-widget event-handler callback — a corruption surface distinct from the global event_stack v0.99.55-56 fixed. v0.99.59 adds three runtime detectors that convert the crash into a recoverable anomaly + name the widget, and wraps the five Mechanism C anti-pattern callsites the bg-thread detector found on first AD5M deploy.

### Added

- **Three runtime detectors for cluster:pstat-async-delete (3XNZQB2R)** — (1) LVGL dispatch gate snapshots `dsc->cb` and validates against the binary's text segment, converting a corrupted callback into an `event_dsc_cb_oob` anomaly instead of a crash; (2) widget identity capture writes `event_target_class:` + `event_target_name:` lines in crash dumps so we get "lv_button (recovery_dismiss_btn)" instead of just hex; (3) `LifetimeToken::expired()` fires `bg_tok_expired_check` anomalies when the L081 Mechanism C anti-pattern is hit (alive token + non-main thread), with per-thread first-fire deduplication.
- **AMS CFS tool remap via `BOX_MODIFY_TN`** — pre-print options can remap a slot's tool number through the CFS endpoint when the user picks a different tool, with a toast on backend rejection.
- **Friendly hardware label in installer output** — installer prints "Raspberry Pi 4" alongside the `pi` platform tag so users can sanity-check the tarball before flashing.

### Fixed

- **5 background-thread Mechanism C callsites surfaced by the bg-thread detector on first AD5M boot.** `JobQueueState::fetch` was the only outright UAF — its success cb called `on_queue_fetched` which then did `lifetime_.defer()` from the bg thread (#707 race). `MacroModificationManager::check_and_notify` + `analyze_and_launch_wizard` did inline `ToastManager::show()` and member writes on the analyzer's HTTP thread. The other three (`PrintHistoryManager::fetch`, `PrintPreparationManager::analyze_print_start_macro_internal`, `NetworkSettingsOverlay::update_ethernet_status`) already used `tok.defer` for the actual mutation but kept a defensive `tok.expired()` bg-check; simplified to drop the bare check (defer's own guard handles expiry).
- **Multi-color prints sliced to a non-T0 initial tool no longer render in T0's color** — `GCodeLayerRenderer` indexed the palette by `palette[0]` instead of the slicer-recorded initial tool, so any non-T0 starting filament rendered as the wrong color until the first `T<N>` change. Now indexes by `metadata.initial_tool_index`.
- **`helixscreen.env` no longer overrides in-app Log Level setting** — the shipped `.env` had `HELIX_LOG_LEVEL=INFO` which silently shadowed the user's choice in Settings → Logging. Env var is now a fallback when the persisted setting is unset.
- **mdns thread re-assignment crash on rebroadcast (bundle UBZQ94EE)** — reassigned `responder_thread_` without joining; `std::terminate` if the prior responder was still finishing. Now joins first.
- **bed_mesh renderer mutex (bundle N3JTFPA5)** — pixel-format / size mutations weren't holding `render_mutex_`, racing the render thread reading the same fields → SEGV in `argb8888_image_blend`. Same Mechanism B shape as the v0.99.55 gcode_layer fix, applied here.
- **`AmsCurrentToolText` observer bound before `AmsState` subjects existed** — initialization ran during static init, leaving the observer pointed at a placeholder. Now invoked after `AmsState::init_subjects()`.
- **AMS slot color-dot label was empty for `current_tool`-only setups** — `set_current_tool` now mirrors the value to `current_slot` so the color-dot widget renders.
- **PrintStatusPanel double-attached its collector when reconnecting mid-print** — second collector double-counted filament usage. Now suppressed when the active-job state arrives via reconnect.
- **Print-start remap restore deferred until after crash-recovery modal closes** — `restore_user_remap()` raced the modal's tear-down. Now hops via `queue_update`.
- **Installer service `Group=` resolution** — assumed `User==Group`, broke on Armbian configs where the install user is in a different primary group. Now resolves via `id -gn ${INSTALL_USER}`.
- **KIAUH `get_confirm()` keyword arg** — 6.2.x tightened the signature; positional `default_choice` now `TypeError`s. Pass as keyword.

### Changed

- **Filament-color metadata parsing consolidated** — gcode-thumbnail and gcode-viewer paths both had their own copies of `;filament_colour =` parsing. Single helper in `GCodeMetadata::parse_filament_colors()`. Behavior unchanged.

## [0.99.58] - 2026-05-08

### Fixed

- **Silent Moonraker request loss during connect/reconnect (#909)** — libhv's `WebSocketClient::send` only checks that the channel exists, not that the WS protocol is past `CONNECTING` / `WS_UPGRADING` / `RECONNECTING`. A send issued in those windows wrote WS frame bytes onto a stream Moonraker treats as still-handshaking, so the bytes were silently dropped and the request sat in `MoonrakerRequestTracker::pending_requests_` until the 60s timeout — or forever, if the calling panel had already cleared its in-flight flag. Surfaced as the K2 Plus startup race where `PrintSelectPanel::refresh_files` issued `get_directory` before `onopen` and the panel stuck on its refresh spinner for hours with no timeout warning. `MoonrakerClient::ready_to_send()` now gates all four `send_jsonrpc` overloads on `connection_state_ == CONNECTED`; the 5-arg overload fires the caller's `error_cb` synchronously with `CONNECTION_LOST` so panels see immediate failure instead of silence. Adds debug-visibility instrumentation to the tracker's timeout sweep so future regressions surface as a stuck queue in bundles.
- **Background Moonraker/HTTP callbacks now marshal LVGL work to the main thread (#933)** — several success/error callbacks in `PrintStatusPanel` (gcode-for-viewing download, reprint error path) and `CrashReportModal::send_with_bundle` captured a lifetime token and checked `tok.expired()` but ran the body inline on the WebSocket / HttpExecutor worker thread. Bodies that touched LVGL widgets (`safe_delete`, `lv_qrcode_create`, modal `hide`, `ui_set_button_enabled`) raced the render loop and produced the L081-cluster heap corruption surfacing as SIGSEGV in `get_prop_core` / `layout_update_core` the next frame. All affected callbacks now wrap their LVGL touches in `tok.defer(...)` so the work runs on the main thread. Telemetry: WKC5J9SK (ad5x v0.99.56), B4SG5M79 (k1 v0.99.53).

## [0.99.57] - 2026-05-07

### Added

- **Shutdown / Reboot rows in Advanced settings** — adds a POWER section to the Advanced panel with Shutdown and Reboot rows that open the same ShutdownModal the home-panel power widget uses. Single-/dual-host scope, Moonraker-disconnect → local `SystemPower` fallback, and the deferred screen-on-ack ordering for the *Both* flow are preserved. The widget's `handle_click` and its six `execute_*` methods are extracted into a free `helix::show_shutdown_dialog()` helper so the widget and the panel rows share one wiring instead of duplicating it.

### Fixed

- **Multi-tool prints sliced to a non-T0 tool no longer render in T0's color** — in streaming mode (large gcode files on memory-constrained hosts), `GCodeStreamingController` parsed each layer with a fresh `GCodeParser`, so the `T<N>` issued in the file's prologue was invisible to per-layer parses. Every segment was tagged `tool_index=0` and resolved to `palette[0]`. On a Voron + AFC setup with black ASA in lane 1 and a print sliced to T3 (white PLA), this manifested as a solid-black render that looked like *no color applied* but was really *every segment tagged with the wrong tool*. `GCodeLayerIndex::build_from_file` now records the first standalone T-command during its single-pass scan; `GCodeParser::set_active_tool_index()` seeds the parser before each per-layer parse; and the `extruder_colour` single-color fallback now uses the initial tool's color instead of `palette[0]`. Also drops the `!= 0x000000` clobber in `apply_ams_tool_colors` that was wrongly treating legitimately-black filament as *unset*.
- **KIAUH extension index** — KIAUH 6.2.x added *Klipper Adaptive Meshing Purging* at index 14, so the helixscreen extension was skipped with a duplicate-index warning on load. Bumped to 15, then to 99 to dodge any further upstream collisions.
- **KIAUH update / remove now finds the installed binary** — `find_install_dir()` only probed `<dir>/helix-screen`, but releases ship the binary at `<dir>/bin/helix-screen`, so update and remove always reported *helixscreen not installed* on a working install. Probes the release path first, falls back to the top-level for any pre-1.0 layout.

### Changed

- **Installer flag: `--skip-kiauh-registration` replaces `--kiauh yes|no`** — the old flag read awkwardly when KIAUH passed `--kiauh no` to invoke install.sh. Single boolean is cleaner. `install_kiauh_extension` is also now idempotent: when target files match the source byte-for-byte, it logs *already up to date* and preserves mtimes instead of needlessly rewriting files KIAUH already owns at the right version. Bats test added for the no-op path.

## [0.99.56] - 2026-05-06

The headline is the **structural fix** for the cluster:pstat-async-delete crash family — v0.99.55 already closed the render-thread half (#929), and v0.99.56 closes the other: an **array-backed event stack** that replaces LVGL's linked-list `event_head` with a global array, eliminating the wild-pointer dereference path that's been the dominant production crash signature for weeks (#793/#840/#871/#878/#880/#906). Plus a defensive NULL-guard layer on `lv_obj_event.c` for the new VHTR49QJ signature, and a suppress-the-dialog fix for truncated crash files that were generating useless bundle submissions. Round it out with a **K2 phase-tracker fix** that stops mis-flagging BED_MESH on profile-load echoes, a **theme-init DPI fix** for BTT CB1 / sun4i framebuffers reporting bogus `width_mm`, and a **label-printer font-fit fix** for longer vendor strings on continuous tape.

### Fixed

- **Array-backed LVGL event stack (#907 — cluster:pstat-async-delete structural fix)** — `lv_event_mark_deleted` previously walked a linked list of stack-allocated `lv_event_t` via `e->prev` pointers chained through `event_head`. Any frame returning without popping (exception unwind, control-flow bug) stranded a stack pointer that the next walk would dereference, often after heap reuse had written ASCII bytes over it — the dominant crash signature in #793/#840/#871/#878/#880/#906. The chain is now a fixed `lv_event_t * event_stack[256]` in `_lv_global_t`; mark_deleted iterates the array and never dereferences `e->prev`. Per-slot stack-bounds + alignment validation (inherited from the v0.99.54 dispatch-depth-guard work) catches any remaining stomp into the array. Also consolidates the file-static `event_dispatch_depth` into `_lv_global_t::event_depth` so post-mortem heap snapshots can read it.
- **NULL-guards on `lv_obj_event.c` accessors (VHTR49QJ defense)** — `lv_obj_get_event_count`, `lv_obj_get_event_dsc`, `lv_obj_remove_event`, `lv_obj_remove_event_dsc`, `lv_obj_remove_event_cb`, and `lv_obj_remove_event_cb_with_user_data` all had release-mode `LV_ASSERT_NULL` (a no-op) on the obj argument. Bundle VHTR49QJ (v0.99.53/ad5x, 2026-05-04) crashed with `fault_addr=0x0` reaching the field load via a deferred async callback that didn't validate widget lifetime. Replace the ASSERT with a runtime check that bails to a sane return value and surfaces telemetry — matches the established `lvgl_observer_null_guards` pattern.
- **Suppress crash dialog for unparseable crash files (CHUQCNAE)** — bundle CHUQCNAE shipped a `crash.txt` containing only `signal:\n` after the signal handler was killed mid-write (OOM / watchdog / power loss). Application::run was still showing the crash report dialog and letting the user submit a useless empty bundle. Now consumes the file and skips the dialog when `read_crash_file` returns null. Regression test pins the contract.
- **K2 phase tracker no longer false-fires BED_MESH on profile load** — K2 `PRINT_START` emits `BED_MESH_PROFILE LOAD="default"` early in pre-print to reuse the saved mesh (no calibration). The profile's BED_MESH regex matched these echoes and falsely transitioned to BED_MESH for several seconds before HEATING_BED took over. Reduces the alternation to `BED_MESH_CALIBRATE` (still covers `BED_MESH_CALIBRATE_START_PRINT`). Also drops three dead profile entries that logged warnings on every load (signal_format with `pattern` instead of `prefix`; HEATING_CHAMBER and LOADING_FILAMENT response patterns referencing phases not in the `PrintStartPhase` enum).
- **TempGraphWidget null-config crash on legacy home-panel layouts** — `set_config` was reading `config.value("follow_overlay", false)` without guarding against JSON null. Layouts saved before v0.99.54 omit `"config"` and `parse_widget_array` hands a default-constructed (= JSON null) config to widgets, throwing `json::type_error::306` from `HomePanel::finalize_setup`. The throw escaped `run()` before v0.99.55's main_loop catch could absorb it, exiting 134. Reported by d0u8l3m, who originally requested the follow-overlay feature.
- **Theme DPI defaults to LV_DPI_DEF on bogus width_mm reports** — on BTT CB1 (sun4i-drmdrmfb) the kernel reports the 800×480 panel as 890mm × 500mm (off by ~6×), making LVGL compute DPI=23. With sub-160 DPI, `LV_DPX_CALC` clamps `PAD_SMALL` to 1px, collapsing dropdown/input padding. Now forces `lv_display_set_dpi(disp, args.dpi > 0 ? args.dpi : LV_DPI_DEF)` before theme init so widget padding survives garbage display metadata. The existing `if(width > 0)` guard in `lv_linux_fbdev.c` only catches drivers that admit ignorance, not drivers that lie.
- **Continuous-tape label fonts shrink to fit longer vendor strings** — vendor names like PRUSAMENT / POLYMAKER were truncating on 62mm tape. Drops phantom `avail_h` from 300 to 260 (STANDARD scale becomes lg=5 / md=4 / sm=3, was 6/5/4), tightens the horizontal-fit floor from 4 to 8 chars, and applies the narrow-tape margin/QR shrink to 29mm and 38mm continuous (was die-cut only).

## [0.99.55] - 2026-05-06

v0.99.54 shipped a regression and a returning crash signature; v0.99.55 closes both. The bigger story is the **Klipper error UX overhaul** — gcode errors used to surface as raw debug text like *retrude error, retrude but not trigger buffer empty limit*; now they map through a friendly-message table with a one-tap recovery path that tries the K2's deeper `helix_recover` shell_command before falling back to `firmware_restart`. Pairs with a busybox `/sbin/reboot` fallback that finally lets the shutdown widget actually work on K2/AD5M/CC1/K1C/SonicPad. Plus a **pre-print phase tracker** that turns the K2's generic 5-minute *starting print* spinner into BED_MESH / HEATING / FILAMENT_LOAD / PURGE phase progress, and a long-overdue **installer fix** that ships the KIAUH extension in the tarball (silently broken since v0.99.34).

### Added

- **Friendly Klipper error toasts with one-tap recovery** — Klipper emits errors as JSON like `{"code":"key851","msg":"retrude error..."}` where `msg` is raw debug log text. The pipeline now extracts the `code`, looks up a friendly message+hint via `CfsErrorDecoder::lookup_message()`, and routes recovery through `PrinterRecoveryService` — which tries the K2's `helix_recover` `[shell_command]` (bouncing the klipper_mcu daemon) before falling back to `printer.firmware_restart`. C++ stays platform-blind: each platform installs its own snippet via `scripts/lib/installer/recovery.sh`. key851 now displays as *Retract didn't reach buffer empty limit* instead of the raw debug string.
- **CFS error toasts include unit/slot locator** — JSON `values` arrays (e.g. `[1,"B"]` for a key849 retract failure) are stringified into a human locator and appended, so users see *which* CFS slot failed.
- **Pre-print phase tracker (K2/CFS)** — `PrintPhaseTracker` exposes coarse pre-print phases (BED_MESH, HEATING, FILAMENT_LOAD, PURGE) as LVGL subjects, replacing the generic *starting print* spinner that left K2 users staring through ~5 min of bed-mesh + heating. Inputs: `print_stats.state` plus the `notify_gcode_response` tag stream (`// [PRTOUCH_MOVE]`, `// [PROBE_STEP_INFO]`, `// [G29_TIME]`, `// [box]`). Per-firmware-family matchers — adding Bambu/RatOS/K1 variants is a sibling matcher, no parser surgery. Mirrored into the legacy `print_start_phase` / `print_start_message` / `print_start_progress` subjects so the existing `preparing_overlay` UI shows the progression with no panel rewire.
- **Shutdown/reboot widget works on busybox/procd hosts** — `SystemPower::reboot_local()` and `shutdown_local()` now fall through from logind/systemctl to `/sbin/reboot` and `/sbin/poweroff`. K2/AD5M/CC1/K1C/SonicPad shutdown buttons now actually do something (procd's sysinit hook handles graceful service teardown on those calls).

### Fixed

- **Crash loop on v0.99.54 startup** (#931) — `Application::main_loop()` now wraps each iteration in `try/catch`. When a callback throws, the type and `what()` are logged, the recent breadcrumb ring is dumped to stderr (so the next user log captures the throw site), telemetry records the event, and the loop continues with a user-facing error toast. A runaway counter (5 catches in 30s) breaks out cleanly to avoid a tight throw-catch loop. Removes the watchdog's *HelixScreen Keeps Crashing* path for any callback throw that previously exited 134.
- **MoonrakerRequestTracker error-callback path** — symmetrized exception handling: `route_response()` already absorbed throws from success callbacks, but error callbacks were unwrapped. A throwing `error_cb` now logs and continues (matching the success path).
- **Render-thread UAF in argb8888_image_blend** (#929, cluster:pstat-async-delete Mechanism B) — `GCodeLayerRenderer::destroy_cache()` / `destroy_ghost_cache()` / `destroy_ssao_cache()` and `GCodeGLESRenderer::~GCodeGLESRenderer()` (plus the resize-time recreate path) all now call `lv_draw_wait_for_finish()` before `lv_draw_buf_destroy()`. The parallel render thread reads `dsc.src` after `lv_draw_image()` returns; freeing the buffer while a draw task was still in flight unmapped the source page and segfaulted in `argb8888_image_blend`. The wait drains pending tasks before the buffer goes away. Single-threaded builds (`LV_USE_OS == 0`) are unaffected — `lv_draw_wait_for_finish` is a no-op there.
- **CFS load no longer transitions to IDLE prematurely** — `AmsBackendCfs` was flipping `action=IDLE` the moment the toolhead extruder `filament_switch_sensor` triggered, but the K2 load script has 5 steps (CR_BOX_PRE_OPT → EXTRUDE → WASTE → FLUSH → END_OPT) and the sensor trips at the END of step 2. The remaining WASTE + FLUSH (~109 mm at 240 °C, ~3 min) ran while the UI told the user the load was idle. IDLE now defers to script completion, and the toolhead is parked during load.
- **No more execute_gcode spam while Klipper is halted** — with `klippy_state=ERROR` after a key298 rpi-MCU shutdown, wiggling the fan slider previously fired ~30 M106 commands per tick that Klipper rejected on each attempt. Now refuses early when state is SHUTDOWN or ERROR; recovery RPCs (`firmware_restart`, `emergency_stop`, `shell_command`) are unaffected.
- **Phase tracker forward-only ordering** — K2 `[PROBE_STEP_INFO]` (mesh probing) and `[WHY_DEBUG]target_temp` (heating) interleave during early mesh because bed heat ramps in parallel; the tracker no longer flips back from HEATING → BED_MESH on the late probe lines.
- **Phase tracker maps PRINTING → IDLE for legacy overlay** — the legacy `preparing_overlay` panel observer treats any non-IDLE `PrintStartPhase` as *still preparing*; mapping PRINTING → COMPLETE was leaving the overlay stuck up for the rest of the print.
- **Print-start no longer enters BED_MESH from QGL/Z_TILT probe lines** — Voron 2.4 PRINT_START runs QGL before BED_MESH_CALIBRATE. QGL probes 4 corner pads with `samples=3` = 12 *probe at X,Y* lines, which tripped the collector's `MESH_PROBE_ENTRY_THRESHOLD=3` and mis-promoted the phase to BED_MESH (off-by-12 probe count, QGL phase invisible). Collector now ignores probe lines until BED_MESH is announced.
- **Installer KIAUH extension actually ships in the tarball** — silently broken since v0.99.34: release tarballs (built by `mk/cross.mk`) never copied `scripts/kiauh/` into the package, so `install_kiauh_extension()` always hit *source files not found* and bailed. Now copied into all 10 platform release targets. Also closes a PII leak — release tarballs were shipping any `telemetry_*.json`, `tool_spools.json`, `crash_report.txt` present in the dev tree (only `settings.json`/`helixconfig*.json` were stripped). And collapsed the longstanding drift between `install-dev.sh` and the bundler's MAIN heredoc into `scripts/lib/installer/main.sh` (sourced by both, install-dev.sh shrinks 295→48 lines).
- **Documentation gallery images render on github.com** (#928) — `GALLERY.md` lives at `docs/devel/` but images live at `docs/images/`; the bare `images/` refs resolved to a non-existent `docs/devel/images/`. Reported by @TMTYD.

### Changed

- **PrintPhaseTracker unified into PrintStartCollector** — internal refactor consolidating the two pre-print tracking surfaces into a single owner.
- **Keyboard per-release event log demoted to trace** — was cluttering `-vv` debug output during normal typing.

## [0.99.54] - 2026-05-04

The follow-up to last week's hotfix. Headline pieces: the **L081 unwind-safe `lv_event_pop`** fix that landed eleven hours after v0.99.53 tagged (so it missed that release), plus a second layer of defensive guards in `lv_event_mark_deleted` for the **cluster:pstat-async-delete** crash family (#906) that's been the dominant production signature since 4/29. The **pre-print options framework** lands across four phases — a uniform contract for every per-printer pre-print Klipper option (K2 Plus AI detect, K1C/SonicPad bed leveling, etc.) replacing the old ad-hoc per-option JSON. Round it out with **in-place fan-carousel dial drag** (no more popping a separate fan-control overlay), **AD5X-IFS hardening** (stale plugin data, custom materials, lane-data sync on external CHANGE_ZCOLOR), the long-pending **L083 std::thread audit** wrapping every remaining bare detached spawn, and a **memory-leak fix** for AMS detail panel.

### Added

- **In-place fan-speed control** — drag the arc on a fan card in the Controls fan carousel to change the speed directly. No more bouncing into a separate fan-control overlay for the common case.
- **"Follow my graph selection" toggle on the temperature-graph card** — when on, dragging through the time axis pins the readout to the touched sample instead of always tracking live.
- **Pre-print options framework (Phases 1-4)** — every per-printer pre-print option (K2 Plus AI detect, bed leveling, calibration prompts, etc.) now flows through a uniform `PrePrintOption` contract. Phase 1 introduced the framework, Phase 2 migrated the JSON DB and all call sites, Phase 3 rebuilt the UI to generate rows dynamically with category subheaders, Phase 4 added K2 Plus `ai_detect` via the `PreStartGcode` strategy.
- **moonraker-timelapse component detection** (#926) — the Settings row appears for users with a pre-existing moonraker-timelapse install (MainsailOS, manual config), not just those who ran the in-app install wizard.
- **Favorite-macro home-screen tap honors safety settings** (#925) — dangerous-macro confirmation prompts now fire from the home-screen tap path the same way they do from the macros panel.
- **Debug bundle now captures platform config files + CFS Klipper objects** — bundles for K2 / Pi printers carry the full Klipper objects payload for CFS units.

### Fixed

- **LVGL event-dispatch unwind safety (L081 root cause)** — when a C++ exception unwinds through `lv_obj_send_event` or `lv_event_push_and_send`, the explicit `lv_event_pop` was being skipped, leaving `event_head` pointing at a defunct stack frame. The next `lv_event_mark_deleted` walk SIGBUSed reading garbage. Patched both entry points to use GCC `__attribute__((cleanup))` (with `-fexceptions` enabled on LVGL TUs) so pop fires on exception unwinds too. Closes the design gap behind crash families #793/#840/#871/#878/#880 and a substantial fraction of the cluster:pstat-async-delete (#906) hits.
- **Cluster:pstat-async-delete defensive guards (#906)** — `lv_event_mark_deleted` now layers two additional checks: a dispatch-depth coherence guard (logs `event_head_stale_leak` anomaly if `event_head` is non-NULL with no active send frame) and a stack-bounds check (logs `event_head_stack_oob` if the chain pointer is outside the current thread's stack range). Both reset `event_head` and bail safely. Catches corruption modes the cleanup-attribute path can't address (heap reuse, memory stomp). Plus per-step breadcrumbs in `PrintStatusPanel::on_activate` so the next field crash names which step set up the corruption. Breadcrumb ring expanded 128→256 to keep async_d/sync_d entries from rotating off.
- **L083 thread spawn audit completes** — every remaining bare `std::thread(...).detach()` site is now wrapped in try/catch surfacing a toast on `std::system_error` from `pthread_create` EAGAIN. Prevents `std::terminate` aborts under thread exhaustion on resource-constrained ARM (AD5M, K1C, MIPS targets).
- **AD5X-IFS stale plugin data + custom materials** (#904) — Adventurer5M.json reads now guard against stale lessWaste/bambufy `save_variables` rows that persist after plugin uninstall, and user-defined materials in `/mod_data/user.cfg` now surface in the slot picker alongside built-ins.
- **AD5X-IFS sync on external CHANGE_ZCOLOR** — when the user runs `CHANGE_ZCOLOR` from Mainsail/Fluidd or the AD5X touchscreen, HelixScreen now syncs `lane_data` and `_IFS_VARS` to match instead of drifting out of agreement.
- **AD5X auto-detect on ZMOD firmware** — was misdetected as AD5M Pro on certain ZMOD revisions.
- **AMS detail panel destroyed on close** — was holding ~MB of widget state for the lifetime of the app.
- **AMS bypass spool position** — pinned to the canvas tube line on size-changed events instead of drifting on resize.
- **AD5X-IFS post-print runout toast suppression** — was firing every print-end since the IFS auto-unloads after the job; now only triggers on real runout.
- **Thermistor widget rebinds temp observer when sensor name changes** (#916) — the bound subject is dynamic; the widget now uses a paired `SubjectLifetime` member so observer cleanup matches the dynamic-subject pattern.
- **Fan-carousel arc value-changed no longer records spurious interactions** — the auto-debounce was treating programmatic arc updates as user input.
- **FilamentConsumptionTracker stopped before `destroy_all` on shutdown** (#927) — closes a use-after-free observed during AD5X tear-down.
- **Print Select self-heals stuck refresh_in_flight_ flag after 30s** — a never-arriving file-list response could lock out subsequent refreshes; a watchdog now clears the flag.
- **Splash process orphaned on DRM platforms (Snapmaker U1)** — `helixscreen.init` starts `helix-splash` early before the display backend is selected, but `helix-watchdog`'s "skip external splash on DRM" branch (added because fbdev splash mmap conflicts with DRM) only avoided spawning a *new* splash; the externally-adopted PID was silently dropped, never forwarded to helix-screen via `--splash-pid`, and never reaped. Splash kept running after the UI took over the display, burning ~60% CPU on a single core forever (rockchipdrmfb's fbdev compat layer kept it from failing). Watchdog now reaps the orphan when DRM is selected. Belt-and-suspenders: `cleanup_splash` escalates to SIGKILL after a 600 ms SIGTERM grace, and the splash binary self-exits after 30 s if no signal arrives.
- **Moonraker component-capability flags cleared on each discovery** — stale capability bits from a prior connection no longer leak into a new printer's profile.
- **Favorite-macro context leak on backdrop/ESC dismiss + i18n confirm body** (#925) — the ButtonCallbackData was leaking when the picker was dismissed via backdrop tap or ESC.
- **Macros-panel dangerous-macro confirm body** — i18n keys hooked up and the body text formatted for readability.

### Changed

- **Color swatches and filament-mapping cards driven by subjects** — converted from imperative C++ visibility toggles to declarative XML `bind_flag_if_eq` against a subject. Removes the last `can_show_*` zombie subjects from `printer_state.cpp`.
- **Debug bundle HTTP fetch + sanitization factored out**; `user.cfg` redaction fixed (was leaking certain custom-material entries verbatim).
- **AD5X re-reads `Adventurer5M.json` on AMS detail-view entry** — picks up out-of-band edits from the AD5X firmware native UI without requiring a HelixScreen restart.

## [0.99.53] - 2026-04-28

A hotfix release. The headline fix: **v0.99.51 and v0.99.52 could brick AD5X stock-ZMOD printers** by corrupting `Adventurer5M.json` to zero bytes through a Moonraker upload that crossed mount boundaries. On the next firmware restart, Klipper's zmod_color.py couldn't parse the empty file and never finished `connect` — recovery required SSH. Both releases have been pulled from the GitHub release feed; v0.99.53 ships the corrected write path. While we were here, this release also lands the per-printer gcode console filter system, OrcaSlicer-compatible bed/nozzle temperature emission to `lane_data` slot records, and a small theme polish.

### Added
- **Per-printer gcode console filter** — every printer can have its own customizable filter list for the gcode console panel. Settings overlay lets you add/remove patterns and toggle the filter on/off without rebooting; defaults are seeded from the printer database so common boilerplate (heartbeat polls, M105 noise) is suppressed out of the box.
- **OrcaSlicer-compatible bed/nozzle temperatures in `lane_data`** — HelixScreen now emits `bed_temp` and `nozzle_temp` on every filament-slot save so OrcaSlicer 2.3.2+ can sync them to its filament presets. Source priority: explicit user entry > bound Spoolman spool's profile > internal material database default. Spec docs bumped to v1.1.

### Fixed
- **AD5X stock-ZMOD bricked at boot from corrupted Adventurer5M.json** — v0.99.51 switched IFS color/material persistence from `CHANGE_ZCOLOR` (which pops a Mainsail/Fluidd "Select print materials" prompt on every edit) to a Moonraker HTTP upload of `Adventurer5M.json`. On AD5X stock-ZMOD, `/root/printer_data/tmp/` and `/usr/prog/config/` are on different mounts and Moonraker's config root entry symlinks across them; the upload's `os.rename(tmp → dest)` throws `EXDEV` and the destination ends up empty. On the next firmware restart, zmod_color.py's `json.load(file)` raised `JSONDecodeError`, klippy never finished `connect`, and the printer was unrecoverable without SSH. Bundle DQK7X96B and DIEHARDave's report on Discord. Fix: when helix-screen runs on the same host as Moonraker (the typical install), skip the upload and write `Adventurer5M.json` directly via filesystem APIs with an atomic temp-then-rename inside the destination directory. The Moonraker upload path remains as a fallback for remote/screen-only installs that genuinely aren't on the same machine.
- **ActionPromptModal footer divider used hardcoded color** — switched to the border design token so it tracks theme.

## [0.99.52] - 2026-04-28

A bug fix release dominated by killing a class of crash loops triggered by Moonraker subscription-restricted JSON nulls. Klipper objects don't always implement every field a subscription names; Moonraker faithfully delivers `null` for the missing ones, and several of our parsers were calling `.get<T>()` straight through, throwing `type_error.302` and unwinding into `main()`'s top-level catch (exit 134). Every restart hit the same bug, and the watchdog couldn't break out. Now: every parser type-guards before extracting, the dispatch path absorbs callback throws so one rogue parser can't take down the others, a CI lint gate fails any new violation, and the watchdog detects same-signature crash loops (≥3 in 90s) and offers a Safe Mode boot that defers the Moonraker connection so the user can reach Settings.

### Added
- **Watchdog Safe Mode boot** — when the same crash signature repeats 3+ times in 90s (typical of a deterministic state-driven crash), the recovery dialog switches to a loop-aware variant with the auto-restart countdown disabled and "Safe Mode" as the primary button. Picking it writes a one-shot marker, restarts the app, and Application defers the Moonraker connection on the next launch with a sticky banner. A clean reboot exits Safe Mode automatically.

### Fixed
- **Subscription-restricted-null crash loops** — Moonraker delivers `null` for subscribed fields the underlying Klipper object lacks (Snapmaker U1's `filament_motion_sensor.detection_count` is the prototypical case). Multiple parsers used unguarded `.get<T>()` that threw `json::type_error::302` on null, unwinding past the dispatch loop and exiting 134. Hardened `printer_state.update_from_notification`, `printer_print_state.update_from_status`, sensor parsers, `led_controller` color channels, and `filament_sensor`. The initial-subscription apply path (`MoonrakerClient::dispatch_status_update`) now also wraps each callback in try/catch so one throwing parser can't take down the rest. A CI lint gate (`scripts/check_subscription_null_safety.py`) prevents regressions.
- **AD5X panel widget rebuilds could abort under memory pressure** — the panel widget manager allocated a small `AsyncCtx` struct per gate-observer firing and queued it via `lv_async_call`. On memory-tight AD5X (~107MB RAM) a `std::bad_alloc` here propagated through LVGL's C event-dispatch frame and aborted the process via `std::terminate`, surfacing as unrelated-looking crashes (L083 family). Now uses a stable per-panel slot stored in the manager singleton — no per-firing allocation in the hot path.
- **AD5X IFS slot overrides wiped on boot** — on startup, `parse_save_variables` ran `update_slot_from_state` for every slot before `Adventurer5M.json` had been fetched. With no firmware color reading yet, the swap-detection helper saw the `SlotInfo` default (0x808080 gray) as the baseline; seconds later when the real color landed it was misread as a "physical spool swap" and the user's override was cleared (brand, spoolman_id, weights, material). On the next boot 0 overrides loaded because boot 1 had wiped them. raza616 caught the symptom: PETG slot flipping back to firmware-truth PLA on every power cycle. Fix: treat empty `colors_[idx]` as a no-signal reading so the baseline is only established from real firmware data. Bundle AQ6DALWG.
- **AD5X IFS: stale lessWaste/bambufy save_variables ignored after plugin uninstall** — `parse_save_variables` was reading `<prefix>_tools` / `_current_tool` / `_external` unconditionally whenever the keys were present. Moonraker's `save_variables` rows persist in `printer_data/database/` long after a plugin uninstall removes the gcode macro, so a user who removed lessWaste/bambufy would silently keep using the dead plugin's last tool map and active-tool guess on every boot. Now those reads are gated on `has_ifs_vars_` (= macro confirmed loaded), matching the contract the latch already advertised.
- **G-code viewer renderer stalls now recover instead of going blank** — added a renderer-stall watchdog with cache-state logging; on detected stall the viewer reinitialises rather than freezing on a blank or partial render.
- **Niimbot B1 cancelled print before the printer started moving** — the BLE PrintEnd packet was being treated as job-finished too early; now we wait for actual motion before allowing the cancel path to close the job.
- **AD5X stock ZMOD: GET_ZCOLOR spam quieted** — the ZMOD slot poller was firing the macro every tick instead of only on JSON content change. Now polls the JSON contents and only emits the macro when something changed.
- **AD5X installer refuses to run outside ZMOD chroot** — preflight now bails with a clear error rather than silently corrupting a non-ZMOD AD5X install.
- **Snapmaker U1 install: self-heal stale S99screen patches** — old patched init scripts from prior installs are detected and replaced rather than skipped.
- **ALSA buffer-underrun spam on Pi** — continuous underrun warnings during sound playback were burning log volume; the underrun path now backs off cleanly.
- **Label printer: faster BlueZ unpair + truthful Forget toast** — the Forget action would hang on BlueZ's slow unpair, then toast success even when the unpair had failed. Now uses a faster path and surfaces real status.

### Changed
- **Debug bundle exposes canonical platform_model** — bundles now include `printer.platform_model` (derived from the build-time platform key) alongside the user-picked `printer.model`, plus a `platform_model_mismatch` flag when they disagree. Lets the triage dashboard surface AD5X-running-as-AD5M-Pro mismatches without trusting the wizard pick blindly.
- **Shutdown observer-release contract test generalised** — the structural check that caught #888 is now table-driven; new singletons destroyed after `StaticSubjectRegistry::deinit_all()` can be added with one row.

## [0.99.51] - 2026-04-28

A targeted fix release. K2 install path works end-to-end now: a procd shim makes the autostart actually fire (devices were sitting at the boot logo on fresh install), uninstall re-enables Creality's stock UI, and a 68s boot trim gets K2 Plus into HelixScreen in ~36s. AD5X stock-ZMOD users get IFS active-slot tracking via GET_ZCOLOR, and slot edits no longer pop Mainsail/Fluidd "Select print materials" prompts or native touchscreen confirmation dialogs on every sync.

### Fixed
- **K2 install autostart never fired** — K2 (Tina Linux / OpenWrt procd) silently skips plain SysV init scripts at boot; procd's iterator only invokes scripts with both the rc.common shebang and a DEPEND directive. Our shared S99helixscreen had neither. Install now writes a procd-compatible shim at `/etc/init.d/helixscreen` that delegates to S99helixscreen, so K2 (Pro/Plus/Max) actually starts HelixScreen at boot instead of sitting at the boot logo. Reported by jacekruf (K2 Pro, F012 board).
- **K2 uninstall left device with no UI** — `hooks-k2.sh` runs `/etc/init.d/app disable` on every helix-screen launch (suppressing the stock procd-managed UI), but the modular uninstaller had no K2 case to re-enable it. Devices booted into the Creality logo with no UI after uninstall. Now enable+start runs against `/etc/init.d/app` on K2 series.
- **AD5X stock ZMOD: active loaded slot not tracked** — `parse_zcolor_silent()` correctly extracted `extruder_slot` from GET_ZCOLOR but `apply_zcolor_result()` dropped the field. For users without lessWaste, `active_tool_` stayed at -1 forever and the panel showed stale slot info on unload. Now derived from `extruder_slot` when no IFS vars are present. Reported by raza616.
- **AD5X stock ZMOD: every slot edit popped two dialogs** — `CHANGE_ZCOLOR` always emits a Mainsail/Fluidd "Select print materials" prompt and (on `display=True` setups) an HTTP-driven confirmation dialog on the native AD5X touchscreen. Both fired every time HelixScreen synced a slot — QR scans, Spoolman pulls, tool-mapping changes. Now writes `Adventurer5M.json` directly (zmod's source of truth) and only fires `_IFS_VARS` for lessWaste users. Confirmed by DIEHARDave.

### Changed
- **K2 boot time trimmed by 68s** — empirical traces on the dev K2 Plus showed `platform_wait_for_services` was treating its 30 timeout as iteration count (not seconds) and `platform_pre_start`'s WiFi association ran synchronously. Now: timeout tracks wall time via `date +%s` and the wpa_supplicant + udhcpc dance is backgrounded. K2 Plus fresh boot: 104s → 36s; most of the remaining 30s is honoring the service-wait budget while Moonraker comes up.
- **i18n** — upgrade banner and shutdown-modal Reboot/Shutdown buttons newly tagged for translation; 15 strings translated across de, es, fr, it, ja, pt, ru, zh. All locales now at 100% coverage (1940/1940).
- **Settings: printer host description binding** — moved from imperative C++ (`lv_obj_find_by_name` + `lv_label_bind_text`) to declarative `bind_description` attribute, aligning with the project's "DATA in C++, APPEARANCE in XML" rule. Internal cleanup; no behavior change.

## [0.99.50] - 2026-04-27

A perf and stability patch. Headlines: animated screensaver now defaults to Off on BASIC (Pi 3B-class) and EMBEDDED (AD5M / AD5X) tiers to keep the CPU out of Klipper's print loop, with a one-time migration notice and reduced frame rate when users re-enable on those tiers; another sweep of L081-family teardown crashes (`lv_event_mark_deleted`); plus targeted fixes for AD5X IFS color sync, AFC `LANE_UNLOAD` serialization, and the Z-offset probe sequence. Re-tagged after the initial CI release build broke on cross-compile targets and a SonicPad user reported the shutdown widget was unreachable when Moonraker failed to connect.

### Fixed
- **Release build broken on cross-compile targets** — the v15→v16 screensaver migration referenced `PlatformCapabilities::detect()` and `platform_tier_to_string()` unconditionally in `config.cpp`. Those symbols ship in helix-screen but not in `helix-splash` / `helix-watchdog`'s extra-obj lists, so every cross-compile target (pi32, ad5m, etc.) failed at link time on the auxiliary binaries. Both call sites are now gated behind the existing `HELIX_SPLASH_ONLY` / `HELIX_WATCHDOG` guard — splash/watchdog don't render the screensaver and treat the migration as a no-op.
- **Shutdown widget unusable when Moonraker can't be reached** — feedback from a SonicPad user: when Klipper failed to boot, the WebSocket never connected and the shutdown button stayed disabled, leaving the hardware switch as the only way to power off. The button is now always clickable; in same-host single-scope mode (helix-screen and Moonraker on the same device) the buttons fall back to `SystemPower::shutdown_local()` / `reboot_local()` (logind → systemctl) when the RPC isn't available. Dual-scope users on a remote screen already had a Screen-only path; ungating the button makes it reachable when the printer is unresponsive.
- **K1C boot logo persisted on SimpleAF firmware** ([#890]) — `boot_display` was only killed when `/etc/init.d/S99start_app` existed, but SimpleAF removes that script while keeping `S12boot_display` in place. The Creality boot logo now stops on all K1 firmware variants when HelixScreen takes over.
- **AD5X identify-wizard sleep regression** — removed the wizard block that force-wrote stale pre-#431 display values on every confirmation. New v14→v15 config migration restores the AD5X backlight-off sleep preset for users polluted by the pre-fix wizard. Resolves the "random RGB colors during sleep" symptom documented in `FLASHFORGE_AD5X_SUPPORT.md`.
- **AD5X IFS color and material type sync** — slot color/type changes from the panel now use Flashforge's `CHANGE_ZCOLOR` macro so the IFS controller actually receives the update.
- **AFC `LANE_UNLOAD` requests are now serialized** — tapping Eject on multiple lanes in quick succession queues the requests and runs them one-at-a-time instead of overlapping AFC macros. Mitigates Turtle_1 "Timer too close" shutdowns from competing stepper / LED-animation work.
- **Shutdown crash with active observer chains** ([#893]) — `Application::shutdown()` now invalidates `ObserverGuard`s *before* tearing down `MoonrakerManager`. The prior teardown order let pending subject notifications fire callbacks against destroyed Moonraker state, producing L081-family crashes during exit.
- **`MoonrakerManager::m_print_duration_observer` not released on shutdown** ([#888]) — observer lingered through the static destructor chain; explicit release on shutdown closes the dangling-observer window.
- **Temp graph chart deletion now async** ([#867]) — chart child cleanup during temp-graph teardown moved off the synchronous LVGL event-list iteration path, with breadcrumb logging for any remaining anomalies.
- **L081 family: panel gate observer rebuilds coalesced via `lv_async_call`** — multiple gate-state subject changes within a single tick now produce one rebuild instead of N synchronous rebuilds, eliminating the multi-sync-delete-per-tick pattern that corrupts LVGL's global event linked list.
- **Z-offset wizard no longer auto-prefixes `CLEAN_NOZZLE` to `PROBE_CALIBRATE`** — the auto-prefix fired on printers without a wipe macro, hanging the calibration sequence. Users with a wipe macro can wire it into their start gcode.

### Changed
- **Animated screensaver disabled by default on BASIC and EMBEDDED tiers** — Raspberry Pi 3B-class (BASIC) and AD5M / AD5X (EMBEDDED) hardware now ship with the screensaver off to prevent print failures from CPU contention with Klipper. Users upgrading with Flying Toasters still enabled on these tiers are migrated to Off with a one-time notification; the setting can be re-enabled under Settings → Display. When re-enabled on these tiers, the screensaver runs at a reduced frame rate (~7 fps) with fewer sprites (Flying Toasters capped at 10) to stay out of the print loop's way.
- **Spool canvas scroll smoothness** — dedup pass, LRU pixel cache, and a sqrt LUT in the canvas pixel renderer. Noticeable on filament panel scroll on EMBEDDED-tier hardware.
- **Crash bundle `queue_prev` ring coalesces consecutive identical tags** — high-frequency callbacks (e.g. `TSM::update_subjects` per WebSocket tick) used to fill all four slots and bury genuinely distinct prior callbacks. The producer now bumps a parallel count on tag-pointer match instead of advancing the ring; emitted as `queue_prev: tag (xN)` when N > 1. Restores diagnostic runway for tracing post-callback heap corruption (#840 / #851 family).

## [0.99.49] - 2026-04-26

A targeted hotfix release for AD5X (ZMOD firmware) and other platforms with aggressive supervisor lifecycles. Real-time triage of bundle EE8X6GSK plus a live SSH session on the affected printer revealed a crash-loop chain combining three independent bugs: ZMOD's display-handoff Klipper macro periodically `killall`s helix-screen, our SIGTERM handler ran the full `Application::shutdown()` teardown, the teardown crashed with SIGBUS in late-stage cleanup (the crash handler was uninstalled in the first line of `shutdown()`, so no diagnostic landed on disk), and the watchdog burned through restart credits leaving a blank screen. All three sides are now hardened.

### Fixed
- **HelixScreen no longer crash-loops on platforms whose firmware aggressively respawns the UI process (AD5X / ZMOD)** — SIGTERM now triggers immediate `_exit(0)` without running `Application::shutdown()`. External supervisors only care about a clean exit code, not whether teardown ran. Skipping the fragile teardown path (LVGL deinit, observer cleanup, static destructors) avoids the L081-family SIGBUS that ZMOD's killall-and-respawn cycle was hitting on AD5X every ~20 s after Klipper restart. SIGINT (Ctrl+C from terminal) keeps the graceful shutdown path. Persisted state (settings, telemetry queue, crash history) is written on each change so nothing is lost.
- **Crash handler stays installed through the entire shutdown sequence** — `crash_handler::uninstall()` was the *first* line of `Application::shutdown()`; now the *last*. Any SIGBUS/SIGSEGV during widget deletion, observer cleanup, or `lv_deinit` is now captured to `crash.txt` instead of falling through to the kernel default with no diagnostic. Critical for surfacing the L081-family teardown crashes seen post-v0.99.46.
- **Crash diagnostics survive misconfigured config dirs (chroot, overlay, PrivateTmp)** — when the signal handler can't `open()` the primary crash path, it now falls back to writing the dump to stderr (with `=== HELIX_CRASH_DUMP ===` marker prefix). Stderr is always open and gets captured by both journald (systemd) and the watchdog's stderr pipe, so users on platforms where `$HELIX_CONFIG_DIR` resolves to a non-writable layer no longer get silent diagnostic blackouts.
- **Debug bundles now include crash data on platforms that set `$HELIX_CONFIG_DIR`** (ZMOD, RatOS, etc.) — collector previously used an ad-hoc `$HOME/helixscreen/config` probe that missed overlay installs entirely. Now uses the canonical `helix::get_user_config_dir()` resolver, matching where the crash handler writes. New `crash_txt` bundle key includes the raw signal-handler dump even when the next-boot reporter hasn't run yet.
- **Webcam discovery skipped on platforms without a camera widget** — Moonraker `server.webcams.list` and the local-endpoint probes (127.0.0.1:8080/8081/4408) now respect the `HELIX_HAS_CAMERA` compile-time gate. Skipped on AD5M/AD5X/CC1/K1/K2/MIPS/SnapmakerU1. On AD5X specifically, the firmware's kernel H.264 codec driver crashes on every `v4l2_open` (`dma_coherent_mem_available` NULL+12 deref) — we never opened `/dev/video*` directly so this isn't the trigger for the recent crash family, but probing for a webcam we can't render is wasted RPC.
- **Installer disk-check uses POSIX `head -n 1`** so BusyBox doesn't reject `head -1` (AD5M/AD5X/K1/K2).
- **Updater download-directory candidate list now includes the install root** — previously skipped install-root candidates, breaking updates on platforms where the install root is the only writable location.

## [0.99.48] - 2026-04-26

A performance and correctness patch. Headlines: drastically narrowed Moonraker subscriptions (the prior nullptr-everything pattern was firing per-frame led_effect notifications on AFC hardware — a Voron Trident bundle showed 46 LED-effect objects updating per render frame); a tangle of six interlocking fixes to the wizard preset path so AD5M Pro on ForgeX firmware actually skips hardware steps on fresh install; and a UAF / OOB-read fix for a shared-component XML callback collision caught in bundle SSHGTVZQ.

### Added
- **Jog soft-stops driven by kinematic envelope** — `MotionPanel::jog()` now refuses moves that would push the toolhead past `toolhead.axis_minimum`/`axis_maximum` and toasts the user. Skipped when bounds aren't published yet or the axis isn't homed.

### Fixed
- **Wizard preset application: AD5M Pro ForgeX skipping hardware steps on fresh install** — six interlocking issues (preset path resolution, late `preset_mode` capture, Moonraker connect gated behind wizard, validate-before-detect ordering, wrong fan names in `ad5m_*_forgex.json`, `_zmod` probe firing on `_forgex` preset, missing aux fan slot recognition).
- **Layer-source precedence within a status update** — slicer `SET_PRINT_STATS_INFO` now wins over `virtual_sdcard.layer` within the same tick (sdcard branch was running second and silently overwriting). Across updates, last source still wins.
- **`toolhead.max_velocity` subscription regression** — caught in code review of subscription narrowing; `max_velocity_` would have stuck at its initial value, breaking the print-tune overlay and machine-limits panel after every reconnect. Field list is now locked by a 19-case / 178-assertion regression test naming each parser file it mirrors.
- **XML callback name collision could SEGV on click** (bundle SSHGTVZQ, Qidi Q2, pi) — first-write-wins for `lv_xml_register_event_cb` meant two C++ owners registering the same callback name globally for a shared XML component (e.g., `WizardWifiStep` + `NetworkSettingsOverlay` on `wifi_network_item`) silently dropped the second registration. The first owner's handler then fired against the second owner's `user_data`, casting a small struct as a much larger one and reading ~400 bytes past valid memory. Now last-write-wins.

### Changed
- **Moonraker subscription narrowing — heaters, fans, sensors, tools, macros** — eliminated the remaining `nullptr` (= all-fields) subscriptions in the discovery sequence. Narrowed to the actual fields each parser consumes (heaters → `{temperature, target}`, standard fans → `{speed}`, native LEDs → `{color_data}`, etc.). Dropped `idle_timeout` entirely (no parser reads it).
- **Moonraker subscription narrowing — `led_effect`, AFC, core motion** — biggest perf win on AFC/BoxTurtle: a Voron Trident bundle showed 46 `led_effect` objects firing per-frame animation updates because we'd been subscribing with `nullptr`. Now `led_effect` is just `[enabled]`. Also narrowed all `AFC*` objects to their parser fields, and the per-step core motion objects (`toolhead`, `gcode_move`, `motion_report`).
- **Launcher `nice +10` when co-hosted with Klipper/Moonraker** — `helix-launcher.sh` reduces UI process priority on shared-CPU hardware (Pi, SonicPad, AD5M class) so the printer control loop keeps CPU headroom for stepper timing and MCU comms. Detection is process-based (`pgrep -f`) with a Unix-socket fallback. Standalone displays (remote-display SonicPad, dev workstation, kiosk pointed at a network printer) are unaffected. Override with `HELIX_NICE=<n>` in `helixscreen.env`; `HELIX_NICE=0` disables.
- **`make clean-tests` now wipes the PCH and all sanitizer test binaries** — recover from a sanitizer-contaminated tree without `make clean`. Previously, ASan/TSan test runs reused the PCH compiled with `-fsanitize=address`, and a subsequent `make test-run` would fail with "ASan runtime does not come first."

## [0.99.47] - 2026-04-26

A defensive-hardening patch release continuing the L081 (`lv_event_mark_deleted` SEGV/SIGBUS) eradication campaign begun in v0.99.45/v0.99.46. Sweeps the remaining unsafe `lv_obj_clean()` callsites that escape UpdateQueue batches, plugs lifetime gaps in two control panel callbacks, and lands a stress harness that will catch regressions of the multiple-async-delete-per-tick race plugged in v0.99.46.

### Fixed
- **L081 family (`lv_event_mark_deleted` SEGV/SIGBUS) — final sweep** ([#873]–[#878]) — 18 remaining `lv_obj_clean()` callsites that ran inside event-dispatch frames or queued callbacks were converted to `safe_clean_children()` so child deletion happens via `lv_obj_delete_async()` rather than synchronously inside an LVGL event-list iteration. Pairs with v0.99.46's wizard root-cause fix to close the #840-adjacent crash family.
- **Container child cleanup escapes event-dispatch batch** — top-level containers cleaned during input-event handling now defer child deletion to the next async tick, eliminating the multiple-sync-delete-per-tick race that corrupted LVGL's global event linked list.
- **Z-offset save and flow up/down callbacks lacked lifetime guards** — the controls panel could fire on a freed `MotionPanel` if a save/flow tap landed during teardown. All three callbacks now use `tok.defer()` with lifetime tokens, matching the rest of the panel.
- **Updater "no space" check used a hardcoded 50 MB floor** — but the current pi32 zip is 69 MB, so users could pass the precheck and fail mid-stream. The check now uses the asset size from the GitHub/R2 manifest with a 1.2× headroom and a 200 MB fallback for unknown-size assets, plus an actionable error message naming the asset and the shortfall (driven by bundle 7ZGHW5KX where systemd namespace weirdness caused statvfs to misreport free space).
- **Debug bundles missed helixscreen log on ZMOD AD5X** — log lives at `/opt/config/mod_data/log/helixscreen.log` (real path `/usr/data/config/mod_data/log/`); both are now in the log-tail cascade.
- **About panel: licenses container border radius** — was hardcoded; now uses the `#border_radius` design token so it matches the rest of the panel.

### Changed
- **action_prompt_modal show/hide stress harness** — `tests/unit/test_action_prompt_modal_stress.cpp` adds seven Catch2 variants (rapid show/hide, #877 burst pattern, alternating shapes, stacked-under-base, click-driven hide, single-tick drain, queue_update racer) tagged `[stress][action_prompt][.ui_integration]`. Will trip if the multiple-async-delete-per-tick race is reintroduced. Override iteration count via `ACTION_PROMPT_STRESS_ITERATIONS`.

## [0.99.46] - 2026-04-26

A targeted fix release. The headline is the ASAN-confirmed root-cause fix for the chronic wizard step-transition crash family that has driven roughly half a dozen patch attempts since v0.99.34. With AddressSanitizer wired into the Pi cross-build, two distinct heap-use-after-frees were caught within minutes of human interaction with the wizard — replacing weeks of victim-site whack-a-mole.

### Fixed
- **Wizard step-transition heap corruption — root cause** ([#880], [#871], [#870], [#872], [#873], [#874], [#875], [#877]) — `WizardConnectionStep::auto_probe_timer_` is a one-shot LVGL timer; LVGL deletes one-shot timers internally after the callback returns, but `attempt_auto_probe()` only nulled the member pointer on its non-early-return path. When the timer fired and early-returned (no IP yet), the member was left pointing into freed memory. On back-nav from the Connection step, `cleanup()` called `lv_timer_set_cb(nullptr)` against the freed timer, corrupting LVGL's timer linked list and the heap region behind it. Downstream effects manifested as crashes in `lv_event_mark_deleted`, `trans_anim_start_cb`, `lv_draw_sw_blend_image`, and the wider LVGL anim/event/render paths. The member is now nulled at the start of the timer callback, before any path can early-return. ASAN-confirmed.
- **Test Connection click crash** — the libhv WebSocket `onopen` lambda captured `const char* url` directly, but callers pass a stack-local `std::string`'s `.c_str()`. The lambda fires on a libhv worker thread after the caller's scope unwinds, and spdlog's format-arg processing strlen'd the freed buffer. Lambda now captures by string-copy. ASAN-confirmed.
- **Connection step IP/port pre-fill** — fields appeared empty on every visit (instead of showing 127.0.0.1/7125 or the saved config) and lost user-typed values on back/forward navigation. `init_subjects()` now seeds buffers only on first init so user input survives re-visits, and `create()` drives pre-fill from the buffer (the source of truth) rather than the subject (which can be transiently empty during XML widget construction).
- **AD5M Wi-Fi step on first launch** — Forge-X firmware doesn't auto-load the Realtek RTL8821CU USB driver, and wpa_supplicant only starts after the user has supplied credentials via the stock UI. Fresh installs running HelixScreen as the launcher therefore never enumerated `wlan0` and reported "no WiFi hardware found." Platform hooks now insmod `8821cu.ko` and start `wpa_supplicant` in `platform_pre_start()`, both idempotent. No-op on AD5M boards without a USB Wi-Fi dongle.
- **Wizard subtree purge regression** — the v0.99.45 fix to cancel style transitions during cleanup recursed into the root container and stripped its flex layout, collapsing new step content into a thin column. Root container now only gets `lv_anim_delete`; descendants still get the full style strip.
- **AMS `set_slot_info` clobbered `mapped_tool`** when the caller didn't pass it; persists across calls now. Horizontal card scrolling on the AMS panel also restored.

### Changed
- **AddressSanitizer build for Pi** — new `make pi-asan-docker` target produces a fully-instrumented binary (LVGL, helix-xml, libhv, helix-screen) with `-static-libasan` for self-contained deployment. Output to `build/pi-asan/` so it doesn't clobber regular builds. Strip is forced off; debug info is split into a separate `.debug` file kept on the build host for symbol resolution.
- **Wizard step-transition stress harness** — Catch2 fixture (`tests/unit/test_wizard_step_stress.cpp`) drives `ui_wizard_navigate_to_step` programmatically in 2↔3 bounces and full-sweep patterns, configurable via `WIZARD_STRESS_ITERATIONS`. Tagged `[wizard][stress][.ui_integration]` — hidden from default test runs.

## [0.99.45] - 2026-04-25

A defensive-hardening release. The dominant theme is converting hard-to-debug LVGL crashes into logged anomalies that telemetry can act on, plus a scope-aware shutdown/reboot UX for users running multiple Klipper hosts.

### Added
- **Scope-aware shutdown/reboot modal** with split-button dual-host UX. When you have more than one host (e.g., a satellite Klipper running over MCU-share), the shutdown widget now lets you target Just This Printer or Both Hosts. Selecting Both chains the second action on the first host's ack; the host_identity cache is invalidated on each action so subsequent shutdowns don't bounce off stale state.
- **Label printer error toasts** — raw BlueZ / vendor error strings are now mapped to actionable toast messages ("Niimbot is busy printing — wait for the current job" instead of `org.bluez.Error.Failed: Operation already in progress`).
- **About panel: install root, config dir, cache dir rows** — pull from the new `app_globals` helpers, with a folder-home MDI glyph for the Install Root row.

### Fixed
- **L081-family LVGL crashes** ([#190], [#80], [#776], [#840], [#873]–[#878]) — `lv_event_mark_deleted` now bails defensively when `event_head` is corrupt (chain depth > 256 or pointer misaligned), reporting via `helix_lvgl_anomaly` with the head/target/depth context and a hex backtrace instead of segfaulting. Combined with the new async-delete breadcrumb hook (below), the next crash report from this family will name the widget being torn down at corruption time.
- **Markdown null-deref on tight-memory devices** ([#879]) — md4c could null-deref deep inside `md_analyze_inlines` when an internal allocation failed (observed on K1, 209 MB RAM). The observer text path now short-circuits empty/null inputs and caps input length at 256 KB before handing off to md4c.
- **Wizard step transition UAF** ([#871]) — style transitions queued by the previous step's widgets could fire `trans_anim_start_cb` on a freed widget after `lv_obj_clean`. The wizard now walks the subtree and cancels every pending animation before the clean.
- **Print Select detail view: clear thumbnail by passing nullptr** — passing the buffer pointer instead of nullptr left the image cache thinking the slot was still live.
- **Advanced panel: timelapse-setup row visibility race** — nested `bind_flag_if_eq` / `bind_flag_if_not_eq` on the same widget produced a last-one-wins race; the row is now nested inside a parent that owns the visibility binding.
- **Modal button row: danger styling now renders** — the `primary_variant` XML prop wasn't wired through, so destructive modals ("Delete profile") rendered with primary blue instead of danger red.
- **SDL sound: resize mix buffer before unpausing audio device** — fixes a transient pop / silence on the first sound after device init.
- **Bluetooth BLE-only device connect** — fall back to `ConnectProfile` when `Connect` fails on BR/EDR-less peripherals, instead of surfacing a generic "connection refused".
- **About panel: Install Root row icon** — was referencing `folder_home` which isn't in the MDI font subset; falls back to the shipped `folder` glyph.

### Changed
- **Crash-capture diagnostics** — LVGL async-delete now leaves an `async_d <class> <ptr>` breadcrumb naming the root widget being torn down (#840 diagnostic). Breadcrumb ring doubled from 64 to 128 slots, plus gated cache-eviction crumbs (#851 family). Pure diagnostics; no behavioral change.
- **i18n** — UI strings synced across 8 locales; 59 dead keys dropped from translation files.
- **Logging** — thumbnail flow is now visible at debug; the 5-second poll log is quieted.
- **CI** — stable-tag releases now dispatch a docs deploy to helixscreen-website automatically.

## [0.99.44] - 2026-04-23

### Added
- **SonicPad hardware brightness** — Creality Sonic Pad (allwinner,r818) now gets real backlight control via `/dev/disp` DISP2 ioctls. The AD5M-specific boot-time PWM-inversion reset is skipped on SonicPad, where it logged "pwm device hdl is NULL" and dragged the display pipeline.
- **Shutdown/Reboot verification** — the shutdown widget now verifies the host actually went down. Moonraker can reply `machine.shutdown`/`machine.reboot` with success but silently no-op on some firmwares (observed on SonicPad Jpe230). If the WebSocket has reconnected 20s after a successful RPC, an error toast tells the user the host is still reachable.
- **Test Screensaver button** in Settings lets you audition the selected screensaver without waiting for the dim timeout.
- **"Confirm before running" macro toggle** (Settings → Safety & Notifications, default on) gates macro execution behind a confirmation dialog. Skipped when params are required or when the macro is flagged dangerous.
- **Memory pressure responders for print status tree and LVGL image cache** — on warning+, the cached print-status widget tree (~400–800 KB) is destroyed when off the nav stack; on critical, the LVGL decoded-image cache is dropped.
- **Memory-monitor diagnostics** — pressure log lines now include our RSS/VmSize/VmSwap, 5-min growth, and system-available; on warning+ the `smaps_rollup` breakdown is dumped; a post-response summary reports the whole-app RSS delta so operators can see whether responders actually recovered memory.
- **Configurable scroll_guard cooldown** — `/input/scroll_guard_cooldown_ms` (and `HELIX_SCROLL_GUARD_COOLDOWN_MS`), clamped to 20–500 ms. Default stays 80 ms; AD5X users still seeing phantom clicks at lift-off can try 150–200 without a code change.
- **AD5X fresh installs ship the AMS widget on the home grid by default** — previously fell through to the generic default layout whose filament/AMS swap depended on `ams_slot_count` being live at wizard-completion time. Existing installs keep their saved layout.
- **Android Play Store publishing pipeline** — CI now builds and uploads signed AABs to Google Play end-to-end, gated on the Play service-account secret.
- **Contributor doc set** — CONTRIBUTOR_GOTCHAS.md, YOUR_FIRST_CONTRIBUTION.md, THEME_CONTRIBUTOR_GUIDE.md, TRANSLATION_CONTRIBUTOR_GUIDE.md, and a CONTRIBUTING.md front door at the repo root.

### Fixed
- **Text-input null-font crash** ([#853], [#864]) — `text_font` is now bound at creation time so a forced theme refresh can't land a null font pointer on the widget mid-layout.
- **Upgrade banner missing / floating "Update" + "X" buttons on every panel** — `upgrade_banner.xml` was registered with the wrong path (it lives under `ui_xml/components/`), and the banner also used an unregistered `<ui_icon>` tag that desynced the XML parser's parent stack and left the child buttons parented to the top layer. Rebuilt against theme tokens matching `toast_notification.xml`.
- **Stuck AMS widget at (-1,-1) with filament sensor occupying its cell** — a one-shot home-panel migration swaps AMS into filament's cell on any page where AMS is enabled-but-unplaced while filament is placed. Fixes existing installs caught by the `build_default_grid` timing window before the AD5X preset ship.
- **AD5M detected over AD5X when a chamber LED is present** — added `macro_exclude` on `SET_EXTRUDER_SLOT` (unique to AD5X IFS) to the four AD5M-family entries, so AD5X no longer loses the tiebreak to AD5M Pro when both fingerprints score 100.
- **AD5X IFS: ejected spool still shown as full** on the `GET_ZCOLOR` / save_variables path — mirrored the #631 eject-clear into `parse_save_variables` (only when IDLE).
- **Niimbot BLE re-pair required on every restart** — the label printer overlay's brand-based dedup mistook Niimbot's rotating BLE random address for a "wrong transport" migration and cleared the saved MAC. Same-transport duplicates are now dropped; the saved MAC sticks. Real BlueZ errors also now surface instead of "unknown error" from post-teardown reads.
- **Moonraker subscription leak across reconnects** — `lifetime_weak()` was handing `SubscriptionGuard` the same guard that `disconnect()` resets, so every reconnect looked like "client destroyed" and the real unsubscribe was skipped. Split into two guards.
- **WiFi wizard step re-entry hardening** — `apply_ethernet_status` and the deferred scan callback now early-out when the step is mid-teardown. Not a root-cause fix for the heap corruption in MCPKABEE/QLCCZKRQ, but removes a known re-entry race.
- **Print status panel "already in stack" warning spam** on long sessions — the print-start auto-push now checks `is_panel_in_stack()` before queuing.

### Changed
- **Crash-capture breadcrumbs** — finer-grained wifi-step tags (`create_enter`, `xml_create`, `scan_fire`, `populate_begin`, `cleanup_enter` under a `wifi` category), plus post-Moonraker-connect discovery and printer-identify breadcrumbs. Diagnostic only.
- **Touch-feel documentation** — CONFIGURATION.md, TROUBLESHOOTING.md, and ENVIRONMENT_VARIABLES.md now carry a symptom → setting table for `scroll_limit`, `scroll_throw`, `jitter_threshold`, and `scroll_guard`, plus a new "Unintended Clicks While Scrolling" troubleshooting entry. The docs also listed the `jitter_threshold` default as 15 (has been 5 since v2→v3); corrected along with two clamp-range typos.
- **Bluetooth setup guide** covers USB barcode scanners and USB-MCU users.

## [0.99.43] - 2026-04-22

### Added
- **Niimbot B-series support (B1 / B18 / B3S)** — added to the brand table with the shared 384-dot printhead profile. Previously these models were unsupported.
- **New label sizes** — 30×20, 30×40, 40×40, 25×30, 14×40, 14×60. Fixes **"feeds but prints blank" on B1 with 30×20 stock**, where the printer previously defaulted to a 50×30 template and advanced 50 mm of material across 2.5 of the user's actual labels per print.

### Fixed
- **Dual-mode Bluetooth label printers no longer fail to connect** — dual-mode Niimbots (D110 and siblings) enumerate as two BlueZ devices with the same name: a BR/EDR half exposing only SPP/PnP and a BLE half carrying the vendor GATT service. Discovery dedup was MAC-based, so both entries appeared in the picker and selecting the BR/EDR half always failed with `br-connection-profile-unavailable`. Dedup now resolves same-name conflicts via the brand table and keeps the vendor's preferred transport. Users whose saved pairing points at the wrong-transport sibling are auto-migrated on the next discovery (saved MAC cleared) so a single re-pair puts them on the right half.
- **"(Saved)" indicator showing on every Bluetooth device** — the saved-pair tag was flagging any device with a non-empty MAC (i.e., all of them); it now compares against the actual saved `bt_address`.
- **AD5M: crash reporter recursion on uncaught exception** — the exception record path is now async-signal-safe. When a C++ exception escaped, the crash writer recursed and the process aborted with "std::terminate without active exception" before the report could be written. Reports from AD5M devices will now arrive intact.
- **Thread exhaustion crash on small devices** ([#837]) — raw detached `std::thread(...)` sites are now routed through managed pools (`HttpExecutor::fast/slow`, `BusThread`) or wrapped in try/catch so `pthread_create` EAGAIN surfaces as a toast instead of terminating. Most visible on AD5M/CC1 under thread pressure.
- **WiFi panel use-after-free on scroll** ([#850]) — the LVGL input device is now reset before clearing the network list, closing the window where the scroll indev dereferenced a freed scrollable mid-rebuild.
- **Overlay dismiss use-after-free** ([#840]) — widget deletion in `destroy_overlay_ui` is now deferred, removing the window where a pending async callback could fire on a freed overlay.
- **Thermistor rediscover UAF** — paired `SubjectLifetime` as a member next to `ObserverGuard` so per-sensor observers expire cleanly when dynamic temperature subjects are destroyed on reconnect.
- **Observer `release()` → `reset()` in widget `LV_EVENT_DELETE` callbacks** — closes a zombie-observer class that caused rare render-state corruption behind the #579 report cluster.
- **Config: `telemetry_enabled` source-of-truth** on v13→v14 migration — resolves cases where the in-app toggle state could disagree with the stored value.
- **Crash-handler diagnostics** ([#851]) — `queue_prev` ring expanded to 4 slots for richer last-actions context; silenced an empty-name image-lookup spam path during crash capture.

### Changed
- **Toast contrast** — toast cards now sit on `elevated_bg` with a theme border, reading cleanly against busy backgrounds on all themes.
- **Printer log noise** — per-tick hot-path logs only emit on an actual state change, significantly lowering journal volume on idle devices.

## [0.99.42] - 2026-04-22

### Fixed
- **Snapmaker U1 self-update bricked on v0.99.41** — the in-app updater fell through to the Raspberry Pi tarball because `HELIX_PLATFORM_SNAPMAKER_U1` had no mapping in `get_platform_key()`. U1 devices ended up with a Pi aarch64 binary missing libsystemd/libinput/libEGL/libGLESv2/libgbm/libasound, and helix-screen failed at exec time with "cannot open shared object file". A CI guardrail now blocks any new platform from shipping without the right asset mapping. **Recovery from v0.99.41:** the broken binary can't self-update out of this bug — reinstall the Snapmaker U1 tarball manually using the instructions on the release page.

## [0.99.41] - 2026-04-21

### Added
- **Spoolman: spool lists sorted by last used** — AMS edit modal and Spoolman panel show the most recently used spool first; never-used spools sink to the bottom.

### Fixed
- **Spoolman new-spool-on-save** — the external spool modal's Save path failed to create the spool in Spoolman and never told Moonraker it was active. Density and diameter are now supplied on POST /v1/filament; initial_weight on POST /v1/spool; AmsEditModal fires sync_active_spool after create and re-syncs on every linked save (recovers from Moonraker state loss).
- **Crash-report modal occluded by install wizard** ([#849]) — backdrop now parents to the top layer so Send/Dismiss is always reachable.
- **i18n: icon_font_hero missing on smaller font tiers** — previously mapped to xxlarge-only glyphs, producing font-not-linked warnings and empty icons on CC1, AD5M, pi32, and K2. Now maps to each tier's largest available glyph.
- **Wizard Back/Next crash during install** ([#848], [#843]) — wizard navigation routes through lv_async_call so the clean+rebuild runs outside the input-release tick.
- **AMS AFC: redundant per-update hashtable lookup** — caller loops already know the slot index; closes the torn-pointer fault seen in bundle 8SA9DQZ4.

### Changed
- **i18n memory: −4.7 MB peak RSS on CC1** — dropped dead lv_i18n path; translations load per-locale on demand. Saves ~1.2 MB binary / ~521 KB VmExe; VmHWM drops 19 MB → 14 MB on CC1 (under the 15 MB OOM warning threshold). All 9 languages remain runtime-selectable.

## [0.99.40] - 2026-04-21

### Added
- **XML hot-reload** — set `HELIX_HOT_RELOAD=1` to watch `ui_xml/` (including breakpoint subdirs) and rebuild the active panel, overlay, or modal in place when a file saves. Panels, overlays, modals, and the navigation stack all implement a `rebuild()` hook; after-reload callbacks re-show the top modal and refresh observer bindings. The watcher warns when a reload would dangle an XML subject pointer so you see it immediately instead of as a later crash.
- **Print Select:** 3-column card grid on 480px breakpoints (previously 2-column) — more prints visible without scrolling on wider portraits.

### Fixed
- **Android update check** — libhv is built without SSL on Android, so HTTPS to R2 and GitHub silently failed with "Connection failed (R2 + GitHub)". Update-check traffic now routes through Android's system TLS stack via a JNI bridge to `HttpURLConnection`. The nightly auto-check and its in-app "New Version Available" notification now work, and the Install button opens the Play Store listing instead of attempting a tarball install.
- Action-prompt modal: remove button event callbacks before freeing `user_data` to avoid a use-after-free on dismissal ([#840]).
- LVGL: guard against a garbage non-null font pointer in the `sw_label` renderer that crashed under rare layout cascades ([#842]).
- AMS edit modal: async-delete spool list children to avoid the LVGL event-list corruption pattern (L081) ([#845]).
- Wizard: stop the connection spinner animation before `lv_obj_clean` to prevent a use-after-free on step transitions ([#843]).
- AMS AD5X IFS: close the race where `_IFS_VARS` was enabled before the macro had been verified present on the printer.
- Discovery (AD5X): IFS runout sensors now appear in `filament_sensor_names` so the runout row shows on AD5X.
- Runout modal: removed stray XML `event_cb` declarations that produced "callback not found" warnings in the log.
- Hot-reload: reset `error_code` between filesystem queries and use the non-throwing iterator increment — stops the watcher from aborting on transient `ENOENT`.
- About settings: Check-for-Updates row description is visible at every breakpoint, so the "Error" / version text doesn't get hidden on narrow layouts.
- Update flow: skip the redundant confirmation modal when the user already confirmed "Install" on the in-app update notification.

### Changed
- AD5M / AD5X launcher: enable `MALLOC_CHECK_=3` and `MALLOC_PERTURB_` for heap diagnostics on the printers most prone to memory-pressure crashes ([#838]).

## [0.99.39] - 2026-04-20

### Added
- **Synth PCM backends on VoiceSlot** — ALSA and SDL backends now render per-sample through `VoiceSlot` with per-sample envelope and filter state. The sequencer publishes `NoteEvent` for continuous-audio backends and per-tick events for PWM/buzzer backends, unifying envelope behavior across every platform.
- **Sound preview overlay** — Browse and preview individual sounds from the sound settings; a new "Preview Sounds" button opens `SoundPreviewOverlay`, a dynamic button grid populated from `SoundManager::get_sound_names()`.
- **Build:** New `PLATFORM_TARGET=ad5m-br` mode and `make install DESTDIR=...`
  target, enabling external build systems (starting with the AD5M Klipper Mod
  firmware) to package HelixScreen as a native variant. See
  `docs/devel/AD5M_KMOD_VARIANT.md`. The existing `ad5m` target is unchanged.
- Per-printer first-print phase defaults — each preset ships its own pre-print warm-up timing.
- Header bar: per-widget `icon_size` override for oversized action-button glyphs.
- Crash diagnostics: breadcrumb on the `discovery-complete` path to narrow the recurring copy-assign crash signature.

### Fixed
- **Installer (K1 / K2 / Snapmaker U1):** Repaired five regressions from the
  earlier `config/` → `assets/config/` refactor. On affected boards the
  installer was silently leaving the stock Creality UI running alongside
  HelixScreen (`[WARN] No platform hooks for: k2`) and skipping Mainsail/Fluidd
  config symlinks (`[INFO] No printer_data/config found, skipping config
  symlink`). The underlying issues: init script sourced hooks from a path
  that no longer existed; `deploy_platform_hooks` looked under
  `config/platform/` instead of `assets/config/platform/`; `KLIPPER_HOME`
  defaulted to `/root` on K1 and K2 whose `printer_data` lives on `/usr/data`
  and `/mnt/UDISK` respectively; Snapmaker U1's detection of `/home/lava` was
  fragile on freshly-flashed units. All five fixes auto-apply on the next
  self-update. Already-installed users who cannot update can apply the
  migration manually:
  ```sh
  /etc/init.d/app stop; /etc/init.d/app disable   # K2 only
  mkdir -p /opt/helixscreen/platform
  cp /opt/helixscreen/assets/config/platform/hooks-k2.sh \
     /opt/helixscreen/platform/hooks.sh
  chmod +x /opt/helixscreen/platform/hooks.sh
  sed -i 's|/assets/config/platform/hooks\.sh|/platform/hooks.sh|' \
     /etc/init.d/S99helixscreen
  /etc/init.d/S99helixscreen restart
  ```
  Verified on K1C, K2 Plus, AD5M Forge-X, Snapmaker U1, and CC1 hardware.
- Discovery: `box` and `enclosure` now match as chamber-sensor keywords — Elegoo COSMOS `temperature_sensor box` and common modder `enclosure` names enable the chamber row on CC1 and similar builds.
- Presets: Snapmaker U1 macros and hotend fan aligned with hardware; CC1 hardware list completed and filament sensor pre-declared; K1C `fan2` role and filament macros aligned with stock firmware; K2 Plus hardware list corrected (bogus "K2 Max" entry removed).
- Display: bundle minimal zoneinfo so IANA timezones resolve on CC1 (ships without `/usr/share/zoneinfo`).
- Synth: step-boundary silence no longer kills active PCM notes; `button_tap` suppressed in the preview overlay; per-sample envelope restores attack/release shape; filter state preserved across buffer boundaries; smaller SDL ring buffer cuts perceived latency.
- Print status: low-memory deactivation clears gcode-viewer dedup guards; `end_overlay` observer reset before subject deinit; MICRO metadata sizing and end-overlay race.
- Print: `ActivePrintMediaManager` uses an immediate observer to close the stale-thumbnail race during rapid print transitions.
- Print select: egg placeholder removed; history/queue fetch guards hardened so transient Moonraker outages don't leave the panel empty.
- Toast: responsive width restored after the stacking refactor.

### Changed
- Synth architecture: `set_voice_envelope` removed (deprecated); PCM backends now own per-sample rendering through `VoiceSlot`.
- Build: libhv's symlinked `LIBHV_DIR` cleaned of stale artifacts; `ad5m-br` added to sound + `.PHONY` filters; cmake scaffolding pruned from the installed `ui_xml/` tree.

## [0.99.38] - 2026-04-20

### Added
- Toast notifications now stack when multiple fire simultaneously; startup warnings (NOTIFY_*) queue through PendingStartupWarnings so none are lost during init.
- QIDI Box AMS backend scaffolding — placeholder for future PLUS4/Q2/MAX4 filament system support.
- Per-extruder filament consumption routing: multi-extruder printers track spool usage independently per tool.
- Tool-changer identity mapping: `slot_for_extruder()` routes the correct AMS slot to each physical tool.
- Printer discovery treats 'cavity' as a chamber synonym (Snapmaker U1 compatibility).

### Fixed
- Wizard: connection step migrated to AsyncLifetimeGuard, preventing use-after-free on rapid step transitions; WiFi subject re-init is now guarded.
- Wizard: manual printer-type pick now correctly applies the selected preset (#837).
- Presets: ForgeX split from KlipperMod profile; ZMOD stepper fan name corrected.
- AD5M ZMOD: probe for `/dev/fb0` before launching to avoid cold-boot race condition.
- Android: read-only seed bundle now ships via build-time packaging manifest.
- Presets: AD5M fan defaults aligned with actual hardware (#837).
- Print start: deduplicate probe samples and complete detection on first extrusion move.
- Home panel: widget ID cache reads from populate snapshot, not post-populate re-read.
- Home panel: favorite macro label no longer overlays icon; long names truncate with ellipsis.
- Home panel: Home label placed on same row as homing buttons.
- AMS panel: blank weight field shown for unknown spool instead of "-1".
- Per-extruder filament subjects pre-populated to prevent null-subject observer crashes.

### Changed
- Print status panel refactored from imperative UI state to declarative XML bindings.

## [0.99.37] - 2026-04-19

### Added
- Filament slot metadata now persists across restarts and reconnects on AD5X IFS, Snapmaker U1, ACE, and CFS — brand, spool name, Spoolman link, and weights survive Klipper reloads and printer reboots.
- OrcaSlicer 2.3.2+ automatic filament sync: HelixScreen writes the AFC-originated `lane_data` Moonraker DB convention, so Orca's filament panel picks up your slot metadata with zero configuration.
- "Clear Spool" entry in the AMS slot context menu — clears user overrides and falls the slot back to firmware-reported state. `Spool Info` and `Clear Spool` are now separate menu actions.
- Empty-but-overridden slots now ghost-render (20% opacity) across all backends, matching the previous AFC-only behavior.
- Automatic override clearing on detected physical spool swap: color change for IFS, RFID UID change for Snapmaker, material+color fingerprint change for CFS, status transition (empty→loaded) for ACE.
- Per-backend material whitelist and alias normalization for AMS (e.g., `PLA-BASIC` → `PLA`).
- Public specification at `docs/specs/filament_slots.md` for the shared `lane_data` convention.
- Crash diagnostics: breadcrumbs for wizard step transitions, language changes, and the previous queue-callback tag.
- ARM32 frame-pointer unwinding fallback so Pi32 and SonicPad crashes produce usable backtraces when `backtrace()` fails.
- Translation strings for camera, QR code, and print stats labels.

### Fixed
- Snapmaker U1: `set_slot_info` honors the `persist` parameter — user edits were previously discarded on the next Klipper status poll.
- Snapmaker U1: RFID `SUB_TYPE` is now mapped to the spool name and shown in the edit modal.
- Wizard: freeze `UpdateQueue` around step transitions to prevent async-callback races on quick advance (#827).
- Crash handler: reorder so `backtrace()` runs last, with FP-walk fallback (#827).
- Crash reporter: filter post-crash lines out of the log tail (#827).
- Crash: `std::terminate` exit encodes `128 + SIGABRT` so the watchdog surfaces the recovery dialog.
- Brother QL label print: route network send through HttpExecutor — Test Print no longer crashes from unbounded thread spawns.
- Spoolman: `Connect` probe runs on HttpExecutor and the button is disabled in-flight.
- Home/widget/tool/temp rebuilds use `safe_clean_children` to escape the UpdateQueue batch (#834, #776).
- AMS pre-print filament runout warning suppressed for auto-unload backends (IFS).
- AMS edit modal: skip Spoolman save when Spoolman is unavailable so local overrides still persist.
- AMS panel: `remaining_weight_g=-1` displays as "unknown" rather than empty; bypass label escapes the canvas; MICRO/TINY layouts no longer clip spool cards or temp graph.
- Moonraker API: `database.get_item` / `database.get_namespace` now unwrap the `response["result"]` envelope.
- AD5X IFS: defer initial `GET_ZCOLOR` until Klippy reports ready.
- AD5X IFS: latch the `_IFS_VARS` macro-missing check so `save_variables` notifications can't re-enable the broken path.
- Color picker: selected-preset outline, segmented tab fill, MICRO/TINY polish.
- Dropdown: honor theme `border_radius` as local style; narrow backup dropdown at MICRO/TINY.
- Filament panel: temperature graph fills and spool card sizes to content so the whole card stays on-screen at MICRO/TINY.
- Network widget: re-detect Wi-Fi when backend reports READY so "Disconnected" unsticks after async init (#819).
- IFS: pre-update baseline color in `set_slot_info` to avoid self-wiping the override it just set.
- Moonraker: tighten `delete_item` missing-key detection (prefer HTTP 404).

### Changed
- ACE: user override fields merge with firmware-reported data field-by-field (empty override fields fall through to firmware). Previously the override replaced the entire slot record.
- Pre-existing `helix-screen:ace_slot_overrides` / `helix-screen:cfs_slot_overrides` DB entries auto-migrate to `lane_data` on first launch; legacy JSON caches are removed after migration.

## [0.99.36] - 2026-04-18

### Added
- Live spool weight tracking without Spoolman: HelixScreen now decrements the locally-stored external spool's weight during a print, persists it on pause/end, re-snapshots when you edit the weight externally, and warns before queuing a print that needs more filament than the spool has left.
- Per-preset default home layouts — fresh installs get curated layouts per printer preset instead of one generic grid.
- Default home anchors nozzle + bed temperature widgets stacked next to print status across every breakpoint.
- Default home swaps the single-slot filament widget for the AMS widget when an AMS is present.
- Unified crash log collection: reports pull log tails from file, syslog, or journalctl automatically (AD5M/AD5X via syslog, Pi via journald) and auto-attach a debug bundle to the report for triage.

### Fixed
- GridEditMode: clear cached widget pointers after async rebuild to prevent SIGSEGV when deferred selection-chrome destroy runs on freed objects.
- Screensaver and confetti teardown routed through safe deferred delete so parent cleanup no longer races with pending async widget deletion.
- LVGL: disable blur tree walk unconditionally (#820).
- LVGL: cap `lv_obj_get_screen` parent walk to survive cyclic parent trees.
- Wi-Fi: NetworkWidget wakes when the backend reports READY after async init (#819).
- AD5X: backlight-off sleep restored for pre-#431 units via config v13 migration.
- FlashForge AD5M: Screws Tilt directions corrected (no longer inverted).
- Bed mesh: calibrate modal sizes to its content.
- CC1: bed mesh calibration sequence and progress count for load_cell_probe.
- AD5X IFS: use `GET_ZCOLOR SILENT=1` for live loaded-slot state.
- Edit mode: widget chrome buttons responsive across breakpoints.
- Default home: carousel deferred until after the first-run wizard dismisses.
- Console: gcode console font bumped to 10px on the micro breakpoint.
- AMS state: `get_external_spool_info` now mutex-locks the in-memory cache.

### Changed
- Theme: Soft corner radius bumped to 3px on the micro breakpoint (was 2px) — less sharp on 480x272 screens.
- Crash report worker renders breadcrumbs, heap snapshot, LVGL event log, and debug-bundle link in filed issues (#826).

## [0.99.35] - 2026-04-17

### Added
- Touch-friendly filament mapping: print-file details card shows a 2×2 grid of pills with the tool number (Tx) centered inside the gcode color dot; overflow past six tools collapses into a "+N" indicator. Mapping modal rows and slot-picker popup rows are taller, wider, and consistent with the same Tx-in-dot treatment.

### Fixed
- Settings → About: update modal reshows correctly after backdrop/ESC dismiss; DownloadStatus is reset before reopening.
- Updater: detect a wedged `/var/log` before launching install.sh; set `O_CLOEXEC` on the instance lock so post-install restart is not blocked.
- Restart: exec in-place instead of fork+exec to avoid a zombie race.
- Barcode scanner: AZERTY punctuation mapping corrected with a cross-layout regression test; BT pairing UX, keycode diagnostics, accent variant, reduced log noise.
- Bluetooth: register BlueZ `Agent1` so pairing completes into a real bond.
- Temperature graph: chamber series now shown for sensor-only (no thermistor) setups.
- Wi-Fi: require a live NetworkManager daemon before choosing the NM backend.
- Crash reports (AD5X / MIPS): surface the return-address register so MIPS backtraces resolve (prestonbrown/helixscreen#818).
- Bed mesh rendering: close a missed-wakeup race in the render thread's stop/request handshake.

### Changed
- Render performance: pre-blended card borders, 1:1 thumbnail blit, and camera FPS throttling cut per-frame CPU load.

## [0.99.34] - 2026-04-16

Hotfix for the v0.99.33 release: cross-compiled release bundles for every embedded platform (ad5m, ad5x, cc1, k1, k2, pi, pi32, snapmaker-u1, x86) shipped without `assets/config/`, causing "Could not load printer database" on first launch and breaking shipped themes, platform init hooks, print start profiles, and sound themes. v0.99.33 artifacts have been withdrawn.

### Fixed
- Release bundle omitted `assets/config/` seed tree — the refactor that split RO seeds out of `config/` updated `scripts/package.sh` but missed `mk/cross.mk`, which is the actual pipeline every embedded release uses. All `release-*` targets now ship `assets/config/printer_database.json`, `printing_tips.json`, `default_layout.json`, `helix_macros.cfg`, `themes/defaults/`, `presets/`, `print_start_profiles/`, `sounds/`, and `platform/hooks-*.sh`.

## [0.99.33] - 2026-04-16 [WITHDRAWN]

Major Bluetooth reliability overhaul, new barcode scanner settings UI, first-run guided tour, HttpExecutor for bounded HTTP threading, responsive setting rows that collapse 7 micro/ XML variants, and a broad config refactor splitting read-only seed data from writable state.

### Added
- First-run guided tour with coach-mark overlay, responsive tooltips, AMS-conditional steps, and replay from Settings > Help
- Barcode scanner settings overlay with BT device discovery, pairing, MAC binding, and USB device list
- HttpExecutor — bounded-worker HTTP executor (fast lane: 4 workers, slow lane: 1) replacing unbounded thread spawns
- Responsive setting rows with info icon, collapsing 7 micro/ XML layout variants (#805)
- New XML binding attributes: hidden_if_prop_eq/not_eq/empty and bind_style_if_eq/not_eq/gt/ge/lt/le (#805)
- Frame performance telemetry: idle filtering, per-panel breakdown, and separate render/flush timers
- LVGL display anomaly section in stability dashboard
- Bluetooth HID scanner binding by MAC address with exclusive grab
- BT HID link verification after pairing with bond-refusal warning
- Bluetooth `enumerate_known` API for paired device listing
- `HELIX_CONFIG_DIR` and `HELIX_DATA_DIR` env vars for Yocto/read-only rootfs deployments
- Config path resolver helpers (`find_readable`, `writable_path`, `get_data_dir`)
- Crash diagnostics: activity breadcrumb ring, cached heap snapshot, LVGL event dispatch hook
- Silenced hardware items logged at startup for easier debugging
- Moonraker silent request mode — suppresses `REQUEST_TIMEOUT` events for background queries
- Cycling encouragement messages during long installs (#809)
- Responsive keyboard sizing with elevated keycaps and smart contrast text
- KIAUH extension shipped and registered on release installs

### Fixed
- Crash: defer GridEditMode rebuild + harden LVGL event chain (#814, #812)
- Bluetooth: serialize all D-Bus operations (discovery, pairing, GATT, notifications) through BusThread, eliminating race conditions and thread-safety issues (#811)
- Bluetooth: `thread_id_` race, submit TOCTOU, slot unref routing, `StartNotify` fallback guard
- Network: async backend init eliminates UI-thread blocking; self-join deadlock and ethernet thread pool fixes
- Exclude object: sync removals from Klipper status, drop stuck optimistic visuals on print end, silence spurious pre-print RPC timeouts
- Moonraker API callbacks guarded with lifetime tokens to prevent use-after-free
- Controls: segmented homing button bar on controls and micro controls panels
- Slider: responsive knob padding at tiny/micro breakpoints; fix overflow_visible attribute name
- Z-offset: compact format and wider temp icon gap at tiny breakpoint
- Camera: sleep callback token survives stream stop/start cycles
- Watchdog: bail out of restart loop on persistent failure instead of infinite retries
- Tour: re-target highlight on breakpoint change; cancel on navigation away from Home
- AMS: unlink external spool updates UI when previous filament color was black
- Scanner: dismiss progress toast on pair failure; wrap BT thread spawns in try/catch
- Label renderer: render negative spool IDs as 'TEST'
- Tool state: atomic write for `tool_spools.json` prevents corruption on crash
- API/camera: catch EAGAIN on `join_helper` thread spawn in destructors
- Print status idle card: micro breakpoint polish, subject-driven visibility
- Installer: prefer `/user-resource` for temp dir on CC1
- Help icon: resolve via responsive theme token
- Updater: 2min timeout extended for slow printers

### Changed
- Config layout: read-only seed configs moved to `assets/config/`, writable state stays in `config/`
- Bluetooth: D-Bus operations serialized through dedicated BusThread instead of ad-hoc thread spawns
- Power-device API calls migrated to HttpExecutor with tok.defer lifecycle safety
- Bluez detection uses pkg-config instead of compile-probe
- Yocto build support: `PLATFORM_TARGET=yocto` mode, bitbake LDFLAGS, Docker dev loop

## [0.99.32] - 2026-04-15

Adds COSMOS firmware support for the Elegoo Centauri Carbon (CC1) and new per-channel display color correction (gamma, warmth, tint). Also includes a motion home widget, extensive crash/observer telemetry, and numerous discovery, installer, and widget fixes.

### Added
- CC1 (Elegoo Centauri Carbon) support on COSMOS firmware with factory white-balance calibration and live-hardware-driven preset
- Per-channel gamma + warmth display color correction (#803)
- Tint axis (G shift) for purple/magenta correction (#803)
- Motion home widget that opens the motion overlay directly
- Type icon stacked under cancel badge for gated panel widgets
- LVGL anomaly detection with call traces and arc/observer silent null-return logging

### Fixed
- Crash handler: stack-scan backtrace fallback for aarch64 and x86_64 (#795, #796)
- Crash: backtrace captured in terminate_handler (#801)
- Grid edit: stale child_count debug loop crash removed (#800)
- PrinterState: aliased hash copy eliminated in set_hardware (#799)
- Print status: Pause/Tune/Cancel enabled during Preparing phase (#798)
- Discovery: live hardware passed to auto_detect_and_save (#802)
- Discovery: retry on klippy state transitions, suppress retry toast, and differentiate deferred vs failed discovery
- Preset: deep-merge full preset and populate printer type from DB; CC1 gamma dropped to 1.0 to stop washing out shadows
- Panel widgets: eliminate load-on-every-access churn (#804); gate-aware rebuild short-circuit so ungating takes effect
- Touch calibration: settings-path accuracy, safety, and watchdog
- Update queue: queue_critical bypass for one-shot init callbacks
- Config: gate preset printer-type lookup out of splash/watchdog builds
- AD5X IFS: use IFS_REMOVE_PRUTOK when unloading active slot
- Snapmaker U1: keep WiFi alive across helixscreen stop/update (#797)
- Installer: escalate to SIGKILL without tripping set -e on orphaned procs; add CC1 + Snapmaker install dirs to uninstall search; work around COSMOS config-manager screen_ui allowlist on CC1
- Updater: cover all release platforms and prefer tar.gz on GitHub fallback
- Scanner picker: re-find device_list widget to avoid stale cached pointer
- LVGL: instrument lv_obj_delete_async for double-schedule + UAF telemetry
- About: source contributor list from committed CONTRIBUTORS.txt
- Home: observe every gate subject directly and shrink coalesce window so late-arriving capabilities un-gate widgets promptly
- Release manifest: default to tar.gz-only until pre-v0.99.31 ages out
- Build: stub helix_lvgl_anomaly() for splash and watchdog binaries
- Tests: stabilize eventloop shard flakes

### Changed
- Print status: button enable driven by subjects + XML bindings
- Home: panel rebuild triggered on capabilities_version instead of coalesce timer

## [0.99.31] - 2026-04-14

Adds a new XXLARGE responsive breakpoint tier and HiDPI font scaling for displays above 1000px tall (1440p/4K), with per-platform font pruning to keep binary size in check on constrained devices. Also includes a telemetry-driven printer database updater, bed mesh nozzle preheat, and a handful of crash and correctness fixes.

### Added
- XXLARGE responsive breakpoint tier for HiDPI displays (1440p/4K) with font, icon, spacing, and component scaling (#773)
- XLARGE/XXLARGE text and icon font assets with CJK fallback mappings
- Per-platform font tier pruning — each platform ships only the font sizes it needs
- Smart tier-aware fallback warnings when fonts are missing for the active breakpoint
- Interactive telemetry-driven printer database updater script with top-10 cap, dedup, auto-skip, and unique-device counting
- Bed mesh preheats the nozzle and bed via TEMPERATURE_WAIT before probing for more consistent results

### Fixed
- PrinterHardware dangling-reference crash after PrinterDiscovery snapshot change — guess_bed_heater / guess_hotend_heater / fan wizard now hold owned copies
- Theme rotation refresh preserving XXLarge breakpoint via shared helper
- Event depth counter detects event_head corruption on AD5X (#795)
- Network list deferred deletion during wizard cleanup causing heap corruption (#793)
- Wizard set_status() during cleanup causing blur walk crash (#792)
- Printer image caches now invalidated when the user changes printer image
- AD5X/ForgeX fan names, detection heuristics, and config paths corrected
- Empty hardware snapshots and false alerts after std::move of api->hardware()
- Release pipeline dispatches archive verification by format; MIPS ELF validation added
- K1/AD5X zip assets omitted from release manifest to unblock 0.99.29→0.99.30 updates
- FONTS_CORE extended and CJK xlarge/xxlarge guarded for platforms with pruned font sets

### Changed
- Unused mdi_icons sizes (20/28/40/56) removed, FONTS_CORE tightened
- AD5X IFS documentation clarifies stock zMod vs lessWaste/bambufy Moonraker visibility

## [0.99.30] - 2026-04-12

### Added
- ZeroG Hydra printer variants and updated Nebula naming in the printer database
- Power Devices chip in the printer manager overlay alongside LED controls
- Android `--test` mode launchable via intent extra

### Fixed
- Use-after-free in gradient cache during layout walk (#788)
- Hash table iteration crash in PrinterDiscovery copy-assignment (#789)
- Self-sizing gradient canvas replaces pre-rendered .bin files for correct rendering on high-res displays
- Diagonal smearing in backdrop blur caused by stride mismatch
- Aligned stride handling in the no-downscale blur path
- Spool canvas draw buffer resize use-after-free
- Modal dialog scroll confined to content area; added minimum height for breathing room
- Print select no longer repopulates when the file list is unchanged
- Metadata overlay flush and reduced thumbnail offset in print select
- Android display buffer resize after scaling, SW/GPU renderer routing, and GLES screen corruption
- Android nav bar white scrim eliminated; persistent nav bar with wider swipe edge zone
- Android contributor marquee animation on wide screens
- Android sounds and tracker MOD files extracted to writable storage
- Android update checker enabled with Play Store redirect
- Barcode scanners filtered from label printer Bluetooth dropdown (#779)
- AFC preferred over Snapmaker backend on U1 with aftermarket MMU (#779)
- Keyboard base character no longer inserted on long-press alternate key
- Perceptual volume curve replaces linear scaling for more natural sound levels

### Changed
- Android software renderer switched from FULL to DIRECT mode for better performance

## [0.99.29] - 2026-04-12

### Added
- High-DPI display support: DRM auto-downscale for panels exceeding 1920px, xlarge font constants for large displays, and fbdev resolution warnings (#773, #774)
- Android system keyboard toggle using native IME via SDL_StartTextInput (#774)
- Android ghost navigation bar with edge-swipe reveal and inactivity auto-hide
- GPU-accelerated SDL drawing backend with fixed temperature graph gradient bands
- Pre-print overlay now dismisses via authoritative Moonraker PRINTING state and RESPOND gcode completion match, replacing heuristic layer/progress triggers
- Zip archive support in release pipeline with tar.gz fallback
- Heap stats and startup phase snapshots in telemetry
- UX micro-breakpoints for responsive layouts (#763)

### Fixed
- Android black screen on app resume caused by missed SDL events while backgrounded (#774)
- Android high-DPI aliasing eliminated with integer display scaling (#774)
- Android crash reporter HTTPS failures bridged via JNI on devices without libhv SSL (#774)
- WiFi wizard click handler crash on deferred-deleted list items (#778)
- Nozzle temperature rows going stale due to lifetime token gate on version observer (#782)
- Printer discovery SIGSEGV from unsynchronized hardware struct copy (#777)
- DRM auto-downscale now correctly skipped when user explicitly sets `-s` (#773)
- Bed mesh fallback probe count divided by samples-per-point to match actual probe density
- G-code viewer streaming load failure now shows an error toast instead of crashing
- Null-guard on Moonraker API notify callback (#765)
- IFS spool eject detection via empty color field on Adventurer5M (#631)
- QWERTZ and AZERTY barcode scanner keymap support
- Webcam list filtered by service type to exclude unsupported streams
- Print select retries empty thumbnails on panel revisit
- G-code viewer layer re-frozen on terminal to idle transition

## [0.99.28] - 2026-04-10

### Added
- `-s WxH` CLI flag now honored end-to-end on DRM and fbdev backends: DRM connector mode selection, simpledrm detection with fbdev fallback, and fbdev kernel-size mismatch warnings surfaced as toasts once the UI is ready (#766)
- Bluetooth SDP channel resolver with cache — MakeID and Brother PT label printer backends auto-discover the correct RFCOMM channel and invalidate on stale-cache failures
- Forget button in the scanner picker modal and label printer settings panel for paired Bluetooth devices (plugin ABI: `helix_bt_remove_device`)
- Long-press a file card in print select to open the delete confirmation dialog
- Runtime log level setting in System settings
- Debug bundle default log tail raised from 200 to 2000 lines

### Fixed
- Back-to-back WiFi scans in the setup wizard could crash inside `std::sort` when a background callback rewrote the cached network list mid-sort (#769)
- LED toggle and brightness sent an all-zero color when the stored config was in a poisoned state, turning the strip off unintentionally
- Scroll position in print select card view was jarringly reset on every refresh tick
- Pre-print ETA now uses wall-clock total on printers with sparse phase detection, instead of producing wildly inaccurate phase-based estimates
- Modal destructor could dereference a stale backdrop pointer
- Printer discovery could double-fire hardware-discovered callbacks after the setup wizard, crashing in a race
- PrinterDiscovery hw-discovered callbacks accessed a destroyed instance when the discovery loop outlived the caller (#761)
- TipsWidget and CoalescedTimer could corrupt LVGL's timer linked list when cancelled during event dispatch (#760)
- Ethernet backend failed to detect non-standard interface names (#762)
- Emergency-stop warning text and position restored on probe calibration panel; cartographer calibration UX tightened (#754)
- BT Forget thread spawns wrapped in try/catch to survive ARM thread limits
- Cross-compilation targets serialize via a mkdir lock to prevent concurrent libhv source tree corruption
- Splash and watchdog binaries now link the display helpers added for #766
- Missing translations for settings menu across all languages
- Invalid `text_tag` XML attribute replaced with `translation_tag` on label printer and other panels
- Wizard defaulted to an empty host on fresh installs; now restores `127.0.0.1` when no config exists

### Changed
- Enhanced 2D G-code shading enabled by default (opt out via `HELIX_SSAO=0`); normal shading reverted to bidirectional at 0.12 strength for legibility

## [0.99.27] - 2026-04-09

### Added
- Enhanced 2D G-code shading with normal-based lighting, anti-aliased lines, and silhouette outlines
- Per-overlay visit tracking in telemetry dashboard

### Fixed
- Thumbnail display bugs in print select and detail views
- Print start timing heuristics for AD5M Klipper mod
- Use-after-free crashes in scanner picker modal and four overlay subclasses with shadowed lifetime guards
- Camera buffer use-after-free and timer deletion crash in setup wizard
- Unbounded mDNS pending records map growth from incomplete entries
- x86 self-update downloading wrong platform tarball (missing HELIX_PLATFORM_X86)
- Bluetooth discovery filter blocking barcode scanners
- WebSocket connection missing proper User-Agent header
- Race condition in G-code render completion flag ordering
- Modal button styling inconsistencies in action prompt and color picker
- Invalid text_secondary XML token replaced with text_muted

## [0.99.26] - 2026-04-08

### Added
- Material mismatch warning before starting a print when loaded filament doesn't match slicer expectations
- Telemetry tracking for in-app vs external print source distribution
- Zero G Nebula 370 printer image

### Fixed
- IFS native ZMOD port presence now inferred from save_variables and slot edits on AD5X
- Pre-print prediction history returning true with no entries, causing stale progress estimates
- Predicted total using dual atomics instead of mutex, risking torn reads
- Predicted weight computation not holding mutex during write
- Legacy bucket 0 entries lost when saving prediction history
- Weighted phase update not tracking detected phases for progress display
- Adaptive pre-print timeout completing prematurely on bed-first start macros
- BusyBox curl detection and HTTP mirror fallback for K1/AD5M installer downloads
- Filament mapping rows restyled as dropdown triggers with anchored picker and overflow clamp
- Dropdown trigger border not highlighted while filament picker is open
- Redundant 'nozzle too cold' warning toasts during filament preheat
- Part fan slider handle clipping at left edge
- Scanner picker not using declarative XML binding for Bluetooth availability
- Telemetry thumbnail/AMS rates, build volume source, and print source field corrections
- macOS CI test hang and TSAN timeout in nightly suite
- Moonraker external update not triggering restart on all platforms

### Changed
- Filament mapping rows now display as dropdown triggers instead of flat list items
- Bypass load routing encapsulated in AmsBackend::requires_slot_selection_for_load()

## [0.99.25] - 2026-04-08

### Added
- Adaptive pre-print time estimation: temperature-driven heating progress, thermal rate learning from PID calibration, and finish time ETA integration
- Bluetooth QR scanner support: discover, pair, and persist BT scanners from the scanner picker
- RGBW LED support: white channel toggle, detection, and color swatch (#737)
- White-only LED detection from Klipper configfile pin config (#748)
- Temperature graph legend chips displayed at 2x widget height or larger
- Auto-preheat for extrude/retract/purge when nozzle is cold with a known spool loaded
- Android Play Store readiness: AAB bundle build, back button handling, lifecycle pause/resume, and display diagnostics
- Klipper shutdown state detection with error message in recovery dialog
- Bed mesh probe progress tracking during pre-print
- Zero G Mercury One.1 and Nebula printer definitions

### Fixed
- Bed observer use-after-free from missing SubjectLifetime token (#746)
- Duplicate Chamber Temperature sensor from mock sensor pollution
- Fan widget 1x1 content centering and resolved display names at 2x1+
- Tool remap colors in 2D G-code renderer and chamber temp alignment
- Pre-print time composite remaining from thermal model + predictor
- Config migration using wrong JSON key for phase durations
- ETA extrapolation restored for progress 1–4% when no slicer estimate available
- IFS native ZMOD presence detection and dirty flag race on AD5X (#716)
- Spoolman active spool not syncing with Moonraker on assignment change
- Bluetooth scanner use-after-free from background thread callbacks
- Printer name discarded on manager overlay dismiss instead of saving
- Split button dropdown toggle, click-outside dismiss, and positioning
- Wizard preset mode incorrectly enabled for secondary printers
- Stale notification warnings after subject reinit
- Android: SDL.h build failure on embedded targets, WAKE_LOCK permission, USB sysfs guard, cache directory path, wizard localhost default
- Test runner false failures from teardown crashes and skipped test detection
- 2D G-code renderer elevation angle mismatch with OrcaSlicer thumbnail camera
- Navigation go_back re-activating closing overlay when animations disabled
- Initial fan/sensor status lost on multi-printer due to queue ordering race (#740)
- Fan speed percentage overlapping slider — moved inline to save vertical space
- G-code render mode not restored on print status reactivation, thumbnail offset
- Print status temperature card not fully clickable for temperature overlay
- External spool info not centered in card when no AMS present
- Pre-print predictor cache not invalidated on view open
- Release builds failing from transient apt-get network errors in Docker toolchain images

### Changed
- AMS spool edit actions consolidated into a split button ("Choose Spool")
- ETA display rounds to 30s/10s buckets for stable readout
- Print status temperature card is now fully clickable to open temperature overlay

## [0.99.24] - 2026-04-07

### Added
- Printer name sync: automatically resolve and write back printer name via Mainsail/Fluidd database on connect, rename, and wizard save
- Bed temperature widget as conditional last widget on home panel
- AMS widget automatically enabled on home panel when AMS detected during setup wizard
- Telemetry enhancements: periodic snapshots, frame time sampling, performance metrics, feature adoption tracking, and analytics dashboard with Performance, Features, and UX Insights views

### Fixed
- Render thread use-after-free from missing wait_for_finish_cb in SW draw unit (#739)
- Network list corruption when clearing entries (safe_delete_deferred)
- LED toggle on now respects saved brightness and color instead of defaulting to full white
- Custom keypad temperature ignored due to expired lifetime token
- Invalid text_primary theme token replaced with correct token
- systemd service startup ordering: use plymouth-quit-wait.service instead of multi-user.target (#536)
- Home screen widgets no longer disabled or show toast when grid is temporarily full during firmware_restart
- Chamber temperature regression in heater gcode generation (#745)
- Wi-Fi connection failures caused by PrivateTmp DGRAM socket isolation
- AMS loading error modal button wiring (#735)
- Use-after-free in bed/chamber observer subjects during grid edit (#734, #736)
- Print status view toggle button padding
- Print thumbnail now visible during Preparing Print phase on home screen
- Fan widget crash when subjects missing at startup
- Level screws showing checkmark icon for all screws, not just reference
- Telemetry timer pointers not nulled when LVGL torn down before shutdown
- Print completion dialog showing wrong icon for Failed/Cancelled states
- Fan arc excluded from long-press rename to prevent conflict with speed control
- Default widget grid layout bugs causing missing and misplaced widgets
- ForgeX printer detection: removed non-specific sensor heuristics
- Noisy warnings silenced on first unconfigured run
- PID calibration ETA smoothed with EMA dampening and 1-second countdown updates
- Artillery M1 Pro cooldown preset now includes chamber

### Changed
- Printer name editing converted to declarative subject binding

## [0.99.23] - 2026-04-06

A stability and polish release focused on probe handling, PID calibration UX, print status improvements, settings reorganization, and crash fixes across multiple subsystems.

### Added
- Smart PID calibration progress tracking with phase detection, ETA, and history persistence
- Timezone selection in display settings with IANA timezone support and UTC offsets
- G-code viewer progress/complete view toggle during prints
- Print Files button on print status widget to configure file picker toggles
- Centered thumbnail on print status widget when all action buttons are hidden

### Fixed
- Use-after-free in G-code streaming memory pressure callback (TOCTOU race, #733)
- Probe Z-offset not loading for FlashForge AD5M/AD5X with loadcell probes (#733)
- Probe discovery not seeding z_offset from config during initial setup
- Single-probe setups not auto-assigning the Z_PROBE role
- Ghost preview rendering too dark for short objects under 50mm
- Bed mesh probe clobbering existing default mesh when using temp profiles
- Probe accuracy test using wrong sample count and not pre-positioning to bed center
- Input shaper peak dot misaligned with graph, progress bar and table spacing issues
- Input shaper triggering disconnect dialog during SAVE_CONFIG restart
- Screws tilt adjust not auto-homing, unsanitized errors, small icons, button heights
- PID calibration progress bar not reflecting actual phase timing
- Nozzle temperature widget re-entrant drain crash replaced with lifetime invalidation (#732)
- Print status widget using positional child index instead of name-based lookup
- Timelapse webcam detection failures and missing retry option
- Camera probe crash and metadata re-fetching on AD5M (#724)
- Sound tracker playback disabled on AD5M/AD5X to prevent print kills
- IFS dirty flag not cleared on native ZMOD persist (#716)
- Multiple crash fixes: observer UAF, style cascade SIGSEGV, AFC null deref (#726, #728, #729, #731)
- Shutdown guards, deferred delete consolidation, and dynamic subject lifetime hardening
- Z-offset display not updating on main panel
- G-code viewer not activating when preview is set to thumbnail
- Chamber temperature not shown on unified temp card
- Screensaver option missing in micro layout

### Changed
- Settings reorganized into 6 category sub-panels
- Power Control renamed to Power Devices
- AMS Management renamed to Multi-Filament Management, card styling removed from rows
- Macro Buttons moved from Hardware to Printing settings
- AD5M/AD5M Pro split into ForgeX and stock printer database entries with macro_exclude heuristic
- Macros widget icon updated to match Advanced Settings

## [0.99.22] - 2026-04-05

### Added
- Artillery M1 Pro printer support with preset, print start profile, and platform hooks
- `--no-sound` flag and `disable_sound` setting to skip audio backend initialization
- Event-driven IFS re-read triggers using Adventurer5M.json instead of CHANGE_ZCOLOR macro
- Reset button in Widget Catalog overlay header
- 2 new translated strings across all languages

### Fixed
- SIGBUS crash in Moonraker health timer after long uptimes from destructor race (#717)
- Re-entrant rebuild crash in nozzle temperature widget (#723)
- IFS sensor re-read trigger narrowed to sensor changes only (was firing on unrelated events)
- Cancel/Reprint button visibility not syncing on panel activation (#546)
- File list not refreshing on reconnect and overlapping RPCs (#577)
- Invalid `flex_align` value in filament panel XML
- IFS dirty flag not cleared on color write failure
- Snapmaker U1 daemon directory and platform hooks deployment (#710)
- Concurrent `connect()` calls in test fixture causing flaky tests

### Changed
- IFS backend reads filament data from Adventurer5M.json via Moonraker HTTP instead of parsing GET_ZCOLOR output

## [0.99.21] - 2026-04-04

### Fixed
- Temperature keypad silently dropping commands when lifetime token expired during overlay hide
- IFS filament slot material/color reverting to stale values when editing multiple slots
- IFS material label not refreshing when only material changed (color unchanged)
- M300 beep command crashing backend from dangling MoonrakerClient pointer (#714)

### Changed
- AMS edit modal spool actions condensed into a split button dropdown to prevent overflow in translated UIs
- Modal button row now supports translation tags for primary/secondary buttons

## [0.99.20] - 2026-04-04

### Fixed
- Print file thumbnails showing placeholder icons instead of actual images
- Use-after-free crashes in async callbacks and observer guards (#704–#708)
- Background-thread lifetime callbacks using unsafe `this` pointer (#707)
- Deferred UI callbacks crashing when container layout is pending (#711)
- Sound settings not visible until hardware discovery completes
- Ghost taps after scrolling on capacitive touchscreens
- Snapmaker U1 installer referencing nonexistent init script
- Junk directories created in working directory from corrupted HOME environment
- CI release artifacts duplicated across platform prefixes

## [0.99.19] - 2026-04-03

### Added
- Chamber temperature control on the controls panel — set target, view status, and graph (community PR #688)
- Chamber temperature mini graph on the filament panel
- Filament eject icon with retract animation at slot sensor
- Safety warning automatically hidden when active spool material is known
- Runtime preset loading — FlashForge AD5M/Pro/5X presets applied automatically after detection
- ZMOD firmware auto-detection with firmware-specific preset support
- Nozzle and bed edit buttons now open the editor directly instead of the graph
- 8 new translated strings across all languages

### Fixed
- Crashes from observer use-after-free, event chain corruption, and DNS SIGSEGV (#697, #698, #700)
- Snapmaker U1 getcwd error and wrong init script path (#703)
- Sound sequencer stalling on thread-starved systems
- Touch calibration crosshair flash not appearing when animations disabled
- Modal dialog text clipping
- Screen artifacts on graceful shutdown (framebuffer not cleared)
- Print file card crashes from stale pool pointers and missing thumbnails
- AFC hub-routed lanes with per-lane extruders now correctly get PARALLEL topology
- IFS backend no longer incorrectly claims firmware spool persistence
- FlashForge AD5M/Pro presets split correctly; non-stock hardware removed
- Quick Actions header visible when macro row 2 active
- Ripple effect not rendering; settings reading calibration from wrong config path
- Duplicate crash reports in telemetry
- AD5M/AD5X preset naming updated for ForgeX and ZMOD firmware

## [0.99.18] - 2026-04-02

### Added
- Unified post-operation cooldown manager turns off extruder heater after filament operations complete (configurable delay, default 2 minutes)

### Fixed
- Touch not registering on AD5M/AD5X after upgrade — device-specific calibration removed from presets so each device calibrates during first-run wizard

## [0.99.17] - 2026-04-02

### Added
- Dynamic grid dimensions for ultrawide and portrait screen layouts
- 32 new HelixScreen feature tips with modal overflow fix
- Filament auto-preheat on load/unload with 2-minute delayed cooldown
- Snapmaker U1 default widget layout preset

### Fixed
- CFS filament swap now works correctly (M8200 param bug bypassed with direct CR_BOX commands)
- CFS active slot detection, partial update state preservation, and K1/K2 nozzle rendering
- Thumbnails now extracted from gcode headers on printers with old Moonraker lacking metascan
- Auto-home before filament load/unload/tool-change across all AMS backends
- Accurate layer tracking using virtual_sdcard.layer instead of linear estimation
- Temperature graph redraws throttled to 1Hz (was ~4Hz per series)
- Memory thresholds use actual available RAM for overlay lifecycle decisions
- Extruder selector rebuild deferred to prevent flex layout crash
- Favorite macro placeholder visibility and catalog ordering improved
- Filament path line gaps closed when backend has no prep sensors
- Non-rectangular 1x1 card backgrounds decomposed into maximal rectangles
- Hardware config synced with actual printer hardware
- Width sensor config loading and display values
- Boot persistence and WiFi for K1/K2 deploy targets
- Error icon token corrected in calibration panels
- AMS mini status widget now shows pressed visual feedback

### Changed
- Default display dim timeout bumped to 10 minutes, sleep to 20 minutes

## [0.99.16] - 2026-04-02

### Added
- Snapmaker U1 preset configuration with automatic wizard skip on detection
- Snapmaker U1 platform detection in installer with auto-start boot support
- `make dev` target for faster debug builds (-O0)

### Fixed
- Crash from unsafe DNS resolution on ARM and LVGL event chain corruption (#689, #690, #691)
- Abort detection now uses printer.info instead of gcode probe to correctly identify Kalico (#685)
- Snapmaker AMS backend not receiving status updates or populating slots
- Snapmaker AMS deadlock in status handler and incorrect extruder subscriptions
- Snapmaker active tool slot not marked as LOADED from toolhead.extruder
- Snapmaker active tool oscillation from incremental status updates
- Snapmaker filament slot using appended sub_type instead of base filament type
- Snapmaker filament color chips now parsed from Moonraker metadata and gcode headers
- Filament material matching uses compatibility groups instead of exact string comparison
- Card gradient backgrounds stretched to fill and rendered at exact dimensions
- Context menu item spacing increased for small screens
- IFS filament system now stores color/type natively without lessWaste/bambufy conversion
- Moonraker request timeout increased from 30s to 60s to prevent timeouts on slow networks
- IFS, CFS, and SnapSwap names displayed correctly in AMS wizard
- Snapmaker per-update debug logging removed (was causing UI freeze)
- Save Z-Offset button hidden for printers with auto-persisted z-offset
- AFC multi-unit ordering sorted by lane number instead of name (#554)
- Wizard step titles and subtitles now translated across all languages
- WiFi wpa_supplicant config persisted after connecting
- Printer name on home screen refreshes after editing in printer manager
- Redundant Power Devices entry removed from advanced panel
- TINY breakpoint (480x320) readability and AMS widget sizing improved
- Snapmaker S99screen recursion prevention and direct GUI restart

### Changed
- Z-offset GCODE_OFFSET strategy renamed to FIRMWARE_MANAGED
- Clog detection widget disabled by default
- Printer display name uses shared helper across UI

## [0.99.15] - 2026-04-01

### Added
- Snapmaker U1 DRM display backend with CRTC keepalive and persistent /userdata deployment

### Fixed
- Crash in temperature graph when parent object is invalid (#674)
- Config symlinks not restored after Moonraker web-type update
- Compressed fonts not rendering (LV_USE_FONT_COMPRESSED was disabled)
- Barcode scanner device ID read from wrong config source (#659)
- Chamber heater UI not showing when heater exists without a dedicated temperature sensor

## [0.99.14] - 2026-03-31

### Added
- Snapmaker U1 filament system support with RFID tag parsing and extruder state tracking
- Snapmaker U1 automatic detection via filament_detect printer object
- Filament macro detection with G-code fallbacks and parameter modal for manual load/unload
- Artillery Sidewinder X2 and Genius Pro added to printer database
- Spoolman fuzzy search with Levenshtein distance for typo-tolerant filament lookups
- Manual barcode scanner selection via USB vendor:product ID for devices not auto-detected

### Fixed
- Chamber heater UI not showing when heater exists without a dedicated temperature sensor
- Snapmaker U1 platform hooks corrected to use SysV init scripts instead of systemd
- Barcode scanners excluded from LVGL keyboard input to prevent ghost keypresses (#659)
- AMS material label not refreshing when slot color changes
- Docker cross-compilation portability for macOS/ARM (#649)
- Build dependency fixes for tools (#670)

### Changed
- CJK font files use RLE compression (~1MB savings)
- Source fonts excluded from release builds (~35MB savings)

## [0.99.13] - 2026-03-31

### Added
- Energy monitoring: power device widget with energy carousel, sensor picker, and mock data for testing
- Moonraker sensor discovery and subscription with SensorState singleton
- Fan rename via Settings panel and long-press on fan cards
- USB HID barcode scanner detection for any keyboard-class device

### Fixed
- External spool weight not syncing from Spoolman when no AMS backend is active
- Spoolman active spool notification handler parsing incomplete JSON-RPC messages
- Filament edit modal defaulting vendor dropdown to empty instead of Generic
- Double-free of libinput device paths on DRM teardown (#650)
- Boot display process not killed on K1 startup, causing stale boot logo (#642)
- libinput keyboard scan SIGABRT at startup on some devices (#648)
- Crash reporter stack-scanned backtrace for ARM32/MIPS plus race condition fixes
- Energy page layout tightened to avoid carousel dot overlap
- Fan touch feedback, event bubble for rename, output_pin prefix stripping
- WebSocket client missing close() call and missing ctime include

### Changed
- Power and power_device widgets consolidated into single two-column layout
- Request tracker decoupled from AbortManager and UI dependencies
- Inspector tool uses real request tracker, drops moonraker_client dependency

## [0.99.12] - 2026-03-30

This release adds fan management with output_pin support and RPM display, temperature graph overhaul via TempGraphController, touch calibration for DRM displays, K1 pre-print phase detection, and flexible nozzle temp widget layouts — alongside crash fixes, graph rendering improvements, and printer database additions.

### Added
- Fan management overlay in Settings with fan listing, type classification, and long-press rename
- Output_pin fan support with M106 P<index> control, fan_feedback RPM display, and Creality fan role detection
- Touch calibration support for DRM backend (#643)
- K1 series pre-print phase detection and progress display
- Nozzle temps widget now supports 1×1 and 2×1 layouts
- Printer name displayed on homescreen widget (#641)
- RH3D E3NG and Artillery M1 Pro added to printer database (#646)
- Exception message plumbed through crash reporter pipeline (#645)

### Fixed
- Temperature graph: consistent scaling across all contexts, correct 5-min and 20-min windows, gradient support at all sizes, target lines on mini graph
- Temperature graph: skip spurious initial rebuild on panel switch, limit history backfill to buffer capacity
- AFC spool assignment routed through AFC backend instead of bypassing it (#644)
- Print cancel RPC timeout increased to 5 minutes for large prints
- Fan role mapping corrected for K1/K1C, unconfigured fans detected, speaker override added
- Accelerometer detection uses AccelSensorManager instead of raw objects list
- GCode viewport scaling uses extrusion-only bounding box
- Smart home button with persistent overlays, printer image aspect ratio fix (#607)
- Touch press-on-capture fix for ns2009 calibration on Ender 3 V3 KE
- Guard against null callback in MoonrakerClient::connect (#639)
- Guard lv_obj_delete_async_cb against use-after-free (#638)
- Control/Filament buttons disabled on startup when Klipper not running (#640)
- IFS: removed unsupported SHOW=0 param, seed Moonraker DB on first load
- ACE: subscribe to ace Klipper object for realtime filament updates
- Friendly error message for accelerometer SPI communication failures
- Graph time axis uses POSIX strftime %I for musl compatibility
- M141 fan role hint corrected from Chamber Circulation to Chamber Exhaust
- Fan rename modal stripped down to fix MIPS SEGV crash at startup

### Changed
- Temperature graph internals refactored to TempGraphController for unified state management
- TempControlPanel renamed to TemperatureService
- Framebuffer cleared when crash report dialog closes
- HELIX_DEBUG_TOUCH and HELIX_DEBUG_TOUCHES environment variables unified

## [0.99.11] - 2026-03-30

This release adds QR scanner improvements and probe accuracy UX, alongside performance enhancements for Spoolman and camera handling, plus fixes for modal stale callbacks, temperature graph rendering, and multiple crash fixes.

### Added
- QR scanner snapshot fallback with local camera auto-discovery at startup
- Auto-lower bed to 150mm on moving-bed printers when opening QR scanner
- Probe accuracy test progress UX with live sample readout and quality assessment
- Color picker for spool edit modal
- Spoolman spool details bridge button in AMS edit modal
- Commanded and actual Z position stacking on position card
- Persistent disk cache for printer image at exact widget dimensions

### Fixed
- QR scanner overlay now renders above modals instead of beneath them
- QR scanner crash on overlay teardown and use-after-free from frame buffer cleanup
- QR decode crash and incorrect callback ordering during result handling
- Auto-save spool data directly from QR scan to prevent data loss
- Stale on_hide callbacks clearing active modal instance after re-show
- Overlay stack close callbacks causing SIGSEGV from synchronous subject observer invocation
- Temperature graph gradient rendering: uniform opacity, correct orientation, proper clipping
- Temperature graph background color mismatch on filament panel mini graph
- JSON error object parsing in motion panel; duplicate raw error toasts suppressed
- Spoolman 'method not found' toast when Spoolman not configured
- Spoolman list performance regression from repeated widget lookups on scroll
- Camera probe using HTTP GET instead of HEAD (mjpg_streamer rejects HEAD)
- Display animations defaulting to enabled on platforms without support
- Compressed .bin source images not detected by cache handler
- USB source tab button styling with proper contrast via declarative XML bindings
- USB button incorrectly hidden when Moonraker returns empty file list
- Filament spool click target dead zones eliminated
- Spoolman settings row always visible so users can configure it

### Changed
- Camera decoding at display resolution instead of full frame for better performance

## [0.99.10] - 2026-03-29

This release adds spool management enhancements — direct weight editing, tool remapping with dropdown, and remaining filament display — alongside fixes for print status not refreshing after navigation, multiple crash fixes, and expanded platform and internationalization support.

### Added
- Direct filament weight editing and remaining weight display in spool edit modal (#629)
- Tool dropdown in spool edit modal for remapping filament to tools (#630)
- Tool labels (T0, T1, etc.) shown in filament mapping card (#554)
- Warning-color tool badge when user has overridden the default tool mapping
- Touch calibration debug logging via HELIX_DEBUG_TOUCH environment variable
- AD5X IFS (Infinite Filament System) support with bambufy macro compatibility

### Fixed
- Print preview not showing after switching navbar tabs (#632, #633)
- Async deletion double-free crash from overlapping parent/child lv_obj_delete_async calls (#632)
- Use-after-free crash when camera stream thread is detached (#624)
- Use-after-free crash on shutdown from client destroyed after API/macro managers (#628)
- Spurious service restart after self-update and NTP clock sync (#536)
- Slot editing now works on CFS and ACE backends with persistent overrides
- AFC partial status updates no longer regress slot loaded state (#631)
- Dryer temperature limits enforced in UI; ACE max corrected to 55°C
- Humidity UI hidden for backends without humidity sensors (ACE)
- Stale print data persisting after print complete/cancel (#546)
- Temperature chart gradient fills restored; overlay lifecycle fixed (#616)
- Chamber icon corrected to fridge_industrial to match other panels
- Temperature labels use icons at small breakpoints to prevent clipping
- K1/K1C SSL enabled for update server connectivity
- Self-update no longer double-starts via path watcher sentinel

### Changed
- 190 new strings translated across 8 languages with regenerated CJK fonts
- Splash screen logo PNGs now include alpha transparency

## [0.99.9] - 2026-03-28

This release focuses on stability with fixes for multiple crashes, adds manual chamber sensor/heater assignment, and improves print history performance with virtual scrolling.

### Added
- Manual chamber sensor and heater assignment in temperature sensor settings
- USB gcode thumbnail extraction with Creality PNG format support (#610)
- Virtual scroll for print history list, improving performance with large job histories (#619)
- Sound settings promoted out of beta with tracker test button
- ACE environment sensor data (temperature, humidity) now exposed

### Fixed
- Crash from backdrop deletion during AMS notification dispatch (#620, #621)
- Crash from synchronous widget deletion in picker and busy overlay dismiss
- Crash from unguarded async gcode callbacks in bed mesh calibration (#611)
- Crash from null subject API calls during startup or reconnection (#617)
- Crash from PowerDeviceState observer firing before PrinterState initialized
- ETA display now respects 12/24-hour time format setting (#597)
- Touchscreen: NS2009 detection fixed, affine calibration disabled during recalibration (#623)
- Filament load/unload now routes through AMS backend when active
- Print history fetches all jobs instead of only the first 500 (#619)
- Print history lifetime stats now use server totals for accuracy (#619)
- Temperature chart sizing and double-deactivate in temp graph overlay (#616)
- Print status memory-aware widget caching and correct temp row height (#617, #618)
- Systemd unit self-healing and removal of unsupported Moonraker options (#617)
- NTP time correction no longer triggers unnecessary service restart (#536)
- Boot race on Plymouth systems resolved by waiting for multi-user.target (#536)
- Filament type JSON array normalization from Moonraker (#554)
- USB file browser: K1C /tmp/udisk mount detection, source selector shown on startup (#610)
- Motion panel: out-of-range errors now show Klipper axis limits (#610)
- Duplicate T0 extruder entry in multi-tool initialization

## [0.99.8] - 2026-03-28

This release enriches the print status panel with speed/flow indicators, estimated finish time, Z height, and a unified temperature card with chamber support. It also fixes several crashes and UI issues across filament management, temperature controls, and navigation.

### Added
- Print status: speed and flow rate indicators visible on medium and larger screens (#597)
- Print status: estimated finish time ETA in metadata overlay (#597)
- Print status: Z height shown alongside layer progress (#597)
- Print status: unified temperature card with chamber temp support (#597)

### Fixed
- Nozzle temps widget: color-coded temperature display replaces progress bars; compact font and bed icon on small screens
- Speed/flow row on print status panel is now clickable to open the Tune overlay (#597)
- Spoolman error toasts no longer appear when Spoolman is not configured (#609)
- Bed mesh probe modal now shows emergency stop instead of cancel
- Power widgets now work when Moonraker is connected but Klipper is not running (#587)
- File list always refreshes when returning to print select panel (#577)
- AMS slot crash from dangling widget pointers during deferred deletion (#604, #579)
- Overlay state not clearing when switching navbar tabs (#607)
- Material temperature save button hidden when preheat macro was selected (#588)
- Temperature chart uses deci-degrees for smoother lines (#600)
- Z offset baby stepping: added MOVE=1, clamped to ±2mm, fixed float rounding (#592)

## [0.99.7] - 2026-03-27

### Added
- Bed mesh calibration now shows determinate progress by querying the configured probe count

### Fixed
- Bed mesh progress bar could get stuck; suppress spurious disconnect toast on profile save
- Spoolman active spool now auto-assigns to the active tool on tool changers (#543)
- AMS slot and path updates deferred to prevent race conditions during hardware discovery (#562, #563)
- About page marquee scroll on wide screens
- Installer self-update on SonicPad (kernel 4.9) when sudo is unavailable
- Startup restart loop on systems with Plymouth boot splash (Armbian, Raspberry Pi OS) (#536)

## [0.99.6] - 2026-03-27

This release adds ACE filament system support, overhauls the filament mapping UI with an inline slot picker and smarter color matching, and continues hardening async callback safety across the UI.

### Added
- ACE filament system support: auto-detection, WebSocket subscription with REST fallback, feed/retract/feed assist device actions, and missing bridge warning
- Inline slot picker context menu replacing the separate picker modal (#554)
- Material-aware color matching with positional fallback when no color match is found
- Warning indicators for tools mapped to empty slots or with material mismatches
- Auto color map toggle in the filament mapping modal
- Gcode material label shown on filament mapping tool rows
- Multi-unit AFC slot label disambiguation

### Fixed
- Deferred object cleanup in AMS panel and exclude objects overlay to prevent crashes (#555)
- Async callback safety migrations for controls panel, Z-offset calibration, camera stream, print status, and screensaver (#550, #552, #553, #555)
- Scrollbar on mapping and picker dialogs when content overflows
- Wizard skip button blocked by touch overlay, stale callbacks, and sample counter off-by-one
- Incorrect theme token, invalid flex stretch, and splash screen timeout

### Changed
- ValgACE renamed to ACE throughout codebase and translations
- Filament mapper allows slot re-use across backends instead of enforcing uniqueness
- QR scanner latency reduced via frame subsampling and offloaded decode
- Updated CJK fonts and added do-not-translate markers for technical terms

## [0.99.5] - 2026-03-27

This release introduces AsyncLifetimeGuard — a unified mechanism for safe async callback handling that replaces all ad-hoc guard patterns — and migrates every modal, overlay, panel, widget, backend, and state manager to use it. Memory usage on constrained devices is further reduced through compile-time feature gates and runtime optimizations.

### Added
- AsyncLifetimeGuard for unified async callback safety, integrated into Modal and OverlayBase base classes
- Compile-time feature gates (CFS, IFS, label printer) to reduce binary size on constrained devices (#546)
- Anet ET5 Pro printer detection

### Fixed
- Crash from gcode renderer use-after-free when destroyed after streaming controller
- DebugBundleModal upload callback not marshalled to UI thread
- Premature restart during update when release_info.json not preserved during atomic swap (#547)
- RGB565 spool canvas breaking transparency on devices with alpha blending

### Changed
- All async callback handling migrated from ad-hoc guard patterns to AsyncLifetimeGuard
- Build hardened with FORTIFY_SOURCE, stack protector, and frame pointers across all platforms
- Reduced memory on constrained devices: smaller canvas sizes, lighter sound theme, tighter gcode cache

## [0.99.4] - 2026-03-26

### Added
- ALSA sound backend with 4-voice polyphony and MOD/MED tracker music playback
- Active Spool widget showing current filament via Spoolman integration (#545)
- Open source licenses section on the About page
- Auto-detect tape width for Brother PT label printers

### Fixed
- Crash from object deletion during LVGL event processing (#543)
- Stale print outcome not clearing when starting a new print after complete/cancel (#546)
- K1C: stabilized time estimates, fixed layer count, added arc support, improved pre-print status
- K2 platform misidentified as AD5M during installation (#544)
- ui_button user_data collisions causing crashes in temperature presets, macros, modal buttons, and print status controls
- Stale modal stack entries when animations are disabled
- Brother PT label printing: single RFCOMM connection, QR code clipping on narrow tape, rotation, auto-cut, dropdown corruption, and reconnect timing
- Die-cut label layout too wide for 38mm tape
- Print status buttons not anchored to bottom of controls column
- Notification badge positioned incorrectly on content-sized containers

### Changed
- Display rotation probe moved from DisplayManager init to Application startup

## [0.99.3] - 2026-03-26

### Added
- Brother PT (P-Touch) label printer support via Bluetooth with auto-detection, PTCBP raster protocol, status feedback, and PackBits compression
- Startup sounds for UI themes

### Fixed
- 5 auto-reported crash bugs with defensive guards (#525+)
- Emergency stop modal validation and toolchanger spool persistence (#540)
- SSH (dropbear) preserved when disabling stock UI on Creality K1 (#535)
- Hidden network WiFi connection and password validation for secured networks
- Config loss during in-app upgrade with backup restore and corruption recovery
- Update service template surviving Moonraker extraction

### Changed
- Print abort timer replaced with RAII lifecycle wrapper
- Cleanup guards added to unguarded queue_update callbacks

## [0.99.2] - 2026-03-25

### Added
- Per-unit environment display and dryer controls for AMS
- Hardware-gated widgets shown as disabled instead of hidden
- x86_64 platform detection and binary validation in installer

### Fixed
- Crash on RGB565 embedded builds from blur tree walk (#528)
- Crash from stale parent pointer in modal dialogs (#522, #523, #524)
- Crash from use-after-free in action prompt modal (#514, #515, #521)
- Crash from PrintStatusWidget use-after-free during macro timeout (#522)
- Brother QL label printing: raster alignment, die-cut support, auto-detection, and async Bluetooth
- Print metadata not fetched when thumbnail was already set (#526)
- Debug bundle upload spinner and text alignment

## [0.99.1] - 2026-03-25

### Added
- Creality K1 and K2 toolhead rendering with auto-detection and settings dropdown
- CFS device actions: refresh, auto-refill toggle, and nozzle clean
- Turbo jog mode with 10mm and 50mm step sizes on motion panel
- Easier carousel swiping and smart home button on home panel
- Telemetry tracking for home panel widget placement and interactions

### Fixed
- Crash from fan widget use-after-free during carousel rebuild (#517, #518, #519, #520)
- Crash from fan picker backdrop deletion corrupting LVGL event list
- Screensaver UI bleed-through from panel lifecycle not being suspended
- Home carousel page tracking using wrong observer type
- Dashboard Y-axis labels cut off; missing CFS/IFS AMS type entries
- Toolhead heat glow drawn behind toolhead body; AntHead glow position corrected
- K2 renderer heat block visible through U-cutout
- Tool changer spool assignments not loaded on startup
- Telemetry hardware profile recorded before build volume was available
- Toolhead settings dropdown not respecting test mode for debug options

### Changed
- Toolhead dropdown separated into native vs aftermarket styles
- Dashboard utilities extracted into shared module

## [0.99.0] - 2026-03-24

A major release bringing multi-page home screen, exclude object map, temperature graph widget, Creality K2/CFS support, preheat macros, tool changer improvements, and a settings.json rename — across 100+ commits.

### Added
- Multi-page home screen with carousel navigation, per-page widget layout, and page deletion (#484)
- Exclude object overhead map view on print status panel with convex hull outlines and touch-to-exclude (#511)
- Temperature graph dashboard widget with sensor config modal, color picker, and adaptive sizing
- Creality K2 Max support: printer definition, preset, platform hooks, and deploy targets
- Creality Filament System (CFS) backend with RFID material database, slot addressing, and status parsing
- Tool changer improvements: tool switcher widget, nozzle temps widget, preheat all tools (#493)
- Preheat custom macro support with per-material macro picker, toggle, and cool down button (#486)
- Fine/Coarse jog toggle on motion panel (#505)
- Optional text labels on icon-only home screen widgets (#501)
- First-class prtouch_v2 z-offset calibration for K1/K1C/K1 Max
- Probe accuracy test results displayed in formatted modal
- x86_64 Debian release target for x86 SBCs
- Creality K1, K1 Max, and K1C linked to k1 preset
- 74 new translated strings across all 8 languages to 100% coverage
- IPP sheet label printing for standard inkjet/laser printers

### Fixed
- Print status thumbnail not visible on first navigation in thumbnail-only mode
- Crash from camera stream callback race and LRU cache splice (#491)
- Crash from macros panel param modal at startup with null screen (#491)
- Crash from null pointer in home panel arrow event callbacks on non-SDL displays
- Crash from wifi wizard use-after-free on network item click
- Crash from null guards in LVGL object deletion path (#511)
- AFC error state not clearing on modal dismiss (#497)
- Bed mesh auto-home before calibration with sanitized error messages
- K1C probe using wrong calibration method (now uses standard probe_calibrate)
- Power disconnect dialog suppressed for all power-off events (#469)
- Service double-restart from pending update watcher on self-update (#509)
- Dangling symlink install error (#496)
- K2 display rotation set to 270° in preset
- Crash from fan widget animation teardown race condition
- CFS slot parsing, subscription, and load/unload operations now use M8200 protocol directly

### Changed
- Config file renamed from `helixconfig.json` to `settings.json` with automatic migration
- Controls panel reorganized: QGL/Z-Tilt moved to Calibration card, enlarged quick action buttons with slots 3 & 4
- Spoolman layout compacted with side-by-side sync toggle and interval
- Dropdown styling improved with responsive item spacing and clipped corners
- Thermistor widget renamed to Temperature Sensors
- Persistently disable stock Creality UI on K1 devices (#495)
- Print status buttons converted to outline style

## [0.98.12] - 2026-03-23

### Fixed
- Systemctl shell syntax in service restart wrapper (#495)

## [0.98.11] - 2026-03-23

### Added
- Camera rotation and flip configuration with edit mode modal (#483)
- Multi-instance thermistor and fan dashboard widgets with icon picker (#342)
- Thermometer variant icons added to icon font (#342)
- `HELIX_SCREEN_SIZE` environment variable as alternative to `-s` flag

### Fixed
- Crash from overlay close callback using synchronous deletion (#491)
- Crash from null style pointer dereferences (#490, #439, #480)
- Crash from blur walk callback and bed mesh layout update (#417, #419, #420)
- Print status auto-navigation blocking the UI thread (#450)
- EGL rotation fallback now continues unrotated with user guidance instead of crashing (#457)
- EGL rotation working on GL renderer path (#457)
- AFC tool-slot reconciliation using registry and merging stepper lanes in mixed setups (#421)
- Update watcher paused during startup to prevent spurious restart (#470)
- Self-restart via fork+exec when no watchdog supervises the process
- Sensor list and power picker card scrollable so content is not cut off (#342, #467)
- AmsState subjects initialized before testing sync_active_spool_after_edit

## [0.98.10] - 2026-03-22

A stability-focused release addressing over 30 crash reports, plus a new power device dashboard widget, multi-instance widget system, and fixes for power device detection, config symlinks, and memory management.

### Added
- Power device dashboard widget with live on/off state, device picker, and icon customization (#467)
- Multi-instance widget system: mint, delete, and rearrange multiple copies of the same widget (#342)
- Favorite macro widget migrated to multi-instance system (#342)
- Power-related MDI icons added to icon font (#467)
- Hardware name telemetry for improved printer detection analytics

### Fixed
- Multiple crash-causing animation callbacks, null dereferences, threading violations, and delete-during-iteration (#442–#482)
- Crash handler using async-signal-unsafe `memset` (#441, #445, #447, #481)
- Cache loader crash from dangling reference and unguarded layer load (#446)
- Power device names not parsed from Moonraker array response (#466, #469)
- Disconnect triggered when turning off power devices (#469)
- Config save breaking symlinks for helixconfig.json (#471)
- Shutdown crash from JobQueueState destruction ordering
- Shutdown crash from held SubjectLifetime in power device widget (#467)
- Memory monitor improved with hysteresis, smoothed growth tracking, and pressure responders
- Debug bundle warnings silenced, printer database compacted for memory savings
- Fan arc widget not applying track width when arc already at target size

## [0.98.9] - 2026-03-17

### Fixed
- Multiple crash-causing synchronous object deletions inside UpdateQueue timer callbacks (#422, #423, #429, #430, #435, #436, #437)
- Overlay animation crash when panel is freed mid-animation (#428, #436, #439, #440)
- Bluetooth thread capturing loader reference that could dangle after thread detach
- GCode viewer using unsafe custom async deletion instead of LVGL's built-in cancellable delete
- Screws tilt adjust panel not initializing API before lazy panel creation (#402)
- AD5X backlight staying on during sleep mode (#431)

## [0.98.8] - 2026-03-15

### Added
- Crash reporter now includes stack dumps, extended ARM32 registers, and full memory maps
- Crash analysis worker displays stack scanning results and memory map details

### Fixed
- Temperature keypad showing raw centidegree values instead of degrees (#401 related)
- Temperature validation falsely flagging valid temperatures as out-of-range
- Heater display dividing by 100 instead of 10 for centidegree conversion
- Config files in Fluidd/Mainsail not editable due to Moonraker reserved path conflict (#401)
- Screensaver async deletion crash and arc subject null guard (#409, #410, #411, #412)
- XML style constants triggering LVGL warnings from incorrect hex color prefix
- Bluetooth plugin missing from Pi release packages
- C++17 structured binding capture incompatibility with GCC 11

### Changed
- Animations disabled on K1, AD5M, and AD5X platforms for improved performance

## [0.98.7] - 2026-03-14

### Added
- Spoolman server setup: configure, change, or remove Spoolman directly from Settings
- MakeID/Wewin BLE label printer support (beta)
- QR scanner flash-bulb effect on successful recognition
- Beacon onboard accelerometer detection for input shaper
- Configurable sections in print status widget
- K1 platform preset with PRINT_PREPARED pre-start gcode
- HTTP fallback installer for no-SSL environments (K1/AD5M)
- K1C and K1 SE printer images

### Fixed
- First-boot rotation probe leaving overlays at half-width until restart
- QR scanner use-after-free crash on successful scan
- Overlay use-after-hide crash from stale alive guard
- Buffer meter destructor crash from dangling on_draw callback (#400)
- KIAUH installer PermissionError when probing install paths (#403)
- Installer failing to stop K1 stock Creality UI during setup
- Installer missing trailing newline when appending to moonraker.asvc (#408)
- LED wizard not saving selection to selected_strips config
- Docs URL pointing to non-existent docs.helixscreen.org

## [0.98.6] - 2026-03-13

### Added
- Bluetooth Low Energy (BLE) read support for Niimbot label printers, fixing D110 printing
- Creality K1C setup guide for installation documentation

### Fixed
- Modal double-free crash from synchronous deletion during LVGL event processing (#399)
- Wayland CSD title bar eating SDL content area
- AMS panel layout with full-height sidebar and text overflow
- Print status widget spacing and hidden icons at 2x2 breakpoint
- Theme engine crash when parsing empty palette for single-mode themes
- Belt tension mock CSV download and results layout
- Info QR modal icon alignment on small breakpoints
- Muted text color lost in button auto-contrast and widget label visibility
- Niimbot protocol packet sequence and blank row handling for D110 compatibility
- Slot registry not clearing old slot when remapping a tool
- Test runner silently swallowing shard failures due to pipeline exit code bug

### Changed
- AMS and micro header titles use uppercase text transform (i18n-safe)
- Midnight dark theme added to theme selection
- Theme engine supports handle_style/handle_color properties and transparent button variant

## [0.98.5] - 2026-03-13

### Added
- Belt tension tuning panel with resonant frequency measurement, strobe fine-tuning, and hardware auto-detection [BETA]
- Belt tension tuning guide in user documentation

### Fixed
- Print cancel now uses `printer.print.cancel` RPC instead of raw CANCEL_PRINT gcode
- Belt tension panel crash from missing JSON `result` wrapper, incorrect icon name, and missing translations
- False mouse cursor appearing on Allwinner SoCs due to MCE IR receiver detected as USB mouse
- Header bar back buttons missing pressed opacity feedback
- Controls panel missing translations for Nozzle, Off, Cooling, fan speeds, macro names, and sensor count
- Content-sized metadata overlays and non-responsive filament mapping pills
- Startup touch grace period reduced from 5s to 1s
- Installer now handles turbojpeg package name differences across distros

### Changed
- Belt tension help text uses progressive disclosure (help icon modals) to reduce scrolling
- CJK fonts regenerated with 1073 characters covering new translation strings
- Translation sync improved: 60 missing keys translated, backfill and coverage truncation fixed

## [0.98.4] - 2026-03-12

Timelapse video browser, USB mouse support, runtime CJK fonts, and filament system improvements.

### Added
- Timelapse video browser with thumbnail grid, render progress tracking, and video playback
- USB mouse support for DRM and fbdev backends with sysfs capability scanning
- Runtime CJK font loading — wizard codepoints compiled in, full character sets loaded on demand
- AntHead toolhead renderer for PrintersForAnts printers
- Library card on home panel with Print Files, Print Last, and Recent actions
- Compact locale-aware date formatting utility

### Fixed
- HDMI CEC devices falsely matched as mouse input, causing spurious pointer events
- AMS ZMOD IFS detection without lessWaste plugin installed
- AFC UNSELECT_TOOL sent invalid TOOL= parameter; added extruder LED support
- Timelapse empty state centering and missing icons
- Icon codepoint sort order in font generation
- Print file detail header bar layout on narrow breakpoints
- Timelapse switch background transparency on options card
- Crash from deferred MacrosPanel rebuild and unguarded CameraStream callbacks
- Duplicate linker symbols for input device scanner in Pi fbdev builds
- Print thumbnails showing grey box and uploaded files not appearing

### Changed
- Timelapse graduated from beta — feature gates removed
- AMS overview and detail headers unified with context-aware back button
- GitHub Actions upgraded from Node 20 to Node 24

## [0.98.3] - 2026-03-12

Macro browser, toolhead renderer, touch and filament fixes, and internationalization improvements.

### Added
- Macro browser panel with description display, one-tap execution with toast feedback, and chevron hidden for no-parameter macros
- JabberWocky V80 toolhead renderer
- Client-side crash report deduplication to prevent repeated uploads
- Default display brightness raised to 80% with automatic migration for existing users
- USB keyboard support for DRM and fbdev backends on Pi builds

### Fixed
- AFC direct_load hub field treated as direct instead of hub-routed (#392, #364)
- Tool changer uses Tn gcode for tool changes, fixing load/unload/purge on non-T0 tools
- IFS real-time sensor updates, stuck operations, and unload homing
- Touch calibration auto-detects and corrects swapped touch axes
- Display wakes on touch during dim/screensaver
- Crash from pending layout flush before widget tree rebuild
- Crash from safe_delete loop in MacrosPanel (#394)
- Post-update restart no longer shows crash report modal or sends crash telemetry

### Changed
- Advanced panel sections reordered by usage frequency, emergency bar pinned at bottom
- Color mismatch dialog shows color names instead of hex codes
- Wizard and macro enhance strings wrapped for internationalization (20 new translation keys across 8 languages)

## [0.98.2] - 2026-03-12

### Fixed
- QR scanner now shows live camera feed with proper mirror flip and visible close button with 60-second auto-timeout
- DRM display prefers PRIMARY plane over OVERLAY and suppresses fbcon bleed-through (#334)
- Bed mesh calibration uses StandardMacros for command instead of hardcoding
- Bed mesh no longer starts async render thread when no mesh data is loaded
- Clog meter and fan carousel widget spacing tightened (#331)
- macOS build uses pkg-config-compatible libusb include path

## [0.98.1] - 2026-03-11

Lock screen, probe management, QR spool scanning, Happy Hare device actions, and HTLF mixed topology support.

### Added
- Lock screen with PIN entry, auto-lock on wake, and Security settings for PIN management
- QR spool scanner for Spoolman spool assignment from the filament panel
- Probe management with Cartographer, Beacon, and Generic/Klicky type-specific panels, Z-offset calibration, and live config editing
- Happy Hare device actions: LED, eSpooler, and flowguard control with topology filtering, persistence across reconnects
- AMS HTLF mixed topology rendering with direct+hub lane paths, bezier curves, and hub indicator
- Input shaper Kalico smooth shaper support for calibration
- Bluetooth setup guide for Raspberry Pi and BTT Pi

### Fixed
- Crash from ghost thread when switching gcode/streaming controller (#387)
- Crash from CameraStream shared_ptr race and ToastManager observer corruption
- Crash from telemetry auto-send timer firing when telemetry disabled (#380)
- Crash from animation on modal backdrop deletion with stale dialog pointer
- Crash from missing CA certificates on AD5M breaking all HTTPS connections
- Home panel SIGSEGV during overlay push from null active_widgets_ iteration (#362)
- Config migration for printer switcher default on v5 fresh installs
- Config wizard not re-running after restoring from backup

### Changed
- Probe management graduated from beta
- Cherry-picked 4 upstream LVGL fixes from post-9.5.0 master
- Symbol files now attached to GitHub releases for permanent retention
- Crash symbolication tool supports AD5M, AD5X, and CC1 platforms

## [0.98.0] - 2026-03-10

Bluetooth label printing, filament mapping, Spoolman location support, and new home panel widgets.

### Added
- Bluetooth label printing with Brother QL (RFCOMM), Phomemo (SPP), and Niimbot (BLE GATT) support
  - BlueZ D-Bus device discovery and pairing UI in label printer settings
  - Bluetooth plugin loaded dynamically via dlopen for optional dependency
- Filament mapping modal for print-time tool-to-spool remapping
- Spoolman location field — filter by location, edit in spool modal, shown inline in list rows
- Bed temperature home panel widget (1x1, scalable to 2x2)
- AD5X IFS backend for FlashForge Adventurer 5X Intelligent Filament Switching
  - 4-lane switching with tool mapping, color/material/presence tracking, bypass mode
  - Mock mode: `HELIX_MOCK_AMS=ifs`
- Venture Delta printer to database
- Label printing user guide for Brother, Phomemo, and Niimbot printers

### Fixed
- AD5X heuristics strengthened to prevent AD5M misdetection (#375)
- Screws tilt panel always shows results table with success banner (#309)
- LED strip list no longer lost when strips are pruned during discovery (#373)
- LVGL observer null-guard prevents crash on subject removal (#378)
- AFC lane tracking on print start with toolchanger tool_number parse (#379)
- AMS spoolman_actions visibility race on modal open (#311)
- Screensaver activation on devices without backlight dimming
- macOS build using pkg-config for libusb

### Changed
- Home panel widget guide updated with all current widgets

## [0.97.5] - 2026-03-10

AFC device configuration improvements with toolhead distance editing and multi-extruder support.

### Added
- AFC Toolhead section with editable extruder distance actions and live extruder overlays
- Multi-extruder toolhead actions with per-lane dist_hub configuration
- Editable numeric text inputs replacing slider value labels in AMS device sections
- Creality Sonic Pad to supported platforms

### Fixed
- AFC HTLF lanes grouped by physical extruder for correct nozzle count (#364)
- AFC textarea jumping, corrected purge/wipe config sources, removed dead config section
- AFC mutex added to get_device_sections to prevent race condition
- AMS section rows always navigating to Setup due to wrong event target

## [0.97.4] - 2026-03-09

### Added
- USB label printer support (Phomemo M110) with auto-detection via libusb
- Split button widget with primary action and dropdown selector
- Preheat home panel widget
- Duplicate option in Spoolman spool context menu
- udev rule for USB label printer device access

### Fixed
- SIGSEGV from synchronous object deletion in timer callbacks (#367)
- SIGSEGV in BufferStatusModal destruction during show (#366)
- Null libusb handle dereference on AD5M platform (#368)
- AMS edit dropdown defaults not syncing for empty slots
- AMS edit modal width too narrow on smaller screens
- Label printer icon rendering, dropdown alignment, and spool edit layout
- A4T toolhead drawing too small on AMS canvas
- Printer config validation accepting non-printer object keys
- WiFi polkit rule priority and permission error detection

### Changed
- MIPS crash reports now include register extraction, stack dumps, and unwind tables (#365)
- In-app self-updates use atomic directory swap for reliability
- Translation sync with 15 new keys

## [0.97.3] - 2026-03-08

Label printing, Spoolman improvements, and startup performance.

### Added
- Print labels directly from the spool edit modal via connected label printers
- mDNS auto-discovery for label printers in Spoolman integration
- Improved label printer presets with richer Standard layout and test print
- Pass downloaded config content through to print start analysis

### Fixed
- WiFi polkit permission errors now detected and displayed correctly, including "insufficient privilege" variant
- PARALLEL AFC lane count inflated by cross-unit remap (#363)
- Null dereference in home panel during widget drain (#362)
- Null static widget pointers during multi-printer switch teardown
- Active spool not synced when editing the currently loaded AFC slot
- Checkbox checkmark invisible when accent color matched background
- WebSocket SIGSEGV in onclose callback during destruction (#357)
- Active spool not synced to filament panel after Spoolman edits
- Stale LED strip names persisted after hardware discovery (#360)
- Startup race between PrintPreparationManager and MacroModificationManager
- Label printer raster mode and horizontal flip issues

### Changed
- WebSocket connection now starts during splash screen for faster startup
- Panel widget rebuilds skipped when widget list is unchanged
- Full translation sync with 100% coverage
- Installer re-bundles polkit restart for immediate rule activation

## [0.97.2] - 2026-03-08

### Added
- Decimal point button and Android-style bottom row layout in numeric keypad
- Actual speed and flow rate display in tuning overlay

### Fixed
- Crash from object deletion in UpdateQueue callbacks; shared library addresses now detected in crash reports (#355, #356)
- Shutdown hang from DRM page-flip poll blocking indefinitely (#334)
- Stale LED macro entries persisted after strip deletion or config reload (#329)
- Slider and arc knob clipping in fan and PID menus (#331)
- Self-update restart loop caused by stale PathExists systemd watcher
- Missing polkit rule not detected or auto-created during self-update
- Polkit auto-creation failing on systems using .pkla format instead of .rules
- Crash worker extracting metadata from wrong debug bundle JSON keys

### Changed
- Printer switcher toggle moved from settings to Manage Printers overlay
- Sensors overlay headers updated to use consistent section header style with status badges

## [0.97.1] - 2026-03-07

### Added
- Callback tagging in UpdateQueue for crash diagnostics (#345)
- Filament system data collection (AFC, MMU, Spoolman) in debug bundles
- Toggle to show/hide printer switcher in navbar

### Fixed
- Modal use-after-free, label buffer overflow, and missing unwind tables in crash handler
- Input shaper chart and table data missing in dual-axis calibration (#341)
- LED macros with empty display names not rejected (#329)
- BGR framebuffer color swap not auto-detected on some displays (#344)
- AFC Vivid stepper motors incorrectly counted as lanes
- AFC toolchanger+hub setups showing wrong topology and tool labels
- Printer switcher toggle always visible instead of respecting settings
- Polkit permission errors not reported in WiFi NetworkManager backend
- Crash report modal layout broken when no network available for QR display
- Empty spool list when opening Spoolman picker from Extruder tab (#311)
- Bed screw calibration results never showing (#309)
- AFC/AMS reactive lane highlighting and current_slot stability (#336)
- A4T toolhead render size too small

## [0.97.0] - 2026-03-06

Multi-printer support (beta) and selectable toolhead renderers. Configure multiple printers in a single HelixScreen instance and switch between them from the navbar or settings. The A4T toolhead style is now available alongside the default and Stealthburner renderers.

### Added
- Multi-printer support: configure, switch, and manage multiple Klipper printers from one device (beta feature flag)
- Printer switcher badge in navbar with connection status indicator
- Printer management overlay for adding, switching, and deleting printers
- Toolhead style selector in display settings (Auto, Default, Stealthburner, A4T)
- A4T toolhead renderer with SVG-traced polygon geometry

### Fixed
- Self-update crash from CWD deletion causing SIGABRT on re-exec
- In-app updates not offered on non-Klipper systemd installs
- Config loss on Moonraker-triggered updates for K1/SysV devices
- Cancel button not working in add-printer wizard flow
- Null JSON values in config causing crashes instead of returning defaults
- AFC filament loaded state detection returning false negatives
- Abort dialog ignoring cancel_escalation_enabled setting

## [0.96.9] - 2026-03-06

### Fixed
- Camera snapshot polling use-after-free crash after thread detach
- Binary GPIO backlight truncating brightness to OFF on wake (#326)
- LED effects stopping on all strips when toggling a single strip (#329)
- LED macro entries persisted as empty on Add (#330)
- Input shaper results modal not dismissed before Klipper restart (#328)
- Modal button focus stealing in host power dialog (#333)
- About screen logo not registered and excess padding (#333)
- Info QR modal OK button not wired (#332)
- Buffer status modal cancel button not wired (#323)
- Power panel using inconsistent overlay layout
- Discovery failure toasts shown during startup on slow devices
- Deploy targets failing with "Text file busy" when process still exiting

### Changed
- Renamed "Filament Sensors" to "Sensors" in settings panel

## [0.96.8] - 2026-03-05

### Added
- AFC buffer health indicators with safe-state green checkmark visualization
- QR code info modals for Discord and Documentation links in settings

### Fixed
- Home screen edit mode triggered while dragging fan arc or slider knobs
- Clog detection widget and single-page carousel not passing clicks through to parent
- Spoolman spool item not accessible from extruder menu (#289)
- Filament preset layout broken with 3-up display; cooldown not stopping bed and nozzle heating together
- G-code viewer showing blank 3D preview when streaming mode is active
- Memory: reduce heap churn, fix string leak, prevent unbounded growth
- Telemetry dashboard data mapping for hardware, panel usage, memory, and themes
- Build compatibility with GCC 10 (std::from_chars float overloads)
- Crash handler preprocessor branches for macOS ARM64 compilation

### Changed
- Default dark theme palette darkened for improved contrast

## [0.96.7] - 2026-03-05

Stability-focused release addressing multiple crash vectors, WiFi driver concurrency, async callback safety, and widget deletion during events. Also adds CJK font support, debug bundle improvements, and numerous UI fixes.

### Added
- CJK glyph support in text fonts with automatic font regeneration on translation changes
- User note field on debug bundle submission for additional crash context

### Fixed
- Multiple crash vectors: async callback use-after-free with alive guards, widget deletion deferred to prevent event corruption, WLED thread safety, and AMS child deletion re-entrancy
- WiFi SIGSEGV from concurrent wpa_ctrl access without mutex protection
- Dual-axis input shaper calibration discarding stale callbacks (#310)
- Screws tilt calibration callbacks not routed to UI thread (#309)
- Tips rotation timer not stopped on panel deactivation (#296)
- Tips label opacity not reset on reactivation after interrupted fade
- Phantom fan widget shown when alternate part fan is configured
- Debug bundle missing syslog and embedded crash file paths
- Bed mesh 3D graph not refreshing on profile switch; row clicks falling through to panel beneath (#307)
- AMS filament edit modal opening on picker view instead of form view
- Spoolman spool list race condition causing incomplete filament data (#311)
- MPC calibration control type incorrectly detected on non-Kalico printers (#306)
- Async observer use-after-free crash on ARM32 with enhanced diagnostics (#317)
- Display waking with blank screen due to FBIOBLANK race on software overlay (#303)
- Slider knob clipped at edges in fan and PID tuning menus (#306)
- Dim/sleep constraint applied on devices without dimming support (#313)
- Installer crash on missing dependencies during self-update (#314)
- Display orientation probe not applying LVGL rotation (#315)
- Grid widget selection before layout update causing misalignment (#308)
- Toast dismissal crash from synchronous deletion during event processing (#316)
- Console log output suppressed by isatty check
- Crash diagnostics: recover actual crash location from CPU registers on shallow ARM32 backtraces

### Changed
- About screen uses native LVGL scroll with prerendered logo replacing custom marquee animation (#312)

## [0.96.5] - 2026-03-05

### Added
- Compile-time ENABLE_MOCKS flag to exclude mock backends from production builds

### Fixed
- Print status card requiring multiple clicks to navigate (grace period timer started on first click instead of app launch)
- Camera stream dying during fullscreen transitions and panel navigation
- Crash serialization cascading failures when writing crash entries
- LED color parsing errors with signed char values and toggle icon state drift
- Click events not reaching parent widget on print status card and other dashboard cards

### Changed
- MJPEG camera decoding uses libturbojpeg for SIMD-accelerated performance
- 3D renderer releases CPU geometry and GPU vertex buffers when no longer needed
- Consistent pressed-state touch feedback across all clickable dashboard widgets
- AMS buffer status extracted into declarative XML modal with corrected meter drawing
- Translations updated: 17 new strings across 9 languages

## [0.96.4] - 2026-03-04

This release adds AMS dryer status, proportional buffer feedback for Happy Hare, a filament health carousel, LED startup brightness control, and proactive memory monitoring. Camera stability received major crash fixes, and G-code viewer memory usage was cut in half.

### Added
- AMS dryer info bar showing humidity and dryer status on filament panels
- AMS proportional buffer meter with color-coded bias visualization and fault context modals
- AMS filament health carousel replacing the clog detection widget
- Visual press feedback on AMS filament slot taps
- LED startup brightness slider in LED settings
- Last printed file thumbnail on idle home screen print status card
- Proactive memory monitoring with device-tier thresholds and telemetry dashboard
- PFA Stealthfork printer profile and updated Micron heuristics
- Single-instance file lock to prevent dual-process DRM conflicts

### Fixed
- Camera use-after-free crashes from detached stream threads and stalled ScopedFreeze rebuilds
- Camera stream not recovering after detach/reattach or widget drag rebuild
- 3D bed mesh view not rendering on initial load
- Crash diagnostics: ARM32 static-PIE load_base capture and AD5M filtered memory maps (#296)
- Object deletion during LVGL input event processing causing child list corruption
- Phantom print starts on startup before printer state is fully synced
- Config backup loss during Moonraker update wipes
- Config backup failing when /var/log is unwritable
- LED toggle state inverted; LED state not queried on startup
- Fan widgets not shown or not disabled when printer disconnected
- AMS toolchanger unload using wrong slot index; AFC current_slot not preserved
- Thumbnail picker selecting smallest instead of best-fit resolution

### Changed
- Camera stream and extended sleep suspended during display off to reduce idle power
- Screensaver CPU usage reduced across all three modes (pre-decoded sprites, optimized toasters)
- G-code ToolpathSegment halved to 40 bytes; loading deferred 5s after print start; streaming forced on ≤4GB RAM
- Adaptive main loop sleep replaces fixed 5ms delay
- Settings reorganized: G-code preview consolidated, Time Format moved to Appearance

## [0.96.3] - 2026-03-03

This release adds a floating emergency stop button, temperature sensor carousel with multi-sensor picker, 3D box effects on AMS trays with Happy Hare Type B hub detection, and improved DRM display rotation robustness. QR label printing for Brother QL printers is available as a beta feature.

### Added
- Floating emergency stop button with confirmation dialog (enabled by default)
- Temperature sensor carousel mode with multi-sensor picker
- AMS tray 3D box effect using oblique projection draw callbacks
- Happy Hare Type B MMU hub topology detection with adjusted tray transparency
- QR label printing for Brother QL printers (beta-gated)
- DRM display rotation auto-fallback to fbdev and legacy atomic commit fallback (#288)

### Fixed
- GCode viewer dangling pointer in deferred ghost label deletion (#290)
- Spoolman spool canvas transparency using ARGB8888 and explicit API query limit (#289)
- Software keyboard appearing during scroll gestures
- Print status panel dedup guards not clearing on navigation, thumbnail binding, and card layout sizing
- Update download modal callbacks registered after modal shown from notification
- Installer polkit PKLA generated inline, eliminating deploy failures
- Macro `variable_*` extraction removed entirely from parameter handling
- AMS unit card radius now fixed for consistent 3D tray box alignment

## [0.96.2] - 2026-03-03

### Added
- QGL and Z-Tilt leveling buttons on the motion overlay

### Fixed
- Dropdown selection always picking the last item on Goodix GT9xx touchscreens (Protocol A release coordinate regression)
- Installer polkit directory checks now use sudo when required (reported by @BO_Andy)

## [0.96.1] - 2026-03-02

This release adds a GCode console with full command history, Mainsail-style per-field macro parameter editing with Klipper variable support, spool temperature presets on filament and temperature panels, and a preset-aware setup wizard. Grid edit mode stability received major crash fixes.

### Added
- GCode console panel with monospace font, timestamped command history, and home screen widget
- Per-field macro parameter inputs with placeholders, `variable_*` field support, and scrollable modal
- Macro picker Done button with responsive height and icon/color customization
- Spool preset button on nozzle, bed, and filament panels using active material temperatures
- ActiveMaterial provider with priority-based resolution across filament sources
- Auto-pass PURGE_TEMP from active spool material to purge macros
- Preset-aware setup wizard that skips hardware steps for preconfigured builds
- Dedicated telemetry opt-in step in wizard for preset mode
- Hex color support in action prompt buttons (#278)
- Toast notification when taking in-app screenshots
- Translated panel widget names and descriptions in catalog overlay

### Fixed
- Grid edit mode SIGSEGV crashes from double-free during overlay deletion and external rebuilds
- Touch axis swap auto-detection removed — broke Nebula Pad and Sonic Pad calibration
- LVGL arc draw crash from negative inner radius and zero-sweep edge cases
- About panel callback names mismatched with XML, leaving update buttons dead
- Print source selector no longer shown when no USB drive is present
- Input field contrast on elevated surfaces and filename truncation (#283)

### Changed
- Console graduated from beta with full documentation
- Preset packages use convention-based lookup by platform target name
- Macro parameter detection uses `'NAME' in params` pattern in addition to dot-access

## [0.96.0] - 2026-03-02

This release adds camera streaming and fullscreen view, TMC stepper driver temperature monitoring, and smarter macro parameter handling. Widget stability and Android support received significant improvements, along with fixes for several crash reports.

### Added
- Camera fullscreen view with MJPEG streaming, separate connect/active timeouts, and data arrival tracking
- TMC stepper driver temperature support with corrected display names
- Macro parameter pre-parsing during discovery for faster parameter dialogs

### Fixed
- Home screen startup jumpiness from uncached grid rows and deferred card backgrounds
- Widget config loss from duplicate PanelWidgetConfig instances during rebuilds
- Edit mode long-press triggering during scroll/swipe on home screen
- Newly added widgets not auto-selected in grid edit catalog
- AD5M crash loops and stream exceptions with unwind tables enabled (#280, #281)
- Binary backlights that only support on/off now handled correctly (#276)
- About panel version subjects not bound and marquee scroll performance (#275)
- NaN/Inf float values in JSON responses no longer cause crashes (#277)
- Installer self-heals un-substituted polkit pkla template (thanks @BO_Andy)
- Android startup crash, tofu glyphs on ARM64, and shutdown crash
- Protocol A touch release for Goodix GT9xx capacitive panels
- Temperature graph now supports up to 16 series for TMC stepper temps
- Invalid theme token references (text_secondary, radius_md) replaced
- Macro config key lookup with improved error logging

### Changed
- Fan arc refactored into shared fan_arc_core component with format_fan_speed helper
- Widget rebuilds skip redundant work with wider coalesce window for gate observers
- Temperature carousel dot spacing now matches fan carousel

## [0.95.3] - 2026-03-02

### Added
- Live webcam panel widget with snapshot polling for camera monitoring
- Print statistics widget with 4 responsive size modes for the home dashboard
- Macro parameter dialog — macros with parameters now prompt for values before execution
- Dangerous macro confirmation — SAVE_CONFIG, FIRMWARE_RESTART, etc. require confirmation
- Locale-aware date/time formatting with translation support across all languages
- Clog detection configuration modal for tuning sensitivity and thresholds

### Fixed
- Macro parameter detection now queries Klipper configfile instead of printer objects, which always returned null
- LED toggle state now syncs with actual hardware on bind and before toggle
- Preparing Print overlay no longer gets stuck after print start
- Clock widget timer restarts correctly after home screen rebuild
- Filament preset button temperatures now pull from filament database
- Assert handler re-entrancy and InputShaper threading violations causing crashes
- Moonraker update detection broken by release name prefix (#270)
- Wizard kinematics filter now supports Kalico and hybrid variants
- Input fields inside dialogs now have proper contrasting background color
- Camera widget initial overlay layout — full-width text and centered spinner

### Changed
- Text input widgets auto-register with software keyboard — no manual wiring needed
- Temperature stack padding matches fan stack; enlarged carousel icons
- Fan arc widget sizing reduced with proper padding in carousel

## [0.95.2] - 2026-03-01

### Added
- Clog detection arc meter on loaded AMS card for real-time filament flow monitoring
- Starfield and 3D Pipes screensavers with selection dropdown
- Buffer detail info modal accessible by tapping filament path coil
- Filament buffer visualization on AMS path canvas
- Full-screen color picker layout with tab switching for tiny screens

### Fixed
- Re-entrancy guard for fan arc resize preventing concurrent animation crashes
- Color picker HSV sizing and bottom-pinned buttons for responsive layouts
- G-code viewer reload after destroy-on-close cycle on print status panel
- NEON alignment, empty vector, and stale widget crashes identified from telemetry
- Telemetry active device count now uses separate query for correct COUNT(DISTINCT)

### Changed
- Build system: static-link libhv OpenSSL for K1/MIPS, avoid double-wrapping compilers with ccache

## [0.95.1] - 2026-03-01

### Added
- Widget catalog now shows icons and descriptions for each widget type
- LED Controls widget for quick access to LED settings from the dashboard
- Configure button in edit mode for widgets that support settings (macros, temperature stacks)
- ZMOD firmware detection for AD5M/AD5M Pro installer (#251)

### Fixed
- Use-after-free crash during WebSocket reconnection (#255)
- SIGSEGV from wrong-pointer lv_anim_delete on bar animations (#259)
- Font linked-list validation to prevent SIGSEGV on corrupted font data (#244)
- Fan button states not updating when animations are disabled (#258)
- AD5X platform key missing from update checker (#253)
- SIGSEGV when registering XML event callbacks after XML parsing
- SIGSEGV from stale input device reference after grid rebuild
- Dangling observer pointer in AMS cross-singleton cleanup during reconnection
- Backlight stays on during sleep mode for AD5X/CC1 displays (#235)
- Job queue fetch race condition at startup before WebSocket is connected
- Spoolman spool editing: vendor, material, color, and filament_id now sync correctly
- Happy Hare gate_spool_id parsing for Spoolman fill gauge display
- AMS slot editing from overview panel context menu
- Internal macros (underscore-prefixed) hidden from macro picker
- Fan panel: temperature_fan classified as auto-controlled, knob hidden; primary color for arc indicator; dial clipping fixed
- Navigation: overlays restored via go_back() now receive on_activate()
- OpenGL ES rendering stride alignment for blit operations
- E-stop button repositioned to top center of print status widget
- Humidity sensor log spam reduced to display-precision changes only
- Pressed feedback added to favorite macro widgets
- Widget auto-shrinks to fit when default size exceeds available space
- Toast notification when no room for widget placement

### Changed
- Translations updated: 47 new strings across all 8 languages
- Home Widgets settings overlay removed (replaced by widget catalog)
- Removed static linking on AD5X platform

## [0.95.0] - 2026-03-01

The biggest release yet — HelixScreen's home panel is now a fully customizable grid dashboard. Drag widgets to reposition, resize from any edge, add new widgets from a catalog, and remove what you don't need. The layout persists per-breakpoint and survives restarts. Also includes significant memory optimizations, a new screensaver, material temperature presets, and a standalone About overlay.

### Added
- Customizable grid dashboard replaces the fixed home panel layout — drag to reposition, resize from any edge, and persist per-breakpoint
- Widget catalog overlay for browsing and adding widgets to the dashboard
- Digital Clock widget with responsive font scaling
- Job Queue widget with queue management actions (start, pause, cancel)
- Shutdown/Reboot widget with modal confirmation
- Material temperature overrides with per-material nozzle and bed customization
- Flying Toasters screensaver (After Dark, 1989)
- Standalone About overlay extracted from settings panel
- Default widget layouts defined in runtime JSON with per-breakpoint anchors

### Fixed
- Job queue filename contrast and empty state visibility
- Clock widget centering and font scaling across breakpoints
- Printer image snapshot transparency (ARGB8888 instead of RGB565)
- Tips widget accent bar sizing and content centering
- Widget click targets improved to prevent use-after-free on child elements

### Changed
- Memory optimizations: ~2.5MB savings from overlay destroy-on-close, observer suspension, and RGB565 color depth option
- Gate observer rebuilds coalesced (300ms window) reducing startup from 4x to 2x
- Over 1000 lines of dead HomePanel code removed after widget extraction

## [0.13.13] - 2026-02-28

### Fixed
- AFC filament system now discovers units with generic `AFC_` prefix, improving compatibility across AFC configurations
- NULL pointer checks added to helix-xml parsing and rotation probe to prevent OOM crashes
- Orientation detection logic corrected for display rotation

## [0.13.12] - 2026-02-28

This release adds MPC calibration support for Kalico/Danger Klipper firmware, a unified temperature graph overlay, and significant performance and stability improvements.

### Added
- MPC (Model Predictive Control) calibration UI with Kalico detection, config migration flow, and mock support
- Unified temperature graph overlay replacing three separate per-sensor overlays with side-by-side layout and per-mode controls
- Clickable mini temperature graph on filament panel opens full overlay
- Friendly status screen displayed before restart during updates
- Klipper config editor: `ConfigEdit` and `safe_multi_edit` for safe multi-key config changes

### Fixed
- Fan speeds stuck at 0% due to race condition in fan state updates
- G-code viewer continues rendering when print status panel is hidden, wasting CPU
- NEON alignment and NULL guard crashes in LVGL software rendering path
- Blend area clipped to buffer bounds to prevent NEON SEGV (#242)
- Black screen on SysV self-update due to unnecessary stop/start cycle
- User config files now preserved across installer updates
- Redundant 'Temperature' suffix stripped from temp graph chip labels

### Changed
- Subject notifications skip redundant updates when values unchanged, reducing UI redraws
- AMS backend eliminates redundant subject fires and cascading redraws
- Theme lookups cached and canvas dirty guards added for improved render performance

## [0.13.11] - 2026-02-28

### Added
- ViViD filament system support in AFC backend with unit discovery and key mapping
- Dedicated logos for Happy Hare and AFC filament systems
- Improved 5GHz Wi-Fi band detection across all backends

### Fixed
- DRM display rotation reworked to eliminate flickering on inverted panels
- EMU filament system compatibility: color formats, gate counts, dryer state, filament names, and sensor readings
- Printer setup wizard no longer loses printer type list when navigating between steps (#231)
- Backlight now fully turns off during sleep mode on sysfs-based displays
- Bed mesh rendering blit failure and axis label offset corrected
- AD5X platform detection improved with secondary /ZMOD indicator (#225)
- Input shaper calibrate-all mode shows progress text instead of premature "Complete" (#225)
- Spoolman spool picker no longer shows empty vendor list on reopen (#225)
- DRM NEON blend buffer overrun prevented on reshape failure (#229)
- Crash loop detection and defensive widget creation for improved stability
- Missing translation tags on header bar action buttons
- Spoolman uses PATCH to update existing filaments correctly

### Known Issues
- Inverted/upside-down panels in DRM mode may exhibit minor flickering during heavy rendering

## [0.13.10] - 2026-02-28

### Added
- Automatic display orientation detection and software rotation for DRM displays

## [0.13.9] - 2026-02-27

### Added
- Asynchronous bed mesh rendering with double-buffered worker thread and adaptive quality degradation
- Off-screen pixel buffer rasterizer for bed mesh visualization
- Animated connector tube through LINEAR selector box in filament path view

### Fixed
- Backdrop blur re-enabled with NULL guards across all color formats and NEON paths
- Bed mesh panel forces initial paint on re-entry
- G-code viewer forces refresh after first 3D GPU render
- Home All sends bare G28 instead of G28 X Y Z
- Global extruder subjects sync correctly on tool selection in temperature panel

## [0.13.8] - 2026-02-27

### Added
- Memory-aware geometry budget system for 3D G-code viewer — automatically selects detail tier based on available memory with graceful 2D fallback
- GPU-accelerated backdrop blur for modals
- Shutdown and reboot widget with modal confirmation dialog
- Speed and flow rate increment buttons replace sliders for precise control (#219)
- Lemontron, Sovol SV08 Max, and Sovol Zero added to printer database
- FlashForge Adventurer 5X support with independent platform toolchain (#203)

### Fixed
- UI freeze during 3D geometry VBO upload eliminated
- AMS panel and spool picker back button click targets enlarged for easier navigation
- Goodix capacitive touch on Creality K1 Max and standalone builds
- DRM plane rotation fallback for VC4 displays (90/270 unsupported)
- Spoolman request flooding prevented with debounce and circuit breaker
- Spoolman filament creation sends required density and diameter fields
- Klippy readiness checked before querying printer objects during discovery
- Android display corruption from conditional style reset reverted
- Signed coordinate crash in LVGL draw path
- CoalescedTimer repeat count bug

## [0.13.7] - 2026-02-27

### Added
- Dropdown options now support translation via `options_tag` attribute
- Broad internationalization pass across C++ UI code with `lv_tr()` calls
- Touch calibration can be forced on startup with `HELIX_TOUCH_CALIBRATE` environment variable
- Thumbnail Only option for G-code render mode in display settings

### Fixed
- AMS tray height reduced for better proportions in slot grid
- G-code metadata parser rejects percentage values in extrusion width fields
- Cancel button now appears immediately when starting a print
- AFC multi-unit bugs: nozzle navigation, lane sorting, and current tool derivation

## [0.13.6] - 2026-02-26

### Added
- Fan speed overlay opens directly when tapping the fan icon in carousel mode
- Dual-output Pi builds: single compilation produces both DRM and fbdev binaries

### Fixed
- LED light state now syncs correctly from hardware on status updates
- Empty AMS slots are clickable with placeholder circle and context menu
- LED widget initial state and reactive bindings fixed
- Print details delete button is now icon-only, giving more room for the print button
- 2D fallback disabled on print details panel; thumbnails used instead
- AMS slot positioning fixes for hidden spool containers and LINEAR selector box sizing
- Keyboard overlay crash when cleanup nulls alternatives mid-use (#207)
- Use-after-free in LED and temperature widget button user data
- Tool badge now shown on empty unassigned AMS gates
- Printer database JSON parsing hardened against type mismatches
- ELF architecture validation uses platform key instead of uname
- Updated Ender 5 Max printer image
- Installer preserves user files in `printer_database.d` during upgrades
- AMS flow animation no longer runs infinitely, fixing 50%+ CPU usage on AMS panel

### Changed
- XML event callbacks registered at startup instead of widget attach time
- Panel switching and widget creation optimized for ARM
- Spoolman vendor list fetched from dedicated endpoint instead of downloading all spools
- AMS gate observer rebuilds coalesced to reduce startup churn

## [0.13.5] - 2026-02-26

### Added
- Touch jitter filter for noisy controllers like Goodix GT9xx with automatic breakout detection
- Auto-detect swapped touch axes during calibration, especially for Creality SonicPad and similar devices with misreported axis orientation
- Power Devices entry in System settings
- AMS filament system header bar now shows system logo and name with declarative bindings
- AMS LINEAR output path with animated slide beneath active slot
- Update telemetry tracking: success/failure recording across update lifecycle with analysis tools
- Seven new telemetry event types with thread-safe recording
- `--debug-touches` flag for touch input diagnostics

### Fixed
- Header back button touch target expanded to full title width for easier navigation
- AMS bypass spool centered on filament line with label moved beneath
- AMS spools centered inside tray with support for variable AFC lane count
- Happy Hare AMS now uses LINEAR topology with SELECTOR butted against prep sensors
- Float-to-int conversion guards against NaN/Inf values (#206)
- AD5X platform now correctly maps to K1 MIPS binary (#203)
- Async callback use-after-free in Spoolman spool selection
- Null guards for keyboard events and NEON blend path (#207, #208)
- Installer no longer shows misleading 'corrupt download' message on slow CDN connections
- Jitter filter correctly disabled after breakout for smooth scrolling

### Changed
- HomePanel refactored into self-contained widgets
- CI build split into separate compile and test jobs

## [0.13.4] - 2026-02-25

### Added
- BMP and GIF format support for custom printer images
- Invalid custom printer images shown as disabled with lazy import and instant gallery refresh
- In-process fbdev display fallback when DRM initialization fails (no restart needed)
- Top-level exception handler prevents unhandled crashes from silently terminating the application

### Fixed
- SonicPad Goodix (gt9xxnew_ts) touchscreen now triggers calibration wizard when kernel reports zero ABS ranges
- DRM backend no longer falls back to `/dev/dri/card0` when no suitable DRM device exists
- Fan carousel arc thumb disabled on auto-controlled fans that don't accept manual speed changes
- Observer cleanup ordering hardened to prevent cascading use-after-free during shutdown
- Thread safety and crash telemetry improvements across observer guards and lifecycle management

### Changed
- Macro search resolves only active include-chain config files, improving performance on large configurations
- Touch calibration and wizard skip decisions promoted to info-level logging for easier diagnostics

## [0.13.3] - 2026-02-25

### Added
- AFC tool change progress display on print status panel with current/total tool change counter
- AFC mock tool change progress in test mode for development

### Fixed
- MT-only touchscreens (e.g., Goodix gt9xxnew_ts on Nebula Pad) now detected correctly — previously invisible due to missing ABS_MT_POSITION_X/Y bit checks
- ABS range queries fall back to multitouch axes when legacy ABS_X/ABS_Y are absent, enabling rotation mismatch detection on MT-only devices
- Installer now preserves user data directories (custom_images, printer_database.d) during upgrades
- Watchdog no longer launches external splash screen in DRM mode, preventing a crash loop
- ObserverGuard cleanup lambda prevents use-after-free when releasing observers
- Print status filament row decluttered by removing redundant "Filament" and "Active" labels
- AFC mock toolchange progress default now set in constructor for consistent test behavior

### Changed
- README LVGL badge updated to 9.5, added helixscreen.org link

## [0.13.2] - 2026-02-25

### Added
- FlashForge AD5X platform support (#203)
- Touch calibration CLI flag (`--calibrate-touch`) and `input.force_calibration` config option
- Touch calibration standalone user guide
- Improved touch calibration UX with tap-to-begin, progress counting, and flash feedback

### Fixed
- Touch input on fbdev devices no longer applies a redundant rotation transform (#186)
- Shutdown sequence hardened against use-after-free crashes
- DRM device configuration now validates before use and falls back to auto-detection on failure
- Diagnostic logging added for DRM initialization failures and startup platform info
- Alive guards and lifecycle safety added to 5 crash-prone components
- WebSocket disconnected before clearing app globals to prevent spurious shutdown errors
- TemperatureSensorManager shutdown crash prevented with alive guard
- Happy Hare MMU slot data now received via mmu object subscription (#214)
- Splash screen skipped on DRM-only systems to prevent master contention
- Telemetry queue file writes now atomic to prevent empty file on interrupted save
- DRM rotation patch includes header declaration (fixes Pi cross-compilation)
- Launcher e2e tests mock system commands to avoid hitting dev machine

## [0.13.1] - 2026-02-25

### Added
- Robust touch calibration with multi-sample input filtering, ADC saturation rejection, and post-compute validation
- Smart calibration auto-revert with 10-second timeout and broken-matrix detection
- DRM plane rotation support for `rotate` config on Raspberry Pi

### Fixed
- Use-after-free crashes in AMS modal destructor and sidebar (#199, #201)
- Static-linked OpenSSL for Pi fbdev variants (fixes missing libssl.so.1.1 on some systems)
- Creality SonicPad/Nebula display backlight no longer killed by display-sleep service
- OTA update downloads no longer fail for releases larger than 50 MB (limit raised to 150 MB)
- Touch calibration verify handler simplified with dead wizard callbacks removed

## [0.13.0] - 2026-02-24

The rendering engine gets a major upgrade — the 3D G-code viewer is ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading, and Pi builds gain GPU-accelerated DRM+EGL rendering with automatic framebuffer fallback. A first-boot rotation probe auto-detects display orientation, and the UI gains carousel modes for temperature and fan widgets, frosted-glass modal backdrops, and a new shared progress bar component with gradient indicators.

### Added
- GPU-accelerated DRM+EGL rendering for Raspberry Pi with automatic fbdev fallback
- 3D G-code renderer ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading and camera-following light
- First-boot display rotation probe with auto-rotating touch coordinates
- Automatic DRM/fbdev binary selection and dual-binary Pi releases
- Carousel display mode for temperature and fan stack widgets with long-press toggle
- Shared progress bar component with dynamic gradient indicator
- Frosted-glass backdrop blur effect on modals
- G-code render mode setting visible when 3D rendering is available
- Chamber temperature overlay on controls panel
- Print lifecycle state extraction for cleaner print status management
- 3D G-code viewer in print file detail panel with async loading and AMS color support
- Pinch-to-zoom gesture support for 3D G-code viewer
- Icons on Delete and Print buttons in print details card
- SSL enabled for native desktop builds
- Translation updates with 7 obsolete keys removed
- External spool widget for printers without AMS
- Loading spinner overlay and 3D rotate hint icon on print file detail panel
- AFC tool changer support with proper SELECT_TOOL/UNSELECT_TOOL commands and extruder dropdown

### Fixed
- Use-after-free crashes in G-code viewer, power panel, mDNS callbacks, thumbnail loading, and AMS widget cleanup (#182, #192, #193)
- Scroll jitter in virtual list views caused by layout-invalidating calls
- Safe name-based widget lookup prevents miscast crashes in event handlers (#194, #195)
- GLES 3D build correctly disabled on macOS (no EGL headers)
- Stale thumbnail and progress data no longer persists when a new print starts
- AMS spools no longer show as full when Spoolman initial_weight is null
- AMS loaded filament card swatch color now updates reactively
- Overlays now close when clicking the navbar button for the active panel
- Progress bar draw no longer triggers lv_inv_area assertion
- Overflow row click passthrough on controls panel
- Temperature chart validates widget before updating series data
- Renamed Voron Micron to PFA Micron in printer database
- AMS edit modal remaining weight defaults and display
- Header bar back button click area expanded for better touch targeting
- 3D viewer camera framing and gesture responsiveness
- Pinch-to-zoom rendering during gesture
- DRM flush timeout from glReadPixels on cached frames
- Print buttons stay at bottom when preprint options are hidden
- Empty preprint options card hidden when no options available
- Z-adjust button icons corrected for bed-moving printers
- Z-adjust button order fixed so down arrow is on bottom
- 3D viewer crash when loading UI triggers on hidden widgets
- Timelapse callback registration

### Changed
- Tertiary theme color changed from orange to blue-violet
- Inline progress bars replaced with shared progress_bar component
- AMS panels refined with more compact loaded filament card and tighter spacing
- Edit icons changed from secondary to tertiary color
- Deprecated AMS mock environment variables removed

## [0.12.1] - 2026-02-23

### Added
- Daily active devices and cumulative growth charts in analytics dashboard
- Zstd compression for debug symbol uploads (~60% size reduction)

### Fixed
- XML `inner_align="contain"` and `inner_align="cover"` now work correctly — images were rendering at native size instead of scaling to fit their containers
- Telemetry device counting uses unique devices instead of sessions for cumulative growth
- Discord invite links updated across all documentation
- Worktree setup script resolves main tree path correctly when run from inside a worktree
- Android CMake build includes helix-xml library
- GitHub release titles no longer show duplicate version numbers

## [0.12.0] - 2026-02-23

A major infrastructure release — LVGL is upgraded to v9.5.0, and the XML layout engine has been extracted into its own library (`helix-xml`) for independent development. The Android port lands with initial build system and CI pipeline support. Developer experience improves with XML hot reload for live UI editing.

### Added
- Android port: build system, APK packaging, asset extraction, CI builds, and release pipeline
- XML hot reload for live UI editing during development (`HELIX_HOT_RELOAD=1`)
- Klipper ERROR state recovery dialog with firmware restart option
- AMS unit names are now pretty-printed for readability
- Widget-safe async callback utilities for thread-safe UI updates

### Fixed
- Use-after-free in deferred observer callbacks (#174)
- Modal use-after-free crash during navigation
- USB drive callback crash from background thread (now marshaled to main thread)
- AMS crash on quit from unjoinable scenario/dryer threads
- Spoolman weight polling skipped when filament backend already tracks weight
- Spoolman active spool management for Happy Hare and AFC backends
- AFC Unload button re-enables after lane scan resets filament state
- AFC mixed topology: correct tool count, hub sensor detection, and status display
- AMS current slot label accuracy for multi-unit and tool changer setups
- AMS overview right column capped at 200px to prevent layout overflow
- Keyboard restores screen position when dismissed via backdrop click
- Firmware restart widget shown for all non-READY Klippy states
- Responsive tokens applied to picker layouts, fan name widths, and panel widget spacing
- Font clipping and padding on panel widgets
- Gcode viewer segfault
- Update check errors now visible in the UI
- Installer uses printf for ANSI escape compatibility

### Changed
- LVGL upgraded from 9.4-pre to v9.5.0
- XML engine extracted to `lib/helix-xml/`, decoupled from LVGL internals
- MoonrakerAPI decomposed into domain-specific modules (Rest, Job, Motion, File, FileTransfer, Timelapse, Advanced)
- Z-offset utilities extracted into shared module
- Wi-Fi status polling refactored to async with responsive connect/disconnect updates

## [0.11.1] - 2026-02-22

### Added
- Panel widgets dim when Moonraker is disconnected or Klippy is not ready
- AMS slot bars resize responsively based on home panel row density
- Bypass spool widget and filament path topology support for AMS systems
- Happy Hare v4 parsing with full v3 backwards compatibility

### Fixed
- AMS "Currently Loaded" display now shows the correct filament in multi-backend setups (e.g., AMS_2 load no longer snaps to AMS_1 state)
- Use-after-free crash on AMS overview back-navigation
- Use-after-free crash when temperature graph chart widget is destroyed
- Bypass path and toggle hidden for tool changers (not applicable)
- Watchdog uses fork-based reboot fallback for crash dialog reliability
- Auto-restart after update install instead of showing unnecessary restart dialog

### Changed
- Filament page: purge button separated from extrude, operations layout reorganized

## [0.11.0] - 2026-02-22

A feature-rich release — fan speeds are now a first-class widget with spinning animations and density-aware labels, chamber temperature gets its own full control panel, and the Printer Manager overlay is available to everyone (no longer gated behind beta). Under the hood, dynamic observer lifetime safety prevents use-after-free crashes, and the MoonrakerClient has been decomposed for maintainability.

### Added
- Chamber temperature panel with graph, presets, and sensor-only monitoring mode — tap the chamber row in the Temperatures widget to open it
- Fan stack widget enabled by default with density-aware compact labels (P/H/C when space is tight)
- Spinning fan icon animations on dials, status cards, and home panel widgets
- Crash history tracking in debug bundles — past crash submissions with GitHub issue references
- SubjectLifetime tokens for safe observation of dynamic per-fan, per-sensor, and per-extruder subjects
- Micro layout support (480x272) with compact controls, theme editor, and display overlays
- Ender-3 V3 KE printer image and hostname detection
- MoonrakerClient decomposition: RequestTracker and DiscoverySequence extracted as independent modules
- Translations synced and fan stack labels localized across all languages

### Fixed
- Printer Manager overlay no longer gated behind beta features — accessible to all users
- Setup wizard auto-fills default port 7125 when the port field is left empty on Test Connection
- Printer detection: hostname_exclude heuristic prevents Ender-3 V3/V3 KE model ambiguity
- Heap corruption during change-host reconnection (double-free in observer cleanup)
- Stack overflow on Pi from `lv_obj_is_valid()` in hot paths (HeatingIconAnimator crash)
- HeatingIconAnimator theme observer uses ObserverGuard to prevent heap corruption
- Config preservation during in-app upgrades (three-layer merge prevents config loss)
- Factory reset now restarts the app automatically
- Installer download progress bar and error reporting
- Release download logic with timeout and speed limits
- Thermistor picker position and temp stack click targets
- Printer image snapshot alignment when clearing
- Home panel widget spacing, fan labels, and click targets
- Print start toast and phase tracking properly gated behind beta features
- Various compiler warnings and dead code removed

### Changed
- MoonrakerClient internals decomposed: discovery callbacks stored as members, stale connection guard added
- Navigation bar widened at medium and large breakpoints
- Printer image snapshotted to eliminate per-frame scaling (performance improvement)
- Install Update row moved under Check for Updates in About section
- Orphan AboutOverlay removed (was unreachable dead code)

## [0.10.14] - 2026-02-22

### Fixed
- AFC unload button and context menu now work on AFC firmware versions that don't expose a top-level `filament_loaded` field (e.g., Box Turtle with `lane_data_enabled=false`)
- AFC `current_load` field parsed as fallback when `current_lane` is absent, fixing loaded lane detection on newer AFC versions
- Crash-hardened 15 vectors found during 48-hour audit

## [0.10.13] - 2026-02-22

Crash hardening and new features — favorite macro widgets let you pin and run macros from the home panel, filament controls get dedicated Extrude/Retract buttons, and Wi-Fi status updates are now async and responsive. Under the hood, the MoonrakerAPI monolith has been split into domain-specific modules.

### Added
- Favorite macro panel widgets with macro picker and automatic parameter detection
- Extrude and Retract buttons replace Purge on filament page, with configurable speed
- Wi-Fi status updates respond immediately to connect/disconnect events

### Fixed
- Multiple crash fixes: glyph null guard, use-after-free in async AMS/telemetry callbacks, SIGSEGV in widget cleanup and render thread race
- AFC Unload button now re-enables after lane scan detects filament state change
- Observer generation counters prevent stale callbacks after controls repopulate
- Unknown CLI arguments warn instead of crash-looping
- Z-offset tune overlay save bug
- Macro parameter modal no longer stomps across widget slots
- Widget picker and overlay cleanup uses safe deletion
- USB drive callbacks marshaled to main thread to prevent crashes
- Panel widget padding and font clipping on small screens

### Changed
- MoonrakerAPI split into 8 domain-specific modules (Job, Motion, File, FileTransfer, Advanced, Rest, Timelapse, History)
- Wi-Fi backend uses async status polling instead of blocking queries
- Z-offset utilities extracted into shared module

## [0.10.12] - 2026-02-21

A stability and polish release focused on crash fixes, responsive UI improvements, and internal refactoring. The home panel widget system is now fully decoupled from HomePanel, keyboard input is more reliable, and several threading bugs have been resolved.

### Added
- Carousel widget component with wrap-around, auto-advance timer, indicator dots, and scroll detection
- Thermistor widget for monitoring custom temperature sensors on the home panel
- Responsive breakpoint subject (`ui_breakpoint`) for reactive visibility changes across screen sizes
- `HELIX_LOG_LEVEL` env var and `--log-level` CLI flag for fine-grained log control
- `HELIX_DPI` env var for overriding display DPI
- `HELIX_SKIP_SPLASH` env var to bypass splash screen
- Long-press auto-insert for alternate keyboard characters
- DWARF debug info pipeline for better crash backtrace resolution

### Fixed
- AFC mutex deadlock when error messages were emitted during state parsing
- Startup deadlock and shutdown race condition in mock mode
- Keyboard backspace and character insertion broken when cursor is mid-string
- Keyboard crash when textarea widget is deleted while keyboard is open
- Framebuffer stomping between splash screen and main process on Pi
- Kernel console text bleeding through LVGL UI on framebuffer devices
- Carousel wrap setting ignored in scroll end callback
- Resistive touchscreen detection for NS2009/NS2016 controllers (#135)
- Fan status hidden on tiny screens
- Slider row padding increased to prevent handle clipping
- Dropdown rows now wrap text responsively in settings
- Network item click handling uses correct event target
- Soft keyboard registered for hidden network modal inputs
- Self-update handles NoNewPrivileges; stale `.old` files cleaned on startup
- Installer polkit rules no longer contain untemplated placeholders
- `enP*` interface naming cleaned up (#145)

### Changed
- Home panel widgets fully decoupled from HomePanel — PanelWidget system with self-registration, per-panel config, and independent lifecycle
- AMS backends share extracted `AmsSubscriptionBackend` base class
- MoonrakerAPI split: `MoonrakerHistoryAPI` and `MoonrakerSpoolmanAPI` extracted
- Settings consumers migrated to domain-specific managers (Display, System, Input, Audio, Safety)
- Panels and overlays migrated to batch callback registration
- Temperature formatting consolidated into `ui_temperature_utils`
- Observer factory adopted across all remaining legacy observers
- Shell test suite optimized from ~4 min to ~90s

## [0.10.11] - 2026-02-20

### Added
- Customizable home panel widgets with drag-to-reorder — choose which widgets appear and arrange them to your preference
- External spool support — set filament type and color for the bypass/direct-drive spool, visible in system path canvas and detail views with Spoolman quick-assign
- SlotRegistry unified slot state management across all AMS backends (AFC, Happy Hare, ValgACE, mock)
- 3D tube rendering for filament paths with curved routing, glow effects, and flow particle animations
- Pipe-routed tube drawing with cable-harness nesting for multi-tool overview
- Pulse animation on target slot during filament swap
- Infimech TX printer support (#139)
- Sovol SV06 printer image

### Fixed
- Graceful shutdown on SIGINT/SIGTERM — no more crash on quit
- Filament subjects now update on first Moonraker status even when state matches defaults
- AMS endcap seam eliminated at straight-to-curve tube junctions
- AMS per-unit topology for filament segment in mixed mode
- AMS layout matching works regardless of initialization order
- AMS context menu positioning and badge sizing for 2-digit slots and tools
- AMS global slot numbering and detail view swatch color
- AFC virtual bypass sensor toggle uses correct sensor name
- Stealthburner polygon centering offset corrected
- Wi-Fi SSID retrieved from wifi list instead of invalid device field
- ABS mismatch detection re-enabled with generic HID range exclusion (#135, #137)
- Default print completion alert changed from notification to alert
- Installer drops unnecessary sudo from systemctl checks and adds polkit error handling
- Home panel falls back to generic printer image when file missing
- Home panel uses filament sensor count for hardware-dependent widget gating

### Changed
- AMS overview uses Stealthburner toolhead for Voron printers
- AMS detail view uses badge-style tool labels
- Sensor rows use status_pill component for type badges
- AMS sidebar extracted as shared component across panel types

## [0.10.10] - 2026-02-19

### Added
- Output pin LED backend for brightness-only chamber lights and enclosure LEDs — auto-detects `[output_pin]` devices with PWM slider or on/off toggle
- Individual X and Y homing buttons in controls quick actions
- Clear Spool context menu action for assigned-but-empty AMS slots
- AFC version warning when firmware is below v1.0.35
- Configurable Allwinner backlight ENABLE/DISABLE ioctls for broader SBC compatibility
- Udev and polkit rules for non-root backlight and Wi-Fi access on Pi

### Fixed
- Self-update under systemd NoNewPrivileges — installer now correctly skips privileged operations during in-place updates
- Installer preserves settings.json, helixscreen.env, and config across updates
- Render thread crash from NULL draw buffer race condition
- AFC unit topology now uses name-based matching instead of fragile index ordering
- Toolchanger uses SELECT_TOOL instead of ASSIGN_TOOL to avoid remapping
- Thumbnail paths resolved correctly for files in subdirectories
- File browser poll timer resumes after returning to print selection panel
- Timeouts added to long-running G-code calls to prevent UI hangs
- Systemd service dependency cycle from multi-user.target removed
- Self-restart uses `_exit(0)` instead of `exit(0)` to avoid background thread races
- Mock sensor dots restored for AMS prep sensors

### Changed
- Update check cooldown reduced from 60 minutes to 10 minutes
- SDL display hints cleaned up for better cross-platform performance

## [0.10.9] - 2026-02-19

### Added
- AMS sensor error states with visual indicators for hub, extruder, and lane sensor faults
- Happy Hare pre-gate sensor support for filament detection at each gate
- External tool change step progress detection — swaps and loads initiated from gcode or other UIs now show correct progress steps

### Fixed
- AFC gcode commands corrected to match actual AFC-Klipper-Add-On API: `CHANGE_TOOL`, `TOOL_UNLOAD`, `SET_MAP`, `RESET_FAILURE`, and `SET_BOWDEN_LENGTH` now use correct command names and parameters
- AFC per-lane commands (`SET_LONG_MOVE_SPEED`, `AFC_RESET_MOTOR_TIME`) now apply to all lanes instead of only the first
- AFC bowden length per-extruder now maps through unit membership to find the correct hub
- AMS mini status widget fills available height in multi-unit stacked layouts
- AMS tool badge labels use pre-formatted buffers with auto-sized badge width
- AMS backend skipped for tool changes when backend doesn't manage the tool
- AMS nozzle count corrected for mixed topology with unique per-lane tool mappings
- Happy Hare tip method detection reads from configfile on startup
- Mock mixed topology corrected to match real hardware (Box Turtle=HUB, AMS_2=PARALLEL)
- Worktree setup script auto-detects worktree when run without arguments

## [0.10.8] - 2026-02-19

### Added
- Debug bundle now collects Moonraker state, config, and Klipper/Moonraker logs via REST API
- PII sanitization in debug bundles for emails, API tokens, webhook URLs, and MAC addresses

### Fixed
- Crash from running animations when navigating away from a panel (#128)
- Crash from NULL font pointer during AMS bar layout rebuild
- Crash from stale async callbacks in gcode viewer
- Systemd update watcher stuck in infinite restart loop due to PathExists check
- Debug bundle log fetching handles HTTP 416 Range Not Satisfiable responses

## [0.10.7] - 2026-02-18

### Fixed
- AMS context menu UX: hidden tool dropdown, auto-close on backup select, conflict toast, and infinity icon for unlimited backup
- AMS Load/Unload/Eject buttons now work correctly from context menu
- AMS filament sensor toasts suppressed during active load/unload operations
- AFC `SET_RUNOUT` parameter corrected from `RUNOUT_LANE` to `RUNOUT`
- AFC 'Loaded' hub status correctly mapped to available instead of loaded
- AFC tip method detection from config with inline comment stripping
- Spoolman polling log noise suppressed unless spool weights actually changed
- Touch calibration wizard disabled ABS mismatch override for HDMI devices
- Power device probe no longer shows error toast on printers without power component
- Moonraker update manager switched from `type: zip` to `type: web` with systemd restart watcher

### Changed
- Removed dead AmsSlotEditPopup code replaced by context menu

## [0.10.6] - 2026-02-18

### Fixed
- Infinite CPU loop when saving Spoolman spool assignments on AFC and Happy Hare systems — Spoolman weight polling now updates slot state without sending G-code back to firmware, breaking a feedback cycle that saturated the CPU

## [0.10.5] - 2026-02-18

### Added
- **Android port**: Initial Android build system with CMake/Gradle, APK asset extraction, SDL fullscreen, and CI release pipeline
- **Power panel**: Moonraker power device control with home panel toggle and advanced menu integration
- Widget-safe async callback utilities for LVGL event handling

### Fixed
- AMS crash on quit from unjoinable scenario/dryer threads
- AMS right column capped at 200px max width for proper flex layout
- AMS tool count, hub sensor, and status corrected for mixed-topology AFC
- AMS current slot label improved for multi-unit and tool changer displays
- AMS 'Tooled' status handled correctly with production data regression tests
- Gcode viewer SEGV from unsafe async callback
- History totals computed from job list instead of hardcoded mock values
- Update check errors now visible in settings UI
- Installer uses printf for ANSI escapes instead of echo for POSIX compliance

### Changed
- Test output cleaned up: ~637 spurious warning/error lines silenced

## [0.10.4] - 2026-02-18

Slicer-preferred progress, Klipper M117 display messages, interactive AMS toolheads, and a batch of AMS rendering and stability fixes.

### Added
- Slicer-preferred progress via `display_status` — uses slicer-reported percentage over file position when available (#122)
- Klipper M117 display message shown on home panel print card (#124)
- Clickable AMS toolheads with docked dimming for parallel topology
- Per-lane eject and reset actions in AMS context menu
- Opacity and dim support for nozzle renderers
- Animated icons on controls panel

### Fixed
- Stale "Print" button text when print state changes (#125)
- Touch calibration wizard now shown for capacitive screens with ABS range mismatch (#123)
- AMS backend priority: MMU preferred over toolchanger when both are present
- AMS filament path lanes aligned with spool visual centers at all breakpoints
- AMS nozzle unloaded color unified and tool changer filament segments corrected
- AMS nozzle tip color changed to charcoal with idle path line fix
- AMS mini status bar sizing no longer applies 2/3 height scaling
- AMS toolchangers use `T{n}` gcode with click lockout during operations
- AFC slots only marked LOADED when `tool_loaded` is true
- Crash from NULL font pointer in AMS panel backend selector (#110)
- Touch input auto-detection scoring improved for multi-input systems (#117)
- `touch_device` config setting now read from the correct location
- Stale thumbnail no longer persists when a new print is started externally
- Self-update survives systemd cgroup kill (#118)
- UI switch size preset initialized before optional parse
- Filament panel left column layout flattened for proper flex_grow on temperature graph
- Noisy `assign_spool` warning downgraded to trace for virtual tool mappings
- Moonraker update manager release name uses tag-only format for compatibility
- `systemctl restart` uses `--no-block` to eliminate race window during updates
- Docker build uses GitHub mirror for zlib download

### Changed
- Crash reporting worker converted from JavaScript to TypeScript with GitHub App integration for dedup issue creation

## [0.10.3] - 2026-02-17

Big AMS release — unified slot editor with Spoolman integration, mixed-topology AFC support, error state visualization, and a major DRY refactor of shared drawing utilities across all AMS panels. Also adds 34 new translations and fixes several installer issues.

### Added
- **Unified Slot Editor**: New AMS slot edit modal with inline Spoolman picker, side-by-side vendor/material dropdowns, cancel/save flow, and in-use spool disabling
- **Spoolman Slot Saver**: Change detection and automatic save flow for slot-to-spool assignments with filament persistence
- **Mixed AFC Topology**: Box Turtle PARALLEL + OpenAMS HUB coexisting in a single AFC system
- **AMS Error Visualization**: Slot error dots, hub tinting, pulsing animations, severity colors, and aligned error detection across mini status and slot views
- **Shared Drawing Utilities**: Consolidated color, contrast, severity, fill, bar-width, display-name, logo fallback, pulse, error badge, slot bar, and container helpers
- Canonical `SlotInfo::is_present()` presence check for consistent slot detection
- Picker sub-view XML and header declarations for the unified edit modal
- 34 new translations across all 8 target languages with missing `translation_tag` attributes added
- Debug bundle fetch/display helper script
- ARM unwind tables and /proc/self/maps in crash reports

### Fixed
- Non-translatable strings (product names, URLs, OK) incorrectly wrapped in lv_tr()
- AMS edit modal Spoolman callbacks not marshalled to main thread
- AMS bypass detection, Happy Hare speed params, and other deferred TODOs resolved
- AMS bypass, dryer, reset, and settings hidden for tool changers
- Brand/spoolman_id missing from AFC and multi-AMS mock slots
- Load button enabled when slot already loaded
- Change Spool button label not updating correctly (ui_button_set_text)
- Static instance pointer for edit modal callbacks
- Updater diagnostic logs too noisy (downgraded to debug), added 2-min install timeout
- zlib updated from 1.3.1 to 1.3.2; Ubuntu CI build timeout bumped to 45min
- Installer stale .old directory blocking repeated updates (PR #102, thanks @bassco)
- Installer false-fail when cleanup_old_install hits root-owned hooks.sh

### Changed
- AMS panels refactored to use shared drawing utilities (DRY across 5 UI files: slot, mini status, overview, spool canvas, panel)
- Assign Spool removed from AMS context menu, replaced by unified slot editor
- Deprecated C-style wrapper APIs and legacy compatibility code removed
- Bundled installer regenerated with latest module changes

## [0.10.2] - 2026-02-17

This release significantly improves multi-tool printer support with per-tool spool persistence and an extruder selector for filament management, decouples Spoolman from AMS backends for cleaner architecture, and fixes several crash bugs and installer issues.

### Added
- Extruder selector dropdown for multi-tool printers in filament management
- Per-tool spool persistence decoupled from AMS backends
- Crash analytics dashboard with crash list view
- Load base and platform metadata in crash telemetry events
- Filament type tracking in print outcome telemetry events
- Discord notifications on successful releases

### Fixed
- Use-after-free during toast notification replacement (fixes #98)
- Dangling pointer after external modal deletion in AMS dryer dialog (fixes #97)
- Crash from null font pointer in AMS mini status overflow label (fixes #90, #91)
- Unsafe move operators corrupting lv_subject_t linked lists in setup wizard
- OTA updater "Installer not found" regression from systemd PATH resolution
- ELF architecture validation for K1/MIPS platform
- Static-linked OpenSSL for pi32 with post-install ldd verification
- AMS context menu positioning and ghost button borders
- Hidden tray and redundant tool badges for tool changers
- AMS bypass, dryer, reset, and settings visibility for tool changers
- Click-through on nozzle icon component
- Navigation bar buttons not filling available width, with lingering focus rings

### Changed
- Spoolman integration decoupled from AMS backends into standalone architecture
- Nozzle icon extracted into reusable component with consolidated tool badge logic
- Codebase migrated to `helix::` namespace with modernized enum classes
- Telemetry worker updated to support schema v2 nested fields

## [0.10.1] - 2026-02-16

### Added
- Debug bundle upload for streamlined support diagnostics
- Unified active extruder temperature tracking across multi-tool setups
- Dynamic nozzle label showing tool number for multi-tool printers
- Configurable size property for filament sensor indicator
- PrusaWire added to printer database
- Email notifications for debug bundle uploads (crash worker)
- 69 new translations across 8 languages

### Fixed
- Use-after-free in deferred observer callbacks (fixes #83)
- Crash from error callbacks firing during MoonrakerClient destruction
- Crash from SubscriptionGuard accessing destroyed MoonrakerClient on shutdown
- Crash from theme token mismatch in AMS backend selector
- Observer crash on quit from NavigationManager init ordering
- Spdlog call in ObserverGuard static destructor causing shutdown hang
- Tool badge showing unnecessarily with single-tool printers
- Hardware discovery falsely flagging expected devices as new
- Setup wizard not clearing hardware config on re-run
- Wizard port input accepting non-numeric characters
- Splash screen suppressing rendering without an external splash process
- Noisy WLED and REST 404 logs downgraded from warn to debug
- AMS slot info updates logged on every poll instead of only on change
- Installer using bare sudo instead of file_sudo for release swap/restore
- AMS edit modal Spoolman callbacks not marshalled to main thread

### Changed
- Spoolman vendor/filament creation moved to modal dialogs
- Spool wizard graduated from beta
- Lifetime checks added to SubscriptionGuard and ObserverGuard
- Shutdown cleanup self-registered in all init_subjects() methods

## [0.10.0] - 2026-02-15

Major feature release bringing full Spoolman spool management, a guided spool creation wizard, multi-unit AMS support for AFC and Happy Hare, probe management, and a Klipper config editor. Also adds Elegoo Centauri Carbon 1 support and fixes several crash bugs.

### Added
- **Spoolman Management**: Browse, search, edit, and delete spools with virtualized list, context menu, and inline edit modal
- **New Spool Wizard** (beta): 3-step guided creation (Vendor → Filament → Spool Details) with dual-source data from Spoolman server and SpoolmanDB catalog, atomic creation with rollback
- **Multi-unit AMS**: Support for multiple AMS/AFC/Happy Hare units with per-unit overview panel, shared spool grid components, and error/buffer health visualization
- **Probe Management** (beta): BLTouch panel with deploy/retract/self-test, probe type detection for Cartographer, Beacon, Tap, and Klicky
- **Klipper Config Editor**: Structure parser with include resolution, targeted edits, and post-edit health check with automatic backup restore
- **Elegoo Centauri Carbon 1**: Platform support with dedicated build toolchain and presets
- AFC error notifications with deduplication and action prompt suppression
- Android-style clear button for all search inputs
- Toast notifications for AMS device actions
- Internationalization for remaining hardcoded UI strings

### Fixed
- Crash from re-entrant observer destruction during callback dispatch (fixes #82)
- Use-after-free when destroying widgets from event callbacks (fixes #80)
- AMS slot tray visibility behind badge/halo overlays
- AFC buffer fault warnings not clearing on recovery
- Happy Hare reason_for_pause not clearing on idle
- Icon font validation locale handling
- Focus on close/context menu buttons causing unintended list scroll
- Modal dialog bind_text subject references missing @ prefix

### Changed
- AMS detail views refactored to shared ams_unit_detail and ams_loaded_card components
- Spoolman and history panel search inputs use shared text_input clear button
- R2 release retention policy added to prune old releases

## [0.9.24] - 2026-02-15

### Fixed
- OTA updates now correctly extract the installer from release tarballs (path mismatch between packaging and extraction)
- Button visual shift on release by setting transform pivot in base button style

## [0.9.23] - 2026-02-15

### Added
- LED colors stored as human-readable #RRGGBB hex strings with automatic legacy integer migration
- ASLR auto-detection in backtrace resolver for more accurate crash report symbol resolution

### Fixed
- Crash from LVGL object user_data ownership collisions causing SIGABRT
- Crash from NULL pointer passed to lv_strdup
- Use-after-free in animation completion callbacks
- Use-after-free when replacing toast notifications during exit animation

## [0.9.22] - 2026-02-15

### Added
- Timelapse phase 2: event handling, render notifications, and video management
- AD5M ready-made firmware image as primary install option in docs

### Fixed
- **Critical**: install.sh now included in release packages, fixing "Installer not found" error during UI-initiated updates (thanks @bassco)

### Changed
- CI release pipeline refactored to matrix builds for easier platform maintenance (thanks @bassco)

## [0.9.21] - 2026-02-14

### Added
- G-code console gated behind beta features setting
- Cancel escalation system: configurable e-stop timeout with settings toggle and dropdown
- Internationalization for hardcoded settings strings

### Fixed
- Nested overlay backdrops no longer double-stack
- Crash handler and report dialog disabled in test mode to prevent test interference
- Installer now extracts install.sh from tarball to prevent stale script failures
- Operation timeout guards increased for homing, QGL, and Z-tilt commands
- Touch calibration option hidden for USB HID input devices

### Changed
- G-code console and cancel escalation documented in user guide

## [0.9.20] - 2026-02-14

This release adds multi-extruder temperature support, tool state tracking, multi-backend AMS (allowing printers with multiple filament systems), and fixes a critical installer bug that prevented Moonraker from starting on ForgeX AD5M printers after reboot.

### Added
- Multi-extruder temperature support with dynamic ExtruderInfo discovery and selection panel
- Tool state tracking (ToolState singleton) with active tool badge and tool-prefixed temperature display
- Multi-backend AMS: per-backend slot storage, event routing, backend selector UI, and multi-system detection
- ASLR-aware crash reports: ELF load base emitted for accurate symbol resolution
- AD5M boot diagnostic script for troubleshooting boot/networking issues
- Russian translation updates (thanks @kostake, @panzerhalzen)
- Telemetry Analytics Engine dashboard

### Fixed
- **Critical**: ForgeX installer logged wrapper broke S99root boot sequence, preventing Moonraker from starting after reboot (#36)
- Splash screen no longer triggers LVGL rendering while it owns the framebuffer
- Exception-safe subject updates in sensor callbacks
- UpdateQueue crash protection with try-catch in process_pending
- Notification and input shaper overlays use modal alert system instead of manual event wiring
- Invalid text_secondary design token replaced with text_muted
- LED macro preset UX improvements and stale deletion bug fix
- systemd service adds tty to SupplementaryGroups for console suppression

### Changed
- Async invoke simplified to forward directly to ui_queue_update
- LED preset labels auto-generated from macro names instead of manual naming

## [0.9.19] - 2026-02-13

### Added
- Crash reports now include fault info, CPU registers, and frame pointers for better diagnostics
- XLARGE breakpoint tier for responsive UI on larger displays
- Responsive fan card rendering with dynamic arc sizing and tiny breakpoint support
- Unified responsive icon sizing via design tokens
- Geralkom X400/X500 and Voron Micron added to printer database
- HelixScreen Discord community link in documentation

### Fixed
- Overlay close callback deferred to prevent use-after-free crash (#70)
- macOS build error caused by libhv gettid() conflict

### Changed
- 182 missing translation keys added across the UI
- Navigation bar width moved from C++ to XML for declarative layout control
- Qidi and Creality printer images updated; Qidi Q2 Pro removed

## [0.9.18] - 2026-02-13

### Added
- Actionable notifications: tapping notification history items now dispatches their associated action (e.g. navigate to update panel)
- Skipped-update notifications persist in notification history with tap-to-navigate

### Fixed
- LED macro integration: macro backend now correctly tracks LED state and handles device transitions
- Pre-rendered generic printer images updated with correct corexy model

## [0.9.17] - 2026-02-13

### Added
- Full LED control system with four backends, auto-state mapping editor, macro device configuration, and settings overlay
- Crash report dialog with automatic submission, QR code for manual upload, and local file fallback
- Layer estimation from print progress when slicer lacks SET_PRINT_STATS_INFO (#37)
- Rate limiting on crash and telemetry ingest workers

### Fixed
- Crash reporter now shows modal before TelemetryManager consumes the crash file
- LED strip auto-selection on first discovery, lazy LED reads, icon and dropdown fixes
- Installer config file operations use minimal permissions instead of broad sudo

### Changed
- Motion overlay refactored to declarative UI with homing indicators and theme colors
- LED settings layout extracted to reusable XML components
- User guide restructured into sub-pages with new screenshots

## [0.9.16] - 2026-02-12

### Added
- Printer Manager overlay accessible from home screen with tap-to-open, custom printer images, inline name editing, and capability chips
- Theme-aware markdown viewer
- Custom printer image selection with import support and list+preview layout

### Fixed
- Setup wizard now defaults IP to 127.0.0.1 for local Moonraker connections
- Whitespace in IP and port input fields no longer causes validation errors

### Changed
- All modals standardized to use the Modal system with ui_dialog
- AMS modals refactored to use modal_button_row component
- Release assets now include install.sh (thanks @Major_Buzzkill)
- Markdown submodule updated with faux bold fix

## [0.9.15] - 2026-02-12

### Fixed
- Touchscreen calibration wizard no longer appears on capacitive displays (#40)
- Calibration verify step now applies new calibration so accept/retry buttons are tappable
- Debug logging via HELIX_DEBUG=1 in env file now works correctly after sourcing order fix
- Release pipeline R2 upload failing when changelog contains special characters
- Symbol resolution script using wrong domain (releases.helixscreen.com → .org)
- User docs referencing `--help | head -1` instead of `--version` for version checks

## [0.9.14] - 2026-02-12

### Fixed
- Installer fails on systems without hexdump (e.g., Armbian) with "Cannot read binary header" error

## [0.9.13] - 2026-02-11

### Added
- Frequency response charts for input shaper calibration with shaper overlay toggles
- CSV parser for Klipper calibration frequency response data
- Filament usage tracking with live consumption during printing and slicer estimates on completion modal
- Unified error modal with declarative subjects and single suppression
- Ultrawide home panel layout for 1920x480 displays
- Internationalization support for header bar and overlay panel titles
- Demo mode for PID and input shaper calibration screenshots
- Klipper/Moonraker pre-flight check in AD5M and K1 installers

### Fixed
- getcwd errors during AD5M startup (#36)
- Installer permission denied on tar extraction cleanup (#34)
- Print tune panel layout adjusted to fit 800x480 screens
- CLI hyphen normalization for layout names

### Changed
- Input shaper graduated from beta to stable
- Width-aware Bresenham line drawing for G-code layer renderer
- Overlay content padding standardized across panels
- Action button widths use percentages instead of hardcoded pixels

## [0.9.12] - 2026-02-11

### Added
- QIDI printer support with detection heuristics and print start profile
- Snapmaker U1 cross-compile target, printer detection, and platform support
- Layout manager with auto-detection for alternative screen sizes and CLI override
- Input shaper panel redesigned with config display, pre-flight checks, per-axis results, and save button
- PID calibration: live temperature graph, progress tracking, old value deltas, abort support, and 15-minute timeout
- Multi-LED chip selection in settings replacing single dropdown
- Macro browser (gated behind beta features)

### Fixed
- Crash on shutdown from re-entrant Moonraker method callback map destruction
- Installer: BusyBox echo compatibility for ANSI colors and temp directory auto-detection
- Missing translations for telemetry, sound, PID, and timelapse strings
- Unwanted borders on navigation bar and home status card buttons
- Scroll-on-focus in plugin install modal
- Beta feature flag conflict hiding hardware check rows in advanced settings

### Changed
- PID calibration ungated from beta features — now available to all users
- Moonraker API abstraction boundary enforced — UI no longer accesses WebSocket client directly
- Test-only methods moved to friend test access pattern (cleaner production API)

## [0.9.11] - 2026-02-10

Sound system, KIAUH installer, display rotation, PID tuning, and timelapse support — plus a splash screen fix for AD5M.

### Added
- Sound system with multi-backend synthesizer engine (SDL audio, PWM sysfs, M300 G-code), JSON sound themes (minimal, retro chiptune), toggle sounds, and theme preview
- Sound settings overlay with volume slider and test beep on release
- KIAUH installer integration for one-click install from KIAUH menu
- Display rotation support for all three binaries (main, splash, watchdog)
- PID tuning calibration with fan control and material presets
- Timelapse plugin detection, install wizard, and settings UI (beta)
- Versioned config migration system
- Shadow support and consistent borders across widgets
- Platform-aware cache directory resolution for embedded targets
- Telemetry analytics pipeline with admin API, pull script, and analyzer

### Fixed
- Splash process not killed on AD5M when pre-started by init script (display flashing)
- Layer count tracking with G-code response fallback
- Print file list 15-second polling fallback for missed WebSocket updates
- Display wake from sleep on SDL click
- Translation sync with extractor false-positive cleanup
- Cross-compiled binaries now auto-stripped after linking
- Build system tracks lv_conf.h as LVGL compile prerequisite
- LayerTracker debug log spam reduced to change-only logging

### Changed
- Sound system and timelapse gated behind beta features flag
- Bug report and feature request GitHub issue templates added

## [0.9.10] - 2026-02-10

Hotfix release — gradient and flag images were broken for all users due to a missing decoder setting, and WiFi initialization caused a 5-second startup delay on NetworkManager-based systems.

### Added
- Optional bed warming step before Z-offset calibration
- Reusable multi-select checkbox widget
- Symbol maps for crash backtrace resolution
- KIAUH extension discovery tests

### Fixed
- Gradient and flag images failing to load (LV_BIN_DECODER_RAM_LOAD not enabled)
- WiFi backend now tries NetworkManager first, avoiding 5-second wpa_supplicant timeout on most systems
- Observer crash on shutdown from subject lifetime mismatch
- Connection wizard mDNS section hidden, subtitle improved

### Changed
- Project permission settings organized into .claude/settings.json

## [0.9.9] - 2026-02-09

Telemetry, security hardening, and a bundled uninstaller — plus deploy packages are now ~60% smaller.

### Added
- Anonymous opt-in telemetry with crash reporting, session recording, and auto-send scheduler
- Hardware survey enrichment for telemetry sessions (schema v2)
- Telemetry opt-in step in setup wizard with info modal
- Cloudflare Worker telemetry backend
- Bundled uninstaller with 151 shell tests
- Creality K2 added to GitHub release workflow

### Fixed
- Framebuffer garbage on home panel from missing container background
- Observer crash on quit from subject/display deinit ordering
- Stale subject pointers in ToastManager and WizardTouchCalibration on shutdown
- Print thumbnail offset and outcome overlay centering
- Confetti particle system rewritten to use native LVGL objects
- Print card thumbnail overlap — e-stop relocated to print card
- Auto-navigation to print status suppressed during setup wizard
- KIAUH extension discovery uses native import paths (fixes #30)
- Data root auto-detected from binary path with missing globals.xml abort
- NaN/Inf guards on all G-code generation paths
- Safe restart via absolute argv[0] path resolution
- Replaced system() with fork/execvp in ping_host()
- Tightened directory permissions, replaced strcpy with memcpy
- K2 musl cross-compilation LDFLAGS
- Telemetry opt-in enforced for crash events
- Telemetry enabled state synced at startup with API key auth

### Changed
- Deploy footprint reduced ~60% with asset excludes and LZ4 image compression
- Shell test gate added to release workflow

## [0.9.8] - 2026-02-09

### Added
- G-code toolpath render uses AMS/Spoolman filament colors for accurate color previews
- Reprint button shown for all terminal print states (error, cancelled, complete)
- Config symlinked into printer_data for editing via Mainsail/Fluidd file manager
- Async button timeout guard to prevent stuck UI on failed operations
- 35 new translation strings synced across all languages

### Fixed
- Slicer time estimate preserved across reprints instead of resetting to zero
- Install directory ownership for Moonraker update manager (fixes #29)
- Python 3.9 compatibility for Sonic Pad KIAUH integration (fixes #28)
- Display sleep using software overlay for unrecognized display hardware (#23)
- Z-offset controls compacted for small displays (#27)
- Print error state handled with badge, reprint button, and automatic heater shutoff
- WebSocket callbacks deferred to main thread preventing UI race conditions
- Responsive breakpoints based on screen height instead of max dimension
- Cooldown button uses TURN_OFF_HEATERS for reliable heater shutoff
- Splash screen support for ultra-wide displays
- 32-bit userspace detection on 64-bit Pi kernels
- Graph Y-axis label no longer clips top padding
- Print card info column taps now navigate to status screen
- Watchdog double-instance prevented on supervised restart
- Internal splash skipped when external splash process is running
- Resolution auto-detection enabled at startup

### Changed
- Z-offset scale layout dynamically adapts to measured label widths
- Filament panel temperature updates are targeted instead of full-refresh
- Machine limits G-code debounced to reduce unnecessary sends
- Delete button on print detail uses danger styling

## [0.9.7] - 2026-02-08

Z-offset calibration redesigned from scratch with a Prusa-style visual meter,
plus display reliability fixes and hardware detection improvements.

### Added
- Z-offset calibration overhaul: Prusa-style vertical meter with draw-in arrow animation, horizontal step buttons, auto-ranging scale, saved offset display, and auto-navigation when calibration is in progress
- Z-offset calibration strategy system for printer-specific save commands
- Automatic update notifications with dismiss support
- Sleep While Printing toggle to keep display on during prints
- Hardware detection: mainboard identification heuristic, non-printer addon exclusion, and kinematics filtering
- Calibration features gated behind beta feature flag

### Fixed
- Crash from rapid filament load/unload button presses
- Crash dialog not initializing touch calibration config
- Keyboard shortcuts firing when typing in text inputs
- Parent directory (..) not always sorted first in file browser
- Splash screen crash when prerendered assets missing
- Console bleed-through on fbdev displays
- Display not repainting fully after wake from sleep
- Moonraker updates switched from git_repo to zip type for reliability
- Thumbnail format forced to ARGB8888 for correct rendering
- Print outcome badges misaligned above thumbnail
- Scroll-on-focus causing unwanted panel jumps
- Install service filtering to only existing system groups
- Screws tilt adjust detection from configfile fallback
- Wizard saving literal 'None' instead of empty string for unselected hardware
- Mock printer kinematics matching actual printer type
- Touch calibration detection unified with headless pointer support

### Changed
- Dark mode applies live without restart
- Calibration button layout redesigned Mainsail-style
- Textarea widgets migrated to text_input component
- Redundant kinematics polling eliminated

## [0.9.6] - 2026-02-08

### Added
- Per-object G-code toolpath thumbnails in Print Objects overlay
- AFC (Armored Turtle) support: live device state, tool mapping, endless spool, per-lane reset, maintenance and LED controls, quiet mode, and mock simulation
- Active object count shown on layer progress line during printing
- Change Host modal for switching Moonraker connection in settings
- Z movement style override setting and E-Stop relocated to Motion section
- K1 dynamic linking toolchain and build target
- Creality K2 series cross-compilation target (ARM, static musl — untested, needs hardware validation)
- CDN-first installer downloads with GitHub fallback
- Multi-channel R2 update distribution with GitHub API fallback

### Fixed
- Toasts now render on top layer instead of active screen (fixes toasts hidden behind overlays)
- Print cancel timeout increased to 15s with active state observation for more reliable cancellation
- Pre-print time estimates seeded from slicer data with blended early progress
- Thread-safe slicer estimate seeding during print start
- G-code viewer cache thrash from current_object changes during exclude-object prints
- ForgeX startup framebuffer stomping by S99root init script
- Wrong-platform binary install prevented with ELF architecture validation and safe rollback
- Use-after-free crash on Print Objects overlay close
- Isometric thumbnail rendering with shared projection, depth shading, and thicker lines
- Install warning text centered in update download modal
- Missing alert_circle icon codepoint
- Settings About section consolidated with cleaner version row layout
- Z baby step icons and color swatch labels
- Exclude object mock mode: objects populated from G-code on print start with proper status dispatch

## [0.9.5] - 2026-02-07

### Added
- Exclude object support for streaming/2D mode with selection brackets and long-press interaction
- Print Objects list overlay showing defined objects during a print
- LED selection dropdown in settings for multi-LED printers
- Version number displayed on splash screen
- Beta and dev update channels with UI toggle and R2 upload script
- Beta feature wrapper component with badge indicator
- 32-bit ARM (armv7l) Raspberry Pi build target (#10)
- Auto-publish tagged releases to R2 with platform detection
- Exclude object G-code parsing and status dispatch in mock mode

### Fixed
- Use-after-free race in wpa_supplicant backend shutdown (#8)
- Deadlock in Happy Hare and ToolChanger AMS backend start (#9)
- DNS resolver fallback for static glibc builds
- Crash when navigating folders during metadata fetch in print selection
- LED detection excluding toolhead LEDs from main LED control
- WebSocket max message size increased from 1MB to 5MB (#7)
- Elapsed/remaining time display during mock printing
- Crash on window close from SDL event handling during shutdown
- Accidental scroll taps by increasing scroll limit default
- G-code parser now reads layer_height, first_layer_height, object_height from metadata
- Invalid text_secondary color token replaced with text_muted
- KIAUH metadata wrapper key and moonraker updater path (#3)
- Installer sparse checkout for updater repo (#11)
- Output_pin lights detected as LEDs with fallback to first LED (#14)
- Percentage rounding instead of truncating to fix float precision (#14)
- Z offset display sync when print tune overlay opens (#14)
- CoreXZ treated as gantry-moves-Z instead of bed-moves (#14)

### Changed
- Log levels cleaned up: INFO is concise, DEBUG is useful without per-layer/shutdown spam
- Duplicate log bugs fixed (PrintStartCollector double-completion, PluginManager double-unload)
- Settings panel version rows deduplicated
- Exclude object modal XML registration and single-select behavior

## [0.9.4] - 2026-02-07

### Added
- Pre-print time predictions based on historical heating/homing data
- Heater status text on temperature cards (Heating, Cooling, At Target)
- Slicer estimated time fallback for remaining time
- Seconds in duration display under 5 minutes

### Fixed
- Crash on 16bpp HDMI screens from forced 32-bit color format
- Elapsed time using wall-clock duration instead of print-only time
- Pre-print overlay showing when it shouldn't
- Backlight not turning off on AD5M
- Heater status colors (heating=red, added cooling state)
- AMS row hidden when no AMS connected
- Modal button alignment
- Install script version detection on Pi (#6)

## [0.9.3] - 2026-02-06

First public beta release. Core features are complete — we're looking for early
adopters to help find edge cases.

**Supported platforms:** Raspberry Pi (aarch64), FlashForge AD5M (armv7l),
Creality K1 (MIPS32)

> **Note:** K1 binaries are included but have not been tested on hardware. If you
> have a K1, we'd love your help verifying it works!

### Added
- Print start profiles with modular, JSON-driven signal matching for per-printer phase detection
- NetworkManager WiFi backend for broader Linux compatibility
- `.3mf` file support in print file browser
- Non-printable file filtering in print selection
- Beta features gating system for experimental UI (HelixPrint plugin)
- Platform detection and preset system for zero-config installs
- Settings action rows with bind_description for richer UI
- Restart logic consolidated into single `app_request_restart_service()` entry point

### Fixed
- Print start collector not restarting after a completed print
- Sequential progress regression on repeated signals during print start
- Bed mesh triple-rendering and profile row click targets
- Wizard WiFi step layout, password visibility toggle, and dropdown corruption
- Touch calibration skipped for USB HID touchscreens (HDMI displays)
- CJK glyph inclusion from C++ sources in font generation
- File ownership for non-root deploy targets
- Console cursor hidden on fbdev displays

### Changed
- Pi deploys now use `systemctl restart` instead of stop/start
- fbdev display backend for Pi (avoids DRM master contention)
- Comprehensive architectural documentation from 5-agent audit
- Troubleshooting guide updated with debug logging instructions

## [0.9.2] - 2026-02-05

Major internal release with live theming, temperature sensor support, and
extensive UI polish across all panels.

### Added
- Live theme switching without restart — change themes in settings instantly
- Dark/light gradient backgrounds and themed overlay constants
- Full-screen 3D splash images with dark/light mode support
- Temperature sensor manager for auxiliary temp sensors (chamber, enclosure, etc.)
- Responsive fan dial with knob glow effect
- Software update checker with download progress and install-during-idle safety
- Platform hook architecture for modularized installer functions
- Auto-detect Pi install path from Klipper ecosystem
- AD5M preset with auto-detection for zero-config setup
- Beta features config flag for gating experimental UI
- CJK glyph support (Chinese, Japanese, Russian) in generated fonts
- Pencil edit icons next to temperature controls
- OS version, MCU versions, and printer name in About section
- Shell tests (shellcheck, bats) gating release builds

### Fixed
- Shutdown crash: stop animations before destroying panels to prevent use-after-free
- Observer crash: reorder display/subject teardown sequence
- Stale widget pointer guards for temperature and fan updates
- Theme palette preservation across dark/light mode switches
- Button text contrast for layout=column buttons with XML children
- Navbar background not updating on theme toggle
- Dropdown corruption with `&#10;` newline entities in XML
- Wizard initialization: fan subscriptions, sensor select, toast suppression
- Kinematics detection and Z button icons for bed-moves printers
- Bed mesh data normalization and zero plane visibility
- Filament panel deferred `set_limits` to main thread
- Touch calibration target spread and full-screen capture

### Changed
- Pi builds target Debian Bullseye for wider compatibility
- Static-link OpenSSL for cross-platform SSL support
- Binaries relocated to `bin/` subdirectory in deploy packages
- Fan naming uses configured roles instead of heuristics
- HelixScreen brand theme set as default
- Installer modularized with platform dispatchers
- Release build timeout increased to 60 minutes

## [0.9.1] - 2026-02-04

Initial tagged release. Foundation for all subsequent development.

### Added
- 30 panels and 16 overlays covering full printer control workflow
- First-run setup wizard with 8-step guided configuration
- Multi-material support: AFC, Happy Hare, tool changers, ValgACE, Spoolman
- G-code preview and 3D bed mesh visualization
- Calibration tools: input shaper, mesh leveling, screws tilt, PID, firmware retraction
- Internationalization system with hot-reload language switching
- Light and dark themes with responsive 800x480+ layout
- Cross-compilation for Pi (aarch64), AD5M (armv7l), K1 (MIPS32)
- Automated GitHub Actions release pipeline
- One-liner installation script with platform auto-detection

[1.1.0-beta.16]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.15...v1.1.0-beta.16
[1.1.0-beta.15]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.14...v1.1.0-beta.15
[1.1.0-beta.14]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.13...v1.1.0-beta.14
[1.1.0-beta.13]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.12...v1.1.0-beta.13
[1.1.0-beta.12]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.11...v1.1.0-beta.12
[1.1.0-beta.11]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.10...v1.1.0-beta.11
[1.1.0-beta.10]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.9...v1.1.0-beta.10
[1.1.0-beta.9]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.8...v1.1.0-beta.9
[1.1.0-beta.8]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.7...v1.1.0-beta.8
[1.1.0-beta.7]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.6...v1.1.0-beta.7
[1.1.0-beta.6]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.5...v1.1.0-beta.6
[1.1.0-beta.5]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.4...v1.1.0-beta.5
[1.1.0-beta.4]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.3...v1.1.0-beta.4
[1.1.0-beta.3]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.2...v1.1.0-beta.3
[1.1.0-beta.2]: https://github.com/prestonbrown/helixscreen/compare/v1.1.0-beta.1...v1.1.0-beta.2
[1.1.0-beta.1]: https://github.com/prestonbrown/helixscreen/compare/v1.0.2...v1.1.0-beta.1
[1.0.6]: https://github.com/prestonbrown/helixscreen/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/prestonbrown/helixscreen/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/prestonbrown/helixscreen/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/prestonbrown/helixscreen/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/prestonbrown/helixscreen/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/prestonbrown/helixscreen/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/prestonbrown/helixscreen/compare/v0.99.118...v1.0.0
[1.0.0-rc.1]: https://github.com/prestonbrown/helixscreen/compare/v0.99.118...v1.0.0-rc.1
[0.99.118]: https://github.com/prestonbrown/helixscreen/compare/v0.99.117...v0.99.118
[0.99.117]: https://github.com/prestonbrown/helixscreen/compare/v0.99.116...v0.99.117
[0.99.116]: https://github.com/prestonbrown/helixscreen/compare/v0.99.115...v0.99.116
[0.99.115]: https://github.com/prestonbrown/helixscreen/compare/v0.99.114...v0.99.115
[0.99.114]: https://github.com/prestonbrown/helixscreen/compare/v0.99.113...v0.99.114
[0.99.113]: https://github.com/prestonbrown/helixscreen/compare/v0.99.112...v0.99.113
[0.99.112]: https://github.com/prestonbrown/helixscreen/compare/v0.99.111...v0.99.112
[0.99.111]: https://github.com/prestonbrown/helixscreen/compare/v0.99.108...v0.99.111
[0.99.110]: https://github.com/prestonbrown/helixscreen/compare/v0.99.109...v0.99.110
[0.99.109]: https://github.com/prestonbrown/helixscreen/compare/v0.99.108...v0.99.109
[0.99.108]: https://github.com/prestonbrown/helixscreen/compare/v0.99.107...v0.99.108
[0.99.107]: https://github.com/prestonbrown/helixscreen/compare/v0.99.106...v0.99.107
[0.99.106]: https://github.com/prestonbrown/helixscreen/compare/v0.99.105...v0.99.106
[0.99.105]: https://github.com/prestonbrown/helixscreen/compare/v0.99.103...v0.99.105
[0.99.103]: https://github.com/prestonbrown/helixscreen/compare/v0.99.102...v0.99.103
[0.99.102]: https://github.com/prestonbrown/helixscreen/compare/v0.99.101...v0.99.102
[0.99.101]: https://github.com/prestonbrown/helixscreen/compare/v0.99.100...v0.99.101
[0.99.100]: https://github.com/prestonbrown/helixscreen/compare/v0.99.99...v0.99.100
[0.99.99]: https://github.com/prestonbrown/helixscreen/compare/v0.99.98...v0.99.99
[0.99.98]: https://github.com/prestonbrown/helixscreen/compare/v0.99.97...v0.99.98
[0.99.97]: https://github.com/prestonbrown/helixscreen/compare/v0.99.96...v0.99.97
[0.99.96]: https://github.com/prestonbrown/helixscreen/compare/v0.99.95...v0.99.96
[0.99.95]: https://github.com/prestonbrown/helixscreen/compare/v0.99.94...v0.99.95
[0.99.94]: https://github.com/prestonbrown/helixscreen/compare/v0.99.93...v0.99.94
[0.99.93]: https://github.com/prestonbrown/helixscreen/compare/v0.99.92...v0.99.93
[0.99.92]: https://github.com/prestonbrown/helixscreen/compare/v0.99.91...v0.99.92
[0.99.91]: https://github.com/prestonbrown/helixscreen/compare/v0.99.90...v0.99.91
[0.99.90]: https://github.com/prestonbrown/helixscreen/compare/v0.99.89...v0.99.90
[0.99.89]: https://github.com/prestonbrown/helixscreen/compare/v0.99.88...v0.99.89
[0.99.88]: https://github.com/prestonbrown/helixscreen/compare/v0.99.87...v0.99.88
[0.99.87]: https://github.com/prestonbrown/helixscreen/compare/v0.99.86...v0.99.87
[0.99.86]: https://github.com/prestonbrown/helixscreen/compare/v0.99.85...v0.99.86
[0.99.85]: https://github.com/prestonbrown/helixscreen/compare/v0.99.84...v0.99.85
[0.99.84]: https://github.com/prestonbrown/helixscreen/compare/v0.99.82...v0.99.84
[0.99.82]: https://github.com/prestonbrown/helixscreen/compare/v0.99.81...v0.99.82
[0.99.81]: https://github.com/prestonbrown/helixscreen/compare/v0.99.80...v0.99.81
[0.99.80]: https://github.com/prestonbrown/helixscreen/compare/v0.99.79...v0.99.80
[0.99.79]: https://github.com/prestonbrown/helixscreen/compare/v0.99.78...v0.99.79
[0.99.78]: https://github.com/prestonbrown/helixscreen/compare/v0.99.77...v0.99.78
[0.99.77]: https://github.com/prestonbrown/helixscreen/compare/v0.99.76...v0.99.77
[0.99.76]: https://github.com/prestonbrown/helixscreen/compare/v0.99.75...v0.99.76
[0.99.75]: https://github.com/prestonbrown/helixscreen/compare/v0.99.74...v0.99.75
[0.99.74]: https://github.com/prestonbrown/helixscreen/compare/v0.99.73...v0.99.74
[0.99.73]: https://github.com/prestonbrown/helixscreen/compare/v0.99.72...v0.99.73
[0.99.72]: https://github.com/prestonbrown/helixscreen/compare/v0.99.71...v0.99.72
[0.99.71]: https://github.com/prestonbrown/helixscreen/compare/v0.99.70...v0.99.71
[0.99.70]: https://github.com/prestonbrown/helixscreen/compare/v0.99.69...v0.99.70
[0.99.69]: https://github.com/prestonbrown/helixscreen/compare/v0.99.68...v0.99.69
[0.99.68]: https://github.com/prestonbrown/helixscreen/compare/v0.99.67...v0.99.68
[0.99.67]: https://github.com/prestonbrown/helixscreen/compare/v0.99.66...v0.99.67
[0.99.66]: https://github.com/prestonbrown/helixscreen/compare/v0.99.65...v0.99.66
[0.99.65]: https://github.com/prestonbrown/helixscreen/compare/v0.99.64...v0.99.65
[0.99.64]: https://github.com/prestonbrown/helixscreen/compare/v0.99.63...v0.99.64
[0.99.63]: https://github.com/prestonbrown/helixscreen/compare/v0.99.62...v0.99.63
[0.99.62]: https://github.com/prestonbrown/helixscreen/compare/v0.99.61...v0.99.62
[0.99.61]: https://github.com/prestonbrown/helixscreen/compare/v0.99.60...v0.99.61
[0.99.60]: https://github.com/prestonbrown/helixscreen/compare/v0.99.59...v0.99.60
[0.99.59]: https://github.com/prestonbrown/helixscreen/compare/v0.99.58...v0.99.59
[0.99.58]: https://github.com/prestonbrown/helixscreen/compare/v0.99.57...v0.99.58
[0.99.57]: https://github.com/prestonbrown/helixscreen/compare/v0.99.56...v0.99.57
[0.99.56]: https://github.com/prestonbrown/helixscreen/compare/v0.99.55...v0.99.56
[0.99.55]: https://github.com/prestonbrown/helixscreen/compare/v0.99.54...v0.99.55
[0.99.54]: https://github.com/prestonbrown/helixscreen/compare/v0.99.53...v0.99.54
[0.99.53]: https://github.com/prestonbrown/helixscreen/compare/v0.99.52...v0.99.53
[0.99.52]: https://github.com/prestonbrown/helixscreen/compare/v0.99.51...v0.99.52
[0.99.51]: https://github.com/prestonbrown/helixscreen/compare/v0.99.50...v0.99.51
[0.99.50]: https://github.com/prestonbrown/helixscreen/compare/v0.99.49...v0.99.50
[0.99.49]: https://github.com/prestonbrown/helixscreen/compare/v0.99.48...v0.99.49
[0.99.48]: https://github.com/prestonbrown/helixscreen/compare/v0.99.47...v0.99.48
[0.99.47]: https://github.com/prestonbrown/helixscreen/compare/v0.99.46...v0.99.47
[0.99.46]: https://github.com/prestonbrown/helixscreen/compare/v0.99.45...v0.99.46
[0.99.45]: https://github.com/prestonbrown/helixscreen/compare/v0.99.44...v0.99.45
[0.99.44]: https://github.com/prestonbrown/helixscreen/compare/v0.99.43...v0.99.44
[0.99.43]: https://github.com/prestonbrown/helixscreen/compare/v0.99.42...v0.99.43
[0.99.42]: https://github.com/prestonbrown/helixscreen/compare/v0.99.41...v0.99.42
[0.99.41]: https://github.com/prestonbrown/helixscreen/compare/v0.99.40...v0.99.41
[0.99.40]: https://github.com/prestonbrown/helixscreen/compare/v0.99.39...v0.99.40
[0.99.39]: https://github.com/prestonbrown/helixscreen/compare/v0.99.38...v0.99.39
[0.99.38]: https://github.com/prestonbrown/helixscreen/compare/v0.99.37...v0.99.38
[0.99.37]: https://github.com/prestonbrown/helixscreen/compare/v0.99.36...v0.99.37
[0.99.36]: https://github.com/prestonbrown/helixscreen/compare/v0.99.35...v0.99.36
[0.99.35]: https://github.com/prestonbrown/helixscreen/compare/v0.99.34...v0.99.35
[0.99.34]: https://github.com/prestonbrown/helixscreen/compare/v0.99.33...v0.99.34
[0.99.33]: https://github.com/prestonbrown/helixscreen/compare/v0.99.32...v0.99.33
[0.99.32]: https://github.com/prestonbrown/helixscreen/compare/v0.99.31...v0.99.32
[0.99.31]: https://github.com/prestonbrown/helixscreen/compare/v0.99.30...v0.99.31
[0.99.30]: https://github.com/prestonbrown/helixscreen/compare/v0.99.29...v0.99.30
[0.99.29]: https://github.com/prestonbrown/helixscreen/compare/v0.99.28...v0.99.29
[0.99.28]: https://github.com/prestonbrown/helixscreen/compare/v0.99.27...v0.99.28
[0.99.27]: https://github.com/prestonbrown/helixscreen/compare/v0.99.26...v0.99.27
[0.99.26]: https://github.com/prestonbrown/helixscreen/compare/v0.99.25...v0.99.26
[0.99.25]: https://github.com/prestonbrown/helixscreen/compare/v0.99.24...v0.99.25
[0.99.24]: https://github.com/prestonbrown/helixscreen/compare/v0.99.23...v0.99.24
[0.99.23]: https://github.com/prestonbrown/helixscreen/compare/v0.99.22...v0.99.23
[0.99.22]: https://github.com/prestonbrown/helixscreen/compare/v0.99.21...v0.99.22
[0.99.21]: https://github.com/prestonbrown/helixscreen/compare/v0.99.20...v0.99.21
[0.99.20]: https://github.com/prestonbrown/helixscreen/compare/v0.99.19...v0.99.20
[0.99.19]: https://github.com/prestonbrown/helixscreen/compare/v0.99.18...v0.99.19
[0.99.18]: https://github.com/prestonbrown/helixscreen/compare/v0.99.17...v0.99.18
[0.99.17]: https://github.com/prestonbrown/helixscreen/compare/v0.99.16...v0.99.17
[0.99.16]: https://github.com/prestonbrown/helixscreen/compare/v0.99.15...v0.99.16
[0.99.15]: https://github.com/prestonbrown/helixscreen/compare/v0.99.14...v0.99.15
[0.99.14]: https://github.com/prestonbrown/helixscreen/compare/v0.99.13...v0.99.14
[0.99.13]: https://github.com/prestonbrown/helixscreen/compare/v0.99.12...v0.99.13
[0.99.12]: https://github.com/prestonbrown/helixscreen/compare/v0.99.11...v0.99.12
[0.99.11]: https://github.com/prestonbrown/helixscreen/compare/v0.99.10...v0.99.11
[0.99.10]: https://github.com/prestonbrown/helixscreen/compare/v0.99.9...v0.99.10
[0.99.9]: https://github.com/prestonbrown/helixscreen/compare/v0.99.8...v0.99.9
[0.99.8]: https://github.com/prestonbrown/helixscreen/compare/v0.99.7...v0.99.8
[0.99.7]: https://github.com/prestonbrown/helixscreen/compare/v0.99.6...v0.99.7
[0.99.6]: https://github.com/prestonbrown/helixscreen/compare/v0.99.5...v0.99.6
[0.99.5]: https://github.com/prestonbrown/helixscreen/compare/v0.99.4...v0.99.5
[0.99.4]: https://github.com/prestonbrown/helixscreen/compare/v0.99.3...v0.99.4
[0.99.3]: https://github.com/prestonbrown/helixscreen/compare/v0.99.2...v0.99.3
[0.99.2]: https://github.com/prestonbrown/helixscreen/compare/v0.99.1...v0.99.2
[0.99.1]: https://github.com/prestonbrown/helixscreen/compare/v0.99.0...v0.99.1
[0.99.0]: https://github.com/prestonbrown/helixscreen/compare/v0.98.12...v0.99.0
[0.98.12]: https://github.com/prestonbrown/helixscreen/compare/v0.98.11...v0.98.12
[0.98.11]: https://github.com/prestonbrown/helixscreen/compare/v0.98.10...v0.98.11
[0.98.10]: https://github.com/prestonbrown/helixscreen/compare/v0.98.9...v0.98.10
[0.98.9]: https://github.com/prestonbrown/helixscreen/compare/v0.98.8...v0.98.9
[0.98.8]: https://github.com/prestonbrown/helixscreen/compare/v0.98.7...v0.98.8
[0.98.7]: https://github.com/prestonbrown/helixscreen/compare/v0.98.6...v0.98.7
[0.98.6]: https://github.com/prestonbrown/helixscreen/compare/v0.98.5...v0.98.6
[0.98.5]: https://github.com/prestonbrown/helixscreen/compare/v0.98.4...v0.98.5
[0.98.4]: https://github.com/prestonbrown/helixscreen/compare/v0.98.3...v0.98.4
[0.98.3]: https://github.com/prestonbrown/helixscreen/compare/v0.98.2...v0.98.3
[0.98.2]: https://github.com/prestonbrown/helixscreen/compare/v0.98.1...v0.98.2
[0.98.1]: https://github.com/prestonbrown/helixscreen/compare/v0.98.0...v0.98.1
[0.98.0]: https://github.com/prestonbrown/helixscreen/compare/v0.97.5...v0.98.0
[0.97.5]: https://github.com/prestonbrown/helixscreen/compare/v0.97.4...v0.97.5
[0.97.4]: https://github.com/prestonbrown/helixscreen/compare/v0.97.3...v0.97.4
[0.97.3]: https://github.com/prestonbrown/helixscreen/compare/v0.97.2...v0.97.3
[0.97.2]: https://github.com/prestonbrown/helixscreen/compare/v0.97.1...v0.97.2
[0.97.1]: https://github.com/prestonbrown/helixscreen/compare/v0.97.0...v0.97.1
[0.97.0]: https://github.com/prestonbrown/helixscreen/compare/v0.96.9...v0.97.0
[0.96.9]: https://github.com/prestonbrown/helixscreen/compare/v0.96.8...v0.96.9
[0.96.8]: https://github.com/prestonbrown/helixscreen/compare/v0.96.7...v0.96.8
[0.96.7]: https://github.com/prestonbrown/helixscreen/compare/v0.96.5...v0.96.7
[0.96.5]: https://github.com/prestonbrown/helixscreen/compare/v0.96.4...v0.96.5
[0.96.4]: https://github.com/prestonbrown/helixscreen/compare/v0.96.3...v0.96.4
[0.96.3]: https://github.com/prestonbrown/helixscreen/compare/v0.96.2...v0.96.3
[0.96.2]: https://github.com/prestonbrown/helixscreen/compare/v0.96.1...v0.96.2
[0.96.1]: https://github.com/prestonbrown/helixscreen/compare/v0.96.0...v0.96.1
[0.96.0]: https://github.com/prestonbrown/helixscreen/compare/v0.95.3...v0.96.0
[0.95.3]: https://github.com/prestonbrown/helixscreen/compare/v0.95.2...v0.95.3
[0.95.2]: https://github.com/prestonbrown/helixscreen/compare/v0.95.1...v0.95.2
[0.95.1]: https://github.com/prestonbrown/helixscreen/compare/v0.95.0...v0.95.1
[0.95.0]: https://github.com/prestonbrown/helixscreen/compare/v0.13.13...v0.95.0
[0.13.13]: https://github.com/prestonbrown/helixscreen/compare/v0.13.12...v0.13.13
[0.13.12]: https://github.com/prestonbrown/helixscreen/compare/v0.13.11...v0.13.12
[0.13.11]: https://github.com/prestonbrown/helixscreen/compare/v0.13.10...v0.13.11
[0.13.10]: https://github.com/prestonbrown/helixscreen/compare/v0.13.9...v0.13.10
[0.13.9]: https://github.com/prestonbrown/helixscreen/compare/v0.13.8...v0.13.9
[0.13.8]: https://github.com/prestonbrown/helixscreen/compare/v0.13.7...v0.13.8
[0.13.7]: https://github.com/prestonbrown/helixscreen/compare/v0.13.6...v0.13.7
[0.13.6]: https://github.com/prestonbrown/helixscreen/compare/v0.13.5...v0.13.6
[0.13.5]: https://github.com/prestonbrown/helixscreen/compare/v0.13.4...v0.13.5
[0.13.4]: https://github.com/prestonbrown/helixscreen/compare/v0.13.3...v0.13.4
[0.13.3]: https://github.com/prestonbrown/helixscreen/compare/v0.13.2...v0.13.3
[0.13.2]: https://github.com/prestonbrown/helixscreen/compare/v0.13.1...v0.13.2
[0.13.1]: https://github.com/prestonbrown/helixscreen/compare/v0.13.0...v0.13.1
[0.13.0]: https://github.com/prestonbrown/helixscreen/compare/v0.12.1...v0.13.0
[0.12.1]: https://github.com/prestonbrown/helixscreen/compare/v0.12.0...v0.12.1
[0.12.0]: https://github.com/prestonbrown/helixscreen/compare/v0.11.1...v0.12.0
[0.11.1]: https://github.com/prestonbrown/helixscreen/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/prestonbrown/helixscreen/compare/v0.10.14...v0.11.0
[0.10.14]: https://github.com/prestonbrown/helixscreen/compare/v0.10.13...v0.10.14
[0.10.13]: https://github.com/prestonbrown/helixscreen/compare/v0.10.12...v0.10.13
[0.10.12]: https://github.com/prestonbrown/helixscreen/compare/v0.10.11...v0.10.12
[0.10.11]: https://github.com/prestonbrown/helixscreen/compare/v0.10.10...v0.10.11
[0.10.10]: https://github.com/prestonbrown/helixscreen/compare/v0.10.9...v0.10.10
[0.10.9]: https://github.com/prestonbrown/helixscreen/compare/v0.10.8...v0.10.9
[0.10.8]: https://github.com/prestonbrown/helixscreen/compare/v0.10.7...v0.10.8
[0.10.7]: https://github.com/prestonbrown/helixscreen/compare/v0.10.6...v0.10.7
[0.10.6]: https://github.com/prestonbrown/helixscreen/compare/v0.10.5...v0.10.6
[0.10.5]: https://github.com/prestonbrown/helixscreen/compare/v0.10.4...v0.10.5
[0.10.4]: https://github.com/prestonbrown/helixscreen/compare/v0.10.3...v0.10.4
[0.10.3]: https://github.com/prestonbrown/helixscreen/compare/v0.10.2...v0.10.3
[0.10.2]: https://github.com/prestonbrown/helixscreen/compare/v0.10.1...v0.10.2
[0.10.1]: https://github.com/prestonbrown/helixscreen/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/prestonbrown/helixscreen/compare/v0.9.24...v0.10.0
[0.9.24]: https://github.com/prestonbrown/helixscreen/compare/v0.9.23...v0.9.24
[0.9.23]: https://github.com/prestonbrown/helixscreen/compare/v0.9.22...v0.9.23
[0.9.22]: https://github.com/prestonbrown/helixscreen/compare/v0.9.21...v0.9.22
[0.9.21]: https://github.com/prestonbrown/helixscreen/compare/v0.9.20...v0.9.21
[0.9.20]: https://github.com/prestonbrown/helixscreen/compare/v0.9.19...v0.9.20
[0.9.19]: https://github.com/prestonbrown/helixscreen/compare/v0.9.18...v0.9.19
[0.9.18]: https://github.com/prestonbrown/helixscreen/compare/v0.9.17...v0.9.18
[0.9.17]: https://github.com/prestonbrown/helixscreen/compare/v0.9.16...v0.9.17
[0.9.16]: https://github.com/prestonbrown/helixscreen/compare/v0.9.15...v0.9.16
[0.9.15]: https://github.com/prestonbrown/helixscreen/compare/v0.9.14...v0.9.15
[0.9.14]: https://github.com/prestonbrown/helixscreen/compare/v0.9.13...v0.9.14
[0.9.13]: https://github.com/prestonbrown/helixscreen/compare/v0.9.12...v0.9.13
[0.9.12]: https://github.com/prestonbrown/helixscreen/compare/v0.9.11...v0.9.12
[0.9.11]: https://github.com/prestonbrown/helixscreen/compare/v0.9.10...v0.9.11
[0.9.10]: https://github.com/prestonbrown/helixscreen/compare/v0.9.9...v0.9.10
[0.9.9]: https://github.com/prestonbrown/helixscreen/compare/v0.9.8...v0.9.9
[0.9.8]: https://github.com/prestonbrown/helixscreen/compare/v0.9.7...v0.9.8
[0.9.7]: https://github.com/prestonbrown/helixscreen/compare/v0.9.6...v0.9.7
[0.9.6]: https://github.com/prestonbrown/helixscreen/compare/v0.9.5...v0.9.6
[0.9.5]: https://github.com/prestonbrown/helixscreen/compare/v0.9.4...v0.9.5
[0.9.4]: https://github.com/prestonbrown/helixscreen/compare/v0.9.3...v0.9.4
[0.9.3]: https://github.com/prestonbrown/helixscreen/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/prestonbrown/helixscreen/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/prestonbrown/helixscreen/releases/tag/v0.9.1
