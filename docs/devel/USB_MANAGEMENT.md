# USB Management (Developer Guide)

How HelixScreen finds USB drives, mounts them when nothing else does, lists their G-code, and talks to USB label printers and barcode scanners. This guide goes deep on the USB-specific pieces; the wider peripheral picture lives elsewhere and is linked rather than repeated.

**Related docs**

- [architecture/13-peripherals.md](architecture/13-peripherals.md): peripheral lifecycles, the label-printer and scanner model across all transports
- [LABEL_PRINTER_SYSTEM.md](LABEL_PRINTER_SYSTEM.md): label protocols, rendering, settings
- [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md): `HELIX_USB_AUTOMOUNT`, plus the USB mouse/keyboard overrides (`HELIX_MOUSE_DEVICE`, `HELIX_KEYBOARD_DEVICE`), which belong to input handling and are not covered here
- User side: [Printing guide](../user/guide/printing.md) (Printer/USB tabs, automount), [label printing](../user/guide/label-printing.md), [barcode scanner](../user/guide/barcode-scanner.md)

---

## What "USB" Covers

Four subsystems share the word and almost nothing else:

| Subsystem | What it does | Detection mechanism | Thread |
|-----------|--------------|---------------------|--------|
| Removable drives | Lists G-code on a mounted stick as a print source, imports printer images | Mount table (`/proc/self/mountinfo` poll) | `UsbBackendLinux` monitor thread |
| Fallback automounter | Mounts sticks read-only on boards with no mounter of their own | sysfs `/sys/block/sd*` | Same monitor thread |
| USB label printers | Phomemo M110 over the kernel `usblp` driver | libusb bus enumeration | `HttpExecutor::fast()` scan driven by an LVGL timer + detached print thread |
| HID barcode scanners | Reads keyboard-wedge scanners for Spoolman spool IDs | evdev `/dev/input` | `UsbScannerMonitor` thread |

USB mice, keyboards and touchscreens go through `input_device_scanner` and the display backends; see [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md) for their overrides.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/usb_backend.h` / `src/api/usb_backend.cpp` | `UsbBackend` interface, `UsbDrive`/`UsbGcodeFile`/`UsbError` types, platform factory |
| `include/usb_backend_linux.h` / `src/api/usb_backend_linux.cpp` | Linux backend: mount-table monitor, USB classification, G-code walk |
| `include/usb_automount.h` / `src/api/usb_automount.cpp` | `UsbAutomount` fallback mounter and its injectable `MountOps` syscall surface |
| `include/usb_backend_mock.h` / `src/api/usb_backend_mock.cpp` | `UsbBackendMock`: demo drive for `--test`, simulate insert/remove for tests |
| `include/usb_manager.h` / `src/api/usb_manager.cpp` | `UsbManager`: owns the backend, single drive-event callback, locked queries |
| `src/application/subject_initializer.cpp` | Creates the `UsbManager`, fans drive events out to the UI and toasts |
| `include/ui_print_select_usb_source.h` / `src/ui/ui_print_select_usb_source.cpp` | `PrintSelectUsbSource`: Printer/USB source switch, subjects, file conversion |
| `src/ui/ui_panel_print_select.cpp` | Hosts the USB source, copies a USB file to Moonraker before printing, Moonraker `usb/` symlink probe |
| `ui_xml/print_select_panel.xml` | `source_selector` bindings |
| `src/ui/ui_overlay_printer_image.cpp` | Image import from the first mounted drive |
| `include/single_flight_walk.h` | `SingleFlightWalk`: one stick walk at a time on `HttpExecutor::fast()`, newest request wins |
| `src/print/print_file_data.cpp` | `PrintFileData::from_usb_file` (keeps the stick path in `local_path`) |
| `include/usb_printer_detector.h` / `src/system/usb_printer_detector.cpp` | libusb scan for known label printer VID:PIDs |
| `src/system/phomemo_printer.cpp` | Phomemo USB transport (`/dev/usb/lpN` write) |
| `src/system/label_printer_utils.cpp` | Transport dispatch for spool labels, including the USB fallback |
| `src/ui/ui_settings_label_printer.cpp` | USB printer dropdown and polling in label printer settings |
| `include/usb_scanner_monitor.h` / `src/system/usb_scanner_monitor.cpp` | evdev reader for HID barcode scanners |

---

## Removable Drives

### Layers

```
UsbBackendLinux monitor thread          UI thread (LVGL)
  |                                       |
  poll(/proc/self/mountinfo, POLLPRI)     |
  UsbAutomount::poll()  (root only)       |
  parse_mounts() -> diff cached_drives_   |
  |                                       |
  EventCallback(DRIVE_INSERTED/REMOVED)   |
    -> UsbManager::on_backend_event       |
       -> SubjectInitializer lambda       |
            queue_update(toast) --------> NOTIFY_SUCCESS / NOTIFY_INFO
            queue_update(panel) --------> PrintSelectPanel::on_usb_drive_inserted/removed
                                            -> PrintSelectUsbSource
                                                 lv_subject_set_int(print_source_usb_present)
                                                 -> print_select_panel.xml bind_flag_if
