#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject a vendored submodule whose applied patches are not the ones in patches/.

mk/patches.mk guards every apply with a variant of "is this file already dirty?"
-- `git -C $(LVGL_DIR) diff --quiet src/drivers/evdev/lv_evdev.c`, or a grep for
one marker string. What it never asks is "is it dirty with the CURRENT revision
of this patch". So the first revision of a patch to reach a checkout is the one
that stays there: editing the patch afterwards changes nothing for anybody whose
submodule already carries the old hunks, and the recipe cheerfully reports
"already applied".

That shipped. 86560d156 added lv_evdev_get_last_raw() to
patches/lvgl-evdev-protocol-a.patch, main's lib/lvgl kept the previous revision,
and every device cross-build failed with "not declared in this scope". Nothing
caught it: `make test` skips patch application entirely, and lv_evdev.c is
compiled out of desktop builds, so the full suite and every existing gate were
green while device builds were hard-broken.

Reverse-applying each patch to check would be the obvious test and does not
work: 17 of 66 patches fail `git apply -R --check` despite being correctly
applied, because several patches touch one file (seven touch
src/misc/lv_event.c) and reverse-applying one alone fails against context its
siblings changed. Concatenating the set and reverse-applying that fails too, in
both orderings -- the real apply order is not derivable from the file.

So this works from a stamp instead. When mk/patches.mk finishes applying a
submodule's patches it records, in that submodule's git directory, the sha256 of
every patch file and the sha256 of every file those patches touch. Two hashes
because they catch different drift:

  patch file hash  -> somebody edited a patch that is already applied
  submodule file   -> the submodule was reset, updated, or hand-edited

WHY THIS DOES NOT AUTO-REPAIR. Repairing means restoring the touched files to
pristine and re-applying the whole set. Several files are touched by multiple
patches, so a targeted restore of one file silently drops the other patches'
hunks from it -- which is a quieter version of the bug this gate exists to
catch. The only safe repair is the whole-submodule one, and that is
`make reapply-patches`, spelled out in every failure message.

Modes:

  (default)     full verdict; what scripts/quality-checks.sh runs
  --pre-apply   what mk/patches.mk runs before its apply blocks. Identical
                except that a patch which is new since the last stamp is not
                drift: its apply guard tests a marker that is not in the tree
                yet, so the blocks about to run WILL apply it. An EDITED patch
                still fails, because its guard matches the marker the old
                revision already left behind.
  --write-stamp record the current state; run by mk/patches.mk after applying
  --clear-stamp forget the recorded state; run by `make reset-patches`, which
                puts the submodule back to pristine and so invalidates it
  --list        one bare finding per line, no remedy text
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

STAMP_NAME = "helix-patches-applied.json"
STAMP_VERSION = 1

# Every apply in mk/patches.mk has this shape, on one line:
#   git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_sdl_window.patch
# which binds the patch to its submodule without a hand-maintained table.
APPLY_RE = re.compile(
    r"git\s+-C\s+\$\((?P<var>[A-Z0-9_]+)_DIR\)\s+apply\b[^;\n]*?"
    r"\$\(PATCH_DIR\)/(?P<patch>[A-Za-z0-9_.-]+\.patch)"
)

# `LVGL_DIR := lib/lvgl` in the top-level Makefile.
DIR_VAR_RE = re.compile(r"^\s*([A-Z0-9_]+)_DIR\s*:?=\s*(\S+)\s*$", re.MULTILINE)

EXEMPT_RE = re.compile(r"^PATCH_EXEMPT\s*:?=\s*(.*)$", re.MULTILINE)

REMEDY = (
    "   Fix: make reapply-patches\n"
    "   (Restoring single files by hand is not equivalent -- several patches\n"
    "    touch one file, so a targeted restore drops the other patches' hunks.)\n"
    "   Run it in the tree that reported this, and only that tree. lib/lvgl and\n"
    "   lib/libhv are a private checkout per worktree, so the run repairs this\n"
    "   tree's submodules against this branch's patches/ and reaches no other.\n"
    "   Check for a live build in THIS tree first -- it rewrites headers a compile\n"
    "   here may be reading:\n"
    "     pgrep -x -d\' \' \'make|cc1plus\'"
)


