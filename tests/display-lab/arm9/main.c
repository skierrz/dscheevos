/*
 * display-lab: DSi hardware tests for the DScheevos notice strip
 * (docs/display-hw-tests.md has the questions and the pass marks).
 *
 * The top screen (engine A) shows a scrolling stand-in "game"; the strip is
 * the display library's own rendering of an unlock, shown with engine A's
 * Main Memory Display mode fed by NDMA0 (startup 09h). The bottom screen is
 * the log. Results go to sd:/dscheevos-lab/log.txt between tests.
 *
 * Two IRQ setups: libnds's handler while logging (SD and the ARM7 LED
 * command use the FIFO), and a raw handler during the strip tests, where
 * the timing is the point.
 */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd.h"

extern const dsd_font_t dsd_font_latin;

#define N 32                          /* strip lines */
#define M 4                           /* margin lines fed past the strip */
#define LINE_TICKS_8M 532             /* one scanline (2130 cycles at 33.5 MHz) at prescaler 1 */

#define REG32(a) (*(vu32*)(a))
#define REG8(a)  (*(vu8*)(a))
#define NDMA_SAD(x)   REG32(0x04004104 + (x) * 0x1C)
#define NDMA_DAD(x)   REG32(0x04004108 + (x) * 0x1C)
#define NDMA_TCNT(x)  REG32(0x0400410C + (x) * 0x1C)
#define NDMA_WCNT(x)  REG32(0x04004110 + (x) * 0x1C)
#define NDMA_BCNT(x)  REG32(0x04004114 + (x) * 0x1C)
#define NDMA_FDATA(x) REG32(0x04004118 + (x) * 0x1C)
#define NDMA_CNT(x)   REG32(0x0400411C + (x) * 0x1C)
#define SCFG_EXT9     REG32(0x04004008)
#define DISPCNT_A     REG32(0x04000000)
#define DISPCNT_MODE  REG8(0x04000002)   /* bits 16-23: display mode in bits 0-1 */
#define MMEM_FIFO     0x04000068

#define NDMA_ON       BIT(31)
#define NDMA_IRQ      BIT(30)
#define NDMA_FOREVER  BIT(29)
#define NDMA_START(m) ((u32)(m) << 24)
#define NDMA_FILL     (3u << 13)
#define NDMA_DFIX     (2u << 10)
#define NDMA_PHYS(n)  ((u32)(n) << 16)  /* 1 << n words */
#define IRQ_NDMA(x)   BIT(28 + (x))

/* ---------------------------------------------------------------- log */

static char logbuf[16384];
static int loglen;

static void say(const char* fmt, ...) {
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	iprintf("%s\n", b);
	const int n = strlen(b);
	if (loglen + n + 2 < (int)sizeof logbuf) {
		memcpy(logbuf + loglen, b, n);
		loglen += n;
		logbuf[loglen++] = '\n';
	}
	char m[260] = "@@";                 /* the headless runner's test-line prefix */
	strncat(m, b, sizeof m - 4);
	strcat(m, "\n");
	nocashMessage(m);
}

static bool sd_ok;
static void flush_log(void) {
	if (!sd_ok || !loglen) return;
	FILE* f = fopen("sd:/dscheevos-lab/log.txt", "a");
	if (!f) f = fopen("fat:/dscheevos-lab/log.txt", "a");
	if (f) {
		fwrite(logbuf, 1, loglen, f);
		fclose(f);
	}
	loglen = 0;
}

/* ---------------------------------------------------------------- the strip */

static u16 banner[256 * (N + M)] __attribute__((aligned(32)));

static const char* lab_text(void* ctx, const dsd_item_t* it, int which) {
	(void)ctx; (void)it;
	return which == DSD_TEXT_TITLE ? "Display lab: test strip" : "If you can read this, the FIFO feed works";
}

static void make_banner(void) {
	static dsd_t d;
	static dscm_notices_t n;
	const dsd_config_t cfg = { &dsd_font_latin, lab_text, 0, 1 };
	dsd_init(&d, &cfg);
	memset(&n, 0, sizeof n);
	n.magic = DSCM_NOTICES_MAGIC;
	n.head = 1;
	n.ring[1].seq = 1;
	n.ring[1].kind = DSCM_NOTICE_UNLOCK;
	n.ring[1].id = 1;
	n.ring[1].value = 10;
	dsd_ingest(&d, &n);
	const dsd_caps_t caps = { N, 0 };
	dsd_tick(&d, 1, &caps);
	dsd_surface_t s = { banner, 256, N, 256 };
	while (!dsd_render(&d, &s, N)) {}
	for (int y = N; y < N + M; y++)                 /* margin: the strip's background */
		for (int x = 0; x < 256; x++) banner[y * 256 + x] = banner[(N - 2) * 256 + x];
	DC_FlushRange(banner, sizeof banner);
}

