#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# netlist_hash <generated-qsf> [extra-files...] -> one md5 over sources/macros.
# Extra files cover includes and variant definitions outside the qsf file list.
# A stored fitter seed only means anything for the netlist it was
# swept on: any RTL change reshuffles placement and makes the seed a lottery
# ticket.  That once shipped a WNS -1.08 black-screen os20 from a seed swept at
# -0.156.  sweep.sh records the hash beside the seed; `make build` compares.
netlist_hash() {
    local qsf="$1"
    shift
    [ -f "$qsf" ] || { echo "no-qsf"; return; }
    {
        grep -E "VERILOG_MACRO|SEED " "$qsf" | grep -v "SEED " | sort
        # MIF_FILE counts: the boot ROM is baked into the bitstream, so a
        # firmware rebuild changes what the fitter places even when every .v is
        # byte-identical.  Leaving it out once let an os30 sweep fail 12/12
        # against an unchanged RTL fingerprint.
        # Quartus rewrites the qsf during a compile and adds a qsf-relative
        # entry for every absolute one, so the same sources appear once before
        # the fit and twice after.  Resolve each entry to one physical path
        # (relative entries against the qsf's directory, as Quartus does) and
        # count each file once.  Otherwise a fresh bld/<job>/ hashed
        # differently from the post-fit qsf that sweep.sh records.
        grep -E "(SYSTEM)?VERILOG_FILE|QIP_FILE|MIF_FILE" "$qsf" \
        | awk '{print $NF}' \
        | while read -r f; do
            case "$f" in
                /*) p="$f" ;;
                *)  p="$(dirname "$qsf")/$f"; [ -f "$p" ] || p="$f" ;;
            esac
            [ -f "$p" ] || continue
            printf '%s/%s\n' "$(cd "$(dirname "$p")" && pwd -P)" "$(basename "$p")"
          done | sort -u \
        | while read -r p; do
            # Hash CONTENT only: md5sum <file> would include the path, which
            # carries the per-seed build dir, so the same netlist hashed
            # differently from bld/<v>-s<best>/ and bld/<v>/ and the staleness
            # warning fired on every build.
            md5sum < "$p"
          done | sort
        for f in "$@"; do
            if [ -f "$f" ]; then md5sum < "$f"
            elif [ -f "$(dirname "$qsf")/$f" ]; then md5sum < "$(dirname "$qsf")/$f"
            else printf 'missing:%s\n' "$f"
            fi
        done
    } | md5sum | cut -d' ' -f1
}
