# SPDX-License-Identifier: GPL-3.0-or-later
"""
Server-side G-code edits: the HelixPrint plugin's `server_edits` feature.

`server.helix.print_edited` takes the edit list the screen would otherwise
apply itself (download, rewrite, upload) and applies it here, disk to disk,
then starts the result through the same linked-print path as print_modified.
The screen's disk and network then carry a few hundred bytes instead of the
whole job twice.

The edit grammar has two implementations, this one and the app's
(src/rendering/gcode_edit_list.cpp over GCodeFileModifier::apply_streaming and
GcodeToolRemapper::apply_to_file). The fixtures in tests/fixtures/gcode_edits/
pin them together: both suites apply every case and compare byte for byte.

One edit is {"line_number", "expect_raw_line", "action", "args"}:

    comment_out    args {"comment"?, "end_line"?}
    delete         args {"end_line"?}
    inject_before  args {"gcode"}
    inject_after   args {"gcode"}
    replace        args {"gcode", "comment"?}
    tool_remap     args {"map": {"<logical>": <physical>, ...}}, line_number 0;
                   the only edit in its list

A non-empty expect_raw_line must match the file's line (surrounding whitespace
ignored) or the whole list is refused: the edits were made against other
G-code, and their line numbers mean nothing in this file.

The feature defaults on. Phase 0 keeps anything that acts on a print off until
the user opts in, but this acts on nothing by itself: it is a transport for a
print the user started from the screen, with the same edits the screen applies
without the plugin. Only the place the rewrite runs changes.

The per-line pass runs in a worker process (main(), this file run as a
script, at the lowest CPU and I/O priority), so its Python work never holds
the GIL Moonraker's event loop needs.

Everything above HelixEdits is pure and touches no Moonraker object. Python
3.8, stdlib only.
"""

from __future__ import annotations

import heapq
import json
import logging
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, BinaryIO, Dict, List, NamedTuple, Optional, Tuple

import helix_gcode_reader as reader

SERVER_EDITS_FEATURE = "server_edits"
SERVER_EDITS_FEATURE_VERSION = 1

ACTIONS = (
    "comment_out", "delete", "inject_before", "inject_after", "replace", "tool_remap",
)

# Each action's args: (required, optional)
_ARG_KEYS = {
    "comment_out": ((), ("comment", "end_line")),
    "delete": ((), ("end_line",)),
    "inject_before": (("gcode",), ()),
    "inject_after": (("gcode",), ()),
    "replace": (("gcode",), ("comment",)),
    "tool_remap": (("map",), ()),
}

# tool_index_for_line ignores a tool number above this
_MAX_TOOL_INDEX = 100000

_COPY_CHUNK = 1024 * 1024

# The app's own bounds: a line number past INT32_MAX is no line it sends.
MAX_LINE_NUMBER = 2147483647
# Far above any list the app builds; it bounds the plan, never a file.
MAX_EDITS = 10000


class EditError(ValueError):
    """The edit list is malformed."""


class EditRefused(Exception):
    """A line differs from the line its edit expects."""

    def __init__(self, line_number: int) -> None:
        super().__init__(f"line {line_number} is not the line the scan found there")
        self.line_number = line_number


class Edit(NamedTuple):
    action: str
    expect: bytes
    comment: bytes
    gcode: bytes


class Span(NamedTuple):
    """Lines `first` to `last`, inclusive, all rewritten by `edit`."""

    first: int
    last: int
    edit: Edit


class EditPlan(NamedTuple):
    """A parsed edit list: positional edits as sorted, disjoint spans of
    lines, or a tool remap."""

    spans: List[Span]
    remap: Optional[Dict[int, int]]

    @property
    def last_line(self) -> int:
        return self.spans[-1].last if self.spans else 0


def resolve_spans(ranges: List[Tuple[int, int, Edit]]) -> List[Span]:
    """Disjoint spans, sorted, in which each line takes the last range in
    list order that covers it. The cost follows the number of ranges, never
    their length."""
    bounds = sorted({r[0] for r in ranges} | {r[1] + 1 for r in ranges})
    by_start = sorted(range(len(ranges)), key=lambda i: ranges[i][0])
    active: List[Tuple[int, int]] = []  # (-list index, last line)
    spans: List[Span] = []
    k = 0
    for first, after in zip(bounds, bounds[1:]):
        while k < len(by_start) and ranges[by_start[k]][0] <= first:
            i = by_start[k]
            heapq.heappush(active, (-i, ranges[i][1]))
            k += 1
        while active and active[0][1] < first:
            heapq.heappop(active)
        if not active:
            continue
        edit = ranges[-active[0][0]][2]
        if spans and spans[-1].edit is edit and spans[-1].last + 1 == first:
            spans[-1] = Span(spans[-1].first, after - 1, edit)
        else:
            spans.append(Span(first, after - 1, edit))
    return spans


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _text(args: Dict[str, Any], key: str, index: int) -> bytes:
    value = args.get(key, "")
    if not isinstance(value, str):
        raise EditError(f"edit {index}: {key} must be a string")
    return value.encode("utf-8")


