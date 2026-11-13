# Barcode Scanner Setup

HelixScreen can read the QR code on a Spoolman spool label from a USB or Bluetooth barcode scanner, so you can pick a spool by scanning it instead of choosing from a list. You print those labels from HelixScreen itself; see [Label Printing](label-printing.md).

Scanning is a shortcut for assigning spools. See [Filament Tracking & Spoolman](filament-tracking.md) for what happens once a spool is linked.

HelixScreen only acts on Spoolman spool QR codes. Any other barcode you scan is ignored.

![Barcode Scanner Settings](../../images/user/settings-barcode-scanner.png)

The settings live at **Settings > Devices > Spoolman > Barcode Scanner**. They are not available on the BigTreeTech K-Touch (ESP32) screen.

---

## Where scanning happens

A scan only counts while the **Scan Spool QR Code** screen is open. Open it from **Scan QR Code** in a filament slot's menu, from the QR scan button in the slot editor's header, or from the external spool's menu. Outside that screen a scanner is just a keyboard, so what you scan is typed into whatever field is focused.

If no scanner is set up, or the one you chose is not plugged in, the screen uses the printer's webcam instead ("Point camera at QR code on spool"). **On a printer whose bed moves in Z, opening this screen first moves Z to 150 mm when the printer is homed and lower than that, to make room for the camera view. Keep the bed and the area below it clear.** With no webcam it says "No camera" and asks you to use a USB barcode scanner. When your chosen scanner is attached, the screen says "Scan the spool label with your barcode scanner" and skips the camera. The screen closes by itself after 60 seconds without a scan.

---

## USB Scanners

Most USB barcode scanners present themselves as a plain keyboard, so no driver is needed.

1. Plug the scanner into any USB port.
2. Open **Settings > Devices > Spoolman > Barcode Scanner**.
3. Under **USB Devices**, tap your scanner to select it (a check mark shows the choice, and **Selected** at the top names it). Tap **Refresh** if it is not listed. **Auto-detect** ("Use first HID scanner found") is the default and picks a keyboard-like device for you, but choosing your scanner by name is more reliable.
4. Open the scan screen as described above and scan a Spoolman label.

### Keyboard Layout

A scanner sends key codes for the keyboard layout it was programmed with, and HelixScreen cannot tell which that is. If scanned text comes out garbled, set **Layout** under **Keyboard Layout** to the layout the scanner uses:

- **US QWERTY** (the default, right for most scanners)
- **German QWERTZ**
- **French AZERTY**

The change takes effect straight away. The app's display language does not change it.

---

## Bluetooth Scanners

HelixScreen supports Bluetooth scanners that pair as a keyboard-like (HID) device. The **Bluetooth Scanners** section appears only when the device running HelixScreen has Bluetooth. If you do not see it, see [Bluetooth Setup](bluetooth-setup.md).

### Pairing

1. Power on the scanner and put it in Classic Bluetooth pairing mode (not BLE). The scanner's manual has a setup barcode for this; most scanners default to Classic.
2. Under **Bluetooth Scanners**, tap **Scan**.
3. Choose your scanner in the **Scanner** dropdown, then tap **Pair** and confirm **Pair with** the scanner's name.
4. When the keyboard connection comes up you see **Paired successfully** and the scanner becomes the selected one. Already paired scanners show "(paired)" in the dropdown; choosing one selects it. **Forget** removes the pairing.

### Pairing warnings

**"Pairing did not complete. The scanner did not bond."** The scanner was not in pairing mode when you tapped **Pair**. Turn it off and on, then tap **Pair** again.

**"Scanner bonded but didn't attach as a keyboard."** The scanner paired, but the Bluetooth system refused the keyboard connection. Turn the scanner off and on and tap **Pair** again. If it still fails, relax the Bluetooth keyboard policy on the device. Many inexpensive scanners cannot meet the default one. You need SSH access.

