"""Every string C++ marks for translation must have a non-empty value in every
shipped locale.

`test_explicit_tag_coverage.py` covers the same ground for `*_tag` attributes in
ui_xml. Neither scan sees the other's strings, so without this gate a string that
only ever appears in an `lv_tr()` call reaches the device in English on every
non-English printer with nothing objecting.

The scan is the extractor the sync tool itself uses, so this gate and
`make translation-sync` always agree on what counts as translatable. A string the
extractor deliberately skips (a log line, a format-only fragment, a marked
non-translatable substring) is out of scope here as well.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import extract_strings_from_cpp_directory  # noqa: E402
from shipped_locales import shipped_locale_codes  # noqa: E402
from translations.yaml_manager import load_yaml_file_readonly  # noqa: E402

# The trees `make translation-sync` passes as its C++ sources.
CPP_SOURCE_DIRS = ("src", "include")

# A floor, only to keep the gate from going vacuous if the extractor stops
# matching. It sits well below the real population so ordinary churn never
# trips it. Measured at 1923 on 2026-09-21.
MIN_CPP_STRINGS = 1500


def _cpp_strings() -> set:
    found = set()
    for name in CPP_SOURCE_DIRS:
        found |= extract_strings_from_cpp_directory(REPO_ROOT / name, recursive=True)
    return found


def test_cpp_scan_is_not_vacuous():
    """A gate that silently matches nothing passes everything."""
    found = _cpp_strings()
    assert len(found) >= MIN_CPP_STRINGS, (
        f"C++ extractor found only {len(found)} translatable strings, "
        f"expected at least {MIN_CPP_STRINGS} -- the scan is probably broken, "
        "not the code"
    )


def test_every_cpp_string_translated_in_every_locale():
    found = _cpp_strings()

    locales = sorted((REPO_ROOT / "translations").glob("*.yml"))
    assert {p.stem for p in locales} == shipped_locale_codes(), (
        "translations/*.yml must be exactly the languages the app lists: "
        f"{[p.name for p in locales]}"
    )

    missing = []
    for yml in locales:
        entries = load_yaml_file_readonly(yml)["translations"]
        for text in sorted(found):
            if not entries.get(text):
                missing.append(f"{yml.name}: {text!r}")

    assert not missing, (
        "lv_tr() strings missing or empty in a locale:\n  " + "\n  ".join(missing)
    )
