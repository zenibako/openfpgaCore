# Pocket platform selection and benchmark evidence

Use [README.md](README.md) and [CLAUDE.md](CLAUDE.md) for build, timing, firmware
and hardware checks. Use the separate
[pocket-dev-tools](https://github.com/zenibako/pocket-dev-tools) checkout
(formerly `pocket-harness`) for shared profile identities and comparable
consumer/device measurements. Contributors need access and an established local
checkout. The [local skill](.agents/skills/pocket-benchmarking/SKILL.md) locates and
validates its canonical workflow: explicit session root, `POCKET_DEV_TOOLS_ROOT`,
then legacy `POCKET_HARNESS_ROOT`, with conflicting explicit roots resolved first.

## Routine regression checks

Use the [shared preserved-baseline contract](https://github.com/zenibako/pocket-dev-tools/blob/fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58/benchmarks/REGRESSIONS.md)
(introduced in shared commit `fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58`) from the verified
`pocket-dev-tools` checkout. Require its `benchmarks/REGRESSIONS.md` and record the
actual shared commit; do not silently substitute an older checkout. Keep private
source exports, accepted evidence, indices and candidate/failure records under
that checkout's ignored directories. Do not ship them in this project's releases.

For a routine OS/RTL change, preserve an accepted source/toolchain/configuration
and its native test evidence; build each candidate independently in the native
isolated job directories. Bind the netlist/MIF, firmware/OS, bitstream/loader,
CPU/variant macros, compiler flags, target/clock, fitter seed and fit/STA results
to the exact source revisions and artifact hashes. Do not overwrite the reference
or modify pins/seeds merely to make a comparison eligible.

Run the affected native RTL and byte-exact acceptance suites named in
[the workflow guide](CLAUDE.md#tests-verilator). `make test` is only its regression
set; it does not run every GPU/scanout/CRAM/system suite. Preserve applicable
negative tests, complete pass/fail logs and known pre-existing failures. Record
private baseline paths/digests and retain candidates and failed evidence under
new names. A source, compiler or platform-configuration change is a separately
recorded experiment with explicit differences and an accepted reference.

Shared `regression.py` verifies supported Butterscotch frames; it is not an OS or
generic consumer regression runner. Existing native RTL, hardware-contract,
firmware pairing, fit/hold/DQ/STA, boot-memory and physical acceptance remain
required. Test a known bad change against the relevant native oracle rather
than treating a consumer's self-consistent replay as OS qualification.

Consumer benchmark bundles supplement those checks. `verify` establishes archive
integrity and `compare` reports compatible numeric deltas, without rerunning OS
or game correctness or enforcing an automatic slowdown threshold. Keep host,
simulated and device domains distinct. Controlled platform mode still requires
the same ELF/toolchain; rebuilt ELFs use declared whole-configuration comparison.
The shared workload's pinned model does not validate new production gateware,
SD behavior, audio, Memories or sleep/wake; preserve each affected device gate.

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
