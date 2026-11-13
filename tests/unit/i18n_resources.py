"""Exercise catalog rejection and deterministic code generation with disposable inputs."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("check_i18n", Path(__file__).resolve().parents[2] / "tools/check_i18n.py")
i18n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(i18n)


class Resources(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cliprope-i18n-")
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def catalogs(self, english, chinese):
        for locale, data in (("en-US", english), ("zh-CN", chinese)):
            (self.root / f"{locale}.json").write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")

    def test_generation_and_utf16(self):
        self.catalogs({"test.label": "{{{count}}} 😀"}, {"test.label": "{count} 条 😀"})
        catalogs = i18n.validate(self.root)
        out = self.root / "generated"
        i18n.generate(out, *catalogs)
        original = (out / "translation_catalog.cpp").read_bytes()
        stamp = (out / "translation_catalog.cpp").stat().st_mtime_ns
        i18n.generate(out, *catalogs)
        self.assertEqual(original, (out / "translation_catalog.cpp").read_bytes())
        self.assertEqual(stamp, (out / "translation_catalog.cpp").stat().st_mtime_ns)
        self.assertIn(b"\\xd83d\\xde00", original)

    def test_duplicate(self):
        self.catalogs({"test.label": "Value"}, {"test.label": "值"})
        (self.root / "en-US.json").write_text('{"test.label":"a","test.label":"b"}', encoding="utf-8")
        with self.assertRaises(i18n.CatalogError): i18n.validate(self.root)

    def test_missing_and_extra(self):
        self.catalogs({"test.one": "One", "test.two": "Two"}, {"test.one": "一"})
        with self.assertRaises(i18n.CatalogError): i18n.validate(self.root)
        i18n.validate(self.root, allow_incomplete=True)
        self.catalogs({"test.one": "One"}, {"test.one": "一", "test.extra": "额外"})
        with self.assertRaises(i18n.CatalogError): i18n.validate(self.root, allow_incomplete=True)

    def test_invalid_values_and_parameters(self):
        for english, chinese in (("", "空"), ("Value", " "), ("{count}", "{number}"),
                                 ("{count!r}", "{count}"), ("{", "{"), ("Value\0", "值"),
                                 ("{count:03}", "{count}")):
            with self.subTest(english=english, chinese=chinese):
                self.catalogs({"test.label": english}, {"test.label": chinese})
                with self.assertRaises(i18n.CatalogError): i18n.validate(self.root)

    def test_key_collision(self):
        self.catalogs({"a.b_c": "One", "a_b.c": "Two"}, {"a.b_c": "一", "a_b.c": "二"})
        with self.assertRaises(i18n.CatalogError): i18n.validate(self.root)


if __name__ == "__main__":
    unittest.main()