# A pre-commit hook runs with GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE pointing
# at the SUPERPROJECT. Those leak into any git we spawn, so
# `git -C lib/lvgl rev-parse --absolute-git-dir` hands back the worktree git dir
# instead of .git/modules/lvgl, the stamp is looked for at a path that never has
# one, and the gate reports [no-stamp] every single time from inside a hook. It
# passed standalone and failed on commit, which reads as flaky rather than wrong.
_GIT_ENV_LEAKS = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY")


def git_env() -> dict:
    """Environment for a git call that must resolve against its own -C path."""
    env = dict(os.environ)
    for name in _GIT_ENV_LEAKS:
        env.pop(name, None)
    return env


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def patched_paths(patch: Path) -> set[str]:
    """Submodule-relative paths a unified diff touches.

    Read off the `+++ b/<path>` headers, with `--- a/<path>` covering a deletion
    (where the `+++` side is /dev/null). A created file has /dev/null on the
    `---` side and a real path on the `+++` side, so it needs no special case.
    """
    paths: set[str] = set()
    text = patch.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("+++ ") or line.startswith("--- "):
            target = line[4:].split("\t")[0].strip()
            if target in ("/dev/null", ""):
                continue
            if target[:2] in ("a/", "b/"):
                target = target[2:]
            paths.add(target)
    return paths


class Submodule:
    """One vendored submodule plus the patches mk/patches.mk applies into it."""

    def __init__(self, var: str, rel: str, root: Path):
        self.var = var
        self.rel = rel
        self.path = root / rel
        self.patches: list[str] = []

    @property
    def exists(self) -> bool:
        return self.path.is_dir()

    def git_dir(self) -> Path | None:
        """Absolute git dir, or None when this is not a git checkout.

        A submodule's working tree holds a `.git` FILE pointing at its git dir,
        which for a worktree's private lvgl/libhv checkout is that worktree's own
        .git/worktrees/<name>/modules/<name>. Asking the checkout is what keeps
        the stamp with the files it describes, since each tree patches its own.
        """
        try:
            out = subprocess.run(
                ["git", "-C", str(self.path), "rev-parse", "--absolute-git-dir"],
                capture_output=True,
                text=True,
                check=True,
                env=git_env(),
            )
        except (OSError, subprocess.CalledProcessError):
            return None
        return Path(out.stdout.strip())

    def stamp_path(self) -> Path | None:
        gd = self.git_dir()
        return None if gd is None else gd / STAMP_NAME

    def touched(self, patch_dir: Path) -> set[str]:
        paths: set[str] = set()
        for name in self.patches:
            p = patch_dir / name
            if p.is_file():
                paths |= patched_paths(p)
        return paths

    def file_hashes(self, patch_dir: Path) -> dict[str, str | None]:
        """sha256 per touched file; None when the file is not on disk."""
        out: dict[str, str | None] = {}
        for rel in sorted(self.touched(patch_dir)):
            f = self.path / rel
            out[rel] = sha256_file(f) if f.is_file() else None
        return out

    def patch_hashes(self, patch_dir: Path) -> dict[str, str]:
        out: dict[str, str] = {}
        for name in sorted(self.patches):
            p = patch_dir / name
            if p.is_file():
                out[name] = sha256_file(p)
        return out

    def looks_patched(self, patch_dir: Path) -> list[str]:
        """Touched files that are not in their pristine state.

        Two signals, because `git diff` only sees tracked files: a tracked file
        that differs from HEAD, and an untracked file that a patch CREATES
        (lvgl's src/misc/lv_check_arg.h) simply being present. Deliberately
        scoped to the touched files rather than the whole submodule -- libhv's
        config.mk and hconfig.h are dirty in any built tree and are written by
        libhv's own ./configure, not by us.
        """
        dirty: list[str] = []
        for rel in sorted(self.touched(patch_dir)):
            f = self.path / rel
            tracked = (
                subprocess.run(
                    ["git", "-C", str(self.path), "ls-files", "--error-unmatch", rel],
                    capture_output=True,
                    env=git_env(),
                ).returncode
                == 0
            )
            if tracked:
                changed = (
                    subprocess.run(
                        ["git", "-C", str(self.path), "diff", "--quiet", "--", rel],
                        capture_output=True,
                        env=git_env(),
                    ).returncode
                    != 0
                )
                if changed:
                    dirty.append(rel)
            elif f.is_file():
                dirty.append(rel)
        return dirty


