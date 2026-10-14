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
    tb->por_n = 0; ticks(4);
    tb->por_n = 1; ticks(16);
}

/* Wake: the bitstream has just loaded and the core is still in APF reset
 * (no Reset Exit yet) when the host pushes the blob and asks for a load. */
static void test_wake_in_reset() {
    std::printf("test_wake_in_reset:\n");
    check_eq("core-in-reset-at-power-up", tb->core_reset_n, 0);
    check_eq("save-in-reset-acked", host_cmd(0x00A0, 1) != 0xDEAD, 1);
    check_eq("save-in-reset-refused", host_cmd(0x00A0, 0), 3);
    check_eq("save-in-reset-no-request", tb->start_req_tgl, 0);
    for (uint32_t i = 0; i < 5; i++) write32(BLOB + i * 4, 0xB10B0000u | i);
    check_eq("blob-words-counted-in-reset", tb->wr_words, 5);
    check_eq("load-in-reset-acked", host_cmd(0x00A4, 1) != 0xDEAD, 1);
    check_eq("load-in-reset-reports-ok", host_cmd(0x00A4, 0), 2);
    check_eq("load-left-pending-for-os", tb->load_req_tgl, 1);

    check_eq("reset-exit", host_cmd(0x0011, 0), 0);
    ticks(8);
    check_eq("core-running", tb->core_reset_n, 1);
    check_eq("pending-load-survives-reset-exit", tb->load_req_tgl, 1);
    check_eq("blob-count-survives-reset-exit", tb->wr_words, 5);
    /* The OS answers the pending load; the host already has its ok. */
    cpu_complete_load(false);
    tb->cpu_clear_tgl ^= 1; ticks(8);
    check_eq("os-answer-keeps-request-level", tb->load_req_tgl, 1);
    check_eq("os-clear-zeroes-count", tb->wr_words, 0);
    check_eq("load-ok-still-reported-after-boot", host_cmd(0x00A4, 0), 2);

    /* Warm-reset back to a clean, running core for the tests below. */
    host_cmd(0x0010, 0); ticks(8);
    host_cmd(0x0011, 0); ticks(8);
}

/* Wake with reset already released but the OS and app not up yet (no size
 * registered): the load is answered at once and left pending. */
static void test_wake_before_app() {
    std::printf("test_wake_before_app:\n");
    host_cmd(0x0010, 0); ticks(8);
    host_cmd(0x0011, 0); ticks(8);
    tb->cpu_size = 0; tb->cpu_clear_tgl = 0; tb->cpu_load_done_tgl = 0;
    tb->cpu_start_done_tgl = 0; ticks(16);
    host_cmd(0x0010, 0); ticks(8);        // settle any toggle edges
    host_cmd(0x0011, 0); ticks(8);
    check_eq("core-running-no-app", tb->core_reset_n, 1);
    check_eq("save-before-app-refused", (host_cmd(0x00A0, 1), host_cmd(0x00A0, 0)), 3);
    check_eq("save-before-app-no-request", tb->start_req_tgl, 0);
    for (uint32_t i = 0; i < 7; i++) write32(BLOB + i * 4, 0xFEED0000u | i);
    host_cmd(0x00A4, 1);
    check_eq("load-before-app-ok-at-once", host_cmd(0x00A4, 0), 2);
    check_eq("load-before-app-left-pending", tb->load_req_tgl, 1);
    check_eq("load-before-app-blob-counted", tb->wr_words, 7);
    /* App registers later; the pending request and count must hold. */
    tb->cpu_size = 0x40020; ticks(16);
    check_eq("pending-survives-app-register", tb->load_req_tgl, 1);
    check_eq("count-survives-app-register", tb->wr_words, 7);
    check_eq("ok-still-reported", host_cmd(0x00A4, 0), 2);
    cpu_complete_load(false);
    tb->cpu_clear_tgl ^= 1; ticks(8);
    /* With an app registered, the next load waits for the CPU again. */
    host_cmd(0x00A4, 1);
    check_eq("load-with-app-is-busy", host_cmd(0x00A4, 0), 1);
    cpu_complete_load(false);
    check_eq("load-with-app-ok-after-cpu", host_cmd(0x00A4, 0), 2);
    tb->cpu_size = 0x40000; ticks(8);
}

