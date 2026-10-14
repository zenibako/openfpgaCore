#!/usr/bin/env python3
"""The Pocket save-state quiesce wait gives up instead of spinning forever.

A streaming app's completion callback can submit its next read from inside
of_file_async_poll(), so the file layer may never go idle. Extract the real
ss_quiesce_files() and drive it with a file layer that stays busy, one that
goes idle, and a timer that wraps.
"""
from pathlib import Path
import subprocess
import tempfile

from test_async_save_abort import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/firmware/os/targets/pocket/savestate.c"


def main():
    source = SOURCE.read_text()
    timeout = [l for l in source.splitlines() if "#define SS_QUIESCE_TIMEOUT_MS" in l][0]
    body = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
''' + timeout + r'''
static uint32_t clock_ms, polls, idle_after;
static uint32_t of_timer_get_ms(void) { return clock_ms++; }
static int of_file_async_busy(void) { return polls < idle_after; }
static int of_file_cram0_quiet(void) { return 1; }
static int of_file_async_poll(void) { ++polls; return 0; }
''' + function(source, "ss_quiesce_files") + r'''
static int run(uint32_t start, uint32_t busy_polls) {
    clock_ms = start; polls = 0; idle_after = busy_polls;
    return ss_quiesce_files();
}
int main(void) {
    assert(run(0, 0) == 1 && polls == 0);               /* already idle */
    assert(run(0, 100) == 1 && polls == 100);           /* goes idle in time */
    assert(run(0, UINT32_MAX) == 0);                    /* never idle: gives up */
    assert(polls < SS_QUIESCE_TIMEOUT_MS + 2u);
    assert(run(UINT32_MAX - 10u, UINT32_MAX) == 0);      /* across timer wrap */
    assert(polls < SS_QUIESCE_TIMEOUT_MS + 2u);
    puts("PASS: quiesce waits for idle and gives up at its deadline");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        c = Path(temp) / "test.c"
        exe = Path(temp) / "test"
        c.write_text(body)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
