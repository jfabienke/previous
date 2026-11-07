/*
  Previous - nextbus_rust.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The NeXTbus timed cycle by cycle: the 68040's bus cycles, the CPU board's
  NBIC, the NeXTbus and each board's NBIC on its local bus, by the Rust
  model in the NeXTdimension archive (emulator/next-bus, through nd-ffi's
  nd_bus_*; design: docs/emulation/i860-emulator-SBB-nextbus.md). It also
  holds the CPU board NBIC's registers.

  Every access to NeXTbus space is reported with the start of the
  instruction making it; at the end of the instruction run_other_MPUs
  charges what the model says it took. Data still moves through the boards
  here. An empty slot's reads time out (about 20 us) and then bus-error;
  writes to it are posted and lost, as the NBIC does with store-and-forward.

  Mode (PREVIOUS_NEXTBUS_TIMING, else bNextBusTiming): 0 off, 1 this model
  (the default for a 68040 Cube built with the Rust board), 2 the interim
  transaction model in nextbus_timing.c (the default otherwise: the Turbo's
  and the 68030's glue are not modelled).

  Settings for what NeXT's specifications leave open:
    PREVIOUS_NEXTBUS_LCLK     board LCLK in MHz (16.5)
    PREVIOUS_NEXTBUS_GRANT    ps from BR* to the board's bus free (121212)
    PREVIOUS_NEXTBUS_WAITS    the board slave's wait states (0)
    PREVIOUS_NEXTBUS_BURSTS   0: boards refuse bursts (1)
    PREVIOUS_NEXTBUS_SYNC     flip-flops between MCLK and a local clock (2)
    PREVIOUS_NEXTBUS_TIMEOUT  acknowledge cycle of an unanswered transaction (256)
    PREVIOUS_NEXTBUS_DATA     0: the Rust ND board moves its own data, timed
                              by a stand-in (phase 2). Otherwise its accesses
                              move their data through the model: the i860
                              gives up its bus for them, host writes take
                              effect at their board time, and the model runs
                              the board on its time base (1)

  Telemetry:
    PREVIOUS_NEXTBUS_STATS    seconds of guest time between log lines
    PREVIOUS_NEXTBUS_TRACE    file: the bus cycles, Chrome trace format
                              (open it in Perfetto); about 500 bytes each
    PREVIOUS_NEXTBUS_TRACE_SKIP   bus cycles to pass over first (0)
    PREVIOUS_NEXTBUS_TRACE_LIMIT  the most to write (200000; 0 no limit)
    PREVIOUS_NEXTBUS_PROFILE  file: bus time by 68040 pc, written at exit
    PREVIOUS_NEXTBUS_REPORT   file: counters and histograms, written at exit
*/
const char NextBusRust_fileid[] = "Previous nextbus_rust.c";

#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "configuration.h"
#include "cycInt.h"
#include "log.h"
#include "m68000.h"
#include "nextbus_rust.h"

int NextBus_TimingMode = NEXTBUS_TIMING_OFF;
bool NextBusRust_Touched = false;

bool NextBusRust_InSpace(uint32_t addr) {
	return (addr >= 0x20000000 && addr < 0x80000000) || addr >= 0xF1000000;
}

static int setting(const char* name, int fallback) {
	const char* s = getenv(name);
	return s && *s ? atoi(s) : fallback;
}

/* The mode for this machine. */
static int timing_mode(void) {
	int dflt = NEXTBUS_TIMING_OFF;
	if (ConfigureParams.System.bNextBusTiming) {
		dflt = NEXTBUS_TIMING_INTERIM;
#ifdef ENABLE_ND_RUST
		if (ConfigureParams.System.nMachineType == NEXT_CUBE040 &&
		    !ConfigureParams.System.bTurbo)
			dflt = NEXTBUS_TIMING_CYCLE;
#endif
	}
	int mode = setting("PREVIOUS_NEXTBUS_TIMING", dflt);
	if (ConfigureParams.System.nMachineType == NEXT_STATION ||
	    mode < NEXTBUS_TIMING_OFF || mode > NEXTBUS_TIMING_INTERIM)
		mode = NEXTBUS_TIMING_OFF;
#ifndef ENABLE_ND_RUST
	if (mode == NEXTBUS_TIMING_CYCLE)
		mode = NEXTBUS_TIMING_INTERIM;
#endif
	return mode;
}

#ifdef ENABLE_ND_RUST

#include "nd_ffi.h"

static nd_bus* bus;
static uint64_t stats_interval, stats_next;
static nd_bus_stats stats_last;
/* Slots whose board moves its data through the model; of those, the ones
 * the model runs (lockstep). */
