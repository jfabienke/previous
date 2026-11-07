/*
  Previous - nextbus_rust.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The NeXTbus, timed cycle by cycle by the Rust model (see nextbus_rust.c).
*/

#pragma once

#ifndef PREV_NEXTBUS_RUST_H
#define PREV_NEXTBUS_RUST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* How the NeXTbus is timed: PREVIOUS_NEXTBUS_TIMING, or bNextBusTiming */
#define NEXTBUS_TIMING_OFF     0 /* not at all */
#define NEXTBUS_TIMING_CYCLE   1 /* cycle by cycle (next-bus, through nd-ffi) */
#define NEXTBUS_TIMING_INTERIM 2 /* the transaction model in nextbus_timing.c */
extern int NextBus_TimingMode;

/* The instruction running made an access the model timed. */
extern bool NextBusRust_Touched;

/* Choose the mode and set up the model; then put the boards in their
 * slots. Called when the boards are installed and at every reset. */
extern void NextBusRust_Init(void);
extern void NextBusRust_Attach(int slot);
extern void NextBusRust_Reset(void);
/* Write the telemetry files and free the model. */
extern void NextBusRust_Exit(void);

/* Whether addr is NeXTbus space the model times: board space of slots 2-7
 * or slot space of slots 1-15 (slot 0's is the CPU board's own). */
extern bool NextBusRust_InSpace(uint32_t addr);

/* One access by the running instruction: 0, or 1 when the 68040 takes a
 * bus error (after the time it waited). */
extern int NextBusRust_Access(uint32_t addr, int size, bool write);
/* A MOVE16 line at addr; its four long words then cost nothing more. For
 * a board attached with its data, data holds a write's words and gets a
 * read's (the four long-word accesses then read them). */
extern int NextBusRust_Line(uint32_t addr, bool write, uint32_t* data);

/* A Rust ND board (nd_board*) in slot, attached so that its accesses move
 * their data through the model (PREVIOUS_NEXTBUS_DATA, on by default):
 * false if it is not (then the board moves its own data). */
extern bool NextBusRust_AttachBoard(int slot, void* board, bool threaded);
extern void NextBusRust_DetachBoard(int slot);
/* The board in slot is attached with its data; and run by the model. */
extern bool NextBusRust_HasData(int slot);
extern bool NextBusRust_AdvancesBoard(int slot);
/* Run the attached lockstep boards to the current instruction's start. */
extern void NextBusRust_Advance(void);
/* An access to an attached board by the running instruction, with its
 * data: 0, 1 for a bus error, 2 if the board faulted (reads give 0). */
extern int NextBusRust_AccessData(uint32_t addr, int size, bool write, uint32_t* val);
/* The bus cycle of an access to the CPU board NBIC's registers (NCS*). */
extern void NextBusRust_Registers(uint32_t offset, int size, bool write);
/* The running instruction ends: the cycles to charge it instead. */
extern uint32_t NextBusRust_EndInstruction(uint32_t cycles);

/* The CPU board NBIC's registers, held by the model: at offset in NCS*
 * space (0x02020000) or, with slot_space, in slot 0's slot space. 0, or 1
 * for a bus error. Only when NextBusRust_Registers_Available(). */
extern bool NextBusRust_Registers_Available(void);
extern int NextBusRust_RegRead(bool slot_space, uint32_t offset, int size, uint32_t *val);
extern int NextBusRust_RegWrite(bool slot_space, uint32_t offset, int size, uint32_t val);
/* The CPU board NBIC's STFWD. */
extern bool NextBusRust_StoreForward(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PREV_NEXTBUS_RUST_H */
