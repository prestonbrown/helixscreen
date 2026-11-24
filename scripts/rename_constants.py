#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rename UPPER_SNAKE_CASE C++ constants to kCamelCase.

Named constants are kCamelCase; UPPER_SNAKE_CASE belongs to preprocessor
macros, because a macro ignores scope and silently rewrites any constant that
shares its name.

    scripts/rename_constants.py --plan FILE            # write the mapping, change nothing
    scripts/rename_constants.py --apply                # rewrite every file in scope
    scripts/rename_constants.py --apply --files A B    # rewrite only these files
    scripts/rename_constants.py --apply --git-range main..HEAD
    scripts/rename_constants.py --apply --mapping FILE # also apply an earlier plan's renames

The mapping is always computed from the whole tree, so a file list narrows what
is rewritten, never what is renamed. A second run is a no-op: once renamed, a
name is no longer UPPER_SNAKE and no longer a candidate. A branch that adds
references to names an earlier run already renamed (so the tree no longer
declares them) applies that run's plan with --mapping.

WHAT IS A CANDIDATE

A variable whose declaration carries `const`, `constexpr` or `constinit`
(namespace scope, static and non-static members, function locals, out-of-line
`Foo::NAME` definitions), declared in a C++ file under src/, include/, tests/,
tools/ or firmware/helixscreen-esp32/, whose name is UPPER_SNAKE: only capitals,
digits and single underscores, starting with a capital, with at least two
capital letters (`PI` and `MAX_X` are; `N`, `T2` and `_FOO` are not). C files
(.c) are not harvested for declarations but their references are rewritten.

CONVERSION

Split on `_`; each word keeps its first character and lowercases the rest; the
words are joined behind `k`. Acronyms get no special case, so the rule has no
exception table: HTTP_TIMEOUT_MS -> kHttpTimeoutMs, MAX_3D_SIZE -> kMax3dSize,
RGB565_BPP -> kRgb565Bpp.

WHAT IS SKIPPED (reported in the plan, never renamed)

  macro        the name is #defined anywhere (repo, lib/, build flags, Kconfig)
  enumerator   the name is also an enumerator somewhere in the scanned tree
  third-party  the name is an identifier in lib/ or a vendored third-party file
  data         the name is used as data: it is a whole string literal in C++,
               or appears in an XML/JSON/Lua/YAML/config file, or is passed to a
               stringifying (#arg) macro defined in this repo
  collision    two old names convert to the same new name, the new name is a
               lib/ or vendored identifier, or a translation unit sees both
               the old and the new name (below)
  generated    declared or referenced in a generated file (first lines say
               "do not edit"); its generator would put the old name back

A new name that already exists in our own code is no collision when no
translation unit can see both names: no file holds both, and no file reaches
one through its quoted or bracketed #include closure (forced -include headers
from the build files count as included everywhere) while reaching the other.
Two such names live in different scopes, so the rename cannot change what
either one refers to.

Every skipped name is listed with its reason and declaration sites on stdout
after --plan and after --apply. With --git-range, each declaration of a skipped
name that the commit gate (scripts/check_constant_naming.py) will flag in the
range is listed again and the run exits 1; --allow-skips accepts them.

REWRITING

C++ files: every code identifier that names a renamed constant, and the same
words in comments; string and character literals are never touched. A comment
word with no underscore (TOTAL, COLORS) is renamed only where the file's code
uses that name, since such a word is often plain emphasis.

