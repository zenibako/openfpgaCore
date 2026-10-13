#!/usr/bin/env python3
"""Run the actual Pocket save function with host timer/MMIO simulation."""
from pathlib import Path
import subprocess
import tempfile

from test_async_save_abort import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/firmware/os/targets/pocket/savestate.c"


def main():
    source = SOURCE.read_text()
    struct_start = source.index("typedef struct {")
    struct_end = source.index("} ss_header_t;", struct_start) + len("} ss_header_t;")
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "of_error.h"
#define SS_MAGIC 0x5353464fu
#define SS_VERSION 1u
#define SS_HEADER_BYTES 32u
#define SS_HOST_READ_TIMEOUT_MS 10000u
#define CRAM0_MODE_CPU 1u
#define CRAM0_MODE_BRIDGE 0u
#define SYS_GAME_ID 123u
#define SS_NONE 0
#define SS_SAVE 1
static volatile uint32_t SS_SIZE, SS_RD_WORDS, CRAM0_MODE;
static uint32_t window[32];
#define SS_BLOB window
#define SS_WINDOW_SIZE ((uint32_t)sizeof(window))
static uint32_t declared_size = 40;   /* header + one padded payload word pair */
static int ss_pending;
static uint32_t ss_max_payload;
static uint32_t clock_ms, origin, read_delay, delivered_words, elapsed;
static uint32_t mie = 8u;
static int completions, hold_count, initial_hold;
static int ss_present(void) { return 1; }
static void fence(void) {}
static void ss_quiesce_files(void) {}
static uint32_t ss_irq_save(void) { uint32_t p = mie; mie = 0; return p; }
static void ss_irq_restore(uint32_t p) { mie = p; }
static void of_save_begin_cpu(void) { ++hold_count; CRAM0_MODE = CRAM0_MODE_CPU; }
static void of_save_end_cpu(void) {
    assert(hold_count > 0);
    if (--hold_count == 0) CRAM0_MODE = CRAM0_MODE_BRIDGE;
}
static int of_save_cpu_held(void) { return hold_count != 0; }
static void ss_complete(int kind, int ok) {
    assert(kind == SS_SAVE && ok == 1 && mie == 0);
    assert(CRAM0_MODE == CRAM0_MODE_BRIDGE);
    assert(hold_count == initial_hold);
    assert(window[0] == SS_MAGIC && window[3] == 7);
    ++completions;
}
static uint32_t of_timer_get_ms(void) {
    assert(completions == 1 && ss_pending == SS_NONE);
    assert(CRAM0_MODE == CRAM0_MODE_BRIDGE);
    uint32_t result = clock_ms;
    clock_ms += 1000u;
    elapsed = (uint32_t)(clock_ms - origin);
    if (elapsed >= read_delay) SS_RD_WORDS = delivered_words;
    assert(elapsed <= SS_HOST_READ_TIMEOUT_MS + 2000u);
    return result;
}
void of_savestate_finish(int ok);
'''
    body = prelude + source[struct_start:struct_end] + "\n"
    if "static const uint32_t crc32_table[256]" in source:
        start = source.index("static const uint32_t crc32_table[256]")
        end = source.index("};", start) + 2
        body += source[start:end] + "\n"
    for name in ["crc32_update", "crc32_buf", "ss_read_stable", "ss_write_words", "of_savestate_save", "of_savestate_finish"]:
        body += function(source, name) + "\n"
    body += r'''
