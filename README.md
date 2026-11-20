# openfpgaOS

Bare-metal OS and RISC-V (VexiiRiscv) FPGA core for the Analogue Pocket
(`pocket`) and MiSTer (`mister`). Apps are built with the
[openfpgaOS SDK](https://github.com/ThinkElastic/openfpgaOS-SDK).

## Requirements

- Docker (or Apple `container` on Apple silicon), git, make, bash
- Quartus (not redistributable): drop the Linux installer tarball anywhere
  under `tools/`
  - pocket: Quartus Prime 25.1 Lite or Standard (`Quartus-*-linux.tar`)
  - mister: Quartus 17.0.x (installer tar or a pre-extracted
    `altera-17-quartus.tar*`)
- Verilator 5.x, only for `make test`

The first build creates the toolchain containers (the Quartus image takes
about 10 minutes, once).

## Build

```bash
make full                    # CPU, bootloader, OS and bitstream (default: pocket)
make full TARGET=mister      # MiSTer core
make build VARIANT=os30      # one Pocket variant bitstream (os20, os25, os30)
make firmware                # rebuild bootloader + os.bin into the last bitstream
make sweep VARIANT=os30      # fitter seed search, stored in seeds/<variant>.seed
make test                    # Verilator RTL tests
make package                 # SD-card tree in build/ (pocket) or release zip (mister)
make sdk DEST=<sdk checkout> # copy headers and runtime into an SDK checkout
```

`make use-mister` or `make use-os30` sets a sticky default; `make` lists all
goals.

For consumer-specific platform selection and shared device benchmark evidence,
use [BENCHMARKING.md](BENCHMARKING.md). It routes profiles, coverage and the three
comparison modes through pocket-harness while retaining native hardware gates.

## Save states (Pocket)

Pocket sleep/wake and Memories are one feature, switched per core by
`"sleep_supported"` in that core's `core.json` (`dist/core/core.json` ships
`false`). Every app launched through a core shares the setting, so either
every app under a core supports save states or the ones that do ship as a
core of their own.

- `true`: the Pocket offers sleep and Memories (Create Memory, load a
  Memory). On a bitstream with save-state support (`os20`, `os25`; not
  `os30`) the OS forwards each request to the app.
- `false`: Memories are greyed out and sleep powers the Pocket off, even on
  a bitstream with save-state support.

What the menu offers is settled when the core loads and does not change at
run time; a save or load the core refuses after that simply fails. Set it
`true` only when every app under the core implements save states and the
bitstream it ships has the service (`OF_HW_SAVESTATE`), and call
`of_savestate_enable()` only in an app shipped with it `true`; see
`src/firmware/api/of_savestate.h` for the API.

`make sdk` writes `runtime/pocket/SAVESTATES` in the SDK, listing the
published bitstreams that have the service, one file name per line (for
example `os25.rbf_r`). Packaging should set `"sleep_supported"` from that
file for the bitstream it ships, rather than from whether `of_savestate.h`
exists: the header ships with every runtime, `os30` included.

The Pocket lists every app's Memories under the core together, so an app
should name its game with `of_savestate_set_id()` (up to 16 bytes, such as a
hash of its data). A state from another game then fails to load with
`OF_ERR_DENIED`, distinct from `OF_ERR_IO` for a damaged one, and the app can
tell the player why: the Pocket itself only says "Load failed".

## Retained Quake2 release

The supported Quake2 I64 release is a frozen, hardware-tested app/core pair,
separate from the general development source tree:

```bash
python3 tools/core_release.py verify
python3 tools/core_release.py export  # new build/releases/os30-quake2/
python3 tools/core_release.py stage   # new build/releases/os30-quake2-sources/
```

The [profile instructions](release/os30-quake2/README.txt) include the exact
sources, binary hashes, licenses and complete 30-seed timing evidence.
Seed 16 estimates **90.69 MHz**, with the clock configured at 100 MHz; this
does **not** establish 100 MHz timing closure. The slower I32 cache cut and
the I64 RAM repack that failed the >90 MHz sweep are excluded.

Use Quake2's approved release targets with this core. Keep SM64's separately
tested core. Generic `make build`, `make package` and `make sdk` operate on
the development tree and do not reproduce these specialized release pairs.
No release-profile command writes to an SD card or publishes a release.

See [os20/os25 qualification](release/OS20-OS25.txt) for the faster os20 divider,
the retained os25 build, rejected trials and scoped SDK update instructions.

## Acknowledgments

VexiiRiscv (SpinalHDL, Charles Papon), musl libc, Analogizer and SNAC
reference (RndMnkIII), openFPGA (Analogue), MiSTer framework (MiSTer-devel,
Sorgelig), FatFs (ChaN).
