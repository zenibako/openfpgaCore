//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

/*
 * openfpgaOS Save-State HAL (APF sleep/wake + "Memories").
 *
 * The app owns the content of a save state; the OS moves it between the app
 * and the host.  See api/of_savestate.h for the app-facing contract.
 * Targets without hardware support implement every call as "not supported".
 */

#ifndef OFOS_SAVESTATE_H
#define OFOS_SAVESTATE_H

#include <stdint.h>

int  of_savestate_enable(uint32_t max_size);
int  of_savestate_poll(void);
int  of_savestate_save(const void *blob, uint32_t len);
int  of_savestate_load(void *buf, uint32_t cap);
void of_savestate_finish(int ok);
int  of_savestate_set_id(const void *id, uint32_t len);

/* Called from of_check_shutdown() -- the timer IRQ and every busy-wait
 * spin, so at no fixed rate: denies requests that no app will answer (save
 * states never enabled, or a load nobody picks up in time). */
void of_savestate_tick(void);

/* Nonzero when the host had already asked for a state load before the OS
 * started (wake from sleep / Memory opened from the Pocket menu).  Valid
 * once the hardware contract has been checked; 0 on targets without it. */
int  of_savestate_boot_pending(void);

#endif /* OFOS_SAVESTATE_H */