def parse_layout(root: Path) -> tuple[list[Submodule], set[str], Path]:
    """Read mk/patches.mk + Makefile into submodules, exempt patches, patch dir."""
    mk_path = root / "mk" / "patches.mk"
    makefile = root / "Makefile"
    patch_dir = root / "patches"

    mk_text = mk_path.read_text(errors="replace") if mk_path.is_file() else ""
    mf_text = makefile.read_text(errors="replace") if makefile.is_file() else ""

    dirs = {m.group(1): m.group(2) for m in DIR_VAR_RE.finditer(mf_text)}
    # mk/*.mk may define one too; the Makefile wins where both do.
    for m in DIR_VAR_RE.finditer(mk_text):
        dirs.setdefault(m.group(1), m.group(2))

    exempt: set[str] = set()
    em = EXEMPT_RE.search(mk_text)
    if em:
        exempt = {tok for tok in em.group(1).split() if tok.endswith(".patch")}

    subs: dict[str, Submodule] = {}
    for m in APPLY_RE.finditer(mk_text):
        var, name = m.group("var"), m.group("patch")
        if name in exempt:
            continue
        rel = dirs.get(var)
        if rel is None:
            continue
        sub = subs.setdefault(var, Submodule(var, rel, root))
        if name not in sub.patches:
            sub.patches.append(name)

    return [subs[k] for k in sorted(subs)], exempt, patch_dir


class Finding:
    def __init__(self, kind: str, sub: str, item: str, detail: str):
        self.kind = kind
        self.sub = sub
        self.item = item
        self.detail = detail

    def line(self) -> str:
        return f"{self.kind} {self.sub} {self.item}"


def check_submodule(
    sub: Submodule, patch_dir: Path, pre_apply: bool
) -> tuple[list[Finding], str | None]:
    """Findings for one submodule, plus a note when it was skipped."""
    if not sub.exists:
        return [], f"{sub.rel}: not checked out -- skipped"

    stamp_path = sub.stamp_path()
    if stamp_path is None:
        return [], f"{sub.rel}: not a git checkout -- skipped"

    if not stamp_path.is_file():
        dirty = sub.looks_patched(patch_dir)
        if not dirty:
            # A clone that has not built yet. The ordinary build flow applies
            # the patches and writes the stamp; failing here would fail every
            # first build.
            return [], f"{sub.rel}: pristine, patches not applied yet"
        return [
            Finding(
                "no-stamp",
                sub.rel,
                f"{len(dirty)} patched files",
                "patched by a build that predates stamping, so which revision "
                "of each patch is in there is unknowable\n"
                "      e.g. " + ", ".join(dirty[:4]),
            )
        ], None

    try:
        stamp = json.loads(stamp_path.read_text())
    except (OSError, ValueError) as exc:
        return [
            Finding("bad-stamp", sub.rel, stamp_path.name, f"unreadable: {exc}")
        ], None

    findings: list[Finding] = []

    stamped_patches: dict[str, str] = stamp.get("patches", {})
    current_patches = sub.patch_hashes(patch_dir)

    for name in sorted(current_patches):
        if name not in stamped_patches:
            if pre_apply:
                # New since the last stamp. Its apply guard tests a marker that
                # is not in the tree, so the blocks about to run will apply it.
                continue
            findings.append(
                Finding(
                    "patch-unapplied",
                    sub.rel,
                    name,
                    "on the shelf but never applied to this checkout",
                )
            )
        elif stamped_patches[name] != current_patches[name]:
            findings.append(
                Finding(
                    "patch-edited",
                    sub.rel,
                    name,
                    "edited since it was applied; the apply guard sees the old "
                    "revision's marker and reports it as already applied",
                )
            )

    for name in sorted(stamped_patches):
        if name not in current_patches:
            findings.append(
                Finding(
                    "patch-removed",
                    sub.rel,
                    name,
                    "gone from patches/ but its hunks are still in the submodule",
                )
            )

    stamped_files: dict[str, str | None] = stamp.get("files", {})
    for rel in sorted(stamped_files):
        f = sub.path / rel
        now = sha256_file(f) if f.is_file() else None
        if now != stamped_files[rel]:
            if now is None:
                detail = "gone since the patches were applied"
            elif stamped_files[rel] is None:
                detail = "appeared since the patches were applied"
            else:
                detail = "changed since the patches were applied"
            findings.append(Finding("file-changed", sub.rel, rel, detail))

    return findings, None