def _parse_map(value: Any, index: int) -> Dict[int, int]:
    if not isinstance(value, dict) or not value:
        raise EditError(f"edit {index}: map must be a non-empty object")
    remap: Dict[int, int] = {}
    for key, physical in value.items():
        if not (isinstance(key, str) and key.isdigit() and key.isascii()):
            raise EditError(f"edit {index}: map key {key!r} is not a tool number")
        if not _is_int(physical) or physical < 0:
            raise EditError(f"edit {index}: map value {physical!r} is not a tool number")
        remap[int(key)] = physical
    return remap


def parse_edits(edits: Any) -> EditPlan:
    """The plan for an edit list, or EditError naming what is wrong with it.

    A range edit checks its own expect_raw_line against every line it
    covers, and a later edit for a line replaces an earlier one, as
    GCodeFileModifier's streaming lookup does. Ranges stay ranges, so what a
    plan costs follows the edit count, which MAX_EDITS bounds.
    """
    if not isinstance(edits, list):
        raise EditError("edits must be a list")
    if len(edits) > MAX_EDITS:
        raise EditError(f"at most {MAX_EDITS} edits")
    ranges: List[Tuple[int, int, Edit]] = []
    remap: Optional[Dict[int, int]] = None
    for index, edit in enumerate(edits):
        if not isinstance(edit, dict):
            raise EditError(f"edit {index} is not an object")
        action = edit.get("action")
        if action not in _ARG_KEYS:
            raise EditError(f"edit {index}: unknown action {action!r}")
        args = edit.get("args", {})
        if not isinstance(args, dict):
            raise EditError(f"edit {index}: args must be an object")
        required, optional = _ARG_KEYS[action]
        for key in required:
            if key not in args:
                raise EditError(f"edit {index}: {action} needs {key}")
        for key in args:
            if key not in required and key not in optional:
                raise EditError(f"edit {index}: {action} takes no {key}")
        line_number = edit.get("line_number")
        expect = edit.get("expect_raw_line", "")
        if not isinstance(expect, str):
            raise EditError(f"edit {index}: expect_raw_line must be a string")

        if action == "tool_remap":
            if len(edits) != 1:
                raise EditError("tool_remap must be the only edit in its list")
            if not _is_int(line_number) or line_number != 0 or expect:
                raise EditError("tool_remap applies to the whole file: line_number 0, no expect")
            remap = _parse_map(args["map"], index)
            continue

        if not _is_int(line_number) or not 1 <= line_number <= MAX_LINE_NUMBER:
            raise EditError(f"edit {index}: line_number must be in 1..{MAX_LINE_NUMBER}")
        end_line = args.get("end_line", line_number)
        if not _is_int(end_line) or not line_number <= end_line <= MAX_LINE_NUMBER:
            raise EditError(f"edit {index}: end_line must be in line_number..{MAX_LINE_NUMBER}")
        if action not in ("comment_out", "delete"):
            end_line = line_number
        parsed = Edit(action, expect.encode("utf-8"), _text(args, "comment", index),
                      _text(args, "gcode", index))
        ranges.append((line_number, end_line, parsed))
    return EditPlan(resolve_spans(ranges), remap)


# ============================================================================
# Line rules, byte for byte as the app's
# ============================================================================

_SPACE = frozenset(b" \t\n\r\x0b\x0c")
_DIGITS = frozenset(b"0123456789")


def same_line(a: bytes, b: bytes) -> bool:
    """`a` and `b` with surrounding whitespace (and a CR) ignored."""
    return a.strip() == b.strip()


def comment_out_line(line: bytes, reason: bytes) -> bytes:
    out = b"; " + line
    if reason:
        out += b"  ; [HelixScreen: " + reason + b"]"
    return out


def text_lines(text: bytes) -> List[bytes]:
    """`text` split as std::getline splits it: "" -> [], "a\\n" -> [a]."""
    parts = text.split(b"\n")
    if parts[-1] == b"":
        parts.pop()
    return parts


def _parse_uint(line: bytes, pos: int) -> Tuple[int, int]:
    end = pos
    while end < len(line) and line[end] in _DIGITS:
        end += 1
    return int(line[pos:end]), end


