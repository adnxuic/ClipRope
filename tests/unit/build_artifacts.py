"""Exercise artifact rejection and backup preservation with disposable directories."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('artifacts', Path(__file__).resolve().parents[2] / 'tools/build_artifacts.py')
artifacts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifacts)


class BuildArtifacts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ClipRope-build-guards-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = self.root / 'build/app'
        self.app.mkdir(parents=True)
        self.main = self.app / 'ClipRope.exe'
        self.main.write_bytes(artifacts.SIGNATURE)

    def test_unique_runtime(self):
        self.assertEqual(artifacts.inventory(self.root)['main_count'], 1)

    def test_second_directory_is_rejected(self):
        folder = self.root / 'build/old'
        folder.mkdir()
        (folder / 'ClipRope.exe').write_bytes(b'old')
        with self.assertRaises(ValueError):
            artifacts.inventory(self.root)

    def test_renamed_main_is_rejected(self):
        (self.app / 'old.exe').write_bytes(artifacts.SIGNATURE)
        with self.assertRaises(ValueError):
            artifacts.inventory(self.root)

    def test_legacy_renamed_main_is_rejected(self):
        (self.app / 'ClipRope.baseline.exe').write_bytes(b'old')
        with self.assertRaises(ValueError):
            artifacts.inventory(self.root)

    def test_hard_link_outside_build_is_rejected(self):
        os.link(self.main, self.root / 'linked.exe')
        with self.assertRaises(ValueError):
            artifacts.inventory(self.root)

    def test_missing_runtime_is_not_a_delivery(self):
        self.main.unlink()
        artifacts.inventory(self.root, allow_empty=True)
        with self.assertRaises(ValueError):
            artifacts.inventory(self.root)

    def test_backup_verifies_history_and_detects_later_changes(self):
        history = self.app / 'history.db'
        history.write_bytes(b'Synthetic history remains recoverable')
        destination = self.root / '.local-artifacts/backup.zip'
        # Windows runners can spell TEMP differently from its resolved path.
        root_alias = self.app / '..' / '..'
        result = artifacts.archive(root_alias, destination)
        self.assertTrue(result['verified'])
        artifacts.check_archive(root_alias, destination)
        history.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            artifacts.check_archive(self.root, destination)

    def test_unverified_archive_is_rejected(self):
        destination = self.root / '.local-artifacts/backup.zip'
        artifacts.archive(self.root, destination)
        destination.write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            artifacts.check_archive(self.root, destination)

    def test_archive_must_be_outside_build(self):
        with self.assertRaises(ValueError):
            artifacts.archive(self.root, self.app / 'backup.zip')

    def test_failed_or_stale_tests_cannot_publish(self):
        cache = self.root / 'build/cmake/CMakeCache.txt'
        cache.parent.mkdir()
        cache.write_text('CLIPROPE_PIN_RELEASE:BOOL=ON\n' + ''.join(f'{name}:BOOL=OFF\n' for name in
            ('BUILD_TESTING','CLIPROPE_DESKTOP_PROBES','CLIPROPE_TEST_DESKTOP_INPUT','CLIPROPE_TEST_TRAY_ICON','CLIPROPE_TEST_OFFICE','CLIPROPE_TEST_OFFICE_MATRIX')))
        record = dict(variant='Production', configuration='Release', source_fingerprint='current', payload={}, cache_sha256='cache')
        (self.root / 'build/build-receipt.json').write_text(json.dumps(record))
        test_file = self.root / 'build/test-receipt.json'
        with patch.object(artifacts, 'receipt', return_value=record):
            for status, source in [('failed','current'),('passed','old')]:
                test_file.write_text(json.dumps(dict(status=status,source_fingerprint=source)))
                with self.assertRaises(ValueError):
                    artifacts.verify(self.root, publish=True)
            required = ['PASTE.uia_press', 'AR3.target_supervision', 'AR3.target_replies',
                        'P0-05.unselected_private', 'ARCH-11.pure_invocations', 'PASTE.desktop_commands']
            cases = ['edge.first','edge.reopen','custom.first','custom.reopen','edge.change','edge.readonly','edge.password','edge.interrupt']
            observed = dict(source_fingerprint='current', payload={}, cases=cases)
            valid = dict(status='passed', source_fingerprint='current', interactive=True,
                         passed_tests=required, release_paste=observed)
            for changes in [{'release_paste': {}}, {'interactive': False}, {'passed_tests': required[1:]},
                            {'release_paste': dict(observed, cases=cases[:-1])},
                            {'release_paste': dict(observed, source_fingerprint='old')},
                            {'release_paste': dict(observed, payload={'ClipRope.exe':'previous'})}]:
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    test_file.write_text(json.dumps(dict(valid, **changes)))
                    artifacts.verify(self.root, publish=True)
            test_file.write_text(json.dumps(valid))
            artifacts.verify(self.root, publish=True)
            record['variant'] = 'Qualification'
            (self.root / 'build/build-receipt.json').write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                artifacts.verify(self.root, publish=True)

    def test_changed_runtime_cannot_reuse_receipt(self):
        saved = dict(variant='Production',configuration='Release',source_fingerprint='source',payload={'ClipRope.exe':'old'},cache_sha256='cache')
        (self.root / 'build/build-receipt.json').write_text(json.dumps(saved))
        current = {**saved, 'payload': {'ClipRope.exe':'new'}}
        with patch.object(artifacts, 'receipt', return_value=current), self.assertRaises(ValueError):
            artifacts.verify(self.root)


if __name__ == '__main__':
    unittest.main()
