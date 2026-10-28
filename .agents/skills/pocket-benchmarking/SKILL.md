---
name: pocket-benchmarking
description: Use when selecting Pocket profiles, preserving platform regression baselines or comparing consumer benchmark evidence; native RTL, fit and timing checks remain in the OS tools.
---

# Shared Pocket benchmarking

Read [the project integration guide](../../../BENCHMARKING.md) and preserve the
native build/hardware gates in [CLAUDE.md](../../../CLAUDE.md).

The canonical repository is <https://github.com/zenibako/pocket-dev-tools>,
formerly `pocket-harness`. Locate an explicit session root, then
`POCKET_DEV_TOOLS_ROOT`, then legacy `POCKET_HARNESS_ROOT`. Resolve conflicting
explicit roots before choosing. Either sibling directory is only a candidate. Verify
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

## Routine regression checks

For recurring software checks, require the shared `benchmarks/REGRESSIONS.md`
and read it with this project's benchmark guide. The contract was introduced in
`fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58`; record the actual verified shared
checkout commit. Preserve the accepted reference, create independent candidates,
and require project correctness and negative controls before interpreting deltas.
A missing adapter or fixture is an explicit coverage gap. Collector `verify` or
`compare` is not an output-regression gate or a substitute for device checks.
Keep native RTL/acceptance, hardware-contract, fit/STA and firmware-pairing
checks in this repo. Platform changes need separately recorded experiments and
fresh accepted references; game-specific `regression.py` does not test the OS.
