#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn an ESP32 firmware console capture back into readable log text.

Usage: detokenize_log.py [--level] <db.json> [serial.log ...]   (stdin when no file)

The firmware prints each info/warn/error/critical log call as a `$` + base64
record (firmware/.../shim/log_token.h); every other console line (ESP-IDF boot
output, the serial snapshot protocol, printf) passes through untouched. The
database is the build's `helixscreen_esp32.logtok.json`
(scripts/esp32_logtok_db.py), shipped in the release zip as
`helixscreen-esp32-ktouch-logtok.json`. A `$HLT1:<sha256>` header line names the
build that printed the records after it (the firmware prints the first nine hex
digits, as its boot banner's `ELF file SHA256` does); one that does not match
the database gets a warning on stderr, since its tokens point at different
strings. A `$word` that does not decode to a record whose token the database
holds is left as it was, so `$HOME`-like text passes through.

Formatting follows fmt: `{}`, `{N}`, `{{`/`}}`, nested width/precision, and the
spec mini-language Python shares with it, plus fmt's own rules for a bare `{}`
(shortest float with no trailing `.0`, `true`/`false`). A value it cannot
format prints as `{?value}`, a missing one as `{?}`.
"""

import argparse
import base64
import json
import re
import struct
import sys

HEADER_RE = re.compile(r"\$HLT1:([0-9a-fA-F]{8,64})")
RECORD_RE = re.compile(r"(?<!\S)\$([A-Za-z0-9+/]{2,})(?!\S)")

TRUNCATED_FLAG = 0x80
LEVEL_NAMES = ["T", "D", "I", "W", "E", "C", "O"]

TAG_INT = 1
TAG_UINT = 2
TAG_DOUBLE = 3
TAG_FLOAT = 4
TAG_BOOL = 5
TAG_CHAR = 6
TAG_STRING = 7
TAG_STRING_CUT = 8
TAG_POINTER = 9
TAG_UNSUPPORTED = 10


class Value:
    """One decoded argument: its kind ('int', 'float', 'float32', 'bool',
    'char', 'str', 'ptr', 'unsupported') and value."""

    def __init__(self, kind, value=None):
        self.kind = kind
        self.value = value

    def __repr__(self):
        return f"Value({self.kind!r}, {self.value!r})"


class DecodeError(ValueError):
    pass


def _uleb(buf, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise DecodeError("truncated varint")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise DecodeError("varint too long")


def decode_record(payload: bytes):
    """(level, truncated, token, [Value]) from a record's bytes."""
    if not payload:
        raise DecodeError("empty record")
    level = payload[0] & 0x7F
    truncated = bool(payload[0] & TRUNCATED_FLAG)
    token, pos = _uleb(payload, 1)
    args = []
    while pos < len(payload):
        tag = payload[pos]
        pos += 1
        if tag == TAG_INT:
            z, pos = _uleb(payload, pos)
            args.append(Value("int", (z >> 1) ^ -(z & 1)))
        elif tag in (TAG_UINT, TAG_POINTER):
            u, pos = _uleb(payload, pos)
            args.append(Value("int" if tag == TAG_UINT else "ptr", u))
        elif tag == TAG_DOUBLE:
            if pos + 8 > len(payload):
                raise DecodeError("truncated double")
            args.append(Value("float", struct.unpack_from("<d", payload, pos)[0]))
            pos += 8
        elif tag == TAG_FLOAT:
            if pos + 4 > len(payload):
                raise DecodeError("truncated float")
            args.append(Value("float32", struct.unpack_from("<f", payload, pos)[0]))
            pos += 4
        elif tag in (TAG_BOOL, TAG_CHAR):
            if pos >= len(payload):
                raise DecodeError("truncated byte")
            b = payload[pos]
            pos += 1
            args.append(Value("bool", bool(b)) if tag == TAG_BOOL else Value("char", chr(b)))
        elif tag in (TAG_STRING, TAG_STRING_CUT):
            n, pos = _uleb(payload, pos)
            if pos + n > len(payload):
                raise DecodeError("truncated string")
            args.append(Value("str", payload[pos:pos + n].decode("utf-8", "replace")))
            pos += n
        elif tag == TAG_UNSUPPORTED:
            args.append(Value("unsupported"))
        else:
            raise DecodeError(f"unknown argument tag {tag}")
    return level, truncated, token, args


def b64decode_unpadded(text: str) -> bytes:
    return base64.b64decode(text + "=" * (-len(text) % 4), validate=True)


# --- fmt formatting ---------------------------------------------------------

_SPEC_RE = re.compile(
    r"^(?:(?P<fill>.)?(?P<align>[<>^]))?(?P<sign>[-+ ])?(?P<alt>#)?(?P<zero>0)?"
    r"(?P<width>\d+)?(?:\.(?P<precision>\d+))?(?P<locale>L)?(?P<type>[a-zA-Z?])?$",
    re.DOTALL,
)


def _shortest_float32(v: float) -> str:
    """The shortest decimal that reads back as the same float32, as fmt prints it."""
    target = struct.pack("<f", v)
    for digits in range(1, 10):
        s = f"{v:.{digits}g}"
        if struct.pack("<f", float(s)) == target:
            return s
    return repr(v)


def _fmt_default_float(v: float, is32: bool) -> str:
    """fmt's `{}` for a float: shortest round trip, exponent past 1e16 or below
    1e-4, never a trailing `.0`."""
    if v != v:
        return "nan"
    if v in (float("inf"), float("-inf")):
        return "inf" if v > 0 else "-inf"
    s = _shortest_float32(v) if is32 else repr(v)
    if "e" in s or "E" in s:
        mant, exp = s.split("e")
        sign = "-" if exp.startswith("-") else "+"
        exp = exp.lstrip("+-").lstrip("0") or "0"
        mant = mant[:-2] if mant.endswith(".0") else mant
        return f"{mant}e{sign}{exp.zfill(2)}"
    if s.endswith(".0"):
        s = s[:-2]
    return s


def _pad(text: str, m, default_align: str) -> str:
    width = int(m.group("width") or 0)
    if m.group("zero") and not m.group("align"):
        sign = text[0] if text[:1] in "+- " and len(text) > 1 else ""
        body = text[len(sign):]
        prefix = ""
        if body[:2].lower() in ("0x", "0b"):
            prefix, body = body[:2], body[2:]
        return sign + prefix + body.rjust(width - len(sign) - len(prefix), "0")
    align = m.group("align") or default_align
    fill = m.group("fill") or " "
    if len(text) >= width:
        return text
    gap = width - len(text)
    if align == "<":
        return text + fill * gap
    if align == ">":
        return fill * gap + text
    left = gap // 2
    return fill * left + text + fill * (gap - left)


def format_value(value: Value, spec: str) -> str:
    """One replacement field, the way fmt would format it."""
    m = _SPEC_RE.match(spec)
    if m is None:
        raise ValueError(f"bad spec {spec!r}")
    ftype = m.group("type") or ""
    precision = m.group("precision")
    kind = value.kind
    v = value.value

    if kind == "unsupported":
        raise ValueError("unsupported argument")
    if kind == "bool" and ftype in ("", "s"):
        return _pad("true" if v else "false", m, "<")
    if kind == "bool":
        kind, v = "int", int(v)
    if kind == "char" and ftype in ("", "c"):
        return _pad(v, m, "<")
    if kind == "char":
        kind, v = "int", ord(v)
    if kind == "ptr":
        if ftype not in ("", "p"):
            raise ValueError("pointer spec")
        return _pad(f"0x{v:x}", m, ">")
    if kind == "str":
        if ftype not in ("", "s"):
            raise ValueError("string spec")
        text = v if precision is None else v[: int(precision)]
        return _pad(text, m, "<")
    if kind == "int":
        if precision is not None:
            raise ValueError("precision on an integer")
        py = spec.replace("L", "")
        return format(v, py)
    if kind in ("float", "float32"):
        is32 = kind == "float32"
        if ftype == "" and precision is None:
            text = _fmt_default_float(v, is32)
            sign = m.group("sign")
            if sign in ("+", " ") and not text.startswith("-"):
                text = sign + text
            return _pad(text, m, ">")
        if ftype == "":
            ftype = "g"
            # fmt's general format with precision keeps no trailing zeros, like %g.
            spec = spec + "g"
        if ftype in ("a", "A"):
            raise ValueError("hex float")
        return format(v, spec.replace("L", ""))
    raise ValueError(f"unknown kind {kind}")


def _parse_spec_fields(spec: str, next_arg, args):
    """Resolves nested `{}` / `{N}` width and precision fields in a spec."""

    def sub(m):
        idx = m.group(1)
        i = next_arg() if idx == "" else int(idx)
        if i >= len(args):
            raise IndexError
        return str(args[i].value)

    return re.sub(r"\{(\d*)\}", sub, spec)


def format_message(fmt: str, args):
    """fmt::format(fmt, args...) for decoded arguments."""
    out = []
    auto = [0]

    def next_arg():
        i = auto[0]
        auto[0] += 1
        return i

    i = 0
    n = len(fmt)
    while i < n:
        c = fmt[i]
        if c == "}":
            out.append("}")
            i += 2 if fmt.startswith("}}", i) else 1
            continue
        if c != "{":
            j = i
            while j < n and fmt[j] not in "{}":
                j += 1
            out.append(fmt[i:j])
            i = j
            continue
        if fmt.startswith("{{", i):
            out.append("{")
            i += 2
            continue
        # Replacement field: find its closing brace past nested {..}.
        depth = 0
        j = i
        while j < n:
            if fmt[j] == "{":
                depth += 1
            elif fmt[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = fmt[i + 1:j]
        i = j + 1
        arg_id, _, spec = body.partition(":")
        try:
            idx = next_arg() if arg_id == "" else int(arg_id)
        except ValueError:
            out.append("{?" + body + "}")
            continue
        if idx >= len(args):
            out.append("{?}")
            continue
        try:
            spec = _parse_spec_fields(spec, next_arg, args)
            out.append(format_value(args[idx], spec))
        except (ValueError, IndexError, TypeError, OverflowError):
            out.append("{?" + str(args[idx].value) + "}")
    return "".join(out)


# --- the console stream -----------------------------------------------------


class Detokenizer:
    def __init__(self, db, show_level=False, warn=None):
        self.tokens = {int(k): v for k, v in db.get("tokens", {}).items()}
        self.build = (db.get("elf_sha256") or "").lower()
        self.show_level = show_level
        self.warn = warn or (lambda msg: print(msg, file=sys.stderr))
        self._warned = set()

    def _header(self, build_id: str):
        build_id = build_id.lower()
        if self.build.startswith(build_id) or build_id.startswith(self.build):
            return
        if build_id not in self._warned:
            self._warned.add(build_id)
            self.warn(f"detokenize_log: records from build {build_id} do not match "
                      f"the database's build {self.build}; their text will be wrong")

    def _record(self, m):
        text = m.group(1)
        try:
            level, truncated, token, args = decode_record(b64decode_unpadded(text))
        except (DecodeError, ValueError):
            return m.group(0)
        fmt = self.tokens.get(token)
        if fmt is None or level >= len(LEVEL_NAMES):
            return m.group(0)
        msg = format_message(fmt, args)
        if truncated:
            msg += " [cut]"
        if self.show_level:
            name = LEVEL_NAMES[level] if level < len(LEVEL_NAMES) else str(level)
            msg = f"[{name}] {msg}"
        return msg

    def line(self, line: str) -> str:
        h = HEADER_RE.search(line)
        if h:
            self._header(h.group(1))
            return line
        if "$" not in line:
            return line
        return RECORD_RE.sub(self._record, line)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--level", action="store_true", help="prefix each record with its level")
    ap.add_argument("db", help="the build's token database (.logtok.json)")
    ap.add_argument("logs", nargs="*", help="console captures (default: stdin)")
    a = ap.parse_args(argv)
    with open(a.db, encoding="utf-8") as f:
        det = Detokenizer(json.load(f), show_level=a.level)
    streams = [open(p, encoding="utf-8", errors="replace") for p in a.logs] or [sys.stdin]
    for stream in streams:
        for raw in stream:
            sys.stdout.write(det.line(raw.rstrip("\r\n")) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
