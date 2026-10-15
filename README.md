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

## Acknowledgments

VexiiRiscv (SpinalHDL, Charles Papon), musl libc, Analogizer and SNAC
reference (RndMnkIII), openFPGA (Analogue), MiSTer framework (MiSTer-devel,
Sorgelig), FatFs (ChaN).
