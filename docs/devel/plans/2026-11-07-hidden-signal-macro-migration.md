# Moving existing printers to the hidden signal macro names (#2006)

HelixScreen 1.1 names the eleven signal macros with a leading underscore (`_HELIX_READY`,
`_HELIX_ENDED`, `_HELIX_RESET`, `_HELIX_PHASE_*`) on a new install, so Fluidd and Mainsail leave
them out of their macro lists. A printer that already defines `HELIX_READY` keeps the visible
names through every update. This plan moves those printers to the hidden names in 1.2 without
ever leaving a call that answers `Unknown command`.

This is a plan, not code. It states interfaces, tests and constraints; bodies are the
implementer's. Every new header gets a `scripts/syntax_check.py` pass before its phase starts.

## What exists today

| Piece | Where | What it does now |
|---|---|---|
| Name choice | `src/printer/macro_manager.cpp#MacroManager::macro_content_for` | `has_helix_macro("HELIX_READY")` picks the visible spelling for the whole pack |
| Visible rewrite | `src/printer/macro_manager.cpp#MacroManager::with_visible_signal_names` | erases the leading `_` from `_HELIX_READY`, `_HELIX_ENDED`, `_HELIX_RESET`, `_HELIX_PHASE_` |
| Callers of the choice | `src/printer/macro_manager.cpp#MacroManager::install_files`, `#MacroManager::update_files` | both upload `macro_content_for(hardware_)` as `helix_macros.cfg` |
| The pack | `assets/config/helix_macros.cfg` (v2.1.0) | defines the eleven hidden signal macros, `_HELIX_STATE`, and four visible helpers; `HELIX_START_PRINT` calls the hidden names |
| Version rungs | `src/printer/macro_manager.cpp#parse_installed_version` | `HELIX_UNLOAD_FILAMENT` = 2.1.0, `HELIX_READY` = 2.0.0, `HELIX_START_PRINT` = 1.0.0 |
| Status | `src/printer/macro_manager.cpp#MacroManager::evaluate_status` | OUTDATED on an older rung or an unwrapped skip step; reaches the UI through `src/printer/printer_plugin_status_state.cpp#PrinterPluginStatusState::set_helix_macros_base_status` and the update button in `src/ui/controls/ui_panel_advanced.cpp#AdvancedPanel::run_helix_macros_stage` |
| Macro set | `src/printer/printer_discovery_parse.cpp` (`gcode_macro` branch) | every macro into `macros_`; only names starting `HELIX_` into `helix_macros_`, so `_HELIX_READY` is visible through `has_macro()` and never through `has_helix_macro()` |
| Config analysis | `src/api/moonraker_discovery_sequence.cpp` (configfile step) | queries `configfile.settings` and runs pure analyzers into `hardware_` under `hardware_mutex_`, e.g. `skip_wrappers::wrappable()/active()` into `PrinterDiscovery::set_skip_wrappers` |
| Marker strip | `moonraker-plugin/strip_phase_tracking.py#main`, `#compute_stripped_content`, `#safe_replace_file` | removes `# <<< HELIX_TRACKING v2 >>>` / `# <<< /HELIX_TRACKING >>>` blocks around a single `HELIX_PHASE_*`/`HELIX_READY` call inside a `[gcode_macro]` body; backs up to `<file>.bak.<YYYYmmdd_HHMMSS>`, verifies the backup, refuses any anomaly, detects a concurrent change; exit 0/1/2 |
| Strip callers | `moonraker-plugin/install.sh#strip_phase_tracking_instrumentation` | uninstall paths only, including the chroot one |
| Plugin load | `moonraker-plugin/helix_print.py#HelixPrint.component_init` | loads siblings with `_load_sibling`, starts each feature through `_start_isolated`, reports it with `register_feature` in `/server/helix/status` `features` |
| Opt-ins | `moonraker-plugin/helix_print.py` (`plugin_features`, `always_on`) | an actor without its own `switch` defaults off; a `switch` owns its on/off |
| Self-heal | `include/plugin_self_heal.h#PluginSelfHeal`, `src/system/helix_plugin_installer.cpp#decide_plugin_self_heal` | local Moonraker: relink and restart Moonraker when the running plugin version differs; remote Moonraker: `ReportOnly`, a log line |
| Signal parse | `src/print/print_start_collector.cpp#PrintStartCollector::check_helix_phase_signal`, completion pattern `HELIX:READY` near the top of that file | reads `HELIX:PHASE:<name>` and `HELIX:READY` responses; the `RESPOND MSG` text does not change with the macro name, so nothing here moves |
| Feature overview | `include/helix_plugin_features.h#kKnownPluginFeatures` | the app's table of plugin features it can name |

