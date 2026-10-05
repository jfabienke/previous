/*
  Previous - nd_rust.hpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  NDRustBoard: a NeXTdimension emulated by the nextdimension-archive's Rust
  board (emulator/nd-ffi, nd_ffi.h), in lockstep with the m68k.
*/

#pragma once

#ifndef __ND_RUST_H__
#define __ND_RUST_H__

#ifdef __cplusplus

#include "nd_board.hpp"
#include "nd_ffi.h"

class NDRustBoard : public NDBoard {
    nd_board* board;
    NDSDL     sdl;
    uint64_t  hostCycles;   /* m68k cycles since the board was created */
    int       ledState;
    int       ledTicks;
    uint64_t  lastInstructions;
    uint64_t  lastHostTime;
    char      report[128];
    /* PREVIOUS_ND_SNAPSHOT: the screen every snapInterval m68k cycles */
    const char* snapDir;
    uint64_t  snapInterval;
    uint64_t  snapNext;
    int       snapCount;

    void      snapshot(void);
    /* PREVIOUS_ND_STATS: who works, every statsInterval m68k cycles */
    uint64_t  statsInterval;
    uint64_t  statsNext;
    uint64_t  statsInstructions;
    uint64_t  statsHostReads, statsHostWrites;
    uint64_t  statsVramRead, statsVramWritten;

    void      stats(void);

    uint32_t read(int space, uint32_t addr, int size);
    void     write(int space, uint32_t addr, int size, uint32_t val);

public:
    NDRustBoard(int slot);
    virtual ~NDRustBoard();

    virtual uint32_t slot_lget(uint32_t addr);
    virtual uint16_t slot_wget(uint32_t addr);
    virtual uint8_t  slot_bget(uint32_t addr);
    virtual void     slot_lput(uint32_t addr, uint32_t val);
    virtual void     slot_wput(uint32_t addr, uint16_t val);
    virtual void     slot_bput(uint32_t addr, uint8_t val);

    virtual uint32_t board_lget(uint32_t addr);
    virtual uint16_t board_wget(uint32_t addr);
    virtual uint8_t  board_bget(uint32_t addr);
    virtual void     board_lput(uint32_t addr, uint32_t val);
    virtual void     board_wput(uint32_t addr, uint16_t val);
    virtual void     board_bput(uint32_t addr, uint8_t val);

    virtual void     reset(void);
    virtual void     pause(bool pause);

    virtual NDSDL&      display(void) { return sdl; }
    virtual void        tick(int nHostCycles);
    virtual bool        gint(void);
    virtual uint32_t*   vram_bgra(void);
    virtual bool        video_enabled(void);
    virtual void        debug_break(void);
    virtual const char* reports(uint64_t realTime, uint64_t hostTime);
};

#endif /* __cplusplus */

#endif /* __ND_RUST_H__ */
