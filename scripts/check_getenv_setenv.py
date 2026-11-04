#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint: a getenv() pointer must not be read after a setenv may have replaced it.
#
# setenv() may reallocate the environ block and the strings it points at, so a
# `const char*` captured from getenv() and dereferenced after any later
# setenv()/unsetenv() reads whatever bytes live at that address now: a
# corrupted variable inherited by every std::system() child the test spawns
# (prestonbrown/helixscreen#1537). Copy the value into a std::string at capture
# (helix::ScopedEnv in tests/test_helpers/scoped_env.h does this, and its
# (name, value) constructor owns the set too) and restore from the copy.
#
# This is a spelling-level scanner, not a C++ parser, and it is NOT exhaustive.
# It sees char and auto* declarations (west-const, east-const, pointer-const,
# wrapped after the name) and bare `auto` initialized by the getenv() call
# itself, retires each capture at its closing brace (a capture declared in a
# control-statement header whose body opens on the same line retires with that
# statement), and blanks comments, literal contents and qualified names
# (`x.name`, `ptr->name`, `ns::name`, `name::member`) before matching. A read
# is stale only when a setenv()/unsetenv() call has COMPLETED - its closing
# paren sits between the capture and the read - so an argument of a call still
# open on the read's line stays quiet, wrapped or not, while a read after a
# call that closed earlier on the same line flags. Known-uncovered spellings:
# assignment-form capture (`const char* p; p = getenv(..)`), direct-init
# (`const char* p(getenv(..))`), the second declarator of a comma declaration
# (`const char* a = getenv(..), *b = getenv(..)`), a declaration wrapped
# BEFORE the name (`const char*\np = getenv(..)`), and bare `auto` with the
# getenv() on a continuation line. The 400-line backstop window exists only so
# a pathological single function cannot pin a name forever. Residual
# false-positive shapes: a same-named local in a NESTED scope of the same
# function, and a control-statement header that WRAPS across lines (its
# capture retires only at the function's closing brace).
#
# Usage: check_getenv_setenv.py ROOT...   (a missing root fails)

import bisect
import pathlib
import re
import sys

# (const )?char( const)? * (const )? NAME =  — west-const, east-const and
# pointer-const spellings; (?!=) keeps == comparisons out.
DECL = re.compile(r'(?:const\s+)?char\s*(?:const\s*)?\*\s*(?:const\s*)?'
                  r'([A-Za-z_]\w*)\s*=(?!=)')
# `auto*` deduces the same raw pointer `char*` spells.
DECL_AUTO_PTR = re.compile(r'(?:const\s+)?auto\s*\*\s*(?:const\s*)?'
                           r'([A-Za-z_]\w*)\s*=(?!=)')
# Bare `auto` is tracked only when the initializer IS the getenv() call: a
# value captured straight into owned storage must not flag the copy's users.
DECL_AUTO_DIRECT = re.compile(r'\bauto\s+([A-Za-z_]\w*)\s*=\s*'
                              r'(?:std\s*::\s*)?getenv\s*\(')
GETENV = re.compile(r'\bgetenv\s*\(')
SETENV = re.compile(r'\b(?:setenv|unsetenv)\s*\(')
WINDOW = 400
WRAP_MAX = 5  # continuation lines scanned for getenv() in a wrapped initializer

def strip_comments_and_strings(lines):
    """Blank out // and /* */ comments and the CONTENTS of string and char
    literals, quote-aware, so commented-out code cannot trip the scan, a '//'
    inside a string keeps its line, and a name mentioned only inside a log
    message is not a use."""
    out = []
    in_block = False
    for raw in lines:
        kept = []
        i = 0
        quote = None  # '"' or "'" while inside that literal
        while i < len(raw):
            c = raw[i]
            if in_block:
                if raw[i:i + 2] == '*/':
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if quote:
                if c == '\\':
                    kept.append('  ')
                    i += 2
                    continue
                if c == quote:
                    quote = None
                    kept.append(c)
                else:
                    kept.append(' ')
                i += 1
                continue
            if c in '"\'':
                quote = c
                kept.append(c)
                i += 1
                continue
            if raw[i:i + 2] == '//':
                break
            if raw[i:i + 2] == '/*':
                in_block = True
                i += 2
                continue
            kept.append(c)
            i += 1
        out.append(''.join(kept))
    return out

def env_call_completions(text):
    """Offsets of every setenv()/unsetenv() call's closing paren, wrapped
    calls included: a read is stale only once the call has completed."""
    done = []
    for m in SETENV.finditer(text):
        j = m.end()
        paren = 1
        while j < len(text) and paren:
            if text[j] == '(':
                paren += 1
            elif text[j] == ')':
                paren -= 1
            j += 1
        if paren == 0:
            done.append(j - 1)
    return sorted(done)

