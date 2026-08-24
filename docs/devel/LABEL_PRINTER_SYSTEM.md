# Label Printer System

HelixScreen supports printing spool labels to thermal label printers via three transports (USB, Network, Bluetooth) and six printer protocol families (Brother QL, Brother PT, Phomemo, Niimbot, MakeID/Wewin, IPP). The IPP backend also supports printing on sheets of Avery-style labels using standard inkjet/laser printers.

## Architecture Overview

```
┌──────────────────────────────┐
│     print_spool_label()      │  Entry point (label_printer_utils.cpp)
│  Selects transport + protocol│
└──────────┬───────────────────┘
           │
    ┌──────┴──────┬──────────────┬─────────────────┬─────────────────┐
    │             │              │                  │                 │
┌───┴────┐  ┌────┴─────┐  ┌────┴──────┐    ┌──────┴──────┐  ┌──────┴──────┐
│ Brother│  │ Phomemo  │  │ Phomemo   │    │  Niimbot    │  │  MakeID     │
│ QL Net │  │ USB      │  │ BT (SPP/  │    │  BT (BLE)   │  │  MakeID     │
│ (TCP)  │  │ (libusb) │  │  BLE)     │    │             │  │  BT (RFCOMM)│
└───┬────┘  └────┬─────┘  └────┬──────┘    └──────┬──────┘  └──────┬──────┘
    │            │              │                   │                │
    │  brother_ql_build_raster  │  phomemo_build_   │ niimbot_build_ │ makeid_build_
    │            │              │  raster            │ print_job      │ print_job
    │            │              │                    │                │
    └────────────┴──────────────┴────────────────────┴────────────────┘
              ILabelPrinter interface
```

## Key Files

| File | Purpose |
|------|---------|
| `include/label_printer.h` | `ILabelPrinter` interface, `LabelSize`, `LabelPreset`, `PrintCallback` |
| `include/label_bitmap.h` | `LabelBitmap` — 1bpp monochrome bitmap (MSB first, black=1) |
| `src/system/label_printer_utils.cpp` | `print_spool_label()` — transport routing and printer dispatch |
| `src/system/label_renderer.cpp` | `LabelRenderer::render()` — renders spool info to LabelBitmap |
| `include/label_printer_settings.h` | `LabelPrinterSettingsManager` — persisted config |

### Protocol Implementations

| File | Protocol |
|------|----------|
| `include/brother_ql_protocol.h` | Brother QL ESC/P raster building |
| `include/phomemo_protocol.h` | Phomemo ESC/POS raster building |
| `include/niimbot_protocol.h` | Niimbot custom BLE packet building |
| `src/system/niimbot_protocol.cpp` | Niimbot packet framing, row encoding, print job builder |
| `include/makeid_protocol.h` | MakeID/Wewin 0x66 protocol: framing, bitmap encode, LZO compress |
| `src/system/makeid_protocol.cpp` | MakeID frame building, response parsing, print job builder |
| `include/ipp_protocol.h` | IPP binary encoding/decoding (operations, attributes, responses) |
| `src/system/ipp_protocol.cpp` | IPP 2.0 message format, request builder, response parser |
| `include/pwg_raster.h` | PWG Raster format generation (PackBits compression) |
| `src/system/pwg_raster.cpp` | PWG page header, 1bpp→8bpp conversion, scanline compression |
| `include/sheet_label_layout.h` | Sheet label template database + tiling engine |
| `src/system/sheet_label_layout.cpp` | 10 Avery templates (A4 + Letter), page layout math |

### Transport Backends

| File | Transport |
|------|-----------|
| `src/system/brother_ql_printer.cpp` | Brother QL over TCP (network) |
| `src/system/phomemo_printer.cpp` | Phomemo over USB (libusb) |
| `src/system/brother_ql_bt_printer.cpp` | Brother QL over BT Classic (RFCOMM) |
| `src/system/phomemo_bt_printer.cpp` | Phomemo over BT Classic (RFCOMM) or BLE GATT |
| `src/system/niimbot_bt_printer.cpp` | Niimbot over BLE GATT |
| `src/system/makeid_bt_printer.cpp` | MakeID over BT Classic (RFCOMM) |
| `src/system/ipp_printer.cpp` | IPP over HTTP POST (libhv, PWG Raster, sheet labels) |
| `include/bt_print_utils.h` | Shared RFCOMM send helper (Brother + Phomemo BT) |
| `include/bt_discovery_utils.h` | Brand detection table, BLE UUID matching |

### Bluetooth Plugin

