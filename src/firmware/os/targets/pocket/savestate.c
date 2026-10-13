//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

/*
 * openfpgaOS Save-State HAL — Pocket (APF sleep/wake + "Memories").
 *
 * Bitstreams with HW_FEATURES bit 31 answer the host's 0x00A0 (save) and
 * 0x00A4 (load) commands in hardware (savestate_ctrl.v): acknowledge at once,
 * report busy, and flip a request toggle in SS_STATUS.  Firmware finishes the
 * request through SS_CTRL.  The blob travels through a 1 MB CRAM0 window at
 * offset 6 MB (bridge 0x20600000):
 *
 *   Save: app hands us its state -> we write [header][payload] to the window,
 *         give CRAM0 to the bridge, report ok, and hold CRAM0 on the bridge
 *         side until the host has read SS_SIZE bytes (SS_RD_WORDS).
 *   Load: the host has already written the blob into the window before its
 *         0x00A4 (bridge writes there are queued even while the CPU owns
 *         CRAM0).  We drain the write FIFO, validate the header + CRC and
 *         hand the payload to the app, which reports success with
 *         of_savestate_finish().
 *
 * The host reads a fixed SS_SIZE (header + the app's declared maximum), so the
 * header carries the real payload length.  It also carries the instance's
 * game id so a state from another game is refused rather than restored.
 */

#include "savestate.h"
#include "save.h"
#include "file.h"
#include "regs.h"
#include "timer.h"
#include "of_caps.h"
#include "of_error.h"

#define SS_MAGIC            0x5353464Fu   /* "OFSS" */
#define SS_VERSION          1u
#define SS_HEADER_BYTES     32u
#define SS_BLOB             ((volatile uint32_t *)(uintptr_t)(OF_TARGET_CRAM0_BASE + SS_CRAM0_OFFSET))

/* How long to keep CRAM0 on the bridge waiting for the host to pull a saved
 * blob, and how long an unanswered load may wait for the app to start and
 * pick it up (wake: the app has to boot first). */
#define SS_HOST_READ_TIMEOUT_MS  10000u
#define SS_LOAD_PICKUP_TIMEOUT_MS 30000u
#define SS_DRAIN_TIMEOUT_MS      2000u

enum { SS_NONE = 0, SS_SAVE = 1, SS_LOAD = 2 };

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t game_id;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t reserved[3];
} ss_header_t;

_Static_assert(sizeof(ss_header_t) == SS_HEADER_BYTES, "header size");
_Static_assert(SS_CRAM0_OFFSET >= OF_TARGET_CRAM0_APP_DMA_OFFSET + OF_TARGET_CRAM0_APP_DMA_SIZE,
               "save-state window overlaps the app DMA pool");

static volatile int      ss_enabled;
static volatile uint32_t ss_max_payload;
static volatile int      ss_pending;
static volatile uint32_t ss_seen;          /* last request toggles consumed */
static volatile uint32_t ss_load_t0_ms;   /* of_timer_get_ms() when a load was seen */

static inline uint32_t ss_irq_save(void) {
    uint32_t prev;
    __asm__ volatile("csrrci %0, mstatus, 0x8" : "=r"(prev) :: "memory");
    return prev & 0x8u;
}

static inline void ss_irq_restore(uint32_t prev) {
    if (prev)
        __asm__ volatile("csrsi mstatus, 0x8" ::: "memory");
}

static int ss_present(void) {
    return hw_feature_present(OF_HW_SAVESTATE);
}

/* Quasi-static counters from clk_74a: read until two reads agree. */
static uint32_t ss_read_stable(volatile uint32_t *reg) {
    uint32_t a = *reg, b = *reg;
    while (a != b) {
        a = b;
        b = *reg;
    }
    return a;
}

/* Reflected CRC-32/ISO-HDLC, polynomial 0xEDB88320. Each entry is
 * eight steps of the previous bit recurrence starting with its byte index.
 * Initial/final complement stays in crc32_buf; update remains incremental. */
static const uint32_t crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    return (crc >> 8) ^ crc32_table[(crc ^ byte) & 0xFFu];
}

static uint32_t crc32_buf(const uint8_t *p, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
        crc = crc32_update(crc, *p++);
    return ~crc;
}

/* Complete the pending request (caller holds IRQs off or owns ss_pending). */
static void ss_complete(int kind, int ok) {
    if (kind == SS_SAVE)
        SS_CTRL = SS_CTRL_SAVE_DONE | (ok ? 0u : SS_CTRL_SAVE_ERR);
    else if (kind == SS_LOAD)
        SS_CTRL = SS_CTRL_LOAD_DONE | (ok ? 0u : SS_CTRL_LOAD_ERR) |
                  SS_CTRL_CLEAR_COUNTS;
}

/* Pick up a new host request, if any.  Saves win over loads when both
 * toggled (the host never issues them together). */
