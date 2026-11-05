/*
  Previous - nextbus_timing.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The time the CPU waits for the NeXTbus. An access to a board across the bus
  (the NeXTdimension) is a NeXTbus transaction through two NBICs, the CPU
  board's and the board's. Following NeXT's NextBus Specification (N6010) and
  NextBus Interface Chip Specification (N6011), printed pages:
  - the bus's major cycle is 80 ns (MCLK, 12.5 MHz). A single-word
    transaction is a start and an acknowledge cycle; a burst moves two words
    a cycle between them (NeXTbus 1-7, 4-8 to 4-11);
  - the NBIC bursts four words only (NBIC 2-12), so a MOVE16 is one burst;
  - with store-forward (the control register's STFWD, which NeXT's kernel
    sets at boot) the NBIC acknowledges a write as soon as its FIFO has room,
    and the board's NBIC acknowledges on the bus without waiting for its local
    logic (NBIC 2-14, 2-16): writes are posted, two transactions in each NBIC
    (NBIC 2-3). Without it the CPU waits for the board;
  - a read waits for the whole round trip;
  - on the CPU board the NBIC is a local slave: a single word ends at CPU
    clock 5, a four-word burst at 11 (NBIC 6-13 to 6-19);
  - on the board the NBIC is local master on LCLK (at most 16.66 MHz,
    NBIC 1-2), after synchronizing to it: a single word takes 9 clocks with
    its data at clock 6, a burst 12 with its last word at clock 9
    (NBIC 6-6 to 6-9).
  The board's LCLK, its memory controller's wait states and whether it takes
  bursts are not documented, so they are settings:
    PREVIOUS_NEXTBUS_TIMING  0 or 1, over the configuration's bNextBusTiming
    PREVIOUS_NEXTBUS_LCLK    the board's local clock in MHz (16.5)
    PREVIOUS_NEXTBUS_WAITS   the board's wait states a transaction, in LCLK (0)
    PREVIOUS_NEXTBUS_BURSTS  1 if the board takes four-word bursts (1)
    PREVIOUS_NEXTBUS_STATS   seconds of guest time between reports
  The CPU core's instruction tables already charge each memory access; that
  much is taken off the bus's cost. Contention with the board's own processor
  and video is not modelled.
*/
const char NextBusTiming_fileid[] = "Previous nextbus_timing.c";

#include <math.h>
#include <stdlib.h>

#include "main.h"
#include "configuration.h"
#include "cycInt.h"
#include "log.h"
#include "m68000.h"
#include "nbic.h"
#include "nextbus_timing.h"

#define MAX_POSTED 8

bool NextBusTiming_Enabled = false;

/* Clocks and the transactions' lengths in them */
static struct {
	double cpu_ns;       /* a CPU clock */
	double mclk_ns;      /* the NeXTbus's major cycle */
	double lclk_ns;      /* the board's local clock */
	double table_ns[5];  /* what the CPU core already charges an access of n bytes */
	int host_single;     /* CPU clocks of a single-word local cycle */
	int host_burst;      /* CPU clocks of a four-word local burst */
	int bus_single;      /* MCLKs of a single-word transaction */
	int bus_burst;       /* MCLKs of a four-word burst */
	int board_sync;      /* LCLKs to synchronize to the board's clock */
	int board_single;    /* LCLKs of a single-word local cycle */
	int board_data;      /* LCLKs to a single word's data */
	int board_burst;     /* LCLKs of a four-word local burst */
	int board_burst_data;/* LCLKs to a burst's last word */
	int board_waits;     /* wait states a transaction */
	int fifo;            /* posted transactions in flight */
	bool bursts;         /* the board takes bursts */
} p;

/* Transactions in flight, in ns of guest time */
static double bus_free;          /* the bus is free from */
static double board_free[16];    /* each board's local side is free from */
static double posted[MAX_POSTED];/* posted writes' completions, oldest first */
static int posted_first, posted_n;
static double stall_ns;          /* waited and not yet charged */
static int burst_left;           /* longwords of the current MOVE16 to come */

