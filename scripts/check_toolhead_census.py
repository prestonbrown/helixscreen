#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The toolhead-safety census from tests/unit/test_job_holds_machine.cpp, in seconds.

Mirrors the test's five XML cases: every ui_xml/ file that declares a control
sits in exactly one of the two census tables, kGuardedFiles or
kNoMachineControlFiles, and no row is stale; each guarded file carries exactly
its row's moves_machine="true" count, spelled "true"; a no-machine file carries
no moves_machine attribute; no <bind_state_*> disables a control on the raw
print_active subject; and no XML names the job_holds_machine subject. The
Catch2 test is the authority; this reads the tables, markers, constants and the
print_active subject name out of that same .cpp, so the two cannot disagree,
and answers at commit time instead of at the end of a full test run. A missing
or unparseable test file is a failure, never a pass.

usage: check_toolhead_census.py [--staged-only] [--root DIR]
  --staged-only  read ui_xml/ and the test from the tree the commit will create
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from staged_content import read_index_blobs, tree_files  # noqa: E402

TEST_PATH = "tests/unit/test_job_holds_machine.cpp"
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def unescape(s: str) -> str:
    return re.sub(r"\\(.)", r"\1", s)


def block(src: str, opener: str, closer: str) -> str:
    start = src.find(opener)
    if start < 0:
        raise ValueError(f"{TEST_PATH}: cannot find {opener!r}")
    end = src.find(closer, start + len(opener))
    return src[start + len(opener):end]


def constant(src: str, name: str) -> str:
    m = re.search(r"constexpr const char\* " + name + r' = "((?:[^"\\]|\\.)*)";', src)
    if not m:
        raise ValueError(f"{TEST_PATH}: cannot find {name}")
    return unescape(m.group(1))


def parse_test(src: str) -> dict:
    guarded = {unescape(p): int(n) for p, n in re.findall(
        r'\{"((?:[^"\\]|\\.)*)",\s*(\d+)\}', block(src, "kGuardedFiles[] = {", "};"))}
    none = [unescape(s) for s in STRING_RE.findall(
        block(src, "kNoMachineControlFiles[] = {", "};"))]
    markers = [unescape(s) for s in STRING_RE.findall(
        block(src, "bool declares_a_control(const std::string& xml) {", "if (xml.find"))]
    # kDerivedGuards entries are {binding, proof}; each may be split across
    # adjacent C++ string literals, which concatenate.
    derived = []
    if src.find("kDerivedGuards[] = {") >= 0:
        body = block(src, "kDerivedGuards[] = {", "};")
        lit = r'(?:"(?:[^"\\]|\\.)*"\s*)+'
        for binding, _proof in re.findall(r"\{\s*(" + lit + r"),\s*(" + lit + r")\}", body):
            derived.append("".join(unescape(p) for p in STRING_RE.findall(binding)))
    raw = re.search(r'bind\.find\("((?:[^"\\]|\\.)*)"\) == std::string::npos', src)
    if not raw:
        raise ValueError(f"{TEST_PATH}: cannot find the raw print subject the binds are checked for")
    return {
        "subject": constant(src, "kSubject"),
        "disabled": constant(src, "kDisabled"),
        "raw_subject": unescape(raw.group(1)),
        "guarded": guarded,
        "none": none,
        "markers": markers,
        "guard": constant(src, "kGuardAttribute"),
        "guard_name": constant(src, "kGuardAttributeName"),
        "socket": constant(src, "kSocketGuard"),
        "derived": derived,
    }


