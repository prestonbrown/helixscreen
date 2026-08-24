# Power-Loss Recovery (PLR)

HelixScreen offers to resume an interrupted print at connect time. Two printer
firmware families expose a recovery mechanism, and they work in fundamentally
different ways: one is **passive** (a status field tells us a snapshot exists),
the other is **active** (we have to ask, and asking has side effects).

Code map:

| File | Role |
|------|------|
| `include/plr_backend.h`, `src/printer/plr_backend.cpp` | Pure backend strategy: capability selection, probe-response parsing, resume/discard plan building. No LVGL, no network. |
| `include/plr_offer.h`, `src/printer/plr_offer.cpp` | Pure offer decision (`plr_should_offer`) and latch re-arm rule (`plr_should_rearm`). |
| `include/plr_offer_controller.h`, `src/ui/ui_plr_offer_controller.cpp` | App-lifetime controller. Observers, one-shot latch, the Creality probe, normalization of both backends into one "recovery available" signal. |
| `src/ui/ui_plr_prompt.{h,cpp}` | The modal and its two button actions. Backend-agnostic — it executes a `PlrRecoveryPlan`. |
| `src/printer/printer_print_state.cpp` | Parses both capability markers out of the status payload. |
| `src/api/moonraker_api_print.cpp` | `check_continue_print_state()` / `cancel_continue_print()` JSON-RPC calls. |

Tests: `tests/unit/test_plr_backend.cpp`, `test_plr_offer.cpp`, `test_plr_state.cpp`,
`test_plr_prompt.cpp`, `test_plr_error.cpp`.

---

## The two backends

### Snapmaker U1 — passive

Snapmaker's Klipper fork adds `virtual_sdcard.pl_env_valid`. The firmware
validates a coherent power-loss snapshot against MCU flash during boot and sets
the flag itself. Mainline Klipper has no such field, and our parser only accepts
a JSON boolean, so the flag is **self-gating**: `pl_env_valid == true` already
means "Snapmaker firmware with a valid recovery snapshot".

No printer-model or AMS-backend gate is needed, and none should be added — an
earlier redundant "AMS backend == SNAPMAKER" gate wrongly suppressed the offer
on an AFC-modded U1 (bundle UDZJQVQZ) whose backend is not the Snapmaker one.
`tests/unit/test_plr_offer.cpp` pins that regression.

| | |
|---|---|
| Capability + availability | `virtual_sdcard.pl_env_valid == true` |
| Recovery filename | `virtual_sdcard.file_path` |
| Resume | gcode `SDCARD_PRINT_PL_RESTORE` |
| Discard | gcode `SDCARD_PRINT_PL_CLEAR_ENV` |
| Errors | JSON-coded; `snapmaker_extract_coded_msg()` pulls out the human `msg` |

### Creality K1 / K1C / K1 Max / K2 Plus / Ender 3 V3 / Hi / i7 — active

Verified by reading the Klipper source on a physical K1C and K2 Plus and by
running the endpoint live against an idle K1C.

Creality's fork registers a Klipper **webhook endpoint**,
`pause_resume/check_continue_print_state`. Moonraker auto-registers every
non-reserved Klipper webhook endpoint (`klippy_connection.py:387-396`), so it is
reachable both ways:

* HTTP: `POST /printer/pause_resume/check_continue_print_state`
* JSON-RPC: `printer.pause_resume.check_continue_print_state`

HelixScreen uses the JSON-RPC form — we already hold the WebSocket.

Live response on an idle K1C:

```json
{"result": {"file_state": false, "eeprom_state": false}}
```

A recovery is available **only when both `file_state` and `eeprom_state` are
true**.

| | |
|---|---|
| Capability | presence of `print_stats.power_loss` (see below) |
| Availability | one-shot probe returns `file_state && eeprom_state` |
| Recovery filename | sidecar JSON, `<base>/creality/userdata/config/print_file_name.json`, key `file_path` |
| Resume | gcode `SDCARD_PRINT_FILE FILENAME="<file>" ISCONTINUEPRINT=1` — quoted, and `<file>` **relative to the sdcard root** (both are load-bearing; see "Filename safety") |
| Discard | JSON-RPC `printer.pause_resume.cancel_continue_print` |

`<base>` is `/usr/data` on K1-class and `/mnt/UDISK` on the OpenWrt-class K2.
HelixScreen runs on the printer, so the read normally succeeds; it is
best-effort and **read-only** — never write to that path.

---

## Capability detection: `print_stats.power_loss`

`power_loss` is a `print_stats` field that exists **only** in Creality's fork.
Mainline Klipper has no such key. So the marker is the **presence** of the key,
not its value — it normally reads `0` and only becomes `1` after the probe.

Two traps:

1. **Presence must mean "present *and* numeric".** We subscribe to a narrowed
   field list, and Moonraker answers a subscribed-but-unpopulated field with an
   explicit `null`. So on mainline Klipper the key *is* present in the payload,
   as `null`. `is_number()` is what actually discriminates the fork.
