# ESP32 Port (BTT K-Touch): developer guide

HelixScreen runs natively on the BigTreeTech K-Touch, an ESP32-S3 panel (8MB octal PSRAM, 16MB flash, 800x480 RGB,
GT911 touch; hardware notes in
[`printer-research/BTT_K_TOUCH_HARDWARE.md`](printer-research/BTT_K_TOUCH_HARDWARE.md)). This page is the hands-on
part: building, flashing, the serial console, the gates, and the debugging recipes. How the firmware is put
together (the source subset, memory model, tasks, boot order, rendering, networking, storage, OTA) is [architecture
chapter 17](architecture/17-esp32-firmware.md); read it before changing anything under
`firmware/helixscreen-esp32/`. Users install from
[`../user/guide/install-esp32.md`](../user/guide/install-esp32.md). The feasibility numbers behind the port are in
[`ESP32_NATIVE_AUDIT.md`](ESP32_NATIVE_AUDIT.md).

ESP32 is not built or shipped on the `release/1.0` line (`.github/workflows/esp32-build.yml` skips it).

## Changing shared code

Most firmware breakage comes from ordinary `src/` changes, because nothing in `make` or `make test` compiles the
firmware. Before you commit:

- **New `src/` file:** add it to `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` or to
  `app_srcs_excluded.txt` with a reason. `scripts/check_esp32_app_srcs.py` fails the commit otherwise.
- **New XML subject or callback:** its registration must be in a file and an `#if` branch the firmware compiles, or
  on the device the binding resolves to nothing. `scripts/check_esp32_xml_bindings.py` ratchets it.
- **Exceptions:** no `try`/`catch`/`throw`, and none of the calls that compile and then abort (`j.value("k", d)`,
  `j.at("k")`, one-argument `json::parse`, `std::sto*`, value-form `std::any_cast`). Use `json_util`'s readers,
  `text_io::parse_leading` and `include/exception_policy.h`.
