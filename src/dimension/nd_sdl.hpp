/*
  Previous - nd_sdl.hpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#pragma once

#ifndef __ND_SDL_H__
#define __ND_SDL_H__

#include <SDL3/SDL.h>

#include "config.h"

#ifdef __cplusplus

class NDSDL {
    int           slot;
    uint32_t*     vram;
    int           width;    /* displayed frame and VRAM pitch in pixels */
    int           height;
    int           pitch;
    SDL_Window*   ndWindow;
    SDL_Renderer* ndRenderer;
    SDL_Texture*  ndTexture;
    /* PREVIOUS_ND_SNAPSHOT=DIR: the screen as DIR/nd-NNN.ppm every
     * snapInterval m68k cycles (PREVIOUS_ND_SNAPSHOT_INTERVAL seconds, 10
     * by default) and when automation's "snap" asks */
    const char*   snapDir;
    uint64_t      snapInterval;
    uint64_t      snapNext;
    int           snapCount;
    void          snapshot(void);

#ifdef ENABLE_RENDERING_THREAD
    volatile bool doRepaint;
    SDL_AtomicInt blitNDFB;
    SDL_Thread*   repaintThread;
    static int    repainter(void *_this);
    int           repainter(void);
#endif
public:
    NDSDL(int slot, uint32_t* vram);
    void    geometry(int width, int height, int pitch);
    void    repaint(void);
    void    init(void);
    void    uninit(void);
    void    destroy(void);
    void    resize(float scale);
    void    titlebar(bool show);
    /* A snapshot if one is due, hostCycles m68k cycles after the board's
     * start (m68k thread) */
    void    snapshots(uint64_t hostCycles);
};

extern "C" {
#endif /* __cplusplus */
#ifndef ENABLE_RENDERING_THREAD
    void nd_sdl_repaint(void);
#endif
    void nd_sdl_titlebar(bool show);
    void nd_sdl_resize(float scale);
    void nd_sdl_show(void);
    void nd_sdl_hide(void);
#ifdef __cplusplus
}
#endif
    
#endif /* __ND_SDL_H__ */