def write_stamps(subs: list[Submodule], patch_dir: Path) -> int:
    written = 0
    for sub in subs:
        if not sub.exists:
            continue
        stamp_path = sub.stamp_path()
        if stamp_path is None:
            continue
        payload = {
            "version": STAMP_VERSION,
            "submodule": sub.rel,
            "patches": sub.patch_hashes(patch_dir),
            "files": sub.file_hashes(patch_dir),
        }
        try:
            stamp_path.write_text(json.dumps(payload, indent=1, sort_keys=True) + "\n")
        except OSError as exc:
            print(f"⚠️  could not write {stamp_path}: {exc}", file=sys.stderr)
            continue
        written += 1
        print(
            f"   stamped {sub.rel}: {len(payload['patches'])} patches, "
            f"{len(payload['files'])} files"
        )
    return written


def clear_stamps(subs: list[Submodule]) -> int:
    cleared = 0
    for sub in subs:
        if not sub.exists:
            continue
        stamp_path = sub.stamp_path()
        if stamp_path is None or not stamp_path.is_file():
            continue
        try:
            stamp_path.unlink()
        except OSError as exc:
            print(f"⚠️  could not remove {stamp_path}: {exc}", file=sys.stderr)
            continue
        cleared += 1
        print(f"   cleared patch stamp for {sub.rel}")
    return cleared


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=str(REPO_ROOT))
    ap.add_argument(
        "--write-stamp",
        action="store_true",
        help="record the applied state (run by mk/patches.mk after applying)",
    )
    ap.add_argument(
        "--clear-stamp",
        action="store_true",
        help="forget the recorded state (run by `make reset-patches`)",
    )
    ap.add_argument(
        "--pre-apply",
        action="store_true",
        help="mk/patches.mk mode: a patch new since the last stamp is not drift",
    )
    ap.add_argument("--list", action="store_true", help="one bare finding per line")
    args = ap.parse_args(argv[1:])

    root = Path(args.repo_root).resolve()
    subs, exempt, patch_dir = parse_layout(root)

    if not subs:
        print("⚠️  no patched submodules found -- nothing to check")
        return 0

    if args.clear_stamp:
        clear_stamps(subs)
        return 0

    if args.write_stamp:
        write_stamps(subs, patch_dir)
        return 0

    findings: list[Finding] = []
    notes: list[str] = []
    for sub in subs:
        f, note = check_submodule(sub, patch_dir, args.pre_apply)
        findings.extend(f)
        if note:
            notes.append(note)

    if args.list:
        for f in findings:
            print(f.line())
        return 1 if findings else 0

    if findings:
        print("❌ patch drift: the applied patches are not the patches in patches/")
        for f in findings:
            print(f"   [{f.kind}] {f.sub} {f.item}")
            print(f"      {f.detail}")
        print()
        print(REMEDY)
        return 1

    total_patches = sum(len(s.patches) for s in subs)
    print(
        f"✅ no patch drift ({total_patches} patches over "
        f"{len(subs)} submodules, {len(exempt)} exempt)"
    )
    for note in notes:
        print(f"   {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