/* Reset Enter from a running core drops stale requests and counters, so
 * the OS does not see a phantom load after a warm reset. */
static void test_warm_reset() {
    std::printf("test_warm_reset:\n");
    write32(BLOB, 1);
    host_cmd(0x00A4, 1);                  // left unanswered (busy)
    check_eq("reset-enter", host_cmd(0x0010, 0), 0);
    ticks(8);
    check_eq("core-back-in-reset", tb->core_reset_n, 0);
    check_eq("load-req-cleared", tb->load_req_tgl, 0);
    check_eq("start-req-cleared", tb->start_req_tgl, 0);
    check_eq("wr-words-cleared", tb->wr_words, 0);
    check_eq("load-result-cleared", host_cmd(0x00A4, 0), 0);
    check_eq("reset-exit-again", host_cmd(0x0011, 0), 0);
    ticks(8);
    check_eq("still-no-phantom-load", tb->load_req_tgl, 0);
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

/* A fresh FPGA model per wake is important: toggling por_n alone does not
 * recreate core_bridge_cmd's power-up initial state. Exercise the real
 * A4-in-reset -> app registration -> A0 protocol twice after wake. The
 * CPU serializer and host SD persistence are external to this RTL model. */
static void test_two_sleep_wake_cycles() {
    std::printf("test_two_sleep_wake_cycles:\n");
    uint32_t previous_words = 3;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        delete tb;
        tb = new Vtb_savestate;
        init();
        check_eq("reload-starts-in-reset", tb->core_reset_n, 0);
        for (uint32_t i = 0; i < previous_words; ++i)
            write32(BLOB + i * 4, 0x51000000u | (cycle << 16) | i);
        check_eq("wake-blob-arrived", tb->wr_words, previous_words);
        host_cmd(0x00A4, 1);
        check_eq("wake-accepted-before-app", host_cmd(0x00A4, 0), 2);
        check_eq("wake-request-retained", tb->load_req_tgl, 1);
        check_eq("wake-reset-exit", host_cmd(0x0011, 0), 0);
        tb->cpu_size = 0x100000;
        ticks(16);
        cpu_complete_load(false);
        tb->cpu_clear_tgl ^= 1;
        ticks(8);
        check_eq("wake-running", tb->core_reset_n, 1);
        check_eq("wake-traffic-count-cleared", tb->wr_words, 0);
        check_eq("wake-left-no-save-result", host_cmd(0x00A0, 0), 0);

        uint32_t before = tb->start_req_tgl;
        host_cmd(0x00A0, 1);
        check_eq("sleep-requests-new-capture", tb->start_req_tgl, before ^ 1);
        check_eq("sleep-busy-before-new-capture", host_cmd(0x00A0, 0), 1);
        check_eq("sleep-cannot-reuse-wake-result", tb->rd_words, 0);
        const uint32_t new_size = 128 + cycle * 16;
        tb->cpu_size = new_size;
        ticks(8);
        cpu_complete_save(false);
        check_eq("sleep-capture-completed", host_cmd(0x00A0, 0), 2);
        check_eq("sleep-publishes-new-size", read32(HOST_R48), new_size);
        previous_words = new_size / 4;
        for (uint32_t i = 0; i < previous_words; ++i)
            read32(BLOB + i * 4);
        check_eq("sleep-host-read-complete", tb->rd_words, previous_words);

        /* Current app behavior: maximum-size rearm after transfer, followed
         * by physical sleep/reload. Document the sticky query result too. */
        tb->cpu_size = 0x100000;
        ticks(8);
        check_eq("completed-save-remains-queryable", host_cmd(0x00A0, 0), 2);
    }
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    tb = new Vtb_savestate;
    std::printf("=== savestate_ctrl + core_bridge_cmd ===\n");
    init();
    test_wake_in_reset();
    test_query();
    test_save();
    test_load();
    test_wake_before_app();
    test_warm_reset();
    test_two_sleep_wake_cycles();
    std::printf("\n=== Results: %d passed, %d failed ===\n", passes, fails);
    delete tb;
    return fails == 0 ? 0 : 1;
}