/* Reports */
static uint64_t stats_interval, stats_next;
static uint64_t n_reads, n_writes, n_bursts, n_full;
static double waited_ns;

static int setting(const char* name, int fallback) {
	const char* v = getenv(name);
	return v && *v ? atoi(v) : fallback;
}

static double ceil_to(double t, double unit) {
	return ceil(t / unit - 1e-9) * unit;
}

static double now_ns(void) {
	return (double)nCyclesMainCounter * p.cpu_ns + stall_ns;
}

static double oldest_posted(void) {
	return posted[posted_first];
}

/* Forget the posted writes finished by t. */
static void retire(double t) {
	while (posted_n > 0 && oldest_posted() <= t) {
		posted_first = (posted_first + 1) % MAX_POSTED;
		posted_n--;
	}
}

/* A write: the time the CPU spends from t. */
static double bus_write(int slot, bool burst, double t) {
	double start = t;
	double local = (burst ? p.host_burst : p.host_single) * p.cpu_ns;
	retire(t);
	if (posted_n >= p.fifo) {
		/* both NBICs' FIFOs are full: wait for the oldest */
		t = oldest_posted();
		retire(t);
		n_full++;
	}
	double bus_start = ceil_to(fmax(t + local, bus_free), p.mclk_ns);
	double bus_done = bus_start + (burst ? p.bus_burst : p.bus_single) * p.mclk_ns;
	double board_start = ceil_to(fmax(bus_done, board_free[slot]), p.lclk_ns)
	                     + p.board_sync * p.lclk_ns;
	double board_done = board_start
	                    + ((burst ? p.board_burst : p.board_single) + p.board_waits) * p.lclk_ns;
	bus_free = bus_done;
	board_free[slot] = board_done;
	if (nbic_store_forward()) {
		posted[(posted_first + posted_n) % MAX_POSTED] = board_done;
		posted_n++;
		return t + local - start;
	}
	/* acknowledged only when the board has the data */
	double ack = ceil_to(board_done, p.mclk_ns) + p.mclk_ns;
	bus_free = ack;
	return ack + p.cpu_ns - start;
}

/* A read: the round trip from t. It follows the posted writes through both
 * NBICs, which keep their order. */
static double bus_read(int slot, bool burst, double t) {
	double bus_start = ceil_to(fmax(t + p.cpu_ns, bus_free), p.mclk_ns);
	double board_start = ceil_to(fmax(bus_start + p.mclk_ns, board_free[slot]), p.lclk_ns)
	                     + p.board_sync * p.lclk_ns;
	double data = board_start
	              + ((burst ? p.board_burst_data : p.board_data) + p.board_waits) * p.lclk_ns;
	/* back on MCLK: a burst's data take two cycles, then the acknowledge */
	double ack = ceil_to(data, p.mclk_ns) + (burst ? 3 : 1) * p.mclk_ns;
	double done = ack + (burst ? p.host_burst - 3 : 2) * p.cpu_ns;
	board_free[slot] = board_start
	                   + ((burst ? p.board_burst : p.board_single) + p.board_waits) * p.lclk_ns;
	bus_free = ack;
	retire(done);
	return done - t;
}

static void report(void) {
	double secs = stats_interval / (ConfigureParams.System.nCpuFreq * 1e6);
	Log_Printf(LOG_WARN, "[NextBus] timing: %llu reads, %llu writes, %llu bursts, "
	           "%llu writes waited for a full FIFO; the CPU waited %.3f ms of %.3f s",
	           (unsigned long long)n_reads, (unsigned long long)n_writes,
	           (unsigned long long)n_bursts, (unsigned long long)n_full,
	           waited_ns / 1e6, secs);
	n_reads = n_writes = n_bursts = n_full = 0;
	waited_ns = 0;
}