```

`UsbBackend` is the platform seam. `UsbManager` is what application code holds: it creates the backend through `UsbBackend::create()` (which returns it unstarted), attaches its event callback, then starts it, so no event can fire before someone is listening. It forwards backend events to one registered `DriveCallback`, which `SubjectInitializer` also attaches before calling `start()`. `get_drives()` runs under the manager's mutex; `scan_for_gcode()` takes it only to copy the backend's `shared_ptr`, so a long walk never blocks a drive query. `SubjectInitializer` owns the only `UsbManager` (`include/subject_initializer.h#SubjectInitializer/usb_manager`) and is the only place that registers a drive callback.

Two consumers hold a raw `UsbManager*`:

- `PrintSelectPanel` gets push events (through the `SubjectInitializer` callback) and also reads `get_drives()` when the manager is attached, which covers a drive that was already mounted before the panel existed (`src/ui/ui_print_select_usb_source.cpp#set_usb_manager`).
- `PrinterImageOverlay` gets no events. It calls `get_drives()` on every `on_activate()` and walks only `drives[0]` for images, off the UI thread through its own `SingleFlightWalk` (`src/ui/ui_overlay_printer_image.cpp#scan_usb_drives`).

### Detection: watching the mount table

procfs never generates inotify events, so the backend polls `/proc/self/mountinfo` for `POLLPRI`, which the kernel raises on any mount table change once the file has been read to EOF and rewound. From `src/api/usb_backend_linux.cpp#monitor_thread_func`:

```cpp
            struct pollfd pfd;
            pfd.fd = mountinfo_fd_;
            pfd.events = POLLPRI;

            int ret = poll(&pfd, 1, 500);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                spdlog::error("[UsbBackendLinux] poll(mountinfo) failed: {}, "
                              "switching to 1s content polling",
                              strerror(errno));
                switch_to_content_polling();
                continue;
            }

            if (ret > 0 && (pfd.revents & (POLLPRI | POLLERR))) {
                drain_mountinfo_fd();
                mounts_changed = true;
            }
```

Three detection modes stack:

| Mode | When | Latency |
|------|------|---------|
| mountinfo `POLLPRI` | Default; `/proc/self/mountinfo` opened at `start()` | Immediate (500ms poll timeout only bounds shutdown) |
| `/proc/mounts` content compare | mountinfo cannot be opened, or `poll()` fails | 1s |
| Safety re-parse | Always, on top of either mode | 10s |

The safety re-parse means a kernel where the event path silently stops firing still converges. Content is compared rather than mtime because `/proc/mounts` is usually a symlink to `/proc/self/mounts`, whose mtime never changes.

On every change the monitor re-parses `/proc/mounts`, diffs against `cached_drives_` by `mount_path`, swaps the cache under the backend mutex, and fires callbacks outside the lock: removals first, then insertions.

### Classification: what counts as a USB drive

`UsbBackendLinux::is_usb_mount` (`src/api/usb_backend_linux.cpp#is_usb_mount`) accepts a `/proc/mounts` row only if all of these hold:

1. The device starts with `/dev/`.
2. The mount point is under one of the prefixes in `is_usb_mount_point`:

   ```cpp
       return (mount_point.find("/media/") == 0 || mount_point.find("/mnt/") == 0 ||
               mount_point.find("/run/media/") == 0 || mount_point.find("/tmp/udisk/") == 0);
   ```

   `/tmp/udisk/` is where the Creality K1C firmware mounts sticks (#610).
3. The filesystem is one of `vfat`, `msdos`, `exfat`, `ntfs`, `ntfs3`, `ext4`, `ext3`, `fuseblk`. `iso9660` and `f2fs` are excluded on purpose: a loop-mounted ISO under `/media` would otherwise surface as a drive, and f2fs is the boards' internal flash.
4. The base block device (partition digits stripped) reads `removable=1` in `/sys/block/<dev>/removable`, **or** its `device/uevent` names `DRIVER=usb-storage` or `DRIVER=uas`, **or**, failing both sysfs checks, the mount point is under `/media/`.

The volume label comes from the mount point leaf when it does not look like a device name (`sd*`, `nvme*`), else a reverse lookup through `/dev/disk/by-label`, else the device basename (`src/api/usb_backend_linux.cpp#get_volume_label`).

`UsbDrive` deliberately has no capacity fields. Free space on a FAT volume costs a full FAT scan on the first query after mount, so whatever displays capacity has to compute it on demand.

### Listing G-code

`scan_for_gcode()` refuses a mount path that is not in `cached_drives_`, then releases the backend mutex and walks it recursively to `max_depth` 3 by default (`src/api/usb_backend_linux.cpp#scan_directory`). Files are kept when `helix::gcode::has_printable_extension()` accepts the name, the same predicate the Moonraker file list uses, so 8.3 names like `3DBENC~1.GCO` from an `msdos` mount still match.

`PrintSelectUsbSource::refresh_files()` takes the drive list and a `shared_ptr` to the backend (`UsbManager::backend_snapshot()`) on the UI thread, then hands the slow part to `HttpExecutor::fast()`: `scan_usb_drives()` walks every mounted drive into one flat list (a file's path already carries its mount point) and pulls each file's header thumbnail into the thumbnail cache (`src/ui/ui_print_select_usb_source.cpp#scan_usb_drives`):

```cpp
    for (const auto& file : scan.files) {
        if (cancelled()) {
            return scan;
        }
        std::string cache_path;
        auto best = helix::gcode::get_best_thumbnail(file.path);
        if (!best.png_data.empty()) {
            cache_path = get_thumbnail_cache().save_raw_png("usb:" + file.path, best.png_data);
        }
        scan.thumbnails.push_back(std::move(cache_path));
    }
```

The cache key is the full path, so same-named files in different folders or on different sticks keep their own thumbnails. `save_raw_png` accepts PNG only; Cura's `; thumbnail_JPG begin` blocks still arrive as PNG, because `get_best_thumbnail()` re-encodes a JPEG thumbnail through stb_image and lodepng (`src/rendering/gcode_parser.cpp#jpeg_to_png`).

One walk runs at a time, through `SingleFlightWalk` (`include/single_flight_walk.h`). Every refresh supersedes the walk in flight: its `cancelled()` predicate, polled before each drive and each file, turns true and its result is dropped, and the newest request runs once it ends, so any number of refreshes during a walk cost one extra walk. A switch to the Printer tab and the source's destruction cancel both the running walk and any queued one. Results come back through the walk's lifetime token, which expires with the source. The walk holds the backend, never the manager, because the application destroys the manager before it stops the executors; `~UsbManager` stops the backend, so a walk still holding it scans nothing and its monitor thread cannot report into the freed manager.

The walk stays on the fast lane rather than the slow one: single-flight it occupies at most one of the four fast workers, while the slow lane's single worker would queue the listing behind any large G-code transfer. On the UI thread each entry goes through `PrintFileData::from_usb_file`, which fills `--` for print time, filament, layers and height and keeps the stick path in `local_path`. The panel then marks every entry `metadata_fetched = true` so no Moonraker metadata request goes out.

That pre-extracted path rides along to `PrintStartController` and on to `ActivePrintMediaManager::set_thumbnail_path`, so the print status panel can show it without a Moonraker fetch.

### Starting a print from the USB source

Moonraker usually cannot read a stick HelixScreen mounted itself (it runs as another user, in another mount namespace, or on another host), so a USB file is copied to Moonraker before it prints. When the selected file has a `local_path`, `PrintSelectPanel::start_print` and `add_to_queue` call `copy_usb_file_to_printer()` (`src/ui/ui_panel_print_select.cpp#copy_usb_file_to_printer`). It lists `gcodes/usb_prints` and names the copy with `choose_usb_copy_target()` (`src/print/print_file_data.cpp#choose_usb_copy_target`), then either reuses a file already there or streams the stick file through `ITransfersAPI::upload_file_from_path`, under a `BusyOverlay` with progress. The panel then hands `PrintStartController` the copy's name with `usb_prints` as its directory (or queues `usb_prints/<name>`), and the normal start pipeline runs against the copy. The filename, tool colors and thumbnail are read when Print is tapped, not when the copy lands. On failure it toasts "Could not copy ... from USB" and starts nothing. Print and Add to Queue taps are ignored while a copy is in flight. `BusyOverlay` has no cancel and libhv's upload timeout is an hour, so a one-shot watchdog (`kUsbCopyStallMs`, 30s) abandons a copy that reports no progress for that long, toasts "Copying from USB stopped responding", and ignores the copy's late answer; each whole-percent progress report pushes the watchdog back.

Policy, as implemented:

- The folder is `usb_prints` (`PrintSelectPanel::kUsbCopyDir`), never `usb`: that name is the stick symlink some images create, which the probe below looks for.
- Naming never replaces a different file. The candidates are `<name>`, then `<stem> (2).<ext>`, `<stem> (3).<ext>`, and so on. The first candidate that is absent is uploaded to; the first that holds a file of the stick file's size is reused with no upload. Size is the identity test.
- The folder is read with a listing rather than per-file metadata: one request answers every candidate, and listing sizes come from the filesystem. Entries outside `usb_prints/` are ignored, because some Moonraker versions answer a missing folder with the root listing. A 404 means the folder does not exist yet and the copy goes ahead as `<name>`; any other listing error stops the copy with a toast.
- Copies are left in place after the print, so history, reprint and the Printer tab keep working. Nothing prunes the folder, so editing and re-slicing one file leaves a numbered copy per version.
- The copy happens after the panel's own preflight checks, before `PrintStartController`'s gates, so a print cancelled at a gate leaves its copy behind.

### The detail view for a USB file

`PrintSelectPanel::show_detail_view` hands the detail view the file's `local_path`. Moonraker has no copy of the file until it is printed, so every read comes off the stick: `local_gcode_source()` returns the stick path, which the preview, the footer read and the whole-file tools scan already read in place for a same-host Moonraker ([architecture/16-gcode-pipeline.md](architecture/16-gcode-pipeline.md), "Getting the file"), and `PrintPreparationManager::scan_file_for_operations` reads the preamble with `text_io::read_file(path, PRINTER_STOP_SCAN_BYTES)` on the slow lane. No metadata request goes out. A stick file that cannot be read fails at once rather than falling back to HTTP. The view keys its tools-used cache and the operations scan by the stick path, so a same-named printer file never answers for it.

Delete is not offered: the delete button binds `hidden` to `print_source_is_usb`, and `show_delete_confirmation()` / `delete_file()` refuse a file with `local_path`, since delete addresses Moonraker storage by name and the automounter mounts sticks read-only.

### The Moonraker `usb/` symlink case

Many Klipper images link a stick into Moonraker's storage (`gcodes/usb -> /media/sda1`). There the Printer source already shows the files and HelixScreen's own USB tab is redundant. On connect, `PrintSelectPanel::check_moonraker_usb_symlink` lists `gcodes/usb` and, if any returned path actually starts with `usb/`, calls `set_moonraker_has_usb_access(true)`. The path check matters because some Moonraker versions answer a missing directory with the root listing instead of a 404 (#610).

### Subjects and XML

| Subject | Type | Owner | Meaning |
|---------|------|-------|---------|
| `print_source_is_usb` | int | `PrintSelectUsbSource` | 0 = Printer tab active, 1 = USB tab active |
| `print_source_usb_present` | int | `PrintSelectUsbSource` | At least one drive mounted |
| `print_source_moonraker_usb_access` | int | `PrintSelectUsbSource` | Moonraker exposes `gcodes/usb` |
| `printer_image_usb_visible` | int | `PrinterImageOverlay` | Show the USB import section |
| `printer_image_usb_status` | string | `PrinterImageOverlay` | Import progress / "No images found" text |

The three `print_source_*` subjects are file-static, registered globally in `PrintSelectUsbSource::init_subjects()` before the panel XML parses, and torn down through `StaticSubjectRegistry`. Visibility is purely declarative (`ui_xml/print_select_panel.xml`):

```xml
        <bind_flag_if cond="not print_source_usb_present or print_source_moonraker_usb_access" flag="hidden"/>
```

The four tab buttons (`source_printer_active`, `source_printer_inactive`, `source_usb_active`, `source_usb_inactive`) each bind `hidden` to `print_source_is_usb`, so C++ only ever sets the int.

On removal, `on_drive_removed()` asks the manager what is still mounted, because the event names one drive and a second may remain. If drives remain and the USB tab is active it rescans; if none remain it switches back to Printer.

### Threading

| Code | Thread | Rule |
|------|--------|------|
| `UsbBackendLinux::monitor_thread_func`, `UsbAutomount::poll` | Monitor thread | Never touches LVGL. All automount syscalls stay here, including the shutdown `unmount_all()`. |
| `UsbBackendMock` demo insert | Mock's demo thread | Same callback path as the real backend |
| `DriveCallback` | Whichever backend thread fired it | Must marshal with `helix::ui::queue_update()` before touching widgets or subjects |
| `PrintSelectUsbSource::refresh_files` | UI thread, then `HttpExecutor::fast()` | Drive list and backend snapshot on the UI thread; walk and thumbnail extraction on the worker, one at a time; result delivered with `tok.defer()` |
| `PrinterImageOverlay` | UI thread, then `HttpExecutor::fast()` | `get_drives()` on the UI thread; the `drives[0]` image walk through `SingleFlightWalk` |

The `SubjectInitializer` callback captures a raw `PrintSelectPanel*` together with a `weak_ptr<bool>` alive guard, and checks `expired()` inside each queued lambda, so a queued update that lands after teardown does nothing (`src/application/subject_initializer.cpp#init_usb_manager`). It also suppresses the "USB drive connected" toast for 3 seconds after setup so a drive present at boot does not announce itself.

`UsbManager::~UsbManager` and `UsbBackendMock::~UsbBackendMock` skip their own mutexes: they can run during static destruction, when the mutex may already be gone. `~UsbManager` still calls the backend's `stop()`, since a scan may hold the backend past the manager.

---

## Fallback Automounter

Some boards ship nothing that mounts a stick. `UsbAutomount` fills that gap without fighting a mounter that does exist. It is owned by `UsbBackendLinux` and driven from the top of each monitor loop pass, so a mount it performs changes the mount table and the stick is detected by the ordinary parse path with no special casing.

**Arming.** `UsbAutomount::create()` returns `nullptr` (disarmed) unless the process runs as root, and also when `HELIX_USB_AUTOMOUNT=0`. A developer's desktop build therefore never mounts the workstation's drives.

**Grace period.** A newly seen unmounted removable device waits 3s (`kDefaultGrace`) before the first attempt, so udisks2, an mdev hotplug helper or a vendor app wins the race. At creation, `probe_primary_mounter()` looks for udev's control socket (`/run/udev/control`) and a registered kernel hotplug helper (`/proc/sys/kernel/hotplug`). Only when both are positively absent does the grace drop to zero; an unreadable probe keeps it.

**Candidates.** `removable_block_devices()` walks `/sys/block/sd*`, keeps disks that pass the same removable / `usb-storage` / `uas` test as `is_usb_mount`, and yields each partition, or the whole disk when there is no partition table. Any device listed anywhere in `/proc/mounts` belongs to someone else and is left alone.

**The option ladder.** Kernel NLS support differs per board, so no single option string mounts FAT everywhere. `automount_ladder()` (`src/api/usb_automount.cpp#automount_ladder`):

```cpp
    return {
        {"vfat", "ro,noatime"},
        {"vfat", "ro,noatime,iocharset=utf8"},
        {"vfat", "ro,noatime,iocharset=iso8859-1"},
        {"vfat", "ro,noatime,codepage=437,iocharset=iso8859-1"},
        {"exfat", "ro,noatime"},
        {"ntfs3", "ro,noatime"},
        {"msdos", "ro,noatime"},
        {"msdos", "ro,noatime,iocharset=utf8"},
        {"msdos", "ro,noatime,iocharset=iso8859-1"},
        {"msdos", "ro,noatime,codepage=437,iocharset=iso8859-1"},
    };
```

`vfat` comes before `msdos` because only vfat keeps long filenames. The combination that worked is cached and tried first next time, since it is a property of the kernel, not the stick. When the whole ladder fails the device sits out a 30s cooldown (`kRetryCooldown`).

**Mount point.** `/mnt/usb/<basename>` (`src/api/usb_automount.cpp#automount_mount_point`). It has to stay inside the `is_usb_mount_point` prefixes, or the backend would never report the drive.

**Ownership.** `ours_` records only mounts this component created. When a device vanishes, its stale mount is unmounted, plain first and then `MNT_DETACH` if busy (safe on a read-only mount, and it returns immediately so `stop()`'s join cannot hang). A mount someone else removed is just forgotten. `unmount_all()` runs on the monitor thread as it exits.

The successful mount logs at info with the filesystem and options, which is the line support needs from a field log:

```
[UsbAutomount] Mounted /dev/sda1 at /mnt/usb/sda1 read-only (vfat, options: ro,noatime)
```

---

## Per-Platform Behaviour

| Platform | Drive backend | Automount | libusb (label printer detection) | Label printer transport |
|----------|---------------|-----------|----------------------------------|-------------------------|
| Linux, root (most printer installs) | `UsbBackendLinux` | Armed unless `HELIX_USB_AUTOMOUNT=0` | Pi, pi32, x86 targets and native Linux builds | `/dev/usb/lpN` via `usblp` |
| Linux, non-root | `UsbBackendLinux` | Disarmed | Same | Needs write access to `/dev/usb/lpN` |
| Linux cross targets without libusb (ad5m, cc1, K1, K2, ...) | `UsbBackendLinux` | As above | Not compiled (`HELIX_HAS_LIBUSB` undefined): `scan()` returns empty | Detection finds nothing, so USB printing reports "No USB printer detected" |
| Android | None (`create()` returns `nullptr`) | Not compiled | n/a | `find_usblp_device` returns empty |
| macOS (dev only) | None (`create()` returns `nullptr`) | Not compiled | When Homebrew libusb is found by pkg-config | No `/dev/usb/lp*`, so nothing prints |
| Any, `--test` | `UsbBackendMock` | n/a | Unchanged | Unchanged |

The libusb gating lives in the `Makefile` platform blocks; `-DHELIX_HAS_LIBUSB=1` is what `src/system/usb_printer_detector.cpp` keys on.

When `UsbManager::start()` fails, `SubjectInitializer` still keeps the manager and its callback, but no consumer receives the pointer, so the USB tab and the image-import section stay hidden.

---

## USB Label Printers

The protocol side is in [LABEL_PRINTER_SYSTEM.md](LABEL_PRINTER_SYSTEM.md). The USB-specific parts:

**Detection** is libusb bus enumeration filtered by a VID:PID table (`src/system/usb_printer_detector.cpp#"static const std::vector<KnownUsbPrinter> s_known_printers"`):

```cpp
static const std::vector<KnownUsbPrinter> s_known_printers = {
    {0x0493, 0x8760, "Phomemo M110"},
    // Future printers added here
};
```

The table lists only devices the `usblp` transport can drive. `0483:5740` is ST's stock virtual COM port id: a CDC-ACM device gets a tty, never a `/dev/usb/lpN` node, and unrelated STM32 boards share it.

`scan()` is static and synchronous: it creates and destroys a fresh `libusb_context` each call and opens matching devices only to read the serial string. `start_polling()` runs an `lv_timer` (3s default) whose tick submits `scan()` to `HttpExecutor::fast()`, one scan at a time; the result comes back through the detector's lifetime token, and the callback fires on the UI thread on the first scan and on any change in the VID/PID/bus/address set. `print_spool_label()` scans once per USB print through `scan_async()`, the same lane-and-queue hop as a static call. The label printer settings overlay starts polling while the transport is USB and stops it on deactivate; it auto-selects the first printer found when no VID is saved yet.

**Settings** are `/label_printer/usb_vid`, `/label_printer/usb_pid`, `/label_printer/usb_serial`, with `printer_type` = `"usb"` (subject value 1). A USB printer counts as configured when VID and PID are both non-zero.

**Transport.** Despite the libusb detection, printing does not use libusb. `PhomemoPrinter::print` spawns a detached thread that maps the VID:PID to a kernel `usblp` node by reading `/sys/class/usbmisc/lp{0..7}/device/../idVendor` and `idProduct`, then writes the raster with an `std::ofstream` (`src/system/phomemo_printer.cpp#find_usblp_device`). The sysfs ids go through `PhomemoPrinter::read_sysfs_usb_id`, which returns 0 for a missing, empty or malformed value rather than throwing on the detached thread. The result comes back through `helix::ui::queue_update()`. The thread spawn is wrapped in `try`/`catch` because `pthread_create` can fail with `EAGAIN` on small ARM boards.

**Dispatch.** `print_spool_label()` rescans the bus off the UI thread before each USB print. If the configured VID:PID is missing but another known printer is present, it prints to that one instead (`src/system/label_printer_utils.cpp#print_spool_label`). `friendly_label_printer_error()` maps `/dev/usb` open failures to "USB printer access denied".

---

## HID Barcode Scanners

[architecture/13-peripherals.md](architecture/13-peripherals.md) covers the scanner model, keymaps and the QR overlay race. USB-specific points:

- Device choice is in `UsbScannerMonitor::find_scanner_devices` (`src/system/usb_scanner_monitor.cpp#"Prefer a paired Bluetooth HID scanner"`). A paired Bluetooth scanner wins; otherwise `input::find_hid_keyboard_devices()` picks the first HID keyboard matching the saved `/scanner/usb_vendor_product` (display name in `/scanner/usb_device_name`), skipping anything in the `input/device_blacklist`.
- USB scanners are read passively, never `EVIOCGRAB`bed, because some stall waiting for the kernel to acknowledge their Caps Lock LED toggle. Their keystrokes can therefore also reach a focused text widget.
- Hotplug is polling: with no device open, the thread re-runs discovery every 5s; a read error closes everything and re-runs discovery immediately.

---

## Mock Mode and Testing

### `--test`

`RuntimeConfig::should_mock_usb()` returns `test_mode`, so every `--test` run uses `UsbBackendMock` (when the build has `HELIX_ENABLE_MOCKS`). There is no `HELIX_MOCK_*` variable for USB. 1.5s after `start()`, the mock inserts one drive, `PRINT_FILES` at `/media/usb0`, with six fake files (`src/api/usb_backend_mock.cpp#add_demo_drives`), two of them under `projects/`. The files do not exist on disk, so their cards show the default thumbnail, their detail view falls back to the thumbnail at once (the log names the `/media/usb0` path it could not read), and printing one copies an empty file: the mock's `upload_file_from_path` records the upload and succeeds, and the mock Moonraker then starts `usb_prints/<filename>`. Within the 3s startup window the insert produces no toast, but the Printer/USB tabs appear on the print select panel.

The mock Moonraker can pretend a `gcodes/usb` symlink exists: `mock_set_usb_symlink_active(true)` (declared in `include/moonraker_client_mock.h`) makes `server.files.list` for `usb` return `usb/test_usb_file.gcode`. Only tests call it.

### Unit tests

| File | Tag | Covers |
|------|-----|--------|
| `tests/unit/test_usb_backend.cpp` | `[usb_backend]`, `[usb_manager]` | Mock lifecycle, simulated insert/remove, callbacks, factory |
| `tests/unit/test_usb_backend_linux.cpp` | `[usb_backend][linux]` | `is_usb_mount` filesystem filter, idempotent start/stop |
| `tests/unit/test_usb_automount.cpp` | `[usb_automount]` | Ladder order, grace, probe, ownership, lazy unmount, cooldown, cache |
| `tests/unit/test_print_select_usb_visibility.cpp` | (XML fixture) | `source_selector` bindings |
| `tests/unit/test_print_select_usb_multi_drive.cpp` | `[usb][multi_drive]`, `[usb][thumbnail]`, `[usb][usb_async]` | Scanning every drive, surviving the loss of one, per-path thumbnail keys, the off-thread scan and its stale-result rules |
| `tests/unit/test_print_select_usb_print.cpp` | `[usb][usb_print]`, `[usb][usb_delete]` | Print and Add to Queue copy the stick file to `usb_prints/` and use the copy; reuse, suffixing and listing errors; a failed copy starts nothing; no delete for a USB file |
| `tests/unit/test_detail_gcode_download_integrity.cpp` | `[detail_view][usb]` | The detail view reads a USB file from the stick and asks Moonraker for nothing |
| `tests/unit/test_single_flight_walk.cpp` | `[usb][usb_async]` | `SingleFlightWalk`: delivery through the UI queue, newest wins, cancel |
| `tests/unit/test_printer_image_usb_import.cpp` | `[usb][usb_async]` | The printer image overlay walks the stick off the UI thread |
| `tests/unit/test_usb_copy_name.cpp` | `[usb][usb_copy_name]` | `choose_usb_copy_target` naming rule |
| `tests/unit/test_metadata_and_usb_symlink.cpp` | `[usb][symlink]` | Moonraker symlink access and source switching |
| `tests/unit/test_usb_printer_detector.cpp` | `[label-printer][usb-detect]` | Known-printer table lookups, polling and the spool-label scan off the UI thread |
| `tests/unit/test_usb_scanner_monitor.cpp` | `[usb_scanner]` | Keymaps, Spoolman pattern parsing |
| `tests/unit/test_interface_drift_usb.cpp` | `[compile][drift]` | `UsbBackendMock` still satisfies `UsbBackend` |

```bash
make t F='[usb_automount]'
make t F='[usb_backend]'
```

`UsbAutomount` decisions are tested against `helix::test::FakeMountOps` (`tests/test_helpers/fake_mount_ops.h`), which records every mount and unmount call and lets a test choose which `(fs, options)` pairs the "kernel" accepts. `tests/test_helpers/usb_backend_linux_test_access.h` reaches into `UsbBackendLinux` privates.

The test binary sets `HELIX_USB_AUTOMOUNT=0` from a startup constructor before any test runs (`tests/test_main.cpp#"helix_disarm_test_usb_automount"`). CI containers run as root, and a root test run must never issue real `mount(2)` calls against the host's drives. A test that wants a live automounter injects one with fake ops instead of going through `create()`.

### On a device

Run with `-vv` and look for `[UsbBackendLinux] Started (mode=mountinfo-poll)`, then `Drive inserted:` on plug-in. `mode=content-poll` means the mountinfo event path was unavailable. With automount armed, the `[UsbAutomount] Mounted ...` line should precede the insert.

---

## Extending

### Accepting a new mount location

Add the prefix to `UsbBackendLinux::is_usb_mount_point`. That function is the shared contract: the automounter's mount point must stay inside it, and any vendor mounter that puts sticks elsewhere is invisible until its prefix is listed. Remember that only `/media/` gets the "no sysfs evidence" fallback in `is_usb_mount`.

### Accepting a new filesystem

Add it to the `is_usb_fs` list in `is_usb_mount`. If the automounter should also try it, add a ladder entry in `automount_ladder()` and update the ordering assertions in `tests/unit/test_usb_automount.cpp`.

### Another consumer of drive events

`UsbManager` holds exactly one `DriveCallback`, and `set_drive_callback()` replaces it. Do not call it from a second place. Extend the lambda in `SubjectInitializer::init_usb_manager` to fan out to the new consumer, marshal with `helix::ui::queue_update()`, and give it a lifetime guard the way the print select panel has one. Pull-style consumers (like the printer image overlay) can instead read `get_drives()` from the UI thread when they activate, and walk the drive through a `SingleFlightWalk`.

### A new platform backend

Implement `UsbBackend`, return it unstarted from `UsbBackend::create()` under the right preprocessor guard, and fire `EventCallback` from your own thread with the backend mutex released. Do not hold the mutex across the `scan_for_gcode()` walk. `get_connected_drives()` should return a cached list rather than touching the disk, since the UI calls it synchronously.

### A new USB label printer

Add the VID:PID to `s_known_printers`. If it speaks something other than Phomemo raster over `usblp`, `print_spool_label()` and `get_sizes_for_current_printer()` in `src/ui/ui_settings_label_printer.cpp` both need a branch, since USB currently means Phomemo in both places.