Docs and scripts (.md, .py, .sh, .bats, .mk, Makefile, scripts/*.txt): names
with an underscore, word-bounded. A bare word is renamed only inside a Markdown
code span. Never a word followed by a file extension (CONTRIBUTORS.txt) or
behind `$`, and never a name the file uses as a shell, Python or make variable
(`NAME=`, `export NAME`, `import NAME`).

GENERATORS

A generator that emits a renamed constant is updated by hand, beside this
run: scripts/gen-contributors.sh writes kContributors and kContributorCount.
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

CXX_EXT = {".h", ".hh", ".hpp", ".hxx", ".cpp", ".cc", ".cxx", ".inc", ".inl", ".ipp", ".mm"}
C_EXT = {".c"}
DECL_DIRS = ("src/", "include/", "tests/", "tools/", "firmware/helixscreen-esp32/")

# Vendored third-party code outside lib/: never rewritten, and its identifiers
# count as third-party names.
THIRD_PARTY = (
    "firmware/helixscreen-esp32/components/esp_websocket_client/",
    "firmware/helixscreen-esp32/components/helixnet/shim/",
    "firmware/helixscreen-esp32/components/helixcore/fonts/",
    "firmware/helixscreen-esp32/components/helixcore/simd/esp_lvgl_port_lv_blend.h",
    "tests/catch_amalgamated.",
)

TEXT_EXT = {".md", ".py", ".sh", ".bats", ".mk"}
TEXT_DIRS = ("docs/", "scripts/", "tests/", "mk/", "tools/", ".claude/rules/", "src/", "include/",
             "firmware/")
TEXT_ROOT_FILES = {"Makefile", "CLAUDE.md", "README.md", "CONTRIBUTING.md"}
DATA_EXT = {".xml", ".json", ".lua", ".yml", ".yaml", ".cfg", ".conf", ".toml", ".ini", ".csv"}

# Files that spell example names on purpose, and history that must not change.
NEVER_REWRITE = (
    "scripts/rename_constants.py",
    "scripts/check_constant_naming.py",
    "tests/python/test_rename_constants.py",
    "CHANGELOG.md",
)

SOURCE_EXT_LIB = CXX_EXT | C_EXT

# ---------------------------------------------------------------------------
# Naming
# ---------------------------------------------------------------------------

UPPER_SNAKE_RE = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*$")


def is_upper_snake(name):
    """True for a name the constant rule renames: MAX_X, PI, RGB565 (not N, T2, _X)."""
    return bool(UPPER_SNAKE_RE.match(name)) and sum(c.isupper() for c in name) >= 2


def to_k_camel(name):
    """MAX_RETRY_COUNT -> kMaxRetryCount; HTTP_TIMEOUT_MS -> kHttpTimeoutMs."""
    return "k" + "".join(w[0] + w[1:].lower() for w in name.split("_"))


# ---------------------------------------------------------------------------
# Lexing
# ---------------------------------------------------------------------------

LEX_RE = re.compile(
    r"""
     (?P<lc>//(?:[^\n\\]|\\.)*)
    |(?P<bc>/\*.*?(?:\*/|\Z))
    |(?P<raw>(?:u8|[uUL])?R"(?P<d>[^()\\\s"]{0,16})\(.*?\)(?P=d)")
    |(?P<str>(?:u8|[uUL])?"(?:[^"\\\n]|\\.)*"?)
    |(?P<num>\.?\d(?:[eEpP][+-]|[\w.']|)*)
    |(?P<chr>(?:u8|[uUL])?'(?:[^'\\\n]|\\.)*'?)
    |(?P<id>[A-Za-z_]\w*)
    """,
    re.S | re.X,
)

WORD_RE = re.compile(r"\b[A-Za-z_]\w*\b")
PP_LINE_RE = re.compile(r"^[ \t]*#(?:[^\n\\]|\\.)*", re.M | re.S)
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.M)


def lex(text):
    """Yield (kind, start, end) for identifiers, comments, strings and numbers.

    kind is 'id', 'comment', 'string' or 'num'; text between yields is
    punctuation and whitespace.
    """
    for m in LEX_RE.finditer(text):
        kind = m.lastgroup
        if kind == "d":
            kind = "raw"
        if kind in ("lc", "bc"):
            yield "comment", m.start(), m.end()
        elif kind in ("raw", "str", "chr"):
            yield "string", m.start(), m.end()
        else:
            yield kind, m.start(), m.end()


def code_only(text):
    """The text with comments, literals and preprocessor lines blanked to spaces.

    Offsets and newlines are preserved, so a match position is a source position.
    """
    return blank_cxx(text, preprocessor=True)


# ---------------------------------------------------------------------------
# Declarations
# ---------------------------------------------------------------------------

# A statement start, a head that names no call or initializer, then the
# declared name with its initializer, array bound, brace init or semicolon.
DECL_RE = re.compile(
    r"(?:^|(?<=[;{}]))(?P<head>[^;{}()=#]*?)\b(?P<name>[A-Z][A-Z0-9_]*)\s*(?=[=\[{;])",
    re.M,
)
CONST_KW_RE = re.compile(r"\b(?:const|constexpr|constinit)\b")
NOT_A_DECL_RE = re.compile(
    r"\b(?:return|case|goto|using|typedef|operator|enum|throw|co_return|co_yield|delete|new)\b"
)
ENUM_RE = re.compile(
    r"\benum\b(?:\s+(?:class|struct)\b)?(?:\s*\[\[[^\]]*\]\])?(?:\s+[A-Za-z_][\w:]*)?"
    r"(?:\s*:\s*[\w:\s]+?)?\s*\{(?P<body>[^{}]*)\}"
)


def find_const_decls(code):
    """(name, offset) for each UPPER_SNAKE const-declared variable in blanked code."""
    found = []
    for m in DECL_RE.finditer(code):
        head, name = m.group("head"), m.group("name")
        if not is_upper_snake(name):
            continue
        if not CONST_KW_RE.search(head) or NOT_A_DECL_RE.search(head):
            continue
        # `Foo<A, B> NAME`: the head ends at the name, so a stray `.` or `->`
        # means the name is a member access, not a declaration.
        tail = head.rstrip()
        if tail.endswith((".", "->")):
            continue
        found.append((name, m.start("name")))
    return found


def find_enumerators(code):
    """Every enumerator name in blanked code."""
    names = []
    for m in ENUM_RE.finditer(code):
        for part in m.group("body").split(","):
            em = re.match(r"\s*([A-Za-z_]\w*)", part)
            if em:
                names.append(em.group(1))
    return names


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------


def git_files(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, stdout=subprocess.PIPE,
                         check=True).stdout.decode("utf-8", "replace")
    return sorted(p for p in out.split("\0") if p and (root / p).is_file())


def lib_source_files(root):
    """Source files under lib/, following the worktree's shared-checkout symlinks."""
    files = []
    lib = root / "lib"
    if not lib.is_dir():
        return files
    for dirpath, dirnames, filenames in os.walk(lib, followlinks=True):
        dirnames[:] = sorted(d for d in dirnames if d != ".git" and not d.startswith("build"))
        for f in sorted(filenames):
            if Path(f).suffix in SOURCE_EXT_LIB:
                files.append(Path(dirpath) / f)
    return files


def read(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="surrogateescape")
    except OSError:
        return ""


def is_third_party(rel):
    return rel.startswith("lib/") or any(rel.startswith(p) for p in THIRD_PARTY)


def is_generated(text):
    return "do not edit" in "\n".join(text.splitlines()[:12]).lower()


def in_dirs(rel, dirs):
    return any(rel.startswith(d) for d in dirs)


def classify(rel):
    """'cxx', 'text', 'data' or None: how a tracked file takes part."""
    if rel in NEVER_REWRITE or is_third_party(rel):
        return None
    ext = Path(rel).suffix
    if ext in CXX_EXT | C_EXT:
        return "cxx" if in_dirs(rel, DECL_DIRS) else None
    if ext in DATA_EXT:
        return "data"
    if "/" not in rel:
        return "text" if rel in TEXT_ROOT_FILES else None
    if ext in TEXT_EXT and in_dirs(rel, TEXT_DIRS):
        return "text"
    if ext == ".txt" and rel.startswith("scripts/"):
        return "text"
    if Path(rel).name == "Makefile" and in_dirs(rel, TEXT_DIRS):
        return "text"
    return None


# ---------------------------------------------------------------------------
# Exclusion sets
# ---------------------------------------------------------------------------

BUILD_DEFINE_RE = re.compile(r"(?<![\w-])-D\s*([A-Za-z_]\w*)")
FORCED_INCLUDE_RE = re.compile(r"(?<![\w-])-include\s+(?:\$\([A-Z_]+\)/)?([\w./-]+\.h\w*)")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]', re.M)
KCONFIG_RE = re.compile(r"^\s*(?:menu)?config\s+([A-Za-z_]\w*)", re.M)
STRINGIFY_MACRO_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)\(([^)]*)\)((?:[^\n\\]|\\.)*)", re.M | re.S
)


