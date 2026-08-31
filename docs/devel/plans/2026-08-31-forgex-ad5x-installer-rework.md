# Forge-X AD5X Installer Rework — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make HelixScreen's installer install safely onto a Forge-X-flavored AD5X — probing the mod instead of testing markers, refusing to destroy mod-owned paths, and adding an explicit `--mod-payload` mode where the mod owns the service and we replace payload contents in place.

**Architecture:** One new probe module (`host_profile.sh`) runs before path setup and exports capability answers; every audited site switches from marker tests to those answers. A `host_path_is_mod_owned` guard lands FIRST and gates every destructive path (update `mv`/`rm -rf`, Moonraker NetDeploy arming, uninstall loops). `--mod-payload` mode makes the dangerous shapes structurally impossible: no service install, no stanza in the mod's git-tracked `moonraker.conf`, no `mv`/`rm -rf` of the root, contents replaced in place with `config/` and `platform/` preserved.

**Tech Stack:** bash installer libs under `scripts/lib/installer/` bundled by `scripts/bundle-installer.sh` / `bundle-uninstaller.sh`; bats under `tests/shell/`; C++ (one file, `log_collector.cpp`).

**Spec:** Preston's 13-item audit (2026-08-31, pasted into this session) + `DrA1ex/ff5m#74` close-out + the hardware-verified facts below. The branch's own plan doc `docs/devel/plans/2026-08-27-forgex-142-headless-install.md` documents the display-mode takeover design this branch already carries.

## Global Constraints

- Never `git add -A` / `git add .` — stage explicit paths. Never `git stash`, `git clean`, `git rm`, `git reset` without asking. Uncommitted work in any tree is sacred.
- `scripts/install.sh` and `scripts/uninstall.sh` are GENERATED. Edit `scripts/lib/installer/*.sh`, then run `./scripts/bundle-installer.sh` and `./scripts/bundle-uninstaller.sh`; commit the regenerated bundles. Verify with the generators' byte-exact reproduction.
- Tests first: every task writes its bats/C++ test, watches it fail, then implements. A test that cannot fail is not a test.
- The rig at `192.168.30.254` is shared. Read-only ssh is fine (announce intent). NEVER run `--update`, `--clean`, or uninstall against it until Task 1's guard is merged AND its refusing test is green. A print may be running — coordinate with the peer session (`debug ad5x forge-x issues`) before any rig write; say so before restarting klippy or replacing files.
- Nothing lands in `ad5m-forgex` (the other repo) without telling the peer session first. This plan requires zero ad5m-forgex edits.
- Installer libs are bash (the rig host has bash — its own `.shell/helixscreen.sh` is `#!/bin/bash`). Keep them lint-clean at the repo's shellcheck gate severity.
- One reviewable commit per task. Subject + ~4-line body. No em-dashes.

## Hardware-Verified Facts (rig reads, 2026-08-31)

