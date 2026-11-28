#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail a commit whose theme tokens moved without the artefacts derived from them.

Two committed files are generated from the <color>/<px>/<string> tokens in the
top-level ui_xml/*.xml:

  src/generated/theme_token_table.cpp   `make regen-tokens`, pure Python, so it
                                        is regenerated in memory and compared
                                        byte for byte. The commit hook asks for
                                        that render (--write-table) and stages
                                        it.
  tests/fixtures/theme_const_golden.txt the [theme_pins] snapshot, which only
                                        the test binary can produce (it resolves
                                        tokens through LVGL at three
                                        resolutions). It is not regenerated here;
                                        this reports a staleness it can prove and
                                        prints the command.

What "provably stale" means for the golden, when it is not part of the commit:
its names are the XML token names, the base name of every suffixed token, and a
set of names C++ registers (read off HEAD's golden as whatever the HEAD XML does
not explain). An XML name missing from it, or a name it still lists that nothing
derives any more, is stale. So is a token whose HEAD value the golden records
verbatim and whose new value it does not. A value the golden does not record
verbatim (a responsive variant reads <unset>, its base resolves through
breakpoints) is left to the test. A golden that is part of the commit was
regenerated, and is trusted. One that regeneration leaves byte-identical (a
dropped XML token whose name C++ also registers) can never join the commit, so
HELIX_GOLDEN_CURRENT=1 records that it was regenerated and is current.

tools/xml-linter/schema/schema.json is the third artefact these tokens feed;
scripts/qc/xml_schema.sh owns it.

usage: check_theme_artifacts.py [--staged-only] [--write-table PATH]
  --staged-only  read the index (the commit) and compare against HEAD
  --write-table  with --staged-only, write the table the index's tokens render
                 to PATH instead of failing; the golden is still judged
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_theme_tokens  # noqa: E402
from staged_content import catfile_batch, tree_files  # noqa: E402

TABLE = gen_theme_tokens.OUT_PATH
GOLDEN = "tests/fixtures/theme_const_golden.txt"
SUFFIX_RE = re.compile(r"_(light|dark|micro|tiny|small|medium|large|xlarge|xxlarge)$")
GOLDEN_CMD = 'make test && HELIX_UPDATE_GOLDEN=1 ./build/bin/helix-tests "[theme_pins]"'


def xml_files(rev: str | None) -> list:
    files = sorted(tree_files(rev, ("ui_xml",), recursive=False))
    return [(p, text.encode("utf-8")) for p, text in files if p.endswith(".xml")]


def blob(rev: str | None, path: str) -> str | None:
    spec = f":{path}" if rev is None else f"{rev}:{path}"
    return dict(catfile_batch([(path, spec)])).get(path)


def golden_names(text: str) -> set:
    return {line.split("=", 1)[0] for line in text.splitlines()
            if "=" in line and not line.startswith(("##", "color:", "palette:"))}


def derived_names(tokens: dict) -> set:
    names = {name for _, name in tokens}
    return names | {SUFFIX_RE.sub("", n) for n in names if SUFFIX_RE.search(n)}


def golden_problems(old_tokens: dict, new_tokens: dict, old_golden: str,
                    new_golden: str) -> list:
    """Names whose golden lines the token change provably invalidates."""
    if old_golden != new_golden:
        return []
    have = golden_names(new_golden)
    cpp_names = golden_names(old_golden) - derived_names(old_tokens)
    want = derived_names(new_tokens) | cpp_names
    stale = sorted(want - have) + sorted(have - want)
    lines = set(old_golden.splitlines())
    for key, value in new_tokens.items():
        name = key[1]
        before = old_tokens.get(key)
        if (before is not None and before != value
                and f"{name}={before}" in lines and f"{name}={value}" not in lines):
            stale.append(name)
    return stale


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--staged-only", action="store_true")
    ap.add_argument("--write-table", metavar="PATH",
                    help="with --staged-only, write the rendered table to PATH "
                         "instead of failing")
    args = ap.parse_args()

    if args.staged_only:
        new_files, table = xml_files(None), blob(None, TABLE)
    else:
        new_files = gen_theme_tokens.top_level_files()
        table = Path(TABLE).read_text(encoding="utf-8") if Path(TABLE).exists() else None
    new_tokens = gen_theme_tokens.collect(new_files)

    failed = False
    written = False
    rendered = gen_theme_tokens.render(new_tokens)
    if table != rendered:
        if args.staged_only and args.write_table:
            with open(args.write_table, "w") as f:
                f.write(rendered)
            written = True
        else:
            failed = True
            print(f"❌ {TABLE} does not match the tokens in ui_xml/*.xml")
            print(f"   Regenerate and commit it: make regen-tokens && git add {TABLE}")

    if args.staged_only:
        has_head = subprocess.run(["git", "rev-parse", "-q", "--verify", "HEAD"],
                                  capture_output=True).returncode == 0
        old_golden = blob("HEAD", GOLDEN) if has_head else None
        new_golden = blob(None, GOLDEN)
        if (old_golden is not None and new_golden is not None
                and os.environ.get("HELIX_GOLDEN_CURRENT") != "1"):
            old_tokens = gen_theme_tokens.collect(xml_files("HEAD"))
            stale = golden_problems(old_tokens, new_tokens, old_golden, new_golden)
            if stale:
                failed = True
                shown = ", ".join(stale[:12]) + (" ..." if len(stale) > 12 else "")
                print(f"❌ {GOLDEN} is stale for: {shown}")
                print(f"   Regenerate it, review the diff and commit it:\n   {GOLDEN_CMD}")
                print("   If regenerating leaves it unchanged, it is current: commit with "
                      "HELIX_GOLDEN_CURRENT=1")

    if failed:
        return 1
    if written:
        print(f"✅ {TABLE} regenerated from the staged tokens ({len(new_tokens)} tokens)")
    else:
        print(f"✅ Theme artefacts match {len(new_tokens)} tokens in ui_xml/*.xml")
    return 0


if __name__ == "__main__":
    sys.exit(main())
