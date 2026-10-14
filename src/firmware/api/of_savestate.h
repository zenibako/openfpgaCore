//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

/*
 * openfpgaOS Save States -- Analogue Pocket sleep/wake and "Memories"
 *
 * The Pocket saves and restores a core through two host commands; on
 * bitstreams with OF_HW_SAVESTATE the OS forwards them to the app.  The app
 * decides what a state contains: the OS only moves an opaque blob (up to the
 * size the app declared) between the app and the Pocket, adding a header
 * with a CRC and the instance's game id, so a corrupt state or one from
 * another game is refused instead of restored.
 *
 * Turning it on, per app: "sleep_supported" in the core's core.json is the
 * one switch for both features.  true offers sleep and Memories ("Create
 * Memory" and loading one); false powers off instead of sleeping and greys
 * out Memories, even on a bitstream with OF_HW_SAVESTATE (measured on a
 * Pocket, 2026-10-14).  The Pocket reads it, and the core's own "supported"
 * answer, once when the core loads, so nothing the app or OS does at run
 * time can change it.  Set it true only for an app that answers requests
 * as below, on a runtime that has this service, and call
 * of_savestate_enable() only in an app shipped with it true: with it false
 * the Pocket never asks, and an enabled app only holds memory and CRAM0
 * handling for nothing.  Neither the app nor the OS can read core.json,
 * so keep the two in step at build time.
 *
 * Usage, once per frame at a point where the app's state is consistent:
 *
 *   of_savestate_enable(MY_MAX_STATE);           // once, at startup
 *   ...
 *   switch (of_savestate_poll()) {
 *   case OF_SAVESTATE_SAVE:
 *       n = serialize(buf, sizeof buf);
 *       if (n > 0) of_savestate_save(buf, n);     // reports ok to the Pocket
 *       else       of_savestate_finish(0);        // refuse this one
 *       break;
 *   case OF_SAVESTATE_LOAD:
 *       n = of_savestate_load(buf, sizeof buf);   // < 0: already refused
 *       if (n > 0) of_savestate_finish(restore(buf, n) == 0);
 *       break;
 *   }
 *
 * Sleep is a save followed by power-off.  On wake the Pocket reloads the core
 * and pushes the state back at once -- measured on hardware, the blob and the
 * load request are already there when the OS starts, long before any app.
 * The OS holds that request for the app, so call of_savestate_enable() early
 * and poll from the first frame.  Requests the app never answers are refused
 * by the OS (saves at once when save states are not enabled; loads after
 * 30 s).
 *
 * The OS hands the Pocket only the state just saved (header + payload), not
 * the maximum declared with of_savestate_enable(), so saves stay short and
 * carry no stale bytes.  (Apps that declare the real size around each save,
 * as pico8pocket and Butterscotch did before this, keep working.)
 *
 * Lessons from the first two ports (pico8pocket, Butterscotch/Undertale):
 *  - The Pocket files Memories and the sleep state under whatever is loaded
 *    in data slot 0.  A core that launches through a launcher/instance file
 *    in slot 0 gets every game's Memories in one list, so put your own game
 *    identity in the state, refuse a state from another game, and tell the
 *    player why: the Pocket itself only says "Load failed".
 *  - Refusing a save during sleep does not keep the Pocket awake: it powers
 *    off anyway, and the next wake loads the previous sleep state.  Refuse
 *    only what cannot be restored, and say so on screen.
 *  - Set "sleep_supported" in core.json only when the runtime you ship has
 *    this service (OF_HW_SAVESTATE); otherwise sleep turns into a cold boot
 *    and every Memory fails.
 *  - A save or load is refused with OF_ERR_BUSY if the file layer does not
 *    go idle within two seconds (a streaming callback that keeps submitting
 *    reads), rather than waiting forever.
 *
 * Contract while enabled: of_savestate_poll() may hand CRAM0 back to the
 * bridge, so do not keep pointers into the CRAM0 app-DMA pool across it.
 * of_savestate_save() blocks until the Pocket has read the blob (normally a
 * fraction of a second; the game is paused in the Pocket menu meanwhile).
 */

#ifndef OF_SAVESTATE_H
#define OF_SAVESTATE_H

#include <stdint.h>
#include <stddef.h>
#include "of_services.h"
#include "of_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OF_SAVESTATE_NONE  0
#define OF_SAVESTATE_SAVE  1   /* the Pocket wants a state: of_savestate_save() */
#define OF_SAVESTATE_LOAD  2   /* a state is waiting: of_savestate_load() */

/* Largest app payload a single state may hold (1 MB window minus header). */
#define OF_SAVESTATE_MAX_PAYLOAD (0x100000u - 32u)

#ifndef OF_PC

#define OF_SAVESTATE_SVC_FIELD_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - \
                 offsetof(struct of_services_table, video_init)) / sizeof(void *)))
#define OF_SAVESTATE_SVC_HAS(field) \
    (OF_SVC->count > OF_SAVESTATE_SVC_FIELD_INDEX(field) && OF_SVC->field)

/* Declare the largest state this app will produce and start answering the
 * Pocket.  0 turns save states off again.  OF_ERR_NOT_SUPPORTED on
 * bitstreams/OS images without the feature. */
static inline int of_savestate_enable(uint32_t max_size) {
    if (!OF_SAVESTATE_SVC_HAS(savestate_enable))
        return OF_ERR_NOT_SUPPORTED;
    return OF_SVC->savestate_enable(max_size);
}

/* OF_SAVESTATE_NONE / _SAVE / _LOAD.  Call once per frame. */
static inline int of_savestate_poll(void) {
    if (!OF_SAVESTATE_SVC_HAS(savestate_poll))
        return OF_SAVESTATE_NONE;
    return OF_SVC->savestate_poll();
}

/* Answer a pending save with the app's state.  Returns 0 or an OF_ERR_*. */
static inline int of_savestate_save(const void *blob, uint32_t len) {
    if (!OF_SAVESTATE_SVC_HAS(savestate_save))
        return OF_ERR_NOT_SUPPORTED;
    return OF_SVC->savestate_save(blob, len);
}

/* Copy a pending load's payload into buf.  Returns its length, or an
 * OF_ERR_* (in which case the OS has already told the Pocket it failed).
 * On success the app must call of_savestate_finish() after restoring. */
static inline int of_savestate_load(void *buf, uint32_t cap) {
    if (!OF_SAVESTATE_SVC_HAS(savestate_load))
        return OF_ERR_NOT_SUPPORTED;
    return OF_SVC->savestate_load(buf, cap);
}

/* Report the outcome of the pending request (ok = 0 refuses / fails it). */
static inline void of_savestate_finish(int ok) {
    if (OF_SAVESTATE_SVC_HAS(savestate_finish))
        OF_SVC->savestate_finish(ok);
}

#else /* OF_PC */

static inline int  of_savestate_enable(uint32_t max_size) { (void)max_size; return OF_ERR_NOT_SUPPORTED; }
static inline int  of_savestate_poll(void) { return OF_SAVESTATE_NONE; }
static inline int  of_savestate_save(const void *blob, uint32_t len) { (void)blob; (void)len; return OF_ERR_NOT_SUPPORTED; }
static inline int  of_savestate_load(void *buf, uint32_t cap) { (void)buf; (void)cap; return OF_ERR_NOT_SUPPORTED; }
static inline void of_savestate_finish(int ok) { (void)ok; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SAVESTATE_H */
