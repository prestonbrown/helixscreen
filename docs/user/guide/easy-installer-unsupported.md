# Printers the Easy Installer Doesn't Handle Yet

The [HelixScreen easy installer](https://helixscreen.org/installer/) recognizes the printers below but has no install recipe for them, so it stops with "This printer can't be set up automatically" and sends you here. It does this on purpose: when it isn't sure it has the right files for a printer, it changes nothing. Nothing on your printer was touched.

Each section says what works today.

## Snapmaker U1

Install by hand. The U1 needs SSH access, either from the **Root access** option on stock firmware 1.2 and newer or from PAXX Extended Firmware. The steps, firmware-upgrade notes and recovery are in the [Snapmaker U1 install guide](install-u1.md). Afterwards, HelixScreen updates from its own screen (**Settings > Updates**). If HelixScreen came with PAXX firmware as a firmware component, the firmware updates it instead.

## QIDI with a TJC screen

This covers the X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro and Plus 4: their serial TJC display can't be driven by HelixScreen (see [Supported Printers](supported-printers.md#qidi-plus-4--q2--plus-5--max-4)). Use HelixScreen as a **remote screen** on a separate touchscreen device pointed at the printer's Moonraker: [Remote screen setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device). The Q2, Max 4 and Plus 5 run it on-device; see [QIDI](install-qidi.md).

## Sovol

The easy installer has no recipe for the Sovol SV08. HelixScreen supports it; follow the [Installation guide](../INSTALL.md) and the printer's entry in [Supported Printers](supported-printers.md), or run HelixScreen as a [remote screen](../INSTALL.md#remote-screen-setup-run-on-a-separate-device).

## Elegoo Neptune 4

Not installable on-device by the easy installer. HelixScreen works with the Neptune 4 as a [remote screen](../INSTALL.md#remote-screen-setup-run-on-a-separate-device) on a separate touchscreen device pointed at the printer's Moonraker.

## Elegoo Centauri Carbon

The installer recognizes a Centauri Carbon running the OpenCentauri COSMOS firmware but has no recipe for it yet. Install by hand: the [Centauri Carbon install guide](install-cc1.md) covers flashing COSMOS first and then installing HelixScreen. Stock Elegoo firmware can't run HelixScreen.

## FlashForge Creator 5

Z-Mod and Reforge, the two replacement firmwares for the Creator 5 and Creator 5 Pro, both bring HelixScreen with them. Z-Mod installs and updates it itself (see the [Z-Mod wiki](https://wiki.zmod.link/C5PRO/)); on [Reforge](https://github.com/Klipper4FlashForge/firmware) it ships with the firmware.

## Anycubic Kobra

The Kobra needs a community firmware such as [Rinkhals](https://github.com/jbatonnet/Rinkhals) that provides Moonraker. With that in place HelixScreen can run as a [remote screen](../INSTALL.md#remote-screen-setup-run-on-a-separate-device) or on-device through the community firmware. See [Anycubic Kobra and ACE Pro](supported-printers.md#anycubic-kobra--ace-pro-and-sovol-sv06-ace) for what's supported.

## Something Else

Any Klipper printer can run HelixScreen as a remote screen, and a printer whose Klipper host is a Raspberry Pi or similar board can run it on that host: see [Installing on a Klipper host](install-generic.md). The full list is in [Supported Printers](supported-printers.md).
