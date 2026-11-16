#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Strip HELIX_TRACKING marker blocks from a Klipper config tree, or rename the
call inside each one to the hidden signal macro name.

HelixScreen versions before 1.1 could instrument a PRINT_START macro with
calls to HELIX_PHASE_*/HELIX_READY, each call wrapped in a pair of marker
comment lines. By default this script undoes that: it walks a config
directory, finds every well-formed marker block sitting inside a
[gcode_macro ...] body, and removes it, backing up each file it edits first.
With --rename-hidden it keeps each block and gives its call line the leading
underscore of the hidden name instead (HELIX_READY becomes _HELIX_READY), so
the signal survives once the visible names are gone. A block that already
calls the hidden name is left as it is, so a second rename edits nothing.

A file with any marker anomaly (unmatched or nested markers, a marker line
outside a gcode body, a block that is not exactly the three lines the
instrumentation ever wrote) is left byte-for-byte untouched; only its path
and the reason are reported. Nothing here restarts Klipper - the caller is
told the edit takes effect at the next restart.

Usage: strip_phase_tracking.py [--rename-hidden] [--dry-run] CONFIG_DIR
--dry-run prints what would change and writes nothing.
Exit status: 0 if every file was clean or fully stripped; 2 if nothing
failed but at least one file was skipped (an anomaly, or a concurrent
change) and needs a human to look at it; 1 if any file's edit failed
outright.
"""

import argparse
import os
import re
import shutil
import stat
import sys
import tempfile
import time

TRACKING_MARKER_BEGIN = b"# <<< HELIX_TRACKING v2 >>>"
TRACKING_MARKER_END = b"# <<< /HELIX_TRACKING >>>"

# main()'s exit status: install.sh's uninstall paths wire this straight
# through into their own exit code, so a caller two layers up (the
# HelixScreen app) can tell "nothing left to clean" from "something here
# needs a human" without re-deriving it from the printed summary.
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_NEEDS_ATTENTION = 2

# The only line _instrument_gcode ever wrote between the markers: a bare
# HELIX_PHASE_* or HELIX_READY call, no arguments. A renamed block calls the
# same name with a leading underscore, which both modes accept.
_MACRO_CALL_RE = re.compile(rb"^[ \t]*_?(HELIX_PHASE_[A-Z_]+|HELIX_READY)[ \t]*$")

# A call line still using the visible name, split around the name so a
# rename changes nothing but the one inserted underscore.
_VISIBLE_CALL_RE = re.compile(rb"^([ \t]*)(HELIX_PHASE_[A-Z_]+|HELIX_READY)([ \t]*\r?\n?)$")

_SECTION_HEADER_RE = re.compile(rb"^\[gcode_macro\s+[^\]]+\]", re.IGNORECASE)

# Klipper's own SAVE_CONFIG snapshots, e.g. "printer-20260101_120000.cfg" -
# point-in-time history that a phase-tracking strip should never touch.
_SNAPSHOT_NAME_RE = re.compile(r"^.+-\d{8}_\d{6}\.cfg$")


class StripAnomaly(Exception):
    """A file's markers don't match the one shape the instrumentation wrote."""


class StripWriteError(Exception):
    """A backup or write step failed; the original is left untouched."""


class StripConcurrentChangeError(Exception):
    """The file changed on disk after the strip decision was computed."""


class StripResult:
    def __init__(self, status, new_bytes=None, reason=None, blocks=0, names=None):
        self.status = status  # "clean" | "anomaly" | "stripped" | "renamed"
        self.new_bytes = new_bytes
        self.reason = reason
        self.blocks = blocks
        # The hidden names the renamed lines now call ("renamed" only)
        self.names = names if names is not None else set()


def _find_marker_lines(lines):
    """Return [(index, "BEGIN"|"END"), ...] for every line that is, after
    trimming surrounding whitespace (CRLF tolerated), exactly one marker.
    A line that merely contains marker text is not a marker."""
    result = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == TRACKING_MARKER_BEGIN:
            result.append((i, "BEGIN"))
        elif stripped == TRACKING_MARKER_END:
            result.append((i, "END"))
    return result


def _pair_markers(marker_lines):
    """Walk marker lines in file order, enforcing BEGIN then END before the
    next BEGIN, with no nesting. Returns [(begin_idx, end_idx), ...]."""
    pairs = []
    open_begin = None
    for idx, kind in marker_lines:
        if kind == "BEGIN":
            if open_begin is not None:
                raise StripAnomaly(f"nested BEGIN marker at line {idx + 1}")
            open_begin = idx
        else:
            if open_begin is None:
                raise StripAnomaly(f"END marker with no BEGIN at line {idx + 1}")
            pairs.append((open_begin, idx))
            open_begin = None
    if open_begin is not None:
        raise StripAnomaly(f"unmatched BEGIN marker at line {open_begin + 1}")
    return pairs


def _find_gcode_macro_body_spans(lines):
    """Yield (start, end) exclusive line-index ranges for each
    [gcode_macro ...] section's gcode: body.

    Mirrors how Klipper itself finds a macro's body: a body runs from the
    line after "gcode:" until a non-blank line starts in column 0, and a new
    section header ends the current one outright, with or without a body.
    """
    spans = []
    i = 0
    n = len(lines)
    while i < n:
        if not _SECTION_HEADER_RE.match(lines[i]):
            i += 1
            continue
        i += 1
        body_start = None
        while i < n:
            cur = lines[i]
            if cur.lstrip(b" \t").startswith(b"["):
                break
            if body_start is None:
                if cur.strip().startswith(b"gcode:"):
                    body_start = i + 1
                i += 1
                continue
            if cur.strip() and not cur[:1].isspace():
                break
            i += 1
        if body_start is not None:
            spans.append((body_start, i))
        # i now sits on the line that ended the section (a new header, or
        # EOF); the outer loop re-examines it without advancing further.
    return spans


def _validate_blocks(lines, pairs, body_spans):
    for begin_idx, end_idx in pairs:
        if end_idx != begin_idx + 2:
            raise StripAnomaly(f"marker block at line {begin_idx + 1} is not exactly 3 lines")
        if not _MACRO_CALL_RE.match(lines[begin_idx + 1].rstrip(b"\r\n")):
            raise StripAnomaly(
                f"marker block at line {begin_idx + 1} does not call a "
                "HELIX_PHASE_* or HELIX_READY macro"
            )
        if not any(start <= begin_idx and end_idx < end for start, end in body_spans):
            raise StripAnomaly(
                f"marker block at line {begin_idx + 1} is outside a [gcode_macro ...] body"
            )


def _valid_block_pairs(lines):
    """[(begin_idx, end_idx), ...] for a file's marker blocks, [] for a file
    with none. Raises StripAnomaly when any block breaks a shape rule."""
    marker_lines = _find_marker_lines(lines)
    if not marker_lines:
        return []
    pairs = _pair_markers(marker_lines)
    _validate_blocks(lines, pairs, _find_gcode_macro_body_spans(lines))
    return pairs


def compute_stripped_content(raw_bytes):
    """Decide what to do with one file's content.

    Returns a StripResult: "clean" (no markers, nothing to do), "anomaly"
    (a marker exists but the shape is wrong; `reason` explains why), or
    "stripped" (`new_bytes` is the content with every valid block removed).
    """
    lines = raw_bytes.splitlines(keepends=True)
    try:
        pairs = _valid_block_pairs(lines)
    except StripAnomaly as exc:
        return StripResult("anomaly", reason=str(exc))
    if not pairs:
        return StripResult("clean")

    remove = set()
    for begin_idx, end_idx in pairs:
        remove.update(range(begin_idx, end_idx + 1))
    new_bytes = b"".join(line for i, line in enumerate(lines) if i not in remove)

    # A line carries its OWN terminator. When the removed content sat at the
    # very end of a file with no trailing newline, the line that is now the
    # new final line still carries the terminator that only separated it
    # from what got removed, and that terminator is no longer needed.
    original_had_no_trailing_newline = bool(lines) and not lines[-1].endswith(b"\n")
    if original_had_no_trailing_newline and (len(lines) - 1) in remove and new_bytes.endswith(b"\n"):
        new_bytes = new_bytes[:-2] if new_bytes.endswith(b"\r\n") else new_bytes[:-1]

    return StripResult("stripped", new_bytes=new_bytes, blocks=len(pairs))


def compute_renamed_content(raw_bytes):
    """Decide how to move one file's marker blocks to the hidden names.

    The same shape rules as compute_stripped_content decide "clean" and
    "anomaly". A file whose blocks all call a hidden name already is
    "clean". Otherwise "renamed": `new_bytes` differs from the input only by
    one underscore before each visible call, `blocks` counts those calls and
    `names` holds the hidden names they now call.
    """
    lines = raw_bytes.splitlines(keepends=True)
    try:
        pairs = _valid_block_pairs(lines)
    except StripAnomaly as exc:
        return StripResult("anomaly", reason=str(exc))

    names = set()
    renamed = 0
    for begin_idx, _end_idx in pairs:
        call = _VISIBLE_CALL_RE.match(lines[begin_idx + 1])
        if call is None:
            continue
        lines[begin_idx + 1] = call.group(1) + b"_" + call.group(2) + call.group(3)
        names.add("_" + call.group(2).decode("ascii"))
        renamed += 1
    if not renamed:
        return StripResult("clean")
    return StripResult("renamed", new_bytes=b"".join(lines), blocks=renamed, names=names)


def _timestamped_backup_path(real_path):
    return f"{real_path}.bak.{time.strftime('%Y%m%d_%H%M%S')}"


def safe_replace_file(path, new_bytes, original):
    """Back up `path`'s real target, verify it, write `new_bytes` through a
    verified temp file, then replace the target's content - never the
    symlink itself, its mode, or (where permitted) its owner.

    `original` is the exact bytes the strip decision (`new_bytes`) was
    computed from. It is checked against the file on disk both up front and
    again immediately before the replace; a mismatch at either point means
    something else wrote to the file in between, and raises
    StripConcurrentChangeError instead of overwriting that change - the
    live file and the backup (if one was already made) are both left alone.

    Raises StripWriteError on any other failure; the original is left
    untouched and any temp file is removed.
    """
    real_path = os.path.realpath(path)

    try:
        orig_stat = os.stat(real_path)
        with open(real_path, "rb") as f:
            current = f.read()
    except OSError as exc:
        raise StripWriteError(f"could not read {path}: {exc}") from exc
    if current != original:
        raise StripConcurrentChangeError(f"{path} changed on disk since it was read")

    backup_path = _timestamped_backup_path(real_path)
    try:
        shutil.copy2(real_path, backup_path)
        with open(backup_path, "rb") as f:
            backup_bytes = f.read()
    except OSError as exc:
        # copy2 can raise partway through (e.g. EFBIG on a full disk),
        # leaving a partial file at backup_path - never leave that behind.
        _remove_quiet(backup_path)
        raise StripWriteError(f"could not back up {path}: {exc}") from exc
    if backup_bytes != original:
        _remove_quiet(backup_path)
        raise StripWriteError(f"backup of {path} did not verify")

    dir_name = os.path.dirname(real_path) or "."
    tmp_fd, tmp_path = tempfile.mkstemp(prefix=".helix-tracking-strip-", dir=dir_name)
    try:
        with os.fdopen(tmp_fd, "wb") as f:
            f.write(new_bytes)
            f.flush()
            os.fsync(f.fileno())
        with open(tmp_path, "rb") as f:
            written = f.read()
        if written != new_bytes:
            raise StripWriteError(f"write to a temp file for {path} did not verify")

        os.chmod(tmp_path, stat.S_IMODE(orig_stat.st_mode))
        try:
            os.chown(tmp_path, orig_stat.st_uid, orig_stat.st_gid)
        except (OSError, AttributeError):
            pass  # not permitted (non-root), or chown unavailable on this platform

        # The narrowest this window can be made without a lock, which a
        # Klipper config directory offers no convention for.
        with open(real_path, "rb") as f:
            just_before_replace = f.read()
        if just_before_replace != original:
            raise StripConcurrentChangeError(
                f"{path} changed on disk while stripping; the edit was not applied "
                f"(the content read is backed up at {backup_path})"
            )

        os.replace(tmp_path, real_path)
        tmp_path = None  # replaced; nothing left to clean up

        with open(real_path, "rb") as f:
            final_bytes = f.read()
        if final_bytes != new_bytes:
            # A rename needs no free space, unlike a copy, so this restore
            # cannot itself be truncated by a full disk.
            try:
                os.replace(backup_path, real_path)
            except OSError as exc:
                raise StripWriteError(
                    f"{path} did not verify after writing, AND restoring it from "
                    f"{backup_path} failed ({exc}) - {path} is left in a damaged state; "
                    f"restore it from {backup_path} by hand"
                ) from exc
            raise StripWriteError(
                f"{path} did not verify after writing - restored by moving the backup "
                f"{backup_path} back into place"
            )
    finally:
        if tmp_path is not None:
            _remove_quiet(tmp_path)

    return backup_path


def _remove_quiet(path):
    try:
        os.remove(path)
    except OSError:
        pass


def _is_snapshot_name(name):
    return bool(_SNAPSHOT_NAME_RE.match(name))


def _discover_cfg_files(scan_dir):
    """Walk scan_dir for every .cfg file, recursively. Returns
    {real_path: discovered_path} - discovered_path is the path the walk
    actually found (a symlink's own path, when one was involved), kept
    because that is where the pre-1.1 writer's own backup for that file
    would sit: `Path.glob()` yields the path as listed in its parent
    directory, and `cfg_file.with_suffix(...)` builds the backup name from
    that same listed path, not from whatever it resolves to.

    When the same real file is reachable under more than one alias, the one
    kept as "discovered" is whichever sorts first by its path parts relative
    to scan_dir - the same order `sorted(Path.glob("**/*.cfg"))` visits
    candidates in, which is the order the pre-1.1 writer relies on to pick
    between aliases. Matching that order, not just being deterministic, is
    what matters: it is the only way the hint's backup name can point at a
    file that writer would actually have created.

    Never descends a directory symlink: the writer that ever instrumented a
    macro located it with pathlib's `**` glob, which does not either, so a
    symlinked directory holds nothing this strip needs to undo - and without
    the guard, one pointing at a large or self-referential tree turns a
    config scan into a walk of that tree. A `.cfg` that is itself a symlink
    is still resolved to its real target, same as any other file, for both
    reading and editing. Skips SAVE_CONFIG snapshot files and anything under
    a config_backups directory.
    """
    candidates = []  # (relative_parts, found_path, real_path)
    for root, dirs, files in os.walk(scan_dir, followlinks=False):
        dirs[:] = [d for d in dirs if d != "config_backups"]
        for name in files:
            if not name.endswith(".cfg") or _is_snapshot_name(name):
                continue
            found_path = os.path.join(root, name)
            real_path = os.path.realpath(found_path)
            relative_parts = tuple(os.path.relpath(found_path, scan_dir).split(os.sep))
            candidates.append((relative_parts, found_path, real_path))

    discovered = {}
    for _relative_parts, found_path, real_path in sorted(candidates, key=lambda c: c[0]):
        if real_path not in discovered:
            discovered[real_path] = found_path
    return discovered


def find_cfg_files(scan_dir):
    """Every .cfg file under scan_dir, deduplicated by realpath and sorted
    for a deterministic processing order. See _discover_cfg_files for the
    discovery rules."""
    return sorted(_discover_cfg_files(scan_dir))


def process_file(path, discovered_path=None, mode="strip", dry_run=False, allowed_names=None):
    """Strip (mode "strip") or rename (mode "rename") one file's marker blocks.

    Returns a dict whose status is "clean", "edited", "would_edit" (dry_run:
    nothing written), "skipped" or "failed". A rename whose calls would name
    a hidden macro outside `allowed_names`, when given, is skipped: the file
    may have changed since the caller checked which names Klipper has.
    """
    if discovered_path is None:
        discovered_path = path

    try:
        with open(path, "rb") as f:
            original = f.read()
    except OSError as exc:
        return {"path": path, "status": "failed", "reason": f"could not read: {exc}"}

    if mode == "rename":
        result = compute_renamed_content(original)
    else:
        result = compute_stripped_content(original)
    if result.status == "clean":
        return {"path": path, "status": "clean"}
    if result.status == "anomaly":
        return {
            "path": path,
            "status": "skipped",
            "reason": result.reason,
            "discovered_path": discovered_path,
            "skip_kind": "anomaly",
        }
    names = sorted(result.names)
    if allowed_names is not None and not result.names <= set(allowed_names):
        unknown = ", ".join(sorted(result.names - set(allowed_names)))
        return {
            "path": path,
            "status": "skipped",
            "reason": f"the rename would call {unknown}, which Klipper has not loaded",
            "discovered_path": discovered_path,
            "skip_kind": "concurrent",
        }
    if dry_run:
        return {"path": path, "status": "would_edit", "blocks": result.blocks, "names": names}

    try:
        backup_path = safe_replace_file(path, result.new_bytes, original)
    except StripConcurrentChangeError as exc:
        # Something else wrote to the file after this decision was made; the
        # safe response is to leave it for the next run, not to overwrite
        # whatever that write was.
        return {
            "path": path,
            "status": "skipped",
            "reason": str(exc),
            "discovered_path": discovered_path,
            "skip_kind": "concurrent",
        }
    except (StripWriteError, OSError) as exc:
        # Any unhandled OS-level failure (a disk-full mid rename, for
        # example) is one file's failure, never a reason to stop processing
        # the rest of the tree or to claim success for this one.
        return {"path": path, "status": "failed", "reason": str(exc)}

    return {
        "path": path,
        "status": "edited",
        "backup": backup_path,
        "blocks": result.blocks,
        "names": names,
    }


class TreeReport:
    """What one pass over a config tree did, or would do on a dry run."""

    def __init__(self, config_dir):
        self.config_dir = config_dir
        self.edited = []
        self.would_edit = []
        self.skipped = []
        self.failed = []

    def add(self, entry):
        bucket = {
            "edited": self.edited,
            "would_edit": self.would_edit,
            "skipped": self.skipped,
            "failed": self.failed,
        }.get(entry["status"])
        if bucket is not None:
            bucket.append(entry)

    def names_needed(self):
        """The hidden names the planned or made edits call."""
        names = set()
        for entry in self.edited + self.would_edit:
            names.update(entry.get("names", ()))
        return names

    def exit_status(self):
        if self.failed:
            return EXIT_FAILED
        if self.skipped:
            return EXIT_NEEDS_ATTENTION
        return EXIT_OK

    def _relative(self, path):
        try:
            return os.path.relpath(path, os.path.realpath(self.config_dir))
        except ValueError:
            return path

    def summary_line(self, verb):
        """One line for the log, e.g. "renaming 9 call(s) in 1 file(s):
        printer.cfg (9)", naming any file left alone after a semicolon."""
        planned = self.edited + self.would_edit
        calls = sum(entry["blocks"] for entry in planned)
        line = f"{verb} {calls} call(s) in {len(planned)} file(s)"
        if planned:
            line += ": " + ", ".join(
                f"{self._relative(entry['path'])} ({entry['blocks']})" for entry in planned
            )
        left_alone = self.skipped + self.failed
        if left_alone:
            line += f"; {len(left_alone)} file(s) left alone: " + ", ".join(
                self._relative(entry["path"]) for entry in left_alone
            )
        return line


def run_tree(config_dir, mode="strip", dry_run=False, allowed_names=None):
    """process_file over every .cfg under config_dir, in a stable order. A
    missing directory is an empty report."""
    report = TreeReport(config_dir)
    if not os.path.isdir(config_dir):
        return report
    for real_path, discovered_path in sorted(_discover_cfg_files(config_dir).items()):
        report.add(process_file(real_path, discovered_path, mode=mode, dry_run=dry_run,
                                allowed_names=allowed_names))
    return report


def _next_step_hint(discovered_path, skip_kind):
    """What to tell a user about a file this script would not touch.

    A concurrent-change skip is not about the file's content at all - the
    right step is to re-run the strip, not to edit or restore anything.
    Every other skip names the marker text to remove by hand, and the
    backup a pre-1.1 HelixScreen's own instrumentation would have left
    beside the file it edited (from the same discovered path Path.glob()
    would have yielded, not the file's real target) - `<name>.bak.<epoch-
    seconds>` (with_suffix drops the extension rather than keeping it, so
    printer.cfg becomes printer.bak.<n>). Restoring that backup discards
    every config change made after it was written, which removing the
    marked lines by hand does not.
    """
    if skip_kind == "concurrent":
        return (
            f"re-run the uninstall (or strip_phase_tracking.py directly) - "
            f"something else changed {discovered_path} while this run was deciding what to strip"
        )
    stem = os.path.splitext(discovered_path)[0]
    return (
        f"remove the '{TRACKING_MARKER_BEGIN.decode()}' ... "
        f"'{TRACKING_MARKER_END.decode()}' block(s) in {discovered_path} by hand and "
        f"restart Klipper, which keeps every later config change; or restore it from a "
        f"backup named {stem}.bak.<a number>, if one exists, which discards every config "
        "change made after that backup was written"
    )


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config_dir", help="Klipper config directory to scan")
    parser.add_argument("--rename-hidden", action="store_true",
                        help="rename each block's call to the hidden name instead of stripping it")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would change and write nothing")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.config_dir):
        print(f"INFO: no config directory at {args.config_dir}; nothing to do")
        return 0

    mode = "rename" if args.rename_hidden else "strip"
    report = run_tree(args.config_dir, mode=mode, dry_run=args.dry_run)

    if args.dry_run:
        verb = "would rename" if mode == "rename" else "would strip"
        print(f"INFO: {report.summary_line(verb)}")
    for r in report.edited:
        if mode == "rename":
            print(f"INFO: renamed {r['blocks']} signal call(s) in {r['path']} "
                  f"(backup: {r['backup']})")
        else:
            print(f"INFO: stripped phase-tracking instrumentation from {r['path']} "
                  f"(backup: {r['backup']})")
    for r in report.skipped:
        print(f"WARN: left {r['path']} untouched: {r['reason']}")
        print(f"WARN: next step: {_next_step_hint(r['discovered_path'], r['skip_kind'])}")
    for r in report.failed:
        print(f"ERROR: failed to {mode} {r['path']}: {r['reason']}", file=sys.stderr)

    if report.edited:
        print("INFO: This takes effect at the next Klipper restart. "
              "Loaded macros keep working until then.")

    label = "signal macro rename" if mode == "rename" else "phase-tracking strip"
    print(f"INFO: {label} summary: edited {len(report.edited)}, "
          f"skipped {len(report.skipped)}, failed {len(report.failed)}")

    return report.exit_status()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
