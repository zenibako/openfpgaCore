#!/usr/bin/env python3
"""Host exercise of the actual Pocket async routines with simulated MMIO.

CSR IRQ masking, savestate sampling and command dispatch are stubbed. Extract
routines rather than compiling unrelated RISC-V file/cache HAL code. Translate
the DS_STATUS W1C assignment into simulated W1C; all async logic is unchanged.
The fixture supplies subsequent hardware status samples.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/firmware/os/targets/pocket/file.c"


def function(source, name):
    import re
    match = re.search(r"^(?:static )?(?:inline )?(?:int|void|uint32_t) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, name
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    source = SOURCE.read_text()
    state = source[source.index("static struct {"):source.index("/* Local IRQ mask")]
    routines = ["async_finalize", "async_complete", "async_copy_slice", "async_copy_drain"]
    if "static int async_quarantine_gate(" in source:
        routines += ["async_quarantine_gate"]
    routines += ["async_gate", "of_file_read_async", "of_file_async_irq_service"]
    if "static void async_abort_pending_save(" in source:
        routines += ["async_abort_pending_save"]
    routines += ["of_file_async_poll", "of_file_async_tick", "of_file_async_busy", "of_file_cram0_quiet"]
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OF_ERR_BUSY -16
#define OF_ERR_TIMEOUT -12
#define OF_ERR_BAD_RANGE -3
#define DS_STATUS_ACK 1u
#define DS_STATUS_DONE 2u
#define DS_STATUS_ERR_MASK 0x1cu
#define DS_STATUS_ERR_SHIFT 2
#define DS_STATUS_READY 32u
#define DS_STATUS_WR_IDLE 64u
#define DS_STATUS_IRQ_PENDING 128u
#define IRQ_MASK_DATASLOT 32u
#define CRAM0_MODE_CPU 1u
#define CRAM0_MODE_BRIDGE 0u
#define CRAM0_BASE 0x30000000u
#define OF_TARGET_CRAM0_ASYNC_BOUNCE_OFFSET 0x410000u
#define OF_TARGET_CRAM0_DMA_CHUNK_SIZE 65536u
#define TIMER_CTRL_ENABLE 1u
#define ASYNC_COPY_SLICE_BYTES 2048u
#define ASYNC_AGE_LIMIT_MS 2000u
#define DS_CMD_READ 1u
#define of_term_printf(...) ((void)0)
static uint32_t DS_STATUS, IRQ_MASK, CRAM0_MODE, TIMER_CTRL;
static uint32_t DS_SLOT_ID, DS_SLOT_OFFSET, DS_BRIDGE_ADDR, DS_LENGTH;
static int async_token_counter, bridge_warmup_active = 1;
static int request, polls, commands, callbacks, last_token, last_result;
static uint32_t mie = 8u;
static uint32_t file_irq_save(void) { uint32_t p = mie; mie = 0; return p; }
static void file_irq_restore(uint32_t p) { mie = p; }
static void bridge_warmup_once(void) { assert(0); }
static int async_dest_in_cram0(void *p, uint32_t n) { (void)p; (void)n; return 1; }
static uint32_t cpu_to_bridge(void *p) { return (uint32_t)(uintptr_t)p; }
int of_savestate_poll(void) {
    assert(mie == 0 && async_state.active && !async_state.copy_pending);
    ++polls; return request;
}
static void ds_status_w1c(uint32_t bits) { DS_STATUS &= ~bits; }
static int ds_issue_command(uint32_t cmd) {
    assert(cmd == DS_CMD_READ);
    /* Old pending IRQ must be W1C-cleared before dispatch. Acceptance resets
       the DONE latch even when READY/WR_IDLE remain high before host ACK. */
    assert(!(DS_STATUS & DS_STATUS_IRQ_PENDING));
    ++commands; DS_STATUS = DS_STATUS_READY | DS_STATUS_WR_IDLE; return 0;
}
static void callback(int token, int result) {
    assert(mie == 0); ++callbacks; last_token = token; last_result = result;
}
'''
    # The savestate stub observes the real async state declaration.
    prefix, rest = prelude.split("int of_savestate_poll", 1)
    body = prefix + state + "int of_savestate_poll" + rest
    body += "\n".join(function(source, name).replace("DS_STATUS = DS_STATUS_IRQ_PENDING;", "ds_status_w1c(DS_STATUS_IRQ_PENDING);") for name in routines)
    body += r'''
static int issue(void) {
    uint32_t irq = file_irq_save();
    int token = of_file_read_async(4, 0, (void *)(uintptr_t)0x30500000u, 64, callback);
    file_irq_restore(irq); return token;
}
int main(void) {
    DS_STATUS = DS_STATUS_READY | DS_STATUS_WR_IDLE;
    int first = issue(); assert(first >= 0 && commands == 1);
    unsigned char app_buffer[64], bounce_buffer[64];
    memset(app_buffer, 0x11, sizeof(app_buffer));
    memset(bounce_buffer, 0x22, sizeof(bounce_buffer));
    async_state.dest = app_buffer;
    async_state.dma_dest = bounce_buffer;
    async_state.bounce_to_dest = 1;
    request = 1; /* SAVE while accepted file read has no ACK/DONE */
    assert(of_file_async_poll() == 1);
    assert(callbacks == 1 && last_token == first && last_result == OF_ERR_TIMEOUT);
    assert(!of_file_async_busy() && of_file_cram0_quiet());
    assert(CRAM0_MODE == CRAM0_MODE_BRIDGE && mie == 8u);
    assert(!(IRQ_MASK & IRQ_MASK_DATASLOT));
    for (unsigned i = 0; i < sizeof(app_buffer); ++i) assert(app_buffer[i] == 0x11);
    assert(issue() == OF_ERR_BUSY && commands == 1); /* READY alone isn't DONE */
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_IRQ_PENDING;
    assert(issue() == OF_ERR_BUSY && commands == 1); /* writes still draining */
    of_file_async_irq_service(); /* stale completion cannot copy or callback */
    assert(callbacks == 1 && CRAM0_MODE == CRAM0_MODE_BRIDGE);
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE | DS_STATUS_IRQ_PENDING;
    request = 0;
    int second = issue(); assert(second > first && commands == 2);
    assert(of_file_async_poll() == 0 && callbacks == 1); /* no stale token result */
    request = 2; /* LOAD must keep this read alive */
    assert(of_file_async_poll() == 0 && of_file_async_busy());
    of_file_async_tick(); assert(callbacks == 1 && of_file_async_busy());
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE;
    assert(of_file_async_poll() == 1);
    assert(callbacks == 2 && last_token == second && last_result == 0);
    assert(CRAM0_MODE == CRAM0_MODE_CPU);
    DS_STATUS = DS_STATUS_READY | DS_STATUS_WR_IDLE;
    int third = issue(); request = 1;
    of_file_async_tick(); /* timer path also breaks mutual wait promptly */
    assert(callbacks == 3 && last_token == third && last_result == OF_ERR_TIMEOUT);
    assert(!of_file_async_busy() && of_file_async_poll() == 1);
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE;
    request = 0; assert(issue() > third);
    async_state.age_ms = ASYNC_AGE_LIMIT_MS - 1;
    of_file_async_tick(); /* legacy age-out receives the same quarantine */
    assert(callbacks == 4 && last_result == OF_ERR_TIMEOUT);
    assert(issue() == OF_ERR_BUSY);
    assert(async_gate() == OF_ERR_BUSY); /* sync fallback also blocked */
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE;
    int fifth = issue(); request = 1;
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE | DS_STATUS_IRQ_PENDING;
    assert(of_file_async_poll() == 1); /* completion wins over pending SAVE */
    assert(callbacks == 5 && last_token == fifth && last_result == 0);
    DS_STATUS = DS_STATUS_READY | DS_STATUS_WR_IDLE;
    int sixth = issue();
    async_state.dest = app_buffer;
    async_state.dma_dest = bounce_buffer;
    async_state.bounce_to_dest = 1;
    TIMER_CTRL = TIMER_CTRL_ENABLE;
    DS_STATUS = DS_STATUS_DONE | DS_STATUS_READY | DS_STATUS_WR_IDLE | DS_STATUS_IRQ_PENDING;
    of_file_async_tick(); /* deferred successful bounce copy is not aborted */
    assert(async_state.copy_pending && callbacks == 5);
    assert(of_file_async_poll() == 1);
    assert(callbacks == 6 && last_token == sixth && last_result == 0);
    assert(memcmp(app_buffer, bounce_buffer, sizeof(app_buffer)) == 0);
    assert(polls > 0 && mie == 8u);
    puts("PASS: SAVE abort, quarantine, late IRQ, LOAD, completion and age-out");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        c = Path(temp) / "test.c"
        exe = Path(temp) / "test"
        c.write_text(body)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
