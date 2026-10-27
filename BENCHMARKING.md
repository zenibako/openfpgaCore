# Pocket platform selection and benchmark evidence

Use [README.md](README.md) and [CLAUDE.md](CLAUDE.md) for build, timing, firmware
and hardware checks. Use the separate
[pocket-harness](https://github.com/zenibako/pocket-harness) checkout for shared
profile identities and comparable consumer/device measurements. The repository
is private; contributors need access and an established local checkout.
The [local skill](.agents/skills/pocket-benchmarking/SKILL.md) locates and validates
its canonical workflow. Set `POCKET_HARNESS_ROOT` to that checkout as needed.

## Select candidates and record actual builds

Each project chooses a compatible shortlist based on ISA, CPU/cache needs,
enabled GPU/audio features, memory and SDK/API support. Record why other profiles
are incompatible, deferred or unselected. The four os20/os25 × 90/100 MHz cells
are an evidence catalog, not a requirement to support every combination. Other
variants/targets remain outside the current collector profile schema.

Create a profile using the harness's `profile create` command and verify it with
`profile verify`. Supply the actual matched packaged `os.bin`, variant `.rbf_r`,
Chip32 `loader.bin` and exact CPU config. Resolve CPU config/generator, variant
macros, source/SDK revisions and clock from that build's provenance. Keep the
saved QSF, netlist/MIF identity, seed and fit/STA results as supporting evidence.
Do not assume a current source checkout or a variant name identifies older bytes.
The 199-byte Chip32 selector is separate from the RV32 bootloader embedded in
the FPGA image. Profile hashing does not verify the source-to-binary build,
firmware embedding, timing closure or what was flashed.

Preserve all existing fit/hold/DQ, hardware-contract, boot memory, RTL, firmware
pairing and physical acceptance gates. Do not change pins, seeds or clocks merely
to fill matrix cells. Benchmark cores should coexist with gameplay without
overwriting a shared runtime or saves.

## Collect and compare the shortlisted configurations

Follow the consumer project's actual benchmark route and independent correctness
checks. Retain exact app source/ELF/compiler/flags, device ID, assets, seed,
warmup, sampling, audio/pacing and effective settings. Capture or write reports
after timed segments. Import supported logs or screenshot-backed `device-summary`
evidence using the matching `--profile`; use `verify` and `matrix` from the harness
root. Matrix imports measure evidence coverage, not independent device trials.

| Comparison | Use | Required controls |
| --- | --- | --- |
| Default `software` | Application change on the same platform | Same profile/toolchain, workload and conditions |
| `--mode platform --vary ...` | Intentional platform changes | Same known ELF/toolchain, device/assets/workload/sampling; exact declared factors |
| `--mode configuration --reason "why builds differ"` | SDK/platform requires another ELF/toolchain | Known identities on both sides; same device/assets/workload/conditions/sampling |

Configuration output retains the reason, component and field/presence differences,
both full build/profile contexts, source checks/limits and literal-unit deltas.
It describes the combined working configuration, without isolating a CPU, clock
or OS effect or automatically ranking a winner. Use paired repeats and project
targets to choose among eligible tested configurations. An untested/deferred
profile does not create a rollout obligation.

The collector accepts new project IDs for real device summaries, but does not
parse native OS RTL, resource-fit or STA reports as game benchmarks. Keep those
results in native tooling. Host, simulated and physical observations remain
separate. Read the harness's actual workload guides for bounded replay support;
an adapter/profile registration is not a generic RTL or device qualification.
