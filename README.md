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

Pocket sleep/wake and Memories are one feature, switched per app by
`"sleep_supported"` in the core's `core.json` (`dist/core/core.json` ships
`false`):

- `true`: the Pocket offers sleep and Memories (Create Memory, load a
  Memory), and the OS forwards each request to the app.
- `false`: Memories are greyed out and sleep powers the Pocket off, even on
  a bitstream with save-state support.

The Pocket reads it once when the core loads, so it cannot change at run
time. Set it `true` only for an app that implements save states, and call
`of_savestate_enable()` only in an app shipped with it `true`; see
`src/firmware/api/of_savestate.h` for the API.

## Acknowledgments

VexiiRiscv (SpinalHDL, Charles Papon), musl libc, Analogizer and SNAC
reference (RndMnkIII), openFPGA (Analogue), MiSTer framework (MiSTer-devel,
Sorgelig), FatFs (ChaN).