def stringifying_macros(text):
    """Names of function-like macros in text that stringify a parameter with #."""
    names = set()
    for m in STRINGIFY_MACRO_RE.finditer(text):
        params = [p.strip() for p in m.group(2).split(",")]
        body = m.group(3)
        for p in params:
            p = "__VA_ARGS__" if p == "..." else p
            if p and re.search(r"(?<!#)#\s*" + re.escape(p) + r"\b", body):
                names.add(m.group(1))
                break
    return names


def macro_args(text, macro):
    """Concatenated argument text of each invocation of macro in text."""
    args = []
    for m in re.finditer(r"\b" + re.escape(macro) + r"\s*\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        args.append(text[m.end():i - 1])
    return "\n".join(args)


class Tree:
    """Everything the plan needs to know about one checkout."""

    def __init__(self, root):
        self.root = Path(root)
        self.files = git_files(self.root)
        self.texts = {}
        self.code = {}

    def text(self, rel):
        if rel not in self.texts:
            self.texts[rel] = read(self.root / rel)
        return self.texts[rel]

    def blanked(self, rel):
        if rel not in self.code:
            self.code[rel] = code_only(self.text(rel))
        return self.code[rel]


def scan(tree):
    """Collect candidates and every exclusion set from the tree."""
    decls = {}             # name -> [site]
    generated_names = set()
    enumerators = {}       # name -> count
    macros = set()
    third_party_ids = set()
    our_ids = set()
    data_words = set()
    whole_string_names = set()
    stringifiers = set()
    cxx_files = []
    forced = set()
    ids_by_file = {}

    for rel in tree.files:
        kind = classify(rel)
        path_ext = Path(rel).suffix
        if kind == "cxx":
            cxx_files.append(rel)
        elif kind == "data":
            data_words.update(WORD_RE.findall(tree.text(rel)))
        elif is_third_party(rel) and path_ext in SOURCE_EXT_LIB:
            text = tree.text(rel)
            macros.update(DEFINE_RE.findall(text))
            third_party_ids.update(text[s:e] for k, s, e in lex(text) if k == "id")
        # Build flags and Kconfig define macros too.
        name = Path(rel).name
        if name == "Makefile" or path_ext in (".mk", ".cmake") or name == "CMakeLists.txt":
            macros.update(BUILD_DEFINE_RE.findall(tree.text(rel)))
            forced.update(FORCED_INCLUDE_RE.findall(tree.text(rel)))
        elif name.startswith("Kconfig"):
            macros.update("CONFIG_" + n for n in KCONFIG_RE.findall(tree.text(rel)))
        elif name.startswith("sdkconfig"):
            macros.update(re.findall(r"^#?\s*(CONFIG_\w+)", tree.text(rel), re.M))

    for path in lib_source_files(tree.root):
        text = read(path)
        macros.update(DEFINE_RE.findall(text))
        third_party_ids.update(text[s:e] for k, s, e in lex(text)
                               if k == "id" and text[s].isupper())

    for rel in cxx_files:
        text = tree.text(rel)
        macros.update(DEFINE_RE.findall(text))
        stringifiers.update(stringifying_macros(text))
        ids = ids_by_file.setdefault(rel, set())
        for kind, s, e in lex(text):
            if kind == "id":
                ids.add(text[s:e])
            elif kind == "string":
                lit = text[s:e]
                inner = re.sub(r'^(?:u8|[uUL])?R?"(?:[^(]*\()?|(?:\)[^)"]*)?"$', "", lit)
                if UPPER_SNAKE_RE.match(inner):
                    whole_string_names.add(inner)

    for ids in ids_by_file.values():
        our_ids |= ids

    for rel in cxx_files:
        text = tree.text(rel)
        code = tree.blanked(rel)
        gen = is_generated(text)
        if Path(rel).suffix not in C_EXT:
            for name, off in find_const_decls(code):
                decls.setdefault(name, []).append(f"{rel}:{line_of(text, off)}")
                if gen:
                    generated_names.add(name)
        for name in find_enumerators(code):
            enumerators[name] = enumerators.get(name, 0) + 1
        if gen:
            generated_names.update(WORD_RE.findall(code))

    stringified = set()
    for rel in cxx_files:
        code = tree.blanked(rel)
        for macro in stringifiers:
            if macro in code:
                stringified.update(WORD_RE.findall(macro_args(code, macro)))

    return {
        "decls": decls,
        "generated": generated_names,
        "enumerators": enumerators,
        "macros": macros,
        "third_party": third_party_ids,
        "our_ids": our_ids,
        "data": (data_words | whole_string_names | stringified),
        "stringifiers": sorted(stringifiers),
        "ids_by_file": ids_by_file,
        "includes": IncludeGraph(tree, cxx_files, forced),
    }


class IncludeGraph:
    """Which of our C++ files can see which: the #include edges between them.

    An include resolves to every scanned file whose path ends with it, so an
    ambiguous one over-connects, which only ever keeps a name from renaming.
    """

    def __init__(self, tree, cxx_files, forced):
        by_name = {}
        for rel in cxx_files:
            by_name.setdefault(Path(rel).name, []).append(rel)

        def resolve(inc):
            return [rel for rel in by_name.get(Path(inc).name, ())
                    if rel == inc or rel.endswith("/" + inc)]

        self.includes, self.includers = {}, {}
        for rel in cxx_files:
            for inc in INCLUDE_RE.findall(tree.text(rel)):
                for target in resolve(inc.strip()):
                    if target != rel:
                        self.includes.setdefault(rel, set()).add(target)
                        self.includers.setdefault(target, set()).add(rel)
        # A forced -include, and everything it includes, is in every unit.
        self.forced = self._closure([t for inc in forced for t in resolve(inc)], self.includes)

    @staticmethod
    def _closure(roots, edges):
        seen, todo = set(roots), list(roots)
        while todo:
            for other in edges.get(todo.pop(), ()):
                if other not in seen:
                    seen.add(other)
                    todo.append(other)
        return seen

    def share_a_unit(self, files_a, files_b):
        """True if some translation unit can see a file of each set."""
        if not files_a or not files_b:
            return False
        if self.forced & (set(files_a) | set(files_b)):
            return True
        return bool(self._closure(files_a, self.includers)
                    & self._closure(files_b, self.includers))


# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------


def build_plan(tree):
    s = scan(tree)
    entries = {}
    for old, sites in s["decls"].items():
        new = to_k_camel(old)
        reason = detail = ""
        if old in s["macros"]:
            reason = "macro"
        elif old in s["enumerators"]:
            reason = "enumerator"
        elif old in s["third_party"]:
            reason = "third-party"
        elif old in s["generated"]:
            reason = "generated"
        elif old in s["data"]:
            reason = "data"
        elif new in s["third_party"]:
            reason = "collision"
            detail = f"{new} is a third-party identifier"
        elif new in s["our_ids"]:
            olds = [f for f, ids in s["ids_by_file"].items() if old in ids]
            news = [f for f, ids in s["ids_by_file"].items() if new in ids]
            if s["includes"].share_a_unit(olds, news):
                reason = "collision"
                detail = f"{new} is visible beside it: {' '.join(sorted(news)[:3])}"
                if len(news) > 3:
                    detail += f" (+{len(news) - 3} more)"
        entries[old] = {"new": new, "reason": reason, "sites": sorted(set(sites)),
                        "detail": detail if reason == "collision" else ""}

    by_new = {}
    for old, e in entries.items():
        if not e["reason"]:
            by_new.setdefault(e["new"], []).append(old)
    for new, olds in by_new.items():
        if len(olds) > 1:
            for old in olds:
                entries[old]["reason"] = "collision"
                entries[old]["detail"] = f"{' and '.join(sorted(olds))} both convert to {new}"

    upper_enumerators = sum(n for name, n in s["enumerators"].items() if is_upper_snake(name))
    return entries, upper_enumerators, s["stringifiers"], s["ids_by_file"]


def skip_lines(entries, names=None):
    """One line per skipped name (of names, when given): reason, old -> new,
    declaration sites, and what it collides with."""
    lines = []
    for old in sorted(entries):
        e = entries[old]
        if not e["reason"] or (names is not None and old not in names):
            continue
        detail = f"  [{e['detail']}]" if e.get("detail") else ""
        lines.append(f"  {e['reason']:<11} {old} -> {e['new']}  {' '.join(e['sites'])}{detail}")
    return lines


def print_skips(entries):
    lines = skip_lines(entries)
    if lines:
        print(f"skipped {len(lines)} name(s), left UPPER_SNAKE:")
        print("\n".join(lines))


def write_plan(path, entries, upper_enumerators, stringifiers):
    counts = {}
    for e in entries.values():
        key = e["reason"] or "rename"
        counts[key] = counts.get(key, 0) + 1
    lines = ["# scripts/rename_constants.py plan: status<TAB>old<TAB>new<TAB>declaration sites"]
    for key in sorted(counts):
        lines.append(f"# {key}: {counts[key]}")
    lines.append(f"# UPPER_SNAKE enumerators left alone: {upper_enumerators}")
    lines.append(f"# stringifying macros: {' '.join(stringifiers) or '(none)'}")
    for old in sorted(entries):
        e = entries[old]
        lines.append("\t".join([e["reason"] or "rename", old, e["new"], " ".join(e["sites"])]))
    Path(path).write_text("\n".join(lines) + "\n")
    return counts


def read_mapping(path):
    mapping = {}
    for line in Path(path).read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) >= 3 and parts[0] == "rename":
            mapping[parts[1]] = parts[2]
    return mapping