- Rig is FORGE-X flavored, not ZMOD: `/usr/data/.mod/.forge-x` exists, no `.zmod`, no `/ZMOD`, no `/srv/helixscreen`. A running `helix-screen` was started by the mod's `.shell/helixscreen.sh` (bootstrap step 9), not by any init script.
- Mod git tree: `/usr/data/config/mod` (in-chroot `/opt/config/mod`, also `/root/printer_data/config/mod`). Contains `.shell/platform.sh`, `.shell/helixscreen.sh`, `moonraker.conf`, `.bin/helixscreen` (our payload: `bin assets ui_xml config platform certs release_info.json`).
- `[update_manager forge-x]` in the mod's `moonraker.conf:59` is `type: git_repo`, `path: /root/printer_data/config/mod/`, `primary_branch: 1.4.2`. `.bin/helixscreen` is **untracked and NOT gitignored** in the mod repo → their OTA (`git clean -fd` in Moonraker's git_repo flow) removes it. **A payload under the mod tree does not survive a Forge-X OTA.**
- `user.moonraker.conf` lives at `/usr/data/config/mod_data/user.moonraker.conf` (included by the mod's conf, line ~93), currently holds only `[authorization]`. This is the sanctioned stanza target.
- Chroot binds: `/opt` (in-chroot) == `/usr/data` (host) — same `mmcblk0p7`, 4.7G free. `df /opt` == `df /usr/data`; the full 12.5M squashfs `/` is the wrong df target, confirming audit item 5.
- `curl`, `unzip`, `busybox` all present in the chroot MIPS rootfs.
- `/etc/init.d` does not exist on disk inside the chroot root (`/usr/data/.mod/.forge-x/etc/` has no `init.d`); the mod's `.root/S*` scripts are the service mechanism (DrA1ex ff5m#74: "support for custom service files inside the chroot environment under /etc/init.d"). Host-side SysV (audit item 9) never runs the UI.
- Main's `forgex.sh` (via `631694461`) and this branch's diverged: main has `dismiss_forgex_feather_promo` + hooks file, no record file; branch has `FORGEX_PREV_DISPLAY_F` record/restore. Neither has any `bash -n` containment (grep confirms zero hits on main).

## Open Decisions (do not block Tasks 0-2; block Task 5's default)

1. **Payload root durability.** `--mod-payload` takes its root from the mod (today `/opt/config/mod/.bin/helixscreen`), which their OTA cleans. Durable alternative: `/usr/data/helixscreen` (== in-chroot `/data/helixscreen`), outside the git repo, same partition. Moving it requires the mod's `.shell/helixscreen.sh` to follow — an ad5m-forgex change → peer conversation + upstream ask to DrA1ex. Until settled: mod-supplied root is the default, `--mod-payload-root <path>` overrides, and the installer prints the OTA-clean warning whenever the root is inside `$(host mod tree)`.
2. **user.moonraker.conf stanza shape.** Proposal (matches our release model, zip like their guppyscreen entry):
   ```ini
   [update_manager helixscreen]
   type: zip
   repo: prestonbrown/helixscreen
   path: /opt/config/mod/.bin/helixscreen
   ```
   Confirm against Moonraker's zip-updater payload layout expectations before Task 5 lands (a zip updater replaces `path` contents — same preservation contract as our in-place mode, but their engine does it).

---

### Task 0: Reconcile branch with main

**Files:**
- Merge: `main` → `feature/forgex-headless-install` (conflicts expected in `scripts/install.sh`, `scripts/lib/installer/forgex.sh`, `scripts/uninstall.sh`; main's side includes `631694461`, `assets/config/platform/hooks-ad5m-forgex.sh`, `main.sh` wiring)

**Interfaces:**
- Produces: a clean base where every later task diffs against current main. No new symbols.

- [ ] **Step 1:** In the worktree: `git merge main`. For each conflict, take main's version as the base and re-apply branch-only machinery function-by-function (record file `FORGEX_PREV_DISPLAY_F` + `configure_forgex_display`'s three-mode takeover + restore contract). Main's `dismiss_forgex_feather_promo` and evolved `FORGEX_DISPLAY_MODES` win where they overlap.
- [ ] **Step 2:** Run `./scripts/bundle-installer.sh && ./scripts/bundle-uninstaller.sh`; confirm `git status` shows no diff in the generated bundles (regenerated == committed).
- [ ] **Step 3:** Run `bats tests/shell/test_forgex_boot.bats tests/shell/test_forgex_display_modes.bats` — all green before anything new.
- [ ] **Step 4:** Commit the merge: `git commit -m "merge: main into feature/forgex-headless-install (take main's 1.4.2 base, keep the takeover record)"`.

### Task 1: host_profile.sh + the mod-owned-path guard (SAFETY — nothing else first)

**Files:**
- Create: `scripts/lib/installer/host_profile.sh`
- Modify: `scripts/lib/installer/common.sh` (bundle include list is in `scripts/bundle-installer.sh:88` module order — insert `host_profile.sh` after `common.sh`), `scripts/lib/installer/common.sh:333-344` (`validate_install_dir`), `scripts/lib/installer/release.sh:1560-1573,1697`, `scripts/lib/installer/moonraker.sh:104-108,185-190`, `scripts/lib/installer/uninstall.sh` (HELIX_INSTALL_DIRS consumers)
- Modify: `scripts/bundle-installer.sh` + `scripts/bundle-uninstaller.sh` (module lists)
- Test: `tests/shell/test_host_profile_guard.bats` (new)

**Interfaces:**
- Produces (all globals, set by `host_profile_probe`, called at the top of `main.sh` before `set_install_paths`):
  - `HOST_MOD_ROOT` — mod git tree (`/usr/data/config/mod`) or empty
  - `HOST_MOD_CHROOT` — `/usr/data/.mod/.forge-x` | `/usr/data/.mod/.zmod` or empty
  - `HOST_CHROOT_STATE` — `inside` | `outside:<path>` | `none`
  - `HOST_SERVICE_MECHANISM` — `systemd` | `sysv` | `mod-managed`
  - `HOST_INSTALL_ROOT`, `HOST_CONFIG_DIR`, `HOST_MOONRAKER_USER_CONF`, `HOST_PLATFORM_HOOK_KEY`
  - `HOST_OWNS_COMPETING_UIS` — `1` when the mod owns them (skip the sweep)
  - `host_path_is_mod_owned <path>` → 0/1 — symlinks resolved; true under `HOST_MOD_ROOT` or `/usr/data/.mod`
  - `HELIX_MOD_PAYLOAD` — `1` when `--mod-payload` was given (parsed in Task 5; the guard reads it now so Task 1 can land without the mode)

- [ ] **Step 1: Write the failing tests.** `tests/shell/test_host_profile_guard.bats`, reusing the sandbox helpers pattern from `test_forgex_display_modes.bats`:

```bash
setup() { SETUP_TEST_SANDBOX; load_lib host_profile; load_lib common; }

@test "host_path_is_mod_owned: true under the mod tree and the .mod chroot, false elsewhere" {
    mkdir -p "$SANDBOX/usr/data/config/mod/.shell" "$SANDBOX/usr/data/.mod/.forge-x"
    touch "$SANDBOX/usr/data/config/mod/.shell/platform.sh"
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    run host_path_is_mod_owned "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ]
    run host_path_is_mod_owned "$SANDBOX/usr/data/.mod/.forge-x/anything"
    [ "$status" -eq 0 ]
    run host_path_is_mod_owned "$SANDBOX/srv/helixscreen"
    [ "$status" -ne 0 ]
}

@test "validate_install_dir refuses a mod-owned INSTALL_DIR outside --mod-payload" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"; HELIX_MOD_PAYLOAD=""
    run validate_install_dir "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -ne 0 ]; [[ "$output" == *"refusing"* ]]
    HELIX_MOD_PAYLOAD=1
    run validate_install_dir "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ]
}

@test "release.sh update backup refuses to mv a mod-owned root" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    INSTALL_DIR="$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    run backup_install_dir_for_update   # extracted from release.sh:1697 block
    [ "$status" -ne 0 ]; [[ "$output" == *"refusing"* ]]
    [ -d "$INSTALL_DIR" ]   # untouched
}
```

- [ ] **Step 2:** Run `bats tests/shell/test_host_profile_guard.bats` — expect FAIL (no `host_profile.sh`, no guard).
- [ ] **Step 3: Implement.** `scripts/lib/installer/host_profile.sh`:

```bash
# Host capability profile. Probes ONCE (host_profile_probe, called from main.sh
# before set_install_paths) and exports answers; downstream code asks these
# instead of testing vendor markers. The mod's own .shell/platform.sh is the
# source of truth for its paths - reading it survives their refactors.

HOST_MOD_ROOT=""; HOST_MOD_CHROOT=""; HOST_CHROOT_STATE="none"
HOST_SERVICE_MECHANISM="systemd"; HOST_INSTALL_ROOT=""
HOST_CONFIG_DIR=""; HOST_MOONRAKER_USER_CONF=""
HOST_PLATFORM_HOOK_KEY=""; HOST_OWNS_COMPETING_UIS=0

host_profile_probe() {
    local cand
    for cand in /usr/data/config/mod /opt/config/mod; do
        if [ -f "$cand/.shell/platform.sh" ]; then HOST_MOD_ROOT="$cand"; break; fi
    done
    for cand in /usr/data/.mod/.forge-x /usr/data/.mod/.zmod; do
        if [ -d "$cand/usr/bin" ]; then HOST_MOD_CHROOT="$cand"; break; fi
    done
    if [ -n "$HOST_MOD_CHROOT" ]; then
        if [ "$(stat -c %d:%i / 2>/dev/null)" = "$(stat -c %d:%i "$HOST_MOD_CHROOT" 2>/dev/null)" ]; then
            HOST_CHROOT_STATE="inside"
        else
            HOST_CHROOT_STATE="outside:$HOST_MOD_CHROOT"
        fi
    fi
    if [ -n "$HOST_MOD_ROOT" ]; then
        HOST_SERVICE_MECHANISM="mod-managed"
        HOST_OWNS_COMPETING_UIS=1
        HOST_INSTALL_ROOT="$HOST_MOD_ROOT/.bin/helixscreen"
        HOST_CONFIG_DIR="/usr/data/config/mod_data/helixscreen/config"
        HOST_MOONRAKER_USER_CONF="/usr/data/config/mod_data/user.moonraker.conf"
        HOST_PLATFORM_HOOK_KEY="ad5x-forgex"
    fi
}

# True when path (symlinks resolved) is managed by the mod: its git tree or the
# mod chroot. Never mv/rm/chmod these outside --mod-payload's in-place contract.
host_path_is_mod_owned() {
    [ -n "$1" ] || return 1
    [ -n "$HOST_MOD_ROOT" ] || return 1
    local p
    p=$(readlink -f "$1" 2>/dev/null) || p="$1"
    case "$p" in
        "$HOST_MOD_ROOT"|"$HOST_MOD_ROOT"/*) return 0 ;;
        /usr/data/.mod|/usr/data/.mod/*)     return 0 ;;
    esac
    return 1
}

host_refuse_mod_owned() {   # $1=what, $2=path  — call before any destructive step
    if [ "$HELIX_MOD_PAYLOAD" != "1" ] && host_path_is_mod_owned "$2"; then
        log_error "refusing ${1} on mod-owned path: $2 (use --mod-payload for in-place payload updates)"
        exit 1
    fi
}
```

Add to `validate_install_dir` (common.sh, after the existing checks): `host_refuse_mod_owned "install into" "$INSTALL_DIR"`. In `release.sh`, wrap the `mv "$INSTALL_DIR" "$INSTALL_DIR.old"` (:1697) and every `rm -rf` in the read-only-parent loop (:1560-1573) with `host_refuse_mod_owned "update backup" "$INSTALL_DIR"`. In `moonraker.sh`, guard the NetDeploy-arming stanza writes (:104-108, :185-190) the same way. Add `host_profile.sh` to both bundle module lists (after `common.sh`), and call `host_profile_probe` first in `main.sh`'s install entry.
- [ ] **Step 4:** Re-run the bats file — all PASS. Regenerate bundles, confirm clean diff.
- [ ] **Step 5:** Commit: `git commit -m "fix(installer): probe the host once and refuse mod-owned paths everywhere destructive"` — stage `scripts/lib/installer/host_profile.sh scripts/lib/installer/common.sh scripts/lib/installer/release.sh scripts/lib/installer/moonraker.sh scripts/lib/installer/main.sh scripts/bundle-installer.sh scripts/bundle-uninstaller.sh scripts/install.sh scripts/uninstall.sh tests/shell/test_host_profile_guard.bats`.

### Task 2: Detection — one mod-flavor detector, called for ad5x too

**Files:**
- Modify: `scripts/lib/installer/platform.sh:187-192` (detect_platform ad5x clause), `:412-413` (`ad5x_check_chroot_context` → `mod_check_chroot_context`), `:513-538` (`detect_ad5m_firmware` → `detect_mod_flavor`, teach `forge_x` for ad5x)
- Modify: `scripts/lib/installer/main.sh:323-329` (call it for ad5x, not just ad5m)
- Test: `tests/shell/test_platform_detect.bats` (extend; it exists for the current clauses)

**Interfaces:**
- Produces: `MOD_FLAVOR` (`forge_x` | `zmod` | `stock` | empty), set for BOTH ad5m and ad5x; `AD5M_FIRMWARE` kept as a compat alias. `detect_platform` returns `ad5x` on a Forge-X host (marker-optional: `HOST_MOD_ROOT` non-empty qualifies).

- [ ] **Step 1: Failing tests** — ad5x detection with a sandbox containing `/usr/data` + `.mod/.forge-x/usr/bin` + `HOST_MOD_ROOT` set (no `/ZMOD`, no `/usr/prog`): `detect_platform` → `ad5x`, `detect_mod_flavor` → `forge_x`, `mod_check_chroot_context` → `0`. And: `MOD_FLAVOR=forge_x` + `HOST_OWNS_COMPETING_UIS=1` short-circuits `stop_competing_uis` (audit item 2's second half).
- [ ] **Step 2:** Run — FAIL.
- [ ] **Step 3: Implement.** Rename with a one-line wrapper (`detect_ad5m_firmware() { detect_mod_flavor "$@"; }` keeps old callers). `forge_x` branch keyed on `[ -n "$HOST_MOD_ROOT" ]` or the chroot dir, before the `/ZMOD` test. In `competing_uis.sh:381-390`, replace the `zmod`-only early return with `[ "$HOST_OWNS_COMPETING_UIS" = "1" ] && return 0` (the mod owns its UI lifecycle; the takeover lives in `configure_forgex_display`).
- [ ] **Step 4:** Bats green; regenerate bundles.
- [ ] **Step 5:** Commit `fix(installer): one mod-flavor detector for ad5m and ad5x, and the mod owns its competing UIs`.

### Task 3: Paths and requirements

**Files:**
- Modify: `scripts/lib/installer/platform.sh:824-833` (install root), `scripts/lib/installer/requirements.sh:156-182` (`check_disk_space`), `:238-288` (reuse the real-write probe), `:408-499` (`verify_binary_deps` chroot-aware), `scripts/lib/installer/moonraker.sh:145-166` (`find_moonraker_conf`)
- Test: `tests/shell/test_platform_detect.bats` + `tests/shell/test_moonraker_conf.bats` (extend/create)

**Interfaces:**
- Consumes: Task 1's `HOST_INSTALL_ROOT`, `HOST_MOONRAKER_USER_CONF`, `HOST_CHROOT_STATE`.
- Produces: `INSTALL_DIR` = `HOST_INSTALL_ROOT` on mod hosts; `check_disk_space` df's the data mount (`/usr/data`), falling back to the real-write probe when `dirname` walks to `/`; `find_moonraker_conf` NEVER returns a `host_path_is_mod_owned` target — on mod hosts it returns `HOST_MOONRAKER_USER_CONF`.

- [ ] **Step 1: Failing tests** — (a) mod host: `set_install_paths` → `INSTALL_DIR=<mod>/.bin/helixscreen`; (b) `check_disk_space` with `dirname $INSTALL_DIR` = nonexistent `/srv` and a fake full `/` still passes when `/usr/data` (sandbox tmpfs) has space; (c) symlink `moonraker.conf -> mod/moonraker.conf`: `find_moonraker_conf` returns the user conf, never the symlink.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Implement** per Interfaces. `verify_binary_deps`: when `HOST_CHROOT_STATE=outside:*`, resolve the binary's loader via the chroot (`chroot "$HOST_MOD_CHROOT" ldd <in-chroot-path>` style, or compare `ldd` interpreter prefix against the chroot's) and only WARN with the fix hint when it cannot run host-side — that is the expected state on Forge-X, not an error (audit item 8).
- [ ] **Step 4:** Green; regenerate bundles.
- [ ] **Step 5:** Commit `fix(installer): mod-aware install root, disk probe, and Moonraker conf target`.

### Task 4: Service + hooks + env preservation

**Files:**
- Create: `assets/config/platform/hooks-ad5x-forgex.sh` (source: the rig payload's `platform/` hooks file — scp it out read-only during execution; if the rig file and `hooks-ad5m-forgex.sh` differ only in paths, derive and say so)
- Modify: `scripts/lib/installer/main.sh:93-101` (`install_platform_hooks` keys on `HOST_PLATFORM_HOOK_KEY`), `scripts/lib/installer/service.sh:342-372` (skip SysV install when `HOST_SERVICE_MECHANISM=mod-managed`), payload `config/helixscreen.env` handling (item 10)
- Test: `tests/shell/test_forgex_boot.bats` (extend), `tests/shell/test_service_install.bats` (extend if exists; else new minimal)

**Interfaces:**
- Produces: on mod hosts — no service files written anywhere, hooks file = `hooks-ad5x-forgex.sh`, existing `config/helixscreen.env` preserved (new payload's env lands beside it as `helixscreen.env.new`).

- [ ] **Step 1: Failing tests** — mod-host sandbox: `install_service` writes nothing under `/etc/init.d` or the chroot; `install_platform_hooks` picks the forgex key; an existing `helixscreen.env` with `HELIX_CONFIG_DIR=/opt/config/mod_data/helixscreen/config` survives an install pass byte-identical.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Implement.** `install_platform_hooks`: `case "$HOST_PLATFORM_HOOK_KEY" in ad5x-forgex) HOOKS=hooks-ad5x-forgex.sh ;; *) <existing per-platform logic> ;; esac`. `install_service`: `[ "$HOST_SERVICE_MECHANISM" = "mod-managed" ] && { log_info "mod manages the UI service (forge-x); skipping service install"; return 0; }`. Env: in the payload-extract path, `[ -f "$INSTALL_DIR/config/helixscreen.env" ] && mv new one to .new`.
- [ ] **Step 4:** Green; regenerate bundles.
- [ ] **Step 5:** Commit `fix(installer): forge-x hooks file, mod-managed service contract, env preservation`.

### Task 5: --mod-payload mode end to end

**Files:**
- Modify: `scripts/install-dev.sh` (arg), `scripts/lib/installer/main.sh` (mode block), `scripts/lib/installer/uninstall.sh` (`HELIX_INSTALL_DIRS` gains the payload path in mod mode; `uninstall_forgex` runs first), `scripts/lib/installer/common.sh:25`
- Test: `tests/shell/test_mod_payload_mode.bats` (new)

**Interfaces:**
- Consumes: all prior tasks. Mode contract: root = `--mod-payload-root` > `HOST_INSTALL_ROOT`; no service; no stanza in any `host_path_is_mod_owned` conf — stanza goes to `HOST_MOONRAKER_USER_CONF` only when `--mod-payload-updates` given; contents replaced in place (tar over the root), `config/` and `platform/` preserved; root never `mv`'d or `rm -rf`'d as a whole — uninstall removes only the payload subtree after `uninstall_forgex` restored the display mode.

- [ ] **Step 1: Failing tests** — full sandbox mod host: (a) `--mod-payload` install leaves `config/helixscreen.env` and `platform/hooks*` intact while replacing `bin/`; (b) the mod's `moonraker.conf` is byte-identical after install (git-clean parity); (c) a `helixscreen` stanza exists in `user.moonraker.conf` only with the updates flag; (d) uninstall removes exactly the payload subtree + stanza + display-mode restore, nothing else under the mod tree (compare `find` before/after); (e) running with `--mod-payload` against a root whose parent walk hits a full `/` still passes disk check.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Implement** the mode block in `main.sh` after detection, before requirements; `HELIX_MOD_PAYLOAD=1` flows from the arg. Print the OTA-clean warning (Open Decision 1) whenever `host_path_is_mod_owned "$INSTALL_DIR"`.
- [ ] **Step 4:** Green; regenerate bundles; run the WHOLE forgex bats family.
- [ ] **Step 5:** Commit `feat(installer): --mod-payload mode - in-place contents, mod-owned service, user-conf updates`.

> **Auto-detect steer (2026-08-31, Preston):** bare install on a mod host IS
> mod-payload; flags are overrides. `mod_payload_autodetect()` in main.sh arms
> the payload contract from the probe (HOST_MOD_ROOT) with no flag;
> `--standalone` escapes back to a self-managed install (platform root, with
> the Task 3 warning), `--payload-root` overrides where, `--auto-update` stays
> the opt-in for the user.moonraker.conf stanza. User-facing flag renames
> (`--standalone` / `--payload-root` / `--auto-update`); internal identifiers
> (HELIX_MOD_PAYLOAD, host_mod_destruct_blocked, payload_replace_contents)
> unchanged. Old `--mod-payload*` spellings accepted as deprecated aliases /
> compat no-op. Safety unchanged: the exemption still arms only via the probe
> or an explicit payload root, ZMOD hosts never arm (unrecognized tree), the
> env scrub stays, and the name gate still refuses the mod tree itself as a
> root.

### Task 6: Launcher and log_collector marker tests (audit item 13)

**Files:**
- Modify: `scripts/helix-launcher.sh:325`, `src/system/log_collector.cpp:134-190`
- Test: `tests/unit/test_log_collector.cpp` (extend)

**Interfaces:**
- Produces: both call the same rule — AD5X chroot is `zmod` OR forge-x (`/opt/config/mod/.shell/platform.sh` reachable from where the code runs). No shared shell/C++ code (different processes); the TEST pins both to the same truth table.

- [ ] **Step 1: Failing test** — unit test feeding the log_collector predicate a fake fs layout: forge-x layout (no `/ZMOD`, `.shell/platform.sh` present) → detected; zmod layout → detected; plain host → not.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Implement** both predicates; `make -j && make test`, run the extended unit test.
- [ ] **Step 4:** Green. Commit `fix(platform): recognize the forge-x chroot in launcher and log collector`.

### Task 7: Text-surgery containment on the reconciled forgex.sh

**Files:**
- Modify: `scripts/lib/installer/forgex.sh` (every `$SUDO mv "$tmp_file"` site: :121, :157, :186, :225, :313, :351 post-reconciliation), `uninstall_forgex` sequence
- Test: `tests/shell/test_forgex_display_modes.bats` (extend with sequence-level cases)

**Interfaces:**
- Produces: `forgex_apply_patch <tmp> <dest>` helper — `bash -n "$tmp"` or exit-with-restore; caller sites shrink to one line each. Sequence cases: `patch_forgex_screen_sh` → `patch_forgex_screen_drawing` and `unpatch_forgex_screen_sh` → `unpatch_forgex_screen_drawing` against fixture `screen.sh` files shaped like real 1.4.0 AND 1.4.2 upstream (byte-shapes already proven against the real files during review).

- [ ] **Step 1: Failing tests** — (a) a fixture where the old two-step unpatch order corrupts `screen.sh` (review's reproduced case) now yields a `bash -n`-clean file; (b) a deliberately broken patch candidate leaves the original file untouched.
- [ ] **Step 2:** FAIL (corruption reproduces on the reconciled code).
- [ ] **Step 3: Implement** the helper + call sites; fix the four line defects from the review as they survive reconciliation (unprivileged record write → `$SUDO tee`; HEADLESS-arrival recording; restore-mode-derived message; unmatched display spelling returns 1).
- [ ] **Step 4:** Green; regenerate bundles.
- [ ] **Step 5:** Commit `fix(forgex): syntax-validate every screen.sh rewrite and restore on failure`.

### Task 8: Hardware cycle on the rig (coordinated, last)

- [ ] Preconditions: Tasks 1-7 merged on the branch, full bats family green, `make test` green. Announce to the peer session; get Preston's OK for the window (a print may be queued).
- [ ] Dry-run reads only first: run the installer with `--mod-payload` + `--dry-run` if the mode exposes one (add it in Task 5 if cheap) and eyeball every path it names against the mod-owned list.
- [ ] Real cycle: install over the existing payload → verify UI boots (peer's `ctl` or screen), `user.moonraker.conf` gains nothing unless flagged, mod `moonraker.conf` byte-identical, `git -C` (from the ff5m checkout, not the rig) parity. Then uninstall → stock/recorded display mode restored, `screen.sh` `bash -n` clean, payload subtree gone, mod tree otherwise untouched.
- [ ] Record results in this plan's footer, then delete the plan on ship per convention.

## Self-Review

- Spec coverage: audit items 1-13 → Tasks: 1 (items 1,3-partial,12-guard), 2 (items 2,4,6), 3 (items 3,5,7,8), 4 (items 9-partial,10,11), 5 (items 1,9,12 structurally), 6 (item 13), 7 (review follow-ups). Order-of-work honored (guard first). ff5m#74 shape → Task 5 + Open Decisions. ✔
- The branch's takeover machinery survives via Task 0 + Task 7 fixes its verified corruption paths. ✔
- No placeholders: every step names files, code, or exact commands. Two deliberate deferrals are called out as Open Decisions with owners (peer conversation), not TBDs inside tasks. ✔
