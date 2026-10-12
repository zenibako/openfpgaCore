//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

// Drives the Pocket host side of the save-state commands the way agg23's
// openFPGA-NES expects them: save = 0xA0 start, poll 0xA0 until ok, read the
// blob; load = write the blob, 0xA4 load, poll 0xA4 until ok.

#include "Vtb_savestate.h"
#include <verilated.h>
#include <cstdio>
#include <cstdint>

static Vtb_savestate *tb;
static int passes = 0, fails = 0;

static constexpr uint32_t HOST_CMD = 0xF8000000u;
static constexpr uint32_t HOST_P20 = 0xF8000020u;
static constexpr uint32_t HOST_R40 = 0xF8000040u;
static constexpr uint32_t HOST_R44 = 0xF8000044u;
static constexpr uint32_t HOST_R48 = 0xF8000048u;
static constexpr uint32_t BLOB     = 0x20600000u;

static void tick() {
    tb->clk = 0; tb->eval();
    tb->clk = 1; tb->eval();
}
static void ticks(int n) { while (n-- > 0) tick(); }

static void check_eq(const char *tag, uint32_t got, uint32_t expect) {
    if (got == expect) { passes++; std::printf("  OK  %s\n", tag); }
    else { fails++; std::printf("  FAIL %s got=0x%08x exp=0x%08x\n", tag, got, expect); }
}

static void write32(uint32_t addr, uint32_t data) {
    tb->bridge_addr = addr; tb->bridge_wr_data = data;
    tb->bridge_wr = 1; tick(); tb->bridge_wr = 0; tick();
}
static uint32_t read32(uint32_t addr) {
    tb->bridge_addr = addr; tb->bridge_rd = 1; tick();
    tb->bridge_rd = 0; tick();
    return tb->bridge_rd_data;
}

/* Issue a host command and wait for its "OK" status word; returns the
 * 16-bit result code (0xFFFF = unknown command, 0xDEAD = timed out). */
static uint32_t host_cmd(uint16_t cmd, uint32_t p20) {
    write32(HOST_P20, p20);
    write32(HOST_CMD, 0x434D0000u | cmd);
    for (int i = 0; i < 200; i++) {
        uint32_t st = read32(HOST_CMD);
        if ((st >> 16) == 0x4F4B) return st & 0xFFFF;
        tick();
    }
    return 0xDEAD;
}

/* CPU side: flip a toggle after a realistic CDC distance. */
static void cpu_complete_save(bool err) {
    tb->cpu_start_err = err; tb->cpu_start_done_tgl ^= 1; ticks(12);
}
static void cpu_complete_load(bool err) {
    tb->cpu_load_err = err; tb->cpu_load_done_tgl ^= 1; ticks(12);
}

static void init() {
    tb->clk = 0; tb->bridge_addr = 0; tb->bridge_rd = 0; tb->bridge_wr = 0;
    tb->bridge_wr_data = 0; tb->cpu_start_done_tgl = 0; tb->cpu_start_err = 0;
    tb->cpu_load_done_tgl = 0; tb->cpu_load_err = 0; tb->cpu_clear_tgl = 0;
    tb->cpu_size = 0;
    ticks(16);
}

static void test_query() {
    std::printf("test_query:\n");
    check_eq("A0-query-result-idle", host_cmd(0x00A0, 0), 0);
    check_eq("A0-supported", read32(HOST_R40), 1);
    check_eq("A0-addr", read32(HOST_R44), BLOB);
    check_eq("A0-size-defaults-to-max", read32(HOST_R48), 0x100000);
    check_eq("A4-query-result-idle", host_cmd(0x00A4, 0), 0);
    check_eq("A4-maxloadsize", read32(HOST_R48), 0x100000);
    tb->cpu_size = 0x12345; ticks(8);
    host_cmd(0x00A0, 0);
    check_eq("A0-size-from-cpu-word-aligned", read32(HOST_R48), 0x12344);
    tb->cpu_size = 0x200000; ticks(8);
    host_cmd(0x00A0, 0);
    check_eq("A0-size-clamped-to-window", read32(HOST_R48), 0x100000);
    tb->cpu_size = 0x40000; ticks(8);
}

static void test_save() {
    std::printf("test_save:\n");
    uint32_t req0 = tb->start_req_tgl;
    host_cmd(0x00A0, 1);
    check_eq("start-req-toggled", tb->start_req_tgl, req0 ^ 1);
    check_eq("poll-busy", host_cmd(0x00A0, 0), 1);
    ticks(50);
    check_eq("still-busy-until-cpu", host_cmd(0x00A0, 0), 1);
    cpu_complete_save(false);
    check_eq("poll-ok", host_cmd(0x00A0, 0), 2);
    check_eq("size-reported", read32(HOST_R48), 0x40000);

    for (uint32_t i = 0; i < 4; i++) read32(BLOB + i * 4);
    read32(0x205FFFFCu);                  // below the window
    read32(0x20700000u);                  // above the window
    check_eq("rd-words-counts-window-only", tb->rd_words, 4);

    // A second save resets the result and the read counter.
    host_cmd(0x00A0, 1);
    check_eq("second-save-busy", host_cmd(0x00A0, 0), 1);
    check_eq("rd-words-reset-on-save", tb->rd_words, 0);
    cpu_complete_save(true);
    check_eq("cpu-denied-save-reports-err", host_cmd(0x00A0, 0), 3);
    check_eq("toggles-without-request-ignored", (cpu_complete_save(false), host_cmd(0x00A0, 0)), 3);
}

static void test_load() {
    std::printf("test_load:\n");
    tb->cpu_clear_tgl ^= 1; ticks(8);
    check_eq("clear-zeroes-wr-words", tb->wr_words, 0);
    for (uint32_t i = 0; i < 3; i++) write32(BLOB + i * 4, 0xC0DE0000u | i);
    write32(0x20500000u, 1);              // app DMA pool, not the blob
    check_eq("wr-words-counts-window-only", tb->wr_words, 3);

    uint32_t req0 = tb->load_req_tgl;
    host_cmd(0x00A4, 1);
    check_eq("load-req-toggled", tb->load_req_tgl, req0 ^ 1);
    check_eq("load-poll-busy", host_cmd(0x00A4, 0), 1);
    check_eq("save-result-cleared-by-load", host_cmd(0x00A0, 0), 0);
    cpu_complete_load(false);
    check_eq("load-poll-ok", host_cmd(0x00A4, 0), 2);

    host_cmd(0x00A4, 1);
    cpu_complete_load(true);
    check_eq("load-err", host_cmd(0x00A4, 0), 3);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    tb = new Vtb_savestate;
    std::printf("=== savestate_ctrl + core_bridge_cmd ===\n");
    init();
    test_query();
    test_save();
    test_load();
    std::printf("\n=== Results: %d passed, %d failed ===\n", passes, fails);
    delete tb;
    return fails == 0 ? 0 : 1;
}
