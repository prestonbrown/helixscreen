#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The distribution partner table: what each firmware mod pulls from a release.

One data file, scripts/distribution_partners.json, feeds both readers, so the doc and the
release check cannot disagree:

  check DIR --tag TAG [--platforms "pi mips ..."]
      Fail when DIR (release.yml's release-files/) lacks an asset a partner fetches. With
      --platforms, only assets built by a listed matrix platform (and install.sh) are
      required, so a dry run of a platform subset still passes. A rename then fails the tag
      instead of leaving a mod's users unable to update, with no error on our side (#1911).
  table
      Print the markdown table.
  doc FILE [--write]
      Fail when FILE's block between the partner-table markers is not the rendered table;
      --write replaces it.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, 'distribution_partners.json')
BEGIN = '<!-- distribution-partners:begin (scripts/distribution_partners.py doc --write) -->'
END = '<!-- distribution-partners:end -->'
INSTALLER_BUILT_BY = 'installer'


def load(path):
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    partners = data.get('partners')
    if not isinstance(partners, list) or not partners:
        raise ValueError(f'{path}: no partners')
    for p in partners:
        for key in ('id', 'name', 'finds_release', 'install_source', 'owner', 'contact', 'fetches'):
            if key not in p:
                raise ValueError(f"{path}: partner {p.get('id', '?')} has no '{key}'")
        for f in p['fetches']:
            if not f.get('asset') or not f.get('built_by'):
                raise ValueError(f"{path}: partner {p['id']} has a fetch without asset/built_by")
    return partners


def required_assets(partners, tag, platforms=None):
    """{asset name: [partner names]} for every asset the release must carry."""
    wanted = None if platforms is None else set(platforms)
    need = {}
    for p in partners:
        for f in p['fetches']:
            built_by = f['built_by']
            if wanted is not None and built_by != INSTALLER_BUILT_BY and built_by not in wanted:
                continue
            name = f['asset'].replace('{tag}', tag)
            need.setdefault(name, [])
            if p['name'] not in need[name]:
                need[name].append(p['name'])
    return need


def cmd_check(args):
    partners = load(args.data)
    platforms = args.platforms.split() if args.platforms is not None else None
    need = required_assets(partners, args.tag, platforms)
    missing = 0
    for name in sorted(need):
        if os.path.isfile(os.path.join(args.dir, name)):
            print(f'  ok: {name}')
        else:
            print(f"::error::release is missing {name}, which {', '.join(need[name])} fetches "
                  f'by that name (scripts/distribution_partners.json)')
            missing += 1
    if missing:
        print(f'{missing} partner asset(s) missing; see docs/devel/INSTALLER.md '
              f'§ Distribution Partners', file=sys.stderr)
        return 1
    print(f'All {len(need)} partner assets present')
    return 0


def cell(text):
    return str(text).replace('|', '\\|').replace('\n', ' ')


def render_table(partners):
    rows = [
        '| Partner | Hardware | Fetches (exact asset name) | How it finds the release '
        '| Install source flag | How it starts us | Incumbent screen | Owner | Contact | Sources |',
        '|---|---|---|---|---|---|---|---|---|---|',
    ]
    for p in partners:
        if p['fetches']:
            fetches = '<br>'.join(
                f"`{f['asset']}`" + (' (via install.sh)' if f.get('via') == 'installer' and f['asset'] != 'install.sh' else '')
                for f in p['fetches'])
        else:
            fetches = 'Nothing'
        sources = '<br>'.join(cell(s) for s in p.get('sources', []))
        rows.append('| ' + ' | '.join([
            f"**{cell(p['name'])}** ({cell(p.get('upstream', ''))})",
            cell(p.get('hardware', '')),
            fetches,
            cell(p['finds_release']),
            cell(p['install_source']),
            cell(p.get('starts', '')),
            cell(p.get('incumbent_screen', '')),
            cell(p['owner']),
            cell(p['contact']),
            sources,
        ]) + ' |')
    return '\n'.join(rows) + '\n'


def cmd_table(args):
    sys.stdout.write(render_table(load(args.data)))
    return 0


def cmd_doc(args):
    table = render_table(load(args.data))
    with open(args.file, encoding='utf-8') as f:
        text = f.read()
    start = text.find(BEGIN)
    stop = text.find(END)
    if start < 0 or stop < start:
        print(f'{args.file}: no partner-table markers ({BEGIN} ... {END})', file=sys.stderr)
        return 2
    head = text[:start + len(BEGIN)] + '\n'
    tail = text[stop:]
    want = head + table + tail
    if want == text:
        return 0
    if args.write:
        with open(args.file, 'w', encoding='utf-8') as f:
            f.write(want)
        print(f'{args.file}: partner table rewritten')
        return 0
    print(f'{args.file}: the partner table does not match scripts/distribution_partners.json; '
          f'run scripts/distribution_partners.py doc {args.file} --write', file=sys.stderr)
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--data', default=DATA, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check')
    c.add_argument('dir')
    c.add_argument('--tag', required=True, help='release tag, e.g. v1.2.3')
    c.add_argument('--platforms', help='space-separated matrix platforms this run built')
    c.set_defaults(func=cmd_check)
    t = sub.add_parser('table')
    t.set_defaults(func=cmd_table)
    d = sub.add_parser('doc')
    d.add_argument('file')
    d.add_argument('--write', action='store_true')
    d.set_defaults(func=cmd_doc)
    args = ap.parse_args(argv)
    if args.cmd == 'check' and not os.path.isdir(args.dir):
        print(f'{args.dir}: not a directory', file=sys.stderr)
        return 2
    try:
        return args.func(args)
    except (OSError, ValueError) as e:
        print(f'error: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