# ---------------------------------------------------------------------------
# Rewriting
# ---------------------------------------------------------------------------


def rewrite_cxx(text, mapping):
    """Rename identifiers and comment words; literals are left as they are.

    A comment word with no underscore (TOTAL, COLORS) is often plain emphasis,
    so it is renamed only when this file's code uses that name.
    """
    if not mapping:
        return text
    spans = list(lex(text))
    used = {text[s:e] for k, s, e in spans if k == "id"}
    comment_map = {k: v for k, v in mapping.items() if "_" in k or k in used}
    out, last = [], 0
    for kind, s, e in spans:
        if kind == "id":
            tok = text[s:e]
            if tok in mapping:
                out.append(text[last:s])
                out.append(mapping[tok])
                last = e
        elif kind == "comment":
            out.append(text[last:s])
            out.append(sub_words(text[s:e], comment_map))
            last = e
    out.append(text[last:])
    return "".join(out)


# A word followed by a file extension names a file (CONTRIBUTORS.txt), and one
# behind `$` is a shell variable.
PROSE_WORD_RE = re.compile(
    r"(?<![\w$])[A-Za-z_]\w*\b"
    r"(?!\.(?:txt|md|h|hh|hpp|cpp|cc|c|json|xml|sh|py|yml|yaml|cfg|conf|log|png|bin|mk|bats|lua|csv|tsv)\b)"
)


