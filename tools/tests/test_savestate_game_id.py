#!/usr/bin/env python3
"""Run the actual Pocket set-id and load functions with host MMIO simulation."""
from pathlib import Path
import subprocess
import tempfile

from test_async_save_abort import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/firmware/os/targets/pocket/savestate.c"


def main():
    source = SOURCE.read_text()
    defines = "".join(line + "\n" for line in source.splitlines()
                      if line.startswith(("#define SS_MAGIC", "#define SS_VERSION",
                                          "#define SS_HEADER_BYTES", "#define SS_ID_")))
    struct_start = source.index("typedef struct {")
    struct_end = source.index("} ss_header_t;", struct_start) + len("} ss_header_t;")
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "of_error.h"
#define SS_DRAIN_TIMEOUT_MS 2000u
#define CRAM0_MODE_CPU 1u
#define CRAM0_MODE_BRIDGE 0u
#define DS_STATUS_WR_IDLE 1u
#define SS_NONE 0
#define SS_LOAD 2
static volatile uint32_t CRAM0_MODE, SS_WR_WORDS, DS_STATUS = DS_STATUS_WR_IDLE;
static uint32_t window[64];
#define SS_BLOB window
#define SS_WINDOW_SIZE ((uint32_t)sizeof(window))
static int ss_pending, completions, last_ok, holds;
static int ss_present(void) { return 1; }
static void fence(void) {}
static int ss_quiesce_files(void) { return 1; }
static uint32_t ss_irq_save(void) { return 0; }
static void ss_irq_restore(uint32_t p) { (void)p; }
static uint32_t of_timer_get_ms(void) { return 0; }
static void of_save_begin_cpu(void) { ++holds; }
static void of_save_end_cpu(void) { --holds; }
static void ss_complete(int kind, int ok) { assert(kind == SS_LOAD); ++completions; last_ok = ok; }
'''
    body = prelude + defines + source[struct_start:struct_end] + "\n"
    body += "static uint32_t ss_id[SS_ID_WORDS];\n"
    start = source.index("static const uint32_t crc32_table[256]")
    body += source[start:source.index("};", start) + 2] + "\n"
    for name in ["crc32_update", "crc32_buf", "ss_read_stable", "ss_read_words",
                 "of_savestate_set_id", "ss_id_zero", "ss_id_matches",
                 "of_savestate_finish", "of_savestate_load"]:
        body += function(source, name) + "\n"
    body += r'''
static const uint8_t payload[9] = {9, 8, 7, 6, 5, 4, 3, 2, 1};

/* Put a state in the window as the host would, stamped with id (16 bytes,
 * or NULL for a state from before ids: all zero). */
static void host_writes(const uint8_t *id) {
    memset(window, 0xcc, sizeof(window));
    ss_header_t h = {.magic = SS_MAGIC, .version = SS_VERSION,
                     .payload_len = sizeof(payload),
                     .payload_crc = crc32_buf(payload, sizeof(payload))};
    uint32_t w[SS_ID_WORDS] = {0};
    for (uint32_t i = 0; id && i < SS_ID_BYTES; i++) w[i / 4] |= (uint32_t)id[i] << (8 * (i % 4));
    h.game_id = w[0];
    for (uint32_t i = 1; i < SS_ID_WORDS; i++) h.game_id_ext[i - 1] = w[i];
    memcpy(window, &h, sizeof(h));
    memcpy((uint8_t *)window + SS_HEADER_BYTES, payload, sizeof(payload));
    SS_WR_WORDS = (SS_HEADER_BYTES + sizeof(payload) + 3) / 4;
}

static int load(void) {
    uint8_t buf[64];
    ss_pending = SS_LOAD; completions = 0; last_ok = -1;
    int n = of_savestate_load(buf, sizeof(buf));
    assert(holds == 0);
    if (n > 0) {
        assert(n == (int)sizeof(payload) && !memcmp(buf, payload, sizeof(payload)));
        assert(completions == 0 && ss_pending == SS_LOAD);   /* the app finishes */
        of_savestate_finish(1);
    } else {
        assert(completions == 1 && last_ok == 0 && ss_pending == SS_NONE);
    }
    return n;
}

int main(void) {
    const uint8_t a[16] = "game A data hash", b[16] = "game B data hash";
    const uint8_t short_a[3] = {1, 2, 3}, short_a_padded[16] = {1, 2, 3};

    /* No id on either side: everything intact loads, as before ids. */
    host_writes(NULL); assert(load() == (int)sizeof(payload));
    host_writes(a);    assert(load() == (int)sizeof(payload));

    assert(of_savestate_set_id(a, sizeof(a)) == 0);
    host_writes(a);    assert(load() == (int)sizeof(payload));
    host_writes(b);    assert(load() == OF_ERR_DENIED);
    host_writes(NULL); assert(load() == (int)sizeof(payload)); /* saved before ids */

    /* Damage is damage, whatever the id: the CRC is checked first. */
    host_writes(b); ((uint8_t *)window)[SS_HEADER_BYTES] ^= 1; assert(load() == OF_ERR_IO);
    host_writes(b); window[0] ^= 1;                              assert(load() == OF_ERR_IO);
    host_writes(b); SS_WR_WORDS -= 1;                            assert(load() == OF_ERR_IO);

    /* Short ids are zero-padded; len 0 clears; bad arguments are refused. */
    assert(of_savestate_set_id(short_a, sizeof(short_a)) == 0);
    host_writes(short_a_padded); assert(load() == (int)sizeof(payload));
    host_writes(a);              assert(load() == OF_ERR_DENIED);
    assert(of_savestate_set_id(NULL, 0) == 0);
    host_writes(b);              assert(load() == (int)sizeof(payload));
    assert(of_savestate_set_id(a, 17) == OF_ERR_INVALID_PARAM);
    assert(of_savestate_set_id(NULL, 1) == OF_ERR_INVALID_PARAM);
    puts("PASS: game id set/pad/clear, other game DENIED, damage IO, pre-id states load");
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
