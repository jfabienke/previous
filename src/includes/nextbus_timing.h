/*
  Previous - nextbus_timing.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The time the CPU waits for the NeXTbus (see nextbus_timing.c).
*/

#pragma once

#ifndef PREV_NEXTBUS_TIMING_H
#define PREV_NEXTBUS_TIMING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Whether accesses to boards are timed (checked before each access) */
extern bool NextBusTiming_Enabled;

/* Read the configuration and settings, forget all transactions. */
extern void NextBusTiming_Reset(void);

/* An access of size bytes (1, 2 or 4) to the board in a slot. */
extern void NextBusTiming_Access(int slot, int size, bool write);

/* The next four longword accesses are one MOVE16 line transfer. */
extern void NextBusTiming_BurstBegin(void);
extern void NextBusTiming_BurstEnd(void);

/* The CPU cycles waited for the bus since the last call (whole cycles; the
 * rest is kept for the next). */
extern int NextBusTiming_TakeStall(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PREV_NEXTBUS_TIMING_H */