def sub_words(chunk, mapping):
    if not mapping:
        return chunk
    return PROSE_WORD_RE.sub(lambda m: mapping.get(m.group(0), m.group(0)), chunk)


def script_variables(text, names):
    """Names text uses as a shell, Python or make variable rather than a C++ reference."""
    used = set()
    for name in names:
        n = re.escape(name)
        if re.search(r"\$\{?" + n + r"\b|\$\(" + n + r"\)|(?<![\w.])" + n + r"\s*[:+?]?=(?!=)"
                     r"|\b(?:export|local|declare|readonly|unset|import)\s+(?:-\w+\s+)*" + n + r"\b"
                     r"|\bimport\b[^\n]*\b" + n + r"\b",
                     text):
            used.add(name)
    return used


MD_CODE_RE = re.compile(r"```.*?(?:```|\Z)|`[^`\n]+`", re.S)


def rewrite_text(text, mapping, markdown=False):
    """Docs and scripts: names with an underscore anywhere; a bare word such as
    TOTAL or CONTRIBUTORS is far likelier to be English or a file name, so it is
    renamed only inside Markdown code spans."""
    present = {w for w in set(WORD_RE.findall(text)) if w in mapping}
    if not present:
        return text
    keep = script_variables(text, present)
    local = {k: v for k, v in mapping.items() if k in present and k not in keep}
    snake = {k: v for k, v in local.items() if "_" in k}
    if not markdown:
        return sub_words(text, snake)
    out, last = [], 0
    for m in MD_CODE_RE.finditer(text):
        out.append(sub_words(text[last:m.start()], snake))
        out.append(sub_words(m.group(0), local))
        last = m.end()
    out.append(sub_words(text[last:], snake))
    return "".join(out)


