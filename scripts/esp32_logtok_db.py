#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the token database for an ESP32 firmware ELF's tokenized log records.

Usage: esp32_logtok_db.py [--allow-empty] <firmware.elf> <out.json>

The firmware keeps each log call's format literal, stringized, in the ELF's
non-loaded `.helix_logfmt` section (firmware/.../shim/log_token.h); a record's
token is that string's offset in the section. This reads the section, resolves
each stringized literal to the text the compiler would have stored, and writes

    {"format": 1, "elf_sha256": "<hex>", "tokens": {"<offset>": "<format>"}}

`elf_sha256` is the SHA-256 of the ELF file, the same value the firmware prints
in its `$HLT1:` header line (the first nine hex digits, from
esp_app_get_elf_sha256), so a capture names its database. A missing or empty
section fails the build: the firmware's records would be unreadable. A build
with CONFIG_HELIX_LOG_TOKENIZE off has no section and passes --allow-empty.
Standard library only: the IDF container runs it.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from translations.extractor import resolve_cpp_literal_run  # noqa: E402

SECTION = ".helix_logfmt"
DB_FORMAT = 1


def read_section(elf: bytes, name: str):
    """The bytes of section @name in a little-endian ELF32 or ELF64, or None."""
    if elf[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    is64 = elf[4] == 2
    if elf[5] != 1:
        raise ValueError("big-endian ELF is not supported")
    if is64:
        shoff, = struct.unpack_from("<Q", elf, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", elf, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)

    def header(i):
        base = shoff + i * shentsize
        if is64:
            sh_name, = struct.unpack_from("<I", elf, base)
            sh_type, = struct.unpack_from("<I", elf, base + 4)
            sh_offset, sh_size = struct.unpack_from("<QQ", elf, base + 0x18)
        else:
            sh_name, sh_type = struct.unpack_from("<II", elf, base)
            sh_offset, sh_size = struct.unpack_from("<II", elf, base + 0x10)
        return sh_name, sh_type, sh_offset, sh_size

    _, _, str_off, str_size = header(shstrndx)
    names = elf[str_off:str_off + str_size]
    for i in range(shnum):
        sh_name, sh_type, off, size = header(i)
        end = names.index(b"\0", sh_name)
        if names[sh_name:end].decode("ascii", "replace") == name:
            if sh_type == 8:  # SHT_NOBITS: no file bytes
                return b""
            return elf[off:off + size]
    return None


def entries(section: bytes):
    """(offset, stringized literal) for each NUL-terminated string in @section.

    Alignment can leave NUL padding between strings; a string starts at any
    non-NUL byte that follows a NUL or the section start.
    """
    out = []
    i = 0
    n = len(section)
    while i < n:
        if section[i] == 0:
            i += 1
            continue
        end = section.find(b"\0", i)
        if end == -1:
            end = n
        out.append((i, section[i:end].decode("utf-8", "replace")))
        i = end + 1
    return out


def build_db(elf: bytes):
    section = read_section(elf, SECTION)
    tokens = {}
    for offset, stringized in entries(section or b""):
        tokens[str(offset)] = resolve_cpp_literal_run(stringized)
    return {
        "format": DB_FORMAT,
        "elf_sha256": hashlib.sha256(elf).hexdigest(),
        "tokens": tokens,
    }


def main(argv):
    args = argv[1:]
    allow_empty = "--allow-empty" in args
    args = [a for a in args if a != "--allow-empty"]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    elf = Path(args[0]).read_bytes()
    db = build_db(elf)
    if not db["tokens"] and not allow_empty:
        print(f"error: {args[0]} has no {SECTION} strings: tokenized log records from this "
              "build could not be decoded (is helix_logfmt.ld still linked?)", file=sys.stderr)
        return 1
    Path(args[1]).write_text(json.dumps(db, indent=0, sort_keys=True, ensure_ascii=False) + "\n",
                             encoding="utf-8")
    print(f"{args[1]}: {len(db['tokens'])} log format strings")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
