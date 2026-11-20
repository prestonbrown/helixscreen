# In-app firmware update: design

When HelixScreen replaces a printer's stock screen, the stock firmware-update menu goes with
it (stock Snapmaker U1: Settings > About > Firmware Version > Local Update). Users then have
no way to update the printer short of SSH (prestonbrown/helixscreen#2131). This plan adds a
firmware-update page backed by a vendor abstraction: one provider table that knows each
firmware's update mechanism, and generic UI that only asks it capability questions.

Research behind the rows (scripts read, docs, third-party mirrors): the per-platform notes
attached to this work. Nothing in this plan has been run on a printer.

## Two sources of updates

| Source | Covers | How |
|---|---|---|
| Moonraker `update_manager` | Klipper, Moonraker, web clients, mods that register an entry (ZMOD, Forge-X, Simple AF, k2-improvements, Kalico...) | `machine.update.status` / `machine.update.upgrade {name}` / `machine.update.refresh`, progress from `notify_update_response` |
| Vendor firmware row | The printer's own OS and MCU images | `helix::fwupdate` provider table: detection predicate, image rule, apply plan, progress parser |

The page shows whichever of the two this printer has. A printer with neither never shows the
Settings entry.

**HelixScreen's own entry** (`[update_manager helixscreen]`) is left out of the list: Settings >
Updates owns it, `UpdateChecker` listens to `notify_update_response` for it and restarts the
app when it completes. Listing it here would give one update two buttons.

## Capability interface

`include/firmware_update.h`, `src/system/firmware_update.cpp`, `namespace helix::fwupdate`.
The vendor-abstraction rule applies: these files are the only place a firmware's update
mechanism is named.

```cpp
enum class Mechanism { None, VendorScript, UsbAtBoot, SelfUpdating, ClosedTool, OwnerTool };
enum class Readiness { Supported, NeedsDeviceVerification, NeedsVendorCooperation,
                       NotPossible, NoActionNeeded };

struct HostFacts { board::Board board; std::string host_mod; std::string printer_type; };
HostFacts current_host_facts();               // board::current(), install_provenance, profile
const Provider* provider_for(const HostFacts&); // nullptr: no vendor row

bool unverified_apply_allowed();               // HELIX_FWUPDATE_UNVERIFIED_APPLY=1
bool apply_enabled(const Provider&, bool unverified_allowed);

ImageVerdict check_image(const Provider&, const FileEntry&);
std::vector<FileEntry> find_images(const Provider&, const std::vector<std::string>& roots);

Blocker first_blocker(const ApplyInputs&);     // not printing / paused / unknown, no update
                                               // running, enough free space
std::vector<ConfirmPoint> confirm_points(const Provider&);  // what the confirm dialog says
ApplyPlan apply_plan(const Provider&, const std::string& image, const std::string& log);
ProgressEvent parse_progress(const Provider&, std::string_view line);
```

Moonraker side, `include/update_manager_status.h` (same namespace, no vendor knowledge):

```cpp
std::optional<UpdateStatus> parse_update_status(const nlohmann::json& result);
std::optional<UpdateResponse> parse_update_response(const nlohmann::json& notification);
std::optional<int> percent_in_message(std::string_view message);
```

`UpdateChecker`'s own `notify_update_response` handler uses `parse_update_response` too, so
the notification is parsed in one place.

Vendor apply, `include/firmware_update_runner.h`: `start_vendor_apply(plan, runner)` launches
the plan through a `ProcessRunner` (tests pass a fake; nothing in tests or the mock ever runs a
vendor script), and `LogTail` feeds the plan's log back through `parse_progress`.

## Provider rows

| Row (predicate) | Mechanism | Readiness | Image | Apply | Notes |
|---|---|---|---|---|---|
| Snapmaker U1 stock (`SnapmakerU1`, host mod not `paxx`) | VendorScript | **Needs device verification**; apply behind `HELIX_FWUPDATE_UNVERIFIED_APPLY=1`, default off | `*.bin` in a USB root, at least 50 MB | `/home/lava/bin/systemUpgrade.sh upgrade all <bin>`, detached (`setsid`, output to a log HelixScreen tails) | Script read from a third-party backup (J0eB0l/U1_Backup); the copy on current firmware may differ. Kills Klipper, flashes 5 MCUs, writes the other A/B slot, reboots. Deletes `/oem/.debug` before it unpacks, so the standalone HelixScreen install is gone even when the update then fails. Lock `/tmp/.system_upgrade.lock` |
| Snapmaker U1 PAXX (`SnapmakerU1`, host mod `paxx`) | OwnerTool | Needs vendor cooperation | - | none | paxx's firmware-config API (`127.0.0.1:9091`) owns updates; HelixScreen needs an explicit, agreed contract first (PR #590 thread: "explicit, not implicit") |
| Creality K1 family (`K1`) | VendorScript | Needs device verification; no apply path this phase | `*.img` | (later) `/etc/ota_bin/local_ota_update.sh <img>` with `OTA_CB_APP` progress callback | Stock `/etc` restored afterwards, HelixScreen likely needs reinstalling (unverified). Simple AF needs pellcorp's view |
| Creality K2 family (`K2`) | VendorScript | Needs device verification; no apply path this phase | `*.img` | (later) same script, which calls `swupdate_cmd.sh` | A/B + recovery; swupdate progress socket and signing unverified; `hooks-k2.sh` stops `upgrade-server` |
| FlashForge AD5M / AD5X / Creator 5 (`Ad5m`, `Ad5x`, `Creator5`, `Creator5Pro`) | UsbAtBoot | Needs device verification; no apply path this phase | model-prefixed `*.tgz` (`Adventurer5M-`, `Adventurer5MPro-`, `AD5X-`, `Creator5-`, `Creator5Pro-`) | (later) "reboot with the stick in" | Which process scans the stick at boot is unconfirmed; a stock update disables ZMOD on AD5X/Creator 5 |
| QIDI Q2 / Max 4 / Plus 5 (printer type starts with `QIDI`) | ClosedTool | Not possible | - | none | The apply lives inside the closed `qidiclient`. Valve Oil is a third-party updater worth asking. Separately: a stock QIDI update deletes `printer_data/config/helixscreen` (candidate bug, unverified) |
| Elegoo CC1 / COSMOS (`Cc1`) | SelfUpdating | No action needed | - | none | Updates arrive as Klipper action prompts HelixScreen already renders |
| Everything else | - | - | - | - | No row: `provider_for()` returns nullptr |

Only rows with an apply plan and `Supported` readiness (none yet), or `NeedsDeviceVerification`
plus the runtime flag (U1 stock only), can reach `start_vendor_apply`.

## Safety rules

1. **No flash without an explicit confirm.** The confirm dialog lists the row's
   `confirm_points()`: keep the printer powered, the printer reboots, Klipper stops, and on U1
   stock that HelixScreen is removed and must be reinstalled.
2. **Never while a job holds the machine.** `first_blocker()` refuses when the app's lifecycle
   says Preparing / Printing / Paused, and the page re-queries `print_stats.state` from
   Moonraker right before sending: only `standby`, `complete`, `cancelled` and `error` pass;
   anything else, including no answer, blocks.
3. **One update at a time.** `update_manager` busy, the vendor lock file, or our own job in
   flight blocks a second start.
4. **Space.** Vendor rows check free space where the vendor unpacks (`/tmp` on U1) against
   the image size; unknown free space blocks.
5. **Unverified rows stay off.** `apply_enabled()` is false for every row without an apply plan,
   and for `NeedsDeviceVerification` rows unless `HELIX_FWUPDATE_UNVERIFIED_APPLY=1`.
6. **Tested code paths only.** Every gate above is a pure function with unit tests; the runner
   takes a `ProcessRunner`, so tests and the mock never run a vendor script.
7. **Detached apply.** The vendor script must outlive HelixScreen (it may stop the service that
   runs us), so it runs under `setsid` with stdin from `/dev/null` and stdout/stderr to a file;
   a pipe back to us would SIGPIPE the script mid-flash if we died.

## UI flow

- Settings > PRINTER > **Firmware Update** row, visible when `printer_has_update_manager` or
  `firmware_update_vendor_available` is 1.
- The overlay (`settings_firmware_update_overlay.xml`):
  - **Printer firmware** card (vendor row with an enabled apply): the image files found on USB,
    each with Install; a status line otherwise ("insert a USB stick with the firmware file").
  - **Software** card (update_manager): one row per entry, name + "installed → available" or
    "Up to date", an Update button where an update exists, and Refresh in the header.
  - **Progress view**: what is updating, a bar when the source reports a percent, the last
    message, "Do not power off the printer", and Done once it completes or fails.
- Install / Update: `first_blocker()` (toast on refusal) → `modal_confirm` with the confirm
  points → fresh `print_stats` query → start.

## Mock

`--test`: the mock Moonraker lists `klipper` (git, behind), `moonraker` (up to date), `mainsail`
(web, update available), `fluidd` (up to date). `machine.update.upgrade` sets busy, streams
`notify_update_response` lines (with a git-style `Receiving objects: NN%`) from the simulation
loop, then completes and reports the entry as up to date.

## Phases

1. **This change:** core table and decisions, update_manager page, U1 stock row behind the
   flag, status-only rows, mock, translations.
2. U1 stock verified on a printer the team holds: run the flagged path, confirm the log
   lines, the user HelixScreen runs as, and what survives; then Supported.
3. K1 / K2 apply plans (`OTA_CB_APP`, swupdate progress), each verified on hardware.
4. FlashForge "reboot with the stick in" guidance; USB-insert prompt across rows.
5. Cooperation rows: PAXX contract, ZMOD `ZFLASH`, Valve Oil on QIDI.

## Open questions

- Does stock U1 HelixScreen run as root (needed by `systemUpgrade.sh`)? Does anything kill it
  when Klipper stops?
- Is `setsid` on the U1's PATH?
- Should Moonraker's `system` (apt) entry be offered at all on printers where it is enabled?
