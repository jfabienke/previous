/*
  Previous - nd_board.hpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  NDBoard: what the host side needs from a NeXTdimension board, whichever
  emulation runs it (the builtin NextDimension, or NDRustBoard).
*/

#pragma once

#ifndef __ND_BOARD_H__
#define __ND_BOARD_H__

#ifdef __cplusplus

#include <stdint.h>
#include "NextBus.hpp"
#include "nd_sdl.hpp"

class NDBoard : public NextBusBoard {
public:
    explicit NDBoard(int slot) : NextBusBoard(slot) {}

    /* The board's own display window */
    virtual NDSDL&      display(void) = 0;
    /* Run the board for nHostCycles cycles of the m68k (m68k thread) */
    virtual void        tick(int nHostCycles) = 0;
    /* Level of the board's NeXTbus interrupt, masked by its NBIC */
    virtual bool        gint(void) = 0;
    /* Display (ND_DISPLAY) or video (ND_VIDEO) blanking from the host's
     * timers: the builtin board takes its video timing from them */
    virtual void        host_vbl(int which, bool blank) {}
    /* VRAM: pixels with bytes B, G, R, A, 1152 per line */
    virtual uint32_t*   vram_bgra(void) = 0;
    virtual bool        video_enabled(void) = 0;
    virtual void        debug_break(void) = 0;
    virtual const char* reports(uint64_t realTime, uint64_t hostTime) = 0;
};

#define IF_ND_BOARD(slot, nd) if (NDBoard* nd = dynamic_cast<NDBoard*>(nextbus[(slot)]))

#endif /* __cplusplus */

#endif /* __ND_BOARD_H__ */
