"""Inventory, archive and qualify the one development runtime. Never deletes files."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ('ClipRope.exe', 'ClipRopeWorker.exe', 'ClipRopePreviewWorker.exe','ClipRopeTargetWorker.exe',
           'cliprope_headless.exe', 'cliprope_profile_support.exe', 'Uninstall.exe', 'ClipRope.build.ini')
SIGNATURE = b'ClipRope.Desktop.Runtime.v1'


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def ordinary_tree(root: Path) -> list[Path]:
    if not root.exists():
        return []
    for parent in (root, *root.parents):
        if parent.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f'Reparse path is not allowed: {parent}')
    files = []
    for directory, folders, names in os.walk(root, followlinks=False):
        for name in folders + names:
            path = Path(directory) / name
            if path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError(f'Reparse path is not allowed: {path}')
        files.extend(Path(directory) / name for name in names)
    return sorted(files)


def source_fingerprint(root: Path) -> str:
    # Include new source files before commit, but never local histories or artifacts.
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root)
    result = hashlib.sha256()
    for name in sorted(set(names.decode('utf-8').split('\0'))):
        if not name or name == '.gitignore' or name.startswith(('Tasks/', 'docs/', '.agent/')) or name == 'AGENTS.md':
            continue
        path = root / name
        result.update(name.encode('utf-8') + b'\0')
        result.update(bytes.fromhex(digest(path)) if path.is_file() else b'deleted')
    return result.hexdigest()


def inventory(root: Path, allow_empty: bool = False) -> dict:
    build = root / 'build'
    files = ordinary_tree(build)
    canonical = build / 'app/ClipRope.exe'
    mains = []
    for path in files:
        if path.suffix.lower() != '.exe':
            continue
        name = path.name.lower()
        # Compiler probes and pinned packaging tools are not product copies.
        if 'CMakeFiles' in path.parts or path.is_relative_to(build / 'packaging-tools'):
            continue
        is_main = name == 'cliprope.exe' or name.startswith('cliprope.')
        if not is_main:
            is_main = SIGNATURE in path.read_bytes()
        if is_main:
            mains.append(path)
    if mains != ([canonical] if canonical.exists() else []) or (not mains and not allow_empty):
        raise ValueError('Expected only build/app/ClipRope.exe; found: ' + ', '.join(str(p.relative_to(root)) for p in mains))
    if canonical.exists() and canonical.stat().st_nlink != 1:
        raise ValueError('The development executable must not have hard-linked copies.')
    return {'main_count': len(mains), 'main': str(canonical), 'files': len(files)}


def archive(root: Path, destination: Path) -> dict:
    build = root / 'build'
    if not destination.resolve().is_relative_to(root.resolve() / '.local-artifacts') or destination.exists():
        raise ValueError('Use a new archive under .local-artifacts.')
    files = ordinary_tree(build)
    destination.parent.mkdir(parents=True, exist_ok=True)
    manifest = {}
    with zipfile.ZipFile(destination, 'x', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as output:
        for index, path in enumerate(files):
            before = path.stat()
            name = path.relative_to(build).as_posix()
            with path.open('rb') as source, output.open(name, 'w', force_zip64=True) as target:
                checksum = hashlib.sha256()
                while data := source.read(1024 * 1024):
                    target.write(data)
                    checksum.update(data)
            after = path.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError(f'File changed during backup: {name}')
            manifest[name] = {'sha256': checksum.hexdigest(), 'size': after.st_size,
                              'mtime_ns': after.st_mtime_ns, 'ctime_ns': after.st_ctime_ns}
            if index and index % 10000 == 0:
                print(f'Archived {index}/{len(files)} files', flush=True)
        output.writestr('__backup_manifest__.json', json.dumps(manifest, indent=2))
    with zipfile.ZipFile(destination) as saved:
        for name, item in manifest.items():
            with saved.open(name) as data:
                if hashlib.file_digest(data, 'sha256').hexdigest() != item['sha256']:
                    raise ValueError(f'Archive verification failed: {name}')
    result = {'archive': str(destination), 'sha256': digest(destination), 'files': len(manifest),
              'source_bytes': sum(item['size'] for item in manifest.values()), 'verified': True}
    destination.with_suffix('.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def receipt(root: Path, variant: str, configuration: str) -> dict:
    inventory(root)
    app = root / 'build/app'
    return {'schema': 1, 'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            'source_fingerprint': source_fingerprint(root), 'variant': variant, 'configuration': configuration,
            'payload': {name: digest(app / name) for name in PAYLOAD},
            'cache_sha256': digest(root / 'build/cmake/CMakeCache.txt')}


def verify(root: Path, publish: bool = False) -> dict:
    saved = json.loads((root / 'build/build-receipt.json').read_text(encoding='utf-8-sig'))
    current = receipt(root, saved['variant'], saved['configuration'])
    for key in ('source_fingerprint', 'payload', 'cache_sha256'):
        if current[key] != saved[key]:
            raise ValueError(f'Build receipt does not match {key}; rebuild and retest.')
    if publish:
        if (saved['variant'], saved['configuration']) != ('Production', 'Release'):
            raise ValueError('Only Production Release can be published.')
        tests = json.loads((root / 'build/test-receipt.json').read_text(encoding='utf-8-sig'))
        if tests.get('status') != 'passed' or tests.get('source_fingerprint') != current['source_fingerprint']:
            raise ValueError('A successful test receipt for these sources is required.')
        cache = (root / 'build/cmake/CMakeCache.txt').read_text(encoding='utf-8-sig')
        for option in ('BUILD_TESTING', 'CLIPROPE_DESKTOP_PROBES', 'CLIPROPE_TEST_DESKTOP_INPUT',
                       'CLIPROPE_TEST_TRAY_ICON', 'CLIPROPE_TEST_OFFICE', 'CLIPROPE_TEST_OFFICE_MATRIX'):
            if f'{option}:BOOL=OFF' not in cache:
                raise ValueError(f'Production must explicitly disable {option}.')
        if 'CLIPROPE_PIN_RELEASE:BOOL=ON' not in cache:
            raise ValueError('CI or unpinned builds cannot be published.')
    return current


def check_archive(root: Path, destination: Path) -> dict:
    if not destination.resolve().is_relative_to(root.resolve() / '.local-artifacts'):
        raise ValueError('Backup must be under .local-artifacts.')
    receipt_path = destination.with_suffix('.json')
    saved = json.loads(receipt_path.read_text(encoding='utf-8'))
    if not saved.get('verified') or digest(destination) != saved['sha256']:
        raise ValueError('Backup is incomplete or has changed.')
    with zipfile.ZipFile(destination) as archive_file:
        manifest = json.loads(archive_file.read('__backup_manifest__.json'))
    build = root / 'build'
    current = ordinary_tree(build)
    if {p.relative_to(build).as_posix() for p in current} != set(manifest):
        raise ValueError('Build files changed after backup; take a new archive.')
    for path in current:
        item = manifest[path.relative_to(build).as_posix()]
        observed = path.stat()
        # The archive has already been read back and checked against every source hash.
        # Rehash only files whose metadata changed since that snapshot. Legacy receipts
        # predate the metadata fields: anything newer than archive creation is rehashed.
        unchanged = (observed.st_mtime_ns == item['mtime_ns'] and observed.st_ctime_ns == item['ctime_ns']) if 'mtime_ns' in item else (
            max(observed.st_mtime_ns, observed.st_ctime_ns) < destination.stat().st_ctime_ns)
        if observed.st_size != item['size'] or (not unchanged and digest(path) != item['sha256']):
            raise ValueError(f'Build file changed after backup: {path}')
    return saved


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inventory', 'archive', 'check-archive', 'ordinary', 'fingerprint', 'receipt', 'verify'))
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--allow-empty', action='store_true')
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--variant', default='Production')
    parser.add_argument('--configuration', default='Release')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.action == 'inventory':
        result = inventory(root, args.allow_empty)
    elif args.action == 'archive':
        result = archive(root, args.destination.resolve())
    elif args.action == 'check-archive':
        result = check_archive(root, args.destination.resolve())
    elif args.action == 'ordinary':
        result = {'files': len(ordinary_tree(root / 'build'))}
    elif args.action == 'fingerprint':
        result = source_fingerprint(root)
    elif args.action == 'receipt':
        result = receipt(root, args.variant, args.configuration)
    else:
        result = verify(root, args.publish)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
