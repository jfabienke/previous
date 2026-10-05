/*
  Previous - nd_rust.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  NDRustBoard: a NeXTdimension emulated by the nextdimension-archive's Rust
  board through nd_ffi.h. It runs in lockstep on the m68k thread: every
  i860_Run call advances it to the m68k's time.
*/

#include "main.h"
#include "configuration.h"
#include "m68000.h"
#include "file.h"
#include "log.h"
#include "statusbar.h"
#include "dimension.hpp"
#include "nd_rust.hpp"

#include <stdio.h>
#include <stdlib.h>

#define ND_ROM_SIZE (128 * 1024)

/* PREVIOUS_ND_TRACE=FILE logs the host's accesses to the board, with the
 * m68k PC. Runs of writes from one PC (loading code) are logged as one line. */
static FILE*    nd_trace = NULL;
static uint32_t nd_trace_run_pc, nd_trace_run_start, nd_trace_run_next, nd_trace_run_count;

static void nd_trace_flush_run(void) {
    if (nd_trace_run_count) {
        fprintf(nd_trace, "W* %08X..%08X %u writes pc=%08X\n", nd_trace_run_start,
                nd_trace_run_next, nd_trace_run_count, nd_trace_run_pc);
        nd_trace_run_count = 0;
    }
}

static void nd_trace_access(int slot, int space, bool write, uint32_t addr, int size, uint32_t val) {
    if (!nd_trace) return;
    uint32_t pc = m68k_getpc();
    if (write && size == 4 && pc == nd_trace_run_pc && addr == nd_trace_run_next) {
        nd_trace_run_next += 4;
        nd_trace_run_count++;
        return;
    }
    nd_trace_flush_run();
    if (write && size == 4) {
        nd_trace_run_pc = pc;
        nd_trace_run_start = addr;
        nd_trace_run_next = addr + 4;
        nd_trace_run_count = 0;
    }
    fprintf(nd_trace, "%c%d %c %08X %0*X pc=%08X\n", write ? 'W' : 'R', size,
            space == ND_SLOT_SPACE ? 'S' : 'B', addr, size * 2, val, pc);
}

static void nd_rust_log(void* user, int level, const char* msg) {
    int slot = *(int*)user;

    switch (level) {
        case ND_LOG_ERROR: Log_Printf(LOG_ERROR, "[ND] Slot %i: %s", slot, msg); break;
        case ND_LOG_WARN:  Log_Printf(LOG_WARN,  "[ND] Slot %i: %s", slot, msg); break;
        case ND_LOG_INFO:  Log_Printf(LOG_INFO,  "[ND] Slot %i: %s", slot, msg); break;
        default:           Log_Printf(LOG_DEBUG, "[ND] Slot %i: %s", slot, msg); break;
    }
}

static nd_board* nd_rust_create(int* slot) {
    NDBOARD* cfg = &ConfigureParams.Dimension.board[ND_NUM(*slot)];
    long     len = 0;
    uint8_t* rom = File_ReadAsIs(cfg->szRomFileName, &len);

    if (!rom) {
        Log_Printf(LOG_ERROR, "[ND] Slot %i: cannot read the ROM %s", *slot, cfg->szRomFileName);
        len = 0;
    } else if (len > ND_ROM_SIZE) {
        len = ND_ROM_SIZE;
    }

    nd_config c = {};
    c.size    = sizeof(c);
    c.slot    = *slot;
    for (int i = 0; i < 4; i++) {
        c.bank_mb[i] = cfg->nMemoryBankSize[i];
    }
    c.rom     = rom;
    c.rom_len = (uint32_t)len;
    c.i860_hz = 33000000;
    c.host_hz = ConfigureParams.System.nCpuFreq * 1000000;
    c.sync    = ND_SYNC_LOCKSTEP;
    c.lag     = ND_LAG_HOST_WAITS;
    c.log     = nd_rust_log;
    c.user    = slot;

    nd_board* b = nd_create(&c);
    free(rom);
    if (!b) {
        Log_Printf(LOG_ERROR, "[ND] Slot %i: the Rust board failed: %s", *slot, nd_last_error());
    } else {
        Log_Printf(LOG_WARN, "[ND] Slot %i: Rust board (nd_ffi ABI %u), lockstep", *slot, nd_abi_version());
    }
    return b;
}

NDRustBoard::NDRustBoard(int slot) :
    NDBoard(slot),
    board(nd_rust_create(&this->slot)),
    sdl(slot, (uint32_t*)nd_vram(board)),
    hostCycles(0),
    ledState(-1),
    ledTicks(0),
    lastInstructions(0),
    lastHostTime(0)
{
    report[0] = 0;
    sdl.init();
    const char* trace = getenv("PREVIOUS_ND_TRACE");
    if (trace && !nd_trace) {
        nd_trace = fopen(trace, "w");
    }
}

NDRustBoard::~NDRustBoard() {
    Statusbar_SetNdLed(0);
    sdl.destroy();
    nd_destroy(board);
}