static inline void mode_set(int m) { DISPCNT_MODE = (u8)((DISPCNT_MODE & ~3) | m); }
static inline int mode_get(void) { return DISPCNT_MODE & 3; }

static void arm_fifo(void) {
	NDMA_CNT(0) = 0;
	NDMA_SAD(0) = (u32)banner;
	NDMA_DAD(0) = MMEM_FIFO;
	NDMA_TCNT(0) = (N + M) * 128;
	NDMA_WCNT(0) = 4;
	NDMA_BCNT(0) = 0;
	NDMA_CNT(0) = NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX;
}

/* ---------------------------------------------------------------- stand-in game */

static int scroll;

static void setup_game(void) {
	videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankA(VRAM_A_MAIN_BG);
	REG_BG0CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(1) | BG_PRIORITY(1);
	u8 tiles[32];
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x += 2) {
			const int p0 = (((x + y) >> 1) & 1) ? 1 : 2, p1 = (((x + 1 + y) >> 1) & 1) ? 1 : 2;
			tiles[y * 4 + x / 2] = p0 | (p1 << 4);
		}
	memcpy((void*)0x06004000, tiles, sizeof tiles);
	for (int b = 0; b < 16; b++) {
		BG_PALETTE[b * 16 + 1] = RGB15((b * 2) & 31, 31 - b * 2, 10 + b);
		BG_PALETTE[b * 16 + 2] = RGB15(4, 4 + b, 4);
	}
	BG_PALETTE[0] = RGB15(0, 0, 8);
	u16* map = (u16*)0x06000000;
	for (int r = 0; r < 32; r++)
		for (int c = 0; c < 32; c++) map[r * 32 + c] = (u16)((r % 16) << 12);
	lcdMainOnTop();
}

/* ---------------------------------------------------------------- raw IRQ phase */

enum { T_NONE, T_H1, T_H2A, T_H2B, T_NEW, T_STRESS_NDMA, T_STRESS_DMA3 };
static volatile int test;
static volatile u32 frame;

/* H1 / H5 / H6: the old arming (mode 3 at VBlank), switch back from the main loop */
static volatile u32 fifo_done, armed_old;

/* H2 */
static volatile u32 h2_ticks;               /* BCNT interval at prescaler 1 */
static volatile u32 h2_irqs, h2_irq_frames, h2_last_irq_frame, h2_hist[263];
static volatile u32 h2_t33[4], h2_t33n;     /* a few precise delays (33.5 MHz ticks) */
static u32 dummy __attribute__((aligned(32)));

/* new timing (H3, H6b, H8) */
static volatile int new_phase;              /* 0 idle, 1 strip on (waiting exit), 2 guard */
static volatile u32 entries, late_entries, exits, guard_fixes, byte_clobber;
static volatile u32 exit_late_hist[16];
static volatile int use_vblank_start;       /* H2b worked: NDMA2 re-fires by itself */

static void timer_start(void) {
	TIMER0_CR = 0; TIMER1_CR = 0;
	TIMER0_DATA = 0; TIMER1_DATA = 0;
	TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
	TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
}
static u32 timer_read(void) { return TIMER0_DATA | (u32)TIMER1_DATA << 16; }