def line_depths(line, start):
    """Depth at each column, the minimum depth over each suffix, and the
    depth at line end, for scope retirement."""
    at = []
    run = start
    for c in line:
        at.append(run)
        if c == '{':
            run += 1
        elif c == '}':
            run -= 1
    at.append(run)
    suffix_min = list(at)
    for c in range(len(at) - 2, -1, -1):
        suffix_min[c] = min(suffix_min[c], suffix_min[c + 1])
    return at, suffix_min, run

def register_capture(m, line, i, lines, active, pos, depth_at, retire_col):
    initializer = line[m.end():]
    # '{' ends an initializer the same way ';' does: a parameter default like
    # `const char* value = nullptr) : m_(m) {` must not swallow the
    # constructor body's getenv() as its own
    terminated = (';' in initializer) or ('{' in initializer)
    if not terminated:
        for cont in lines[i + 1:i + 1 + WRAP_MAX]:
            stop = re.search(r'[;{]', cont)
            if stop:
                initializer += ' ' + cont[:stop.start() + 1]
                break
            initializer += ' ' + cont
    if GETENV.search(initializer):
        # a redeclaration of the same name starts a new capture
        active[m.group(1)] = [i, retire_col, pos, depth_at]

bad = 0
for arg in sys.argv[1:]:
    root = pathlib.Path(arg)
    if not root.exists():
        print(f"scan root does not exist: {arg}")
        bad = 1
        continue
    paths = sorted(root.rglob('*')) if root.is_dir() else [root]
    for path in paths:
        if path.suffix not in ('.cpp', '.h', '.cc', '.hpp', '.c'):
            continue
        try:
            raw = path.read_text(errors='replace')
        except OSError:
            continue
        # A read flags only after a setenv()/unsetenv() call, and a file that
        # never spells setenv has none: skip it before the character-level
        # strip that dominates the scan's cost.
        if 'setenv' not in raw:
            continue
        lines = strip_comments_and_strings(raw.splitlines())
        text = '\n'.join(lines)
        completions = env_call_completions(text)
        starts = []
        o = 0
        for ln in lines:
            starts.append(o)
            o += len(ln) + 1
        depth = 0  # brace depth; a capture dies when its scope closes
        active = {}  # name -> [capture line, retire col, offset, depth]
        for i, line in enumerate(lines):
            at, smin, depth = line_depths(line, depth)
            for rx in (DECL, DECL_AUTO_PTR):
                for m in rx.finditer(line):
                    prefix = line[:m.start(1)]
                    # A capture declared in a for/if/while header whose body
                    # opens on the same line is scoped to that statement: one
                    # level deeper, and it retires from the body's opening
                    # brace rather than from the name.
                    in_header = (prefix.count('(') - prefix.count(')') > 0 and
                                 re.search(r'\b(?:for|if|while)\b[^;{}]*$', prefix) and
                                 '{' in line[m.end(1):])
                    retire_col = (line.rfind('{', m.end(1)) + 1
                                  if in_header else m.end(1))
                    register_capture(m, line, i, lines, active,
                                     starts[i] + m.start(1),
                                     at[m.start(1)] + (1 if in_header else 0),
                                     retire_col)
            for m in DECL_AUTO_DIRECT.finditer(line):
                active[m.group(1)] = [i, m.end(1), starts[i] + m.start(1),
                                      at[m.start(1)]]
            # A read is stale only when a setenv()/unsetenv() completed at an
            # offset between the capture and the read: an argument of a call
            # still open at the read's position is evaluated before that call
            # runs, while a read after a closing paren earlier on the same
            # line is post-setenv and flags.
            for name, state in list(active.items()):
                if i < state[0] or i > state[0] + WINDOW:
                    continue
                # cfg.name, ptr->name, ns::name and name::member are different
                # entities, not uses of the captured local; blanked same-length
                # so offsets stay valid
                quiet = re.sub(r'(?:\.|->|::)\s*' + re.escape(name) + r'\b' +
                               r'|\b' + re.escape(name) + r'\s*::',
                               lambda mm: ' ' * len(mm.group(0)), line)
                for um in re.finditer(r'\b' + re.escape(name) + r'\b', quiet):
                    k = bisect.bisect_right(completions, state[2])
                    if (k < len(completions) and
                            completions[k] < starts[i] + um.start()):
                        print(f"{path}:{state[0] + 1}: '{name}' from getenv() is read at "
                              f"line {i + 1}, after a setenv() between the two; copy the "
                              f"value into a std::string at capture (helix::ScopedEnv)")
                        bad = 1
                        del active[name]
                        break
            # The running depth dipping below the depth at capture closes the
            # capture's scope, so one inline method's local never reaches the
            # next method's same-named local.
            for name, state in list(active.items()):
                if i == state[0]:
                    if smin[state[1]] < state[3]:
                        del active[name]
                elif smin[0] < state[3]:
                    del active[name]
sys.exit(bad)