def rewrite_file(tree, rel, mapping):
    """True if the file changed."""
    kind = classify(rel)
    if kind not in ("cxx", "text"):
        return False
    text = tree.text(rel)
    if kind == "cxx" and is_generated(text):
        return False
    if kind == "cxx":
        new = rewrite_cxx(text, mapping)
    else:
        new = rewrite_text(text, mapping, markdown=rel.endswith(".md"))
    if new == text:
        return False
    (tree.root / rel).write_text(new, encoding="utf-8", errors="surrogateescape")
    tree.texts[rel] = new
    tree.code.pop(rel, None)
    return True


FORMAT_EXT = {".cpp", ".c", ".h", ".mm", ".hpp", ".cc"}


def pinned_clang_format(root):
    """The clang-format the commit hook accepts (scripts/qc/phase2.sh), or None.

    A rename shortens names, so a line may now fit where clang-format would
    join it; formatting with the pinned binary makes the output exactly what
    the hook would leave.
    """
    req = (Path(root) / "requirements.txt")
    pin = re.search(r"^clang-format==([0-9.]+)", read(req), re.M)
    cand = os.environ.get("CLANG_FORMAT") or str(Path(root) / ".venv/bin/clang-format")
    if not pin:
        return None
    try:
        ver = subprocess.run([cand, "--version"], stdout=subprocess.PIPE, text=True).stdout
    except OSError:
        return None
    return cand if pin.group(1) in ver else None