def _is_macro_param(word: bytes) -> bool:
    eq = word.find(b"=")
    if eq <= 0 or eq + 1 == len(word):
        return False
    return all(c in _DIGITS or (65 <= c <= 90) or (97 <= c <= 122) or c == 95
               for c in word[:eq])


def tool_index_for_line(raw: bytes) -> Optional[Tuple[int, int, int]]:
    """(tool, digits_begin, digits_end) when `raw` is a standalone tool change,
    as helix::gcode::tool_index_for_line reads one, else None."""
    end = raw.find(b";")
    if end < 0:
        end = len(raw)
    start = 0
    while start < end and raw[start] in _SPACE:
        start += 1
    while end > start and raw[end - 1] in _SPACE:
        end -= 1
    sv = raw[start:end]
    if len(sv) < 2 or sv[0] != ord("T"):
        return None
    value = 0
    number_end = 1
    while number_end < len(sv) and sv[number_end] in _DIGITS:
        value = value * 10 + (sv[number_end] - 48)
        if value > _MAX_TOOL_INDEX:
            return None
        number_end += 1
    if number_end == 1:
        return None
    pos = number_end
    if pos < len(sv) and sv[pos] not in _SPACE:
        return None
    while pos < len(sv):
        while pos < len(sv) and sv[pos] in _SPACE:
            pos += 1
        word_begin = pos
        while pos < len(sv) and sv[pos] not in _SPACE:
            pos += 1
        if not _is_macro_param(sv[word_begin:pos]):
            return None
    return value, start + 1, start + number_end


_PRESTART_PREFIXES = (
    b"SM_PRINT_AUTO_FEED EXTRUDER=",
    b"SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=",
    b"SM_PRINT_FLOW_CALIBRATE EXTRUDER=",
)

_TOOL_PARAM_KEYS = (b"INITIAL_TOOL", b"TOOL")


def _remap_bare_toolchange(line: bytes, remap: Dict[int, int]) -> Optional[bytes]:
    found = tool_index_for_line(line)
    if found is None:
        return None
    idx, begin, end = found
    m = remap.get(idx, idx)
    if m == idx:
        return None
    return line[:begin] + str(m).encode() + line[end:]


def _remap_prestart(line: bytes, remap: Dict[int, int]) -> Optional[bytes]:
    for prefix in _PRESTART_PREFIXES:
        if not line.startswith(prefix):
            continue
        n = len(prefix)
        if n >= len(line) or line[n] not in _DIGITS:
            return None
        idx, pos = _parse_uint(line, n)
        m = remap.get(idx, idx)
        if m == idx:
            return None
        return line[:n] + str(m).encode() + line[pos:]
    return None


def _remap_temp(line: bytes, remap: Dict[int, int]) -> Optional[bytes]:
    if not (line.startswith(b"M104") or line.startswith(b"M109")):
        return None
    scan_end = line.find(b";")
    if scan_end < 0:
        scan_end = len(line)
    for i in range(scan_end - 1):
        if line[i] != ord("T"):
            continue
        if i == 0 or line[i - 1] not in _SPACE:
            continue
        if line[i + 1] not in _DIGITS:
            continue
        idx, pos = _parse_uint(line, i + 1)
        m = remap.get(idx, idx)
        if m == idx:
            return None
        return line[: i + 1] + str(m).encode() + line[pos:]
    return None


def _remap_tool_params(line: bytes, remap: Dict[int, int]) -> Optional[bytes]:
    if not line or line[0] == ord(";"):
        return None
    spaces = [p for p in (line.find(b" "), line.find(b"\t")) if p >= 0]
    if not spaces:
        return None
    first_space = min(spaces)
    end = line.find(b";", first_space)
    if end < 0:
        end = len(line)
    out: List[bytes] = []
    copied = 0
    i = first_space
    while i < end:
        while i < end and line[i] in _SPACE:
            i += 1
        token_begin = i
        while i < end and line[i] not in _SPACE:
            i += 1
        token = line[token_begin:i]
        eq = token.find(b"=")
        if eq <= 0:
            continue
        key, value = token[:eq], token[eq + 1:]
        if key.upper() not in _TOOL_PARAM_KEYS or not value or not all(c in _DIGITS for c in value):
            continue
        value_begin = token_begin + eq + 1
        idx, pos = _parse_uint(line, value_begin)
        m = remap.get(idx, idx)
        if m == idx:
            continue
        out.append(line[copied:value_begin])
        out.append(str(m).encode())
        copied = pos
    if copied == 0:
        return None
    out.append(line[copied:])
    return b"".join(out)


