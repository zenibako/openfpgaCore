#!/usr/bin/env python3
"""Exercise the Pocket firmware recipe with controlled external Quartus exits."""
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "src/fpga/targets/pocket/Makefile"

class FirmwarePatchTest(unittest.TestCase):
    def run_recipe(self, sof=True, cdb=0, asm=0):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            if sof:
                (root / "ap_core.sof").touch()
            for name, result in (("quartus_cdb", cdb), ("quartus_asm", asm)):
                tool = root / name
                tool.write_text(f"#!/bin/sh\nexit {result}\n")
                tool.chmod(0o755)
            text = SOURCE.read_text()
            recipe = text[text.index("firmware: bootloader os\n"):]
            recipe = recipe[:recipe.index("\n# ── FPGA compile")]
            makefile = root / "Makefile"
            makefile.write_text(f"QOUT := {root}\nPROJECT := ap_core\nQRUN := env PATH={root}:$(PATH)\n"
                "VARIANT := os25\n.PHONY: bootloader os install-bitstream\n"
                "bootloader os: ; @true\ninstall-bitstream: ; @touch published\n" + recipe)
            proc = subprocess.run(["make", "--no-print-directory", "firmware"], cwd=root,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            return proc.returncode, (root / "published").exists(), proc.stdout

    def test_missing_sof_fails(self):
        rc, published, output = self.run_recipe(sof=False)
        self.assertNotEqual(rc, 0, output)
        self.assertFalse(published)

    def test_failed_cdb_does_not_publish(self):
        rc, published, output = self.run_recipe(cdb=42)
        self.assertNotEqual(rc, 0, output)
        self.assertFalse(published, output)

    def test_failed_asm_does_not_publish(self):
        rc, published, output = self.run_recipe(asm=43)
        self.assertNotEqual(rc, 0, output)
        self.assertFalse(published, output)

    def test_success_publishes(self):
        rc, published, output = self.run_recipe()
        self.assertEqual(rc, 0, output)
        self.assertTrue(published, output)

if __name__ == "__main__":
    unittest.main()