/* Host accesses: a board bus error is the m68k's bus error */
uint32_t NDRustBoard::read(int space, uint32_t addr, int size) {
    uint32_t val = 0;

    if (!board || nd_host_read(board, space, addr, size, &val) == ND_BUS_ERROR) {
        M68000_BusError(addr, BUS_ERROR_READ, size, BUS_ERROR_ACCESS_DATA, 0);
    }
    nd_trace_access(slot, space, false, addr, size, val);
    return val;
}

void NDRustBoard::write(int space, uint32_t addr, int size, uint32_t val) {
    nd_trace_access(slot, space, true, addr, size, val);
    if (!board || nd_host_write(board, space, addr, size, val) == ND_BUS_ERROR) {
        M68000_BusError(addr, BUS_ERROR_WRITE, size, BUS_ERROR_ACCESS_DATA, val);
    }
}

uint32_t NDRustBoard::slot_lget(uint32_t addr) { return read(ND_SLOT_SPACE, addr, 4); }
uint16_t NDRustBoard::slot_wget(uint32_t addr) { return read(ND_SLOT_SPACE, addr, 2); }
uint8_t  NDRustBoard::slot_bget(uint32_t addr) { return read(ND_SLOT_SPACE, addr, 1); }
void NDRustBoard::slot_lput(uint32_t addr, uint32_t val) { write(ND_SLOT_SPACE, addr, 4, val); }
void NDRustBoard::slot_wput(uint32_t addr, uint16_t val) { write(ND_SLOT_SPACE, addr, 2, val); }
void NDRustBoard::slot_bput(uint32_t addr, uint8_t val)  { write(ND_SLOT_SPACE, addr, 1, val); }

uint32_t NDRustBoard::board_lget(uint32_t addr) { return read(ND_BOARD_SPACE, addr, 4); }
uint16_t NDRustBoard::board_wget(uint32_t addr) { return read(ND_BOARD_SPACE, addr, 2); }
uint8_t  NDRustBoard::board_bget(uint32_t addr) { return read(ND_BOARD_SPACE, addr, 1); }
void NDRustBoard::board_lput(uint32_t addr, uint32_t val) { write(ND_BOARD_SPACE, addr, 4, val); }
void NDRustBoard::board_wput(uint32_t addr, uint16_t val) { write(ND_BOARD_SPACE, addr, 2, val); }
void NDRustBoard::board_bput(uint32_t addr, uint8_t val)  { write(ND_BOARD_SPACE, addr, 1, val); }

/* A NeXTbus reset: the board starts over from its ROM, keeping its time */
void NDRustBoard::reset(void) {
    nd_reset(board);
    i860_Run = nd_run_boards;
    nd_start_interrupts();
}

void NDRustBoard::pause(bool pause) {
    nd_pause(board, pause);
}

void NDRustBoard::tick(int nHostCycles) {
    hostCycles += nHostCycles;
    nd_advance(board, hostCycles);

    /* The status bar LED: off when stopped, 1 in the ROM, 2 running */
    if (++ledTicks < 1024) return;
    ledTicks = 0;
    nd_status s = {};
    s.size = sizeof(s);
    nd_get_status(board, &s);
    int led = s.state != ND_STATE_RUNNING ? 0 : (s.pc >= 0xFFF00000 ? 1 : 2);
    if (led != ledState) {
        if (s.state == ND_STATE_FAULTED) {
            Log_Printf(LOG_ERROR, "[ND] Slot %i: the board faulted: %s", slot, nd_fault_message(board));
        }
        Statusbar_SetNdLed(led);
        ledState = led;
    }
}

bool NDRustBoard::gint(void) {
    return nd_gint(board) != 0;
}

uint32_t* NDRustBoard::vram_bgra(void) {
    return (uint32_t*)nd_vram(board);
}

bool NDRustBoard::video_enabled(void) {
    return nd_video_enabled(board) != 0;
}

void NDRustBoard::debug_break(void) {
    /* nd_ffi version 1 has no debugger: the board pauses until resumed */
    Log_Printf(LOG_WARN, "[ND] Slot %i: paused for the debugger (resume to continue)", slot);
    nd_debug_break(board);
}

const char* NDRustBoard::reports(uint64_t realTime, uint64_t hostTime) {
    nd_status s = {};
    s.size = sizeof(s);
    nd_get_status(board, &s);

    double dt = (hostTime - lastHostTime) / 1000000.0;
    if (dt <= 0) dt = 0.0001;
    snprintf(report, sizeof(report), "i860:{MIPS=%.1f pc=%08X%s}",
             (s.instructions - lastInstructions) / (dt * 1000 * 1000), s.pc,
             s.state == ND_STATE_FAULTED ? " faulted" : s.state == ND_STATE_PAUSED ? " paused" : "");
    lastInstructions = s.instructions;
    lastHostTime     = hostTime;
    return report;
}