def remap_line(line: bytes, remap: Dict[int, int]) -> bytes:
    """`line` with its tool numbers mapped, as GcodeToolRemapper rewrites one:
    a bare tool change, a Snapmaker prestart EXTRUDER=, an M104/M109 T word,
    or an INITIAL_TOOL= / TOOL= parameter, the first that applies."""
    for rule in (_remap_bare_toolchange, _remap_prestart, _remap_temp, _remap_tool_params):
        out = rule(line, remap)
        if out is not None:
            return out
    return line


# ============================================================================
# Applying a plan
# ============================================================================

class _Writer:
    """Emits records the way the app's rewriters do: a newline before every
    record but the first, and a final one only where the source had one."""

    def __init__(self, dst: BinaryIO) -> None:
        self.dst = dst
        self.wrote = False

    def emit(self, record: bytes) -> None:
        if self.wrote:
            self.dst.write(b"\n")
        self.dst.write(record)
        self.wrote = True


def _edited(writer: _Writer, line: bytes, edit: Edit) -> int:
    """Writes `line` as `edit` rewrites it; the number of lines it changed."""
    if edit.action == "comment_out":
        if line.startswith(b";"):
            writer.emit(line)
            return 0
        writer.emit(comment_out_line(line, edit.comment))
        return 1
    if edit.action == "delete":
        return 1
    if edit.action == "inject_before":
        for added in text_lines(edit.gcode):
            writer.emit(added)
        writer.emit(line)
        return 1
    if edit.action == "inject_after":
        writer.emit(line)
        for added in text_lines(edit.gcode):
            writer.emit(added)
        return 1
    for added in text_lines(edit.gcode):  # replace
        writer.emit(added)
    return 1


def apply_plan(src: BinaryIO, dst: BinaryIO, plan: EditPlan) -> int:
    """Writes `src` with `plan` applied to `dst`; the number of lines changed.

    Raises EditRefused, having written only part of `dst`, when a line differs
    from the one its edit expects. Past the last edited line the rest of the
    file is copied as it is.
    """
    writer = _Writer(dst)
    changed = 0
    last_line = plan.last_line
    spans = plan.spans
    span = 0
    line_number = 0
    ended_with_newline = False
    for raw in iter(src.readline, b""):
        line_number += 1
        ended_with_newline = raw.endswith(b"\n")
        line = raw[:-1] if ended_with_newline else raw
        if plan.remap is not None:
            out = remap_line(line, plan.remap)
            if out != line:
                changed += 1
            writer.emit(out)
            continue
        while span < len(spans) and spans[span].last < line_number:
            span += 1
        edit = None
        if span < len(spans) and spans[span].first <= line_number:
            edit = spans[span].edit
        if edit is None:
            writer.emit(line)
        else:
            if edit.expect and not same_line(line, edit.expect):
                raise EditRefused(line_number)
            changed += _edited(writer, line, edit)
        if line_number >= last_line and ended_with_newline:
            # Nothing past here changes: the remaining records follow verbatim.
            rest = src.read(1)
            if not rest:
                break
            if writer.wrote:
                dst.write(b"\n")
            dst.write(rest)
            shutil.copyfileobj(src, dst, _COPY_CHUNK)
            return changed
    if writer.wrote and ended_with_newline:
        dst.write(b"\n")
    return changed


def apply_file(src_path: str, dst_path: str, plan: EditPlan) -> int:
    """apply_plan from file to file. On any failure `dst_path` is removed."""
    try:
        with open(src_path, "rb") as src, open(dst_path, "wb") as dst:
            changed = apply_plan(src, dst, plan)
            dst.flush()
            os.fsync(dst.fileno())
        return changed
    except BaseException:
        try:
            os.unlink(dst_path)
        except OSError:
            pass
        raise


# ============================================================================
# The worker process
# ============================================================================

WorkerFailed = reader.WorkerFailed


def worker_command(src_path: str, dst_path: str) -> List[str]:
    """The worker's argv: this file as a script (main())."""
    return reader.worker_argv(__file__, [src_path, dst_path])


def encode_worker_output(changed: Optional[int], refused: Optional[int]) -> bytes:
    if refused is not None:
        return json.dumps({"refused": refused}).encode("utf-8")
    return json.dumps({"changed": changed}).encode("utf-8")


def decode_worker_output(data: bytes) -> int:
    """The worker's stdout as the number of lines changed. Raises EditRefused
    for a refusal, WorkerFailed for anything else."""
    try:
        doc = json.loads(data.decode("utf-8"))
    except ValueError as e:
        raise WorkerFailed("unreadable worker output: %s" % e)
    if isinstance(doc, dict) and _is_int(doc.get("refused")):
        raise EditRefused(doc["refused"])
    if isinstance(doc, dict) and _is_int(doc.get("changed")):
        return doc["changed"]
    raise WorkerFailed("unreadable worker output")