Nothing else in `src/`, `include/`, `config/`, `ui_xml/` or `docs/user/` names the visible signal
macros. `docs/devel/PRINT_START_INTEGRATION.md` § "Available Macros" describes the current rule.

## Decisions

### D1. The forwarders are a generated block at the end of `helix_macros.cfg`, not a second file

The issue proposes a small file of forwarding macros. A second file needs an include, and the
include and the file have to agree at every Klipper restart: Klipper refuses to start on a
missing plain include, and a forwarder loaded beside the old visible pack merges with its
same-named section (Klipper merges same-named sections across files, later file winning per
option) and forwards to a `_HELIX_*` macro the old pack does not define. Appending the block to
the content `macro_content_for` already generates keeps it to one atomic upload of one file,
with no new include, no ordering between two uploads, and nothing extra for `uninstall()` to
delete. The base file on disk (`assets/config/helix_macros.cfg`) does not change and keeps
v2.1.0, so the version rungs and `get_macro_names()` stay as they are.

The block has one section per aliased name, in this exact shape:

```
[gcode_macro HELIX_READY]
description: HelixScreen alias for _HELIX_READY
gcode:
    _HELIX_READY
```

The `description` line is how a later scan tells our forwarder from a macro of the same name the
user wrote (`kAliasDescriptionPrefix`).

### D2. The scan reads the loaded config and errs toward keeping aliases

The scan reads `configfile.settings`, the config Klipper loaded at its last restart, in the
existing discovery step. It looks in every string option of every section, not only
`gcode_macro` bodies: `[delayed_gcode]`, `[gcode_button]`, `[idle_timeout]`, runout sensors and
`[homing_override]` all hold gcode. It skips the sections the bundled pack defines (under both
spellings) and our own alias sections. A name matches case-insensitively, with no
`[A-Za-z0-9_]` on either side, so `_HELIX_READY` is not a call to `HELIX_READY` and
`HELIX_READYX` is no call at all.

A false positive keeps a visible alias, which is cosmetic. A false negative leaves a call that
answers `Unknown command` and loses the ready signal for that print. So the scan over-matches on
purpose: a mention inside a Jinja string or a `printer["gcode_macro HELIX_READY"]` lookup counts
as a call. Only the names a caller uses are aliased (Q3 asks whether that should be all eleven).

A call written to disk but not yet loaded is invisible to the scan. It loads at the same restart
as the pack, so for one print it answers `Unknown command`; the next discovery sees it, the
status goes OUTDATED, and the next update stages its alias. Reading the on-disk config through
`server.helix.config` would close that window only where the plugin runs, so it is left out.

### D3. Legacy marker blocks are renamed to the hidden names, not stripped

The issue proposes running `strip_phase_tracking.py` on plugin load. Stripping removes the
`HELIX_READY` call the 1.0.x plugin put at the end of `PRINT_START` and every phase call, so those
printers would fall back from the definitive signals to the collector's heuristics. Renaming the
one call line inside an exact marker block (`HELIX_READY` to `_HELIX_READY`) keeps every signal,
and the scan then stops counting those calls, so the alias can go. A rename is only safe once
Klipper has the hidden names loaded, so the plugin checks `configfile.settings` for each hidden
name a block would call before it writes anything (Phase 3). Strip stays what uninstall runs; its
call-line pattern learns to accept the underscore, so a renamed block still uninstalls cleanly.
Preston may still prefer strip (Q1).

