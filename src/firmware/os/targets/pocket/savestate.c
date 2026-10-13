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

static uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    return crc;
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

    if (prev_mode && of_save_cpu_held()) {
        CRAM0_MODE = CRAM0_MODE_CPU;
        for (volatile int s = 0; s < 8; s++) {}
    }
    return 0;
}

int of_savestate_load(void *buf, uint32_t cap) {
    if (!ss_present())
        return OF_ERR_NOT_SUPPORTED;
    if (ss_pending != SS_LOAD)
        return OF_ERR_INVALID_STATE;

    ss_quiesce_files();

    /* Let any blob words still queued in the bridge write FIFO land. */
    CRAM0_MODE = CRAM0_MODE_BRIDGE;
    for (volatile int s = 0; s < 8; s++) {}
    uint32_t t0 = of_timer_get_ms();
    while (!(DS_STATUS & DS_STATUS_WR_IDLE) &&
           (uint32_t)(of_timer_get_ms() - t0) < SS_DRAIN_TIMEOUT_MS) {}

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