def main(argv: Optional[List[str]] = None) -> int:
    """Worker: apply the edit list on stdin to argv[1], writing argv[2]; the
    outcome JSON on stdout. A refusal leaves argv[2] removed."""
    args = sys.argv[1:] if argv is None else argv
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    if len(args) != 2:
        sys.stderr.write("usage: helix_edits.py SOURCE DEST < edits.json\n")
        return 2
    reader.worker_nice()
    plan = parse_edits(json.loads(sys.stdin.read()))
    try:
        out = encode_worker_output(apply_file(args[0], args[1], plan), None)
    except EditRefused as e:
        out = encode_worker_output(None, e.line_number)
    out_stream = getattr(sys.stdout, "buffer", sys.stdout)
    out_stream.write(out)
    out_stream.flush()
    return 0


def default_output_name(temp_dir: str, original_filename: str, now: float) -> str:
    """Where an edited copy goes when the caller names no path: the staging
    shape the app gives a bare filename."""
    return f"{temp_dir}/modified_{int(now)}_{Path(original_filename).name}"


# ============================================================================
# Moonraker component
# ============================================================================

class HelixEdits:
    """print_edited inside a running Moonraker. helix_print.py creates one in
    its component_init and starts the edited copy through its linked-print
    path."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server

    def start(self) -> None:
        self.server.register_endpoint(
            "/server/helix/print_edited", ["POST"], self.handle_print_edited
        )

    async def handle_print_edited(self, web_request: Any) -> Dict[str, Any]:
        helix = self.helix
        if not await helix.feature_enabled(SERVER_EDITS_FEATURE):
            raise self.server.error("Server-side edits are unavailable", 503)
        if helix.gc_path is None:
            raise self.server.error("File manager not initialized", 500)

        original_filename = web_request.get_str("original_filename")
        temp_file_path = web_request.get_str("temp_file_path", "")
        edits = web_request.get_list("edits", [])
        modifications = web_request.get_list("modifications", [])
        tools = helix.tools_param(web_request)

        try:
            parse_edits(edits)
        except EditError as e:
            raise self.server.error(f"Invalid edit list: {e}", 400)

        original = helix.resolve_original(original_filename)
        if not temp_file_path:
            temp_file_path = default_output_name(helix.temp_dir, original_filename, time.time())
        output = helix.resolve_temp(temp_file_path, original)
        if output.exists():
            raise self.server.error(f"{temp_file_path} already exists", 409)
        output.parent.mkdir(parents=True, exist_ok=True)
        helix.check_free_space(original.stat().st_size)

        started = time.monotonic()
        changed = await self._rewrite(original, output, edits, original_filename)
        logging.info(
            "HelixPrint: Edited %s -> %s (%d edits, %d lines changed, %.1fs)",
            original_filename, temp_file_path, len(edits), changed,
            time.monotonic() - started,
        )

        result = await helix.start_linked_print(
            original_filename, temp_file_path, modifications, True, "print_edited",
            tools=tools,
        )
        result["lines_changed"] = changed
        return result

    async def _rewrite(self, original: Path, output: Path, edits: List[Any],
                       original_filename: str) -> int:
        """Writes `original` with `edits` applied to `output` through a worker
        process; the number of lines changed. Nothing is left at `output` or
        beside it unless the whole rewrite succeeded."""
        try:
            fd, partial = tempfile.mkstemp(dir=str(output.parent), prefix=output.name + ".",
                                           suffix=".part")
            os.close(fd)
        except OSError as e:
            raise self.server.error(f"Failed to write the edited copy: {e}", 500)
        try:
            out = await reader.WorkerProcess().run(
                worker_command(str(original), partial), json.dumps(edits).encode("utf-8"),
                reader.worker_timeout(original.stat().st_size))
            changed = decode_worker_output(out)
            # mkstemp makes the partial 0600; the copy reads like its original.
            os.chmod(partial, original.stat().st_mode & 0o666 or 0o644)
            reader.atomic_publish(partial, str(output), exclusive=True)
            return changed
        except EditRefused as e:
            raise self.server.error(f"Refusing to edit {original_filename}: {e}", 409)
        except FileExistsError:
            raise self.server.error(f"{output.name} already exists", 409)
        except (WorkerFailed, reader.WorkerStopped, OSError) as e:
            raise self.server.error(f"Failed to write the edited copy: {e}", 500)
        finally:
            try:
                os.unlink(partial)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
