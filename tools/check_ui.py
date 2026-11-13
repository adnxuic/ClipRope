"""Check native UI escape hatches and source-bound visual acceptance records."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UI_CHECKS = ('shared_components','control_states','edge_states','input','accessibility',
             'localization_layout','failure','resources','visual_review','performance','physical','automation')
LEX = re.compile(
    r'(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?:u8|[LuU])?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<string>(?:u8|[LuU])?"(?:\\[\s\S]|[^"\\])*")'
    r"|(?P<char>(?:u8|[LuU])?'(?:\\[\s\S]|[^'\\])*')"
    r'|(?P<number>0[xX][0-9a-fA-F]+[uUlL]*)'
    r'|(?P<id>[A-Za-z_]\w*)|(?P<punct>[^\s])')


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    offset: int
    line: int
    detail: str


def literal(token: str) -> str:
    """Decode just C++ glyph escapes, without interpreting arbitrary user text."""
    if re.match(r'(?:u8|[LuU])?R"',token):
        return token[token.index('(')+1:token.rindex(')')]
    value = re.sub(r'^(?:u8|[LuU])?', '', token)[1:-1]
    return re.sub(r'\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8})|x([0-9a-fA-F]{1,8}))',
                  lambda m: chr(int(next(g for g in m.groups() if g),16))
                  if int(next(g for g in m.groups() if g),16)<=0x10FFFF else m[0],value)


def scan(path: str, source: str) -> list[Finding]:
    tokens = [m for m in LEX.finditer(source) if m.lastgroup != 'comment']
    hits: dict[tuple[str, int], Finding] = {}

    def add(rule, token, detail):
        start = token.start()
        hits[rule, start] = Finding(rule, path, start, source.count('\n', 0, start) + 1, detail)

    def arguments(index):
        args, begin, depth = [], index + 2, 0
        for j in range(begin, len(tokens)):
            text = tokens[j][0]
            if text == ')' and depth == 0:
                return args + [tokens[begin:j]]
            if text == ',' and depth == 0:
                args.append(tokens[begin:j]); begin = j + 1
            elif text in ('(', '{', '['):
                depth += 1
            elif text in (')', '}', ']'):
                depth -= 1
        return []

    def glyphs(items):
        for t in items:
            if t.lastgroup in ('string', 'char') and literal(t[0]) in ('↑', '↓', '×', '⌄', '›', '✓', '✔'):
                add('text-icon', t, 'Use a semantic Icon, not a character glyph')

    constructors = {'child': 1, 'create_ui_window': 1, 'CreateWindowExW': 1,
                    'CreateWindowExA': 1, 'CreateWindowEx': 1, 'CreateWindowW': 0,
                    'CreateWindowA': 0, 'CreateWindow': 0}
    aliases = {}
    for i, token in enumerate(tokens):
        if i >= 2 and tokens[i-1][0] == '=' and tokens[i-2].lastgroup == 'id' and token.lastgroup in ('string','raw','id'):
            aliases[tokens[i-2][0]] = token
    for i, token in enumerate(tokens):
        value = token[0]
        if path.endswith('.rc') and token.lastgroup == 'id':
            if value.upper() in ('LISTBOX','COMBOBOX'):
                add('native-selection',token,'Resource dialogs must use the shared list/choice adapter')
            if value.upper() == 'CONTROL' and i+5 < len(tokens):
                part=tokens[i+5]
                name=literal(part[0]).upper() if part.lastgroup in ('string','raw') else part[0].upper()
                if name in ('LISTBOX','COMBOBOX'):
                    add('native-selection',part,'Resource dialogs must use the shared list/choice adapter')
                if name in ('TOOLTIPS_CLASS','TOOLTIPS_CLASSW','TOOLTIPS_CLASS32'):
                    add('native-tooltip',part,'Use fluent::Tooltip')
        # Six/eight-digit RGB/ARGB literals (including leading zeros) are material data.
        if token.lastgroup == 'number' and len(value.rstrip('uUlL')) == 6 and 0xE000 <= int(value.rstrip('uUlL'), 16) <= 0xF8FF:
            add('icon-codepoint', token, 'Register private-use icon values in fluent_icons.hpp')
        if token.lastgroup in ('string', 'char') and any(0xE000 <= ord(c) <= 0xF8FF for c in literal(value)):
            add('icon-codepoint', token, 'Use the shared icon registry')
        if i + 1 >= len(tokens) or tokens[i + 1][0] != '(' or token.lastgroup != 'id':
            # Covers CardInk glyph assignment / comparison; user strings are otherwise data.
            if value in ('glyph', 'icon'):
                glyphs(tokens[i + 1:i + 4])
            continue
        if value == 'DrawFocusRect':
            add('gdi-focus', token, 'Use the shared rounded focus ring')
        if value in ('MessageBox', 'MessageBoxW', 'MessageBoxA'):
            add('system-dialog', token, 'Only pre-UI startup failures may use MessageBox')
        args = arguments(i)
        if value in constructors and len(args) > constructors[value]:
            class_arg = args[constructors[value]]
            for part in class_arg:
                resolved = aliases.get(part[0],part) if part.lastgroup == 'id' else part
                name = literal(resolved[0]).upper() if resolved.lastgroup in ('string','raw') else resolved[0].upper()
                if name in ('LISTBOX', 'COMBOBOX', 'WC_LISTBOX', 'WC_LISTBOXW', 'WC_LISTBOXA',
                            'WC_COMBOBOX', 'WC_COMBOBOXW', 'WC_COMBOBOXA'):
                    add('native-selection', part, 'Construct lists and choices through fluent_list')
                if name in ('TOOLTIPS_CLASS', 'TOOLTIPS_CLASSW', 'TOOLTIPS_CLASSA', 'TOOLTIPS_CLASS32'):
                    add('native-tooltip', part, 'Use fluent::Tooltip')
                if name == 'BUTTON' and len(args) > constructors[value] + 1:
                    glyphs(args[constructors[value] + 1])
        if re.search(r'(?:icon|glyph|button|action|draw_text)$', value, re.I):
            for arg in args:
                glyphs(arg)
    return sorted(hits.values(), key=lambda f: (f.offset, f.rule))


def apply_exceptions(findings: list[Finding], sources: dict[str, str], exceptions: list[dict]) -> list[str]:
    errors, covered = [], set()
    for entry in exceptions:
        try:
            rule, path, anchor = entry['rule'], entry['path'], entry['anchor']
            if not entry['reason'].strip() or not entry['review_when'].strip():
                raise ValueError('reason and review_when must be nonempty')
            if path not in sources or '*' in path or not anchor.strip() or sources[path].count(anchor) != 1:
                raise ValueError('path and unique source anchor must identify one location')
            start = sources[path].index(anchor)
            matches = [f for f in findings if f.rule == rule and f.path == path and start <= f.offset < start + len(anchor)]
            if len(matches) != 1 or matches[0] in covered:
                raise ValueError('exception must match exactly one uncovered violation')
            covered.add(matches[0])
        except (KeyError, ValueError, TypeError) as error:
            errors.append(f'Invalid/stale UI exception {entry}: {error}')
    errors.extend(f'{f.path}:{f.line}: {f.rule}: {f.detail}' for f in findings if f not in covered)
    return errors


def source_hash(path: Path) -> str:
    data=path.read_bytes()
    if path.suffix.lower() not in ('.ico','.png','.jpg','.jpeg','.bmp','.ttf','.otf'):
        data=data.decode('utf-8-sig').replace('\r\n','\n').encode('utf-8')
    return hashlib.sha256(data).hexdigest()


def ui_path(path: str) -> bool:
    return (path.startswith(('src/platform_win/', 'src/rendering/', 'src/presentation/', 'resources/i18n/'))
            or path == 'src/host/desktop.cpp'
            or path in ('packaging/windows/ClipRope.nsi', 'packaging/windows/uninstall.nsh')
            or (path.startswith('resources/') and Path(path).suffix in ('.rc','.manifest','.ico','.svg','.png','.jpg','.jpeg','.bmp','.ttf','.otf')))


def safe_file(root: Path, name: str) -> Path:
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f'Expected a repository file: {name}')
    return path


def check_evidence(root: Path, changed: set[str]) -> list[str]:
    required = {p for p in changed if ui_path(p)}
    if not required:
        return []
    records = sorted(p for p in changed if p.startswith('tests/compatibility/fluent/evidence/') and p.endswith('.json'))
    covered, errors = set(), []
    for name in records:
        try:
            data = json.loads(safe_file(root, name).read_text(encoding='utf-8'))
            if data['status'] not in ('accepted', 'accepted_with_open_checks'):
                raise ValueError('An explicit visual acceptance decision is required')
            review = safe_file(root, data['review'])
            if review.suffix != '.md' or not data['reviewer'].strip() or not data['conclusion'].strip():
                raise ValueError('A named reviewer, conclusion and Markdown review are required')
            if not data['checks'] or not isinstance(data['open_checks'], list):
                raise ValueError('Record performed checks and remaining acceptance work')
            if data['status'] == 'accepted' and data['open_checks']:
                raise ValueError('Open acceptance work requires accepted_with_open_checks')
            if set(data['checklist']) != set(UI_CHECKS):
                raise ValueError('Complete every mandatory UI checklist category')
            for category, item in data['checklist'].items():
                if not isinstance(item,dict) or item.get('status') not in ('passed','not_applicable','open') or not isinstance(item.get('evidence'),str) or not item['evidence'].strip():
                    raise ValueError(f'{category}: supply a status and evidence or a reasoned disposition')
                if item['status'] == 'open' and (data['status'] != 'accepted_with_open_checks' or not data['open_checks']):
                    raise ValueError(f'{category}: retain the open check in the acceptance status and open_checks')
            for phase in ('before', 'after'):
                if not data['screenshots'][phase]:
                    raise ValueError(f'Missing {phase} captures (for new components use a documented baseline reference)')
                for picture in data['screenshots'][phase]:
                    capture = safe_file(root, picture).read_bytes()
                    if len(capture) < 32 or not capture.startswith((b'\x89PNG\r\n\x1a\n',b'BM',b'\xff\xd8\xff')):
                        raise ValueError(f'Expected a nonempty PNG, BMP or JPEG capture: {picture}')
            for path, digest in data['sources'].items():
                if path in required:
                    if digest == 'deleted' and not (root / path).exists():
                        covered.add(path)
                        continue
                    if source_hash(safe_file(root, path)) != digest:
                        raise ValueError(f'Capture evidence is stale for {path}')
                    covered.add(path)
        except (KeyError, ValueError, OSError, TypeError) as error:
            errors.append(f'{name}: {error}')
    errors.extend(f'{path}: UI change needs a changed, source-bound visual acceptance record' for path in sorted(required - covered))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--base', help='Git comparison base for mandatory changed visual evidence')
    args = parser.parse_args()
    root = args.root.resolve()
    sources = {p.relative_to(root).as_posix(): p.read_text(encoding='utf-8-sig')
               for folder in ('src','resources') for p in (root / folder).rglob('*')
               if p.suffix in ('.cpp', '.hpp', '.h', '.cc', '.rc')}
    findings = [f for path, source in sources.items() for f in scan(path, source)]
    errors = apply_exceptions(findings, sources, json.loads((root / 'tools/ui_exceptions.json').read_text(encoding='utf-8')))
    if args.base:
        changed = set(subprocess.check_output(['git', 'diff', '--name-only', '--diff-filter=ACMRD', args.base],
                                              cwd=root, text=True, encoding='utf-8').splitlines())
        errors += check_evidence(root, changed)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'UI contracts passed ({len(sources)} production files; precise exceptions verified)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
