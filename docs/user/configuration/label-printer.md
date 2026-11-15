# Label Printer

The thermal label printer used to print filament spool labels. These keys are **per screen** and live in the `label_printer` section. Scanning and picking a printer at **Settings > Devices > Spoolman > Label Printer** fills them in for you, so you rarely edit them by hand. The [Label Printing guide](../guide/label-printing.md) has the full walkthrough.

[Back to the Configuration Reference](../CONFIGURATION.md)

```json
"label_printer": {
  "type": "network",
  "address": "",
  "port": 9100,
  "protocol": "raw",
  "label_size": 0,
  "preset": 0,
  "label_count": 1,
  "usb_vid": 0,
  "usb_pid": 0,
  "usb_serial": "",
  "bt_address": "",
  "bt_name": "",
  "bt_transport": "spp"
}
```

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `type` | `"network"` | `"network"`, `"usb"`, `"bluetooth"` | How the printer connects: network (a Brother QL over Ethernet or Wi-Fi), USB (a Phomemo over a cable), or Bluetooth (any supported printer paired over Bluetooth). Bluetooth is offered only when your device has Bluetooth hardware. **Type**. |
| `address` | `""` | | The IP address or host name of a network label printer. Used only when `type` is `"network"`. |
| `port` | `9100` | | The TCP port of a network label printer. 9100 is the standard raw (JetDirect) port that Brother QL printers use. Used only when `type` is `"network"`. |
| `protocol` | `"raw"` | `"raw"`, `"ipp"` | The network print protocol. `raw` works for most printers, and `ipp` is used by some networked models. |
| `label_size` | `0` | | The index of the label or tape size for your printer. `0` is the model's default size. The sizes depend on the detected printer (a Niimbot D11 offers different sizes from a B21), so pick yours from **Label Size** rather than guessing an index. |
| `preset` | `0` | `0` Standard, `1` Compact, `2` QR Only | What is printed on the label. Standard is the full label with spool name, material, colour, temperatures and a QR code. For small square labels only QR Only is available, and the menu is locked to it. **Preset**. |
| `label_count` | `1` | | How many copies to print for each label job. **Labels Per Sheet**. |
| `usb_vid`, `usb_pid`, `usb_serial` | `0`, `0`, `""` | | The USB vendor id, product id and serial number of a USB label printer. `0` and an empty serial mean auto-detect, so you normally never set these. Used only when `type` is `"usb"`. |
| `bt_address`, `bt_name`, `bt_transport` | `""`, `""`, `"spp"` | `bt_transport`: `"spp"` or `"ble"` | The Bluetooth address, advertised name and transport of a Bluetooth label printer. `spp` (Bluetooth Classic) is used by Brother PT, Phomemo and MakeID. `ble` (Bluetooth Low Energy) is used by Niimbot. They are filled in when you scan and pick a printer. Used only when `type` is `"bluetooth"`. |