static void ss_sample(void) {
    uint32_t now = SS_STATUS & (SS_STATUS_START_REQ | SS_STATUS_LOAD_REQ);
    uint32_t changed = now ^ ss_seen;
    if (!changed)
        return;
    ss_seen = now;
    if (ss_pending != SS_NONE)
        ss_complete(ss_pending, 0);   /* superseded */
    if (changed & SS_STATUS_START_REQ) {
        ss_pending = SS_SAVE;
        if (changed & SS_STATUS_LOAD_REQ)
            ss_complete(SS_LOAD, 0);
    } else {
        ss_pending = SS_LOAD;
        ss_load_t0_ms = of_timer_get_ms();
    }
}

/* Wait for any in-flight async file transfer (and its deferred bounce copy)
 * so nothing flips CRAM0 to the CPU behind our back. */
static void ss_quiesce_files(void) {
    while (of_file_async_busy() || !of_file_cram0_quiet())
        of_file_async_poll();
}

static int ss_cram0_parkable(void) {
    return !of_save_cpu_held() && of_file_cram0_quiet() && !of_file_async_busy();
}

int of_savestate_boot_pending(void) {
    return ss_present() && (SS_STATUS & SS_STATUS_LOAD_REQ) != 0;
}

void of_savestate_tick(void) {
    if (!ss_present())
        return;
    ss_sample();
    if (ss_pending == SS_NONE)
        return;
    if (!ss_enabled) {
        if (ss_pending == SS_SAVE) {
            ss_complete(SS_SAVE, 0);           /* no app support: deny now */
            ss_pending = SS_NONE;
        } else if ((uint32_t)(of_timer_get_ms() - ss_load_t0_ms) >=
                   SS_LOAD_PICKUP_TIMEOUT_MS) {
            /* Wall-clock, not a call count: this runs from the timer IRQ
             * AND from every spin of of_timer_delay_us(), far above 1 kHz. */
            ss_complete(SS_LOAD, 0);           /* wake, but no app took it */
            ss_pending = SS_NONE;
        }
    }
}

int of_savestate_enable(uint32_t max_size) {
    if (!ss_present())
        return OF_ERR_NOT_SUPPORTED;
    if (max_size > SS_WINDOW_SIZE - SS_HEADER_BYTES)
        return OF_ERR_INVALID_PARAM;

    uint32_t irq = ss_irq_save();
    if (max_size == 0) {
        ss_enabled = 0;
        ss_max_payload = 0;
        SS_SIZE = 0;
    } else {
        ss_max_payload = max_size;
        SS_SIZE = SS_HEADER_BYTES + ((max_size + 3u) & ~3u);
        ss_enabled = 1;
    }
    ss_irq_restore(irq);
    return 0;
}

int of_savestate_poll(void) {
    if (!ss_present() || !ss_enabled)
        return SS_NONE;

    uint32_t irq = ss_irq_save();
    ss_sample();
    int pending = ss_pending;
    ss_irq_restore(irq);

    /* Called at the app's frame boundary, where no file operation is in
     * progress: park CRAM0 on the bridge so a blob the host pushes for a
     * load lands straight in CRAM0 instead of waiting in the 4 KB FIFO. */
    if ((CRAM0_MODE & CRAM0_MODE_CPU) && ss_cram0_parkable()) {
        fence();
        CRAM0_MODE = CRAM0_MODE_BRIDGE;
        for (volatile int s = 0; s < 8; s++) {}
    }
    return pending;
}

static void ss_write_words(uint32_t word_off, const uint8_t *src, uint32_t len) {
    volatile uint32_t *dst = SS_BLOB + word_off;
    uint32_t i = 0;
    for (; i + 4u <= len; i += 4u)
        *dst++ = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) |
                 ((uint32_t)src[i + 2] << 16) | ((uint32_t)src[i + 3] << 24);
    if (i < len) {
        uint32_t w = 0;
        for (uint32_t k = 0; i + k < len; k++)
            w |= (uint32_t)src[i + k] << (8u * k);
        *dst = w;
    }
}

static void ss_read_words(uint32_t word_off, uint8_t *dst, uint32_t len) {
    volatile const uint32_t *src = SS_BLOB + word_off;
    uint32_t i = 0;
    for (; i + 4u <= len; i += 4u) {
        uint32_t w = *src++;
        dst[i] = (uint8_t)w;
        dst[i + 1] = (uint8_t)(w >> 8);
        dst[i + 2] = (uint8_t)(w >> 16);
        dst[i + 3] = (uint8_t)(w >> 24);
    }
    if (i < len) {
        uint32_t w = *src;
        for (uint32_t k = 0; i + k < len; k++)
            dst[i + k] = (uint8_t)(w >> (8u * k));
    }
}