def format_files(root, rels):
    code = [r for r in rels if Path(r).suffix in FORMAT_EXT]
    if not code:
        return 0
    fmt = pinned_clang_format(root)
    if not fmt:
        print("warning: pinned clang-format not found (make venv-setup); "
              f"{len(code)} rewritten file(s) left unformatted", file=sys.stderr)
        return 0
    batches = [code[i:i + 50] for i in range(0, len(code), 50)]
    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
        for r in pool.map(lambda b: subprocess.run([fmt, "-i", *b], cwd=root), batches):
            r.check_returncode()
    return len(code)


def files_in_range(root, rng):
    out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=ACMR", rng], cwd=root,
                         stdout=subprocess.PIPE, check=True).stdout.decode()
    return sorted(p for p in out.splitlines() if p)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--plan", metavar="FILE", help="write the mapping and skip list here")
    ap.add_argument("--apply", action="store_true", help="rewrite files")
    ap.add_argument("--mapping", metavar="FILE", action="append", default=[],
                    help="also apply the renames of an earlier plan")
    ap.add_argument("--files", nargs="+", metavar="PATH", help="rewrite only these files")
    ap.add_argument("--git-range", metavar="A..B", help="rewrite only files this range changes")
    ap.add_argument("--allow-skips", action="store_true",
                    help="with --git-range, exit 0 even when the range declares skipped names")
    ap.add_argument("--root", default=str(ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if not args.plan and not args.apply:
        ap.error("give --plan FILE, --apply, or both")

    tree = Tree(args.root)
    entries, upper_enums, stringifiers, ids_by_file = build_plan(tree)
    if args.plan:
        counts = write_plan(args.plan, entries, upper_enums, stringifiers)
        summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
        print(f"plan: {summary}; enumerators left {upper_enums} -> {args.plan}")
        if not args.apply:
            print_skips(entries)

    if not args.apply:
        return 0
    mapping = {old: e["new"] for old, e in entries.items() if not e["reason"]}
    for path in args.mapping:
        for old, new in read_mapping(path).items():
            mapping.setdefault(old, new)

    if args.files:
        targets = sorted({os.path.relpath(Path(f).resolve(), tree.root) for f in args.files})
    elif args.git_range:
        targets = files_in_range(tree.root, args.git_range)
    else:
        targets = tree.files
    changed = [rel for rel in targets if (tree.root / rel).is_file()
               and rewrite_file(tree, rel, mapping)]
    formatted = format_files(tree.root, changed)
    print(f"apply: {len(mapping)} names, {len(changed)} files changed ({formatted} formatted)")
    print_skips(entries)

    if not args.git_range:
        return 0
    flagged = range_skips(tree.root, args.git_range, targets, entries)
    if not flagged:
        return 0
    print(f"\n*** {len(flagged)} declaration(s) in {args.git_range} keep a skipped "
          "UPPER_SNAKE name, and the commit gate (check_constant_naming.py) flags each:")
    for path, lineno, name in flagged:
        e = entries[name]
        detail = f"  [{e['detail']}]" if e.get("detail") else ""
        print(f"  {path}:{lineno}: {name} -> {e['new']}  ({e['reason']}){detail}")
    if args.allow_skips:
        return 0
    print("*** rename them by hand, or mark each `// CONST_NAME_OK: <reason>`; "
          "--allow-skips accepts them as they are")
    return 1


def range_skips(root, rng, targets, entries):
    """[(path, lineno, name)] for each skipped name the range declares on a line
    the commit gate reads as new: the range's files as they are on disk now,
    against the merge base of its ends."""
    from check_constant_naming import findings  # it imports this module

    a, _, b = rng.partition("...") if "..." in rng else rng.partition("..")
    base = subprocess.run(["git", "merge-base", a or "HEAD", b or "HEAD"], cwd=root,
                          stdout=subprocess.PIPE, check=True).stdout.decode().strip()
    code = [rel for rel in targets if classify(rel) == "cxx"]
    if not code:
        return []
    diff = subprocess.run(["git", "diff", "-U0", "--no-ext-diff", "--diff-filter=AM", base, "--",
                           *code], cwd=root, stdout=subprocess.PIPE,
                          check=True).stdout.decode("utf-8", "replace")
    skipped = {old for old, e in entries.items() if e["reason"]}
    return [(path, lineno, name) for path, lineno, name, _ in
            findings(diff, lambda p: read(Path(root) / p)) if name in skipped]


if __name__ == "__main__":
    sys.exit(main())