### D4. Safety of editing config on plugin load

| Concern | Rule |
|---|---|
| Scope | Only lines inside a well-formed marker block, inside a `[gcode_macro]` body, whose single call line matches `^[ \t]*(HELIX_PHASE_[A-Z_]+\|HELIX_READY)[ \t]*$`. Every anomaly rule `compute_stripped_content` applies today applies unchanged; a file with any anomaly is left byte-for-byte alone. Indentation and the line ending (CRLF too) are kept. Klipper's `printer-YYYYMMDD_HHMMSS.cfg` snapshots and `config_backups/` are skipped as today |
| Backups | `safe_replace_file` as is: a verified `<file>.bak.<stamp>` beside the file before the atomic replace, and the backup moved back if the write does not verify. The name ends in `.bak.<stamp>`, so no `[include *.cfg]` glob loads it |
| Idempotence | A renamed block calls a hidden name, which the rename treats as clean. The second load edits nothing and writes no backup, so a file gets one backup from this migration, ever |
| Ordering | Nothing is written until every hidden name the planned edits call is in Klipper's loaded `configfile.settings`. Until then the state is `waiting_for_pack` |
| Dry run | Every load computes the plan first and logs it in one line before acting, e.g. `HelixPrint: signal macro migration: renaming 9 call(s) in 1 file(s): printer.cfg (9)`. With the switch off the same line says `would rename` and the plugin writes nothing. `strip_phase_tracking.py --rename-hidden --dry-run CONFIG_DIR` prints the same plan by hand |
| Opt-out | `[helix_print] migrate_signal_macros: False` in `moonraker.conf`. It is the feature's `switch`, so no `plugin_features` key turns it on or off, and it works the same on a remote Moonraker. Default on (Q2) |
| No restart | The plugin never restarts Klipper. Klipper keeps running the loaded macros, which still call names the aliases define, until the next restart |
| Concurrency | `safe_replace_file`'s concurrent-change check stays; a file Fluidd wrote mid-run is skipped and retried at the next `klippy_ready`. Runs are serialized by one `asyncio.Lock`; file I/O runs off the event loop |
| Reporting | The feature `signal_macro_migration` in `/server/helix/status`, the endpoint `GET /server/helix/signal_migration` with the last result, persisted in Moonraker's database so it survives the Moonraker restart that follows, and the Moonraker log lines. The app shows it in Phase 4 |

### D5. Remote Moonraker

The rename runs inside the plugin, so it runs on whatever host Moonraker runs on, remote ones
included, as soon as that host has a plugin that carries it. What does not reach a remote host is
the plugin update itself: self-heal only logs `ReportOnly` there. Phase 4 adds a line to the
macros row naming the plugin version that does the rename and the existing remote install
command, shown only while the scan still finds marker-block callers and the plugin does not report
`signal_macro_migration`. Printers without the plugin at all keep their aliases; the scan and the
aliases need no plugin.

### D6. Sliced files

A scan of the config cannot see a sliced file that calls `HELIX_READY` itself. On a printer that
moved to hidden-only, that print gets `Unknown command:"HELIX_READY"` (Klipper's default command handler
answers with an info response rather than raising, so the print should continue; confirm on one
printer before Phase 4 relies on it) and no ready signal from the file. Phase 4 recognises that
response during a print, records the name in settings so the next macro update aliases it, and
shows one notice saying which name the file calls. The release note says the same thing up front.

## Phases

Each phase lands on its own with its own tests. While building, run only that phase's tags
(`make t F='[signal_macros]'`, `make t F='[config]'`, `make test-plugin` filtered with
`-k` where the target allows it); the batch gets one review and one `make land-gate`, and
`mutate-diff` after landing, per CLAUDE.md § Verification Cadence.

