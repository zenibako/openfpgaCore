//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------
//
// Verilator cosim for scaler_geom_cdc.v inside the Pocket raster
// (tb_scaler_geom.v).  clk_cpu runs at 100 MHz and clk_vid at 24.576 MHz,
// unrelated, so the handshake sees every phase relationship.
//
// Build/run: make scaler-geom   (test/Makefile target)
//
#include "Vtb_scaler_geom.h"
#include "verilated.h"
#include <cstdio>
#include <cstdint>
#include <set>

static Vtb_scaler_geom *tb;
static uint64_t now_ps = 0, next_cpu = 0, next_vid = 0;
static const uint64_t CPU_HALF = 5000;    // 100 MHz
static const uint64_t VID_HALF = 20345;   // ~24.576 MHz
static int passed = 0, failed = 0;
static std::set<uint32_t> presented;      // every word the CPU side showed

static uint32_t word(uint32_t slot, uint32_t w, uint32_t h) {
    return (slot << 20) | (w << 10) | h;
}

static void check(const char *name, bool ok, long got = -1, long want = -1) {
    if (ok) { passed++; return; }
    failed++;
    printf("  FAIL %s (got %ld want %ld)\n", name, got, want);
}

// Advance to the next clock edge (whichever comes first).
static void step() {
    if (next_cpu <= next_vid) {
        now_ps = next_cpu; tb->clk_cpu = !tb->clk_cpu; next_cpu += CPU_HALF;
    } else {
        now_ps = next_vid; tb->clk_vid = !tb->clk_vid; next_vid += VID_HALF;
    }
    tb->eval();
}

// One clk_cpu rising edge.
static void cpu_cycle() {
    do { step(); } while (!(tb->clk_cpu && now_ps == next_cpu - CPU_HALF));
}

static void present(uint32_t slot, uint32_t w, uint32_t h) {
    tb->slot_cpu = slot; tb->width_cpu = w; tb->height_cpu = h;
    presented.insert(word(slot, w, h));
}

struct Frame { unsigned lines, pixels, first; bool mixed; uint32_t word; };

// Run until the next frame_done pulse and return that frame's stats.
static Frame next_frame() {
    bool was = tb->frame_done;
    for (;;) {
        step();
        if (tb->clk_vid && tb->frame_done && !was) break;
        was = tb->frame_done;
    }
    Frame f{tb->last_de_lines, tb->last_de_pixels, tb->last_first_line,
            (bool)tb->last_mixed,
            word(tb->slot_vid, tb->width_vid, tb->height_vid)};
    return f;
}

static void check_frame(const char *name, const Frame &f, unsigned w, unsigned h) {
    char buf[96];
    unsigned first = 18 + (h < 240 ? (240 - h) / 2 : 0);
    snprintf(buf, sizeof buf, "%s-lines", name);  check(buf, f.lines == h, f.lines, h);
    snprintf(buf, sizeof buf, "%s-pixels", name); check(buf, f.pixels == w, f.pixels, w);
    snprintf(buf, sizeof buf, "%s-centre", name); check(buf, f.first == first, f.first, first);
    snprintf(buf, sizeof buf, "%s-no-mid-frame-change", name); check(buf, !f.mixed);
    snprintf(buf, sizeof buf, "%s-not-torn", name);
    check(buf, presented.count(f.word) == 1);
}

// After a change, the new geometry must be in place within two frames.
static Frame settle(const char *name, unsigned w, unsigned h) {
    Frame f = next_frame();
    f = next_frame();
    f = next_frame();   // first full frame entirely on the new geometry
    check_frame(name, f, w, h);
    return f;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    tb = new Vtb_scaler_geom;
    tb->clk_cpu = 0; tb->clk_vid = 0; tb->reset_n = 0; tb->v_total = 525;
    next_cpu = CPU_HALF; next_vid = VID_HALF / 3;   // unrelated phase
    present(0, 320, 240);
    for (int i = 0; i < 40; i++) step();
    tb->reset_n = 1;

    printf("=== scaler_geom_cdc: runtime slot geometry ===\n");
    settle("reset-320x240", 320, 240);

    present(6, 128, 128);
    settle("slot6-128x128", 128, 128);

    present(4, 320, 288);
    settle("slot4-320x288", 320, 288);

    // Back-to-back changes, one per clk_cpu cycle (4x faster than clk_vid):
    // intermediate words may be skipped, but the last one must arrive and
    // nothing in between may be torn.
    for (int i = 0; i < 40; i++) {
        present(1 + (i % 6), 100 + 7 * i, 50 + 3 * i);
        cpu_cycle();
    }
    present(6, 128, 128);
    cpu_cycle();
    settle("burst-then-128x128", 128, 128);

    // Program the active slot's width, then select another slot on the next
    // cycle (the case the review called out).
    present(6, 160, 128);
    cpu_cycle();
    present(2, 320, 224);
    cpu_cycle();
    settle("program-then-select", 320, 224);

    // Tall slot with a longer frame (640x576 needs V_TOTAL >= 594).
    tb->v_total = 600;
    present(5, 640, 576);
    settle("slot5-640x576", 640, 576);
    tb->v_total = 525;

    present(0, 320, 240);
    settle("back-to-320x240", 320, 240);

    printf("=== Results: %d passed, %d failed ===\n", passed, failed);
    delete tb;
    return failed ? 1 : 0;
}