| File | Purpose |
|------|---------|
| `include/bluetooth_plugin.h` | C ABI for runtime-loaded BT plugin |
| `include/bluetooth_loader.h` | `BluetoothLoader` singleton — dlopen wrapper |
| `src/bluetooth/bt_plugin.cpp` | Plugin core, sd-bus event loop |
| `src/bluetooth/bt_discovery.cpp` | BlueZ D-Bus device discovery |
| `src/bluetooth/bt_pairing.cpp` | BlueZ pairing, trust |
| `src/bluetooth/bt_rfcomm.cpp` | RFCOMM socket connect/write |
| `src/bluetooth/bt_ble.cpp` | BLE GATT connect/write via D-Bus |
| `src/bluetooth/bt_lzo.cpp` | LZO1X compress for MakeID protocol (via miniLZO) |
| `lib/minilzo/` | miniLZO 2.10 (public domain, compiled into BT plugin only) |

## Printer Protocol Details

### Brother QL (ESC/P Raster)

- **Transport:** TCP (port 9100) or BT Classic RFCOMM
- **Protocol:** Binary ESC/P command stream
- **Sequence:** 200-byte invalidation → ESC @ init → raster mode → media info → auto-cut → per-row raster data → 0x1A print
- **Row format:** Horizontal flip, 90-byte row width for 62mm labels
- **DPI:** 300 (native)
- **Models:** QL-820NWB, QL-810W, QL-800, PT-*, TD-*, RJ-*

### Phomemo (ESC/POS Raster)

- **Transport:** USB (libusb), BT Classic RFCOMM, or BLE GATT
- **Protocol:** ESC/POS command stream with GS v 0 raster block
- **Sequence:** Speed + density + media type commands → GS v 0 raster → finalize + feed-to-gap
- **DPI:** 203 (native)
- **Models:** M110, M120, M02, Q199, and other M*/Q* series

### Niimbot (Custom BLE)

- **Transport:** BLE GATT only (Transparent UART service `e7810a71-...`)
- **Protocol:** Custom binary packets with XOR checksum, bidirectional (write + read)
- **Packet format:** `[0x55 0x55 CMD LEN DATA... XOR_CHECKSUM 0xAA 0xAA]`
- **DPI:** 203 (native)
- **Connection initialization:**
  1. BLE GATT connect with 1-second settle delay (firmware may discard early writes)
  2. `Connect` (0xC1) — printer-level session handshake that arms the thermal subsystem
  3. Read and log Connect response
- **Print job sequence:**
  1. `SetDensity` (0x21) — density 1-5
  2. `SetLabelType` (0x23) — WithGaps/BlackMark/Continuous/Transparent
  3. `PrintStart` (0x01)
  4. `PrintClear` (0xF0) — required by D110 before PageStart
  5. `PageStart` (0x03)
  6. `SetPageSize` (0x13) — height + width as u16be
  7. `SetQuantity` (0x15) — 1 copy (prevents D110 double-printing)
  8. Image rows: `PrintBitmapRow` (0x85) or `PrintEmptyRow` (0x84)
  9. `PageEnd` (0xE3)
  10. Poll `PrintStatus` (0xA3) until B3 response received
  11. `PrintEnd` (0xF3) — sent at first B3 response to prevent auto-repeat
- **Row encoding:** Per-row with 3-chunk or total black pixel count, repeat compression for identical consecutive rows
- **Persistent connections:** BLE connection is kept alive across prints. Disconnect/reconnect causes blank output on the D110 unless the full initialization sequence (settle + Connect) is repeated.
- **Models:** B21 (384px/48mm wide), D11/D110 (96px/12mm wide)

### MakeID/Wewin (Custom RFCOMM)

- **Transport:** BT Classic RFCOMM (channel 1). Despite being a dual-mode BLE device, the E1 uses RFCOMM for print data transfer. Advertises as "YichipFPGA-XXXX" over Bluetooth.
- **Protocol:** Custom binary frames with LZO1X-compressed bitmap data
- **Frame format:** `[0x66 LEN_LO LEN_HI CMD PAYLOAD... CHECKSUM]` — checksum is negated sum
- **DPI:** 203 (native for E1, varies by model)
- **Connection initialization:**
  1. RFCOMM connect to channel 1 (reuses existing connection if keepalive succeeds)
  2. Drain stale buffer data
  3. Handshake poll (0x10, state=search) until success response
- **Print job sequence:**
  1. Build bitmap: 1bpp MSB-first, 16-bit byte-swap, LZO1X compress
  2. Split into chunks of 56 rows max
  3. For each chunk: send 0x1B print data frame with 17-byte header
  4. Wait for 36-byte ACK response between frames
  5. Handle wait/resend states by retrying
  6. Send cancel handshake (0x10, state=cancel) to finalize
