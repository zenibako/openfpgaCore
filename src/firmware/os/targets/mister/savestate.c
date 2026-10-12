//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

/* No APF save-state host on this target: every call reports "not supported". */

#include "savestate.h"
#include "of_error.h"

int  of_savestate_enable(uint32_t max_size) { (void)max_size; return OF_ERR_NOT_SUPPORTED; }
int  of_savestate_poll(void) { return 0; }
int  of_savestate_save(const void *blob, uint32_t len) { (void)blob; (void)len; return OF_ERR_NOT_SUPPORTED; }
int  of_savestate_load(void *buf, uint32_t cap) { (void)buf; (void)cap; return OF_ERR_NOT_SUPPORTED; }
void of_savestate_finish(int ok) { (void)ok; }
void of_savestate_tick(void) {}