static void ndma2_oneshot(u32 ticks) {      /* H2a: start now, IRQ after one interval */
	NDMA_CNT(2) = 0;
	NDMA_DAD(2) = (u32)&dummy;
	NDMA_FDATA(2) = 0;
	NDMA_WCNT(2) = 2;
	NDMA_TCNT(2) = 2;
	NDMA_BCNT(2) = ticks | (1u << 16);
	NDMA_CNT(2) = NDMA_ON | NDMA_IRQ | NDMA_START(0x10) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

static void ndma2_vblank(u32 ticks) {       /* H2b: V-Blank startup, repeat forever */
	NDMA_CNT(2) = 0;
	NDMA_DAD(2) = (u32)&dummy;
	NDMA_FDATA(2) = 0;
	NDMA_WCNT(2) = 2;
	NDMA_TCNT(2) = 2;
	NDMA_BCNT(2) = ticks | (1u << 16);
	NDMA_CNT(2) = NDMA_ON | NDMA_IRQ | NDMA_FOREVER | NDMA_START(0x06) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

static void ndma3_hblanks(u32 count) {      /* IRQ at the H-Blank ending the count-th visible line */
	NDMA_CNT(3) = 0;
	NDMA_DAD(3) = (u32)&dummy;
	NDMA_FDATA(3) = 0;
	NDMA_WCNT(3) = 1;
	NDMA_TCNT(3) = count;
	NDMA_BCNT(3) = 0;
	NDMA_CNT(3) = NDMA_ON | NDMA_IRQ | NDMA_START(0x07) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

/* Line ~261: the new entry. */
static void strip_entry(void) {
	const u32 vc = REG_VCOUNT;
	entries++;
	if (vc < 259 || vc > 262) { late_entries++; return; }
	const u32 before = DISPCNT_A;
	arm_fifo();
	mode_set(3);
	if ((DISPCNT_A & 0xFF00FFFF) != (before & 0xFF00FFFF)) byte_clobber++;
	new_phase = 1;
	ndma3_hblanks(N);
}

static void strip_exit_or_guard(void) {
	const u32 vc = REG_VCOUNT;
	if (new_phase == 1) {
		if (mode_get() == 3) mode_set(1);
		exits++;
		int late = (int)vc - (N - 1);
		if (late < 0) late = 0;
		if (late > 15) late = 15;
		exit_late_hist[late]++;
		new_phase = 2;
		ndma3_hblanks(M);
	} else if (new_phase == 2) {
		if (mode_get() == 3) { mode_set(1); guard_fixes++; }
		new_phase = 0;
	}
}

static VoidFn saved_handler;
static u32 saved_ie;
#define RAW_BITS (IRQ_VBLANK | IRQ_NDMA(2) | IRQ_NDMA(3))

__attribute__((target("arm"), noinline)) static void raw_isr(void) {
	const u32 f = REG_IE & REG_IF & RAW_BITS;
	if (f & IRQ_NDMA(2)) {
		REG_IF = IRQ_NDMA(2);
		if (test == T_H2A || test == T_H2B) {
			const u32 vc = REG_VCOUNT;
			if (h2_t33n < 4 && test == T_H2A) h2_t33[h2_t33n++] = timer_read();
			h2_irqs++;
			if (h2_last_irq_frame != frame) { h2_irq_frames++; h2_last_irq_frame = frame; }
			h2_hist[vc < 263 ? vc : 262]++;
		} else if (test == T_NEW) {
			strip_entry();
		}
	}
	if (f & IRQ_NDMA(3)) {
		REG_IF = IRQ_NDMA(3);
		if (test == T_NEW) strip_exit_or_guard();
	}
	if (f & IRQ_VBLANK) {
		REG_IF = IRQ_VBLANK;
		frame++;
		scroll++;
		REG_BG0HOFS = scroll;
		REG_BG0VOFS = scroll / 2;
		if (mode_get() == 3) mode_set(1);       /* never carry mode 3 into a frame we didn't arm */
		switch (test) {
		case T_H1: case T_STRESS_NDMA: case T_STRESS_DMA3:
			if (armed_old && !(NDMA_CNT(0) & NDMA_ON)) fifo_done++;
			arm_fifo();
			mode_set(3);
			armed_old++;
			break;
		case T_H2A:
			timer_start();
			ndma2_oneshot(h2_ticks);
			break;
		case T_NEW:
			if (new_phase) new_phase = 0;           /* a frame whose exit never came */
			if (!use_vblank_start) ndma2_oneshot(h2_ticks);
			break;
		}
	}
	INTR_WAIT_FLAGS |= f;
	/* Everything else (the ARM7's FIFO messages: SD, keys) stays libnds's. */
	if (REG_IE & REG_IF & ~RAW_BITS) saved_handler();
}

static void raw_begin(int t) {
	REG_IME = 0;
	saved_handler = IRQ_HANDLER;
	saved_ie = REG_IE;
	test = t;
	IRQ_HANDLER = raw_isr;
	REG_IF = RAW_BITS;
	REG_IE = (saved_ie & ~IRQ_VCOUNT & ~IRQ_HBLANK) | RAW_BITS;
	REG_DISPSTAT = DISP_VBLANK_IRQ;
	REG_IME = 1;
}

static void raw_end(void) {
	REG_IME = 0;
	for (int i = 0; i < 4; i++) NDMA_CNT(i) = 0;
	mode_set(1);
	test = T_NONE;
	IRQ_HANDLER = saved_handler;
	REG_IF = RAW_BITS;
	REG_IE = saved_ie;
	REG_IME = 1;
}

static void wait_frames(u32 n) {
	const u32 end = frame + n;
	while ((s32)(frame - end) < 0) {}
}

/* ---------------------------------------------------------------- tests */

static void title(const char* s) {
	consoleClear();
	say("== %s", s);
}

/* H4: VRAMCNT readback. Runs before anything is set up. */
static void test_h4(void) {
	title("H4 VRAMCNT readback");
	static const u32 regs[9] = { 0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
	                             0x04000245, 0x04000246, 0x04000248, 0x04000249 };
	int readable = 0;
	for (int i = 0; i < 9; i++) {
		const u8 orig = REG8(regs[i]);
		REG8(regs[i]) = 0x80;                  /* enabled, LCDC */
		const u8 a = REG8(regs[i]);
		REG8(regs[i]) = 0x00;
		const u8 b = REG8(regs[i]);
		REG8(regs[i]) = orig;
		if (a == 0x80 && b == 0x00) readable++;
		say("H4 bank %c orig %02x read %02x/%02x", "ABCDEFGHI"[i], orig, a, b);
	}
	say("H4 vramcnt readable=%d/9", readable);
}

/* R5: SCFG_EXT9 bit 8 and IE bits 28-31. */
static void test_r5(void) {
	title("R5 SCFG_EXT9 bit 8");
	const u32 ext = SCFG_EXT9;
	say("R5 SCFG_EXT9=%08lx bit8=%d scfg_access=%d", ext, (int)(ext >> 8 & 1), (int)(ext >> 31));
	const int ime = REG_IME;
	REG_IME = 0;
	const u32 ie = REG_IE;
	REG_IE = ie | 0xF0000000;
	say("R5 IE 28-31 with bit8 as is: %lx", (REG_IE >> 28) & 0xF);
	if (ext >> 31) {
		SCFG_EXT9 = ext & ~BIT(8);
		REG_IE = ie | 0xF0000000;
		say("R5 bit8 clear -> IE 28-31 %lx", (REG_IE >> 28) & 0xF);
		SCFG_EXT9 = ext | BIT(8);
		REG_IE = ie | 0xF0000000;
		say("R5 bit8 set   -> IE 28-31 %lx", (REG_IE >> 28) & 0xF);
		SCFG_EXT9 = ext;
	} else {
		say("R5 SCFG locked: can't toggle bit 8 here");
	}
	REG_IE = ie;
	REG_IME = ime;
}

/* H1: does 09h feed the FIFO (the old arming, main-loop switch back). */
static void strip_main_loop_frames(u32 frames, int stress) {
	static u32 copybuf[2][1024] __attribute__((aligned(32)));
	const u32 end = frame + frames;
	while ((s32)(frame - end) < 0) {
		if (mode_get() == 3 && REG_VCOUNT >= N - 1 && REG_VCOUNT < 192) mode_set(1);
		if (stress == 1 && !(NDMA_CNT(1) & NDMA_ON)) {      /* card-like: 128-word blocks */
			NDMA_SAD(1) = (u32)copybuf[0];
			NDMA_DAD(1) = (u32)copybuf[1];
			NDMA_WCNT(1) = 1024;
			NDMA_BCNT(1) = 0;
			NDMA_CNT(1) = NDMA_ON | NDMA_START(0x10) | NDMA_PHYS(7);
		}
		if (stress == 2 && !(DMA_CR(3) & DMA_BUSY)) {
			DMA_SRC(3) = (u32)copybuf[0];
			DMA_DEST(3) = (u32)copybuf[1];
			DMA_CR(3) = DMA_ENABLE | DMA_32_BIT | 1024;
		}
	}
	NDMA_CNT(1) = 0;
}

static void test_h1(void) {
	title("H1 NDMA 09h feeds the FIFO (10 s)");
	say("H1 look: banner on lines 0-%d, game below", N - 1);
	fifo_done = armed_old = 0;
	raw_begin(T_H1);
	strip_main_loop_frames(600, 0);
	raw_end();
	say("H1 fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);
}

/* H2a / H2b: NDMA as a private timer. */
static void h2_reset(void) {
	h2_irqs = h2_irq_frames = 0;
	h2_last_irq_frame = ~0u;
	h2_t33n = 0;
	memset((void*)h2_hist, 0, sizeof h2_hist);
}

static void h2_report(const char* name, u32 frames, u32 target_line) {
	u32 lo = 999, hi = 0, within = 0;
	for (int l = 0; l < 263; l++) {
		if (!h2_hist[l]) continue;
		if ((u32)l < lo) lo = l;
		if ((u32)l > hi) hi = l;
		const int d = l - (int)target_line;
		if (d >= -1 && d <= 1) within += h2_hist[l];
	}
	say("%s irqs=%lu frames_with_irq=%lu/%lu line %lu..%lu target %lu within1=%lu",
	    name, h2_irqs, h2_irq_frames, frames, lo == 999 ? 0 : lo, hi, target_line, within);
}

static void test_h2(void) {
	title("H2 NDMA as a private timer");
	if (!((SCFG_EXT9 >> 8) & 1)) say("H2 warning: SCFG_EXT9 bit 8 clear, NDMA IRQs may not fire");
	static const u32 lines[3] = { 20, 40, 69 };
	for (int i = 0; i < 3; i++) {
		h2_reset();
		h2_ticks = lines[i] * LINE_TICKS_8M;
		const u32 f0 = frame;
		raw_begin(T_H2A);
		wait_frames(1000);
		raw_end();
		char name[32];
		snprintf(name, sizeof name, "H2a delay %lu lines", lines[i]);
		/* VBlank IRQ enters at line 192 (+ its own latency); target = 192 + lines */
		h2_report(name, frame - f0, 192 + lines[i]);
		for (u32 k = 0; k < h2_t33n; k++)
			say("H2a   precise %lu: %lu ticks@33M, expected %lu", k, h2_t33[k], h2_ticks * 4);
	}
	h2_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	const u32 f0 = frame;
	raw_begin(T_H2B);
	ndma2_vblank(h2_ticks);
	wait_frames(1000);
	raw_end();
	h2_report("H2b vblank-start forever", frame - f0, 261);
}

/* H3 + H8 + H6b: the new timing. */
static void new_reset(void) {
	entries = late_entries = exits = guard_fixes = byte_clobber = 0;
	memset((void*)exit_late_hist, 0, sizeof exit_late_hist);
	new_phase = 0;
}

static void new_report(const char* name) {
	say("%s entries=%lu late_entry=%lu exits=%lu guard_fixes=%lu byte_clobber=%lu",
	    name, entries, late_entries, exits, guard_fixes, byte_clobber);
	char b[160];
	int o = snprintf(b, sizeof b, "%s exit lateness (lines 0..15+):", name);
	for (int i = 0; i < 16 && o < (int)sizeof b - 8; i++) o += snprintf(b + o, sizeof b - o, " %lu", exit_late_hist[i]);
	say("%s", b);
}

static void test_h3(void) {
	title("H3/H8 byte switch, entry at 261 (10 s)");
	say("H3 look: clean edge at line %d, banner from line 0", N);
	new_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	raw_begin(T_NEW);
	if (use_vblank_start) ndma2_vblank(h2_ticks);
	wait_frames(600);
	raw_end();
	new_report("H3");
}

static void test_h6b(void) {
	title("H6b exit latency, game masks IRQs (167 s)");
	new_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	raw_begin(T_NEW);
	if (use_vblank_start) ndma2_vblank(h2_ticks);
	u32 seed = 12345;
	const u32 end = frame + 10000;
	while ((s32)(frame - end) < 0) {
		/* the "game": every so often, IRQs off for 0-2 lines */
		seed = seed * 1103515245u + 12345u;
		const u32 r = seed >> 16;
		if ((r & 7) == 0) {
			const u32 lines = (r >> 3) % 3;
			REG_IME = 0;
			u32 vc = REG_VCOUNT, seen = 0;
			while (seen < lines) { const u32 v = REG_VCOUNT; if (v != vc) { vc = v; seen++; } }
			REG_IME = 1;
		}
		for (volatile int i = (int)((r >> 6) & 1023); i > 0; i--) {}
	}
	raw_end();
	new_report("H6b");
}

/* H5: bus cost, main-RAM reads during the strip lines, strip off vs on. */
static u32 bus_loop_frames(u32 frames, bool strip) {
	static u32 buf[4096] __attribute__((aligned(32)));
	vu32* unc = (vu32*)memUncached(buf);          /* uncached mirror of main RAM */
	u64 total = 0;
	fifo_done = armed_old = 0;
	raw_begin(strip ? T_H1 : T_NONE);
	for (u32 f = 0; f < frames; f++) {
		while (REG_VCOUNT != 0) {}
		u32 count = 0, i = 0;
		while (REG_VCOUNT < N + M) {
			if (strip && mode_get() == 3 && REG_VCOUNT >= N - 1) mode_set(1);
			(void)unc[i];
			i = (i + 9) & 4095;
			count++;
		}
		total += count;
		while (REG_VCOUNT < 100) {}
	}
	raw_end();
	return (u32)(total / frames);
}

static void test_h5(void) {
	title("H5 bus cost of the strip (10 s)");
	const u32 off = bus_loop_frames(300, false);
	const u32 on = bus_loop_frames(300, true);
	/* loss in the strip lines, scaled to the whole frame (263 lines) */
	const u32 strip_pct_x100 = off ? (u32)((u64)(off - (on < off ? on : off)) * 10000 / off) : 0;
	const u32 frame_pct_x100 = strip_pct_x100 * (N + M) / 263;
	say("H5 reads/strip off=%lu on=%lu", off, on);
	say("H5 loss: strip lines %lu.%02lu pct, whole frame %lu.%02lu pct (pass <= 3.50)",
	    strip_pct_x100 / 100, strip_pct_x100 % 100, frame_pct_x100 / 100, frame_pct_x100 % 100);
}

static void test_h6(void) {
	title("H6a strip + NDMA1 128-word blocks (10 s)");
	say("H6a look: corruption expected (record it)");
	fifo_done = armed_old = 0;
	raw_begin(T_STRESS_NDMA);
	strip_main_loop_frames(600, 1);
	raw_end();
	say("H6a fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);

	title("H6b' strip + legacy DMA3 bursts (10 s)");
	say("H6 look: banner must stay clean");
	fifo_done = armed_old = 0;
	raw_begin(T_STRESS_DMA3);
	strip_main_loop_frames(600, 2);
	raw_end();
	say("H6-dma3 fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);
}

/* H9: camera LED via the ARM7. */
static u32 led(u32 v) {
	fifoSendValue32(FIFO_USER_01, v);
	while (!fifoCheckValue32(FIFO_USER_01)) swiWaitForVBlank();
	return fifoGetValue32(FIFO_USER_01);
}

static void test_h9(void) {
	title("H9 camera LED: on 3 s, blink 5 s, off");
	const u32 t1 = led(1);
	for (int i = 0; i < 180; i++) swiWaitForVBlank();
	const u32 t2 = led(2);
	for (int i = 0; i < 300; i++) swiWaitForVBlank();
	const u32 t0 = led(0);
	/* 33.514 MHz ticks -> microseconds */
	say("H9 i2c write us: on=%lu blink=%lu off=%lu (pass < 500)",
	    t1 * 1000 / 33514, t2 * 1000 / 33514, t0 * 1000 / 33514);
}

int main(void) {
	consoleDemoInit();
	if (!isDSiMode()) {
		iprintf("display-lab needs DSi mode.\nStart it from TWiLight Menu++\nin DSi mode.\n");
		for (;;) swiWaitForVBlank();
	}
	sd_ok = fatInitDefault();
	if (sd_ok) mkdir("sd:/dscheevos-lab", 0777);

	say("display-lab N=%d M=%d SCFG_EXT9=%08lx sd=%d", N, M, SCFG_EXT9, sd_ok);
	test_h4();
	test_r5();
	flush_log();

	setup_game();
	make_banner();
	iprintf("\nStart filming both screens.\nPress A to run (about 6 min).\n");
	for (;;) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_A) break;
	}

	test_h1();      flush_log();
	test_h2();      flush_log();
	use_vblank_start = h2_irq_frames >= 990;   /* H2b re-fired by itself every frame */
	say("new timing uses %s", use_vblank_start ? "V-Blank startup (H2b)" : "one-shot from VBlank (H2a)");
	test_h3();      flush_log();
	test_h5();      flush_log();
	test_h6();      flush_log();
	test_h6b();     flush_log();
	test_h9();      flush_log();

	consoleClear();
	say("DONE, log %s", sd_ok ? "written to sd:/dscheevos-lab/log.txt" : "NOT written (no SD)");
	flush_log();
	for (;;) swiWaitForVBlank();
}