- **0x1B frame header:** magic, length(LE), cmd, darkness|label_type, cut|save, total_copies(LE), current_copy(LE), 0x01, width_px(LE), chunk_height(LE), remaining_chunks, 0x00
- **Response parsing:** 36-byte responses. byte[4] bits 0-5=error code (0 or 23=success), bit 6=resend, bit 7=wait. byte[35] bit 7=isPrinting, bits 5-6: 1=pause, 3=cancel
- **LZO compression:** miniLZO compiled into BT plugin (`bt_lzo.cpp`), accessed via `BluetoothLoader::lzo_compress`. Falls back to raw data when plugin unavailable.
- **Persistent connections:** Static globals + mutex, heartbeat liveness check via handshake poll
- **Models:** E1 (also branded MakeID 2AUMQ-E1), L1, M1 — support 9mm, 12mm, and 16mm continuous tape (no die-cut labels)
- **Printhead:** 203 DPI, 96px width

## Brand Detection

`bt_discovery_utils.h` contains a unified `PrinterBrand` table for detecting printer type from BLE device name:

```cpp
struct PrinterBrand {
    const char* prefix;
    bool is_ble;       // BLE-only (no SPP/RFCOMM)
    bool is_brother;   // Brother QL protocol
    bool is_niimbot;   // Niimbot protocol
    bool is_makeid;    // MakeID/Wewin protocol
};
```

Key helpers:
- `find_brand(name)` — table lookup by name prefix
- `is_brother_printer(name)` — Brother QL family
- `is_niimbot_printer(name)` — Niimbot family (B21, D11, D110)
- `is_makeid_printer(name)` — MakeID/Wewin family (E1, L1, M1)
- `name_suggests_ble(name)` — device needs BLE transport
- `is_label_printer_uuid(uuid)` — matches SPP, Phomemo BLE, Niimbot BLE, or MakeID service UUIDs

## Bluetooth Transport

### Plugin Architecture

Bluetooth support is a runtime-loadable shared library (`libhelix-bluetooth.so`). Zero impact when BT hardware is absent — no libraries loaded, no threads started.

```
BluetoothLoader (dlopen) → libhelix-bluetooth.so (sd-bus, BlueZ D-Bus)
```

### RFCOMM (Brother, Phomemo SPP, MakeID)

Shared `rfcomm_send()` helper in `bt_print_utils.cpp` (Brother, Phomemo):
1. Init BT context → connect RFCOMM → write data in chunks → 5s drain sleep → disconnect → deinit
2. Single shared mutex prevents concurrent RFCOMM connections
3. Detached thread, callback via `queue_update()`

**MakeID:** Uses its own RFCOMM transport (`makeid_bt_printer.cpp`) with persistent connections, handshake-based keepalive, and bidirectional frame exchange (write + read response per chunk).

### BLE GATT (Niimbot, Phomemo BLE)

Each backend manages its own BLE connection:
1. Init BT context → `connect_ble()` with service UUID → bidirectional `ble_write()`/`ble_read()` per packet
2. Per-printer mutex serialization
3. Inter-packet delays: 10ms for image rows (fire-and-forget), 100ms for command packets (read ACK response)
4. **Niimbot:** Persistent connection kept alive across prints. Connection initialization includes 1-second BLE settle delay and Connect (0xC1) handshake. PrintStatus polling with `ble_read()` confirms print completion before sending PrintEnd.

## Label Sizes

Each printer family defines its own label size table:

| Function | Printhead | DPI | Example Sizes |
|----------|-----------|-----|---------------|
| `BrotherQLPrinter::supported_sizes_static()` | 720px (62mm) | 300 | 29mm, 62mm, 29x90mm |
| `PhomemoPrinter::supported_sizes_static()` | varies | 203 | 40x30mm, 50x30mm |
| `niimbot_b21_sizes()` | 384px (48mm) | 203 | 50x30mm, 40x30mm, 50x50mm |
| `niimbot_d11_sizes()` | 96px (12mm) | 203 | 12x40mm, 12x22mm, 12x30mm |
| `niimbot_sizes_for_model(name)` | auto-detect | 203 | Selects B21 or D11 from device name |

## Testing

| Test File | Coverage |
|-----------|----------|
| `tests/unit/test_niimbot_protocol.cpp` | Packet framing, checksum, print job sequence, blank rows, label sizes |
| `tests/unit/test_bt_discovery_utils.cpp` | UUID matching, brand detection, BLE classification |

## Adding a New Printer Protocol

1. Create `include/<brand>_protocol.h` with pure packet/raster building functions
2. Create `src/system/<brand>_protocol.cpp` with implementation
3. Create `include/<brand>_bt_printer.h` implementing `ILabelPrinter`
4. Create `src/system/<brand>_bt_printer.cpp` with BLE/RFCOMM transport
5. Add brand entries to `KNOWN_BRANDS[]` in `bt_discovery_utils.h`
6. Add routing in `label_printer_utils.cpp` (both size selection and print dispatch)
7. Add unit tests for protocol in `tests/unit/test_<brand>_protocol.cpp`
