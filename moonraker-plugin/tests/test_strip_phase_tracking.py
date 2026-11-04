# SPDX-License-Identifier: GPL-3.0-or-later
"""
Unit tests for strip_phase_tracking.py.

Run with: pytest tests/test_strip_phase_tracking.py -v
"""

import os
import re
import shutil
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import strip_phase_tracking as strip  # noqa: E402
from fixtures import pre_1_1_instrumentation as old_writer  # noqa: E402


BEGIN = "# <<< HELIX_TRACKING v2 >>>"
END = "# <<< /HELIX_TRACKING >>>"

VALID_BLOCK = f"    {BEGIN}\n    HELIX_PHASE_HOMING\n    {END}\n"


def write(path: Path, content: str):
    path.write_bytes(content.encode())


def read(path: Path) -> str:
    return path.read_bytes().decode()


# ============================================================================
# Matching contract (item 1)
# ============================================================================


class TestMarkerMatching:
    def test_no_markers_leaves_file_untouched_and_makes_no_backup(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n"
        write(cfg, original)

        result = strip.main([str(tmp_path)])

        assert result == 0
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_a_comment_naming_both_markers_is_not_a_marker(self, tmp_path):
        # A line that merely contains marker text - copied into a comment,
        # say - is not itself a marker, so nothing about it looks anomalous
        # either; the file simply has nothing to strip.
        cfg = tmp_path / "printer.cfg"
        original = (
            f"# See {BEGIN} and {END} in the docs\n"
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
        )
        write(cfg, original)

        result = strip.main([str(tmp_path)])

        assert result == 0
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_a_comment_naming_one_marker_beside_a_real_block_only_touches_the_block(
        self, tmp_path
    ):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"# old marker was {BEGIN}\n"
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n" + VALID_BLOCK
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0

        stripped = read(cfg)
        assert BEGIN not in stripped.split("\n", 1)[1]  # the real block is gone
        assert stripped.startswith(f"# old marker was {BEGIN}\n")  # the comment survives

    def test_unmatched_begin_is_left_untouched_with_no_backup(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original
        assert list(tmp_path.iterdir()) == [cfg]

    def test_end_with_no_begin_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    HELIX_PHASE_HOMING\n    {END}\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original

    def test_nested_begin_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n    {BEGIN}\n"
            f"    HELIX_PHASE_HOMING\n    {END}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original

    def test_a_count_balanced_hand_edit_is_caught_by_shape_not_just_counts(self, tmp_path):
        # BEGIN of the first block and END of a second block were deleted by
        # hand: two BEGINs and two ENDs total, but the pairing is broken, and
        # a naive count comparison alone would call this well-formed.
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            "    HELIX_PHASE_HOMING\n"  # orphaned - its BEGIN was deleted
            f"    {END}\n"
            "    QUAD_GANTRY_LEVEL\n"
            f"    {BEGIN}\n"  # orphaned - its END was deleted
            "    HELIX_PHASE_QGL\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_block_outside_a_gcode_macro_body_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        # A real-shaped block sitting between two sections, in nobody's body.
        original = "[printer]\nkinematics: corexy\n\n" + VALID_BLOCK + "\n[extruder]\nstep_pin: PA1\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original

    def test_a_block_of_the_wrong_length_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n"
            "    HELIX_PHASE_HOMING\n    G28\n"
            f"    {END}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original

    def test_a_block_calling_something_other_than_a_phase_macro_is_left_untouched(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = f"[gcode_macro PRINT_START]\ngcode:\n    {BEGIN}\n    G28\n    {END}\n"
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == strip.EXIT_NEEDS_ATTENTION
        assert read(cfg) == original

    def test_crlf_markers_are_recognized_and_crlf_is_preserved(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\r\n"
            "gcode:\r\n"
            "    G28\r\n"
            f"    {BEGIN}\r\n"
            "    HELIX_PHASE_HOMING\r\n"
            f"    {END}\r\n"
            "    M109 S{EXTRUDER_TEMP}\r\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert BEGIN not in result
        assert "\r\n" in result
        assert result == "[gcode_macro PRINT_START]\r\ngcode:\r\n    G28\r\n    M109 S{EXTRUDER_TEMP}\r\n"

    def test_two_blocks_in_one_file_both_removed(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\ngcode:\n    G28\n"
            + VALID_BLOCK
            + "    M190 S{BED_TEMP}\n"
            f"    {BEGIN}\n    HELIX_PHASE_HEATING_BED\n    {END}\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert BEGIN not in result and END not in result
        assert result == (
            "[gcode_macro PRINT_START]\ngcode:\n    G28\n    M190 S{BED_TEMP}\n"
            "    M109 S{EXTRUDER_TEMP}\n"
        )

    def test_content_outside_removed_blocks_is_byte_identical(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            + VALID_BLOCK
            + "    M190 S{BED_TEMP}\n"
            "      ; oddly indented comment\n"
            "    M109 S{EXTRUDER_TEMP}\n"
            "\n"
            "    BED_MESH_CALIBRATE\n"
        )
        write(cfg, original)
        expected = (
            "[gcode_macro PRINT_START]\n"
            "gcode:\n"
            "    G28\n"
            "    M190 S{BED_TEMP}\n"
            "      ; oddly indented comment\n"
            "    M109 S{EXTRUDER_TEMP}\n"
            "\n"
            "    BED_MESH_CALIBRATE\n"
        )

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == expected

    def test_removing_the_files_final_line_does_not_invent_a_trailing_newline(self, tmp_path):
        # The block sits at absolute EOF with no trailing newline of its own
        # (how the real writer always left one there); removing it must not
        # leave the newline separating it from G28 dangling as a new
        # trailing newline G28 never had.
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK.rstrip("\n")
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert result == "[gcode_macro PRINT_START]\ngcode:\n    G28"

    def test_removing_the_files_final_crlf_line_trims_the_whole_crlf_pair(self, tmp_path):
        # Same shape as the LF case above, but every line ends \r\n and the
        # removed block's own END line carries none (absolute EOF) - the
        # trim must drop both bytes of the dangling \r\n, not just the \n.
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\r\ngcode:\r\n    G28\r\n"
            f"    {BEGIN}\r\n    HELIX_PHASE_HOMING\r\n    {END}"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert result == "[gcode_macro PRINT_START]\r\ngcode:\r\n    G28"

    def test_a_middle_of_file_no_trailing_newline_file_is_unaffected(self, tmp_path):
        # The removed block is NOT at the file's end, so the true final line
        # (already carrying no trailing newline) must stay exactly as it was.
        cfg = tmp_path / "printer.cfg"
        original = (
            "[gcode_macro PRINT_START]\ngcode:\n    G28\n"
            + VALID_BLOCK
            + "    M109 S{EXTRUDER_TEMP}"
        )
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        result = read(cfg)
        assert result == "[gcode_macro PRINT_START]\ngcode:\n    G28\n    M109 S{EXTRUDER_TEMP}"

    def test_running_the_strip_twice_is_harmless(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        assert strip.main([str(tmp_path)]) == 0
        after_first = read(cfg)
        backups_after_first = list(tmp_path.glob("*.bak.*"))

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == after_first
        assert list(tmp_path.glob("*.bak.*")) == backups_after_first


# ============================================================================
# File discovery (item 2)
# ============================================================================


class TestFileDiscovery:
    def test_a_macro_in_a_subdirectory_is_found_and_stripped(self, tmp_path):
        (tmp_path / "macros").mkdir()
        top = tmp_path / "printer.cfg"
        write(top, "[include macros/*.cfg]\n[printer]\nkinematics: corexy\n")
        sub = tmp_path / "macros" / "print_start.cfg"
        write(sub, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        assert strip.main([str(tmp_path)]) == 0
        assert BEGIN not in read(sub)
        assert read(top) == "[include macros/*.cfg]\n[printer]\nkinematics: corexy\n"

    def test_save_config_snapshot_is_skipped(self, tmp_path):
        snapshot = tmp_path / "printer-20260101_120000.cfg"
        write(snapshot, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        original = read(snapshot)

        assert strip.main([str(tmp_path)]) == 0
        assert read(snapshot) == original
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_symlinked_cfg_is_dereferenced_and_the_real_target_stripped(self, tmp_path):
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        link = config_dir / "printer.cfg"
        link.symlink_to(real_file)

        assert strip.main([str(config_dir)]) == 0

        assert link.is_symlink()
        assert os.readlink(link) == str(real_file)
        assert BEGIN not in real_file.read_text()

    def test_two_paths_to_the_same_real_file_are_only_edited_once(self, tmp_path):
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "a.cfg").symlink_to(real_file)
        (config_dir / "b.cfg").symlink_to(real_file)

        assert strip.main([str(config_dir)]) == 0
        assert len(list(real_dir.glob("*.bak.*"))) == 1

    def test_find_cfg_files_dedupes_by_realpath(self, tmp_path):
        # Asserts the discovery step itself, not the end state of a strip
        # run: once one alias's target is stripped, a second alias finds no
        # markers left and is silently skipped either way, so a test that
        # only checks the end state cannot distinguish real dedupe from
        # that coincidence.
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "content")

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "a.cfg").symlink_to(real_file)
        (config_dir / "b.cfg").symlink_to(real_file)

        found = strip.find_cfg_files(str(config_dir))
        assert found == [str(real_file.resolve())]

    def test_discover_cfg_files_keeps_the_symlinks_own_path(self, tmp_path):
        # The pre-1.1 writer's own backup for a symlinked cfg lands beside
        # the symlink, not beside its real target - Path.glob() yields the
        # path as listed in its parent directory, and with_suffix() builds
        # the backup name from that same path.
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "content")

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        link = config_dir / "printer.cfg"
        link.symlink_to(real_file)

        discovered = strip._discover_cfg_files(str(config_dir))
        assert discovered == {str(real_file.resolve()): str(link)}

    def test_two_aliases_at_different_depths_keep_the_ones_sorted_glob_order(self, tmp_path):
        # The pre-1.1 writer located a macro with sorted(Path.glob("**/*.cfg")),
        # which visits "a/real.cfg" before the top-level "b.cfg" (path parts
        # ("a", "real.cfg") sort before ("b.cfg",)) - so that writer's own
        # backup for this file always lands beside a/real.cfg. os.walk visits
        # a root's own files before any subdirectory's, so a naive walk-order
        # pick (first or last encountered) would keep "b.cfg" instead, and a
        # later hint would name a backup that was never written.
        config_dir = tmp_path / "config"
        (config_dir / "a").mkdir(parents=True)
        real_file = config_dir / "a" / "real.cfg"
        write(real_file, "content")
        (config_dir / "b.cfg").symlink_to(real_file)

        discovered = strip._discover_cfg_files(str(config_dir))
        assert discovered == {str(real_file.resolve()): str(real_file)}

    def test_a_symlinked_directory_is_not_descended(self, tmp_path):
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        write(real_dir / "printer.cfg", "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "linked").symlink_to(real_dir)

        assert strip.find_cfg_files(str(config_dir)) == []
        assert strip.main([str(config_dir)]) == 0
        assert BEGIN in (real_dir / "printer.cfg").read_text()

    def test_a_directory_symlink_loop_terminates(self, tmp_path):
        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "loop").symlink_to(config_dir)
        write(config_dir / "printer.cfg", "content")

        found = strip.find_cfg_files(str(config_dir))
        assert found == [str((config_dir / "printer.cfg").resolve())]

    def test_a_file_in_config_backups_is_skipped_regardless_of_its_name(self, tmp_path):
        backups_dir = tmp_path / "config_backups"
        backups_dir.mkdir()
        cfg = backups_dir / "macros.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        original = read(cfg)

        assert strip.main([str(tmp_path)]) == 0
        assert read(cfg) == original
        assert not list(backups_dir.glob("*.bak.*"))


# ============================================================================
# Safe writes (item 3)
# ============================================================================


class TestSafeWrites:
    def test_backup_and_target_mode_are_preserved_0444(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        cfg.chmod(0o444)

        try:
            assert strip.main([str(tmp_path)]) == 0
            assert stat.S_IMODE(cfg.stat().st_mode) == 0o444
            backups = list(tmp_path.glob("*.bak.*"))
            assert len(backups) == 1
            assert stat.S_IMODE(backups[0].stat().st_mode) == 0o444
        finally:
            cfg.chmod(0o644)  # tmp_path cleanup needs write access

    def test_mode_0600_is_preserved(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)
        cfg.chmod(0o600)

        assert strip.main([str(tmp_path)]) == 0
        assert stat.S_IMODE(cfg.stat().st_mode) == 0o600

    def test_a_read_only_directory_fails_cleanly_without_touching_the_file(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)
        tmp_path.chmod(0o555)

        try:
            result = strip.main([str(tmp_path)])
        finally:
            tmp_path.chmod(0o755)  # restore so pytest can clean up tmp_path

        assert result == 1
        assert read(cfg) == original
        assert not list(tmp_path.glob("*.bak.*"))
        assert not list(tmp_path.glob(".helix-tracking-strip-*"))

    def test_a_write_failure_leaves_the_original_and_backup_intact(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        real_fsync = os.fsync

        def failing_fsync(fd):
            raise OSError("simulated disk-full during fsync")

        monkeypatch.setattr(os, "fsync", failing_fsync)
        try:
            result = strip.main([str(tmp_path)])
        finally:
            monkeypatch.setattr(os, "fsync", real_fsync)

        assert result == 1
        assert read(cfg) == original
        backups = list(tmp_path.glob("*.bak.*"))
        assert len(backups) == 1
        assert read(backups[0]) == original
        assert not list(tmp_path.glob(".helix-tracking-strip-*"))

    def test_a_post_write_verification_mismatch_restores_from_backup(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        real_replace = os.replace
        calls = []

        def corrupting_replace(src, dst):
            # Simulate the replace landing, but the destination's bytes not
            # matching what was verified in the temp file (e.g. another
            # process wrote in between). Only the strip's own write - not
            # its later restore-from-backup, also an os.replace - should be
            # corrupted, or the restore could never succeed either.
            calls.append((src, dst))
            if ".helix-tracking-strip-" in str(src):
                with open(src, "ab") as f:
                    f.write(b"CORRUPT")
            real_replace(src, dst)

        monkeypatch.setattr(os, "replace", corrupting_replace)
        try:
            result = strip.main([str(tmp_path)])
        finally:
            monkeypatch.setattr(os, "replace", real_replace)

        assert result == 1
        assert len(calls) == 2  # the write, then the restore
        assert read(cfg) == original  # restored from the verified backup

    def test_a_corrupted_backup_is_refused_and_removed(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        real_copy2 = shutil.copy2

        def truncating_copy2(src, dst):
            real_copy2(src, dst)
            with open(dst, "wb") as f:
                f.write(b"short")

        monkeypatch.setattr(shutil, "copy2", truncating_copy2)
        result = strip.main([str(tmp_path)])

        assert result == 1
        assert read(cfg) == original  # the edit never happened
        assert not list(tmp_path.glob("*.bak.*"))  # the bad backup was removed

    def test_a_backup_raising_partway_leaves_no_partial_file(self, tmp_path, monkeypatch):
        # Distinct from the truncated-but-successful copy above: copy2 here
        # writes part of the file and then raises (an EFBIG-shaped failure),
        # instead of returning normally with short content.
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)

        def raising_copy2(src, dst):
            with open(src, "rb") as f:
                partial = f.read()[:5]
            with open(dst, "wb") as f:
                f.write(partial)
            raise OSError("simulated EFBIG partway through the backup")

        monkeypatch.setattr(shutil, "copy2", raising_copy2)
        result = strip.main([str(tmp_path)])

        assert result == 1
        assert read(cfg) == original
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_corrupted_temp_write_is_caught_before_touching_the_real_file(self, tmp_path, monkeypatch):
        cfg = tmp_path / "printer.cfg"
        original = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original)
        inode_before = cfg.stat().st_ino

        real_fdopen = os.fdopen

        class CorruptingFile:
            def __init__(self, fd):
                self._f = real_fdopen(fd, "wb")

            def write(self, data):
                return self._f.write(data + b"\x00")

            def flush(self):
                return self._f.flush()

            def fileno(self):
                return self._f.fileno()

            def __enter__(self):
                return self

            def __exit__(self, *exc_info):
                self._f.close()

        def fake_fdopen(fd, mode="r", *args, **kwargs):
            if mode == "wb":
                return CorruptingFile(fd)
            return real_fdopen(fd, mode, *args, **kwargs)

        real_replace = os.replace
        replace_calls = []

        def spying_replace(src, dst):
            replace_calls.append((src, dst))
            return real_replace(src, dst)

        monkeypatch.setattr(os, "fdopen", fake_fdopen)
        monkeypatch.setattr(os, "replace", spying_replace)
        result = strip.main([str(tmp_path)])

        assert result == 1
        # The corrupted temp file must never reach printer.cfg at all - not
        # get replaced in and then restored, which would also leave the
        # bytes matching but would have replaced a live config file with
        # unverified content, if only for an instant.
        assert replace_calls == []
        assert cfg.stat().st_ino == inode_before
        assert read(cfg) == original
        assert not list(tmp_path.glob(".helix-tracking-strip-*"))

    def test_safe_replace_file_resolves_a_symlink_path_to_its_real_target(self, tmp_path):
        # Called directly, bypassing find_cfg_files' own realpath resolution -
        # this is what actually proves safe_replace_file replaces onto the
        # real path itself rather than trusting an already-resolved caller.
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(real_file, "original content\n")

        link = tmp_path / "link.cfg"
        link.symlink_to(real_file)

        strip.safe_replace_file(str(link), b"new content\n", b"original content\n")

        assert link.is_symlink()
        assert real_file.read_bytes() == b"new content\n"

    def test_a_file_changed_between_read_and_write_is_skipped_not_overwritten(self, tmp_path):
        cfg = tmp_path / "printer.cfg"
        original_text = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original_text)
        original_bytes = cfg.read_bytes()

        # Simulate a concurrent save (a Mainsail edit, or Klipper's own
        # SAVE_CONFIG) landing after the strip decision was computed.
        edited_bytes = original_bytes + b"\n; a concurrent edit\n"
        cfg.write_bytes(edited_bytes)

        with pytest.raises(strip.StripConcurrentChangeError):
            strip.safe_replace_file(str(cfg), b"whatever the stale decision computed", original_bytes)

        assert cfg.read_bytes() == edited_bytes  # the concurrent edit survives
        assert not list(tmp_path.glob("*.bak.*"))

    def test_a_concurrent_edit_landing_just_before_the_replace_is_caught(self, tmp_path, monkeypatch):
        # Distinct from the test above: that one lands the race before the
        # backup is even made. This one lets the backup and the verified
        # temp write both complete normally, then lands the edit in the
        # narrow window safe_replace_file's own pre-replace re-check exists
        # for - os.chmod runs right before that re-check, so injecting the
        # write there lands it as late as a monkeypatch can.
        cfg = tmp_path / "printer.cfg"
        original_text = "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK
        write(cfg, original_text)
        original_bytes = cfg.read_bytes()

        real_chmod = os.chmod
        concurrent_edit = original_bytes + b"\n; a concurrent edit landing late\n"

        def chmod_then_race(path, mode, *args, **kwargs):
            if ".helix-tracking-strip-" in str(path):
                cfg.write_bytes(concurrent_edit)
            return real_chmod(path, mode, *args, **kwargs)

        monkeypatch.setattr(os, "chmod", chmod_then_race)

        result = strip.process_file(str(cfg))

        assert result["status"] == "skipped"
        assert "changed" in result["reason"]
        assert cfg.read_bytes() == concurrent_edit  # the concurrent edit survives
        assert len(list(tmp_path.glob("*.bak.*"))) == 1  # made before the race, kept intact

    def test_process_file_reports_a_concurrent_change_as_skipped_not_failed(
        self, tmp_path, monkeypatch
    ):
        # A race narrow enough to land between process_file's own read and
        # safe_replace_file's re-read is exercised directly, at the
        # safe_replace_file level, by the test above; this proves the
        # exception it raises is classified as "skipped", not "failed".
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        def always_raises(path, new_bytes, original):
            raise strip.StripConcurrentChangeError(f"{path} changed on disk since it was read")

        monkeypatch.setattr(strip, "safe_replace_file", always_raises)

        result = strip.process_file(str(cfg))

        assert result["status"] == "skipped"
        assert "changed" in result["reason"]
        assert result["skip_kind"] == "concurrent"

    def test_main_prints_the_concurrent_hint_not_the_anomaly_hint_for_a_race(
        self, tmp_path, monkeypatch, capsys
    ):
        # The test above pins process_file's own skip_kind; this pins that
        # main() actually prints the hint that kind maps to, rather than the
        # anomaly wording - a mislabel between process_file and main() would
        # pass a unit test of either one alone.
        cfg = tmp_path / "printer.cfg"
        write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        def always_raises(path, new_bytes, original):
            raise strip.StripConcurrentChangeError(f"{path} changed on disk since it was read")

        monkeypatch.setattr(strip, "safe_replace_file", always_raises)

        result = strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert result == strip.EXIT_NEEDS_ATTENTION
        assert "re-run the uninstall" in out
        assert "restart Klipper" not in out
        assert "by hand" not in out


# ============================================================================
# Output contract (item 4)
# ============================================================================


class TestOutputContract:
    def test_summary_counts_edited_skipped_and_failed(self, tmp_path, capsys):
        clean = tmp_path / "clean.cfg"
        write(clean, "[printer]\nkinematics: corexy\n")

        edited = tmp_path / "edited.cfg"
        write(edited, "[gcode_macro PRINT_START]\ngcode:\n    G28\n" + VALID_BLOCK)

        anomaly = tmp_path / "anomaly.cfg"
        write(
            anomaly,
            f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n",
        )

        result = strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert result == strip.EXIT_NEEDS_ATTENTION
        assert "edited 1, skipped 1, failed 0" in out
        assert str(edited) in out
        assert str(anomaly) in out
        assert "Klipper restart" in out

    def test_no_edits_prints_no_restart_line(self, tmp_path, capsys):
        write(tmp_path / "clean.cfg", "[printer]\nkinematics: corexy\n")

        strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert "Klipper restart" not in out
        assert "edited 0, skipped 0, failed 0" in out

    def test_missing_config_dir_is_not_a_failure(self, tmp_path):
        assert strip.main([str(tmp_path / "does-not-exist")]) == 0

    def test_skip_output_includes_a_next_step_line(self, tmp_path, capsys):
        anomaly = tmp_path / "anomaly.cfg"
        write(
            anomaly,
            f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n",
        )

        strip.main([str(tmp_path)])
        out = capsys.readouterr().out

        assert "WARN: next step:" in out
        assert "restart Klipper" in out

    def test_a_symlinked_anomalys_hint_names_the_symlinks_own_directory(self, tmp_path, capsys):
        # End-to-end proof that main() wires the discovered path, not the
        # realpath, into the hint - a unit test of _next_step_hint alone
        # cannot see which one main() actually passes it.
        real_dir = tmp_path / "elsewhere"
        real_dir.mkdir()
        real_file = real_dir / "printer.cfg"
        write(
            real_file,
            f"[gcode_macro PRINT_START]\ngcode:\n    G28\n    {BEGIN}\n    HELIX_PHASE_HOMING\n",
        )
        config_dir = tmp_path / "config"
        config_dir.mkdir()
        (config_dir / "printer.cfg").symlink_to(real_file)

        strip.main([str(config_dir)])
        out = capsys.readouterr().out

        assert str(config_dir / "printer.bak.") in out
        assert str(real_dir / "printer.bak.") not in out


class TestNextStepHint:
    def test_anomaly_hint_names_the_marker_text_and_a_backup_pattern(self):
        hint = strip._next_step_hint("/cfg/printer.cfg", "anomaly")
        assert "printer.bak.<a number>" in hint
        assert "restart Klipper" in hint
        assert "discards every config change" in hint

    def test_anomaly_hint_uses_the_path_it_was_given_not_a_realpath(self):
        # A symlinked cfg's pre-1.1 backup would sit beside the symlink
        # itself, not beside whatever it resolves to - main() passes the
        # discovered path in for exactly this reason, and the hint must
        # build the backup name from it rather than re-deriving one.
        hint = strip._next_step_hint("/config/printer.cfg", "anomaly")
        assert "/config/printer.bak.<a number>" in hint
        assert "/repo/" not in hint

    def test_concurrent_hint_says_to_rerun_not_to_edit_or_restore(self):
        hint = strip._next_step_hint("/cfg/printer.cfg", "concurrent")
        assert "re-run" in hint
        assert "by hand" not in hint
        assert "restore" not in hint


# ============================================================================
# Cross-check against the vendored pre-1.1 writer's own disable logic
# ============================================================================
#
# fixtures/pre_1_1_instrumentation.py is that writer's marker format and
# body-splicing logic, taken as-is with the Moonraker component plumbing
# trimmed away - not a git lookup, so this runs the same way everywhere,
# including with no git history at all.


def _write_helix_macros_cfg(config_dir):
    # The old enable refuses to instrument PRINT_START unless it can already
    # see the HELIX_PHASE_* macros defined somewhere in config_dir. A printer it
    # instrumented has the pack under its visible names, as
    # MacroManager::with_visible_signal_names produces it.
    repo_root = Path(__file__).resolve().parents[2]
    macros_cfg = repo_root / "assets" / "config" / "helix_macros.cfg"
    visible = re.sub(rb"_(HELIX_(?:READY|ENDED|RESET|PHASE_))", rb"\1", macros_cfg.read_bytes())
    (config_dir / "helix_macros.cfg").write_bytes(visible)


def test_matches_the_old_writers_own_disable_output(tmp_path):
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    BED_MESH_CALIBRATE\n"
        "    M109 S{EXTRUDER_TEMP}\n"
        "    CLEAN_NOZZLE\n",
    )

    enable_result = old_writer.enable(tmp_path)
    assert enable_result["success"] is True

    old_disable_input = cfg.read_bytes()
    disable_result = old_writer.disable(tmp_path)
    assert disable_result["success"] is True
    old_disable_output = cfg.read_bytes()

    # Reset to the instrumented state and run OUR strip over it instead.
    cfg.write_bytes(old_disable_input)
    assert strip.main([str(tmp_path)]) == 0

    assert cfg.read_bytes() == old_disable_output


def test_a_non_utf8_sibling_cfg_is_skipped_like_the_real_writers(tmp_path):
    # Both real writers catch every exception while scanning the config
    # tree, not just OSError, so a sibling file that fails to decode does
    # not stop either one from finding PRINT_START in a later file. A name
    # sorting before "printer.cfg" puts it first in the scan.
    _write_helix_macros_cfg(tmp_path)
    (tmp_path / "aaa_notes.cfg").write_bytes(b"\xb0 not valid utf-8\n")
    cfg = tmp_path / "printer.cfg"
    write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n")

    enable_result = old_writer.enable(tmp_path)
    assert enable_result["success"] is True
    assert old_writer.TRACKING_MARKER_BEGIN in cfg.read_text()

    disable_result = old_writer.disable(tmp_path)
    assert disable_result["success"] is True
    assert old_writer.TRACKING_MARKER_BEGIN not in cfg.read_text()


def test_a_read_only_printer_cfg_fails_like_the_real_v2_writer(tmp_path):
    # The real v2 _update_macro wraps its backup-plus-write step in
    # try/except and returns success: False rather than raising - the
    # fixture's docstring claims to be that logic "taken as-is", so it must
    # match this shape for a write failure too, not just for the content it
    # produces on the happy path.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n")
    cfg.chmod(0o444)

    try:
        result = old_writer.enable(tmp_path)
    finally:
        cfg.chmod(0o644)  # tmp_path cleanup needs write access

    assert result["success"] is False
    assert len(list(tmp_path.glob("printer.bak.*"))) == 1


def test_a_read_only_printer_cfg_fails_like_the_real_legacy_writer(tmp_path):
    # Same shape, for the pre-#1268 legacy writer: its per-file try wraps the
    # backup and write too, and moves on rather than raising.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n")
    cfg.chmod(0o444)

    try:
        result = old_writer.enable_legacy(tmp_path)
    finally:
        cfg.chmod(0o644)

    assert result["success"] is False


def test_matches_the_old_writer_with_no_git_available(tmp_path, monkeypatch):
    # The oracle above must not depend on git being reachable at all - there
    # is no subprocess/git call left in this file to disable, so this proves
    # it by removing every git from PATH and repeating the same check.
    monkeypatch.setenv("PATH", str(tmp_path / "empty-path-for-this-test"))
    (tmp_path / "empty-path-for-this-test").mkdir()
    assert shutil.which("git") is None

    config_dir = tmp_path / "config"
    config_dir.mkdir()
    _write_helix_macros_cfg(config_dir)
    cfg = config_dir / "printer.cfg"
    write(cfg, "[gcode_macro PRINT_START]\ngcode:\n    G28\n    M109 S{EXTRUDER_TEMP}\n")

    assert old_writer.enable(config_dir)["success"] is True
    old_disable_input = cfg.read_bytes()
    assert old_writer.disable(config_dir)["success"] is True
    old_disable_output = cfg.read_bytes()

    cfg.write_bytes(old_disable_input)
    assert strip.main([str(config_dir)]) == 0
    assert cfg.read_bytes() == old_disable_output


def test_correctly_strips_a_pre_1268_legacy_instrumented_body(tmp_path):
    # The legacy writer deepens the WHOLE body by four spaces on every
    # write, enable or disable, independently of what strip_phase_tracking.py
    # does - so byte-equivalence to that writer's own disable is not the
    # right check here (its disable would re-deepen the body again, which
    # this script correctly never does). What must hold is that every valid
    # marker block a real pre-#1268 install wrote is still recognized and
    # removed, and every substantive line survives.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    BED_MESH_CALIBRATE\n"
        "    M109 S{EXTRUDER_TEMP}\n"
        "    CLEAN_NOZZLE\n",
    )

    assert old_writer.enable_legacy(tmp_path)["success"] is True
    instrumented = cfg.read_text()
    assert old_writer.TRACKING_MARKER_BEGIN in instrumented

    assert strip.main([str(tmp_path)]) == 0
    stripped = cfg.read_text()

    assert old_writer.TRACKING_MARKER_BEGIN not in stripped
    assert old_writer.TRACKING_MARKER_END not in stripped
    assert "HELIX_PHASE_" not in stripped
    assert "HELIX_READY" not in stripped
    for original_line in (
        "G28",
        "QUAD_GANTRY_LEVEL",
        "BED_MESH_CALIBRATE",
        "M109 S{EXTRUDER_TEMP}",
        "CLEAN_NOZZLE",
    ):
        assert original_line in stripped


def test_a_legacy_body_with_a_blank_line_still_strips_leaving_its_duplicated_tail(tmp_path):
    # The legacy writer's write side only replaces the body up to its first
    # blank line, so a blank-line body gets a duplicated, un-instrumented
    # tail after that line - a pre-existing writer bug, unrelated to this
    # strip. Every block the writer did inject sits inside the same
    # [gcode_macro ...] body span as the duplicated tail (nothing else
    # follows in this file), so they are still well-formed and get removed;
    # the duplicate itself was never marker-delimited, so it survives too.
    _write_helix_macros_cfg(tmp_path)
    cfg = tmp_path / "printer.cfg"
    write(
        cfg,
        "[gcode_macro PRINT_START]\n"
        "gcode:\n"
        "    G28\n"
        "\n"
        "    QUAD_GANTRY_LEVEL\n"
        "    M109 S{EXTRUDER_TEMP}\n",
    )

    assert old_writer.enable_legacy(tmp_path)["success"] is True
    assert old_writer.TRACKING_MARKER_BEGIN in cfg.read_text()

    assert strip.main([str(tmp_path)]) == 0
    stripped = cfg.read_text()

    assert old_writer.TRACKING_MARKER_BEGIN not in stripped
    assert "HELIX_PHASE_" not in stripped
    assert "HELIX_READY" not in stripped
    # The duplicated, un-instrumented tail the legacy writer left behind
    # is untouched: QUAD_GANTRY_LEVEL and M109 each still appear twice.
    assert stripped.count("QUAD_GANTRY_LEVEL") == 2
    assert stripped.count("M109 S{EXTRUDER_TEMP}") == 2
