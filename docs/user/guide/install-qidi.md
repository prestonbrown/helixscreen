# Installing HelixScreen on QIDI Printers

> **Status:** updating a QIDI that already runs HelixScreen is the tested path. A first install uses the same steps and is still being tested.

How to put HelixScreen on the built-in screen of a QIDI Q2, Max 4 or Plus 5, with the [HelixScreen easy installer](https://helixscreen.org/installer/) or from the command line. HelixScreen replaces QIDI's own screen program.

> **Which QIDI models?** On-device install works on the **Q2**, **Max 4** and **Plus 5**, whose screens are driven by Linux. The Plus 4 and the 3-series (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro) use a serial TJC display HelixScreen can't drive; they can still be [controlled from another screen](easy-installer-unsupported.md#qidi-with-a-tjc-screen).

## Read This First: QIDI Studio and the Cloud

While HelixScreen runs, QIDI's screen program is off, so QIDI Studio box sync, QIDI cloud and Box filament edits from QIDI's apps stop working. Prints can still be sent through Moonraker, Fluidd or Mainsail. Removing HelixScreen brings them back; the full list is in [Supported Printers](supported-printers.md#qidi-plus-4--q2--plus-5--max-4). The easy installer shows this notice before it changes anything.

## With the Easy Installer

1. Switch the printer on and make sure it is on the same network as your computer.
2. Open the installer and pick your printer from the list. If it isn't listed, choose **My printer isn't listed**, pick the QIDI model and type the printer's address.
3. The installer signs in over SSH with the QIDI logins it knows (`mks` with password `makerbase` on most printers) and asks you if none works.
4. It checks everything first and changes nothing until those checks pass, then installs HelixScreen and waits up to five minutes for it to start on the printer's screen.

**The sudo password.** QIDI logins are not root, so the install needs `sudo`. If the account can use sudo without a password, nothing is asked. Otherwise the installer gives sudo the password you signed in with. If you signed in with an SSH key, it asks for the account's password on a screen titled "Your printer needs your password", checks it with sudo before uploading anything, and lets you try three times. The password is used for this install only and is never saved.

## From the Command Line

Sign in over SSH with the printer's login (for example `ssh mks@<printer-ip>`) and run the one-line installer:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

It asks for the account's sudo password when it needs it.

### What the installer does

- Recognizes the QIDI board and installs the 64-bit Raspberry Pi-class build, as a systemd service named `helixscreen`.
- Turns QIDI's screen program off before stopping it, and keeps it off at every boot, so it does not come back after a restart. It sets aside only the QIDI screen software actually present, and puts back only that when you uninstall.
- Where the printer's Moonraker does not make thumbnails for `.3mf` files itself, takes over that job from QIDI's screen program so print files still show previews.

---

## Getting Started on a QIDI

HelixScreen appears on the printer's screen when the install finishes, and the setup wizard runs:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen looks for the printer's own Moonraker and fills it in. The address `127.0.0.1` and port `7125` are right; tap **Test Connection**, then continue.
4. **Printer Identification:** confirm your QIDI model. HelixScreen then fills in the heaters, fans and sensors for you and skips those steps.
5. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
6. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Load filament from HelixScreen.** QIDI's load macro is only the first step of a guided load, and HelixScreen runs the whole thing; see [Loading on a QIDI](filament.md#loading-on-a-qidi-m604).
- **Check the QIDI Box**, if you have one, on the Filament screen; see [Supported Printers: QIDI](supported-printers.md#qidi-plus-4--q2--plus-5--max-4).
- **Check the version** in **Settings > Help & About > About**, and pick your update channel in **Settings > Updates**.

---

## Updating

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **With the easy installer:** run it again. When HelixScreen is already on the printer it offers an update on the channel you are on, and never installs an older version over a newer one.
- **From the command line:** run the one-line installer with `--update`: `curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update`.

## Removing HelixScreen

The easy installer can't remove HelixScreen yet. From an SSH session on the printer:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall
```

This removes HelixScreen and brings QIDI's screen program back, so Studio box sync and the cloud work again. See [Uninstalling](../INSTALL.md#uninstalling) for details.

## If HelixScreen Doesn't Appear

Wait a few minutes; a first start can be slow. If the screen stays on QIDI's interface, collect the log over SSH with `journalctl -u helixscreen -n 100` and ask in [Discord](https://discord.gg/RZCT2StKhr) or on [GitHub](https://github.com/prestonbrown/helixscreen/issues).

## See Also

- [Supported Printers: QIDI](supported-printers.md#qidi-plus-4--q2--plus-5--max-4): what HelixScreen does with the QIDI Box
- [Remote screen setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device): for models that can't run it on the printer
