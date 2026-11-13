# Installing HelixScreen on the BTT K-Touch (ESP32) - Alpha

The BigTreeTech K-Touch is a small standalone touchscreen built around an ESP32-S3 chip. HelixScreen runs on it as firmware: the panel joins your WiFi and talks to your printer's Moonraker, like any other [remote screen](../INSTALL.md#remote-screen-setup-run-on-a-separate-device). Nothing is installed on the printer.

> **Support status in 1.1: alpha.** The K-Touch firmware is built with every 1.1 release, beta or stable, but it is an early port: expect rough edges and missing features (see [What doesn't work yet](#what-doesnt-work-yet-on-the-k-touch)). Because it is an alpha, a release whose firmware build fails ships without it; if a release has no K-Touch zip, use the one before or after it. The 1.0 releases have no K-Touch firmware. Flashing replaces BigTreeTech's stock firmware on the panel.

## Getting started

1. Open [helixscreen.org/flash](https://helixscreen.org/flash/) in Chrome or Edge on a desktop computer.
2. Plug in the K-Touch with a USB-C cable that carries data, and switch it on.
3. Click **Install HelixScreen**, pick the panel's port (usually named USB Serial or CH340) and tick **Erase device** for a first install over BigTreeTech's firmware.
4. When the panel restarts, join its `HelixScreen-XXXX` WiFi hotspot from your phone and enter your WiFi network and your printer's address, as described in [First boot](#first-boot-wifi-and-printer-setup).
5. Optional: install the [HelixPrint plugin](#install-the-helixprint-plugin-on-your-printer) on your printer for thumbnails and filament checks.

To update later, flash again from the same page with **Erase device** left unticked; that keeps the panel's WiFi and settings.

The rest of this page covers the same install from the easy installer or a terminal, first-boot setup, updating, recovery and other ESP32-S3 panels.

## With the easy installer

The [HelixScreen easy installer](https://helixscreen.org/installer/) is a desktop app (Windows, macOS, Linux) that can flash the K-Touch over USB, with no Python, esptool or browser needed. It installs the newest beta build. If your copy of the easy installer does not list the K-Touch, use the [browser flasher](#getting-started) instead.

1. Plug the K-Touch into your computer with a USB-C data cable and switch it on.
2. Open the installer. A K-Touch plugged in by USB is listed alongside the printers it finds on your network. Pick it.
3. The installer reads the chip before writing anything and only goes ahead on an ESP32-S3 with 16 MB of flash, which is what the K-Touch has. Any other USB device is left untouched.
4. A first install over BigTreeTech's firmware erases the panel first, including its WiFi and settings. Keep it plugged in until the installer says it is done.
5. The panel restarts into HelixScreen and finishes on its own setup screen, not in the installer. Continue at [First boot](#first-boot-wifi-and-printer-setup).

**Updating:** plug the panel in and run the installer again. An update writes the new firmware and keeps the panel's WiFi network and settings. The installer will not install an older version over a newer one. A panel that is not running HelixScreen is treated as a fresh install and erased first.

**USB drivers:**

- **Linux:** your user needs permission to use the serial port, normally through the `dialout` group. Run `sudo usermod -aG dialout $USER`, then log out and back in. The panel shows up as `/dev/ttyUSB0`.
- **Windows:** if no new COM port appears when you plug the panel in, install the CH340 USB driver from WCH, then plug it in again.
- **macOS:** the driver is built in.

If the panel is not listed, try another cable (some only charge). If the panel was already misbehaving, see [Recovery](#recovery).

## From a terminal (esptool)

**What you need:**

- A BigTreeTech K-Touch
- A USB-C cable that carries data
- A computer with Python 3 and esptool, installed with `pipx install esptool` (or `pip install esptool` inside a virtualenv)
- A driver for the K-Touch's USB chip, a CH340. Linux has it built in and the panel shows up as `/dev/ttyUSB0`. On Windows, if no new COM port appears when you plug the panel in, install the CH340 driver from WCH. Recent macOS versions include one; the panel appears as `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*`

**Download.** On the [releases page](https://github.com/prestonbrown/helixscreen/releases), pick a 1.1 release or newer and download `helixscreen-esp32-ktouch-v<VERSION>.zip`. Unzip it and open a terminal inside the folder it creates. The commands below write `PORT` for the panel's port: `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-...` on macOS, or a COM port such as `COM3` on Windows (Device Manager lists it under **Ports**).

**Optional: back up the stock firmware first.** This saves the whole 16 MB flash so you can put BigTreeTech's firmware back later. It takes a few minutes. Keep the speed at 460800 or lower: the CH340 drops bytes at 921600.

```bash
python3 -m esptool --chip esp32s3 -p PORT -b 460800 read_flash 0 0x1000000 ktouch-stock-backup.bin
```

**Install.** Plug the K-Touch in, switch it on, and write the factory image:

```bash
python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 helixscreen-esp32-ktouch-factory.bin
```

If the write fails part way, run the same command again with `-b 115200`. Newer esptool versions print "Deprecated" warnings about `write_flash`; they are harmless. When it finishes, the panel restarts into HelixScreen.

## First boot: WiFi and printer setup

The K-Touch has no setup wizard. When it has no WiFi network saved, it creates its own WiFi hotspot and shows a **Set Up WiFi** message with the hotspot's name (`HelixScreen-` plus four characters, such as `HelixScreen-8CFC`) and a web address.

1. On your phone or computer, join the `HelixScreen-XXXX` network. It has no password.
2. Open the address shown on the panel (normally `http://192.168.4.1`). Many phones open the setup page on their own as soon as they join.
3. Fill in the form:
   - **WiFi network:** pick your network from the list or type its name
   - **WiFi password**
   - **Moonraker host:** your printer's IP address, for example `192.168.1.50`
   - **Moonraker port:** leave it at `7125` unless you changed it
4. Tap **Connect**. The panel joins your network, the hotspot shuts down, and HelixScreen connects to your printer. The panel also takes its time zone from your phone or computer.

If the panel cannot join, the page says why (a wrong password, for example) and you can try again. The hotspot closes on its own after 10 minutes; restart the panel to bring it back.

Prefer to set it up on the panel itself? Tap **Use Settings Instead**, join your network in **Settings > Connection > Network Settings**, and enter your printer's address in **Settings > Connection > Host**. The Host dialog lists the Moonraker printers the panel finds on your network, so you can usually pick yours instead of typing it. That is also where you change either one later.

## Updating

The K-Touch does not update itself over WiFi and never offers an update on screen. Update it over USB, from the [browser flasher](https://helixscreen.org/flash/) with **Erase device** unticked, from the [easy installer](#with-the-easy-installer), or from a terminal: download the new zip, unzip it, and from inside that folder run

```bash
python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash @flash_args
```

This writes the new firmware and keeps your WiFi network and settings. Run it from inside the unzipped folder, because `flash_args` lists the other files by name.

The factory-image command from [From a terminal](#from-a-terminal-esptool) also works for an update, but the panel forgets its WiFi network, so you go through the hotspot setup again. Your other settings are kept either way.

## Recovery

If the panel will not start, or keeps restarting, erase it completely and install fresh:

```bash
python3 -m esptool --chip esp32s3 -p PORT erase_flash
python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 helixscreen-esp32-ktouch-factory.bin
```

Erasing removes everything, including your settings and WiFi, so you set the panel up again from the hotspot.

To go back to BigTreeTech's firmware, write the backup you made:

```bash
python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 ktouch-stock-backup.bin
```

## Install the HelixPrint plugin on your printer

The K-Touch is too small to read whole G-code files itself. With the HelixPrint plugin installed on the printer, the printer reads each file once after upload, and the K-Touch shows:

- a thumbnail for files your slicer saved without one
- the filament amount and color for each tool, which the checks before a print use

To install it, open **Advanced** on the K-Touch and tap the plugin's **Install** row. The K-Touch is not attached to the printer, so it shows a one-line command instead; run that command on the printer over SSH. [HelixPrint Plugin](helixprint-plugin.md) covers everything else the plugin adds.

Without the plugin, those files show no thumbnail and the per-tool filament checks have nothing to go on. Nothing else on the K-Touch needs the plugin.

## What doesn't work yet on the K-Touch

- The camera feed and QR code features
- The 2D G-code view and the 3D bed mesh view
- Chinese and Japanese (the panel has no room for their font)
- Sounds, screensavers, label printers, belt tuning, the timelapse viewer, and screen plugins (the ones listed under Settings > Plugins on other screens)
- Updating over WiFi

What does work is listed under [ESP32-S3 Firmware](beta-features.md#esp32-s3-firmware-alpha) in the beta features guide.

## Other ESP32-S3 panels

Only the K-Touch is tested and supported. The firmware will not run on a different panel as it is, even one with the same chip, because the screen and touch wiring differ from board to board.

A panel is a candidate for a port if it has:

- an ESP32-S3 with 16 MB of flash and 8 MB of octal PSRAM
- an 800x480 screen driven over a 16-bit RGB parallel interface (RGB565)
- a GT911 touch controller on I2C

Bringing one up means describing its pins and screen timings in a new board file next to the K-Touch's and building from source. Developers can start in the [firmware directory](https://github.com/prestonbrown/helixscreen/tree/main/firmware/helixscreen-esp32).

## See also

- [Beta Features: ESP32-S3 Firmware](beta-features.md#esp32-s3-firmware-alpha): what the alpha includes
- [Installation overview](../INSTALL.md): all other platforms