- **Includes:** no `<regex>`, `<filesystem>`, `<locale>` or any iostream header (`<sstream>`, `<fstream>`,
  `<iomanip>`...; each links libstdc++'s locale machinery), and `<spdlog/fmt/fmt.h>` rather than `<fmt/format.h>`,
  which the firmware toolchain does not have.
- **Statics:** a large static in a firmware-compiled file is `HELIX_PSRAM_BSS`, `const`, or heap.
  `check_esp32_size.py` fails CI when `.dram0.bss` crosses its ceiling, and on the device the symptom is
  `wifi:malloc buffer fail` at boot and no WiFi.
- **Push:** `.githooks/pre-push` runs `scripts/esp32_syntax_check.py`, which compiles the touched firmware units
  `-fsyntax-only` in the `espressif/idf` image and catches the host-green, Xtensa-red class (`int32_t` is `long`).
  It skips with a note when docker or the image is missing; `HELIX_ESP32_SYNTAX=0` skips it.

## Building

CI (`.github/workflows/esp32-build.yml`) is the reference recipe. ESP-IDF is pinned (`ESP_IDF_VERSION`, v5.5.5);
the image size depends on the IDF container as well as the code (unwind table size varies between releases), so
re-measure the image against `size_budget.json` before moving the pin.

```bash
make apply-patches                       # LVGL patches, from the repo root
cd firmware/helixscreen-esp32
idf.py reconfigure || true               # fetches managed components (frogfs's packer); fails on the missing image
python3 ../../scripts/esp32_printer_images.py   # needs Pillow; staging regenerates them when absent
python3 ../../scripts/esp32_stage_assets.py     # minified ui_xml/ + ui_xml_overrides/, config JSON, pictures
python3 ../../scripts/esp32_pack_assets.py      # needs pyyaml; build/storage_frogfs.bin, gated on the partition
idf.py build
python3 ../../scripts/check_esp32_size.py build/helixscreen_esp32.bin size_budget.json build/helixscreen_esp32.map
```

The packed asset image is a build input: `main/CMakeLists.txt` refuses to configure without it and
`scripts/esp32_check_asset_staleness.py` fails a build whose image is older than any XML, asset or picture. Re-run
stage and pack after editing `ui_xml/`.

On a jobpool machine run the IDF steps in the `espressif/idf:v5.5.5` container through `scripts/pool-docker.sh`, so
the firmware's ninja joins the machine-wide pool; `scripts/esp32_build_debug_heap.sh` is a working example of the
`docker run` line, including the extra read-only mount a `setup-worktree.sh` tree needs for its symlinked `lib/`.
Natively, source ESP-IDF's environment first (`. $IDF_PATH/export.sh`).

### Local configuration

`sdkconfig.local` in the firmware directory is git-ignored and layered over `sdkconfig.defaults` when present. Put
bench-only values there, never in a committed file:

| Option | Use |
|--------|-----|
| `CONFIG_HELIX_HIL_WIFI_SSID` / `_PASS` | Seed WiFi without the first-boot portal |
| `CONFIG_HELIX_HIL_MOONRAKER_URL` | First-boot Moonraker host (`ws://host:7125/websocket`); never overrides a host saved in Settings |
| `CONFIG_HELIX_MOCK_PRINTER=y` | Synthetic printer, no network (chapter 17, Mock mode) |
| `CONFIG_HELIX_LOG_STRIP_DEBUG=n` | Keep `spdlog::debug`/`trace`; stripped by default, so a debug line you add does not print |
| `CONFIG_HELIX_AMS_HTTP_POLL_BACKENDS=y` | Let the ACE and AD5X IFS backends run |
| `CONFIG_HELIX_FAULT_CRASH_HOST` | Abort when connected to a matching host, to exercise the boot crash guard (#1750) |
| `CONFIG_HELIX_NET_HIL` / `CONFIG_HELIX_HTTP_HIL` | Test-only transport and HTTP-lane probes |

`HELIX_SDKCONFIG_OVERLAY=<file>` layers one more file last; the debug-heap build uses it.

## Flashing

Claim the board first (`scripts/helix-claim take device:ktouch-esp32 ...`, see the root `CLAUDE.md`). From the
build directory:

```bash
python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 921600 \
    --before default_reset --after hard_reset write_flash @flash_args
```

`flash_args` writes the bootloader, partition table, `otadata`, the app at `ota_0` and the asset image. It never
touches `nvs` (WiFi credentials) or `cfg` (`settings.json`), so a reflash keeps the printer and the network.
Changing `partitions.csv` needs this full USB flash; OTA cannot move partitions. The release zip
(`scripts/esp32_package_release.sh`) carries a merged factory image that stops short of `cfg` and refuses to build
one that would reach it.

After a `ui_xml/` or asset edit, re-run stage and pack and reflash only the asset partition:
`idf.py -p /dev/ttyUSB0 storage-flash` (the target is defined in `main/CMakeLists.txt`). The app image is
untouched.

Without a dev setup, the same images install from a browser through the helixscreen.org/flash web flasher, or from
a release zip with esptool; [`../user/guide/install-esp32.md`](../user/guide/install-esp32.md) has both. The
firmware has no on-device updater.

## Serial console

The board's CH340 bridge resets the ESP32 every time the port is opened, and can drop off USB and re-enumerate
during boot. Open the port once per capture, and never run a reader that reopens on disconnect: each reopen resets
the board, which re-enumerates, forever.

`scripts/esp32_serial_snapshot.py` drives the console commands (`main/serial_snapshot.h`):

```bash
scripts/esp32_serial_snapshot.py /dev/ttyUSB0 out.png --settle 45            # screenshot once booted and connected
scripts/esp32_serial_snapshot.py /dev/ttyUSB0 out.png --tap 37,330 --settle 45   # tap the navbar gear first
scripts/esp32_serial_snapshot.py /dev/ttyUSB0 out.png --notes                # also print the notification history
```

Without `--settle` the capture lands during boot, before WiFi and Moonraker: zero temperatures and a greyed navbar
mean "not connected yet", not a bug. The log lines worth grepping are listed in chapter 17 (Crash recovery and
diagnostics); the boot heap gates print as `heap before pthread` and `internal heap before rgb panel`.

## Debugging recipes

| Symptom | Look at |
|---------|---------|
| Boot loop, `pthread_create failed: 12` or `no mem for bounce buffer` | Internal DRAM: compare `.dram0.bss` against `size_budget.json`, and the `[heap:*]` lines. A new static in a firmware file is the usual cause |
| WiFi never associates, `wifi:malloc buffer fail` | Same: internal DRAM at boot |
| Greyed navbar, `transport could not start` or `transport start refused` | The WebSocket task found no 8KB internal block; check what allocated internal RAM since boot |
| A one-frame glitch on some action | `[scanout] underruns` and `[present] late blit` in the same 10s window; a PSRAM heap walk or a flash write in that path |
| Taps ignored during load | `slow ui cycle` / `slow refresh cycle`: the UI thread was busy and the polled touch read was skipped |
| Panic | Symbolize with `xtensa-esp32s3-elf-addr2line -pfiaC -e <elf>` against the exact ELF flashed; the crash log's `ELF file SHA256` names it |
| Heap corruption | `scripts/esp32_build_debug_heap.sh` builds with heap poisoning in its own directory and archives the matching ELF |
| Reconnect behaviour | `scripts/esp32_ws_chaos_proxy.py` sits between the panel and Moonraker and drops the connection on demand |

## Gates

| Tool | When | What |
|------|------|------|
| `scripts/check_esp32_app_srcs.py` | commit (`scripts/qc/esp32_app_srcs.sh`) | Manifest coverage, stale lines, locale-pulling includes, exception sites. `--link` (advisory) reads native objects for a kept file calling a symbol only an excluded file defines |
| `scripts/check_esp32_xml_bindings.py` | commit (`scripts/qc/esp32_xml_bindings.sh`) | XML bindings the firmware does not register; baseline `scripts/esp32_xml_binding_baseline.txt` |
| `scripts/esp32_syntax_check.py` | pre-push | Xtensa `-fsyntax-only` on touched firmware units |
| `scripts/check_esp32_size.py` | `esp32-build.yml` | App image vs `app_max_bytes`, `.dram0.bss`/`.data` vs their ceilings, exception support or `std::locale` linked |
| `tests/python/test_esp32_pack_assets.py` | `esp32-build.yml` | The frogfs packer round trip |
| `tests/shell/test_code_lint.bats` | bats suite | A PSRAM heap walk on a per-event path (thumbnail, print select, print status) |
| `tests/unit/test_esp32_*.cpp` | unit sweep | Reconnect backoff, link liveness, transport ordering, the HTTP lane queue, tested on the host |

`size_budget.json` ceilings are raised only after checking internal free at boot on the device; the comment in that
file says so.
