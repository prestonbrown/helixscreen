# Bluetooth System (Developer Guide)

How HelixScreen talks to Bluetooth: the runtime-loaded plugin and its C ABI, the bus thread that owns the BlueZ D-Bus connection, how discovery and pairing work, the two data paths (RFCOMM and BLE GATT), and how the label printer and barcode scanner features consume all of it.

**Read first**: [Architecture chapter 13, "Bluetooth: a runtime-loaded plugin on one bus thread"](architecture/13-peripherals.md#bluetooth-a-runtime-loaded-plugin-on-one-bus-thread) for the one-page model. This doc is the deeper reference behind it.

**Main consumer**: [LABEL_PRINTER_SYSTEM.md](LABEL_PRINTER_SYSTEM.md) covers the printer protocols (Brother QL/PT, Phomemo, Niimbot, MakeID) that ride on top of this layer. Nothing about wire formats or label rendering is repeated here.

---

## Architecture Overview

```
 main binary (helix-screen)                       libhelix-bluetooth.so (dlopen)
 ─────────────────────────                        ─────────────────────────────
 LabelPrinterSettingsOverlay ─┐                   helix_bt_get_info / init / deinit
 BarcodeScannerSettingsOverlay┤                   helix_bt_discover / enumerate_known
 rfcomm_send() (bt_print_utils)├─> BluetoothLoader ─> helix_bt_pair / is_paired / is_bonded ...
 Phomemo / Niimbot / MakeID   │    (::instance(),     helix_bt_connect_rfcomm / sdp_find_rfcomm_channel
 Brother QL / PT backends    ─┘     C fn-pointer      helix_bt_connect_ble / ble_write / ble_read
                                    table)            helix_bt_disconnect / lzo_compress
                                                              │
                                                      helix_bt_context (one per init())
                                                        ├─ sd_bus* (system bus)
                                                        ├─ BusThread  ── owns every sd_bus_* call
                                                        ├─ BlueZ Agent1 at /helix/bt/agent
                                                        ├─ tracked RFCOMM fds
                                                        └─ BLE connections (handles tagged kBleHandleTag)
```

The main binary never links libsystemd or libbluetooth for Bluetooth. Everything that touches BlueZ lives in the plugin, and the app reaches it only through the function pointers that `BluetoothLoader` resolves. A device without Bluetooth hardware, or a build without the plugin, gets `is_available() == false` and no library is loaded.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/bluetooth_plugin.h` | The C ABI: `HELIX_BT_API_VERSION`, `helix_bt_device`, every function-pointer typedef and `HELIX_BT_SYM_*` symbol name |
| `include/bluetooth_loader.h` / `src/system/bluetooth_loader.cpp` | `BluetoothLoader` singleton: hardware check, `dlopen`, version check, symbol resolution, `get_or_create_context()` |
| `src/bluetooth/bt_context.h` | Private `helix_bt_context` struct (bus, bus thread, agent slot, discovery lock, RFCOMM fd set, BLE connection table), the BLE handle tag, and the adapter/device path helpers. Never seen by the app |
| `src/bluetooth/bt_bus_thread.h` / `.cpp` | `BusThread`: the single thread that owns the `sd_bus*` |
| `src/bluetooth/bt_plugin.cpp` | `helix_bt_get_info`, `helix_bt_init`, `helix_bt_deinit`, `helix_bt_last_error`, `device_dbus_path` |
| `src/bluetooth/bt_agent.cpp` | BlueZ `Agent1` (NoInputNoOutput, auto-accept) so "Just Works" pairing bonds |
| `src/bluetooth/bt_discovery.cpp` | `helix_bt_discover`, `helix_bt_enumerate_known`, `helix_bt_stop_discovery`, `find_adapter_path`, device filtering |
| `src/bluetooth/bt_pairing.cpp` | `helix_bt_pair`, `is_paired` / `is_bonded` / `is_connected`, `helix_bt_remove_device` |
| `src/bluetooth/bt_rfcomm.cpp` | `helix_bt_connect_rfcomm`: a plain `AF_BLUETOOTH` RFCOMM socket |
| `src/bluetooth/bt_sdp.cpp` | `helix_bt_sdp_find_rfcomm_channel`: SDP lookup of the RFCOMM channel for a UUID16 |
| `src/bluetooth/bt_ble.cpp` | BLE GATT connect, chunked write, notification read, and the unified `helix_bt_disconnect` |
| `src/bluetooth/bt_lzo.cpp` | `helix_bt_lzo_compress`: miniLZO wrapper for the MakeID protocol |
| `include/bt_discovery_utils.h` | Printer brand table (`kKnownBrands`), printer UUID prefixes, `name_suggests_ble()` |
| `include/bt_scanner_discovery_utils.h` | HID scanner classification (`0x1124` classic HID, `0x1812` HID-over-GATT, brand names) and `classify_device()`, the printer/scanner traits discovery reports |
| `include/bt_discovery_run.h` / `src/system/bt_discovery_run.cpp` | `SharedContext` (a plugin context co-owned by an overlay and its workers) and `DiscoveryRun` (one scan on a worker, reported on the UI thread) |
| `include/bt_print_utils.h` / `src/system/bt_print_utils.cpp` | `rfcomm_send()`, `rfcomm_send_receive()`, `resolve_label_printer_channel()` |
| `src/ui/settings/ui_settings_label_printer.cpp` | Label printer settings: scan, pair, forget a BT printer |
| `src/ui/settings/ui_settings_barcode_scanner.cpp` | Barcode scanner settings: list known scanners, scan, pair (HID bonding), forget |
| `mk/bluetooth.mk` | Builds `libhelix-bluetooth.so` when the dependencies exist |
| `lib/minilzo/` | miniLZO, compiled into the plugin only |

---

## Build Flags and Platforms

`src/bluetooth/*.cpp` is excluded from the app (`Makefile#"APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/bluetooth/*.cpp),$(APP_SRCS))"`) and built by `mk/bluetooth.mk` into `build/<target>/bin/libhelix-bluetooth.so`, beside the binary, as part of `make all` (the `bluetooth-plugin` goal in `mk/rules.mk`).

The plugin is built only when both libraries are found (`mk/bluetooth.mk#"BT_AVAILABLE := yes"`):

| Build | How the dependency check runs |
|-------|-------------------------------|
| Native | `pkg-config --exists libsystemd` and `pkg-config --exists bluez` |
| Cross (`CROSS_COMPILE` set) | `libbluetooth.so`/`.a` and `libsystemd.so`/`.a` present under `/usr/lib/$(TARGET_TRIPLE)/` |

Otherwise the build prints `Bluetooth plugin: skipped (missing libbluetooth-dev or libsystemd-dev)` and carries on.

**Which platforms ship it.** Only the toolchains that install `libbluetooth-dev` produce the plugin: the `docker/Dockerfile.pi` (arm64), `docker/Dockerfile.pi32` (armhf) and `docker/Dockerfile.x86` images, plus CI's native jobs in `.github/workflows/build.yml`. The MIPS, AD5M, CC1 and other buildroot/vendor targets have no libbluetooth in their sysroot, so their releases contain no plugin and Bluetooth is simply absent there. `release-package` (`mk/cross.mk#"define release-package"`) copies the `.so` from `build/<target>/bin/` into the release's `bin/` when it exists, and `make deploy-*` rsyncs it to the device's `bin/`.

A separate gate, `HELIX_HAS_LABEL_PRINTER` (default `1`, forced to `0` on AD5M-class targets in `mk/cross.mk`), compiles out `bt_print_utils.cpp` and the label printer backends. It does not affect the plugin build.

**Plugin location at runtime.** The loader looks for `libhelix-bluetooth.so` in the same directory as the running executable (`/proc/self/exe`). Every build puts it there. The loader first requires an `hci*` entry under `/sys/class/bluetooth/`, then asks `bluetooth_enabled()` (`src/system/bluetooth_loader.cpp#bluetooth_enabled`): `HELIX_BLUETOOTH=0` never loads it, `1` always does, and otherwise production loads it while a `--test` run does not, so a mock run never opens the developer's system bus or registers a BlueZ agent there. `helix-tests` pins `0` ([ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md)).

---

## Plugin Loading and the C ABI

The ABI is plain C so the plugin can be built and shipped separately from the app without C++ name-mangling or STL layout agreements. `BluetoothLoader::try_load()` (`src/system/bluetooth_loader.cpp#try_load`) resolves `helix_bt_get_info` first and refuses a plugin whose `api_version` differs from the one the app was compiled against:

```cpp
// src/system/bluetooth_loader.cpp
auto* info = get_info();
if (!info || info->api_version != HELIX_BT_API_VERSION) {
    spdlog::warn("[BluetoothLoader] API version mismatch: expected {}, got {}",
                 HELIX_BT_API_VERSION, info ? info->api_version : -1);
    dlclose(dl_handle_);
    dl_handle_ = nullptr;
    return false;
}
```

After the version check every other symbol is resolved with `dlsym`. Only `init` and `deinit` are required; any other missing symbol logs `Missing symbol: <name>` and leaves that pointer null. That is why consumers check the specific pointer before calling it (`if (!loader.is_available() || !loader.pair)`), and why `resolve_label_printer_channel()` falls back to a configured channel when `sdp_find_rfcomm_channel` is null.

Return conventions across the ABI:

| Kind | Convention |
|------|------------|
| Actions (`discover`, `pair`, `remove_device`, `ble_write`, `sdp_find_rfcomm_channel`) | `0` on success, negative errno on failure |
| Queries (`is_paired`, `is_bonded`, `is_connected`) | `1` / `0`, or negative errno |
| Connects | RFCOMM returns the socket fd; BLE returns a handle with `kBleHandleTag` (`0x40000000`) set. `connect_rfcomm` refuses an fd carrying that bit, so the two can never collide |
| Errors | Every failure path stores a string in `ctx->last_error`; read it with `last_error(ctx)` |

Strings handed to a discovery callback (`helix_bt_device::mac`, `name`, `service_uuid`) point into plugin-owned temporaries and are only valid for the duration of the callback. Copy them, as both settings overlays do.

The plugin logs with `fprintf(stderr, "[bt] ...")`, not through the app's spdlog sinks, so its lines appear on the process's stderr and not in the app log file. Look for the `[bt]` prefix when reading a device's console output.

---

## Contexts

`helix_bt_init()` (`src/bluetooth/bt_plugin.cpp#"helix_bt_init(void)"`) allocates one `helix_bt_context`: it opens its own system-bus connection, sets a 5-second method-call timeout, starts a `BusThread`, and registers the pairing agent.

```cpp
// src/bluetooth/bt_plugin.cpp (helix_bt_init)
int r = sd_bus_open_system(&ctx->bus);
...
sd_bus_set_method_call_timeout(ctx->bus, 5 * 1000000ULL); // microseconds

ctx->bus_thread = std::make_unique<helix::bluetooth::BusThread>(ctx->bus);
ctx->bus_thread->start();
...
helix_bt_register_agent(ctx);
```

`BluetoothLoader::get_or_create_context()` hands out one lazily created, process-wide context, which `~BluetoothLoader` deinits at exit. Not every consumer uses it. The current owners are:

| Consumer | Context |
|----------|---------|
| `rfcomm_send()` / `rfcomm_send_receive()` / `resolve_label_printer_channel()` | Shared (`get_or_create_context()`) |
| MakeID backend, Brother PT backend | Shared |
| Phomemo backend, BLE path | Private: `init()` and `deinit()` around each print |
| Niimbot backend | Private, persistent static `s_ctx`, rebuilt when the connection dies (`src/system/niimbot_bt_printer.cpp#ensure_connected`) |
| `LabelPrinterSettingsOverlay`, `BarcodeScannerSettingsOverlay` | Private `SharedContext` each (`include/bt_discovery_run.h#SharedContext`): the first worker that needs it calls `init()`, and `deinit()` runs once the overlay and every worker holding it have let go |

Each `init()` registers its own `Agent1` and asks to be the default agent, so the most recent context wins default-agent status. The MakeID backend uses the shared one because a second context on an RFCOMM link the UI already established fails with `ECONNABORTED` (`src/system/makeid_bt_printer.cpp#"loader.get_or_create_context()"`). New code should use `get_or_create_context()` unless it has a measured reason to own a bus connection.

`get_or_create_context()` (`src/system/bluetooth_loader.cpp#get_or_create_context`) is thread-safe: print workers and the Forget workers call it concurrently, and one mutex makes the first callers share a single `init()`. A failed `init()` leaves no context, so the next caller retries.

---

## Threading Model

The repo-wide rules are in [THREADING.md](THREADING.md); its table in section 8 routes "sd-bus / BlueZ DBus call" to `BusThread::run_sync`. This is what that means in practice.

### BusThread

sd-bus connections are not thread-safe, so each context's `BusThread` (`src/bluetooth/bt_bus_thread.h#"class BusThread {"`) is the only thread that calls `sd_bus_*` on that connection. Its loop drains queued work items, runs `sd_bus_process()`, then polls the bus fd plus a wakeup pipe for up to 500 ms (`src/bluetooth/bt_bus_thread.cpp#loop`). `submit()` writes a byte to the pipe so queued work runs promptly even with no D-Bus traffic.

Every public plugin function that needs the bus wraps its D-Bus work in `run_sync`:

```cpp
// src/bluetooth/bt_bus_thread.cpp
void BusThread::run_sync(BusWork work) {
    if (on_thread()) {
        work(bus_);
        return;
    }
    submit(std::move(work)).get();
}
```

Rules that follow from this:

- **Work submitted from the bus thread runs inline.** `submit()` and `run_sync()` detect `on_thread()` and execute immediately, so a signal handler that calls back into the plugin does not deadlock on its own future.
- **A stopped thread breaks promises.** After `stop()`, or when `sd_bus_process()` fails and the loop exits, queued items get a `std::runtime_error` (`src/bluetooth/bt_bus_thread.cpp#drain_and_break_promises_locked`). Every plugin entry point catches it and returns `-EIO` with `last_error` set, so a dead bus thread surfaces as an ordinary error code.
- **A null bus is legal.** `BusThread(nullptr)` runs the queue and skips all sd-bus calls. The unit tests rely on this.

### Which thread runs what

| Operation | Runs on |
|-----------|---------|
| `discover()` | Blocks the **calling** thread for up to `timeout_ms`, polling every 100 ms. D-Bus setup and teardown go through `run_sync` |
| Discovery and `enumerate_known()` callbacks | The **bus thread**. `InterfacesAdded` fires from `sd_bus_process()`, and the known-device walk runs inside `run_sync` |
| `pair()`, `is_*()`, `remove_device()`, `connect_ble()` | Calling thread blocks while the D-Bus calls run on the bus thread. Each call can take up to the 5 s method timeout; `pair()` makes several in a row |
| `connect_rfcomm()`, `sdp_find_rfcomm_channel()` | Entirely on the **calling** thread (plain sockets and libbluetooth, no sd-bus) |
| `ble_write()` | Calling thread. Writes straight to the `AcquireWrite` fd when there is one; otherwise one `WriteValue` `run_sync` per chunk |
| `ble_read()` | Calling thread, waiting on the connection's notification queue (filled by a bus-thread signal handler) or polling the `AcquireNotify` fd |

Because almost every call blocks, the app never calls the plugin from the LVGL thread. Consumers spawn a worker (`try { std::thread(...).detach(); } catch (const std::system_error&)`, per THREADING.md section 8) and marshal results back with `helix::ui::queue_update()` or a lifetime token's `defer()`.

Both settings overlays scan through `DiscoveryRun` (`include/bt_discovery_run.h#DiscoveryRun`). It owns the parts that outlive any one overlay call: the per-scan state lives in a `shared_ptr` the worker co-owns, so a Stop, a rescan or the overlay's destruction never frees what the worker or its queued callbacks still read. The overlay supplies three callbacks:

```cpp
// src/ui/settings/ui_settings_barcode_scanner.cpp (start_bt_discovery)
helix::bluetooth::DiscoveryRun::Callbacks callbacks;
callbacks.accept = [](const helix_bt_device& dev) { return dev.is_scanner; };  // bus thread
callbacks.on_device = [this](const helix::bluetooth::DiscoveredDevice& found) { ... };  // UI thread
callbacks.on_finished = [this](bool context_ok) { ... };  // UI thread
bt_discovery_.start(bt_ctx_, 15000, lifetime_.token(), std::move(callbacks));
```

`accept` runs on the bus thread inside the plugin's callback; `DiscoveryRun` copies each accepted device's strings before the callback returns and defers `on_device` through the token. `cancel()` silences the scan's remaining callbacks and sets that scan's own cancel flag, which the plugin reads even before the scan has started; other scans on the context (the label printer's rediscover-before-pair, say) keep running. A later `start()` cancels the scan it replaces the same way, then gets fresh state. `on_finished(false)` means the scan never ran: `SharedContext::get()` could not create a context, or `discover()` failed (no adapter, `StartDiscovery` refused; the plugin's `last_error` is logged). Both overlays then show "Could not start Bluetooth discovery" over their empty list.

The barcode scanner overlay seeds its list from `enumerate_known()` the same way: the saved scanner shows at once, and BlueZ's known scanners merge in from a worker (`src/ui/settings/ui_settings_barcode_scanner.cpp#seed_known_bt_devices`).

### Teardown order

`helix_bt_deinit()` (`src/bluetooth/bt_plugin.cpp#"helix_bt_deinit(helix_bt_context* ctx)"`) unregisters the agent, unrefs BLE notification slots and the discovery slot **through the bus thread**, stops and joins the thread, then closes BLE acquired fds and tracked RFCOMM fds, and only then flushes and closes the bus. Slot unrefs are sd-bus calls, so they have to happen while the bus thread is still alive.

---

## Discovery

`helix_bt_discover()` (`src/bluetooth/bt_discovery.cpp#"helix_bt_discover(helix_bt_context* ctx"`) holds the context's `discover_mutex` for the whole scan, so two scans on one context run one after the other instead of sharing its slot and flag. It runs in four steps:

1. On the bus thread, find the adapter (first object exposing `org.bluez.Adapter1` in `GetManagedObjects`), report every device BlueZ already knows, add an `InterfacesAdded` match, and call `Adapter1.StartDiscovery` (`InProgress` counts as success).
2. On the caller's thread, sleep in 100 ms steps until `timeout_ms` elapses, the caller's `cancel` flag goes nonzero (that scan only), or `helix_bt_stop_discovery()` bumps `ctx->discover_stop_gen` (every scan on the context). Both are checked again once the mutex is taken, so a scan cancelled or stopped while it was queued ends at once.
3. On the bus thread, `StopDiscovery` and unref the match.
4. Return `0`.

`helix_bt_enumerate_known()` is step 1 without the scan: it reports devices BlueZ has cached (paired or previously seen) and returns immediately. The barcode scanner overlay uses it to fill its dropdown before the user presses Scan.

### Filtering and classification

`parse_device_properties()` (`src/bluetooth/bt_discovery.cpp#parse_device_properties`) reads `Address`, `Name`/`Alias`, `Paired` and `UUIDs` from each `org.bluez.Device1` and reports a device only if at least one of these holds:

| Reason | Test |
|--------|------|
| Printer UUID | `is_label_printer_uuid()`: SPP `00001101`, Phomemo `0000ff00`, Niimbot `e7810a71`, MakeID `0000abf0` prefixes |
| Printer name | `is_likely_label_printer()` against the brand table `kKnownBrands` (`include/bt_discovery_utils.h#"kKnownBrands[]"`) |
| Already paired | `Paired` is true and the device has a real name |
| Scanner | A HID UUID (`0x1124` or `0x1812`, `include/bt_scanner_discovery_utils.h#is_hid_scanner_uuid`) or `is_likely_bt_scanner()` on the name |

`classify_device()` (`include/bt_scanner_discovery_utils.h#classify_device`) computes the printer-UUID, printer-name and scanner traits, and the unit tests call the same function.

Everything else is dropped with a `[bt] filtered:` line. Two fields are then derived:

- `is_ble` comes from the matching printer UUID, but the brand table overrides it: a name whose brand is marked BLE-only (Niimbot, MakeID/YichipFPGA, Supvan) is reported as BLE even when it also advertises SPP.
- `is_scanner` is set only when the device matched as a scanner and not as a printer:

```cpp
// include/bt_scanner_discovery_utils.h (DeviceTraits)
bool is_scanner() const { return scanner && !printer_uuid && !printer_name; }
```

`service_uuid` carries the matching **printer** UUID, or null. A HID UUID never lands there, so a consumer that wants to know whether a device is a scanner reads `is_scanner`, as both overlays do.

Dual-mode printers (a Niimbot D110 is one) can show up as two BlueZ entries with the same name, one per transport. The label printer overlay keeps the entry whose transport matches the brand table and drops the other.

---

## Pairing

### The agent

BlueZ only stores link keys when an agent is registered. `helix_bt_register_agent()` (`src/bluetooth/bt_agent.cpp#"helix_bt_register_agent(helix_bt_context* ctx)"`) exports `org.bluez.Agent1` at `/helix/bt/agent` with capability `NoInputNoOutput`, registers it with `AgentManager1`, and requests default-agent status (failure there is non-fatal). Every method auto-accepts: `RequestConfirmation`, `RequestAuthorization` and `AuthorizeService` all reply success. There is no PIN or passkey UI, so a device that insists on a PIN cannot pair.

### `helix_bt_pair()`

`src/bluetooth/bt_pairing.cpp#"helix_bt_pair(helix_bt_context* ctx, const char* mac)"` does everything inside one `run_sync`:

1. Log the device's pre-pair `Paired`/`Connected`/`Trusted`/`Bonded` state.
2. `Device1.Pair()`. On success, or on `AlreadyExists`, set `Trusted=true` and then bring up a profile: `ConnectProfile(HID UUID)` first, falling back to a bare `Connect()` when the device reports `NotSupported` or `DoesNotExist`. Requesting HID explicitly matters for dual-profile scanners that also advertise SPP, where a bare `Connect()` can pick SPP and no evdev node appears.
3. On any other `Pair()` failure, assume a BLE device that does not support classic pairing and call `Device1.Connect()`, which bonds implicitly on LE. Success or `AlreadyConnected` sets `Trusted` and returns `0`.
4. If both fail, `last_error` gets the `Connect()` error message.

`device_dbus_path()` (`src/bluetooth/bt_plugin.cpp#device_dbus_path`) maps `AA:BB:...` to `<adapter>/dev_AA_BB_...` under the adapter `find_adapter_path()` reports, the same lookup discovery uses, falling back to `/org/bluez/hci0` when BlueZ lists none. Pairing, property queries, `remove_device()` (which calls `RemoveDevice` on that adapter) and `connect_ble()` all go through it.

### After pairing

The two settings overlays check different things once `pair()` returns:

| Overlay | Post-pair check | Outcome |
|---------|-----------------|---------|
| Label printer | `is_paired()` and `is_connected()` | Saves `bt_address`, `bt_name` and `bt_transport` (`"ble"` or `"spp"`) to `LabelPrinterSettingsManager` |
| Barcode scanner | `is_paired()`, `is_bonded()`, then polls up to 5 s for the scanner's HID input device | When no HID device appears and the scanner is not bonded, it is removed again with `remove_device()` so the next attempt starts clean; the toast wording depends on `is_bonded()` (`src/ui/settings/ui_settings_barcode_scanner.cpp#"ldr.is_bonded"`) |

`is_bonded()` exists because "Just Works" SSP can leave a device paired without a stored long-term key, and BlueZ's input plugin refuses HID for unbonded devices.

`helix_bt_remove_device()` (`src/bluetooth/bt_pairing.cpp#"helix_bt_remove_device(helix_bt_context* ctx"`) does a best-effort `Device1.Disconnect`, then `Adapter1.RemoveDevice`, each with its own 5 s timeout so a device stuck mid-connect fails fast. Both overlays' Forget buttons call it.

---

## Data Paths

### RFCOMM (Bluetooth Classic SPP)

`helix_bt_connect_rfcomm()` opens an `AF_BLUETOOTH`/`BTPROTO_RFCOMM` socket with a 10 s send timeout, connects to the given channel, records the fd in `ctx->rfcomm_fds`, and returns it. The caller writes to it like any socket and releases it with `disconnect(ctx, fd)`.

App code does not call this directly; it goes through `rfcomm_send()` (`src/system/bt_print_utils.cpp#rfcomm_send`), which owns the whole lifecycle: resolve the channel, connect, write until done, sleep 5 s so the kernel buffer drains over the air, disconnect. One static mutex serializes every RFCOMM print in the process. The channel comes from `resolve_label_printer_channel()` (`src/system/bt_print_utils.cpp#resolve_label_printer_channel`): the cached value in `LabelPrinterSettingsManager` if it is 1 to 30, else an SDP lookup of SPP (`0x1101`) whose answer is cached, else the caller's fallback, which is never cached. A connect failure on a cached channel invalidates the cache and retries once:

```cpp
// src/system/bt_print_utils.cpp (rfcomm_send)
bool channel_was_cached = (cached_before == channel && cached_before > 0);
if (channel_was_cached && attempt == 0) {
    spdlog::warn("[{}] Cached channel {} failed ({}); invalidating and retrying",
                 log_tag, channel, err);
    helix::LabelPrinterSettingsManager::instance().set_bt_channel(0);
    continue;
}
```

`rfcomm_send_receive()` is the same lifecycle with a response read (500 ms settle, then up to 3 s of `poll`) for protocols that query printer status.

### BLE GATT

`helix_bt_connect_ble(ctx, mac, write_uuid)` (`src/bluetooth/bt_ble.cpp#"helix_bt_connect_ble(helix_bt_context* ctx"`):

1. `Device1.Connect()`. If BlueZ answers with a `br-connection-*` error (it tried BR/EDR on a device it cached as dual-mode), retry with `ConnectProfile(write_uuid)`, which forces LE.
2. Poll `ServicesResolved` for up to 10 s.
3. Find the characteristic whose UUID matches `write_uuid`, then try `AcquireWrite` (a raw fd plus the negotiated MTU). Without it, read the device MTU and fall back to one `WriteValue` D-Bus call per chunk.
4. Try `AcquireNotify` for responses. Without an fd, `StartNotify` plus a `PropertiesChanged` match feeds the connection's receive queue from the bus thread.
5. Store the `BleConnection` in the first free slot of the table and return `kBleHandleTag | slot`. Slots hold `shared_ptr`, so a read or write that looked a connection up keeps it alive while a disconnect frees the slot.

`ble_write()` splits data into `MTU - 3` byte chunks with a 10 ms gap between them. `ble_read()` waits up to `timeout_ms` on the receive queue, then does a zero-timeout poll of the notify fd; it returns the byte count, `0` on timeout, or a negative errno. A disconnect wakes a blocked reader, which returns `-ENOTCONN`.

`helix_bt_disconnect(ctx, handle)` (`src/bluetooth/bt_ble.cpp#"helix_bt_disconnect(helix_bt_context* ctx, int handle)"`) dispatches on the handle value: one carrying `kBleHandleTag` is a BLE connection (mark inactive and wake readers, close fds, unref the signal match, `Device1.Disconnect`, free the slot for the next connection), anything else is a tracked RFCOMM fd.

### LZO

`helix_bt_lzo_compress()` is not Bluetooth at all. It lives in the plugin so miniLZO is linked only where MakeID printing can happen. Size the output buffer at `in_len + in_len/16 + 64 + 3` bytes. Its work memory (`LZO1X_1_MEM_COMPRESS`, 128 KB on 64-bit) comes from the heap, not the caller's stack.

---

## Consumers

| Consumer | Uses | Notes |
|----------|------|-------|
| `LabelPrinterSettingsOverlay` | `discover`, `pair`, `is_paired`, `is_connected`, `remove_device` | Skips devices flagged `is_scanner`. See LABEL_PRINTER_SYSTEM.md for the settings it writes |
| `BarcodeScannerSettingsOverlay` | `enumerate_known`, `discover`, `pair`, `is_paired`, `is_bonded`, `remove_device` | Keeps only devices flagged `is_scanner`; the bonded HID device then feeds `UsbScannerMonitor` as an ordinary input device |
| Brother QL, Phomemo SPP | `rfcomm_send()` | |
| Brother PT, MakeID | Shared context + `connect_rfcomm` / `rfcomm_send_receive()`; MakeID also `lzo_compress` | |
| Phomemo BLE, Niimbot | `connect_ble`, `ble_write`, `ble_read` | Private contexts, see [Contexts](#contexts) |

All label-printer paths enter through `print_spool_label()`; the dispatch from configured transport and brand to backend is in [LABEL_PRINTER_SYSTEM.md](LABEL_PRINTER_SYSTEM.md).

---

## Failure Modes

| Symptom | Where to look |
|---------|---------------|
| No Bluetooth rows in settings | Log line `[BluetoothLoader] No Bluetooth hardware detected` (no `hci*` under `/sys/class/bluetooth`), `Disabled (HELIX_BLUETOOTH=0, or --test without HELIX_BLUETOOTH=1)`, or `Plugin not available` (`.so` missing next to the binary, or a platform that does not build it). At debug level `dlopen failed: ...` names the reason |
| `API version mismatch: expected N, got M` | The `.so` in `bin/` is from a different build than the binary. Redeploy both |
| Toast "Bluetooth initialization failed" | `init()` returned null: `[bt] failed to open system bus` on stderr means no D-Bus system bus or no permission to it |
| Pairing succeeds but the device does not stay paired | Check stderr for `[bt] agent: RegisterAgent failed`. Without an agent the bond never stores. Another agent (a running `bluetoothctl`) can also hold default-agent status |
| Scanner pairs but never types | No HID evdev node appeared within 5 s of pairing. If `is_bonded()` also returned 0, the overlay removes the device so a retry starts clean. The `[bt] pair:` lines show which of `Pair`/`ConnectProfile(HID)`/`Connect` succeeded |
| `RFCOMM connect failed: Connection refused` / `Host is down` | Wrong channel or printer asleep. A cached channel is invalidated and retried once automatically; SDP failure falls back to the backend's default channel |
| `timeout waiting for BLE services` | `ServicesResolved` never became true within 10 s, usually a printer that went to sleep or is connected to a phone |
| `GATT characteristic not found` | The device exposes no characteristic with the backend's write UUID: wrong brand match, or a dual-mode device's BR/EDR half was selected |
| Calls fail with `BusThread not running` / `BusThread exited` | `sd_bus_process` failed (`[bt] BusThread sd_bus_process error` on stderr), typically `bluetoothd` or `dbus` restarting. The context is dead; a private context is rebuilt on next use, the shared one is not |

---

## Testing and Mocking

There is no mock Bluetooth backend and no `HELIX_MOCK_*` variable for it: a `--test` run leaves the plugin unloaded, and `HELIX_BLUETOOTH=1` loads the real one on a machine with an adapter. What is tested:

| Test | Covers | Tag |
|------|--------|-----|
| `tests/unit/test_bt_bus_thread.cpp` | `BusThread` lifecycle, serialization, broken promises on stop, submit racing stop. Compiles `bt_bus_thread.cpp` directly into the test with a null bus; empty when `<systemd/sd-bus.h>` is absent | `[bt][slow]` |
| `tests/unit/test_bluetooth_loader.cpp` | Loader singleton and null pointers when unavailable | `[bluetooth]` |
| `tests/unit/test_bt_channel_resolver.cpp` | `resolve_label_printer_channel()` cache, SDP and fallback paths | `[bt][resolver]` |
| `tests/unit/test_bt_discovery_utils.cpp`, `test_bt_scanner_discovery_utils.cpp` | Brand table, UUID and name classifiers | `[bluetooth][discovery]`, `[bluetooth][scanner]` |
| `tests/unit/test_bt_device_classification.cpp` | The `is_scanner` decision matrix, through `classify_device()` | `[bluetooth][scanner][classification]` |
| `tests/unit/test_bt_ble_connections.cpp` | BLE handle tagging, slot reuse, an RFCOMM fd above 1000, a blocked `ble_read` woken by disconnect. Compiles the plugin sources into the test on a bus-less context | `[bt][ble]`, the wakeup case `[slow]` |
| `tests/unit/test_bt_discovery_run.cpp` | `DiscoveryRun` and `SharedContext` against fake loader entry points: cancel then rescan, a dead owner, init failure, deinit after the last worker | `[bt][discovery_run][slow]` |

The `[slow]` cases run in nightly CI, not in `make unit-sweep`; run them with `make t F='[bt]'`.

`BluetoothLoader`'s function pointers are public members, so a test can swap one for a fake and restore it afterwards. `test_bt_channel_resolver.cpp` does exactly that for `sdp_find_rfcomm_channel` (`tests/unit/test_bt_channel_resolver.cpp#"struct LoaderMock"`), and `test_bt_discovery_run.cpp` for `init`, `deinit` and `discover`. Use the same scoped-swap pattern to test consumer logic without hardware.

Anything that touches BlueZ (discovery, pairing, BLE connect) has to be verified on hardware: a Pi with the plugin deployed, or a Linux desktop whose BlueZ exposes an adapter, running a native build (with `HELIX_BLUETOOTH=1` under `--test`).

---

## Extension Guide

### Adding a function to the plugin ABI

1. Add the typedef and `HELIX_BT_SYM_*` name to `include/bluetooth_plugin.h`.
2. Implement it as `extern "C"` in the right `src/bluetooth/*.cpp` file. Put every `sd_bus_*` call inside `ctx->bus_thread->run_sync(...)`, catch `std::exception` around it and turn it into `-EIO`, and set `ctx->last_error` on every failure path.
3. Add the pointer to `BluetoothLoader` and resolve it in `try_load()`.
4. Bump `HELIX_BT_API_VERSION` when the change alters an existing signature or struct layout. A purely additive symbol works without a bump, since the loader tolerates missing optional symbols and callers null-check them.
5. Callers check the pointer before calling it.

### Teaching discovery a new printer brand

Add a row to `kKnownBrands` in `include/bt_discovery_utils.h` with the name prefix and the BLE-only flag. If the brand advertises its own service UUID, add the prefix to `is_label_printer_uuid()` and, for a BLE service, to `uuid_is_ble()`. Add cases to `test_bt_discovery_utils.cpp`. The protocol backend itself is a LABEL_PRINTER_SYSTEM.md job.

### Teaching discovery a new scanner brand

Add the name prefix to `kKnownScannerBrands` in `include/bt_scanner_discovery_utils.h` and a case to `test_bt_scanner_discovery_utils.cpp`. Brand names only matter for scanners that do not advertise a HID UUID.

### Writing a new consumer

- Get the context from `get_or_create_context()`, or hold a `SharedContext` when the consumer owns workers that can outlive it.
- To scan, use `DiscoveryRun` rather than a hand-written discovery thread.
- Never call the plugin from the LVGL thread. Spawn a worker inside `try`/`catch (const std::system_error&)` and report back through `helix::ui::queue_update()` guarded by a lifetime token (THREADING.md sections 2 and 8).
- Copy every string out of a `helix_bt_device` before the callback returns.
- For RFCOMM, call `rfcomm_send()` / `rfcomm_send_receive()` rather than opening sockets yourself; they hold the process-wide RFCOMM mutex and the channel cache.
- Read `last_error(ctx)` immediately after a failure, before any `deinit()` of that context.

---

## Related Docs

- [architecture/13-peripherals.md](architecture/13-peripherals.md): chapter-level model of Bluetooth alongside the other peripherals
- [LABEL_PRINTER_SYSTEM.md](LABEL_PRINTER_SYSTEM.md): printer protocols, rendering, and the settings that select a BT printer
- [THREADING.md](THREADING.md): `queue_update`, lifetime tokens, and the detached-thread rules every BT consumer follows
- [architecture/03-threading-lifetime.md](architecture/03-threading-lifetime.md): the `BusThread`/`HttpExecutor` submit and `run_sync` contract
