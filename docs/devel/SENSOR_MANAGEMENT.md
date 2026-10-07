# Sensor Management

HelixScreen tracks the printer's auxiliary sensors through a set of per-category singleton managers in `src/sensors/` (plus `FilamentSensorManager` in `src/print/`). Each manager recognizes its own Klipper objects, keeps a config and a runtime state per sensor, and publishes LVGL subjects that panels, home widgets and the Settings > Sensors overlay bind to. Extruder and bed temperatures are not part of this subsystem; they live in `PrinterState`.

This doc goes deeper than the architecture guide on the sensor managers themselves. For the surrounding machinery, read these first:

| Topic | Owner |
|-------|-------|
| How `printer.objects.list` becomes a `PrinterDiscovery` snapshot, and the six-row "which manager uses which discovery source" table | [architecture/06-discovery-capabilities.md](architecture/06-discovery-capabilities.md#the-sensor-framework-two-sources-six-managers) |
| The census of singletons, `StaticSubjectRegistry` self-registration, `SensorState` in the state map | [architecture/05-printer-state.md](architecture/05-printer-state.md) |
| `SubjectLifetime`, `ObserverGuard`, `observe<T>()` | [architecture/02-subjects-dataflow.md](architecture/02-subjects-dataflow.md) |
| Threading invariants, `AsyncLifetimeGuard`, `queue_update` | [THREADING.md](THREADING.md) |
| How AMS backends use filament sensors | [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md), [architecture/07-filament-ams.md](architecture/07-filament-ams.md) |
| Mains-voltage monitor (`helix::power_loss`) and recovery | [POWER_LOSS_RECOVERY.md](POWER_LOSS_RECOVERY.md) |

Two things share the word "sensor" and are unrelated. The managers below consume **Klipper** objects, config and status frames. `SensorState` (`include/sensor_state.h`) tracks **Moonraker's own** `[sensor]` components (`server.sensors.list`, `notify_sensor_update`), mostly power meters. It has its own section near the end.

## Key Files

### Managers

| Manager | Header / source | Klipper objects it claims | Namespace |
|---------|-----------------|---------------------------|-----------|
| `FilamentSensorManager` | `include/filament_sensor_manager.h`, `src/print/filament_sensor_manager.cpp` | `filament_switch_sensor <name>`, `filament_motion_sensor <name>` | `helix` |
| `TemperatureSensorManager` | `include/temperature_sensor_manager.h`, `src/sensors/temperature_sensor_manager.cpp` | `temperature_sensor <name>`, `temperature_fan <name>`, `heater_generic <name>`, `tmc2240 <name>`, `tmc5160 <name>` | `helix::sensors` |
| `HumiditySensorManager` | `include/humidity_sensor_manager.h`, `src/sensors/humidity_sensor_manager.cpp` | the chip table in `include/humidity_sensor_types.h` (`bme280`, `htu21d`, `sht3x`, `aht10`, `aht20`, `aht20_f`) | `helix::sensors` |
| `ProbeSensorManager` | `include/probe_sensor_manager.h`, `src/sensors/probe_sensor_manager.cpp` | `probe`, `bltouch`, `smart_effector`, `cartographer`, `beacon`, `probe_eddy_current <name>` | `helix::sensors` |
| `AccelSensorManager` | `include/accel_sensor_manager.h`, `src/sensors/accel_sensor_manager.cpp` | config sections for ADXL345, LIS2DW, LIS3DH, MPU9250, ICM20948, plus Beacon's onboard accelerometer | `helix::sensors` |
| `WidthSensorManager` | `include/width_sensor_manager.h`, `src/sensors/width_sensor_manager.cpp` | `tsl1401cl_filament_width_sensor`, `hall_filament_width_sensor` | `helix::sensors` |
| `LoadCellManager` | `include/load_cell_manager.h`, `src/sensors/load_cell_manager.cpp` | `load_cell`, `load_cell <name>` | `helix::sensors` |

Each manager has a matching `*_types.h` header (`filament_sensor_types.h`, `humidity_sensor_types.h`, `load_cell_types.h`, ...) holding its role and type enums, the `*Config` and `*State` structs, and the string conversions used for persistence and display.

### Plumbing and consumers

| File | Purpose |
|------|---------|
| `include/sensor_managers.h` | `for_each_sensor_manager()`: the one list of manager singletons, walked for subject init and the status fan-out |
| `src/printer/printer_discovery.cpp` | `init_subsystems_from_hardware()`: calls each manager's `discover()` from the Klipper object list |
| `src/api/moonraker_discovery_sequence.cpp` | Buckets sensor objects during object-list parsing, builds their status subscriptions, and queues the `configfile.config` discovery arm |
| `src/application/moonraker_manager.cpp` | `dispatch_status_frame()` hands every status frame to each manager after `PrinterState` |
| `src/application/subject_initializer.cpp` | `init_ams_subjects()` calls every manager's `init_subjects()` before any panel XML exists |
| `include/printer_hardware.h` | `PrinterHardware::is_ams_sensor()`: hides filament sensors an AMS backend owns |
| `src/ui/ui_settings_sensors.cpp`, `ui_xml/sensors_overlay.xml` | Settings > Sensors overlay, one section per manager |
| `src/ui/ui_wizard_filament_sensor_select.cpp` | First-run wizard step that assigns the RUNOUT role |
| `src/ui/ui_filament_runout_handler.cpp` | Runout guidance modal, driven by `filament_any_runout` and `has_real_runout()` |
| `src/ui/panel_widgets/thermistor_widget.cpp`, `humidity_widget.cpp`, `width_sensor_widget.cpp`, `filament_sensor_widget.cpp` | Home-panel widgets for each category |
| `include/sensor_state.h`, `src/printer/sensor_state.cpp` | Moonraker `[sensor]` components (separate universe, see below) |

## Architecture

```
Klipper objects.list ──► MoonrakerDiscoverySequence (bucket names, build subscription)
        │                         │
        │                         └─ configfile.config ──► queue_update ──► Probe/Accel discover_from_config()
        ▼
PrinterDiscovery ──► init_subsystems_from_hardware()
                         ├─ FilamentSensorManager::discover_sensors() + load_config_from_file()
                         ├─ TemperatureSensorManager::discover()
                         ├─ LoadCellManager::discover()
                         ├─ ProbeSensorManager::discover() + load_config_from_file()
                         ├─ HumiditySensorManager::discover()
                         └─ WidthSensorManager::discover() + load_config_from_file()

notify_status_update ──► dispatch_status_frame()
                              └─ for_each_sensor_manager(m.update_from_status(status))
                                     │  parse under the manager's mutex
                                     └─ lifetime_.token().defer ──► update_subjects() on the main thread
                                                                          │
                                       XML bind_* / observe<int>() ◄──────┘
```

### One list of managers

The managers share a method vocabulary, not a base class. What ties them together is a single template that names every singleton:

```cpp
// include/sensor_managers.h
template <typename Fn> void for_each_sensor_manager(Fn&& fn) {
    fn(FilamentSensorManager::instance());
    fn(HumiditySensorManager::instance());
    fn(WidthSensorManager::instance());
    fn(ProbeSensorManager::instance());
    fn(AccelSensorManager::instance());
    fn(TemperatureSensorManager::instance());
    fn(LoadCellManager::instance());
}
```

Two call sites walk it, so a manager added here gets both for free:

- `src/application/subject_initializer.cpp#init_ams_subjects` calls `m.init_subjects()` on each. Every `init_subjects()` registers its own `deinit_subjects()` with `StaticSubjectRegistry`, so shutdown ordering needs no extra wiring.
- `src/printer/printer_state.cpp#update_from_status` ends with `m.update_from_status(state)` on each, so every manager sees every status frame and picks out its own keys.

Discovery is deliberately not on that list, because each manager reads a different input (object names, config sections, or both).

### Discovery

`init_subsystems_from_hardware()` (`src/printer/printer_discovery.cpp#init_subsystems_from_hardware`) is the production entry point. The inputs differ per manager:

| Manager | Input | Persisted config loaded |
|---------|-------|-------------------------|
| Filament | `hardware.filament_sensor_names()`, only when `has_filament_sensors()` | `load_config_from_file()` |
| Temperature | `hardware.sensors()` | none |
| Load cell | `hardware.load_cells()` | none |
| Probe | `hardware.printer_objects()`, then `discover_from_config()` later | `load_config_from_file()` |
| Humidity | `hardware.printer_objects()` | none |
| Width | `hardware.width_sensor_objects()`, only when `has_width_sensors()` | `load_config_from_file()` |
| Accel | `discover_from_config()` only | none |

The `configfile.config` arm lives in `src/api/moonraker_discovery_sequence.cpp#continue_discovery_objects`. It posts a copy of the config to the main thread and hands it to `ProbeSensorManager::discover_from_config()` (which only seeds `z_offset` for probes already discovered, for modules whose status reports it as null) and `AccelSensorManager::discover_from_config()` (the only place accelerometers are found, since they have no `get_status`).

Every `discover()` has the same shape: clear `sensors_`, parse each name with a private `parse_klipper_name()`, push a default `*Config`, mark the `states_` entry `available`, mark vanished sensors unavailable, update the `*_count` subject, then run `update_subjects()`. Some managers also auto-assign roles from the sensor name at this point (see each manager below). Because `discover()` rebuilds `sensors_` from scratch, any role set at runtime and not persisted is gone after a reconnect.

### Subscriptions

A manager only sees fields that `MoonrakerDiscoverySequence::build_subscription_objects()` (`src/api/moonraker_discovery_sequence.cpp#build_subscription_objects`) asked for:

| Objects | Fields |
|---------|--------|
| temperature sensors, `temperature_fan`, TMC drivers, humidity chips | `temperature`, `humidity` (`temperature_fan` gets `temperature`, `target`, `speed` from the fans loop) |
| `heater_generic *` | `temperature`, `target`, `power` (from the heaters loop) |
| `load_cell *` | `force_g` |
| filament switch/motion sensors | `filament_detected`, `enabled`, `detection_count` |
| width sensors | `Diameter`, `Raw` |
| probe objects (`probe`, `bltouch`, `beacon`, ...), one per physical probe | `last_z_result` and `z_offset`, plus `last_query` where the type publishes a usable one, per the table in [Probe status keys](#probe-status-keys), from `ProbeSensorManager::required_status_objects()` |

`ProbeSensorManager` owns which object names are probes and which object and keys each type publishes, so the builder asks it rather than listing them. Mainline modules do not publish `z_offset` and Klipper answers the requested key with `null`, so the offset seeded by `discover_from_config()` stands; the Creality and QIDI forks publish it, and a numeric value replaces the seed. Accelerometers have no `get_status()` and are never subscribed; `AccelSensorManager::update_from_status()` is a no-op that exists only for the shared fan-out.

A field-restricted subscription makes Moonraker send `null` for a field the object lacks. Every parser therefore uses `find()` plus a type check before `get<>()`, never `value()` or a bare `get<>()`:

```cpp
// src/sensors/temperature_sensor_manager.cpp#update_from_status
if (auto it = sensor_data.find("temperature");
    it != sensor_data.end() && it->is_number()) {
    state.temperature = it->get<float>();
}
```

### Threading

`discover*()`, `load_config*()` and the `set_*()` mutators touch subjects directly and run on the main thread. `update_from_status()` takes the manager's `std::recursive_mutex`, updates `states_`, and if anything changed defers `update_subjects()` to the main thread with `lifetime_.token().defer(...)`. `deinit_subjects()` invalidates that `AsyncLifetimeGuard` first, so an update queued before teardown is dropped instead of landing on torn-down (or freshly re-created) subjects. `AccelSensorManager` has nothing to defer: accelerometers publish no status.

Every manager that parses status also has `set_sync_mode(bool)`. With it on, `update_from_status()` calls `update_subjects()` inline, which is how unit tests avoid pumping the `UpdateQueue`.

### Subjects

All values are integers so XML can bind them. Fixed-name subjects register globally through `UI_MANAGED_SUBJECT_INT`; per-sensor temperature subjects are dynamic and never registered with XML.

| Subject | Owner | Encoding |
|---------|-------|----------|
| `filament_runout_detected`, `filament_toolhead_detected`, `filament_entry_detected` | Filament | -1 no sensor holds the role, 0 empty, 1 filament present, 2 configured but disabled |
| `filament_runout_scoped` | Filament | same -1/0/1/2, scoped to the running print's tools; written by `PrintStatusPanel` via `set_scoped_runout()` |
| `filament_any_runout` | Filament | 1 when `has_any_runout()` and outside the startup grace period |
| `filament_motion_active`, `filament_master_enabled`, `filament_sensor_count` | Filament | 0/1, 0/1, count |
| `filament_probe_triggered` | Filament (a switch sensor in the Z_PROBE role) | -1/0/1/2 |
| `probe_triggered` | Probe | -1/0/1 |
| `temp_sensor_count` | Temperature | count; the per-sensor subjects carry decidegrees (°C x 10) |
| `chamber_humidity`, `dryer_humidity` | Humidity | % x 10, -1 when no enabled sensor holds the role |
| `chamber_pressure` | Humidity | Pa (hPa x 100), -1 when unavailable |
| `humidity_sensor_count` | Humidity | count |
| `probe_last_z`, `probe_z_offset`, `probe_count` | Probe | microns, microns, count |
| `accel_count` | Accel | count |
| `filament_width_diameter`, `filament_diameter_text`, `width_sensor_count` | Width | microns, string (`"--"` default), count |
| `load_cell_count` | Load cell | count |

The `*_count` subjects double as hardware gates: `src/ui/panel_widget_registry.cpp` names `temp_sensor_count`, `filament_sensor_count`, `humidity_sensor_count` and `width_sensor_count` as `hardware_gate_subject`s, and `ui_xml/sensors_overlay.xml` hides each section with `bind_flag_if_eq ... ref_value="0"`.

Each manager exposes `get_subjects_lifetime()`. Pass it as the fourth argument of `observe<int>()` for any observer that can outlive the manager's teardown, as `src/ui/ui_filament_runout_handler.cpp#show_runout_guidance_modal` does with `get_any_runout_subject()`.

### Config persistence

Three managers persist per-sensor config under the active printer's prefix (`Config::df()`): `filament_sensors`, `probe_sensors` and `width_sensors`. Each stores a `sensors` array of `{klipper_name, role, enabled, ...}` and the filament block adds `master_enabled` and an optional per-sensor `lane`. The UI writes through `save_config_to_file()` after each change (Settings > Sensors and the wizard).

Temperature, humidity and accelerometer managers implement `load_config(json)` / `save_config()`, but nothing in `src/` calls them, and nothing in `src/` calls their `set_sensor_role()` either: the Settings overlay lists these sensors read-only. Their roles are therefore exactly what discovery derives from the names, plus the chamber promotion `PrinterState` applies from the saved chamber assignment, and a rediscovery reproduces them. Wire `load_config_from_file()` / `save_config_to_file()` the way width does when a UI starts assigning these roles. `LoadCellManager::load_config()` is a documented no-op.

## Filament Sensors

`FilamentSensorManager` is by far the largest manager, because runout is a print-safety decision and has to coexist with AMS backends, firmware stand-downs and tool changes.

### Roles and types

```cpp
// include/filament_sensor_types.h#FilamentSensorRole
enum class FilamentSensorRole {
    NONE = 0,     ///< Sensor discovered but not assigned to a role
    RUNOUT = 1,   ///< Primary runout detection sensor
    TOOLHEAD = 2, ///< Toolhead/nozzle proximity sensor
    ENTRY = 3,    ///< Entry point detection sensor
    Z_PROBE = 10  ///< Z probing sensor (maps to Klipper "probe" object)
};
```

`FilamentSensorType` is `SWITCH` or `MOTION`, read off the object prefix. Motion sensors also report `detection_count`.

Role assignment (`src/print/filament_sensor_manager.cpp#set_sensor_role`) keeps one holder per role, except RUNOUT, which keeps one holder per lane so multi-head printers can have a runout sensor per head. Discovery auto-assigns RUNOUT to the first sensor whose name contains `runout`; the wizard step assigns it when there is exactly one non-AMS sensor and asks when there are several.

Sensors that belong to an AMS backend (lane, hub, buffer and toolhead sensors named by Happy Hare, AFC, AD5X IFS, CFS and others) are filtered out of the Settings list and the wizard by `PrinterHardware::is_ams_sensor()`, which defers per-backend names to `AmsBackend::sensor_belongs_to_backend()` (`include/ams_backend.h#"static bool sensor_belongs_to_backend("`). They are still discovered and still reported; they just are not offered for role assignment.

### Presence versus runout

Two questions look alike and are answered differently.

**Presence** (`is_filament_detected()`, `is_sensor_available()`): is filament physically at the sensor? A sensor the firmware stood down with `SET_FILAMENT_SENSOR ENABLE=0` keeps reporting `filament_detected`, and presence still reads it. The pre-print check and the load/unload buttons ask this.

**Runout** (`has_any_runout()`, `has_real_runout()`, the scoped scan): should HelixScreen act? Every runout consumer goes through one predicate:

```cpp
// src/print/filament_sensor_manager.cpp#monitors_runout
bool FilamentSensorManager::monitors_runout(const FilamentSensorConfig& config) const {
    if (!config.enabled || config.role == FilamentSensorRole::NONE) {
        return false;
    }
    auto it = states_.find(config.klipper_name);
    return it != states_.end() &&
           (it->second.enabled || observed_runouts_.count(config.klipper_name) > 0);
}
```

A sensor counts when the user enabled it, it holds a role, and the firmware is running it. `observed_runouts_` keeps a runout that was seen while the firmware ran the sensor counting after a pause macro stands every sensor down, and is cleared on refill or when the job lets go of the machine.

There are three runout queries, from coarse to fine:

| Query | Considers | Used by |
|-------|-----------|---------|
| `has_any_runout()` | every monitoring sensor | `filament_any_runout` subject |
| `has_real_runout()` | same, but a sensor that maps to an AMS lane the backend reports empty is not a runout | runout guidance modal, print status panel |
| `find_empty_required_lanes()` / `compute_scoped_runout_value()` | only the tools the current print uses, via the AMS backend's per-slot presence | pre-print check, `filament_runout_scoped` |

`print_scopes_runout_badge()` limits the scoped badge to PRINTING and PAUSED, since during Preparing the panel's tool list still describes the previous job.

### State defaults and the grace period

`FilamentSensorState::filament_detected` defaults to `true`, so a read before the first status frame never looks like a runout. On top of that, `discover_sensors()` restarts a stabilization window (`AppConstants::Startup::SENSOR_STABILIZATION_PERIOD`, 5 s) during which toasts are suppressed and `filament_any_runout` is held at 0. `has_any_runout()` itself ignores the window, so a print status panel opened right after a restart on an already-paused print still sees the runout.

### Toasts

`update_from_status()` collects notifications under the lock and fires the state-change callback and toasts after releasing it. A sensor edge produces a "Filament inserted" / "Filament removed" toast only when none of these hold:

| Suppressed when | Why |
|-----------------|-----|
| inside the stabilization window | first frames after connect are not news |
| the setup wizard is active | the wizard is walking the user through it |
| `AmsState::is_filament_operation_active()` | load/unload moves filament past sensors on purpose |
| AD5X IFS backend and no job holds the machine | the firmware auto-unloads between prints |
| removal edge inside the post-unload grace | the user just unloaded |
| `backend_owns_runout_during_job()` for the active backend, during a job | Happy Hare, AFC and CFS raise their own runout prompt (`src/system/runtime_config.cpp#backend_owns_runout_during_job`) |
| master switch off, sensor disabled, no role, or the firmware was not running the sensor on both sides of the edge | not monitoring |

When a job holds the machine and any AMS backend is present, a removal toast is not fired on the edge. It waits `RUNOUT_TOAST_DWELL` (45 s, `include/filament_sensor_types.h#"RUNOUT_TOAST_DWELL"`), because a tool change clears the toolhead sensor for 26 to 33 s and then refills it. A refill inside the dwell cancels the toast silently; the dwell is swept on every status frame rather than on a timer. Detection and the runout subjects still fire on the edge; only the toast waits.

### Bypass arming

Filament fed through an AMS bypass reaches the toolhead through no lane, so the toolhead sensor is the only runout protection, and some firmwares leave it disabled outside their own filament operations. `AmsState` reports its `any_bypass_active()` edge to `on_bypass_active_changed()`, which arms every RUNOUT-role sensor the firmware has disabled (`SET_FILAMENT_SENSOR SENSOR=<name> ENABLE=1`) and records them in `bypass_armed_`. On disengage it restores exactly that set. A sensor the firmware reports disabled in the meantime drops out of the set, since someone else now owns its state. The user's `enabled` setting is never touched. The API handle comes from `MoonrakerManager` via `set_moonraker_api()`.

## Temperature Sensors

`TemperatureSensorManager` covers `temperature_sensor`, `temperature_fan`, `heater_generic` and TMC2240/TMC5160 drivers. `heater_generic` objects come from `hardware.heaters()` rather than `hardware.sensors()`: `init_subsystems_from_hardware()` (`src/printer/printer_discovery.cpp#init_subsystems_from_hardware`) appends them, which is how filament dryer heaters (Happy Hare `MMU_heater`, QIDI Box `heater_boxN`) reach the temp graph without any filament-backend code. `discover()` skips anything named `extruder*` or `heater_bed` and assigns a role and sort priority from the name: CHAMBER (contains `chamber`, priority 0), MCU (contains `mcu`, 10), HOST (`raspberry_pi`, `host_temp`, `host`, `rpi`, or contains `raspberry`, 20), STEPPER_DRIVER (TMC objects, 30), AUXILIARY (everything else, 100). `get_sensors_sorted()` orders by that priority.

Each sensor gets a heap-allocated `DynamicIntSubject` holding decidegrees for its temperature and its target (`get_temp_subject()` / `get_target_subject()`), both covered by one `SubjectLifetime`. The target reads 0 for objects without one; `klipper_object_has_target()` (`include/temperature_sensor_types.h#klipper_object_has_target`) answers which objects have one (`heater_generic`, `temperature_fan`). Rediscovery removes subjects for vanished sensors in two phases: first expire every orphan's lifetime token, then erase the map entries (whose destructor calls `lv_subject_deinit()`). Consumers fetch a subject and its token together:

```cpp
// src/ui/panel_widgets/thermistor_widget.cpp#bind_carousel_sensors
SubjectLifetime& lifetime = carousel_lifetimes_.emplace_back();
lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
```

The thermistor widget observes `temp_sensor_count` to rebind when the sensor set changes. In single mode it mirrors the selected sensor's temperature and target into per-instance subjects its `temp_display` binds to, and a tap on a sensor with a target opens the keypad and sends through `TemperatureController::set_target(klipper_name, ...)` (`src/ui/panel_widgets/thermistor_widget.cpp#open_target_keypad`). The keypad ceiling is `TemperatureController::keypad_max_for()`: the heater's configfile `max_temp` when the printer reported one, else 120°C.

Chamber sensors whose names lack `chamber` (Snapmaker `cavity`, Elegoo `enclosure`) are promoted by `PrinterState` once discovery resolves the chamber sensor: it calls `apply_chamber_sensor_override(chamber_sensor, chamber_heater)` (`src/sensors/temperature_sensor_manager.cpp#apply_chamber_sensor_override`), which demotes the incumbent CHAMBER sensor to an inferred role and promotes the named one. A `heater_generic` chamber heater is promoted alongside it, so it is graphed once as "Chamber" and not again as an auxiliary heater; a `temperature_fan` in the heater slot keeps its own role. A sensor name the printer does not report is ignored. Without the promotion the temp graph would list the chamber twice. [CHAMBER_HEATER.md](CHAMBER_HEATER.md) owns the chamber heater/sensor assignment rules; the user-facing pick lives in the Settings > Sensors chamber dropdowns.

## Humidity Sensors

Recognized chips are one table. Adding a chip is one enum value and one row; discovery, the subscription bucketing in `MoonrakerDiscoverySequence`, config strings and display names all derive from it:

```cpp
// include/humidity_sensor_types.h#humidity_sensor_chips
static const std::vector<HumiditySensorChip> chips = {
    {HumiditySensorType::BME280, "bme280 ", "bme280", "BME280", true},
    {HumiditySensorType::HTU21D, "htu21d ", "htu21d", "HTU21D", false},
    {HumiditySensorType::SHT3X, "sht3x ", "sht3x", "SHT3X", false},
    {HumiditySensorType::AHT10, "aht10 ", "aht10", "AHT10", false},
    {HumiditySensorType::AHT20, "aht20 ", "aht20", "AHT20", false},
    {HumiditySensorType::AHT20_F, "aht20_f ", "aht20_f", "AHT20-F", false},
};
```

Prefixes carry the trailing space so `aht20 ` never matches `aht20_f heater_box1`. Only BME280 reports pressure.

Roles are CHAMBER and DRYER, auto-assigned at discovery to the first sensor whose name contains `chamber` or `dryer`. The humidity home widget turns `chamber_humidity` into the `chamber_humidity_text` string subject it binds. Happy Hare's dryer and the QIDI box read these chips' status directly through their AMS backends; see the backend docs.

## Probe, Accelerometer, Width and Load Cell

**Probe.** `parse_klipper_name()` matches exact object names (`probe`, `bltouch`, `smart_effector`, `cartographer`, `beacon`) and the `probe_eddy_current <name>` prefix. One physical probe often registers several objects: Klipper's `bltouch`, `smart_effector` and `probe_eddy_current`, Beacon and the Cartographer plugin all also register the generic `probe`. `probes_in()` keeps only the most specific object (and drops an eddy object beside a named scanner), and both `discover()` and the status subscription go through it, so such a printer has one probe sensor, subscribed once. `load_config_from_file()` applies a saved `probe_sensors` role first, then auto-assigns Z_PROBE when exactly one probe remains and none holds it. Nothing in the UI assigns probe roles, so a printer with two genuinely distinct probes and no saved choice reads -1 on the probe subjects. `set_probe_type_override()` lets the printer database retype a generic `probe` as the real hardware; `PrinterState` calls it after detection. `ui_probe_overlay.cpp` reads `get_z_offset()`.

### Probe status keys

What each object's `get_status()` returns, read from the upstream source (Klipper `461c4e37`, Kalico `0028cf70`, beacon3d/beacon_klipper `3eb01346`, Cartographer3D/cartographer3d-plugin `06e01690`, Cartographer3D/cartographer-klipper `d8fbed79`, vvuk/eddy-ng `1ed056b1`, CrealityOfficial/K1_Series_Klipper `e09f36e6`, CrealityOfficial/K2_Series_Klipper `bc0a5207`, QIDITECH/klipper `653d7a8f`). Klipper's `objects/list` (`Klipper3d/klipper: klippy/webhooks.py#_handle_list`) lists only objects that have `get_status`, so an object without one never reaches `parse_klipper_name()`.

The last column omits `z_offset`, which HelixScreen requests on every probe object it reads (see the `z_offset` note below).

| Object | Source | `get_status` keys | Also registers `probe`? | HelixScreen reads |
|--------|--------|-------------------|-------------------------|-------------------|
| `probe` | `Klipper3d/klipper: klippy/extras/probe.py#ProbeCommandHelper.get_status` | `name`, `last_query`, `last_probe_position`, `last_z_result` | is `probe` | `probe`: `last_query`, `last_z_result` |
| `probe` | `KalicoCrew/kalico: klippy/extras/probe.py#PrinterProbe.get_status` | `name`, `last_query`, `last_z_result` | is `probe` | same |
| `probe` | `CrealityOfficial/K1_Series_Klipper: klippy/extras/probe.py#PrinterProbe.get_status` (also `CrealityOfficial/K2_Series_Klipper`) | `last_query`, `last_z_result`, `z_offset` (`z_offset_calibrate` once `Z_OFFSET_APPLY_PROBE` ran, so it changes live) | is `probe` | same |
| `probe` | `QIDITECH/klipper: klippy/extras/probe.py#PrinterProbe.get_status` | `last_query`, `last_z_result`, `x_offset`, `y_offset`, `z_offset` (static config values) | is `probe` | same |
| `bltouch`, `smart_effector` | `Klipper3d/klipper: klippy/extras/bltouch.py#PrinterBLTouch.get_status`, `Klipper3d/klipper: klippy/extras/smart_effector.py#PrinterSmartEffector.get_status`: both delegate to `ProbeCommandHelper` | same four keys as `probe` | yes, the same object (`load_config` adds it) | own object: `last_query`, `last_z_result` |
| `bltouch`, `smart_effector` | `KalicoCrew/kalico: klippy/extras/bltouch.py#load_config`, `KalicoCrew/kalico: klippy/extras/smart_effector.py#load_config` | none (no `get_status`, so not listed) | yes, a `PrinterProbe` wrapper | seen as plain `probe` |
| `probe_eddy_current <name>` | `Klipper3d/klipper: klippy/extras/probe_eddy_current.py#PrinterEddyProbe.get_status` (`ProbeCommandHelper`, built without `query_endstop`) | same four keys; `last_query` stays `false` because `QUERY_PROBE` is rejected | yes, the same object | own object: `last_z_result` |
| `probe_eddy_current <name>` | `KalicoCrew/kalico: klippy/extras/probe_eddy_current.py#PrinterEddyProbe` | none (not listed) | yes, a `PrinterProbe` wrapper | seen as plain `probe` |
| `beacon` | `beacon3d/beacon_klipper: beacon.py#BeaconProbe.get_status` | `last_sample`, `last_received_sample`, `last_z_result`, `last_probe_position`, `last_probe_result`, `last_offset_result`, `last_poke_result`, `model` | yes when `register_as_probe` (default for the unnamed sensor); its status is `{"name": "beacon"}` only (`BeaconProbeWrapper.get_status`) | `beacon`: `last_z_result` |
| `cartographer` | `Cartographer3D/cartographer3d-plugin: src/cartographer/core.py#PrinterCartographer.get_status` | `scan`, `touch`, `mcu`; `scan`/`touch` each hold `current_model`, `models`, `last_z_result` (`Cartographer3D/cartographer3d-plugin: src/cartographer/probe/scan_mode.py`, `Cartographer3D/cartographer3d-plugin: src/cartographer/probe/touch_mode.py`) | yes when `register_as_probe` (default `true`); `Cartographer3D/cartographer3d-plugin: src/cartographer/adapters/klipper/probe.py#get_status` returns `name`, `last_query` (int 0/1), `last_z_result`, `last_probe_position` | `probe`: `last_query`, `last_z_result` |
| `cartographer` | `Cartographer3D/cartographer-klipper: cartographer.py` (v1 module, section `[cartographer]`) | `last_sample`, `model` | yes, but that wrapper has no `get_status` | nothing useful |
| `scanner` | `Cartographer3D/cartographer-klipper: scanner.py#Scanner.get_status` (section `[scanner]`) | `last_sample`, `last_received_sample`, `model` | yes, `ScannerWrapper.get_status`: `name`, `last_z_result` | not parsed; seen as plain `probe` |
| `probe_eddy_ng <name>` | `vvuk/eddy-ng: probe_eddy_ng.py#ProbeEddy.get_status` | `ProbeCommandHelper` keys plus `home_trigger_height`, `tap_offset`, `last_tap_z`, ... | yes, the same object | not parsed; seen as plain `probe` |

Klicky has no module: it is a plain `[probe]` with dock macros, and `discover()` retypes it from the macros.

- **`last_z_result`** is set only by the `PROBE` command. Klipper stores the toolhead Z at trigger (`bed_z + z_offset`, marked deprecated in `cmd_PROBE`); Kalico and the Cartographer plugin store the Z their probe run returns; Beacon stores trigger Z minus the probe's `z_offset`.
- **`last_query`** is the result of the last `QUERY_PROBE`, not a live endstop state; nothing publishes a live one. It drives `probe_triggered`. Klipper and Kalico publish a bool, the Cartographer plugin an int.
- **`z_offset`** is not published by any mainline, Kalico, Beacon or Cartographer module. The Creality K1 and K2 forks publish it and update it live through `Z_OFFSET_APPLY_PROBE`; the QIDI fork publishes it with `x_offset` and `y_offset` as static config values. It is requested on every probe object: Klipper answers a requested key the module lacks with `null` (`Klipper3d/klipper: klippy/webhooks.py#_do_query` fills it with `res.get(ri, None)`), which keeps the `discover_from_config()` seed, and a number replaces it. The Flashforge firmware's `null` is that fill, not a published field. Creality's Ender-3 V3 and Elegoo have no public Klipper source to check.
- A null or absent field never overwrites state.
- A Cartographer configured with `register_as_probe: false` beside a separate `[probe]` reads that probe's status: the objects list cannot tell the two apart.

**Accelerometer.** Found only in `configfile.config` sections (`adxl345`, `adxl345 bed`, `lis2dw hotend`, ...). A `beacon` section with `accel_scale` or `accel_axes_map` adds Beacon's onboard LIS2DW. The role INPUT_SHAPER backs `is_sensor_available(AccelSensorRole::INPUT_SHAPER)`.

**Width.** Klipper allows one of each width-sensor module, so the names are fixed (`tsl1401cl`, `hall`). The first sensor is auto-assigned FLOW_COMPENSATION; `get_flow_compensation_diameter()` reads it. `filament_diameter_text` is pre-formatted for the width widget.

**Load cell.** Auto-assigns SPOOL_WEIGHT by name, or to a lone load cell. `has_spool_weight_load_cell()` is what `src/printer/consumption_sink.cpp` asks before trusting load-cell spool weight. Load cells have no persisted config.

## Moonraker Sensors (`SensorState`)

`SensorState` is a separate singleton for Moonraker's `[sensor]` components. Partial discovery (Moonraker up, Klipper possibly not) calls `MoonrakerDiscoverySequence::discover_sensors()`, which fetches `server.sensors.list`, sets the `sensor_count` capability subject, and queues `SensorState::set_sensors()` to the main thread. `subscribe()` registers for `notify_sensor_update`; `on_sensor_update()` defers each value through its `AsyncLifetimeGuard` token.

Values are per-sensor, per-key `DynamicIntSubject`s fetched with `get_value_subject(sensor_id, key, lifetime)`. They are centi-unit encoded (`to_centi_units()`): power, voltage and energy x 100, current x 100000 for milliamp precision. `format_value()` reverses it with units. `energy_sensor_ids()` picks sensors with power/voltage/current/energy keys; `src/ui/widgets/power_device_widget.cpp` is the main consumer.

## UI Consumers

| Surface | Reads |
|---------|-------|
| Settings > Devices > Sensors (`SensorSettingsOverlay`) | every manager's `get_sensors()` plus counts; writes roles/enables and `save_config_to_file()` for filament, probe and width; chamber heater/sensor assignment dropdowns |
| First-run wizard (`WizardFilamentSensorSelectStep`) | non-AMS filament sensors; assigns RUNOUT |
| Runout guidance modal (`FilamentRunoutHandler`) | `has_real_runout()` on pause, `filament_any_runout` to auto-close |
| Print status panel and `filament_sensor_indicator.xml` | `filament_runout_detected`, `filament_runout_scoped`, `filament_sensor_count` |
| Home widgets | thermistor (per-sensor temp subjects), humidity (`chamber_humidity`), width (`filament_diameter_text`), filament sensor |
| Temp graph, temperature history | `TemperatureSensorManager` sensor list and per-sensor subjects |
| Telemetry | per-category `sensor_count()` |

## Mock and Tests

`--test` seeds the same managers through the same discovery and status paths, so mock runs exercise production code. `HELIX_MOCK_FILAMENT_SENSORS` and `HELIX_MOCK_FILAMENT_STATE` shape the filament sensor set; see [MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md).

Unit tests live in `tests/unit/test_*_sensor_manager.cpp`, `test_load_cell_manager.cpp`, `test_sensor_state.cpp`, `test_sensor_settings_overlay.cpp`, `test_filament_sensor_bypass_arming.cpp` and the wizard tests. Each manager declares a `*TestAccess` friend for resetting the singleton between cases. Run one category with `make t F='[humidity]'` (check the file for its tags).

## Adding a New Sensor

### A new chip or object name in an existing category

Extend the manager's name recognition and nothing else.

- **Humidity chip:** add a `HumiditySensorType` value and a row to `humidity_sensor_chips()`. Discovery, subscription bucketing and display follow.
- **Temperature-reporting object:** add the prefix to `TemperatureSensorManager::parse_klipper_name()` and to the object-list bucketing in `src/api/moonraker_discovery_sequence.cpp#parse_objects` so it is subscribed. TMC drivers are the worked example: both places list `tmc2240 ` and `tmc5160 `.
- **Probe or accelerometer:** add a case to that manager's `parse_klipper_name()` and to its `*_types.h` enum and string tables.

Add a case to the manager's unit test that discovers the new name and feeds a status frame.

### A new category

Copy the shape of `LoadCellManager` (the smallest manager) or `TemperatureSensorManager` (if you need per-sensor subjects).

1. **Types header** `include/<thing>_types.h`: role and type enums, `*Config` (`klipper_name`, `sensor_name`, `role`, `enabled`), `*State` (values plus `available`), and to/from-string helpers.
2. **Manager** in `src/sensors/`, namespace `helix::sensors`, a Meyers singleton with:
   - `init_subjects()` using `UI_MANAGED_SUBJECT_INT` into a `SubjectManager`, at least a `<thing>_count` subject, and a `StaticSubjectRegistry::instance().register_deinit(...)` call at the end
   - `deinit_subjects()` that invalidates the `AsyncLifetimeGuard` first, then clears collections and subjects under the mutex
   - `discover()` and/or `discover_from_config()`, main thread only
   - `update_from_status(const nlohmann::json&)` that parses with `find()` plus a type check under the mutex and defers `update_subjects()` with `lifetime_.token().defer(...)`
   - `get_subjects_lifetime()`, `set_sync_mode()`, and a `*TestAccess` friend
3. **Register** the manager in `for_each_sensor_manager()` (`include/sensor_managers.h`). That alone wires subject init and status fan-out.
4. **Discover:** call `discover()` from `init_subsystems_from_hardware()`, or queue `discover_from_config()` beside the probe and accel calls in the discovery sequence. If the objects need a new `PrinterDiscovery` accessor, add it in `src/printer/printer_discovery_parse.cpp`.
5. **Subscribe:** add the objects and the exact fields you read to `build_subscription_objects()`. A manager whose objects are not subscribed never receives status.
6. **Persist**, only if users assign roles: follow `WidthSensorManager::load_config_from_file()` / `save_config_to_file()` under a new `Config::df()` key, and call the load right after discovery.
7. **Surface it:** a section in `ui_xml/sensors_overlay.xml` gated on `<thing>_count`, a `populate_*` method in `SensorSettingsOverlay`, and if it gets a home widget, a `hardware_gate_subject` in `panel_widget_registry.cpp` (see [PANEL_WIDGET_GUIDE.md](PANEL_WIDGET_GUIDE.md)).
8. **Mock:** add the objects and an initial status to the mock client so `--test` shows the category.

Vendor names stay out of generic code: if one firmware publishes the reading under its own object, put that knowledge in one provider module the way `helix::power_loss` does (`include/power_loss_sensor.h`), and let the subscription builder ask it for `required_status_objects()`. See `.claude/rules/vendor-abstraction.md`.

## Gotchas

- **`discover*()` is main-thread only.** It sets subjects directly. Only `update_from_status()` may be called from elsewhere, and in production even that arrives on the main thread via `PrinterState`.
- **Rediscovery resets runtime roles.** `discover()` rebuilds the config list; anything not reloaded from settings right after is lost on reconnect.
- **Null fields are normal.** A field-restricted subscription sends `null` for fields an object lacks. Never `get<>()` without checking the type.
- **Use presence queries for "is it there", runout queries for "act on it".** Mixing them either ignores a firmware stand-down or alerts on a sensor the firmware is deliberately not running.
- **Per-sensor subjects need their token.** Use the `get_temp_subject(name, lifetime)` / `get_value_subject(id, key, lifetime)` overloads and pass that lifetime to `observe<int>()`.
- **Imperative rows.** The humidity and accelerometer sections of the Settings overlay build their rows in C++. New sections should create an XML component the way switch sensors use `filament_sensor_row` and load cells use `load_cell_row`, per `.claude/rules/declarative-ui.md`.
