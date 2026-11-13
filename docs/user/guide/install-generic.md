# Installing HelixScreen on a Klipper Host (Raspberry Pi and Similar)

This page is for printers whose Klipper runs on a separate computer with its own touchscreen: a Raspberry Pi, a BTT CB1 or CB2, an Orange Pi, a Radxa board and the like. That covers Voron, RatRig, Sovol and most self-built printers. You can install from a terminal on the host, or with the [HelixScreen easy installer](https://helixscreen.org/installer/) from your computer.

## Getting started

1. Make sure Klipper and Moonraker already work on the host (MainsailOS, FluiddPi and KIAUH setups all qualify), and that the host runs Debian 11 (Bullseye) or newer.
2. Sign in to the host over SSH, for example `ssh pi@mainsailos.local`.
3. Run the installer:
   ```bash
   curl -sSL https://releases.helixscreen.org/install.sh | sh
   ```
   It picks the right build for your board, shows its plan and asks `Continue? [Y/n]`. It stops KlipperScreen or any other screen program it finds, installs HelixScreen, and starts it.
4. Finish the setup wizard on the touchscreen: language, network, your printer's connection, then its heaters, fans, lights and sensors. The wizard steps are described in [First Boot & Setup Wizard](../INSTALL.md#first-boot--setup-wizard).
5. If the picture is rotated or the touch is off, see [Display Configuration](../INSTALL.md#display-configuration) and [Touch Calibration](touch-calibration.md).

The full command-line guide, with every option, is [Generic Linux Install](../INSTALL.md#generic-linux-install-raspberry-pi-btt-x86). The rest of this page covers the easy installer.

## The easy installer

> **Status:** updating a Klipper host that already runs HelixScreen is the tested path today. A first install uses the same steps and is still being tested.

The easy installer is a desktop app for Windows, macOS and Linux. It finds the host on your network, signs in over SSH and installs or updates HelixScreen for you.

### What it needs

- A host running Klipper and Moonraker, switched on and on the same network as your computer
- SSH access to it, with a login that can use `sudo`
- A 64-bit (`aarch64`) or 32-bit (`armv7l`) ARM board. The installer picks the matching build for you
- A board the installer recognizes: Raspberry Pi, BigTreeTech CB1/CB2, Orange Pi, Radxa or Rock boards. It does not guess at a board it does not know; it stops and points you to the command-line guide

### Signing in

There is no factory password on a Klipper host, so the installer always asks for a login.

**With a password:** type the user name and password. You get three tries.

**With an SSH key:** if the installer finds a key on your computer, a "Use my SSH key instead of a password" box appears. It tries, in this order:

1. Keys held by your SSH agent (the OpenSSH agent on Windows, `ssh-agent` or your desktop's agent on macOS and Linux)
2. The key files in your `.ssh` folder, in the order `id_ed25519`, `id_ecdsa`, `id_rsa`

If a key file is protected by a passphrase, a "Key passphrase" field appears. Type the passphrase you chose when you made the key, or leave it empty if the key has none. A key held in your agent is already unlocked and needs no passphrase.

Keys with other file names are not read. Add them to your agent first (`ssh-add ~/.ssh/my_key`) and the installer uses them.

The password and passphrase are used for this install only and are never saved to disk, to the logs or to the support bundle.

### The sudo password

Installing needs root. If you sign in as `root`, or the account has passwordless sudo, nothing more is asked. Otherwise:

- Signed in with a **password:** the installer gives that password to sudo without asking again. If sudo rejects it (the account's sudo password differs from its login password), the install stops; sign in with an SSH key instead and the installer asks for the sudo password separately.
- Signed in with an **SSH key:** the installer has no password to give, so it asks for the account's password on a screen titled "Your printer needs your password". It checks the password with sudo before anything is uploaded and asks again if sudo refuses it, three tries in all.

If sudo cannot be used at all, the installer tells you to sign in as root or give the account sudo rights, or to use the [command-line install](../INSTALL.md#generic-linux-install-raspberry-pi-btt-x86).

### Installing and updating

On a host without HelixScreen, the easy installer installs the newest stable release. The setup wizard then appears on the touchscreen.

On a host that already has HelixScreen, it updates instead:

- It never installs an older version over a newer one, including a development build newer than any release.
- It follows the update channel you are on. A stable install is offered stable releases. If a prerelease is installed, it is offered the newest of beta and stable, so a beta tester reaches the stable release once it is out.
- It reads the installed version from the running HelixScreen. If it cannot tell what is installed, it refuses to update and tells you to reinstall following the guide, because it could otherwise put an older release over a newer one.
- When nothing newer exists, it says HelixScreen is up to date and changes nothing.

## If it stops

| What it says | What to do |
|--------------|------------|
| It does not recognize this computer's board | Use the [command-line install](../INSTALL.md#generic-linux-install-raspberry-pi-btt-x86) |
| There is no HelixScreen build for this CPU | The command-line guide lists what is supported |
| HelixScreen does not appear on the screen | Give it five minutes, then run `sudo systemctl status helixscreen` and `sudo journalctl -u helixscreen -n 100` on the host |

## See also

- [Installation guide](../INSTALL.md): the command-line steps, display configuration and the setup wizard
- [Remote screen setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device): a screen device that drives a printer somewhere else
- [Run HelixScreen in a window on a Linux desktop](install-linux-desktop.md): the app on your desktop PC instead of a touchscreen