### Phase 1: aliases replace the visible rewrite

Every printer's pack defines the hidden names. A printer that defines `HELIX_READY` gets the
alias block for all eleven names, so its macros and sliced files keep working, and Klipper now
also has the hidden names loaded, which Phase 3 waits for. No status change: the new content
lands at the next update the user makes for any reason. `with_visible_signal_names` is deleted
here (step 3 of the issue), since nothing else calls it.

**Interfaces.** New `include/signal_macro_names.h`, `src/printer/signal_macro_names.cpp`, no I/O,
no LVGL, `json_fwd.h` only:

```cpp
namespace helix::signal_macros {

/// The signal macros' visible spellings, in pack order. The hidden name is "_" + this.
inline constexpr std::array<std::string_view, 11> kVisibleNames = {
    "HELIX_READY", "HELIX_ENDED", "HELIX_RESET",
    "HELIX_PHASE_HOMING", "HELIX_PHASE_HEATING_BED", "HELIX_PHASE_HEATING_NOZZLE",
    "HELIX_PHASE_BED_MESH", "HELIX_PHASE_QGL", "HELIX_PHASE_Z_TILT",
    "HELIX_PHASE_CLEANING", "HELIX_PHASE_PURGING"};

inline constexpr std::string_view kAliasDescriptionPrefix = "HelixScreen alias for ";

[[nodiscard]] std::string hidden_name(std::string_view visible);

/// One [gcode_macro <visible>] forwarder per name, in kVisibleNames order whatever
/// the input order; unknown names are dropped; empty input gives "".
[[nodiscard]] std::string alias_block(const std::vector<std::string>& visible_names);

} // namespace helix::signal_macros
```

`MacroManager::macro_content_for(const PrinterDiscovery&)` keeps its signature: the base content,
plus `alias_block(all eleven)` when `has_helix_macro("HELIX_READY")`. `with_visible_signal_names`
is removed from `include/macro_manager.h` and its test case is replaced.

**Tests** (`tests/unit/test_signal_macro_names.cpp`, `[signal_macros]`; macro cases stay in
`tests/unit/test_helix_macro_manager.cpp`, `[config][content]`):

| Case | Expect |
|---|---|
| `kVisibleNames` against the bundled pack | every `[gcode_macro _HELIX_*]` in `assets/config/helix_macros.cfg` except `_HELIX_STATE` is `_` + a listed name, and every listed name has one; adding a signal without listing it goes red |
| `alias_block({"HELIX_READY"})` | exactly the four-line section of D1, trailing newline |
| `alias_block` with names out of order and one unknown | sections in `kVisibleNames` order; the unknown name absent |
| `alias_block({})` | empty string |
| `macro_content_for`, printer with `HELIX_READY` | contains every `[gcode_macro _HELIX_*]` of the base pack unchanged, and one `[gcode_macro HELIX_X]` forwarder per name whose body is `_HELIX_X`; `MACRO=_HELIX_STATE` unchanged; no visible name appears anywhere except its own section header and description |
| `macro_content_for`, fresh printer and hidden-only printer | byte-equal to `get_macro_content()` (no block) |
| The content parses | every section header in the generated content is unique (a duplicate would merge in Klipper) |
| Rung unchanged | a printer with the alias block loaded (`HELIX_READY`, `_HELIX_READY`, `HELIX_UNLOAD_FILAMENT`) reads 2.1.0 and INSTALLED |
| `update_files` / `install_files` (existing `MacroStageFixture` cases "keeps the visible names a printer has", "keeps the signal names the printer has") | rewritten: the uploaded file has the hidden sections and the alias block |
| `test_matches_the_old_writers_own_disable_output` (`moonraker-plugin/tests/test_strip_phase_tracking.py`) | its fixture builds the visible pack itself with a regex; its comment naming `with_visible_signal_names` is reworded, behaviour unchanged |