2. **The capability latches up and never down.** Moonraker sends deltas; a later
   `print_stats` notification carrying only `print_duration` has no `power_loss`
   key at all. Clearing on absence would produce a spurious 1→0→1 edge and
   re-fire the side-effectful probe. Capability is reset only on the disconnect
   edge, by the offer controller.

Subscription: `power_loss` is in the `print_stats` field list in
`MoonrakerDiscoverySequence::build_subscription_objects()`.

---

## SAFETY: the probe must precede the resume

**This ordering is mandatory, not stylistic.**

The stock Creality sensorless-homing macro gates its pre-homing Z clearance lift
on:

```jinja
{% if printer.print_stats.z_pos|float <= 20.0 or printer.print_stats.power_loss == 1 %}
    ... big lift ...
{% else %}
    ... 0.1mm lift ...
{% endif %}
```

After a power loss high up in a tall print, `z_pos` correctly reads e.g. 150. So
**without** `power_loss == 1` the machine lifts only 0.1 mm and then
sensorless-homes X/Y — dragging the nozzle straight through the part.

Calling `check_continue_print_state` is what sets `print_stats.power_loss = 1`
(and only when both states are true and `print_stats.state == "standby"`).

> **Never issue the Creality resume gcode unless the probe has been made this
> connection and returned both states true.**

This is enforced in code, not by comment:

* `PlrDetectResult::completed` is set **only** by
  `plr_parse_check_continue_response()` on a well-formed
  `result.{file_state,eeprom_state}` boolean pair.
* `plr_build_plan(CREALITY, file, detect)` returns an **empty `resume_gcode`**
  unless `detect.completed && detect.file_state && detect.eeprom_state`.
* `PlrOfferController` refuses to show the prompt when
  `plan.resume_allowed()` is false, and does not latch the one-shot.
* The prompt's Resume handler re-checks `plan.resume_allowed()` before sending.

Pinned by `tests/unit/test_plr_backend.cpp` — "CREALITY refuses resume when the
probe never ran" and its half-confirmed siblings.

### Second footgun: the EEPROM gate

The Klipper-side resume branch requires the `bl24c16f` EEPROM object. If it is
absent, `ISCONTINUEPRINT=1` **silently starts the print from the beginning**
rather than failing. The `file_state && eeprom_state` gate covers this. Do not
weaken it to `file_state` alone.

### Probe discipline

`check_continue_print_state` is **side-effectful**:

* on JSON parse failure it **deletes** the recovery sidecar;
* it clears `exclude_object_info` when the state is false;
* it sets `print_stats.power_loss = 1` when both states are true.

Therefore the probe is issued:

* **at most once per connection** (latched by `creality_probed_this_connect_`,
  re-armed only on a CONNECTED → not-CONNECTED edge);
* **only when `print_stats.state == "standby"`**;
* **never polled**.

---

## Normalization and reuse

Both backends collapse into one internal signal so the existing latch / re-arm /
wizard logic is reused unchanged:

```
Snapmaker:  pl_env_valid subject ──────────────────────┐
                                                        ├──> recovery_available ──> plr_should_offer()
Creality:   power_loss key ──> one-shot probe ──> both ─┘
                               (standby only)    states
```

`PlrOfferSignals::recovery_available` is that normalized field.
`plr_should_offer()` stays pure — no LVGL, no threading, no singletons — and its
other three inputs (`printer_idle`, `already_prompted`, `wizard_active`) are
unchanged.

The `printer_idle` input comes from the derived lifecycle, not the wire: the
controller computes `idle = !job_holds_machine(get_print_lifecycle())`
(`src/ui/ui_plr_offer_controller.cpp:88`). During a host-side pre-print block
`print_stats` still reads `standby` — it describes the previous job — so an
idle check derived from the wire would offer "Resume interrupted print?" on top
of a start the user has already committed to, and the Resume button would start
a different file than the one they chose. Anchoring idle to the lifecycle means
a committed start never gets the offer. (`PRINT_STATE_MACHINE.md` § "Asking
whether a job owns the machine" covers the predicate and why `print_active` is
the wrong signal for this question.)

The one-shot latch, the wizard-close re-evaluation, and the disconnect re-arm
are documented at the decision site, `PlrOfferController::evaluate_offer()`.

### Why the prompt carries a plan

`show_plr_recovery_prompt()` takes a `PlrRecoveryPlan` by value. The plan owns
its strings, so the button handlers never re-derive the backend or re-check the
invariant against live state that may have moved on between the offer and the
tap. The modal itself stays backend-agnostic: primary button runs
`plan.resume_gcode`, secondary runs `plan.discard_gcode` **or**
`plan.discard_rpc_method`.

### Filename safety

The Creality resume gcode embeds the filename as an extended-command parameter,
and **it must be double-quoted**:

```
SDCARD_PRINT_FILE FILENAME="/usr/data/printer_data/gcodes/My Part_PLA.gcode" ISCONTINUEPRINT=1
```

Klipper tokenizes extended parameters with `shlex` in POSIX mode
(`whitespace_split=True`, `commenters="#;"`), then splits each token on the
first `=`. An unquoted filename containing a space therefore produces tokens
with no `=` at all, and the whole command is rejected before the file is ever
looked up:

```
{"code":"key514", "msg": "Malformed command args 'SDCARD_PRINT_FILE FILENAME=/usr/data/
 printer_data/gcodes/PTOP Phone Stand_Elegoo PLA Matte Slate Grey_1h55m.gcode
 ISCONTINUEPRINT=1 ; from helixscreen'", "values": ["not enough values to unpack
 (expected 2, got 1)"]}
```

Spaces are the common case, not the exotic one — slicers put the model and
filament names in the filename. Moonraker's own print-start path quotes it
identically (klippy_apis.py, `SDCARD_PRINT_FILE FILENAME="{filename}"`).

Inside the quotes, `\n`, `\r`, `;`, `#`, `*`, `=`, `"` and `\` are still
rejected: `"` closes the value early, `\` is shlex's POSIX escape, and the rest
are comment/terminator characters that older regex-path Klipper forks strip
before shlex ever runs. `plr_is_safe_recovery_filename()` enforces that;
`MoonrakerAPI::is_safe_gcode_param()` is the wrong helper here because it
rejects whitespace. `IMoonrakerAPI::is_safe_material_param()` allows whitespace
but is the wrong helper too, and in the other direction: it is an **allowlist**
(alphanumerics plus `+ - _ . ( ) /`), while a slicer filename legitimately
carries commas, brackets, percent signs and non-ASCII. `GCODE_PARAM_BREAKERS`
is the **blocklist** form of the same idea - `\n\r;#*="\` - which is what a
free-form filename needs. Same shlex mechanism, three call-site-specific
charsets; the material one is in `FILAMENT_MANAGEMENT.md` § "Material names as
G-code parameter values".

> A trailing ` ; from helixscreen` comment would ALSO break this command, and
> for a non-obvious reason: Creality special-cases `SDCARD_PRINT_FILE` to use a
> second regex (`extended_r1`) whose terminators are `|` and `*`, *not* `;` and
> `#` — "Support filename contain '#'". So the `;` survives into
> `shlex.split()`, which is called without `comments=True` and therefore treats
> `;` as an ordinary character. HelixScreen no longer annotates outgoing G-code
> at all (`src/api/moonraker_gcode_guards.h`), so this is historical — but it is
> why the field report showed both faults in one line.

### Path form: relative, not absolute

The sidecar stores an **absolute** path (`/usr/data/printer_data/gcodes/…`),
because Klipper writes it from the opened file handle's `.name`.
`SDCARD_PRINT_FILE` cannot be given that path:

```python
if filename[0] == '/':
    filename = filename[1:]          # cmd_SDCARD_PRINT_FILE
self._load_file(gcmd, filename, check_subdirs=True)
```

`_load_file` looks the name up in `get_file_list()`, whose entries are relative
to `sdcard_dirname` (`/usr/data/printer_data/gcodes` on K1-class). A
de-slashed absolute path is not in that list, so the lookup raises and the user
gets `{"code":"key121", "msg": "Unable to open file"}`.

There is a second, quieter reason. Once the file is open, the resume branch
does:

```python
if result.get("file_path", "") == self.current_file.name:
    sameFileName = True
else:
    os.remove(self.print_file_name_path)   # recovery data discarded
```

`current_file.name` is `os.path.join(sdcard_dirname, <relative name>)`. Sending
the relative name is what makes that comparison succeed; anything else that
still manages to open would **delete the recovery snapshot and print from the
beginning**.

`plr_creality_sdcard_relative_name()` does the stripping — known data roots
first (`/usr/data`, `/mnt/UDISK`), then the first `/gcodes/` segment as a
fallback for a relocated `virtual_sdcard: path`.

Verified by reading klippy/extras/virtual_sdcard.py and klippy/gcode.py on a
physical K1C.

With no resolvable filename there is no safe command to send, so the offer is
suppressed entirely rather than showing a Resume button that cannot work.

---

## Threading

`update_from_status()` runs on the libhv WebSocket thread; both capability
parsers only mutate an already-initialized `lv_subject_t` int in place, matching
the surrounding code in `printer_print_state.cpp`.

`PlrOfferController`'s observers are registered with `observe_int_sync`, which
defers through `UpdateQueue`, so every callback body runs on the main thread.
The one place that genuinely crosses the boundary is the probe response — a
JSON-RPC callback that arrives on the WebSocket thread — so it is wrapped in
`AsyncLifetimeGuard::bg_cb()`.