1. Connect:
   ```bash
   ssh pi@helixpi.local
   ```
2. Edit the Bluetooth input config:
   ```bash
   sudo nano /etc/bluetooth/input.conf
   ```
3. Find the line `#ClassicBondedOnly=true`, remove the `#` and set it to:
   ```
   ClassicBondedOnly=false
   ```
4. Save, then restart Bluetooth:
   ```bash
   sudo systemctl restart bluetooth
   ```
5. In HelixScreen, tap **Forget** for the scanner and pair it again.

> **Security note:** this setting lets any unbonded keyboard-type Bluetooth device connect, which slightly widens the risk of someone injecting keystrokes. That only matters in a hostile radio environment. For a home or workshop it is generally fine.

### BLE mode

Some scanners also have a BLE mode, which always bonds, so the warning above does not apply. Not every scanner advertises reliably in BLE, so if Classic works, stay with it.

---

## Sharing a scanner with another tool (device blacklist)

If another program on the same device already reads your scanner, for example [afc-spool-scan](https://github.com/AFCProject/afc-spool-scan), HelixScreen can get in its way. A USB scanner looks like a keyboard, and HelixScreen claims keyboard-like devices it finds, so both programs end up competing for it.

You can tell HelixScreen to ignore a specific device by adding its USB ID to a blacklist. A blacklisted device is skipped entirely, for keyboard input and for the scan screen. It still appears in the USB Devices list so you can identify it.

### 1. Find the scanner's USB ID

The ID is a `vendor:product` pair such as `002c:261a`. Find it either way:

- **In HelixScreen:** open **Settings > Devices > Spoolman > Barcode Scanner**. Each entry under **USB Devices** shows its ID.
- **Over SSH:** run `lsusb` and read the pair after `ID`, for example `Bus 001 Device 005: ID 002c:261a ...`.

### 2. Add it to your settings

Edit `~/helixscreen/config/settings.json` (the path varies by platform; see the table in [Configuration](../CONFIGURATION.md#configuration-file-location)) and add the ID to the `input` section, in lowercase:

```json
"input": {
  "device_blacklist": ["002c:261a"]
}
```

List as many devices as you need: `["002c:261a", "1a2c:4c5e"]`.

### 3. Restart HelixScreen

```bash
sudo systemctl restart helixscreen
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No **Bluetooth Scanners** section | The device has no Bluetooth, or it is disabled. See [Bluetooth Setup](bluetooth-setup.md). |
| The scanner dropdown shows "(none)" | Tap **Scan** with the scanner in pairing mode. |
| "Pairing failed" | Power-cycle the scanner, put it in Classic pairing mode and try again. |
| "Pairing did not complete" | The scanner was not in pairing mode. Turn it off and on, then tap **Pair** again. |
| "Scanner bonded but didn't attach as a keyboard" | See [Pairing warnings](#pairing-warnings). |
| Scanned text appears in a text field instead of selecting a spool | Open the **Scan Spool QR Code** screen before scanning. |
| Scanned text has wrong characters | Set the right **Layout** under **Keyboard Layout**. |
| The scan screen shows the camera although a scanner is plugged in | Select the scanner by name under **USB Devices** (or pair it); **Auto-detect** does not count as a chosen scanner. |
| Another tool stopped seeing the scanner after HelixScreen was installed | Blacklist the device; see [Sharing a scanner with another tool](#sharing-a-scanner-with-another-tool-device-blacklist). |

---

## Notes

- One scanner is used at a time. If a Bluetooth scanner is paired and present, it wins over a USB one.
- A selected Bluetooth scanner is captured exclusively, so its keystrokes do not leak into other fields. USB scanners are read passively, because some depend on that. If a USB scanner types into a focused text field, that is a known limitation.

---

**Next:** [Calibration & Tuning](calibration.md) | **Prev:** [Label Printing](label-printing.md) | [Back to User Guide](../USER_GUIDE.md)
