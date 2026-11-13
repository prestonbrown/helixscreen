# SPDX-License-Identifier: GPL-3.0-or-later
"""The ESP32 tokenized-log host tools: scripts/detokenize_log.py and
scripts/esp32_logtok_db.py (#1915).

tests/fixtures/logtok/corpus.json holds records the firmware's encoder wrote
(tests/unit/test_esp32_logtok.cpp pins them) together with fmt::format's text
for each, so decoding every record must give back exactly that text.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import detokenize_log as dt  # noqa: E402
import esp32_logtok_db as db  # noqa: E402

CORPUS = json.loads((REPO_ROOT / "tests/fixtures/logtok/corpus.json").read_text(encoding="utf-8"))


def corpus_db(sha="ab" * 32):
    return {"format": 1, "elf_sha256": sha,
            "tokens": {str(e["token"]): e["fmt"] for e in CORPUS}}


def test_every_corpus_record_decodes_to_fmts_text():
    det = dt.Detokenizer(corpus_db())
    assert len(CORPUS) >= 15
    for e in CORPUS:
        assert det.line(e["line"]) == e["text"], e["fmt"]


def test_record_inside_a_line_is_replaced_and_the_rest_kept():
    det = dt.Detokenizer(corpus_db())
    e = CORPUS[0]
    assert det.line(f"12:00:01 {e['line']}") == f"12:00:01 {e['text']}"


def test_other_console_lines_pass_through_untouched():
    det = dt.Detokenizer(corpus_db())
    for line in ["I (312) cpu_start: Starting scheduler on PRO CPU.",
                 "=====HELIX-SNAP 800 480",
                 "NOTE: warning t=12ms price is $5 today",
                 "cost $AB$CD",
                 ""]:
        assert det.line(line) == line


def test_header_from_another_build_warns_once():
    # The firmware prints the first nine hex digits (CONFIG_APP_RETRIEVE_LEN_ELF_SHA).
    warnings = []
    det = dt.Detokenizer(corpus_db("ab" * 32), warn=warnings.append)
    header = "$HLT1:cdcdcdcdc"
    assert det.line(header) == header
    det.line(header)
    det.line("$HLT1:ababababa")
    assert len(warnings) == 1
    assert "cdcdcdcdc" in warnings[0]


def test_header_naming_the_databases_build_is_quiet():
    warnings = []
    det = dt.Detokenizer(corpus_db("ab" * 32), warn=warnings.append)
    det.line("$HLT1:ababababa")
    det.line("$HLT1:" + "ab" * 32)
    assert warnings == []


def test_dollar_words_that_are_not_records_are_left_alone():
    det = dt.Detokenizer(corpus_db())
    # "$AAU" is valid base64 for level 0, token 5, which this database lacks.
    for line in ["export PATH=$HOME/bin", "cost $HOME", "x $AAU y", CORPUS[0]["line"] + "x"]:
        assert det.line(line) == line
    assert dt.Detokenizer({"tokens": {}}).line(CORPUS[0]["line"]) == CORPUS[0]["line"]


def test_truncated_record_is_marked_and_missing_arguments_shown():
    # level 2 with the truncated flag, token 7, one int (zigzag 10 -> 5); "{} {}".
    payload = bytes([0x82, 7, 1, 10])
    line = "$" + dt.base64.b64encode(payload).decode().rstrip("=")
    det = dt.Detokenizer({"tokens": {"7": "a={} b={}"}}, show_level=True)
    assert det.line(line) == "[I] a=5 b={?} [cut]"


def test_unformattable_value_falls_back_to_its_raw_form():
    assert dt.format_message("x={:q}", [dt.Value("int", 3)]) == "x={?3}"
    assert dt.format_message("x={}", [dt.Value("unsupported")]) == "x={?None}"


def test_fmt_float_and_bool_rules():
    f = dt.format_message
    assert f("{}", [dt.Value("float", 100.0)]) == "100"
    assert f("{}", [dt.Value("float", 1e-7)]) == "1e-07"
    assert f("{}", [dt.Value("float32", struct.unpack("<f", struct.pack("<f", 0.1))[0])]) == "0.1"
    assert f("{:.3}", [dt.Value("float", 100.0)]) == "100"
    assert f("{} {:d}", [dt.Value("bool", True), dt.Value("bool", True)]) == "true 1"
    assert f("{:08.3f}", [dt.Value("float", -3.14159)]) == "-003.142"


def _elf32(section_name: bytes, section_data: bytes) -> bytes:
    """A minimal little-endian ELF32 with one PROGBITS section and .shstrtab."""
    shstr = b"\0" + section_name + b"\0.shstrtab\0"
    data_off = 52
    str_off = data_off + len(section_data)
    sh_off = str_off + len(shstr)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    header = ident + struct.pack("<HHIIIIIHHHHHH", 1, 94, 1, 0, 0, sh_off, 0, 52, 0, 0, 40, 3, 2)

    def sh(name, typ, off, size):
        return struct.pack("<IIIIIIIIII", name, typ, 0, 0, off, size, 0, 0, 1, 0)

    sections = (sh(0, 0, 0, 0) + sh(1, 1, data_off, len(section_data))
                + sh(2 + len(section_name), 3, str_off, len(shstr)))
    return header + section_data + shstr + sections


def test_database_from_an_elf_resolves_each_stringized_literal():
    # What `#x` stores: the literal's source text, quotes and escapes included,
    # with alignment padding between entries.
    section = (b'"temp: {}\\n"\0' b'\0\0\0' b'"a" "b\\x41" "\\"q\\""\0'
               b'"deg \\xc2\\xb0""C"\0')
    elf = _elf32(b".helix_logfmt", section)
    out = db.build_db(elf)
    assert out["elf_sha256"] == hashlib.sha256(elf).hexdigest()
    second = section.index(b'"a"')
    third = section.index(b'"deg')
    assert out["tokens"] == {"0": "temp: {}\n", str(second): 'abA"q"', str(third): "deg °C"}


def test_database_from_an_elf_without_the_section_fails_unless_allowed(tmp_path):
    for section, data in ((b".comment", b"GCC\0"), (b".helix_logfmt", b"")):
        elf = tmp_path / "fw.elf"
        elf.write_bytes(_elf32(section, data))
        out = tmp_path / "db.json"
        assert db.main(["esp32_logtok_db.py", str(elf), str(out)]) != 0, section
        assert db.main(["esp32_logtok_db.py", "--allow-empty", str(elf), str(out)]) == 0
        assert json.loads(out.read_text())["tokens"] == {}