Break check: drop the alias block from `macro_content_for` and the HELIX_READY case goes red.

### Phase 2: the scan decides which aliases a printer keeps

**Interfaces** (same header):

```cpp
namespace helix::signal_macros {

struct SignalCaller {
    std::string section; ///< configfile.settings key, lowercased by Klipper ("gcode_macro print_start")
    std::string option;  ///< "gcode", "press_gcode", ...
    std::string name;    ///< the visible name it calls, uppercased
    bool operator==(const SignalCaller& o) const; // written out: firmware builds C++17
};

struct SignalMacroScan {
    std::vector<SignalCaller> callers; ///< config outside the pack and the aliases calling a visible name
    std::vector<std::string> aliases;  ///< visible names defined by our forwarder (kAliasDescriptionPrefix)
    std::vector<std::string> foreign;  ///< visible names defined by a section that is neither ours nor an alias
    bool hidden_loaded = false;        ///< every hidden name is defined
};

/// @param pack_sections the section names the bundled pack defines, both spellings, lowercased
[[nodiscard]] SignalMacroScan scan(const nlohmann::json& config_settings,
                                   const std::vector<std::string>& pack_sections);

/// Visible names to alias: each name a caller uses, minus `foreign` (the user's own
/// macro of that name wins, and a forwarder would merge into it). Sorted in kVisibleNames order.
[[nodiscard]] std::vector<std::string> names_to_alias(const SignalMacroScan& scan,
                                                       const std::vector<std::string>& extra_names);

enum class SignalNameDrift {
    None,         ///< what is loaded is what an update would stage
    MoveToHidden, ///< the old visible pack is loaded: no hidden names
    AddAliases,   ///< a caller uses a name with no definition loaded
    DropAliases,  ///< an alias no caller uses
};
[[nodiscard]] SignalNameDrift drift(const SignalMacroScan& scan,
                                    const std::vector<std::string>& extra_names);

} // namespace helix::signal_macros
```

`extra_names` are the names Phase 4 records from sliced files; Phase 2 passes `{}`.

`PrinterDiscovery` gains `set_signal_macro_scan(signal_macros::SignalMacroScan)`,
`const std::optional<signal_macros::SignalMacroScan>& signal_macro_scan() const` (nullopt until
`configfile.settings` was read), and its `clear()` resets it. The discovery sequence fills it in
the same locked block as `set_skip_wrappers`. `MacroManager` gains
`[[nodiscard]] static std::vector<std::string> pack_section_names()` (all section names of the
bundled pack, both spellings, lowercased), which feeds `scan`.

`macro_content_for` then appends `alias_block(names_to_alias(*scan, extra))`; with no scan yet it
keeps Phase 1's rule (all eleven when `HELIX_READY` is defined). `evaluate_status` returns
OUTDATED when the scan is present and `drift(...) != None`, after the existing version and skip
checks; with no scan it is unchanged.

**Tests** (`[signal_macros]`, scan inputs written as `configfile.settings` JSON):