void NextBusTiming_Access(int slot, int size, bool write_access) {
	double t = now_ns();
	double spent;
	bool burst = false;
	if (burst_left > 0) {
		burst_left--;
		if (p.bursts && size == 4) {
			if (burst_left < 3) {
				return; /* charged with the burst's first longword */
			}
			burst = true;
			n_bursts++;
		}
	}
	if (write_access) {
		n_writes++;
		spent = bus_write(slot & 15, burst, t);
	} else {
		n_reads++;
		spent = bus_read(slot & 15, burst, t);
	}
	double table = p.table_ns[size] * (burst ? 4 : 1);
	if (spent > table) {
		stall_ns += spent - table;
		waited_ns += spent - table;
	}
	if (stats_interval && nCyclesMainCounter >= stats_next) {
		report();
		stats_next = nCyclesMainCounter + stats_interval;
	}
}

void NextBusTiming_BurstBegin(void) {
	burst_left = 4;
}

void NextBusTiming_BurstEnd(void) {
	burst_left = 0;
}

int NextBusTiming_TakeStall(void) {
	if (stall_ns < p.cpu_ns) {
		return 0;
	}
	int cycles = (int)(stall_ns / p.cpu_ns);
	stall_ns -= cycles * p.cpu_ns;
	return cycles;
}

void NextBusTiming_Reset(void) {
	int on = setting("PREVIOUS_NEXTBUS_TIMING", ConfigureParams.System.bNextBusTiming ? 1 : 0);
	int mhz = ConfigureParams.System.nCpuFreq > 0 ? ConfigureParams.System.nCpuFreq : 25;
	const char* lclk = getenv("PREVIOUS_NEXTBUS_LCLK");
	double lclk_mhz = lclk && *lclk ? atof(lclk) : 16.5;
	const char* stats = getenv("PREVIOUS_NEXTBUS_STATS");

	NextBusTiming_Enabled = on != 0 && ConfigureParams.System.nMachineType != NEXT_STATION;
	p.cpu_ns = 1000.0 / mhz;
	p.mclk_ns = 80.0;
	p.lclk_ns = 1000.0 / (lclk_mhz > 1.0 ? lclk_mhz : 16.5);
	/* the core's tables: 4 cycles a word access, 8 a long, then scaled for
	 * the CPU (gencpu.c adjust_cycles: *1/3 on the 68040, *3/2 on the 68030) */
	double scale = ConfigureParams.System.nCpuLevel >= 4 ? 1.0 / 3.0 : 1.5;
	p.table_ns[0] = 0;
	p.table_ns[1] = p.table_ns[2] = p.table_ns[3] = 4 * scale * p.cpu_ns;
	p.table_ns[4] = 8 * scale * p.cpu_ns;
	p.host_single = 5;
	p.host_burst = 11;
	p.bus_single = 2;
	p.bus_burst = 4;
	p.board_sync = 1;
	p.board_single = 9;
	p.board_data = 6;
	p.board_burst = 12;
	p.board_burst_data = 9;
	p.board_waits = setting("PREVIOUS_NEXTBUS_WAITS", 0);
	p.fifo = 4;
	p.bursts = setting("PREVIOUS_NEXTBUS_BURSTS", 1) != 0;

	bus_free = 0;
	for (int i = 0; i < 16; i++) {
		board_free[i] = 0;
	}
	posted_first = posted_n = 0;
	stall_ns = 0;
	burst_left = 0;
	n_reads = n_writes = n_bursts = n_full = 0;
	waited_ns = 0;
	stats_interval = stats ? (uint64_t)(atof(stats) * mhz * 1e6) : 0;
	stats_next = nCyclesMainCounter + stats_interval;

	if (NextBusTiming_Enabled) {
		Log_Printf(LOG_WARN, "[NextBus] timing on: MCLK 80 ns, board LCLK %.2f MHz, "
		           "%d wait states, bursts %s", 1000.0 / p.lclk_ns, p.board_waits,
		           p.bursts ? "taken" : "split into words");
	}
}
