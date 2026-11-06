import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from verify_m5gfx import EXPECTED, verify


class M5GFXVersionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="stopwatch-m5gfx-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.main = self.root / "main/idf_component.yml"
        self.managed = self.root / "managed_components/m5stack__m5gfx/idf_component.yml"
        self.main.parent.mkdir(parents=True)
        self.managed.parent.mkdir(parents=True)
        self.write_main('"0.2.36"')
        self.managed.write_text("description: fixture\nversion: 0.2.36\n", encoding="utf-8")

    def write_main(self, value):
        self.main.write_text("dependencies:\n  other/component: '^1.0'\n  m5stack/m5gfx: " + value + "\n", encoding="utf-8")

    def test_verified_files_are_unchanged(self):
        before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.root.rglob("*") if p.is_file()}
        result = verify(self.root)
        self.assertEqual(result["managed_m5gfx_version"], EXPECTED)
        self.assertTrue(result["read_only"])
        self.assertFalse(result["other_dependencies_verified"])
        self.assertEqual(before, {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.root.rglob("*") if p.is_file()})

    def test_managed_drift_is_rejected(self):
        self.managed.write_text("version: 0.3.0\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_relaxed_or_wrong_project_constraint_is_rejected(self):
        for value in ('">=0.2.22"', '"^0.2.36"', '"0.3.0"', '"==0.2.36"'):
            with self.subTest(value=value):
                self.write_main(value)
                with self.assertRaises(ValueError):
                    verify(self.root)

    def test_supported_scalar_forms(self):
        for value in ("0.2.36", "'0.2.36'", '"0.2.36" # exact'):
            with self.subTest(value=value):
                self.write_main(value)
                self.managed.write_text("version: " + value + "\n", encoding="utf-8")
                self.assertEqual(verify(self.root)["status"], "verified")

    def test_missing_managed_metadata_is_rejected(self):
        self.managed.unlink()
        with self.assertRaises(OSError):
            verify(self.root)

    def test_missing_project_pin_is_rejected(self):
        self.main.write_text("dependencies:\n  other/component: '1.0'\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_duplicate_project_pin_is_rejected(self):
        with self.main.open("a", encoding="utf-8") as stream:
            stream.write("  m5stack/m5gfx: '0.3.0'\n")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_duplicate_managed_version_is_rejected(self):
        self.managed.write_text("version: 0.2.36\nversion: 0.3.0\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_nested_version_cannot_substitute(self):
        self.managed.write_text("nested:\n  version: 0.2.36\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_nested_dependency_cannot_substitute(self):
        self.main.write_text("dependencies:\n  other/component:\n    m5stack/m5gfx: 0.2.36\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_mapping_dependency_format_is_rejected(self):
        self.write_main("\n    version: 0.2.36")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_duplicate_dependency_mapping_is_rejected(self):
        with self.main.open("a", encoding="utf-8") as stream:
            stream.write("dependencies:\n  other/component: '2.0'\n")
        with self.assertRaises(ValueError):
            verify(self.root)

    def test_cli_success_and_failure_exit_codes(self):
        script = Path(__file__).with_name("verify_m5gfx.py")
        success = subprocess.run([sys.executable, str(script), "--project", str(self.root)], capture_output=True, text=True)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertIn('"managed_m5gfx_version": "0.2.36"', success.stdout)
        self.managed.write_text("version: 0.3.0\n", encoding="utf-8")
        failed = subprocess.run([sys.executable, str(script), "--project", str(self.root)], capture_output=True, text=True)
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(failed.stdout, "")
        self.assertIn("M5GFX version guard failed:", failed.stderr)


if __name__ == "__main__":
    unittest.main()