| Case | Expect |
|---|---|
| `PRINT_START` gcode calls `HELIX_READY` | one caller `{gcode_macro print_start, gcode, HELIX_READY}` |
| Lowercase `helix_ready` in a body | caller `HELIX_READY` |
| Body calls `_HELIX_READY`, `HELIX_READYX`, `MY_HELIX_READY` | no caller |
| `[gcode_button x] press_gcode: HELIX_PHASE_HOMING`, `[delayed_gcode]`, `[idle_timeout] gcode` | one caller each, right option name |
| `printer["gcode_macro HELIX_READY"]` in a Jinja expression | a caller (over-match is deliberate) |
| The pack's own sections, visible and hidden (`HELIX_START_PRINT` calls `_HELIX_PHASE_*`; an old visible pack's `HELIX_START_PRINT` calls `HELIX_PHASE_*`) | no caller |
| Our alias sections | listed in `aliases`, not callers, not `foreign` |
| User's own `[gcode_macro HELIX_READY]` with another description | in `foreign`; `names_to_alias` never returns it even when called |
| `hidden_loaded` | true only with all eleven `gcode_macro _helix_*` keys |
| Non-object settings, non-string options, empty object | empty scan, no crash |
| `drift`: old visible pack, no hidden | MoveToHidden |
| `drift`: hidden only, PRINT_START calls `HELIX_READY` | AddAliases |
| `drift`: aliases for `HELIX_READY`, no callers | DropAliases |
| `drift`: aliases match callers exactly | None |
| `macro_content_for` with a scan of one caller | alias block has exactly that name |
| `macro_content_for` with an empty scan on a printer that had `HELIX_READY` | no alias block |
| `evaluate_status` with AddAliases on an otherwise current pack | OUTDATED; with no scan, INSTALLED |
| Discovery wiring (`MoonrakerClientMock` with `configfile.settings` calling `HELIX_READY`) | `signal_macro_scan()` holds the caller after discovery |

Break checks: make the boundary test accept a leading `_` and the `_HELIX_READY` case goes red;
drop the pack-section exclusion and the old-visible-pack case goes red.

### Phase 3: the plugin renames its own legacy marker blocks

**Interfaces.** `moonraker-plugin/strip_phase_tracking.py`:

```python
_MACRO_CALL_RE = re.compile(rb"^[ \t]*_?(HELIX_PHASE_[A-Z_]+|HELIX_READY)[ \t]*$")

def compute_renamed_content(raw_bytes: bytes) -> StripResult
    # status "clean" | "anomaly" | "renamed"; blocks = visible calls renamed;
    # .names = the hidden names the renamed lines now call

def process_file(path, discovered_path=None, mode="strip", dry_run=False) -> dict
    # mode "strip" | "rename"; dry_run returns status "would_edit" and writes nothing

class TreeReport:
    edited: list; would_edit: list; skipped: list; failed: list
    def names_needed(self) -> set        # hidden names the planned edits call
    def exit_status(self) -> int         # EXIT_OK / EXIT_NEEDS_ATTENTION / EXIT_FAILED as main() today
    def summary_line(self, verb: str) -> str   # the one dry-run log line of D4

def run_tree(config_dir: str, mode: str = "strip", dry_run: bool = False) -> TreeReport

# main(): adds --rename-hidden and --dry-run; with neither, output and exit status are unchanged
```

New `moonraker-plugin/helix_signal_migration.py`, loaded with `_load_sibling` after
`strip_phase_tracking`, started with `_start_isolated("signal_macro_migration", True, ...)`:

```python
SIGNAL_MIGRATION_FEATURE = "signal_macro_migration"
SIGNAL_MIGRATION_FEATURE_VERSION = 1
CONFIG_OPTION = "migrate_signal_macros"     # [helix_print], default True
DB_KEY = "signal_migration_last"

def missing_hidden_names(settings: Dict[str, Any], needed: Iterable[str]) -> List[str]  # pure

class HelixSignalMigration:
    def __init__(self, helix: "HelixPrint", enabled: bool) -> None
    def start(self) -> None                 # klippy_ready handler, GET /server/helix/signal_migration, feature
    async def on_klippy_ready(self, *args: Any) -> None
    async def run_once(self) -> Dict[str, Any]   # the report; serialized by one asyncio.Lock
    def report(self) -> Dict[str, Any]
```