def check(test_src: str, xml: dict[str, str]) -> list[str]:
    t = parse_test(test_src)
    if not t["markers"] or not t["guarded"] or not t["none"]:
        return [f"{TEST_PATH}: the census tables or control markers parsed empty; "
                "update scripts/check_toolhead_census.py to the test's new shape"]
    problems = []
    classified = set(t["guarded"])
    for path in t["none"]:
        if path in classified:
            problems.append(f"{path} appears in both census tables")
        classified.add(path)

    bears = {p for p, text in xml.items() if any(m in text for m in t["markers"])}
    unclassified = sorted(bears - classified)
    if unclassified:
        problems.append(
            "These files declare a control and are in neither census table:"
            + "".join(f"\n  {p}" for p in unclassified)
            + "\nAdd each to kGuardedFiles with the number of toolhead guards its controls "
              "carry, or to kNoMachineControlFiles to state that none of them commands the "
              f"printer. Both tables are in {TEST_PATH}.")
    stale = sorted(classified - bears)
    if stale:
        problems.append(
            "These census rows name a file that is gone or no longer declares a control:"
            + "".join(f"\n  {p}" for p in stale) + "\nDrop the row.")

    def attrs(text: str) -> int:
        return text.count(t["guard_name"]) - text.count(t["socket"])

    for path, want in sorted(t["guarded"].items()):
        text = xml.get(path)
        if text is None:
            continue
        spelled = text.count(t["guard"])
        have = spelled + sum(text.count(d) for d in t["derived"])
        if attrs(text) != spelled:
            problems.append(f'{path} carries {attrs(text)} moves_machine attributes but {spelled} '
                            'spelled "true". Spell it "true" or drop the attribute.')
        elif have != want:
            problems.append(f"{path} pins {want} toolhead guards and carries {have}. If a "
                            "control was legitimately added or removed, update its "
                            f"kGuardedFiles row in {TEST_PATH}.")
    for path in t["none"]:
        text = xml.get(path)
        derived_here = sum(text.count(d) for d in t["derived"]) if text is not None else 0
        if text is not None and (attrs(text) or derived_here):
            problems.append(f"{path} carries {attrs(text)} moves_machine attributes and "
                            f"{derived_here} derived guards, so a control in it does command "
                            "the printer. Move its row to kGuardedFiles with that count.")
    for path, text in sorted(xml.items()):
        mentions = text.count(t["subject"])
        if mentions:
            problems.append(f'{path} names {t["subject"]} {mentions} times. Carry the guard as '
                            'moves_machine="true" on the control, and say the rest in words '
                            'that are not the subject name.')
        for bind in state_binds(text):
            if t["disabled"] in bind and t["raw_subject"] in bind:
                problems.append(f'{path} disables a control on {t["raw_subject"]}, which reads '
                                '0 throughout a host-side pre-print block while the toolhead '
                                f'homes and probes: {bind}')
    return problems


def state_binds(xml: str) -> list[str]:
    """Each <bind_state_* element in the file, as raw text, as the test slices it."""
    binds, pos = [], 0
    while (pos := xml.find("<bind_state_", pos)) != -1:
        end = xml.find("/>", pos)
        if end == -1:
            binds.append(xml[pos:])
            break
        binds.append(xml[pos:end + 2])
        pos = end + 2
    return binds


def load(root: Path, staged: bool) -> tuple[str, dict[str, str]]:
    if staged:
        test_src = dict(read_index_blobs([TEST_PATH], root)).get(TEST_PATH, "")
        xml = {p: s for p, s in tree_files(None, ("ui_xml",), root)
               if p.endswith(".xml") and "/." not in p}
        return test_src, xml
    test_file = root / TEST_PATH
    test_src = test_file.read_text(encoding="utf-8") if test_file.is_file() else ""
    xml = {}
    for f in sorted((root / "ui_xml").rglob("*.xml")):
        rel = f.relative_to(root).as_posix()
        if "/." not in rel:
            xml[rel] = f.read_text(encoding="utf-8", errors="replace")
    return test_src, xml


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--staged-only", action="store_true")
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    test_src, xml = load(root, args.staged_only)
    if not test_src:
        where = "the index" if args.staged_only else str(root)
        print(f"❌ Toolhead-safety census: {TEST_PATH} is not in {where}; the census tables "
              "live there. Restore it, or update scripts/check_toolhead_census.py to their "
              "new home.")
        return 1
    try:
        problems = check(test_src, xml)
    except ValueError as e:
        problems = [f"{e}; update scripts/check_toolhead_census.py to the test's new shape"]
    if problems:
        print("❌ Toolhead-safety census ([job_holds_machine]):")
        for p in problems:
            print("   " + p.replace("\n", "\n   "))
        return 1
    print(f"✅ Toolhead-safety census: {len(xml)} ui_xml files, every control-bearing one "
          "classified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