static bool data_slot[16], run_slot[16];

void NextBusRust_Init(void) {
	NextBus_TimingMode = timing_mode();
	NextBusRust_Touched = false;
	if (bus) {
		nd_bus_reset(bus);
		return;
	}
	nd_bus_config c;
	nd_bus_config_default(&c);
	c.cpu_hz = (uint32_t)(ConfigureParams.System.nCpuFreq > 0 ? ConfigureParams.System.nCpuFreq : 25) * 1000000;
	c.sync = (uint32_t)setting("PREVIOUS_NEXTBUS_SYNC", (int)c.sync);
	c.timeout_cycle = (uint32_t)setting("PREVIOUS_NEXTBUS_TIMEOUT", (int)c.timeout_cycle);
	bus = nd_bus_create(&c);
	if (!bus) {
		Log_Printf(LOG_ERROR, "[NextBus] no timing model: %s", nd_last_error());
		if (NextBus_TimingMode == NEXTBUS_TIMING_CYCLE)
			NextBus_TimingMode = NEXTBUS_TIMING_INTERIM;
		return;
	}

	const char* trace = getenv("PREVIOUS_NEXTBUS_TRACE");
	const char* skip = getenv("PREVIOUS_NEXTBUS_TRACE_SKIP");
	const char* limit = getenv("PREVIOUS_NEXTBUS_TRACE_LIMIT");
	uint64_t trace_skip = skip && *skip ? strtoull(skip, NULL, 10) : 0;
	uint64_t trace_limit = limit && *limit ? strtoull(limit, NULL, 10) : 200000;
	if (trace && *trace && nd_bus_trace(bus, trace, trace_skip, trace_limit) != ND_OK)
		Log_Printf(LOG_ERROR, "[NextBus] trace: %s", nd_last_error());
	const char* profile = getenv("PREVIOUS_NEXTBUS_PROFILE");
	if (profile && *profile)
		nd_bus_profile(bus, 1);
	const char* secs = getenv("PREVIOUS_NEXTBUS_STATS");
	stats_interval = secs && *secs ? (uint64_t)(atof(secs) * c.cpu_hz) : 0;
	stats_next = nCyclesMainCounter + stats_interval;
	memset(&stats_last, 0, sizeof stats_last);

	if (NextBus_TimingMode == NEXTBUS_TIMING_CYCLE)
		Log_Printf(LOG_WARN, "[NextBus] timed cycle by cycle (next-bus): BCLK %u MHz, "
		           "%u synchronizer stages, timeout at cycle %u",
		           c.cpu_hz / 1000000, c.sync, c.timeout_cycle);
}

void NextBusRust_Attach(int slot) {
	if (!bus)
		return;
	nd_bus_slot_config s;
	nd_bus_slot_config_default(&s);
	const char* lclk = getenv("PREVIOUS_NEXTBUS_LCLK");
	if (lclk && *lclk && atof(lclk) > 1.0)
		s.lclk_hz = (uint32_t)(atof(lclk) * 1e6);
	s.grant_ps = (uint32_t)setting("PREVIOUS_NEXTBUS_GRANT", (int)s.grant_ps);
	s.waits = (uint32_t)setting("PREVIOUS_NEXTBUS_WAITS", 0);
	if (!setting("PREVIOUS_NEXTBUS_BURSTS", 1))
		s.flags |= ND_SLOT_NO_BURSTS;
	if (nd_bus_attach_idle(bus, slot, &s) != ND_OK)
		Log_Printf(LOG_ERROR, "[NextBus] slot %d: %s", slot, nd_last_error());
	else if (NextBus_TimingMode == NEXTBUS_TIMING_CYCLE)
		Log_Printf(LOG_WARN, "[NextBus] slot %d: LCLK %.2f MHz, %u ps to the bus, "
		           "%u wait states, bursts %s", slot, s.lclk_hz / 1e6, s.grant_ps,
		           s.waits, s.flags & ND_SLOT_NO_BURSTS ? "refused" : "taken");
}

void NextBusRust_Reset(void) {
	NextBus_TimingMode = timing_mode();
	NextBusRust_Touched = false;
	if (bus)
		nd_bus_reset(bus);
	if (!bus && NextBus_TimingMode == NEXTBUS_TIMING_CYCLE)
		NextBus_TimingMode = NEXTBUS_TIMING_INTERIM;
}