`run_once`: `run_tree(config_dir, "rename", dry_run=True)` off the event loop; log the summary
line; nothing to do, done; switched off, state `disabled`; read `configfile.settings` with
`klippy_apis.query_objects` and `missing_hidden_names`; any missing, state `waiting_for_pack`
with the names; else `run_tree(..., "rename")`, persist the report and send
`notify_features_changed`. Report shape:
`{"state": "nothing"|"disabled"|"waiting_for_pack"|"done"|"needs_attention"|"failed",
"missing": [...], "edited": [{"path", "backup", "blocks"}], "skipped": [{"path", "reason"}],
"failed": [{"path", "reason"}], "at": <epoch>}`. The config dir is
`file_manager.get_directory("config")`, as `helix_config.py` and `helix_plr.py` read it. Python
3.8 compatible, the plugin's floor (`docs/devel/HELIXPRINT_PLUGIN.md`), and follows that doc's add-a-feature checklist. Bump `PLUGIN_VERSION`.

**Tests** (`moonraker-plugin/tests/test_strip_phase_tracking.py`,
`moonraker-plugin/tests/test_helix_signal_migration.py`):

| Case | Expect |
|---|---|
| Rename on the pre-1.1 writer's own output (`fixtures/pre_1_1_instrumentation.py`) | every call line gains `_`, markers and every other byte unchanged |
| Rename keeps CRLF and tab indentation | byte-exact |
| Rename, second run on its own output | `clean`, no backup written |
| Rename on each anomaly fixture the strip tests already use | `anomaly`, file untouched |
| Rename outside a gcode body, or a block of four lines | `anomaly` |
| Strip on a renamed block | stripped (uninstall after migration still cleans up) |
| `--dry-run` | prints the plan, writes nothing, no backup, exit 0 |
| `main()` with no new flags | output and exit status identical to today's cases |
| `missing_hidden_names` | lowercased Klipper keys match uppercased names; missing ones listed |
| `run_once`, hidden names not loaded | no file changed, state `waiting_for_pack`, `missing` lists them |
| `run_once`, switched off | no file changed, state `disabled`, the log line says `would rename` |
| `run_once`, all loaded | file renamed, backup exists and equals the original, state `done`, report persisted |
| `run_once` twice | second is `nothing`, no second backup |
| Two `on_klippy_ready` at once | one edit, one backup |
| Concurrent write between plan and replace | file skipped, state `needs_attention`, next run edits it |
| Feature entry | `signal_macro_migration` in status `features`, `acts` true, enabled follows the option |
| `strip_phase_tracking` missing | feature unavailable with `module_missing`, the rest of the plugin starts |

Break check: remove the `missing_hidden_names` gate and the `waiting_for_pack` case goes red.

### Phase 4: tell the user, and catch sliced files

**Interfaces.**

```cpp
namespace helix::signal_macros {
/// The visible signal name in a Klipper `Unknown command:"HELIX_X"` response, or nullopt.
[[nodiscard]] std::optional<std::string> unknown_signal_command(std::string_view response);

enum class SignalNamesLine { None, AllHidden, AliasesKept, PluginWillRename, UpdatePluginToRename };
/// What the helper-macros row says about the names; the row maps it to text.
[[nodiscard]] SignalNamesLine signal_names_line(const SignalMacroScan& scan, bool plugin_reports_migration,
                                                bool marker_callers_present);
}
```

`PrintStartCollector` (or the gcode response path it already reads) passes each response through
`unknown_signal_command`; a hit appends the name to the setting `/helix_macros/sliced_signal_names`
(a string list, read as `extra_names` by Phase 2) and raises one notice per name per app run. The
plugin features table gains `{"signal_macro_migration", "Signal macro renaming", ...,
PluginFeatureHome::None}`. The helper-macros row shows `signal_names_line` and, for
`UpdatePluginToRename` on a remote Moonraker, `get_remote_install_command()`. Where that line sits
is a layout call with `ctl geom` at 800x480 and the smallest breakpoint before it is drawn.

