"""Run the extracted pure JS test suite via `node --test` from the CPython side."""

import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
class TestJsModules(unittest.TestCase):
    def test_all(self):
        tests = sorted((ROOT / "serve/js-tests").glob("*.test.mjs"))
        self.assertTrue(tests, "No .test.mjs files found under serve/js-tests/")
        result = subprocess.run(
            ["node", "--test", *[str(p.relative_to(ROOT)) for p in tests]],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        self.assertIn("# fail 0", result.stdout)

    def test_module_syntax(self):
        # `node --check file.js` does not catch ESM syntax errors here; check stdin as a module instead.
        files = sorted((ROOT / "serve/web").rglob("*.js"))
        self.assertTrue(files, "No .js files found under serve/web/")
        for path in files:
            with self.subTest(path=path):
                result = subprocess.run(
                    ["node", "--input-type=module", "--check"],
                    input=path.read_bytes(),
                    cwd=ROOT,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 0, f"{path}:\n{result.stderr.decode(errors='replace')}")
