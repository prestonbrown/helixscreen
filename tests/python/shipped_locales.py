"""The language codes the app offers, read from the list the Settings picker uses."""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SETTINGS_SRC = REPO_ROOT / "src" / "system" / "system_settings_manager.cpp"


def shipped_locale_codes() -> set[str]:
    """Codes in SystemSettingsManager's kLanguageCodes: one translations/<code>.yml each."""
    text = SETTINGS_SRC.read_text(encoding="utf-8")
    block = re.search(r"kLanguageCodes\[\]\s*=\s*\{(.*?)\};", text, re.S)
    assert block, "kLanguageCodes not found in system_settings_manager.cpp"
    codes = set(re.findall(r'"([A-Za-z-]+)"', block.group(1)))
    assert len(codes) >= 8, f"language list looks wrong: {sorted(codes)}"
    return codes
