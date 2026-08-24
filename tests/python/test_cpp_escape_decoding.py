# SPDX-License-Identifier: GPL-3.0-or-later
"""C hex-escape decoding in extracted translation keys.

A C++ source literal is what the extractor sees, but the runtime lookup key is
what the *compiler* produces. `"\\xc2\\xb0"` in source is two raw bytes that
together form U+00B0, so the extracted key must be the degree sign, not the
eight characters of the escape sequence.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import (  # noqa: E402
    decode_c_escapes,
    extract_strings_from_cpp,
    resolve_cpp_literal_run,
)


# --- decode_c_escapes(): one literal token -----------------------------------


def test_hex_escape_lowercase_decodes_to_degree_sign():
    assert decode_c_escapes(r"%d\xc2\xb0") == "%d°"


def test_hex_escape_uppercase_decodes_to_degree_sign():
    assert decode_c_escapes(r"%d\xC2\xB0") == "%d°"


def test_three_byte_sequence_decodes_to_em_dash():
    assert decode_c_escapes(r"a \xe2\x80\x94 b") == "a — b"


def test_mixed_case_within_one_sequence():
    assert decode_c_escapes(r"\xE2\x80\x94") == "—"


def test_newline_escape_decodes_to_a_real_newline():
    assert decode_c_escapes(r"Line one\nLine two") == "Line one\nLine two"


def test_tab_and_carriage_return_decode():
    assert decode_c_escapes(r"a\tb\rc") == "a\tb\rc"


def test_escaped_quote_decodes_to_a_bare_quote():
    assert decode_c_escapes(r'tap \"Check Again\"') == 'tap "Check Again"'


def test_escaped_backslash_decodes_to_one_backslash():
    assert decode_c_escapes(r"a\\b") == "a\\b"


def test_escaped_backslash_does_not_start_a_hex_escape():
    # \\ is a literal backslash; the following xc2 is plain text, not an escape.
    assert decode_c_escapes(r"\\xc2") == "\\xc2"


def test_universal_character_name_decodes():
    # \u2014 is how ui_panel_belt_tension.cpp writes its em dash.
    assert decode_c_escapes(r"freq \u2014 matched") == "freq — matched"


def test_octal_escape_decodes():
    assert decode_c_escapes(r"\101\102") == "AB"


def test_plain_string_is_unchanged():
    assert decode_c_escapes("Heating to 200C") == "Heating to 200C"


def test_already_utf8_text_is_unchanged():
    assert decode_c_escapes("Heating to 200°C") == "Heating to 200°C"


def test_truncated_utf8_sequence_returns_input_unchanged():
    # \xc2 alone is not a complete UTF-8 character. Decoding must not raise and
    # must not substitute a replacement character; the raw literal is returned
    # so the corruption stays visible to the translation gates.
    raw = r"bad \xc2 tail"
    assert decode_c_escapes(raw) == raw


def test_hex_escape_with_no_digits_returns_input_unchanged():
    raw = r"trailing \x"
    assert decode_c_escapes(raw) == raw


def test_hex_escape_out_of_byte_range_returns_input_unchanged():
    # C consumes hex digits greedily, so "\xB0C" is one out-of-range escape and
    # is ill-formed. Real call sites split the literal to avoid it; anything
    # that still hits this is corrupt input, not a string to guess at.
    raw = r"%d\xB0C"
    assert decode_c_escapes(raw) == raw


def test_lone_trailing_backslash_is_preserved():
    assert decode_c_escapes("ends with \\") == "ends with \\"


def test_latin1_codepoint_decoding_is_not_used():
    # codecs.decode(s, "unicode_escape") maps \xc2 -> U+00C2, which is the wrong
    # answer. Guard against a future rewrite reaching for it.
    assert decode_c_escapes(r"\xc2\xb0") != "Â°"


# --- resolve_cpp_literal_run(): adjacent literals -----------------------------
#
# C++ resolves escapes per literal TOKEN, to bytes, and only then concatenates.
# That ordering is why "Heating to %d\xC2\xB0" "C" compiles: the hex escape
# stops at the closing quote instead of swallowing the following C as a third
# hex digit.


def test_run_resolves_each_token_before_joining():
    run = '"Heating to %d\\xC2\\xB0"\n  "C... %.0f\\xC2\\xB0"\n  "C"'
    assert resolve_cpp_literal_run(run) == "Heating to %d°C... %.0f°C"


def test_run_joins_a_utf8_character_split_across_tokens():
    # Byte-level concatenation: neither token is valid UTF-8 alone.
    run = '"\\xe2\\x80" "\\x94"'
    assert resolve_cpp_literal_run(run) == "—"


def test_run_with_a_single_token_matches_decode_c_escapes():
    assert resolve_cpp_literal_run(r'"a\nb"') == "a\nb"


# --- end-to-end through the C++ extractor ------------------------------------


def _extract(tmp_path, source: str):
    f = tmp_path / "sample.cpp"
    f.write_text(source, encoding="utf-8")
    return extract_strings_from_cpp(f)


def test_lv_tr_hex_escape_key_matches_compiler_output(tmp_path):
    # Single literal: the escape is followed by a space, so greedy hex-digit
    # consumption stops on its own and no split is needed.
    src = 'snprintf(buf, sizeof(buf), lv_tr("Chamber at %d\\xC2\\xB0 now"), a);'
    assert "Chamber at %d° now" in _extract(tmp_path, src)


def test_lv_tr_adjacent_literals_join_then_decode(tmp_path):
    # The real call site splits the literal so the hex escape cannot swallow the
    # following 'C' as a third hex digit.
    src = (
        "snprintf(buf, sizeof(buf),\n"
        '         lv_tr("Heating to %d\\xC2\\xB0"\n'
        '               "C... %.0f\\xC2\\xB0"\n'
        '               "C"),\n'
        "         a, b);\n"
    )
    assert "Heating to %d°C... %.0f°C" in _extract(tmp_path, src)


def test_lv_tr_em_dash_key_matches_compiler_output(tmp_path):
    src = (
        "snprintf(step_text, sizeof(step_text),\n"
        '         lv_tr("Touch the target (point %1$d of 3) \\xe2\\x80\\x94 touch %2$d of %3$d"),\n'
        "         a, b, c);\n"
    )
    expected = "Touch the target (point %1$d of 3) — touch %2$d of %3$d"
    assert expected in _extract(tmp_path, src)


def test_lv_tr_rotation_key_matches_compiler_output(tmp_path):
    src = 'lv_tr("Testing rotation: %d\\xc2\\xb0 (%d/%d) - %ds remaining")'
    assert "Testing rotation: %d° (%d/%d) - %ds remaining" in _extract(tmp_path, src)


def test_extracted_lv_tr_keys_never_contain_a_raw_hex_escape(tmp_path):
    src = (
        'lv_tr("Testing rotation: %d\\xc2\\xb0 (%d/%d) - %ds remaining");\n'
        'lv_tr("Heating to %d\\xC2\\xB0" "C");\n'
    )
    for key in _extract(tmp_path, src):
        assert "\\x" not in key, key


def test_lv_tr_newline_key_matches_compiler_output(tmp_path):
    src = 'set_status(lv_tr("Moonraker restarting...\\nWaiting for reconnection..."));'
    assert "Moonraker restarting...\nWaiting for reconnection..." in _extract(tmp_path, src)


def test_lv_tr_escaped_quote_key_matches_compiler_output(tmp_path):
    src = 'set_status(lv_tr("Install the plugin via SSH,\\nthen tap \\"Check Again\\"."));'
    assert 'Install the plugin via SSH,\nthen tap "Check Again".' in _extract(tmp_path, src)


def test_extracted_lv_tr_keys_never_contain_any_c_escape(tmp_path):
    src = (
        'lv_tr("Testing rotation: %d\\xc2\\xb0 (%d/%d)");\n'
        'lv_tr("Heating to %d\\xC2\\xB0" "C");\n'
        'lv_tr("Game Over!\\nScore: {}");\n'
        'lv_tr("tap \\"Check Again\\"");\n'
        'lv_tr("freq \\u2014 matched");\n'
    )
    for key in _extract(tmp_path, src):
        assert "\\" not in key, key


# --- round trip: compiler key -> XML pack -> expat --------------------------
#
# The bug this guards: XML attribute-value normalization (XML 1.0 s3.3.3)
# collapses a LITERAL newline in an attribute value to a space, so a key holding
# a real newline only survives if the generator emits a numeric character
# reference. helix-xml parses the packs with expat
# (lib/helix-xml/src/xml/lv_xml_translation.c), and xml.parsers.expat is the
# same library, so this exercises the real normalization rather than an
# assumption about it.


def _expat_attr(doc: str, attr: str = "tag") -> str:
    import xml.parsers.expat

    seen = {}
    parser = xml.parsers.expat.ParserCreate()
    parser.StartElementHandler = lambda name, attrs: seen.update(attrs)
    parser.Parse(doc, True)
    return seen[attr]


def _escape_xml_attr(text: str) -> str:
    import importlib.util

    path = REPO_ROOT / "scripts" / "generate_translations.py"
    spec = importlib.util.spec_from_file_location("_gen_trans", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.escape_xml_attr(text)


ROUND_TRIP_KEYS = [
    "Moonraker restarting...\nWaiting for reconnection...",
    "No changes selected.\n\nClick Cancel to close.",
    'Install the plugin via SSH,\nthen tap "Check Again".',
    "Testing rotation: %d° (%d/%d)",
    "freq — matched frequencies",
    "a\tb",
    "plain ascii key",
    "ampersand & angle < > quote \" apostrophe '",
]


def test_keys_survive_the_xml_pack_round_trip():
    for key in ROUND_TRIP_KEYS:
        doc = f'<translation tag="{_escape_xml_attr(key)}"/>'
        assert _expat_attr(doc) == key, f"round trip lost data for {key!r}"


def test_literal_newline_in_an_attribute_would_not_survive():
    # Proves the round-trip test above is actually testing something: without
    # the character reference, expat normalizes the newline away.
    doc = '<translation tag="a\nb"/>'
    assert _expat_attr(doc) == "a b"


def test_cpp_source_to_pack_round_trip(tmp_path):
    src = 'set_status(lv_tr("No webcam detected.\\nA webcam is required for timelapse."));'
    (key,) = [k for k in _extract(tmp_path, src) if "webcam" in k]
    doc = f'<translation tag="{_escape_xml_attr(key)}"/>'
    assert _expat_attr(doc) == "No webcam detected.\nA webcam is required for timelapse."


# --- the raw-hex-escape regression guard -------------------------------------


def _load_gate():
    import importlib.util

    path = REPO_ROOT / "scripts" / "check_translation_format_specifiers.py"
    spec = importlib.util.spec_from_file_location("_fmt_gate", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _write_locale(tmp_path, locale: str, translations: dict):
    import yaml

    body = {"locale": locale, "translations": translations}
    (tmp_path / f"{locale}.yml").write_text(
        yaml.safe_dump(body, allow_unicode=True), encoding="utf-8"
    )


def test_gate_flags_a_key_holding_a_raw_hex_escape(tmp_path, monkeypatch):
    gate = _load_gate()
    monkeypatch.setattr(gate, "TRANS_DIR", tmp_path)
    _write_locale(tmp_path, "de", {r"Rotation: %d\xc2\xb0": "Drehung: %d°"})
    problems = gate.check_unresolved_escapes()
    assert [(loc, kind) for loc, kind, _ in problems] == [("de", "key")]


def test_gate_flags_a_value_holding_a_raw_hex_escape(tmp_path, monkeypatch):
    gate = _load_gate()
    monkeypatch.setattr(gate, "TRANS_DIR", tmp_path)
    _write_locale(tmp_path, "fr", {"Rotation: %d°": r"Rotation : %d\xc2\xb0"})
    problems = gate.check_unresolved_escapes()
    assert [(loc, kind) for loc, kind, _ in problems] == [("fr", "value")]


def test_gate_flags_a_key_holding_a_literal_backslash_n(tmp_path, monkeypatch):
    # The 24-key regression: a stored backslash-n can never match the real
    # newline the compiler emits.
    gate = _load_gate()
    monkeypatch.setattr(gate, "TRANS_DIR", tmp_path)
    _write_locale(tmp_path, "it", {r"Game Over!\nScore": "Fine partita"})
    problems = gate.check_unresolved_escapes()
    assert [(loc, kind) for loc, kind, _ in problems] == [("it", "key")]


def test_gate_flags_other_surviving_c_escapes(tmp_path, monkeypatch):
    gate = _load_gate()
    monkeypatch.setattr(gate, "TRANS_DIR", tmp_path)
    _write_locale(
        tmp_path,
        "pt",
        {
            r"tap \"Check Again\"": "toque",
            r"tab\there": "tabulacao",
            r"dash \u2014 here": "traco",
        },
    )
    kinds = sorted(k for _, kind, k in [(a, b, c) for a, b, c in gate.check_unresolved_escapes()])
    assert len(kinds) == 3


def test_gate_accepts_resolved_keys(tmp_path, monkeypatch):
    gate = _load_gate()
    monkeypatch.setattr(gate, "TRANS_DIR", tmp_path)
    _write_locale(
        tmp_path,
        "es",
        {
            "Rotation: %d°": "Rotación: %d°",
            "Line one\nLine two": "Linea uno\nLinea dos",
            'tap "Check Again"': 'toque "Comprobar"',
        },
    )
    assert gate.check_unresolved_escapes() == []


def test_shipped_locales_carry_no_unresolved_escapes():
    # End-to-end on the real translation set: the field bug was a key that could
    # never match its runtime lookup.
    assert _load_gate().check_unresolved_escapes() == []


# --- acceptance: every lv_tr() key in src/ exists as a tag in en.xml ---------


def test_every_lv_tr_key_in_src_resolves_to_a_pack_tag():
    import xml.parsers.expat

    sys.path.insert(0, str(REPO_ROOT / "scripts"))
    from translations.extractor import LV_TR_RUN_RE, resolve_cpp_literal_run

    tags = set()
    parser = xml.parsers.expat.ParserCreate()
    parser.StartElementHandler = lambda n, a: (
        tags.add(a["tag"]) if n == "translation" and "tag" in a else None
    )
    parser.Parse((REPO_ROOT / "ui_xml/translations/en.xml").read_bytes(), True)

    misses = {}
    for path in sorted((REPO_ROOT / "src").rglob("*")):
        if path.suffix not in {".cpp", ".h", ".hpp"} or "generated" in str(path):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in LV_TR_RUN_RE.finditer(text):
            key = resolve_cpp_literal_run(m.group(1))
            if key and key.strip() and key not in tags:
                misses.setdefault(key, f"{path}:{text[:m.start()].count(chr(10)) + 1}")
    assert not misses, "lv_tr() keys with no tag in en.xml:\n" + "\n".join(
        f"  {k!r}  ({site})" for k, site in sorted(misses.items())
    )