void NextBusRust_Exit(void) {
	if (!bus)
		return;
	for (int slot = 0; slot < 16; slot++)
		data_slot[slot] = run_slot[slot] = false;
	const char* profile = getenv("PREVIOUS_NEXTBUS_PROFILE");
	if (profile && *profile && nd_bus_profile_write(bus, profile) != ND_OK)
		Log_Printf(LOG_ERROR, "[NextBus] profile: %s", nd_last_error());
	const char* report = getenv("PREVIOUS_NEXTBUS_REPORT");
	if (report && *report && nd_bus_stats_write(bus, report) != ND_OK)
		Log_Printf(LOG_ERROR, "[NextBus] report: %s", nd_last_error());
	nd_bus_destroy(bus); /* closes the trace */
	bus = NULL;
}

int NextBusRust_Access(uint32_t addr, int size, bool write) {
	if (!bus)
		return 0;
	NextBusRust_Touched = true;
	return nd_bus_access(bus, nCyclesMainCounter, regs.instruction_pc, addr, size,
	                     write ? ND_BUS_WRITE : 0) == ND_BUS_ERROR;
}

int NextBusRust_Line(uint32_t addr, bool write, uint32_t* data) {
	if (!bus)
		return 0;
	NextBusRust_Touched = true;
	int slot = addr >> 28 == 0xF ? (addr >> 24) & 0xF : addr >> 28;
	if (data_slot[slot] && data)
		return nd_bus_line_data(bus, nCyclesMainCounter, regs.instruction_pc, addr,
		                        write ? ND_BUS_WRITE : 0, data) == ND_BUS_ERROR;
	return nd_bus_access(bus, nCyclesMainCounter, regs.instruction_pc, addr, 16,
	                     ND_BUS_LINE | (write ? ND_BUS_WRITE : 0)) == ND_BUS_ERROR;
}

bool NextBusRust_AttachBoard(int slot, void* board, bool threaded) {
	if (!bus || NextBus_TimingMode != NEXTBUS_TIMING_CYCLE || slot < 1 || slot > 15 ||
	    !setting("PREVIOUS_NEXTBUS_DATA", 1))
		return false;
	nd_bus_local_config lc;
	nd_bus_local_config_default(&lc);
	lc.memory_waits = (uint32_t)setting("PREVIOUS_NEXTBUS_WAITS", 0);
	if (!setting("PREVIOUS_NEXTBUS_BURSTS", 1))
		lc.flags |= ND_SLOT_NO_BURSTS;
	if (nd_bus_attach(bus, slot, (nd_board*)board, nCyclesMainCounter, &lc) != ND_OK) {
		Log_Printf(LOG_ERROR, "[NextBus] slot %d: %s", slot, nd_last_error());
		return false;
	}
	data_slot[slot] = true;
	run_slot[slot] = !threaded;
	Log_Printf(LOG_WARN, "[NextBus] slot %d: the Rust board's data goes through the model "
	           "(%s), %u wait states", slot, threaded ? "threaded: timed by a stand-in" :
	           "lockstep: the i860 gives up its bus", lc.memory_waits);
	return true;
}

void NextBusRust_DetachBoard(int slot) {
	if (slot < 1 || slot > 15 || !data_slot[slot])
		return;
	data_slot[slot] = run_slot[slot] = false;
	if (bus)
		nd_bus_detach(bus, slot);
}

bool NextBusRust_HasData(int slot) {
	return slot >= 0 && slot < 16 && data_slot[slot];
}

bool NextBusRust_AdvancesBoard(int slot) {
	return slot >= 0 && slot < 16 && run_slot[slot];
}

void NextBusRust_Advance(void) {
	if (bus)
		nd_bus_advance(bus, nCyclesMainCounter);
}

int NextBusRust_AccessData(uint32_t addr, int size, bool write, uint32_t* val) {
	if (!bus)
		return 2;
	NextBusRust_Touched = true;
	switch (nd_bus_access_data(bus, nCyclesMainCounter, regs.instruction_pc, addr, size,
	                           write ? ND_BUS_WRITE : 0, val)) {
	case ND_OK:        return 0;
	case ND_BUS_ERROR: return 1;
	default:           return 2;
	}
}

void NextBusRust_Registers(uint32_t offset, int size, bool write) {
	if (!bus)
		return;
	NextBusRust_Touched = true;
	nd_bus_access(bus, nCyclesMainCounter, regs.instruction_pc, offset, size,
	              ND_BUS_REGISTERS | (write ? ND_BUS_WRITE : 0));
}

