#!/usr/bin/env python3
"""Offline regressions for timing report parsing and failed seed compiles."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent


def panel(title, slack, tns="0.000"):
    return (f"+--------+\n; {title} ;\n+--------+\n"
            "; Clock ; Slack ; End Point TNS ;\n+--------+\n"
            f"; cpu ; {slack} ; {tns} ;\n+--------+\n")


def report(setup="0.200", hold="0.100"):
    return ("; 150.00 MHz ; 150.00 MHz ; pll_hdmi|counter[0] ;\n"
            "; 101.01 MHz ; 101.01 MHz ; emu|pll|pll_inst|counter[0] ;\n"
            + panel("Setup Summary", setup) + panel("Hold Summary", hold))


class TimingTests(unittest.TestCase):
    def parse(self, function, contents):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "timing.rpt"
            path.write_text(contents)
            return subprocess.check_output(
                ["bash", "-c", '. "$1"; "$2" "$3"', "test",
                 str(TOOLS / "sta_lib.sh"), function, str(path)], text=True).strip()

    def test_single_corner(self):
        self.assertEqual(self.parse("sta_wns_tns", report()), "0.200 0.0")
        self.assertEqual(self.parse("sta_hold_wns", report()), "0.100")

    def test_all_corners_including_negative_temperature(self):
        text = panel("Slow 1100mV 100C Model Setup Summary", "0.200")
        text += panel("Slow 1100mV -40C Model Setup Summary", "-0.250", "-1.500")
        text += panel("Fast 1100mV -40C Model Setup Summary", "0.100")
        text += panel("Slow 1100mV 100C Model Hold Summary", "0.150")
        text += panel("Fast 1100mV -40C Model Hold Summary", "-0.050")
        self.assertEqual(self.parse("sta_wns_tns", text), "-0.250 -1.5")
        self.assertEqual(self.parse("sta_hold_wns", text), "-0.050")

    def test_missing_panels(self):
        for function in ("sta_wns_tns", "sta_hold_wns"):
            self.assertEqual(self.parse(function, "  8. Setup Summary\n"), "")

    def test_fingerprint_includes_build_date_and_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            qsf = job / "mister.qsf"
            date = job / "build_id.v"
            variant = job / "mister.mk"
            qsf.write_text("set_global_assignment -name SEED 1\n")
            date.write_text('`define BUILD_DATE "260909"')
            variant.write_text("DEFS := INCLUDE_HW_MIXER\n")

            def fingerprint():
                return subprocess.check_output(
                    ["bash", "-c", '. "$1"; shift; netlist_hash "$@"', "test",
                     str(TOOLS / "netlist_hash.sh"), str(qsf), str(date), str(variant)])

            original = fingerprint()
            qsf.write_text("set_global_assignment -name SEED 21\n")
            self.assertEqual(fingerprint(), original)
            date.write_text('`define BUILD_DATE "260910"')
            dated = fingerprint()
            self.assertNotEqual(dated, original)
            variant.write_text("DEFS := INCLUDE_HW_MIXER INCLUDE_CLK_AUTOTUNE\n")
            self.assertNotEqual(fingerprint(), dated)

    def test_fingerprint_ignores_quartus_relative_duplicates(self):
        # Quartus adds a qsf-relative entry for each absolute one during a
        # compile; the generated (pre-fit) and rewritten (post-fit) qsf must
        # fingerprint the same, and the hash must not depend on the cwd.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "rtl" / "core.v"
            mif = root / "boot.mif"
            source.parent.mkdir()
            source.write_text("module core; endmodule\n")
            mif.write_text("WIDTH=32;\n")
            job = root / "bld" / "job"
            job.mkdir(parents=True)
            qsf = job / "ap_core.qsf"
            entries = (f"set_global_assignment -name VERILOG_MACRO INCLUDE_X\n"
                       f"set_global_assignment -name VERILOG_FILE {source}\n"
                       f"set_global_assignment -name MIF_FILE {mif}\n")

            def fingerprint(cwd):
                return subprocess.check_output(
                    ["bash", "-c", '. "$1"; shift; netlist_hash "$@"', "test",
                     str(TOOLS / "netlist_hash.sh"), str(qsf)], cwd=cwd)

            qsf.write_text(entries)
            generated = fingerprint(root)
            qsf.write_text(entries
                           + "set_global_assignment -name VERILOG_FILE ../../rtl/core.v\n"
                           + "set_global_assignment -name MIF_FILE ../../boot.mif\n")
            self.assertEqual(fingerprint(root), generated)
            self.assertEqual(fingerprint(job), generated)
            source.write_text("module core(input a); endmodule\n")
            self.assertNotEqual(fingerprint(root), generated)

    def sweep(self, scenario):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / "output_files").mkdir()
            (job / "mister.qsf").write_text("set_global_assignment -name SEED 7\n")
            (job / "stored.seed").write_text("7\n")
            (job / "stored.seed.src").write_text("original fingerprint\n")
            (job / "good.rpt").write_text(report())
            (job / "cold.rpt").write_text(report() + panel(
                "Slow 1100mV -40C Model Setup Summary", "-0.300", "-1.0"))
            (job / "nohold.rpt").write_text(panel("Setup Summary", "0.200"))
            # Existing passing reports/bitstreams must never rescue a failed compile.
            (job / "output_files/mister.sta.rpt").write_text(report())
            (job / "output_files/mister.sof").write_text("old bitstream")
            runner = job / "qrun"
            runner.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
p = pathlib.Path("calls")
n = int(p.read_text()) + 1 if p.exists() else 1
p.write_text(str(n))
assert "--verilog_macro=INCLUDE_HW_MIXER" in sys.argv[-1]
scenario = os.environ["SCENARIO"]
if scenario == "fail" or (scenario == "final_fail" and n == 2):
    sys.exit(1)
rpt = "good.rpt"
if scenario == "cold": rpt = "cold.rpt"
if scenario == "nohold": rpt = "nohold.rpt"
if scenario == "final_regression" and n == 2: rpt = "cold.rpt"
pathlib.Path("output_files/mister.sta.rpt").write_text(pathlib.Path(rpt).read_text())
pathlib.Path("output_files/mister.sof").write_text("fresh bitstream")
if scenario == "partial": sys.exit(1)
''')
            runner.chmod(0o755)
            env = dict(os.environ, USE_CONTAINER="0", PROJECT="mister",
                       QRUN=str(runner), SEED_FILE="stored.seed", STOP_ON_PASS="1",
                       CLOCK_RE=r"^emu[|]pll[|]pll_inst.*(general\[0\]|counter\[0\])",
                       VARIANT_DEFS="INCLUDE_HW_MIXER", SCENARIO=scenario)
            result = subprocess.run(["bash", str(TOOLS / "sweep.sh"), "1", "2"],
                                    cwd=job, env=env, text=True, capture_output=True)
            return (result.returncode, (job / "stored.seed").read_text().strip(),
                    (job / "stored.seed.src").read_text().strip(),
                    int((job / "calls").read_text()), result.stdout + result.stderr)

    def test_failed_compiles_preserve_seed(self):
        for scenario in ("fail", "partial", "final_fail", "final_regression", "nohold"):
            with self.subTest(scenario=scenario):
                code, seed, fingerprint, _, output = self.sweep(scenario)
                self.assertNotEqual(code, 0, output)
                self.assertEqual(seed, "7", output)
                self.assertEqual(fingerprint, "original fingerprint", output)

    def test_success_requires_fresh_final_compile(self):
        code, seed, _, calls, output = self.sweep("pass")
        self.assertEqual(code, 0, output)
        self.assertEqual(seed, "1", output)
        self.assertEqual(calls, 2, output)
        self.assertIn("101.01 MHz", output)

    def test_cold_failure_does_not_stop_early(self):
        code, _, _, calls, output = self.sweep("cold")
        self.assertEqual(code, 0, output)
        self.assertEqual(calls, 3, output)
        self.assertNotIn("stopping early", output)

    def test_container_backend_rejects_failed_reports(self):
        for scenario in ("pass", "partial", "prerequisite"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as directory:
                job = Path(directory)
                scripts = job / "tools"
                scripts.mkdir()
                for name in ("sweep.sh", "sta_lib.sh"):
                    shutil.copy2(TOOLS / name, scripts / name)
                (job / "stored.seed").write_text("7\n")
                (job / "good.rpt").write_text(report())
                (job / "Makefile").write_text(
                    ".PHONY: cpu bootloader\ncpu bootloader:\n"
                    '\t@test "$(SCENARIO)" != prerequisite\n'
                    "bld/%/ap_core.qsf:\n\t@touch $@\n")
                for seed in (1, 2):
                    output = job / f"bld/test-s{seed}/output_files"
                    output.mkdir(parents=True)
                    (output / "ap_core.sta.rpt").write_text(report())
                    (output / "ap_core.sof").write_text("old bitstream")
                runner = scripts / "quartus-container.sh"
                runner.write_text('''#!/bin/bash
set -e
cp good.rpt "$1/output_files/ap_core.sta.rpt"
printf fresh > "$1/output_files/ap_core.sof"
test "$SCENARIO" != partial
''')
                env = dict(os.environ, USE_CONTAINER="1", PROJECT="ap_core",
                           TARGET_DIR=str(job), VARIANT="test", MAXJOBS="2",
                           SEED_FILE="stored.seed", SCENARIO=scenario)
                result = subprocess.run(["bash", str(scripts / "sweep.sh"), "1", "2"],
                                        cwd=job, env=env, text=True, capture_output=True)
                if scenario == "pass":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((job / "stored.seed").read_text(), "1\n")
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((job / "stored.seed").read_text(), "7\n")


if __name__ == "__main__":
    unittest.main()