static uint32_t reference_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int k = 0; k < 8; ++k)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}
static uint32_t reference_crc(const uint8_t *p, uint32_t len) {
    uint32_t crc = UINT32_MAX;
    while (len--) crc = reference_update(crc, *p++);
    return ~crc;
}
static void test_crc(void) {
    assert(crc32_buf(NULL, 0) == 0);
    assert(crc32_buf((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    const uint32_t seeds[] = {0, UINT32_MAX, 0x12345678u, 0x80000000u};
    uint8_t buf[4097];
    const uint32_t lengths[] = {0, 1, 2, 7, 8, 31, 32, 255, 256, 257, 4095, 4096, 4097};
    for (unsigned byte = 0; byte < 256; ++byte) {
        for (unsigned seed = 0; seed < sizeof(seeds)/sizeof(*seeds); ++seed)
            assert(crc32_update(seeds[seed], (uint8_t)byte) == reference_update(seeds[seed], (uint8_t)byte));
        memset(buf, byte, sizeof(buf));
        for (unsigned n = 0; n < sizeof(lengths)/sizeof(*lengths); ++n)
            assert(crc32_buf(buf, lengths[n]) == reference_crc(buf, lengths[n]));
    }
    uint32_t random = 0x31415926u;
    for (unsigned i = 0; i < sizeof(buf); ++i) {
        random = random * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(random >> 24);
    }
    uint32_t incremental = UINT32_MAX;
    for (unsigned len = 0; len <= sizeof(buf); ++len) {
        /* Verify every prefix, including incremental state and all lengths. */
        assert(crc32_buf(buf, len) == reference_crc(buf, len));
        assert(~incremental == crc32_buf(buf, len));
        if (len < sizeof(buf)) incremental = crc32_update(incremental, buf[len]);
    }
    for (unsigned split = 0; split <= sizeof(buf); split += 127) {
        uint32_t crc = UINT32_MAX;
        for (unsigned i = 0; i < split; ++i) crc = crc32_update(crc, buf[i]);
        for (unsigned i = split; i < sizeof(buf); ++i) crc = crc32_update(crc, buf[i]);
        assert(~crc == reference_crc(buf, sizeof(buf)));
    }
    puts("PASS: CRC standard vector, every byte, every prefix, incremental parity");
    fflush(stdout);
}
static void run(uint32_t start, uint32_t delay, uint32_t words, int expected, int held) {
    const uint8_t payload[7] = {1,2,3,4,5,6,7};
    memset(window, 0xcc, sizeof(window));
    ss_pending = SS_SAVE; ss_max_payload = 7;
    SS_SIZE = declared_size; SS_RD_WORDS = 0;
    origin = clock_ms = start; elapsed = 0;
    read_delay = delay; delivered_words = words;
    completions = 0; initial_hold = hold_count = held;
    CRAM0_MODE = held ? CRAM0_MODE_CPU : CRAM0_MODE_BRIDGE;
    assert(of_savestate_save(payload, sizeof(payload)) == expected);
    assert(completions == 1 && ss_pending == SS_NONE && mie == 8u);
    assert(hold_count == held);
    assert(CRAM0_MODE == (held ? CRAM0_MODE_CPU : CRAM0_MODE_BRIDGE));
    assert(memcmp((uint8_t *)window + SS_HEADER_BYTES, payload, sizeof(payload)) == 0);
    assert(window[4] == crc32_buf(payload, sizeof(payload)));
    /* The host reads SS_SIZE bytes: everything after the payload up to
     * there is zeroed, and nothing beyond it is touched. */
    assert(((uint8_t *)window)[SS_HEADER_BYTES + sizeof(payload)] == 0);
    for (uint32_t w = 10; w < declared_size / 4u; w++) assert(window[w] == 0);
    for (uint32_t w = declared_size / 4u; w < 32; w++) assert(window[w] == 0xccccccccu);
    if (expected == OF_ERR_TIMEOUT) assert(elapsed >= SS_HOST_READ_TIMEOUT_MS);
    else assert(elapsed < SS_HOST_READ_TIMEOUT_MS);
    /* An app reporting the returned error must not emit another toggle. */
    of_savestate_finish(0); assert(completions == 1);
}
int main(void) {
    test_crc();
    run(0, 3000, 0, OF_ERR_TIMEOUT, 0); /* host never reads */
    run(0, 3000, 9, OF_ERR_TIMEOUT, 0); /* one rounded word missing */
    run(0, 3000, 10, 0, 0);           /* all rounded words received */
    run(0, 3000, 11, 0, 1);          /* excess count + nested CPU hold */
    run(UINT32_MAX - 2500u, 3000, 10, 0, 1); /* success across timer wrap */
    run(UINT32_MAX - 2500u, 3000, 0, OF_ERR_TIMEOUT, 1);
    run(UINT32_MAX - 2500u, 3000, 9, OF_ERR_TIMEOUT, 0);
    declared_size = 56;                 /* a declared maximum above the payload */
    run(0, 3000, 14, 0, 0);             /* the zeroed tail is part of the read */
    run(0, 3000, 13, OF_ERR_TIMEOUT, 0);
    declared_size = 40;
    puts("PASS: actual save complete/partial/zero, timer wrap, ownership, one toggle");
}
'''
    with tempfile.TemporaryDirectory() as temp:
        c = Path(temp) / "test.c"
        exe = Path(temp) / "test"
        c.write_text(body)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I" + str(ROOT / "src/firmware/api"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