/* One log line of what happened since the last. */
static void log_stats(void) {
	nd_bus_stats s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	nd_bus_get_stats(bus, -1, &s);
	double secs = (double)stats_interval / (ConfigureParams.System.nCpuFreq * 1e6);
	uint64_t reads = s.reads - stats_last.reads, writes = s.writes - stats_last.writes;
	uint64_t lines = s.lines - stats_last.lines, bytes = s.bytes - stats_last.bytes;
	uint64_t rb = s.read_bclks - stats_last.read_bclks;
	uint64_t stall = s.stall_bclks - stats_last.stall_bclks;
	Log_Printf(LOG_WARN, "[NextBus] %.1f s: %llu reads (%.1f BCLKs each), %llu writes, "
	           "%llu lines, %.2f MB/s; FIFO full %llu, %llu remote arbitrations, "
	           "%llu follow-ons; CPU stalled %.1f%%",
	           secs, (unsigned long long)reads, reads ? (double)rb / reads : 0.0,
	           (unsigned long long)writes, (unsigned long long)lines, bytes / secs / 1e6,
	           (unsigned long long)(s.fifo_full - stats_last.fifo_full),
	           (unsigned long long)(s.arbitrations - stats_last.arbitrations),
	           (unsigned long long)(s.follow_ons - stats_last.follow_ons),
	           100.0 * stall / (double)stats_interval);
	stats_last = s;
}

uint32_t NextBusRust_EndInstruction(uint32_t cycles) {
	NextBusRust_Touched = false;
	if (!bus)
		return cycles;
	uint32_t charged = nd_bus_end_instruction(bus, nCyclesMainCounter, cycles);
	if (stats_interval && nCyclesMainCounter >= stats_next) {
		log_stats();
		stats_next = nCyclesMainCounter + stats_interval;
	}
	return charged;
}

bool NextBusRust_Registers_Available(void) {
	return bus != NULL;
}

int NextBusRust_RegRead(bool slot_space, uint32_t offset, int size, uint32_t* val) {
	return nd_bus_reg_read(bus, slot_space ? ND_SLOT_SPACE : ND_BUS_NCS, offset, size, val) != ND_OK;
}

int NextBusRust_RegWrite(bool slot_space, uint32_t offset, int size, uint32_t val) {
	return nd_bus_reg_write(bus, slot_space ? ND_SLOT_SPACE : ND_BUS_NCS, offset, size, val) != ND_OK;
}

bool NextBusRust_StoreForward(void) {
	uint32_t control = 0;
	if (!bus || nd_bus_reg_read(bus, ND_BUS_NCS, 0, 4, &control) != ND_OK)
		return true;
	return (control & 0x08000000) != 0; /* STFWD */
}

#else /* !ENABLE_ND_RUST */

void NextBusRust_Init(void) {
	NextBus_TimingMode = timing_mode();
}
void NextBusRust_Attach(int slot) { (void)slot; }
void NextBusRust_Reset(void) {
	NextBus_TimingMode = timing_mode();
}
void NextBusRust_Exit(void) {}
int NextBusRust_Access(uint32_t addr, int size, bool write) {
	(void)addr; (void)size; (void)write;
	return 0;
}
int NextBusRust_Line(uint32_t addr, bool write, uint32_t* data) {
	(void)addr; (void)write; (void)data;
	return 0;
}
bool NextBusRust_AttachBoard(int slot, void* board, bool threaded) {
	(void)slot; (void)board; (void)threaded;
	return false;
}
void NextBusRust_DetachBoard(int slot) { (void)slot; }
bool NextBusRust_HasData(int slot) {
	(void)slot;
	return false;
}
bool NextBusRust_AdvancesBoard(int slot) {
	(void)slot;
	return false;
}
void NextBusRust_Advance(void) {}
int NextBusRust_AccessData(uint32_t addr, int size, bool write, uint32_t* val) {
	(void)addr; (void)size; (void)write; (void)val;
	return 2;
}
void NextBusRust_Registers(uint32_t offset, int size, bool write) {
	(void)offset; (void)size; (void)write;
}
uint32_t NextBusRust_EndInstruction(uint32_t cycles) {
	NextBusRust_Touched = false;
	return cycles;
}
bool NextBusRust_Registers_Available(void) {
	return false;
}
int NextBusRust_RegRead(bool slot_space, uint32_t offset, int size, uint32_t* val) {
	(void)slot_space; (void)offset; (void)size; (void)val;
	return 1;
}
int NextBusRust_RegWrite(bool slot_space, uint32_t offset, int size, uint32_t val) {
	(void)slot_space; (void)offset; (void)size; (void)val;
	return 1;
}
bool NextBusRust_StoreForward(void) {
	return true;
}

#endif /* ENABLE_ND_RUST */
