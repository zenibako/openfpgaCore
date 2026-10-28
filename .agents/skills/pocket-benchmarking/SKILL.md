---
name: pocket-benchmarking
description: Use when selecting Pocket platform profiles or collecting and comparing device benchmark evidence for openfpgaOS consumers; native RTL, fit and timing checks remain in the OS tools.
---

# Shared Pocket benchmarking

Read [the project integration guide](../../../BENCHMARKING.md) and preserve the
native build/hardware gates in [CLAUDE.md](../../../CLAUDE.md).

Locate the established `pocket-harness` checkout from the session or
`POCKET_HARNESS_ROOT`. A sibling directory is only a candidate. Verify
`.agents/skills/pocket-benchmarking/SKILL.md` and `benchmarks/__main__.py`, resolve
the canonical skill path and open it before following its relative references.
Run `python3 -m benchmarks compare --help` from that root to check that the
checkout supports `configuration` and `--reason`; read its `PLATFORMS.md` and
`SELECTION.md` for the current contracts. If unavailable, report the missing
dependency and continue applicable native checks. Do not copy the shared skill
body or make a cross-repository skill symlink.

Evaluate each consumer's compatible profile shortlist. Register actual packaged
OS/RBF/Chip32-loader/config identities, preserve independent clock/variant axes
and keep evidence status separate from project applicability. Software compares
on a fixed platform; controlled platform mode keeps the ELF/toolchain fixed;
configuration mode permits known ELF/toolchain changes with a required reason
and whole-configuration attribution. Retain device/assets/workload/conditions/
sampling controls and each consumer's correctness/lifecycle gates.

Generic device summaries need original screenshots. Native OS RTL/fit/STA reports
do not become device or game measurements by importing them. Replay support is
workload-specific; read `os25-harness` for the available Pico8 os20 and
Butterscotch os25 adapters. Collection grants no pin change, deployment or release.