**Tests** (`[signal_macros]`, plus the collector's existing tag):

| Case | Expect |
|---|---|
| `Unknown command:"HELIX_READY"`, `!! Unknown command:"helix_phase_qgl"` | the uppercased name |
| `Unknown command:"_HELIX_READY"`, `Unknown command:"FOO"` | nullopt |
| Collector sees the response during a print | the setting gains the name once, one notice |
| `drift` with an extra name not in the scan | AddAliases |
| `signal_names_line` for each state | the matching enum |
| `kKnownPluginFeatures` | contains `signal_macro_migration` |

## Failure modes

| Failure | Effect | Detected by |
|---|---|---|
| A config call the scan missed | `Unknown command`, no ready signal, the print continues | Phase 4 response match; the next discovery's AddAliases sets OUTDATED |
| A sliced file calls a visible name | same, for that file | Phase 4 response match and notice; release note |
| User wrote their own `HELIX_READY` | aliasing it would merge into theirs | `foreign` in the scan; never aliased; covered by a test |
| The plugin renames before Klipper has the hidden names | `Unknown command` from `PRINT_START` after the next restart | cannot happen past the `missing_hidden_names` gate; test |
| Rename hits an odd block | config damaged | anomaly rules leave the file untouched; backup verified before replace; restore on a failed verify |
| Fluidd writes the file mid-migration | lost user edit | concurrent-change check skips the file; next `klippy_ready` retries |
| Every plugin load rewrites | backup pile-up | idempotence test; second run writes nothing |
| Uninstall after migration | renamed blocks left behind calling deleted macros | strip accepts `_HELIX_*` call lines; test |
| `configfile` query fails at discovery | no scan | `signal_macro_scan()` nullopt: Phase 1 rule, status unchanged |
| Remote Moonraker with an old plugin | marker blocks keep their visible calls, so aliases stay | Phase 4 line with the remote install command |
| Aliases outlive their callers | names stay visible | DropAliases sets OUTDATED; the next update drops them |
| Restart between Phase 2's alias drop and a new hand-added call | one print with `Unknown command` | Phase 4 response match |

## Release note draft (1.2, `Upgrading from 1.1?`)

- **HelixScreen's print signals leave your macro list.** When you update the helper macros from
  Settings, the signal macros move to their underscore names (`_HELIX_READY`, `_HELIX_ENDED`,
  `_HELIX_RESET` and `_HELIX_PHASE_*`), and Fluidd and Mainsail stop listing them. If your own
  macros still call a name without the underscore, HelixScreen keeps that one name working and
  leaves it in the list; rename the call and update the macros again to hide it. The HelixPrint
  plugin renames the phase-tracking lines an earlier HelixScreen added to your `PRINT_START` by
  itself, after backing up each file it changes (`printer.cfg.bak.<date>`), and you can turn that
  off with `migrate_signal_macros: False` under `[helix_print]` in `moonraker.conf`. HelixScreen
  cannot look inside sliced files: if your slicer's start G-code calls `HELIX_READY` directly,
  change it to `_HELIX_READY` and re-slice. A file that still calls the old name prints normally
  but shows `Unknown command`, and HelixScreen tells you which name it called and keeps that name
  working from the next macro update.

## Open questions for Preston

1. **Rename or strip the legacy marker blocks?** This plan renames them (D3), which keeps the
   phase and ready signals those printers have today. The issue proposed stripping, which loses
   them and falls back to the collector's heuristics.
2. **Should the on-load rename be on by default?** The plan has it on, with a `moonraker.conf`
   opt-out, because it only touches lines HelixScreen itself wrote. The plugin's rule for actors
   is off until the user opts in on the screen.
3. **Alias only the names a printer calls, or all eleven once it calls any?** The plan aliases
   only the called ones, so most printers hide ten of the eleven at once. All eleven would cover
   a sliced file that calls a phase name the config never does.
4. **Should DropAliases offer an update?** It sets OUTDATED, so the macros row offers an update
   whose only effect is hiding names. The alternative is to drop aliases only during an update made
   for another reason.
5. **Firmware-managed packs.** On printers whose firmware ships or pins our files (Snapmaker U1
   under paxx), should the plugin rename at all, or report only?
