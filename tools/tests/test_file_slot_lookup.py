#!/usr/bin/env python3
"""Exercise actual kernel lookup; isolated host binaries, no firmware build.

Pocket fixed slots must not issue GETFILE/primer I/O on registry hits.
MiSTer must still reject stale dynamic mappings and resolve the live name.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OS = ROOT / 'src/firmware/os'


def extract(source, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'missing production function: {name}')
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end]


HARNESS = r'''
#include "platform.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define FILE_SLOT_NAME_MAX 96
static struct { uint32_t slot_id; const char *filename; } file_slots[4];
static int file_slot_count;
static const char *host_name;
static int get_calls, get_error, resolve_calls;
static const char *resolved_name;
static int resolved_id;
static int char_lower(unsigned char c) { return tolower(c); }
static int of_file_get_name(uint32_t id, char *out, size_t n) {
    assert(id == 10 || id == 20 || id == 21);
    ++get_calls;
    if (get_error) return -1;
    snprintf(out, n, "%s", host_name);
    return 0;
}
static int of_file_resolve_name(const char *name) {
    ++resolve_calls;
    return strcmp(name, resolved_name) == 0 ? resolved_id : -1;
}
static void reset(void) {
    file_slot_count = 1; file_slots[0].slot_id = 10;
    file_slots[0].filename = "undertale_0.sav";
    host_name = "undertale_0.sav"; get_calls = get_error = resolve_calls = 0;
    resolved_name = "never"; resolved_id = -1;
}
'''

CASES = r'''
int main(void) {
    reset(); assert(file_slot_lookup("/UNDERTALE_0.SAV") == 10);
#if OF_TARGET_PLATFORM_ID == OF_PLATFORM_POCKET
    assert(get_calls == 0 && resolve_calls == 0);
#else
    assert(get_calls == 1 && resolve_calls == 0);
#endif
    reset(); assert(file_slot_lookup("folder/undertale_0.sav") == 10);
    assert(resolve_calls == 1); /* full-path resolution remains before basename */
#if OF_TARGET_PLATFORM_ID == OF_PLATFORM_POCKET
    assert(get_calls == 0);
#else
    assert(get_calls == 1);
#endif
    /* Full registered paths take precedence over an earlier basename hit. */
    reset(); file_slots[0].slot_id = 20; file_slots[0].filename = "pak0.pak";
    file_slots[1].slot_id = 21; file_slots[1].filename = "HIPNOTIC/PAK0.PAK";
    file_slot_count = 2; host_name = "HIPNOTIC/PAK0.PAK";
    assert(file_slot_lookup("/hipnotic/pak0.pak") == 21 && resolve_calls == 0);
    /* A matching host basename remains acceptable in the basename pass. */
    reset(); host_name = "mounted/undertale_0.sav";
    assert(file_slot_lookup("folder/undertale_0.sav") == 10);
    /* MiSTer registry id now points at another file after remount. */
    reset(); host_name = "different.sav";
    resolved_name = "undertale_0.sav"; resolved_id = 30;
#if OF_TARGET_PLATFORM_ID == OF_PLATFORM_POCKET
    assert(file_slot_lookup("undertale_0.sav") == 10 && get_calls == 0 && resolve_calls == 0);
#else
    assert(file_slot_lookup("undertale_0.sav") == 30 && get_calls == 2 && resolve_calls == 1);
#endif
    reset(); host_name = "different.sav";
    resolved_name = "undertale_0.sav"; resolved_id = 30;
#if OF_TARGET_PLATFORM_ID == OF_PLATFORM_POCKET
    assert(file_slot_lookup("folder/undertale_0.sav") == 10 && get_calls == 0 && resolve_calls == 1);
#else
    assert(file_slot_lookup("folder/undertale_0.sav") == 30 && get_calls == 1 && resolve_calls == 2);
#endif
    /* Preserve direct subpath resolution before a basename registry match. */
    reset(); resolved_name = "folder/undertale_0.sav"; resolved_id = 30;
    assert(file_slot_lookup("folder/undertale_0.sav") == 30 && get_calls == 0 && resolve_calls == 1);
    reset(); get_error = 1;
    assert(file_slot_lookup("undertale_0.sav") == 10 && resolve_calls == 0);
    reset(); host_name = "";
    assert(file_slot_lookup("undertale_0.sav") == 10 && resolve_calls == 0);
    reset(); file_slot_count = 0;
    assert(file_slot_lookup("missing.sav") == -1 && get_calls == 0 && resolve_calls == 1);
    return 0;
}
'''


class LookupTests(unittest.TestCase):
    def test_fixed_and_dynamic_registry_behavior(self):
        source = (OS / 'kernel/syscall.c').read_text()
        production = '\n'.join(extract(source, name) for name in
                               ('stricmp', 'path_basename', 'file_slot_lookup'))
        with tempfile.TemporaryDirectory(prefix='os-slot-lookup-') as directory:
            path = Path(directory)
            c = path / 'test.c'
            c.write_text(HARNESS + production + CASES)
            for target in ('pocket', 'mister', 'sim'):
                with self.subTest(target=target):
                    binary = path / target
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', '-fsanitize=undefined',
                                    '-I', str(OS / 'hal'), '-I', str(OS / 'targets' / target),
                                    '-I', str(ROOT / 'src/firmware/api'),
                                    str(c), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
