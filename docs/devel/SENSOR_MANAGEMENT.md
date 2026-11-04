# Sensor Management

HelixScreen tracks the printer's auxiliary sensors with one object table and one manager base class. `src/printer/hardware_object_table.cpp` decides what a Klipper object or config section *is*. `include/sensor_manager.h` (`SensorManager`) owns everything a sensor manager has in common: the sensor list, reconcile against discovery, threading, subjects, roles, persistence and the Settings row model. Eight managers derive from it (filament, temperature, humidity, probe, accelerometer, width, load cell, input), each holding only its own readings, subjects and domain rules, and `SensorState` derives from it too. Extruder and bed temperatures are not part of this subsystem; they live in `PrinterState`.

For the surrounding machinery, read these first:

| Topic | Owner |
|-------|-------|
| How `printer.objects.list` becomes a `PrinterDiscovery` snapshot | [architecture/06-discovery-capabilities.md](architecture/06-discovery-capabilities.md#the-sensor-framework-one-table-one-base) |
| The census of singletons, `StaticSubjectRegistry` self-registration | [architecture/05-printer-state.md](architecture/05-printer-state.md) |
| `SubjectLifetime`, `ObserverGuard`, `observe<T>()` | [architecture/02-subjects-dataflow.md](architecture/02-subjects-dataflow.md) |
| Threading invariants, `AsyncLifetimeGuard`, `queue_update` | [THREADING.md](THREADING.md) |
| How AMS backends use filament sensors | [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md), [architecture/07-filament-ams.md](architecture/07-filament-ams.md) |
| Mains-voltage monitor (`helix::power_loss`) and recovery | [POWER_LOSS_RECOVERY.md](POWER_LOSS_RECOVERY.md) |

Two things share the word "sensor". The managers consume **Klipper** objects, config and status frames. `SensorState` tracks **Moonraker's own** `[sensor]` components (`server.sensors.list`, `notify_sensor_update`), mostly power meters. It derives from the same base but is fed differently; see [Moonraker sensors](#moonraker-sensors-sensorstate).

## Key Files

| File | Purpose |
|------|---------|
| `include/hardware_object_table.h`, `src/printer/hardware_object_table.cpp` | The one object table: `classify_object()`, `classify_section()`, `status_fields()`, the `is_*` kind predicates |
| `include/sensor_manager.h`, `src/sensors/sensor_manager.cpp` | `SensorManager` base, `SensorEntry`, `RoleSpec`, `TypeSpec`, `SensorRowView`, the registry `sensor_managers()`, `rediscover_from_config()` |
| `include/dynamic_subject_map.h`, `src/sensors/dynamic_subject_map.cpp` | `DynamicSubjectMap`: integer subjects created at runtime, one per key, each with its own death signal |
| `include/temperature_sensor_manager.h`, `src/sensors/temperature_sensor_manager.cpp` | Temperature sensors, fans with a sensor, generic heaters, TMC drivers |
| `include/humidity_sensor_manager.h`, `src/sensors/humidity_sensor_manager.cpp`, `include/humidity_sensor_types.h` | Humidity chips and the chip table |
| `include/probe_sensor_manager.h`, `src/sensors/probe_sensor_manager.cpp` | Native Klipper probes, per-type subscriptions |
| `include/accel_sensor_manager.h`, `src/sensors/accel_sensor_manager.cpp` | Accelerometers (config sections only) |
| `include/width_sensor_manager.h`, `src/sensors/width_sensor_manager.cpp` | Filament width sensors |
| `include/load_cell_manager.h`, `src/sensors/load_cell_manager.cpp` | Load cells |
| `include/input_sensor_manager.h`, `src/sensors/input_sensor_manager.cpp` | `gcode_button` inputs (door switches, buttons) |
| `include/filament_sensor_manager.h`, `src/print/filament_sensor_manager.cpp` | Filament switch and motion sensors, runout policy |
| `include/sensor_state.h`, `src/printer/sensor_state.cpp` | Moonraker `[sensor]` components |
| `src/printer/printer_discovery.cpp#init_subsystems_from_hardware` | Calls `discover()` then `load_settings()` on every manager |
| `src/printer/status_dispatch.cpp#dispatch_status_frame` | Hands every status frame to every manager after `PrinterState` |
| `src/api/moonraker_discovery_sequence.cpp#build_subscription_objects` | Asks the table and each manager's `extra_subscriptions()` what to subscribe |
| `src/application/subject_initializer.cpp` | Calls every manager's `init_subjects()` before any panel XML exists |
| `ui_xml/sensor_row.xml`, `ui_xml/sensors_overlay.xml`, `src/ui/settings/ui_settings_sensors.cpp` | Settings > Sensors: one row component, one section per manager |
| `include/printer_hardware.h` | `PrinterHardware::is_ams_sensor()`: hides filament sensors an AMS backend owns |
| `src/ui/wizard/ui_wizard_filament_sensor_select.cpp` | First-run wizard step that assigns the RUNOUT role |
| `src/ui/filament/ui_filament_runout_handler.cpp` | Runout guidance modal |
| `src/ui/panel_widgets/thermistor_widget.cpp`, `humidity_widget.cpp`, `width_sensor_widget.cpp`, `filament_sensor_widget.cpp` | Home-panel widgets |

Each manager has a matching `*_types.h` header (`filament_sensor_types.h`, `humidity_sensor_types.h`, `load_cell_types.h`, ...) holding its role and type enums and the string conversions used for persistence and display.

## Architecture

```
printer.objects.list ─► PrinterDiscovery ──► classify_object() (the object table)
configfile.config ────►  (objects + sections)       │
                                                    ▼
                   init_subsystems_from_hardware(): for every manager
                       discover(hw)  ─► reconcile entries ─► on_discovered(hw)
                       load_settings()  (saved roles and flags win)
                   configfile pass: rediscover_from_config(hw)
                       discover(hw) again, only where rediscover_on_config

build_subscription_objects(): status_fields(kind) per object  +  extra_subscriptions(hw)

notify_status_update ─► dispatch_status_frame() ─► every manager: update_from_status(status)
                          parse_status() under the mutex ─► publish()
                          publish() ─► main thread: count, revision, refresh_subjects()
                                                          │
                                  XML bind_* / observe<int>() ◄┘
```

## The Object Table

`src/printer/hardware_object_table.cpp` is the one place that says what a Klipper name is. Discovery, the subscription builder, the sensor managers, the hardware role registry, fan state, the LED controller, the MCU performance source and device display names all ask it. Vendor and chip names appear there as data rows and nowhere else in generic code (`.claude/rules/vendor-abstraction.md`).

```cpp
// include/hardware_object_table.h#ObjectClass
struct ObjectClass {
    ObjectKind kind = ObjectKind::Unknown;
    std::string instance; ///< the part after the prefix: "chamber" in "temperature_sensor chamber"
    uint8_t chip = 0;     ///< kind-specific type index (humidity chip, probe type, accel chip, width type)
};
ObjectClass classify_object(std::string_view klipper_object);
ObjectClass classify_section(std::string_view section, const nlohmann::json& body);
const std::vector<std::string>& status_fields(ObjectKind kind);
```

`classify_object()` answers for names from `printer.objects.list`. `classify_section()` answers for `configfile.config` sections and reads the section body where a rule needs it (Beacon's onboard accelerometer). Rows are scanned in order and the first match wins, so a more specific pattern precedes any shorter one that would swallow it. Each row carries a match mode (`Exact`, `Prefix`, `BareOrPrefix`), the pattern, the `ObjectKind`, a `chip` index and a scope:

```cpp
// src/printer/hardware_object_table.cpp#kRows
{Match::Prefix, "temperature_sensor ", ObjectKind::TemperatureSensor, 0, Scope::Both},
{Match::Exact, "hall_filament_width_sensor", ObjectKind::WidthSensor,
 chip_of(WidthSensorType::HALL), Scope::Both},
{Match::BareOrPrefix, "adxl345", ObjectKind::Accelerometer, chip_of(AccelSensorType::ADXL345),
 Scope::SectionOnly},
```

`Scope::SectionOnly` rows answer `classify_section()` only. Accelerometers have no `get_status()`, so they never reach `printer.objects.list` and exist only as config sections; `load_cell_probe` and `dockable_probe` are section-only because they describe the generic `probe` object. Humidity chips are not in `kRows`: `classify()` walks `humidity_sensor_chips()` (`include/humidity_sensor_types.h#humidity_sensor_chips`) first, so one table row of chips feeds classification, display names and config strings.

`status_fields(kind)` is what the subscription builder asks. An empty list subscribes nothing; a single `"*"` entry subscribes the whole object. `Probe` is empty because its fields vary per probe type and `ProbeSensorManager::extra_subscriptions()` supplies them.

| Kind | Fields subscribed |
|------|-------------------|
| `TemperatureSensor`, `DriverTemperature`, `HumidityChip` | `temperature`, `humidity` |
| `TemperatureFan` | `temperature`, `target`, `speed` |
| `Heater`, `GenericHeater` | `temperature`, `target`, `power` |
| `FilamentSwitch`, `FilamentMotion`, `BackendFilamentSensor` | `filament_detected`, `enabled`, `detection_count` |
| `WidthSensor` | `Diameter`, `Raw` |
| `LoadCell` | `force_g` |
| `Input` | `state` |
| `Probe`, `Accelerometer` | none from the table |

`chip_name(kind, chip)` turns a chip index back into the persisted or display string, and `is_temperature()`, `is_filament_sensor()` and the other `is_*` predicates answer kind-set questions so callers never list kinds themselves.

### Adding a row

A new Klipper object or config section of an existing kind is one row in `kRows`, placed before any row whose pattern would match it first. If it carries a type (probe type, width type, accel chip), add the enum value in the manager's `*_types.h` and pass it as the row's `chip`. Discovery, the subscription, roles, persistence and the Settings row follow with no other change. Add a case to `tests/unit/test_hardware_object_table.cpp` (`[hw_object_table]`) that classifies the new name.

## The SensorManager Base

`SensorManager` (`include/sensor_manager.h`) owns the sensor list (`SensorEntry`: `klipper_name`, `display_name`, `type`, `role`, `enabled`, `available`), the lifecycle, threading, sync mode, the count and revision subjects, reconcile against discovery, role assignment, settings persistence and the Settings row model. A derived manager is a `make_options()` plus the hooks it needs.

### Options

A manager describes itself to the base with an `Options` struct built in a static `make_options()`:

```cpp
// src/sensors/load_cell_manager.cpp#LoadCellManager/make_options
Options o;
o.name = "LoadCellManager";
o.kinds = {hw::ObjectKind::LoadCell};
o.count_subject = "load_cell_count";
o.revision_subject = "load_cell_revision";
o.settings_section = "load_cell";
o.row_toggle = RowToggle::None;
o.roles.push_back({kSpoolWeight, "spool_weight", "Spool Weight", /*exclusive=*/true,
                   /*offered=*/false});
return o;
```

| Option | Meaning |
|--------|---------|
| `kinds` | The `ObjectKind`s this manager owns. `discover()` collects `hw.objects_of(kinds)` and `hw.sections_of(kinds)` |
| `count_subject`, `revision_subject` | Names of the two subjects every manager publishes (below) |
| `config_key` | Persisted settings key under `Config::df()`; `nullptr` means nothing is saved |
| `roles` (`RoleSpec`) | `id`, persisted `config_name`, translation `label_key`, `exclusive` (at most one holder), `offered` (listed in the Settings dropdown) |
| `types` (`TypeSpec`) | `id`, persisted `config_name`, `label_key`; indexed by the object table's `chip` |
| `from_printer_discovery` | `false` for a manager fed from elsewhere (`SensorState`); `discover()` is then a no-op |
| `rediscover_on_config` | Run `discover()` again on the configfile pass |
| `settings_section` | Slug of the Settings > Sensors section; `nullptr` means no section |
| `row_toggle` | The toggle the row offers: `None`, `Enabled`, `Invert` |

Role id 0 is always "none". The persisted role and type strings are fixed by `config_name`: saved settings keep their exact JSON shape across versions.

### Hooks, threads and locks

The base wraps every cross-thread concern, so a hook is written for one thread and one lock state:

| Hook | Thread | Mutex | Job |
|------|--------|-------|-----|
| `register_subjects(SubjectManager&)` | main | not held | Create the manager's own fixed subjects |
| `parse_status(status)` | any | held | Update typed readings; return whether anything changed |
| `refresh_subjects()` | main | NOT held | Copy what is needed under a short lock, then set subjects |
| `on_discovered(hw)` | main | held | Auto-assign roles, read config sections, reset per-connect state |
| `on_entries_changed()` | main | held | Resize parallel readings to the new entry list |
| `read_entry_extra` / `write_entry_extra` | main | held | Per-sensor extras in saved settings |
| `read_extra_settings` / `write_extra_settings` | main | held | Manager-level extras in saved settings |
| `on_deinit_subjects()` | main | held, lifetime already invalidated | Tear down subjects kept outside the `SubjectManager` (a `DynamicSubjectMap`'s `clear()`) |
| `extra_subscriptions(hw)` | main | not held | Subscriptions beyond `status_fields()` |
| `set_role`, `set_option`, `row`, `listed_in_settings` | main | virtual, default in the base | Overridden for domain rules |

`discover()`, `load_settings()`, the setters and every subject touch run on the main thread. `update_from_status()` may arrive on any thread: its default locks, calls `parse_status()`, unlocks, and calls `publish()` if anything changed.

### Snapshot, then set

`refresh_subjects()` never holds the manager mutex while it sets a subject. Setting a subject runs its observers synchronously, and observers call back into managers and `AmsState`; a held lock would deadlock or invert lock order (`.claude/rules/threading.md`). The pattern is a short locked copy followed by unlocked writes:

```cpp
// src/sensors/sensor_manager.cpp#SensorManager/refresh_on_main_thread
int count = 0;
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    count = static_cast<int>(entries_.size());
}
if (lv_subject_get_int(&count_) != count) {
    lv_subject_set_int(&count_, count);
}
```

### publish()

Every state change ends in `publish()`. In production it defers `refresh_on_main_thread()` through the manager's `AsyncLifetimeGuard` token; `deinit_subjects()` invalidates that token first, so a refresh queued before teardown is dropped instead of landing on freed subjects. In sync mode (`set_sync_mode(true)`, which unit tests use to avoid pumping the `UpdateQueue`) it refreshes inline. With subjects not yet initialized it does nothing, and `init_subjects()` publishes once. `refresh_on_main_thread()` syncs the count subject, bumps the revision subject and then calls `refresh_subjects()`. The revision subject is the one signal that anything about the manager changed, which is how Settings rows know to redraw.

### Discover and reconcile

`discover(hw)` builds the candidate list from the object table: objects first, in `printer.objects.list` order, then config sections whose name no object already has (config-only kinds). Under the mutex it calls `replace_entries_locked()`, then `on_discovered(hw)`, then publishes. `replace_entries_locked()` carries `role` and `enabled` over from entries with the same `klipper_name`, so a rediscovery on reconnect never discards a choice made at runtime, then calls `on_entries_changed()`.

`reconcile(std::vector<SensorEntry>)` is the same replacement for a manager that is not fed by `PrinterDiscovery`: the manager builds its own entry list and the base applies the same carry-over rule. `SensorState` is the only caller.

### Roles: auto first, saved second

A manager that infers a role from a name does it in `on_discovered()`: the first sensor whose name holds `runout`, a lone probe becoming Z_PROBE, `chamber` and `dryer` humidity chips. `init_subsystems_from_hardware()` calls `load_settings()` right after `discover()`, so a saved role, including a saved role of none, overrides the automatic one. Auto-assignment therefore guards itself against taking a role something else already holds, and a manager that must not undo a user's choice on rediscovery records which names it has already seen (`FilamentSensorManager` does).

`set_role()` makes exclusive roles single-holder by clearing every other holder. `set_enabled()` and the `set_option()` hook (the Invert toggle) edit one entry. All three publish; the UI calls `save_settings()` after each.

### Persistence

A manager with a `config_key` saves under `Config::df() + config_key`. Four do: `filament_sensors`, `input_sensors`, `probe_sensors` and `width_sensors`. The shape:

```json
{
  "sensors": [
    {"klipper_name": "filament_switch_sensor runout", "role": "runout", "enabled": true, "type": "switch"}
  ]
}
```

`write_entry_extra()` adds per-sensor fields (filament adds a `lane` when the sensor has one; input adds `inverted`) and `write_extra_settings()` adds top-level fields (`master_enabled` for filament, `door_check` for input, absent until the user sets it). `load_settings()` is lenient: a wrong-typed field is skipped and the rest still applies, and a saved name that discovery does not list is ignored. With nothing saved it still reaches `read_extra_settings()`, which restores defaults.

`saved_sensor_names()` returns the `klipper_name` of every saved entry whether or not discovery lists it now. `src/printer/hardware_validator.cpp` uses it to warn about a configured filament sensor the printer no longer reports.

Temperature, humidity, accelerometer and load cell managers have no `config_key`: their roles are exactly what `on_discovered()` derives, so a rediscovery reproduces them.

### Subscriptions: extra_subscriptions

The builder merges `status_fields(kind)` for every object the table classifies, then asks each manager `extra_subscriptions(hw)`, which returns a `printer.objects.subscribe` map (`{object: [fields...] or null}`). Most managers return the empty default. `ProbeSensorManager` owns every probe subscription there, because the object and fields differ per probe type (see [Probe status keys](#probe-status-keys)).

A field-restricted subscription makes Moonraker send `null` for a field the object lacks. Every `parse_status()` therefore uses `find()` plus a type check before `get<>()`, never `value()` or a bare `get<>()`:

```cpp
// src/sensors/load_cell_manager.cpp#LoadCellManager/parse_status
const auto sensor = status.find(entry.klipper_name);
if (sensor == status.end() || !sensor->is_object()) {
    continue;
}
```

### rediscover_on_config

The first discovery pass runs from the object list, before `configfile.config` has arrived. A manager that reads config sections in `on_discovered()` sets `rediscover_on_config`, and `rediscover_from_config(hw)` (`include/sensor_manager.h#rediscover_from_config`) runs its `discover()` again against discovery data that now carries the sections. `src/api/moonraker_discovery_sequence.cpp#continue_discovery_objects` queues that call to the main thread. Three managers need it:

- accelerometers, which exist only as config sections;
- probes, which seed `z_offset` from their section for modules whose status reports it as null;
- inputs, which read each `gcode_button`'s `press_gcode` / `release_gcode` to learn whether the printer's own config reacts to the edge.

It is off for every other manager because a second `discover()` repeats that manager's per-connect resets (filament restarts its stabilization window and clears its dwell state). Roles and enabled flags survive the second pass through the carry-over rule.

## The Registry

`sensor_managers()` returns the one list of managers, in a stable order:

```cpp
// src/sensors/sensor_manager.cpp
static std::vector<SensorManager*> managers = {&FilamentSensorManager::instance(),
                                               &WidthSensorManager::instance(),
                                               &HumiditySensorManager::instance(),
                                               &ProbeSensorManager::instance(),
                                               &LoadCellManager::instance(),
                                               &AccelSensorManager::instance(),
                                               &InputSensorManager::instance(),
                                               &TemperatureSensorManager::instance(),
                                               &SensorState::instance()};
```

Every fan-out walks it, so a manager listed here gets all of them with no further wiring:

| Walker | What it does |
|--------|--------------|
| `src/application/subject_initializer.cpp` | `init_subjects()`; each manager registers its own `deinit_subjects()` with `StaticSubjectRegistry` |
| `src/printer/printer_discovery.cpp#init_subsystems_from_hardware` | `discover()` then `load_settings()`; also `reload_settings_after_preset()` |
| `rediscover_from_config()` | the configfile pass |
| `src/printer/status_dispatch.cpp#dispatch_status_frame` | `update_from_status()` for every frame |
| `src/api/moonraker_discovery_sequence.cpp#build_subscription_objects` | `extra_subscriptions()` |
| `src/ui/settings/ui_settings_sensors.cpp` | one Settings section per manager with a `settings_section` |
| `src/system/telemetry_manager.cpp` | per-manager `sensor_count()` |

Discovery is unconditional: an empty hardware list is what clears the previous printer's sensors on a printer switch.

## Subjects

All values are integers so XML can bind them. Fixed-name subjects register globally through `UI_MANAGED_SUBJECT_INT`. Every manager publishes `<x>_count` and `<x>_revision`; per-sensor subjects whose set follows discovery live in a `DynamicSubjectMap` and are never registered with XML.

`DynamicSubjectMap` (`include/dynamic_subject_map.h#DynamicSubjectMap`) creates one integer subject per key with `get_or_create()`, each with its own `SubjectLifetime`. `remove_missing()` flips a removed subject's lifetime to false and then deinits it, so an `ObserverGuard` holding a copy of the token sees it dead before `lv_subject_deinit()` frees the observers it would otherwise remove. Consumers fetch a subject and its token together and observe with that token. Main thread only.


| Subject | Owner | Encoding |
|---------|-------|----------|
| `<x>_count`: `filament_sensor_count`, `temp_sensor_count`, `humidity_sensor_count`, `probe_count`, `accel_count`, `width_sensor_count`, `load_cell_count`, `input_sensor_total`, `sensor_state_count` | every manager | number of entries, set by the base |
| `<x>_revision`: `filament_sensor_revision`, `temp_sensor_revision`, `humidity_sensor_revision`, `probe_sensor_revision`, `accel_revision`, `width_sensor_revision`, `load_cell_revision`, `input_sensor_revision`, `sensor_state_revision` | every manager | bumped by every `publish()`; Settings rows refill on it |
| `filament_runout_detected`, `filament_toolhead_detected`, `filament_entry_detected` | Filament | -1 no sensor holds the role, 0 empty, 1 filament present, 2 configured but disabled |
| `filament_runout_scoped` | Filament | same -1/0/1/2, scoped to the running print's tools; written by `PrintStatusPanel` via `set_scoped_runout()` |
| `filament_any_runout` | Filament | 1 when `has_any_runout()` and outside the startup grace period |
| `filament_master_enabled` | Filament | 0/1 |
| `filament_motion_signature` | Filament | changes whenever an input of `clog_motion_sensor()` changes; the value means nothing |
| `filament_sensor_config_revision` | Filament | ticks when a sensor's role or enabled flag, or the master switch, changes |
| `probe_triggered` | Probe | -1/0/1 |
| per-sensor temperature and target | Temperature | decidegrees (°C x 10), in a `DynamicSubjectMap` |
| `chamber_humidity`, `dryer_humidity` | Humidity | % x 10, -1 when no enabled sensor holds the role |
| `chamber_pressure` | Humidity | Pa (hPa x 100), -1 when unavailable |
| `probe_last_z`, `probe_z_offset` | Probe | microns, microns |
| `filament_width_diameter`, `filament_diameter_text` | Width | microns, string (`"--"` default) |
| `input_door_check` | Input | 0/1, the door-check setting |

The count subjects double as hardware gates: `src/ui/home/panel_widget_registry.cpp` names `temp_sensor_count`, `filament_sensor_count`, `humidity_sensor_count` and `width_sensor_count` as `hardware_gate_subject`s, and `ui_xml/sensors_overlay.xml` hides each section with `bind_flag_if_eq ... ref_value="0"`.

Each manager exposes `subjects_lifetime()`. Pass it as the fourth argument of `observe<int>()` for any observer that can outlive the manager's teardown, as `src/ui/settings/ui_settings_sensors.cpp#watch` does with the revision subject.


## Filament Sensors

`FilamentSensorManager` is by far the largest manager, because runout is a print-safety decision and has to coexist with AMS backends, firmware stand-downs and tool changes. It claims `filament_switch_sensor` and `filament_motion_sensor` objects (and the backend-owned sensor kinds the table classifies separately, which `PrinterHardware::is_ams_sensor()` keeps out of Settings).

### Roles and types

```cpp
// include/filament_sensor_types.h#FilamentSensorRole
enum class FilamentSensorRole {
    NONE = 0,     ///< Sensor discovered but not assigned to a role
    RUNOUT = 1,   ///< Primary runout detection sensor
    TOOLHEAD = 2, ///< Toolhead/nozzle proximity sensor
    ENTRY = 3,    ///< Entry point detection sensor
};
```

A saved `"z_probe"` role reads as NONE: a filament switch is never the Z probe.

`FilamentSensorType` is `SWITCH` or `MOTION`, the hardware object table's type index for each prefix. Motion sensors also report `detection_count`.


### The two overrides

Filament overrides exactly two behaviors of the base, and each exists because the generic version cannot express the rule.

**`set_role()`** (`src/print/filament_sensor_manager.cpp#set_role`). The base makes an exclusive role single-holder. RUNOUT is single-holder *per lane*: assigning it clears it from other sensors watching the same lane and from sensors watching none, while heads that each have their own sensor keep one RUNOUT holder apiece. Every role is `exclusive` in `Options::roles`; the lane rule is applied by the override.

**`update_from_status()`** (`src/print/filament_sensor_manager.cpp#update_from_status`). The base parses under the mutex and publishes. Filament must read `AmsState` before it takes its own mutex (lock order), collect sensor edges under the lock, and fire the state-change callback and the toasts after releasing it, because both call into other managers and the UI. It also sweeps the removal-toast dwell on every status frame. Its `parse_status()` therefore returns `false`: the work happens in the override.

Everything else is hooks. `on_discovered()` restarts the stabilization window and clears the dwell state, and auto-assigns RUNOUT to the first sensor whose name contains `runout`, only for a name new to the manager (`known_`), so a rediscovery never undoes the user's choice. The wizard step assigns RUNOUT when there is exactly one non-AMS sensor and asks when there are several. `on_entries_changed()` keeps the reading of a sensor missing from a discovery, marked unavailable, for a reconnect that brings it back, and drops its saved lane. `read_entry_extra()` / `write_entry_extra()` carry the per-sensor `lane`, and `read_extra_settings()` / `write_extra_settings()` carry `master_enabled`. `listed_in_settings()` hides sensors an AMS backend owns, and `row()` hides the Enabled toggle on a row with no role.

Sensors that belong to an AMS backend (lane, hub, buffer and toolhead sensors named by Happy Hare, AFC, AD5X IFS, CFS and others) are still discovered and still reported; they just are not offered for role assignment. `PrinterHardware::is_ams_sensor()` defers per-backend names to `AmsBackend::sensor_belongs_to_backend()` (`include/ams_backend.h#"static bool sensor_belongs_to_backend("`).

### Presence versus runout

Two questions look alike and are answered differently.

**Presence** (`is_filament_detected()`, `is_sensor_available()`): is filament physically at the sensor? A sensor the firmware stood down with `SET_FILAMENT_SENSOR ENABLE=0` keeps reporting `filament_detected`, and presence still reads it. The pre-print check and the load/unload buttons ask this.

**Runout** (`has_any_runout()`, `has_real_runout()`, the scoped scan): should HelixScreen act? Every runout consumer goes through one predicate:

```cpp
// src/print/filament_sensor_manager.cpp#monitors_runout
bool FilamentSensorManager::monitors_runout(const SensorEntry& sensor) const {
    if (!sensor.enabled || filament_role(sensor) == FilamentSensorRole::NONE) {
        return false;
    }
    auto* st = state(sensor.klipper_name);
    return st && (st->enabled || observed_runouts_.count(sensor.klipper_name) > 0);
}
```

A sensor counts when the user enabled it, it holds a role, and the firmware is running it. `observed_runouts_` keeps a runout that was seen while the firmware ran the sensor counting after a pause macro stands every sensor down, and is cleared on refill or when the job lets go of the machine.

There are three runout queries, from coarse to fine:

| Query | Considers | Used by |
|-------|-----------|---------|
| `has_any_runout()` | every monitoring sensor | `filament_any_runout` subject |
| `has_real_runout()` | same, but a sensor that maps to an AMS lane is a runout only when that lane is present and, if the backend names a current slot, is that slot | runout guidance modal, print status panel |
| `find_empty_required_lanes()` / `compute_scoped_runout_value()` | only the tools the current print uses, via the AMS backend's per-slot presence | pre-print check, `filament_runout_scoped` |

`print_scopes_runout_badge()` limits the scoped badge to PRINTING and PAUSED, since during Preparing the panel's tool list still describes the previous job.

### State defaults and the grace period

`FilamentSensorState::filament_detected` defaults to `true`, so a read before the first status frame never looks like a runout. On top of that, each discovery restarts a stabilization window (`helix::app_constants::startup::kSensorStabilizationPeriod`, 5 s) during which toasts are suppressed and `filament_any_runout` is held at 0. `has_any_runout()` itself ignores the window, so a print status panel opened right after a restart on an already-paused print still sees the runout.

### Toasts

`update_from_status()` collects notifications under the lock and fires the state-change callback and toasts after releasing it. A sensor edge produces a "Filament inserted" / "Filament removed" toast only when none of these hold:

| Suppressed when | Why |
|-----------------|-----|
| inside the stabilization window | first frames after connect are not news |
| the setup wizard is active | the wizard is walking the user through it |
| `AmsState::is_filament_operation_active()` | load/unload moves filament past sensors on purpose |
| the backend unloads to its hub between prints (`BackendTraits::unloads_to_hub_between_prints`, AD5X IFS) and no job holds the machine | the firmware auto-unloads between prints |
| removal edge inside the post-unload grace | the user just unloaded |
| `BackendTraits::raises_runout_fault_during_job` for the active backend, during a job | Happy Hare, AFC and CFS raise their own runout prompt (`include/ams_backend.h#BackendTraits`) |
| master switch off, sensor disabled, no role, or the firmware was not running the sensor on both sides of the edge | not monitoring |

When a job holds the machine and any AMS backend is present, a removal toast is not fired on the edge. It waits `kRunoutToastDwell` (45 s, `include/filament_sensor_types.h#"kRunoutToastDwell"`), because a tool change clears the toolhead sensor for 26 to 33 s and then refills it. A refill inside the dwell cancels the toast silently; the dwell is swept on every status frame rather than on a timer. Detection and the runout subjects still fire on the edge; only the toast waits.

### Bypass arming

Filament fed through an AMS bypass reaches the toolhead through no lane, so the toolhead sensor is the only runout protection, and some firmwares leave it disabled outside their own filament operations. `AmsState` reports its `any_bypass_active()` edge to `on_bypass_active_changed()`, which arms every RUNOUT-role sensor the firmware has disabled (`SET_FILAMENT_SENSOR SENSOR=<name> ENABLE=1`) and records them in `bypass_armed_`. On disengage it restores exactly that set. A sensor the firmware reports disabled in the meantime drops out of the set, since someone else now owns its state. The user's `enabled` setting is never touched. The API handle comes from `MoonrakerManager` via `set_moonraker_api()`.

## Temperature Sensors

`TemperatureSensorManager` owns four kinds: `TemperatureSensor`, `TemperatureFan`, `GenericHeater` and `DriverTemperature` (TMC2240/TMC5160), plus `HumidityChip`, because a humidity chip also reports a temperature. Extruders and `heater_bed` classify as `Heater`, which no sensor manager owns, so they stay in `PrinterState`. `heater_generic` objects arrive through the same object list, which is how filament dryer heaters (Happy Hare `MMU_heater`, QIDI Box `heater_boxN`) reach the temp graph without any filament-backend code.

`on_discovered()` assigns a role from the name, only to an entry with no role yet (`src/sensors/temperature_sensor_manager.cpp#role_for_name`): CHAMBER (name contains `chamber`, sort priority 0), MCU (contains `mcu`, 10), HOST (`raspberry_pi`, `host_temp`, `host`, `rpi`, or contains `raspberry`, 20), STEPPER_DRIVER (TMC objects, 30), AUXILIARY (everything else, 100). The roles are never persisted and never offered in the Settings dropdown; a rediscovery reproduces them. `get_sensors_sorted()` orders by priority.

Each sensor gets a temperature subject and a target subject in `DynamicSubjectMap`s (decidegrees; the target reads 0 for objects without one, and `klipper_object_has_target()` (`include/temperature_sensor_types.h#klipper_object_has_target`) answers which have one). `on_entries_changed()` creates and removes them to match the entry list, and `on_deinit_subjects()` clears them. Consumers fetch a subject and its token together:

```cpp
// src/ui/panel_widgets/thermistor_widget.cpp#bind_carousel_sensors
SubjectLifetime& lifetime = carousel_lifetimes_.emplace_back();
lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
```

The thermistor widget observes `temp_sensor_count` to rebind when the sensor set changes. In single mode it mirrors the selected sensor's temperature and target into per-instance subjects its `temp_display` binds to, and a tap on a sensor with a target opens the keypad and sends through `TemperatureController::set_target(klipper_name, ...)` (`src/ui/panel_widgets/thermistor_widget.cpp#open_target_keypad`). The keypad ceiling is `TemperatureController::keypad_max_for()`: the heater's configfile `max_temp` when the printer reported one, else 120°C.

Chamber sensors whose names lack `chamber` (Snapmaker `cavity`, Elegoo `enclosure`) are promoted by `PrinterState` once discovery resolves the chamber sensor: it calls `apply_chamber_sensor_override(chamber_sensor, chamber_heater)` (`src/sensors/temperature_sensor_manager.cpp#apply_chamber_sensor_override`), which demotes the incumbent CHAMBER sensor to an inferred role and promotes the named one. A `heater_generic` chamber heater is promoted alongside it, so it is graphed once as "Chamber" and not again as an auxiliary heater; a `temperature_fan` in the heater slot keeps its own role. A sensor name the printer does not report is ignored. A CHAMBER sensor is not a Settings row: `listed_in_settings()` hides it and the chamber assignment dropdowns own that choice. [CHAMBER_HEATER.md](CHAMBER_HEATER.md) owns the chamber heater/sensor assignment rules.

## Humidity Sensors

Recognized chips are one table, `humidity_sensor_chips()` in `include/humidity_sensor_types.h`. Adding a chip is one enum value and one row; classification (`classify()` in the object table walks the table), the subscription, config strings and display names all derive from it:

```cpp
// include/humidity_sensor_types.h#humidity_sensor_chips
static const std::vector<HumiditySensorChip> chips = {
    {HumiditySensorType::BME280, "bme280 ", "bme280", "BME280", true},
    {HumiditySensorType::HTU21D, "htu21d ", "htu21d", "HTU21D", false},
    {HumiditySensorType::SHT3X, "sht3x ", "sht3x", "SHT3X", false},
    ...
};
```

Prefixes carry the trailing space so `aht20 ` never matches `aht20_f heater_box1`. Only BME280 reports pressure. Each chip is a `TypeSpec` in `Options::types`, so the chip index the table returns is the entry's `type`.

Roles are CHAMBER and DRYER (exclusive, not offered in Settings), auto-assigned in `on_discovered()` to the first sensor whose name contains `chamber` or `dryer`. The humidity home widget turns `chamber_humidity` into the `chamber_humidity_text` string subject it binds. Happy Hare's dryer and the QIDI box read these chips' status directly through their AMS backends; see the backend docs.


## On/off Inputs

`InputSensorManager` lists every `gcode_button` and reads its `state` (`PRESSED` / `RELEASED`). Each input has a role: Door, or Other. A name containing `door`, `lid` or `cover` (any case) starts as Door; the user can change it in Settings > Sensors > Inputs, and the choice persists under `input_sensors` with the per-input `inverted` flag. A Door input reads closed while pressed, or while released when inverted, and an input with no reported state never reads open. Pressed-means-closed matches the hardware we have evidence for: Z-Mod's `c5pro_display_off.cfg` (ghzserg/z_c5pro) defines `[gcode_button frontDoor]` and `[gcode_button topDoor]` with a `release_gcode` that prompts "Front Door is Open!" and a `press_gcode` that clears it.

The one generic consumer is the door-open print-start gate (`src/printer/print_start_checks.cpp#gate_door_open`, documented with the gate pipeline in `FILAMENT_MANAGEMENT.md`), which warns when the door check is on, a Door input reads open, and the print has a material that needs a warm enclosure (`door_warning_material()`). The door check defaults on exactly when the printer has a Door input; an explicit choice persists as `input_sensors/door_check`. While printing, a Door input that opens also raises one warning toast per opening (`InputSensorManager::mid_print_door_warnings()`), by the same material rule applied to the active job: `ActivePrintMediaManager` sets `PrinterPrintState::print_materials()` from the job's file metadata through `materials_in_print()`, and while those materials are unknown the toast stays quiet. It also stays quiet when the printer's own config already reacts to that edge: `on_discovered()` reads each `gcode_button` section from the discovery data (the configfile pass runs discovery again), and a door whose `release_gcode` (its `press_gcode` when inverted) holds anything but whitespace is announced by the printer, so the toast skips it (`firmware_reacts_to_opening()`). Z-Mod's Creator 5 config is one such printer. The print-start gate does not apply this rule: a printer that prompts on the open edge does not check the door before a print.

Inputs whose meaning belongs to a subsystem, like a tool changer's dock and carriage buttons, show here as plain inputs; that subsystem keeps reading them its own way. Settings > Sensors lists each input through `InputSensorManager::row()` (live state, role, Invert), on the shared `sensor_row` like every other section.

What generic code asks (`include/input_sensor_manager.h`, `include/input_sensor_types.h`):

```cpp
InputSensorRole default_input_role(std::string_view sensor_name);
bool input_reads_open(InputSensorRole role, const InputReading& reading);
std::vector<std::string> doors_newly_open(const std::vector<std::string>& before,
                                          const std::vector<std::string>& now);
InputConfigReaction input_config_reaction(const nlohmann::json& section); // {on_press, on_release}
bool firmware_reacts_to_opening(const InputConfigReaction& reaction, bool inverted);

bool InputSensorManager::has_door_input() const;
std::vector<std::string> InputSensorManager::open_doors() const;
bool InputSensorManager::door_check_enabled() const;
bool InputSensorManager::firmware_reacts_to_opening(const std::string& klipper_name) const;
std::vector<std::string> InputSensorManager::mid_print_door_warnings(
    const std::vector<std::string>& newly_open, const std::vector<std::string>& print_materials) const;
void SensorManager::set_role(std::string_view klipper_name, uint8_t role);   // InputSensorRole
void InputSensorManager::set_option(std::string_view klipper_name, bool inverted); // Invert
void InputSensorManager::set_door_check_enabled(bool enabled);
```

Door is not an exclusive role: a front door and a top cover can both hold it. The persisted form is `{"sensors": [{klipper_name, role, enabled, type, inverted}], "door_check": bool}`, with `door_check` absent until the user sets it.

**Another kind of on/off input** (a Klipper object other than `gcode_button`) is one file: give its name pattern an `ObjectKind::Input` row in `src/printer/hardware_object_table.cpp`, then read its field in `InputSensorManager::parse_status`. Discovery, the subscription, roles, persistence, the overlay rows and the door gate follow from the row. **Another consumer** of a role asks `open_doors()` or adds a role to `kInputSensorRoles` and a matching query; it never names a printer.

Tests: `[input_sensors]` in `tests/unit/test_input_sensor_manager.cpp` (default roles, polarity, discovery and subscription, state parsing, door-check default, persistence, overlay row subjects, the mid-print edge rule, the printer-announces-it rule both ways). Mock: the Creator 5 personas publish `gcode_button frontDoor` / `topDoor`, and `creator5_zmod` also serves Z-Mod-style `press_gcode` / `release_gcode` for them; `HELIX_MOCK_DOOR_OPEN` opens them (`MOCK_ENVIRONMENT_VARIABLES.md`).


## Probe, Accelerometer, Width and Load Cell

**Probe.** The object table matches exact object names (`probe`, `bltouch`, `smart_effector`, `cartographer`, `beacon`) and the `probe_eddy_current <name>` prefix; the config-only `load_cell_probe` and `dockable_probe` sections describe the generic `probe` object, so beside it they type it (a load cell) and supply its `z_offset` rather than adding a sensor. One physical probe often registers several objects: Klipper's `bltouch`, `smart_effector` and `probe_eddy_current`, Beacon and the Cartographer plugin all also register the generic `probe`. `keep_most_specific()` keeps only the most specific object (and drops an eddy object beside a named scanner), and both `on_discovered()` and `extra_subscriptions()` go through it, so such a printer has one probe sensor, subscribed once. `on_discovered()` auto-assigns Z_PROBE when exactly one probe remains and none holds it; a saved `probe_sensors` role, applied by `load_settings()` afterwards, overrides that, including a saved role of none. Settings shows a probe's role as text and does not offer a dropdown (`offered` is false), so a printer with two genuinely distinct probes and no saved choice reads -1 on the probe subjects. `set_probe_type_override()` lets the printer database retype a generic `probe` as the real hardware; `PrinterState` calls it after detection.

### Probe status keys

What each object's `get_status()` returns, read from the upstream source (Klipper `461c4e37`, Kalico `0028cf70`, beacon3d/beacon_klipper `3eb01346`, Cartographer3D/cartographer3d-plugin `06e01690`, Cartographer3D/cartographer-klipper `d8fbed79`, vvuk/eddy-ng `1ed056b1`, CrealityOfficial/K1_Series_Klipper `e09f36e6`, CrealityOfficial/K2_Series_Klipper `bc0a5207`, QIDITECH/klipper `653d7a8f`). Klipper's `objects/list` (`Klipper3d/klipper: klippy/webhooks.py#_handle_list`) lists only objects that have `get_status`, so an object without one never reaches the object table.

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

Klicky has no module: it is a plain `[probe]` with dock macros, and `on_discovered()` retypes it from the macros.

- **`last_z_result`** is set only by the `PROBE` command. Klipper stores the toolhead Z at trigger (`bed_z + z_offset`, marked deprecated in `cmd_PROBE`); Kalico and the Cartographer plugin store the Z their probe run returns; Beacon stores trigger Z minus the probe's `z_offset`.
- **`last_query`** is the result of the last `QUERY_PROBE`, not a live endstop state; nothing publishes a live one. It drives `probe_triggered`. Klipper and Kalico publish a bool, the Cartographer plugin an int.
- **`z_offset`** is not published by any mainline, Kalico, Beacon or Cartographer module. The Creality K1 and K2 forks publish it and update it live through `Z_OFFSET_APPLY_PROBE`; the QIDI fork publishes it with `x_offset` and `y_offset` as static config values. It is requested on every probe object: Klipper answers a requested key the module lacks with `null` (`Klipper3d/klipper: klippy/webhooks.py#_do_query` fills it with `res.get(ri, None)`), which keeps the seed `on_discovered()` read from the config section, and a number replaces it. The Flashforge firmware's `null` is that fill, not a published field. Creality's Ender-3 V3 and Elegoo have no public Klipper source to check.
- A null or absent field never overwrites state.
- A Cartographer configured with `register_as_probe: false` beside a separate `[probe]` reads that probe's status: the objects list cannot tell the two apart.

**Accelerometer.** Found only in `configfile.config` sections, through `Scope::SectionOnly` rows and `rediscover_on_config` (`adxl345`, `adxl345 bed`, `lis2dw hotend`, ...). A `beacon` section with `accel_scale` or `accel_axes_map` adds Beacon's onboard LIS2DW. The role INPUT_SHAPER backs `is_sensor_available(AccelSensorRole::INPUT_SHAPER)`.

**Width.** Klipper allows one of each width-sensor module, so the names are fixed (`tsl1401cl`, `hall`). The first sensor is auto-assigned FLOW_COMPENSATION; `get_flow_compensation_diameter()` reads it. `filament_diameter_text` is pre-formatted for the width widget.

**Load cell.** Auto-assigns SPOOL_WEIGHT by name, or to a lone load cell. `has_spool_weight_load_cell()` is what `src/printer/consumption_sink.cpp` asks before trusting load-cell spool weight. Load cells have no `config_key`.


## Moonraker Sensors (`SensorState`)

`SensorState` (`include/sensor_state.h#SensorState`) derives from `SensorManager` with `Options::from_printer_discovery = false`, so the base's `discover()` does nothing. Its sensors come from Moonraker's `server.sensors.list`: partial discovery (Moonraker up, Klipper possibly not) calls `MoonrakerDiscoverySequence::discover_sensors()`, which sets the `sensor_count` capability subject and queues `SensorState::set_sensors()` to the main thread. `set_sensors()` builds a `SensorEntry` per sensor and hands the list to `reconcile()`, so the count, revision and carry-over rules are the base's. It has no roles, no `config_key` and no Settings section.

`subscribe()` registers for `notify_sensor_update`; `on_sensor_update()` defers each value through the manager's lifetime token. Values are per-sensor, per-key subjects in a `DynamicSubjectMap`, fetched with `get_value_subject(sensor_id, key, lifetime)`, rebuilt on every `set_sensors()`. They are centi-unit encoded (`to_centi_units()`): power, voltage and energy x 100, current x 100000 for milliamp precision. `format_value()` reverses it with units. `energy_sensor_ids()` picks sensors with power/voltage/current/energy keys; `src/ui/widgets/power_device_widget.cpp` is the main consumer.

## Settings > Sensors

Every section of Settings > Sensors is one `sensor_row` per sensor, drawn from the manager's `row()` and bound entirely through subjects.

### The row component

`ui_xml/sensor_row.xml` takes four props: `section` (the manager's `settings_section` slug), `index`, `role_options` (the dropdown's option labels, one per line, as translation keys) and `icon`. Everything it shows binds to subjects named `sensor_<slug>_row_<field>_<index>`:

| Field | Drives |
|-------|--------|
| `name`, `detail` (`has_detail`) | the name and the type or static reading line |
| `state` (`has_state`), `alert` | live state text, hidden when empty, in the warning style when `alert` is 1 |
| `role` (`has_role`) | the dropdown's selected index; the dropdown is hidden when the row offers no roles |
| `held_role` (`has_held_role`) | a held role the dropdown does not offer, shown as text |
| `toggle_kind`, `toggle` | which toggle (0 none, 1 Enabled, 2 Invert) and its value |

`ui_xml/sensors_overlay.xml` has one setting group per section and repeats the component with `<repeat count="sensor_<slug>_row_count">`:

```xml
<!-- ui_xml/sensors_overlay.xml -->
<repeat count="sensor_filament_row_count">
  <sensor_row section="filament"
              index="${i}" icon="filament_sensor" role_options="None&#10;Runout&#10;Toolhead&#10;Entry"/>
</repeat>
```

### Row sections

`SensorRowSection` (`include/ui_settings_sensors.h#SensorRowSection`) owns one section's subjects. `SensorSettingsOverlay::init_subjects()` creates one for every manager in `sensor_managers()` that has a `settings_section`, so a new manager gets its section's subjects with no UI code. `fill()` lists the entries `listed_in_settings()` accepts, asks `row(i)` for each `SensorRowView`, grows the row subjects first, writes them with change checks, and sets the row count last: the `<repeat>` rebuild that the count triggers binds by name, so the subjects must exist before it fires.

Refresh is revision-driven. On activation each section observes its manager's `revision_subject()` with the manager's `subjects_lifetime()` and refills on every change; deactivation stops the observers. A status frame, a role change or a rediscovery all end in `publish()`, which bumps the revision, which refills the rows. Nothing in the overlay polls.

### Role options map through RoleSpec

The dropdown's option index is not a role id. `pick_role()` maps the index through `manager.offered_roles()`, the ids of the `RoleSpec`s with `offered` set, in `Options::roles` order, then calls `set_role()` and `save_settings()`. `row()` builds `role_labels` from the same list, so the labels, the XML `role_options` and the mapping all follow one order: the XML option list must name the offered roles in `Options::roles` order. A held role that is not offered (a probe's Z_PROBE, a chamber humidity role) has `selected_role` -1 and shows through `held_role_label`; the dropdown's own selection is hidden so it never contradicts it. The toggle follows `row_toggle`: `Enabled` calls `set_enabled()`, `Invert` calls `set_option()`.

A manager customizes its rows by overriding `row()` (a display name, a live state, a toggle policy) and `listed_in_settings()` (filament hides AMS-owned sensors, temperature hides the chamber sensor), never by writing UI code.

## UI Consumers

| Surface | Reads |
|---------|-------|
| Settings > Sensors (`SensorSettingsOverlay`) | every manager through `SensorRowSection`; the chamber heater and sensor dropdowns and the filament master switch are overlay-specific |
| First-run wizard (`WizardFilamentSensorSelectStep`) | non-AMS filament sensors; assigns RUNOUT |
| Runout guidance modal (`FilamentRunoutHandler`) | `has_real_runout()` on pause, `filament_any_runout` to auto-close |
| Print status panel and `filament_sensor_indicator.xml` | `filament_runout_detected`, `filament_runout_scoped`, `filament_sensor_count` |
| Home widgets | thermistor (per-sensor temp subjects), humidity (`chamber_humidity`), width (`filament_diameter_text`), filament sensor |
| Temp graph, temperature history | `TemperatureSensorManager` sensor list and per-sensor subjects |
| Telemetry | per-manager `sensor_count()` |

## Mock and Tests

`--test` seeds the same managers through the same discovery and status paths, so mock runs exercise production code. `HELIX_MOCK_FILAMENT_SENSORS` and `HELIX_MOCK_FILAMENT_STATE` shape the filament sensor set; see [MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md).

| Tag | File | Pins |
|-----|------|------|
| `[hw_object_table]` | `tests/unit/test_hardware_object_table.cpp` | the table's classification and `status_fields()` |
| `[sensor_manager_base]` | `tests/unit/test_sensor_manager_base.cpp` | reconcile, persistence, roles, publish, rows |
| `[sensor_golden]` | `tests/unit/test_sensor_discovery_golden.cpp` | discovery results per manager against fixed printers |
| `[subscription_parity]` | `tests/unit/test_subscription_parity.cpp` | the subscription map the table and managers produce |
| `[sensor_row]` | `tests/unit/test_sensor_row_xml.cpp`, `tests/unit/test_sensor_settings_overlay.cpp` | the row component and the section subjects |
| per manager | `tests/unit/test_*_sensor_manager.cpp`, `test_load_cell_manager.cpp`, `test_sensor_state.cpp`, `test_filament_sensor_bypass_arming.cpp` | each manager's own rules |

Each manager declares a `*TestAccess` friend for resetting the singleton between cases, and tests call `set_sync_mode(true)` so a refresh runs inline. Run one category with `make t F='[humidity]'` (check the file for its tags).

## Adding a New Sensor

### A new object or chip of an existing kind: one table row

Add a row to `kRows` in `src/printer/hardware_object_table.cpp` (or a chip to `humidity_sensor_chips()` for humidity), with the `chip` index from the manager's `*_types.h` enum if the kind has types. Nothing else changes: the manager discovers it, the subscription builder subscribes its `status_fields()`, and the Settings row appears. Add a case to `[hw_object_table]` and, if the status shape is new, one to the manager's test.

### A new category: one manager class plus one registry line

1. **Kind.** Add the `ObjectKind` value, its `status_fields()` case and its table rows in the object table.
2. **Types header** `include/<thing>_types.h`: role and type enums with string tables, and the reading struct.
3. **Manager** in `src/sensors/` (namespace `helix::sensors`), a Meyers singleton deriving `SensorManager`. Copy `LoadCellManager` (the smallest) or `TemperatureSensorManager` (per-sensor subjects). It supplies a `make_options()` and the hooks it needs:
   - `register_subjects()` for fixed subjects (a manager with none leaves it empty);
   - `parse_status()` using `find()` plus a type check, under the mutex;
   - `refresh_subjects()`, which copies under a short lock and sets subjects unlocked;
   - `on_discovered()` for auto roles, `on_entries_changed()` to resize readings, and the `read_*` / `write_*` extras only if it persists more than role, enabled and type;
   - `extra_subscriptions()` only if its fields vary beyond the table;
   - `row()` only if the default row does not fit.
   In `Options`: `kinds`, `count_subject`, `revision_subject`, `roles`, `types`, `settings_section` and a `config_key` only if users assign roles or toggles.
4. **Register** the manager in `src/sensors/sensor_manager.cpp`: one line. Subject init, discovery, the configfile pass (when `rediscover_on_config` is set), the status fan-out, subscriptions, telemetry and the Settings section's subjects all follow.
5. **Settings section.** Add a `setting_group` to `ui_xml/sensors_overlay.xml` gated on the manager's count subject, with a `<repeat count="sensor_<slug>_row_count">` of `sensor_row` (`role_options` in `Options::roles` order if roles are offered).
6. **Source lists.** A new `.cpp` under `src/` goes into `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (or `app_srcs_excluded.txt` with a reason).
7. **Home widget**, if any: a `hardware_gate_subject` in `src/ui/home/panel_widget_registry.cpp` (see [PANEL_WIDGET_GUIDE.md](PANEL_WIDGET_GUIDE.md)).
8. **Mock.** Add the objects and an initial status to the mock client so `--test` shows the category.
9. **Tests.** A tagged test that discovers the new names, feeds a status frame, and round-trips settings if it persists.

Vendor names stay out of generic code: if one firmware publishes the reading under its own object, put that knowledge in one provider module the way `helix::power_loss` does (`include/power_loss_sensor.h`), and let the subscription builder ask it for `required_status_objects()`. See `.claude/rules/vendor-abstraction.md`.

## Gotchas

- **`refresh_subjects()` must not hold the manager mutex while it sets a subject.** Observers run synchronously and call back into managers and `AmsState`. Copy under a short lock, then set.
- **`discover()`, `load_settings()` and the setters are main-thread only.** Only `update_from_status()` may arrive from another thread; `parse_status()` runs under the mutex and `publish()` defers the subject work.
- **A saved role overrides an auto role.** `load_settings()` runs after `discover()`. An auto-assignment in `on_discovered()` must not take a role something else holds, and one that must not undo a user's choice records the names it has seen.
- **`rediscover_on_config` repeats `discover()`.** Turn it on only for a manager that reads config sections in `on_discovered()`, and make its `on_discovered()` safe to run twice. Without the flag a config-section manager never sees the sections, because the first pass runs before `configfile.config` arrives.
- **A new manager must be in the registry.** A manager that is not in `sensor_managers()` gets no subjects, no discovery, no status and no Settings section.
- **The XML `role_options` order is the `Options::roles` order of the offered roles.** A mismatch shows the wrong label for a role without any error.
- **`DynamicSubjectMap` subjects need their lifetime.** An observer that outlives a rediscovery must hold the token from `get_or_create()` or the getter, or it observes a freed subject.
- **Null fields are normal.** A field-restricted subscription sends `null` for fields an object lacks. Never `get<>()` without checking the type.
- **Use presence queries for "is it there", runout queries for "act on it".** Mixing them either ignores a firmware stand-down or alerts on a sensor the firmware is deliberately not running.
