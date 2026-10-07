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
#include "automation.h"

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
    /* PREVIOUS_ND_CLOCK=MHZ: an i860 at another clock than the
     * NeXTdimension's 33 MHz (the board takes any) */
    const char* clock = getenv("PREVIOUS_ND_CLOCK");
    c.i860_hz = clock ? (uint32_t)(atof(clock) * 1000000.0) : 33000000;
    c.host_hz = ConfigureParams.System.nCpuFreq * 1000000;
    /* PREVIOUS_ND_SYNC=threaded: the board on a thread of its own, beside
     * the m68k's (nd_ffi.h); otherwise in lockstep on the m68k's */
    const char* sync = getenv("PREVIOUS_ND_SYNC");
    bool threaded = sync && !strcmp(sync, "threaded");
    c.sync    = threaded ? ND_SYNC_THREADED : ND_SYNC_LOCKSTEP;
    c.lag     = ND_LAG_HOST_WAITS;
    /* PREVIOUS_ND_CPUS=2 or 4: a lab board of that many i860XPs
     * (docs/emulation/i860-emulator-SBB-mp.md), not a NeXTdimension */
    const char* cpus = getenv("PREVIOUS_ND_CPUS");
    if (cpus && atoi(cpus) > 1) {
        c.cpus   = (uint32_t)atoi(cpus);
        c.flags |= ND_FLAG_XP;
    }
    /* PREVIOUS_ND_GDB_PORT=BASE: a GDB remote-protocol server for the board
     * on 127.0.0.1:BASE+slot (LLDB: gdb-remote PORT); the i860 debugger
     * shortcut stops the board for it */
    const char* gdb = getenv("PREVIOUS_ND_GDB_PORT");
    if (gdb && atoi(gdb) > 0) {
        c.gdb_port = (uint16_t)(atoi(gdb) + *slot);
    }
    /* PREVIOUS_ND_EVENTS=FILE: the board's own events as JSON lines (traps,
     * GINT, device registers, the debugger; nd_config.trace_path), %d in
     * FILE replaced by the slot */
    const char* events = getenv("PREVIOUS_ND_EVENTS");
    char events_path[1024];
    if (events) {
        const char* d = strstr(events, "%d");
        if (d) {
            snprintf(events_path, sizeof events_path, "%.*s%d%s", (int)(d - events), events, *slot,
                     d + 2);
        } else {
            snprintf(events_path, sizeof events_path, "%s", events);
        }
        c.trace_path = events_path;
    }
    /* A larger screen than the NeXTdimension's, for NeXT's software patched
     * to draw it (nCore = 1 only) */
    c.vram_mb        = cfg->nVRAMSize;
    c.display_width  = cfg->nDisplayWidth;
    c.display_height = cfg->nDisplayHeight;
    c.log     = nd_rust_log;
    c.user    = slot;

    nd_board* b = nd_create(&c);
    free(rom);
    if (!b) {
        Log_Printf(LOG_ERROR, "[ND] Slot %i: the Rust board failed: %s", *slot, nd_last_error());
    } else {
        Log_Printf(LOG_WARN, "[ND] Slot %i: Rust board (nd_ffi ABI %u), %s, i860 at %.1f MHz", *slot,
                   nd_abi_version(), threaded ? "threaded" : "lockstep", c.i860_hz / 1e6);
        if (c.gdb_port) {
            Log_Printf(LOG_WARN, "[ND] Slot %i: GDB server on 127.0.0.1:%u", *slot, c.gdb_port);
        }
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
    lastHostTime(0),
    snapDir(getenv("PREVIOUS_ND_SNAPSHOT")),
    snapInterval(0),
    snapNext(0),
    snapCount(0),
    statsInterval(0),
    statsNext(0),
    statsInstructions(0),
    statsHostReads(0),
    statsHostWrites(0),
    statsVramRead(0),
    statsVramWritten(0)
{
    uint32_t w, h, pitch;

    report[0] = 0;
    nd_display_geometry(board, &w, &h, &pitch);
    if (w != ND_DISPLAY_WIDTH || h != ND_DISPLAY_HEIGHT || pitch != ND_VRAM_PITCH) {
        Log_Printf(LOG_WARN, "[ND] Slot %i: display %u x %u on %u-pixel lines, %i MB VRAM",
                   slot, w, h, pitch, ConfigureParams.Dimension.board[ND_NUM(slot)].nVRAMSize);
    }
    sdl.geometry(w, h, pitch);
    sdl.init();
    if (snapDir) {
        const char* s = getenv("PREVIOUS_ND_SNAPSHOT_INTERVAL");
        double secs = s ? atof(s) : 10.0;
        snapInterval = (uint64_t)(secs * ConfigureParams.System.nCpuFreq * 1000000.0);
        snapNext = snapInterval;
    }
    const char* stats_secs = getenv("PREVIOUS_ND_STATS");
    if (stats_secs) {
        statsInterval = (uint64_t)(atof(stats_secs) * ConfigureParams.System.nCpuFreq * 1000000.0);
        statsNext = statsInterval;
    }
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

/* Board space reaches the board at 0xF and the low 28 bits: VRAM is
 * 0xFE000000 up (16 MB at most). */
static bool nd_host_vram(int space, uint32_t addr) {
    return space == ND_BOARD_SPACE && (addr & 0x0F000000) == 0x0E000000;
}

/* Host accesses: a board bus error is the m68k's bus error */
uint32_t NDRustBoard::read(int space, uint32_t addr, int size) {
    uint32_t val = 0;

    if (statsInterval) {
        statsHostReads++;
        if (nd_host_vram(space, addr)) statsVramRead += size;
    }

    if (!board || nd_host_read(board, space, addr, size, &val) == ND_BUS_ERROR) {
        M68000_BusError(addr, BUS_ERROR_READ, size, BUS_ERROR_ACCESS_DATA, 0);
    }
    nd_trace_access(slot, space, false, addr, size, val);
    return val;
}

void NDRustBoard::write(int space, uint32_t addr, int size, uint32_t val) {
    if (statsInterval) {
        statsHostWrites++;
        if (nd_host_vram(space, addr)) statsVramWritten += size;
    }
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

/* Write what the ND window shows, VRAM as Previous blits it, to
 * PREVIOUS_ND_SNAPSHOT/nd-NNN.ppm. */
void NDRustBoard::snapshot(void) {
    uint32_t w, h, pitch;
    char path[FILENAME_MAX];
    const uint8_t* vram = (const uint8_t*)nd_vram(board);

    nd_display_geometry(board, &w, &h, &pitch);
    snprintf(path, sizeof(path), "%s/nd-%03d.ppm", snapDir, snapCount++);
    FILE* f = fopen(path, "wb");
    if (!f) {
        Log_Printf(LOG_WARN, "[ND] Slot %i: cannot write %s", slot, path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t* line = (uint8_t*)malloc(w * 3);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = vram + (size_t)y * pitch * 4; /* bytes B, G, R, A */
        for (uint32_t x = 0; x < w; x++) {
            line[3 * x + 0] = src[4 * x + 2];
            line[3 * x + 1] = src[4 * x + 1];
            line[3 * x + 2] = src[4 * x + 0];
        }
        fwrite(line, 3, w, f);
    }
    free(line);
    fclose(f);
}

/* Log who did the work since the last report: the i860's instructions,
 * and the m68k's accesses to the board (those to VRAM separately). */
void NDRustBoard::stats(void) {
    nd_status s = {};
    s.size = sizeof(s);
    nd_get_status(board, &s);
    double secs = statsInterval / (ConfigureParams.System.nCpuFreq * 1000000.0);
    uint64_t n = s.instructions - statsInstructions;
    Log_Printf(LOG_WARN, "[ND] Slot %i stats: i860 %llu instructions (%.2f MIPS); m68k %llu reads, "
               "%llu writes of the board, VRAM %llu bytes read, %llu written", slot,
               (unsigned long long)n, n / secs / 1e6, (unsigned long long)statsHostReads,
               (unsigned long long)statsHostWrites, (unsigned long long)statsVramRead,
               (unsigned long long)statsVramWritten);
    statsInstructions = s.instructions;
    statsHostReads = statsHostWrites = statsVramRead = statsVramWritten = 0;
}

void NDRustBoard::tick(int nHostCycles) {
    hostCycles += nHostCycles;
    nd_advance(board, hostCycles);

    if (statsInterval && hostCycles >= statsNext) {
        statsNext = hostCycles + statsInterval;
        stats();
    }

    if (snapDir && (hostCycles >= snapNext || Automation_SnapshotRequest)) {
        Automation_SnapshotRequest = 0;
        snapNext = hostCycles + snapInterval;
        snapshot();
    }

    /* "profile start" / "profile write FILE" (automation.c) */
    if (Automation_ProfileRequest == AUTOMATION_PROFILE_START) {
        Automation_ProfileRequest = 0;
        if (nd_profile(board, 1) == ND_OK)
            Log_Printf(LOG_WARN, "[ND] Slot %i: clock profile started", slot);
        else
            Log_Printf(LOG_WARN, "[ND] Slot %i: no clock profile on this board", slot);
    } else if (Automation_ProfileRequest == AUTOMATION_PROFILE_WRITE) {
        Automation_ProfileRequest = 0;
        if (nd_profile_write(board, Automation_ProfilePath) == ND_OK)
            Log_Printf(LOG_WARN, "[ND] Slot %i: clock profile written to %s", slot,
                       Automation_ProfilePath);
        else
            Log_Printf(LOG_WARN, "[ND] Slot %i: clock profile not written", slot);
        nd_profile(board, 0);
    }

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
    /* The board stops; with PREVIOUS_ND_GDB_PORT a GDB client attaches to
     * it (and detaching resumes it), otherwise it stays paused until
     * resumed */
    const char* gdb = getenv("PREVIOUS_ND_GDB_PORT");
    if (gdb && atoi(gdb) > 0) {
        Log_Printf(LOG_WARN, "[ND] Slot %i: stopped for the debugger: gdb-remote %d", slot,
                   atoi(gdb) + slot);
    } else {
        Log_Printf(LOG_WARN, "[ND] Slot %i: paused for the debugger (resume to continue)", slot);
    }
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