int of_savestate_save(const void *blob, uint32_t len) {
    if (!ss_present())
        return OF_ERR_NOT_SUPPORTED;
    if (ss_pending != SS_SAVE)
        return OF_ERR_INVALID_STATE;
    if (!blob || len == 0 || len > ss_max_payload) {
        of_savestate_finish(0);
        return OF_ERR_INVALID_PARAM;
    }

    ss_quiesce_files();

    ss_header_t h = {
        .magic = SS_MAGIC,
        .version = SS_VERSION,
        .game_id = SYS_GAME_ID,
        .payload_len = len,
        .payload_crc = crc32_buf((const uint8_t *)blob, len),
    };

    of_save_begin_cpu();
    fence();
    ss_write_words(0, (const uint8_t *)&h, sizeof(h));
    ss_write_words(SS_HEADER_BYTES / 4u, (const uint8_t *)blob, len);
    /* The host reads SS_SIZE bytes whatever the payload length: zero the
     * rest so a state file never carries an earlier state's tail. */
    {
        uint32_t used = (SS_HEADER_BYTES + len + 3u) / 4u;
        uint32_t end = SS_SIZE / 4u;
        if (end > SS_WINDOW_SIZE / 4u)
            end = SS_WINDOW_SIZE / 4u;
        for (uint32_t w = used; w < end; w++)
            SS_BLOB[w] = 0;
    }
    fence();
    of_save_end_cpu();

    /* The host reads the blob only after it sees "ok", and only while the
     * bridge owns CRAM0.  Hand it over and keep it there until all SS_SIZE
     * bytes have gone out (or the host gives up). */
    uint32_t prev_mode = CRAM0_MODE & CRAM0_MODE_CPU;
    CRAM0_MODE = CRAM0_MODE_BRIDGE;
    for (volatile int s = 0; s < 8; s++) {}

    uint32_t want_words = SS_SIZE / 4u;
    uint32_t irq = ss_irq_save();
    ss_complete(SS_SAVE, 1);
    ss_pending = SS_NONE;
    ss_irq_restore(irq);

    uint32_t t0 = of_timer_get_ms();
    while (ss_read_stable(&SS_RD_WORDS) < want_words &&
           (uint32_t)(of_timer_get_ms() - t0) < SS_HOST_READ_TIMEOUT_MS) {
        /* Spin: the app is paused here, so nothing else can claim CRAM0. */
    }

    /* "Blob ready" was already signalled once, before the host could read.
     * Report an incomplete read to the app without issuing a second toggle. */
    int result = ss_read_stable(&SS_RD_WORDS) >= want_words ? 0 : OF_ERR_TIMEOUT;

    if (prev_mode && of_save_cpu_held()) {
        CRAM0_MODE = CRAM0_MODE_CPU;
        for (volatile int s = 0; s < 8; s++) {}
    }
    return result;
}

int of_savestate_load(void *buf, uint32_t cap) {
    if (!ss_present())
        return OF_ERR_NOT_SUPPORTED;
    if (ss_pending != SS_LOAD)
        return OF_ERR_INVALID_STATE;
    if (!buf) {
        of_savestate_finish(0);
        return OF_ERR_INVALID_PARAM;
    }

    ss_quiesce_files();

    /* Let any blob words still queued in the bridge write FIFO land. */
    CRAM0_MODE = CRAM0_MODE_BRIDGE;
    for (volatile int s = 0; s < 8; s++) {}
    uint32_t t0 = of_timer_get_ms();
    while (!(DS_STATUS & DS_STATUS_WR_IDLE) &&
           (uint32_t)(of_timer_get_ms() - t0) < SS_DRAIN_TIMEOUT_MS) {}
    if (!(DS_STATUS & DS_STATUS_WR_IDLE)) {
        /* Still draining: the window may be incomplete, and taking CRAM0
         * now would strand the queued words.  Fail rather than read it. */
        of_savestate_finish(0);
        return OF_ERR_IO;
    }

    uint32_t got_words = ss_read_stable(&SS_WR_WORDS);
    ss_header_t h;
    int rc = 0;

    of_save_begin_cpu();
    fence();
    ss_read_words(0, (uint8_t *)&h, sizeof(h));
    if (h.magic != SS_MAGIC || h.version != SS_VERSION)
        rc = OF_ERR_IO;
    else if (h.game_id != SYS_GAME_ID)
        rc = OF_ERR_DENIED;
    else if (h.payload_len == 0 || h.payload_len > cap ||
             h.payload_len > SS_WINDOW_SIZE - SS_HEADER_BYTES)
        rc = OF_ERR_BAD_RANGE;
    else if (got_words < (SS_HEADER_BYTES + h.payload_len + 3u) / 4u)
        rc = OF_ERR_IO;                       /* blob arrived short */
    else
        ss_read_words(SS_HEADER_BYTES / 4u, (uint8_t *)buf, h.payload_len);
    fence();
    of_save_end_cpu();

    if (rc == 0 && crc32_buf((const uint8_t *)buf, h.payload_len) != h.payload_crc)
        rc = OF_ERR_IO;

    if (rc != 0) {
        of_savestate_finish(0);
        return rc;
    }
    return (int)h.payload_len;
}

void of_savestate_finish(int ok) {
    if (!ss_present())
        return;
    uint32_t irq = ss_irq_save();
    if (ss_pending != SS_NONE) {
        ss_complete(ss_pending, ok);
        ss_pending = SS_NONE;
    }
    ss_irq_restore(irq);
}
