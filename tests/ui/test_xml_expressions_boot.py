# SPDX-License-Identifier: GPL-3.0-or-later
"""Every <subject_expr> compiles when its component registers.

A <subject_expr> resolves its operands when the component registers, before
any init_subjects() runs. One over a subject an init_subjects() creates never
registers, and every binding to it silently does nothing: the app only logs a
warning (docs/devel/LVGL9_XML_GUIDE.md § "Which phase registers the subject").
All components register at boot, so one boot names every such expression.
"""
import os
import re
from pathlib import Path

import pytest

from helix.app import HelixApp

_REPO_ROOT = Path(__file__).resolve().parents[2]
_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY", str(_REPO_ROOT / "build" / "bin" / "helix-screen")))

_EXPR_FAILURE = re.compile(r"expr: (unknown subject|parse error)")


def test_every_subject_expr_compiles_at_boot(tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")

    app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                   log_path=tmp_path / "app.log", extra_args=["-vv"])
    with app:
        boot_log = (tmp_path / "app.log").read_text(errors="replace")

    failures = [line for line in boot_log.splitlines() if _EXPR_FAILURE.search(line)]
    assert not failures, (
        "XML expressions that did not compile at boot (use a view-time cond, or "
        "publish the answer from C++):\n" + "\n".join(failures))
